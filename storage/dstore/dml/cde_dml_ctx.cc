/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "cde_dml_ctx.h"

#include "common/cde_errorcode.h"
#include "heap/dstore_heap_interface.h"
#include "heap/dstore_heap_struct.h"
#include "tuple/dstore_tuple_interface.h"
#include "tuple/dstore_tuple_struct.h"

#include "cde_heap.h"
#include "common/cde_compare_utils.h"
#include "common/cde_trxmgr.h"

using DSTORE::DSTORE_FAIL;
using DSTORE::HeapTuple;
using DSTORE::INVALID_DATUM;
using DSTORE::INVALID_ITEM_POINTER;
using DSTORE::ItemPointer;
using DSTORE::RetStatus;
using DSTORE::ScanDirection;
using DSTORE::ScanKey;
using DSTORE::ScanKeyData;
using DSTORE::StorageRelationData;
using DSTORE::TupleDescData;

namespace CDE {

int dml_ctx::init() {
  if (m_table == nullptr) {
    return CDE_ERROR;
  }
  return CDE_OK;
}

/*
 * We use only one block of memory here for all kinds of dml_ctx(dml_ins_ctx,
 * dml_upd_ctx...) to alloc buffer they need. E.g., dml_upd_ctx use this block
 * of memory to alloc: m_tuple + m_index_tuple + m_tuple_old + m_is_changed, and
 * only need to free m_tuple which is already freed by ~dml_ctx. Notice that all
 * single piece of memory should be aligned with 8 byte.
 */
char *dml_ctx::init_tuple(dml_tuple_t *&p_tuple, char *buf) {
  uint32_t numberOfFields = m_fields_num - m_vfields_num;

  p_tuple = (dml_tuple_t *)buf;
  buf += sizeof(dml_tuple_t);
  p_tuple->ctid = INVALID_ITEM_POINTER;

  p_tuple->values = (Datum *)buf;
  buf += numberOfFields * sizeof(Datum);

  p_tuple->varlenas = (CdeVarlena *)buf;
  buf += numberOfFields * sizeof(CdeVarlena);

  p_tuple->is_nulls = (bool *)buf;
  buf += numberOfFields * sizeof(bool);

  p_tuple->is_blob = (bool *)buf;
  memset_s(p_tuple->is_blob, m_fields_num * sizeof(bool), 0,
           numberOfFields * sizeof(bool));
  buf += numberOfFields * sizeof(bool);
  return buf;
}

char *dml_ctx::init_full_tuple(dml_tuple_t *&p_tuple, char *buf) {
  p_tuple = (dml_tuple_t *)buf;
  buf += sizeof(dml_tuple_t);
  p_tuple->ctid = INVALID_ITEM_POINTER;

  p_tuple->values = (Datum *)buf;
  buf += m_fields_num * sizeof(Datum);

  p_tuple->varlenas = (CdeVarlena *)buf;
  buf += m_fields_num * sizeof(CdeVarlena);

  p_tuple->is_nulls = (bool *)buf;
  buf += m_fields_num * sizeof(bool);

  p_tuple->is_blob = (bool *)buf;
  memset_s(p_tuple->is_blob, m_fields_num * sizeof(bool), 0,
           m_fields_num * sizeof(bool));
  buf += m_fields_num * sizeof(bool);
  return buf;
}

char *dml_ctx::init_index_tuple(dml_index_tuple_t *&p_index_tuple, char *buf) {
  p_index_tuple = (dml_index_tuple_t *)buf;
  buf += sizeof(dml_index_tuple_t);

  p_index_tuple->index_values = (Datum *)buf;
  buf += m_fields_num * sizeof(Datum);

  p_index_tuple->index_is_nulls = (bool *)buf;
  buf += m_fields_num * sizeof(bool);
  buf = (char *)CDE_MEM_ALIGN(buf);

  p_index_tuple->index_varlenas = (CdeVarlena *)buf;
  buf += m_fields_num * sizeof(CdeVarlena);

  p_index_tuple->key_infos = (ScanKeyData *)buf;
  buf += m_fields_num * sizeof(ScanKeyData);

  p_index_tuple->col_oids = (Oid *)buf;
  buf += m_fields_num * sizeof(Oid);

  p_index_tuple->col_collation_oids = (Oid *)buf;
  buf += m_fields_num * sizeof(Oid);

  return buf;
}

session_rel_info *dml_ctx::getRelation(cde_dict_t *checkTable) {
  session_rel_info *checkRelInfo = getPinnedRelation(checkTable->m_id);
  if (nullptr == checkRelInfo) {
    RelationCache *relationCache =
        relationCacheManager.getCache(m_current_thd->thread_id());
    { /** for lock */
      std::lock_guard<RelationCache> lock(*relationCache);
      checkRelInfo = relationCache->getRelation(checkTable->m_id);
    }
    /** There is no relation pinned to context's cache, copy it there */
    if (nullptr == checkRelInfo) {
      auto relInfo = std::make_unique<session_rel_info>(checkTable->m_id);
      bool ret = CdeCopyRelInfoToLocalStorage(checkTable, relInfo.get());
      DBUG_EXECUTE_IF("copy_indexes_failed_injection", { ret = CDE_FAIL; });
      if (CDE_SUCC != ret) return nullptr;
      CDE_ASSERT_DEBUG(nullptr != relInfo.get());
      checkRelInfo = relInfo.get();
      /** Add this relation to used relations, as we are going to use it right
       * now. */
      { /** for lock */
        std::lock_guard<RelationCache> lock(*relationCache);
        relationCache->addUsedRelation(relInfo.release());
      }
    }
    pinRelation(checkRelInfo);
  }
  CDE_ASSERT_DEBUG(nullptr != checkRelInfo);
  return checkRelInfo;
}

int dml_ins_ctx::init() {
  if (dml_ctx::init() != CDE_OK) {
    return CDE_ERROR;
  }
  bool needsFullTuple = m_vfields_num > 0;
  size_t total_size =
      calc_tuple_mem_size() + calc_index_tuple_mem_size() + CDE_MEM_ALIGNMENT;
  if (needsFullTuple)
    total_size += calc_full_tuple_mem_size() + CDE_MEM_ALIGNMENT;
  char *buf = (char *)CdeAlloc(total_size);
  DBUG_EXECUTE_IF("alloc_mem_fail_injection", CdeFree(buf); buf = nullptr;);
  if (buf == nullptr) return HA_ERR_OUT_OF_MEM;
  char *cur_ptr = buf;
  cur_ptr = init_tuple(m_tuple, cur_ptr);
  cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
  if (needsFullTuple) {
    cur_ptr = init_full_tuple(m_full_tuple, cur_ptr);
    cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
  } else {
    /** in case we don't have virual columns - full_tuple is the same
     * as tuple */
    m_full_tuple = m_tuple;
  }
  cur_ptr = init_index_tuple(m_index_tuple, cur_ptr);
  CDE_ASSERT((uint64_t)cur_ptr <= (uint64_t)buf + total_size);
  return CDE_OK;
}

bool dml_ins_ctx::check_has_null(uint32_t col_nums) {
  uint32_t *index_cols = get_index()->shared_dict->index_cols;
  bool *is_nulls = tuple()->is_nulls;
  for (uint32_t i = 0; i < col_nums; i++) {
    if (is_nulls[m_table->heapAttrIdx(index_cols[i])]) {
      return true;
    }
  }
  return false;
}

dml_upd_ctx::~dml_upd_ctx() {
  CDE_ASSERT_DEBUG(nullptr == m_cascade);

  if (m_full_tuple_old && m_full_tuple_old != m_tuple_old) {
    for (uint32_t i = 0; i < m_fields_num; i++) {
      if (m_full_tuple_old->is_blob[i] &&
          m_full_tuple_old->varlenas[i].data != nullptr) {
        CdeFree(
            const_cast<unsigned char *>(m_full_tuple_old->varlenas[i].data));
        m_full_tuple_old->varlenas[i].data = nullptr;
      }
    }
  }
  if (m_tuple_old != nullptr) {
    for (uint32_t i = 0; i < m_fields_num - m_vfields_num; i++) {
      if (m_tuple_old->is_blob[i] && m_tuple_old->varlenas[i].data != nullptr) {
        CdeFree(const_cast<unsigned char *>(m_tuple_old->varlenas[i].data));
        m_tuple_old->varlenas[i].data = nullptr;
      }
    }
  }
  for (auto varlenaVirt : virtualDatum) delete[] varlenaVirt;
}

int dml_upd_ctx::init() {
  if (dml_ctx::init() != CDE_OK) {
    return CDE_ERROR;
  }
  bool needsFullTuple = m_vfields_num > 0;

  size_t total_size = calc_tuple_mem_size() + CDE_MEM_ALIGNMENT +
                      calc_index_tuple_mem_size() + CDE_MEM_ALIGNMENT +
                      calc_tuple_mem_size() + CDE_MEM_ALIGNMENT +
                      m_fields_num * sizeof(bool);

  if (needsFullTuple) {
    total_size += calc_full_tuple_mem_size() + CDE_MEM_ALIGNMENT +
                  calc_full_tuple_mem_size() + CDE_MEM_ALIGNMENT;
    calculatedVColumns.resize(m_fields_num, false);
  }

  char *buf = (char *)CdeAlloc(total_size);
  DBUG_EXECUTE_IF("alloc_mem_fail_injection", CdeFree(buf); buf = nullptr;);
  if (unlikely(buf == nullptr)) return HA_ERR_OUT_OF_MEM;
  char *cur_ptr = buf;
  cur_ptr = init_tuple(m_tuple, cur_ptr);
  cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
  cur_ptr = init_index_tuple(m_index_tuple, cur_ptr);
  cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
  cur_ptr = init_tuple(m_tuple_old, cur_ptr);
  cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
  if (needsFullTuple) {
    cur_ptr = init_full_tuple(m_full_tuple, cur_ptr);
    cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
    cur_ptr = init_full_tuple(m_full_tuple_old, cur_ptr);
    cur_ptr = (char *)CDE_MEM_ALIGN(cur_ptr);
  } else {
    m_full_tuple = m_tuple;
    m_full_tuple_old = m_tuple_old;
  }
  m_is_changed = (bool *)cur_ptr;

  CDE_ASSERT((uint64_t)cur_ptr + m_fields_num * sizeof(bool) <=
             (uint64_t)buf + total_size);
  return CDE_OK;
}

int dml_ins_ctx::create_heap_scan() {
  if (m_heap_scan != nullptr) {
    return CDE_OK;
  }

  StorageRelationData *dstore_rel = get_rel()->rd_storage_releation;
  m_heap_scan = HeapInterface::CreateHeapScanHandler(dstore_rel);
  if (m_heap_scan == nullptr) {
    return GetAndConvertDstoreErrcodeToMysql();
  }

  // current fk is disabled, ignore snapshot set.
  DSTORE::SnapshotData dstore_snapshot;
  CDESetSnapshotByTrans(dstore_snapshot);
  HeapInterface::BeginScan(m_heap_scan, &dstore_snapshot);

  return CDE_OK;
}

void dml_ins_ctx::destroy_heap_scan() {
  if (m_heap_scan != nullptr) {
    HeapInterface::EndScan(m_heap_scan);
    HeapInterface::DestroyHeapScanHandler(m_heap_scan);
    m_heap_scan = nullptr;
  }
}

int dml_ins_ctx::create_index_scan(session_index_info *index,
                                   uint32_t col_num) {
  if (m_index_scan != nullptr) {
    // todo: assert m_index_scan match index
    return CDE_OK;
  }
  bool is_asc = !(index->rel->index->indexOption[col_num - 1] & 1);

  for (uint32_t i = 0; i < col_num; i++) {
    index_tuple()->col_oids[i] = index->rel->attr->attrs[i]->atttypid;
    index_tuple()->col_collation_oids[i] =
        index->rel->attr->attrs[i]->attcollation;
  }

  ScanKey key_infos = index_tuple()->key_infos;
  cde_btree_api::BuildKeyInfos(
      key_infos, index->rel->attr->attrs, index_tuple()->index_values,
      index_tuple()->index_is_nulls, col_num, HA_READ_KEY_EXACT, is_asc);

  m_index_scan =
      IndexInterface::ScanBegin(index->rel, index->rel->index, col_num, 0);
  if (m_index_scan == nullptr) {
    return GetAndConvertDstoreErrcodeToMysql();
  }

  DSTORE::SnapshotData dstore_snapshot;
  /** Foreign key cascade update should always see the committed changes made
  by other sessions. Despite the transaction isolation level. Thus,
  setting snapshot to current. */
  CDESetSnapshotByCurrent(dstore_snapshot);
  IndexInterface::IndexScanSetSnapshot(m_index_scan, &dstore_snapshot);
  IndexInterface::ScanSetWantItup(m_index_scan, true);
  RetStatus status = IndexInterface::ScanRescan(m_index_scan, key_infos);
  if (status == DSTORE_FAIL) {
    CDE_LOG_ERROR("IndexInterface::ScanRescan return error: %d", status);
    return GetAndConvertDstoreErrcodeToMysql();
  }
  return CDE_OK;
}

int dml_ins_ctx::index_scan_next(ItemPointer &p_ctid, ScanDirection direction) {
  bool recheck = false;
  bool found = false;
  DSTORE::RetStatus status =
      IndexInterface::ScanNext(m_index_scan, direction, &found, &recheck);
  if (status == DSTORE::DSTORE_FAIL) {
    return HA_ERR_END_OF_FILE;
  }

  p_ctid = IndexInterface::GetResultHeapCtid(m_index_scan);
  if (!found || (p_ctid == nullptr) || (*p_ctid == INVALID_ITEM_POINTER)) {
    return HA_ERR_END_OF_FILE;
  }

  return CDE_OK;
}

int dml_ins_ctx::fetch_tuple(ItemPointer ctid, HeapTuple *&tuple,
                             HeapTuple *&realTuple, HeapTuple *&tupleWithLob) {
  tuple = nullptr;
  if (m_index_scan == nullptr) {
    return CDE_ERROR;
  }

  tuple = HeapInterface::FetchTuple(m_heap_scan, *ctid);
  if (tuple == nullptr) {
    CDE_LOG_ERROR("FetchTuple fail");
    return GetAndConvertDstoreErrcodeToMysql();
  }

  cde_isolation_level_t isolationLevel = READ_COMMITTED;
  int ret = CdeLockTuple(isolationLevel, &(get_trxinfo()->scanSnapshot),
                         get_rel()->rd_storage_releation, ctid, &tuple);
  if (ret != CDE_OK) {
    TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
    tuple = nullptr;
    return ret;
  }

  realTuple = tuple;
  DSTORE::TupleDesc tupleDesc = get_rel()->rd_storage_releation->attr;
  if (tupleDesc->tdhaslob) {
    tupleWithLob = FetchBlobFiled(get_rel()->rd_storage_releation, tuple,
                                  &(get_trxinfo()->scanSnapshot), false);
    if (!tupleWithLob) {
      TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
      return GetAndConvertDstoreErrcodeToMysql();
    }

    if (tuple != tupleWithLob) {
      realTuple = tupleWithLob;
    } else {
      tupleWithLob = nullptr;
    }
  }

  return CDE_OK;
}

void dml_ins_ctx::destroy_index_scan() {
  if (m_index_scan == nullptr) {
    return;
  }
  IndexInterface::ScanEnd(m_index_scan);
  m_index_scan = nullptr;
}

int dml_ins_ctx::fill_index_tuple(uint32_t fk_col_nums,
                                  dml_ins_ctx *parent_ctx) {
  Datum *index_values = index_tuple()->index_values;
  bool *index_is_nulls = index_tuple()->index_is_nulls;
  Datum *index_values_parent = parent_ctx->index_tuple()->index_values;
  bool *index_is_nulls_parent = parent_ctx->index_tuple()->index_is_nulls;

  for (uint32_t i = 0; i < fk_col_nums; i++) {
    index_values[i] = index_values_parent[i];
    index_is_nulls[i] = index_is_nulls_parent[i];
  }
  return CDE_OK;
}

/**
Checks if we need to keep memory allocated for virtual datum.

@param[in]  typeoid type to check

@return true    yes, we need to keep the memory allocated
        false   no, the memory was coppied, we can release
                the virtual datum
*/
static bool needsVirtualDatum(Oid typeOid) {
  Oid baseTypeOid = CdeConvertDatatypeOid(typeOid);
  return CdeCheckIsVarlenaType(baseTypeOid) || baseTypeOid == CDE_BIT_OID ||
         baseTypeOid == CDE_BIN_OID || baseTypeOid == CDE_CHAR_OID;
}

int dml_upd_ctx::DStoreGetComputedValue(TABLE *mysqlTable, DictVirtualCol *vCol,
                                        dml_tuple_t *tuple) {
  CDE_ASSERT_DEBUG(nullptr != mysqlTable);

  bool *isNulls = tuple->is_nulls;
  Datum *fullValues = tuple->values;

  std::unique_ptr<unsigned char[]> mysqlRec{
      new unsigned char[mysqlTable->s->reclength]};
  memset_s(mysqlRec.get(), mysqlTable->s->reclength, 0,
           mysqlTable->s->reclength);

  /* we may has index like this CREATE INDEX i7 ON t3((0.549590962943679))
  this 0.549590962943679 is saved as one HIDDEN_SQL generated virtual column
  in such case we don't need calculate, and we can't get from heap, so skip it
  and directly invoke my_eval_gcolumn_expr */
  if (!vCol->m_baseColumnInd.empty()) {
    uint32_t numberOfBaseColumns = vCol->m_baseColumnInd.size();
    std::vector<uint32_t> indexCol(numberOfBaseColumns);
    std::vector<uint32_t> attrCol(numberOfBaseColumns);
    std::unique_ptr<bool[]> indexIsNulls(new bool[numberOfBaseColumns]);
    std::unique_ptr<Datum[]> baseValues(new Datum[numberOfBaseColumns]);
    for (uint32_t i = 0; i < vCol->m_baseColumnInd.size(); ++i) {
      indexCol[i] = vCol->m_baseColumnInd[i];
      attrCol[i] = m_table->m_dictCols[indexCol[i]].m_phyPos;
      indexIsNulls[i] = isNulls[attrCol[i]];
      baseValues[i] = fullValues[attrCol[i]];
    }

    DstoreIndexDataToMysql(mysqlTable, mysqlRec.get(), baseValues.get(),
                           indexIsNulls.get(), indexCol.data(), attrCol.data(),
                           numberOfBaseColumns, m_table->attrFull,
                           m_blobMemRoot);
  }

  uint32_t virtualPos = m_table->m_dictCols[vCol->ind].m_phyPos;
  cde_dict_t *tableHandler = get_table();
  Oid typeOid = tableHandler->attrFull->attrs[virtualPos]->atttypid;
  dml_tuple_t destTuple;
  destTuple.values = &(fullValues[virtualPos]);
  destTuple.varlenas = &(tuple->varlenas[virtualPos]);
  destTuple.is_nulls = &(tuple->is_nulls[virtualPos]);
  destTuple.is_blob = &(tuple->is_blob[virtualPos]);
  int retStore = StoreVirtualFieldValueToDstore(
      m_current_thd, mysqlTable, mysqlRec.get(), vCol, typeOid, &destTuple);
  if (retStore != CDE_OK) {
    return retStore;
  }

  if (needsVirtualDatum(typeOid)) {
    /** check comment on virtualDatum*/
    virtualDatum.push_back(mysqlRec.release());
  }
  return retStore;
}

void dml_upd_ctx::FillFullTuple(dml_tuple_t *fromTuple, dml_tuple_t *toTuple) {
  Datum *fromValues = fromTuple->values;
  bool *fromIsNulls = fromTuple->is_nulls;
  CdeVarlena *fromVarlenas = fromTuple->varlenas;

  Datum *toValues = toTuple->values;
  bool *toIsNulls = toTuple->is_nulls;
  CdeVarlena *toVarlenas = toTuple->varlenas;
  toTuple->ctid = fromTuple->ctid;

  for (uint32_t i = 0; i < get_fields_num(); ++i) {
    if (m_table->m_dictCols[i].m_isVirtual) continue;
    uint32_t phyPos = m_table->m_dictCols[i].m_phyPos;
    toValues[phyPos] = fromValues[phyPos];
    toIsNulls[phyPos] = fromIsNulls[phyPos];
    toVarlenas[phyPos] = fromVarlenas[phyPos];
  }
}

void dml_upd_ctx::CalculateChangeVecForVirtualColumns() {
  bool *isChanged = get_is_changed_vec();

  for (auto vCol : m_table->m_vCols) {
    for (auto baseColumnInd : vCol.m_baseColumnInd) {
      if (isChanged[m_table->m_dictCols[baseColumnInd].m_phyPos])
        isChanged[m_table->m_dictCols[vCol.ind].m_phyPos] = true;
    }
  }
}

bool dml_upd_ctx::CheckNeedCalculateVirtualColumn(uint32_t *indexCols,
                                                  uint32_t indexColNum) {
  DictCol *dictCols = get_table()->m_dictCols;
  for (uint32_t i = 0; i < indexColNum; ++i) {
    if (dictCols[indexCols[i]].m_isVirtual && !calculatedVColumns[indexCols[i]])
      return true;
  }

  return false;
}

int dml_upd_ctx::FillVirtualColumns(uint32_t *indexCols, uint32_t indexColNum,
                                    TABLE *mysqlTable) {
  CDE_ASSERT_DEBUG(get_table()->m_vColCount > 0);
  DictCol *dictCols = get_table()->m_dictCols;

  /* open table's handler */
  Temp_table_handle tblhdl; /** handler will be closed by dtor */

  std::string dbName, tableName;
  int ret = GetDbAndTableNameFromDictName(m_table->name, dbName, tableName);
  if (CDE_OK != ret) return ret;

  if (dbName.empty()) {
    CDE_LOG_ERROR("Failed to get database name.");
    return HA_ERR_COMPUTE_FAILED;
  }

  if (tableName.empty()) {
    CDE_LOG_ERROR("Failed to get table name.");
    return HA_ERR_COMPUTE_FAILED;
  }

  if (nullptr == mysqlTable)
    mysqlTable = tblhdl.open(m_current_thd, dbName.c_str(), tableName.c_str());
  if (nullptr == mysqlTable) {
    CDE_LOG_ERROR("Failed to open table : %s.%s", dbName.c_str(),
                  tableName.c_str());
    return HA_ERR_COMPUTE_FAILED;
  }

  /** table's handler opened, calculate virtual columns now */
  for (uint32_t i = 0; i < indexColNum; ++i) {
    if (false == dictCols[indexCols[i]].m_isVirtual ||
        true == calculatedVColumns[indexCols[i]])
      continue;

    uint16_t vColPos = dictCols[indexCols[i]].m_virtualPos;
    DictVirtualCol &vCol = get_table()->m_vCols[vColPos];

    ret = DStoreGetComputedValue(mysqlTable, &vCol, m_full_tuple_old);
    if (CDE_OK != ret) return ret;
    /** we don't need new values for delete */
    if (false == is_delete()) {
      ret = DStoreGetComputedValue(mysqlTable, &vCol, m_full_tuple);
      if (CDE_OK != ret) return ret;
    }
  }
  return CDE_OK;
}

TABLE *dml_upd_ctx::GetMySQLTable() { return m_mysqlTable; }

int dml_upd_ctx::fill_tuple(HeapTuple *heap_tuple, uint32_t fk_col_nums,
                            uint8_t fk_rule, dml_upd_ctx *parent_ctx) {
  Datum *values_old = tuple_old()->values;
  bool *is_nulls_old = tuple_old()->is_nulls;
  TupleInterface::DeformHeapTuple(heap_tuple,
                                  get_rel()->rd_storage_releation->attr,
                                  values_old, is_nulls_old);

  Datum *values = tuple()->values;
  bool *is_nulls = tuple()->is_nulls;
  CdeVarlena *varlenas = tuple()->varlenas;
  Datum *values_parent = parent_ctx->tuple()->values;
  bool *is_nulls_parent = parent_ctx->tuple()->is_nulls;
  bool *is_changed = get_is_changed_vec();
  bool *is_changed_parent = parent_ctx->get_is_changed_vec();

  cde_dict_index_t *for_index = get_index()->shared_dict;
  TupleDescData *heap_desc = get_rel()->rd_storage_releation->attr;
  for (int i = 0; i < get_rel()->rd_storage_releation->attr->natts; i++) {
    values[i] = values_old[i];
    is_nulls[i] = is_nulls_old[i];
  }

  // is changed is used to check if any of the index fields got changed
  // this is why we need to initialize it for full tuple (inluding virtual
  // columns)
  for (uint32_t i = 0; i < get_fields_num(); ++i) is_changed[i] = false;

  cde_dict_index_t *ref_index = parent_ctx->get_index()->shared_dict;

  uint32_t fk_for_pos, fk_ref_pos;
  cde_dict_field_t *field_dict;
  Oid type_oid;
  if (!parent_ctx->is_delete() &&
      (fk_rule & CDE_DDL_FK_RULE_ON_UPDATE_CASCADE)) {
    /** ON UPDATE CASCADE */
    for (uint32_t i = 0; i < fk_col_nums; i++) {
      fk_for_pos = m_table->heapAttrIdx(for_index->index_cols[i]);
      fk_ref_pos = parent_ctx->m_table->heapAttrIdx(ref_index->index_cols[i]);

      is_nulls[fk_for_pos] = is_nulls_parent[fk_ref_pos];
      if (heap_desc->attrs[fk_for_pos]->attnotnull && is_nulls[fk_for_pos])
        return HA_ERR_ROW_IS_REFERENCED;

      // for indexing is_changed we need to used index position - which is aware
      // of virtual columns. is_changed is for check if index field got changed
      // and virtual columns can be indexed.
      is_changed[fk_for_pos] = is_changed_parent[fk_ref_pos];

      // for update cascade rule and update xxx set null
      if (is_nulls[fk_for_pos]) continue;

      type_oid = heap_desc->attrs[fk_for_pos]->atttypid;

      // handler plain type, regardless it came from dstore or mysql
      if (!CdeCheckIsVarlenaType(CdeRmCollFromOid(type_oid))) {
        values[fk_for_pos] = values_parent[fk_ref_pos];
        continue;
      }

      // handler varchar/char from dstore
      if (((uint64_t)values_parent[fk_ref_pos] & DEREF_TAG_MASK) == 0) {
        values[fk_for_pos] = values_parent[fk_ref_pos];
        continue;
      }

      // handler varchar,char from mysql
      field_dict = &(for_index->fields[i]);

      values[fk_for_pos] =
          CdeSetVarlenaWithPrefix(true, type_oid, values_parent[fk_ref_pos],
                                  field_dict, &(varlenas[fk_for_pos]));
      if (values[fk_for_pos] == INVALID_DATUM) return HA_ERR_ROW_IS_REFERENCED;
    }
  } else {
    if (fk_rule & (CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL |
                   CDE_DDL_FK_RULE_ON_DELETE_SET_NULL)) {
      /** ON UPDATE/DELETE SET NULL */
      for (uint32_t i = 0; i < fk_col_nums; i++) {
        fk_for_pos = m_table->heapAttrIdx(for_index->index_cols[i]);
        is_nulls[fk_for_pos] = true;
        if (heap_desc->attrs[fk_for_pos]->attnotnull)
          return HA_ERR_ROW_IS_REFERENCED;
        is_changed[fk_for_pos] = !(is_nulls_old[fk_for_pos]);
      }
    }
    /** Common matching for ON DELETE CASCADE and ON UPDATE/DELETE SET NULL */
    for (uint32_t i = 0; i < get_fields_num(); i++) {
      if ((m_table->m_dictCols[i].m_isVisible &&
           !m_table->m_dictCols[i].m_isVirtual)) {
        uint32_t heapDesAttrIdx = m_table->heapAttrIdx(i);
        if (!is_nulls_old[heapDesAttrIdx])
          set_varlena_datum(heap_desc->attrs[heapDesAttrIdx]->atttypid, i,
                            heapDesAttrIdx, values_old, varlenas);
      }
    }
  }

  if (m_table->m_vColCount > 0) {
    CDE_ASSERT_DEBUG(m_full_tuple != m_tuple);
    FillFullTuple(m_tuple_old, m_full_tuple_old);
    if (false == is_delete())
      FillFullTuple(m_tuple,
                    m_full_tuple); /** we don't need new values for delete */
    CalculateChangeVecForVirtualColumns();
  }

  return CDE_OK;
}

bool dml_upd_ctx::check_old_has_null(uint32_t col_nums) {
  uint32_t *index_cols = get_index()->shared_dict->index_cols;
  bool *is_nulls = tuple_old()->is_nulls;
  for (uint32_t i = 0; i < col_nums; i++) {
    if (is_nulls[m_table->heapAttrIdx(index_cols[i])]) {
      return true;
    }
  }
  return false;
}

void dml_upd_ctx::set_varlena_datum(const Oid &atttypeId, uint32_t fieldIdx,
                                    uint32_t valueIdx, Datum *values,
                                    CdeVarlena *varlenas) {
  DSTORE::Oid typeOid = CdeRmCollFromOid(atttypeId);
  if (CdeCheckIsVarlenaType(typeOid)) {
    if (typeOid == CDE_COMPACT_CHAR_OID) {
      auto length = m_table->m_fieldLenInfos[fieldIdx].m_packLength;
      auto mbmaxlen = m_table->m_fieldLenInfos[fieldIdx].m_mbmaxlen;
      auto mbminlen = m_table->m_fieldLenInfos[fieldIdx].m_mbminlen;
      CDE_ASSERT(mbmaxlen != 0);
      CDE_ASSERT((length % mbmaxlen) == 0);

      // refer to the CdeCmpctCharDstoreToMysql, use the buffer in
      // values[fieldIdx] directly to avoid memory alloc and copy.
      const uint16_t head_len =
          CdeGetCmpctCharHeadLen((char *)values[valueIdx]);
      const uint16_t char_len = CdeGetCmpctCharLen((char *)values[valueIdx]);

      values[valueIdx] = CdeSetCmpctCharDatum(
          (const unsigned char *)values[valueIdx] + head_len, char_len, length,
          mbmaxlen, mbminlen, &varlenas[valueIdx]);
    } else if ((typeOid == CDE_VARCHAR1B_OID) ||
               (typeOid == CDE_VARCHAR2B_OID)) {
      // the values is in dstore format, should convert to mysql format
      // first by CdeVarcharDstoreToMysql, and then convert varchar to special
      // format. but the CdeVarcharDstoreToMysql is implemented by memcpy_s,
      // so use values directly to replace the convertion from dstore to
      // mysql.
      CDE_ASSERT_DEBUG(m_table->m_fieldLenInfos[fieldIdx].m_fieldLenBytes != 0);
      values[valueIdx] = CdeSetVarcharDatumFromMysql(
          (const unsigned char *)values[valueIdx],
          m_table->m_fieldLenInfos[fieldIdx].m_fieldLenBytes,
          &varlenas[valueIdx]);
    }
  }
}

} /* namespace CDE */
