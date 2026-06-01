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

#include "ddl/ha_cde_alter.h"
#include "handler/ha_cde.h"
#include "sql/create_field.h"
#include "sql/dd/impl/types/column_impl.h"
#include "sql/dd/impl/types/table_impl.h"
#include "sql/dd/object_id.h"
#include "sql/dd/properties.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/table.h"

#include "common/cde_compare_utils.h"
#include "common/cde_errorcode.h"
#include "common/cde_perf.h"
#include "common/cde_prototypes.h"
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "dict/cde_dict_bg_stat.h"
#include "dml/cde_dml.h"

#include "ddl/cde_heap_builder.h"
#include "ddl/dd_helper.h"
#include "dml/cde_heap.h"

#include "mysql/plugin.h"
#include "scope_guard.h"
#include "sql/sql_alter.h"
#include "sql/sql_class.h"
#include "sql/sql_table.h"
#include "sql/sql_thd_internal_api.h"

#include "boot/cde_instance.h"
#include "framework/dstore_thread_interface.h"
#include "systable/dstore_systable_interface.h"
#include "tablespace/dstore_tablespace.h"
#include "transaction/dstore_transaction.h"
#include "transaction/dstore_transaction_interface.h"
#include "tuple/dstore_memheap_tuple.h"

using DSTORE::CallbackFunc;
using DSTORE::Datum;
using DSTORE::DSTORE_FAIL;
using DSTORE::DSTORE_SUCC;
using DSTORE::FunctionCallInfo;
using DSTORE::HeapTuple;
using DSTORE::IndexBuildInfo;
using DSTORE::RetStatus;
using DSTORE::ScanKey;
using DSTORE::SegmentType;
using DSTORE::StorageRelation;
using DSTORE::TablespaceId;
using DSTORE::TBS_ID;
using DSTORE::TupleDesc;

namespace CDE {

ConvertHeapValueToIndexValue::ConvertHeapValueToIndexValue(
    const TABLE *oldTable, TABLE *alteredTable, cde_dict_t *dictTable,
    TupleDesc oldHeapAttr, cde_dict_t *newDictTable,
    const cde_dict_index_t *newIndex, MEM_ROOT *blobMemRoot,
    std::unordered_map<uint32_t, DictVirtualCol> &new_addedVirtualCols,
    std::unordered_map<uint32_t, DictVirtualCol> &old_existedVirtualCols,
    bool isRebuild)
    : m_oldTable(oldTable),
      m_newTable(alteredTable),
      m_oldDictTable(dictTable),
      m_oldHeapAttr(oldHeapAttr),
      m_newDictTable(newDictTable),
      m_newIndex(newIndex),
      m_blobMemRoot(blobMemRoot),
      m_isRebuild(isRebuild) {
  for (uint32_t i = 0; i < newIndex->index_col_num; ++i) {
    cde_dict_field_t &field = newIndex->fields[i];
    FieldExtraInfo extraInfo;
    extraInfo.indexPos = i;
    extraInfo.heapPos = field.attId;
    m_maxHeapPos = std::max(m_maxHeapPos, extraInfo.heapPos);
    if (field.m_isVirtual) {
      if (!m_isRebuild) {
        auto newAddedVirtualCol = new_addedVirtualCols.find(field.fieldId);
        if (newAddedVirtualCol == new_addedVirtualCols.end()) {
          auto oldExistVirtualCol = old_existedVirtualCols.find(field.fieldId);
          if (oldExistVirtualCol != old_existedVirtualCols.end()) {
            extraInfo.virtualCol = &(oldExistVirtualCol->second);
          } else {
            extraInfo.virtualCol =
                &(dictTable->m_vCols[dictTable->m_dictCols[field.fieldId]
                                         .m_virtualPos]);
          }
        } else {
          extraInfo.virtualCol = &(newAddedVirtualCol->second);
        }
      } else {
        extraInfo.virtualCol =
            &(m_newDictTable->m_vCols[m_newDictTable->m_dictCols[field.fieldId]
                                          .m_virtualPos]);
      }
      if (!extraInfo.virtualCol->m_baseColumnInd.empty()) {
        extraInfo.numColumnsToDecode =
            extraInfo.virtualCol->m_baseColumnInd.size();
        extraInfo.columnsToDecode = (MysqlAndDstoreIndex *)Memory::Zalloc(
            sizeof(MysqlAndDstoreIndex) * extraInfo.numColumnsToDecode);
        for (size_t j = 0; j < extraInfo.numColumnsToDecode; ++j) {
          uint16_t pos = extraInfo.virtualCol->m_baseColumnInd[j];
          if (!m_hasBlob) {
            Field *sqlField =
                m_isRebuild ? alteredTable->field[pos] : oldTable->field[pos];
            auto type = sqlField->type();
            m_hasBlob = (is_blob(type) || type == MYSQL_TYPE_JSON);
          }

          if (m_isRebuild) {
            extraInfo.columnsToDecode[j].m_mysqlIndex = pos;
            extraInfo.columnsToDecode[j].m_dstoreIndex =
                m_newDictTable->m_dictCols[pos].m_phyPos;
          } else {
            extraInfo.columnsToDecode[j].m_mysqlIndex =
                getFiledSQLIndexInNewTable(pos);
            extraInfo.columnsToDecode[j].m_dstoreIndex =
                dictTable->m_dictCols[pos].m_phyPos;
          }
          m_deformTupleEndIndex =
              std::max(m_deformTupleEndIndex,
                       (uint32_t)extraInfo.columnsToDecode[j].m_dstoreIndex);
        }
      }
      m_canMultiThread = false;
    } else if (field.prefix_len != 0) {
      extraInfo.virtualCol = nullptr;
      m_deformTupleEndIndex = std::max(m_deformTupleEndIndex, field.attId);
      m_canMultiThread = false;
    }
    m_fieldExtraInfos.push_back(extraInfo);
  }
  ++m_deformTupleEndIndex;
  ++m_maxHeapPos;
  m_relation = m_isRebuild ? m_newDictTable->dstore_relation
                           : m_oldDictTable->dstore_relation;
}

uint16_t ConvertHeapValueToIndexValue::getFiledSQLIndexInNewTable(
    uint16_t oldSqlIndex) {
  const char *colName = m_oldDictTable->get_col_name(oldSqlIndex);
  for (ulint i = 0; i < m_newTable->s->fields; i++) {
    Field *field = m_newTable->field[i];
    if (CdeStrcasecmp(field->field_name, colName) == 0) {
      return (uint16_t)i;
    }
  }

  return (uint16_t)-1;
}

void ConvertHeapValueToIndexValue::init() {
  size_t totalSize =
      sizeof(dml_tuple_t) +
      m_maxHeapPos * (sizeof(DSTORE::Datum) + sizeof(CdeVarlena) +
                      sizeof(bool) + sizeof(bool));
  char *buf = (char *)Memory::Zalloc(totalSize);
  m_fullTuple = (dml_tuple_t *)buf;
  buf += sizeof(dml_tuple_t);
  m_fullTuple->values = (Datum *)buf;
  buf += m_maxHeapPos * sizeof(Datum);
  m_fullTuple->varlenas = (CdeVarlena *)buf;
  buf += m_maxHeapPos * sizeof(CdeVarlena);
  m_fullTuple->is_nulls = (bool *)buf;
  buf += m_maxHeapPos * sizeof(bool);
  m_fullTuple->is_blob = (bool *)buf;

  m_record = (unsigned char *)Memory::Zalloc(m_newTable->s->reclength);
}

void ConvertHeapValueToIndexValue::destroy() {
  Memory::Free(m_record);
  m_record = nullptr;
  Memory::Free(m_fullTuple);
  m_fullTuple = nullptr;

  for (auto &fieldInfo : m_fieldExtraInfos) {
    if (fieldInfo.virtualCol &&
        !fieldInfo.virtualCol->m_baseColumnInd.empty()) {
      Memory::Free(fieldInfo.columnsToDecode);
      fieldInfo.columnsToDecode = nullptr;
    }
  }
  m_fieldExtraInfos.clear();
}

Datum ConvertHeapValueToIndexValue::operator()(FunctionCallInfo fcinfo) {
  if (m_fieldExtraInfos.empty()) {
    return DSTORE_SUCC;
  }

  HeapTuple *heapTuple = (HeapTuple *)DatumGetPointer(fcinfo->prealloc_arg[0]);
  HeapTuple *tupleWithLob = nullptr;
  HeapTuple *realTuple = heapTuple;
  if (m_hasBlob) {
    tupleWithLob = FetchBlobFiled(
        m_relation, heapTuple,
        DSTORE::thrd->GetActiveTransaction()->GetSnapshot(), true);
    if (unlikely(nullptr == tupleWithLob)) {
      CDE_LOG_ERROR("FetchBlobFiled failed for table:%s, index:%s",
                    m_oldDictTable->name.c_str(), m_newIndex->name.c_str());
      return DSTORE_FAIL;
    }

    if (heapTuple != tupleWithLob) {
      realTuple = tupleWithLob;
    } else {
      tupleWithLob = nullptr;
    }
  }
  bool *heapIsNulls = m_fullTuple->is_nulls;
  Datum *heapValues = m_fullTuple->values;
  CdeVarlena *varLena = m_fullTuple->varlenas;

  DSTORE::TupleAttrContext attrContext = {
      m_isRebuild ? m_relation->attr : m_oldHeapAttr, heapValues, heapIsNulls,
      0, true};
  /** m_deformTupleEndIndex already add 1 in constructor */
  realTuple->DeformTuplePart(attrContext, 0, m_deformTupleEndIndex);
  Datum *indexValues = (Datum *)DatumGetPointer(fcinfo->prealloc_arg[1]);
  bool *indexNulls = (bool *)DatumGetPointer(fcinfo->prealloc_arg[2]);
  for (auto fieldExtraInfo : m_fieldExtraInfos) {
    if (fieldExtraInfo.virtualCol) {
      if (!fieldExtraInfo.virtualCol->m_baseColumnInd.empty())
        DstoreHeapDataToMysql(
            m_newTable, m_record, heapValues, heapIsNulls, m_blobMemRoot, true,
            fieldExtraInfo.columnsToDecode, fieldExtraInfo.numColumnsToDecode,
            m_isRebuild ? m_relation->attr : m_oldHeapAttr);

      Oid typeOid =
          m_newIndex->rel->attr->attrs[fieldExtraInfo.indexPos]->atttypid;
      dml_tuple_t destTuple;
      destTuple.values = &(heapValues[fieldExtraInfo.heapPos]);
      destTuple.varlenas = &(varLena[fieldExtraInfo.heapPos]);
      destTuple.is_nulls = &(indexNulls[fieldExtraInfo.indexPos]);
      destTuple.is_blob = nullptr;
      int retStore = StoreVirtualFieldValueToDstore(
          current_thd, m_newTable, m_record, fieldExtraInfo.virtualCol, typeOid,
          &destTuple);

      if (retStore) {
        CDE_LOG_ERROR(
            "calculate virtual column value failed for table:%s, index:%s",
            m_oldDictTable->name.c_str(), m_newIndex->name.c_str());
        return DSTORE_FAIL;
      }
    } else {
      indexNulls[fieldExtraInfo.indexPos] = heapIsNulls[fieldExtraInfo.heapPos];
    }

    /** first check is null */
    if (indexNulls[fieldExtraInfo.indexPos]) {
      continue;
    }

    const cde_dict_field_t &field = m_newIndex->fields[fieldExtraInfo.indexPos];
    if (field.prefix_len != 0) {
      indexValues[fieldExtraInfo.indexPos] = CdeSetVarlenaWithPrefix(
          false, m_newIndex->rel->index->opcinType[fieldExtraInfo.indexPos],
          heapValues[fieldExtraInfo.heapPos], &field,
          &varLena[fieldExtraInfo.heapPos], !fieldExtraInfo.virtualCol);
    } else {
      indexValues[fieldExtraInfo.indexPos] = heapValues[fieldExtraInfo.heapPos];
    }
  }

  if (tupleWithLob != nullptr) {
    TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
  }
  return DSTORE_SUCC;
}

RetStatus ExprHandleHeapTupleToIndex(CallbackFunc fnAddr,
                                     FunctionCallInfo fcinfo, Datum *result) {
  /* return value only represent callback execute normal */
  Datum callbackResult = fnAddr(fcinfo);
  if (callbackResult != (Datum)DSTORE_FAIL) {
    *result = callbackResult;
    return DSTORE_SUCC;
  }

  return DSTORE_FAIL;
}

using Columns = std::vector<Field *>;

enum class DSTORE_INSTANT_OP {
  /** Instant op rename column only */
  RENAME_COLUMN_ONLY,

  /** Instant op varchar resize only */
  VARCHAR_RESIZE_ONLY,

  /** Instant op rename column and varchar resize */
  RENAME_COLUMN_WITH_VARCHAR_RESIZE,

  /*!< Only virtual column ADD AND DROP */
  VIRTUAL_ADD_DROP_ONLY,

  /*!< Virtual column ADD/DROP with RENAME */
  VIRTUAL_ADD_DROP_WITH_RENAME,

  /** Instant add column */
  INSTANT_ADD,

  /** Instant drop column */
  INSTANT_DROP,

  /** No instant op */
  NONE
};

/** Operations for creating indexes (no rebuild needed) */
static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_ONLINE_CREATE =
    Alter_inplace_info::ADD_INDEX | Alter_inplace_info::ADD_UNIQUE_INDEX |
    Alter_inplace_info::ADD_PK_INDEX;

/** Operations for rebuilding a table in place */
static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_ALTER_REBUILD =
    Alter_inplace_info::CHANGE_CREATE_OPTION |
    Alter_inplace_info::ALTER_COLUMN_NULLABLE |
    Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE |
    Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |
    Alter_inplace_info::DROP_STORED_COLUMN |
    Alter_inplace_info::ADD_STORED_BASE_COLUMN |
    Alter_inplace_info::RECREATE_TABLE;

static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_ALTER_DATA =
    DSTORE_ONLINE_CREATE | DSTORE_ALTER_REBUILD;

/** Operations that dstore support instant algorithm */
static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_INSTANT_ALLOWED =
    Alter_inplace_info::ALTER_COLUMN_NAME |
    Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH |
    Alter_inplace_info::ADD_VIRTUAL_COLUMN |
    Alter_inplace_info::DROP_VIRTUAL_COLUMN |
    Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER |
    Alter_inplace_info::ADD_STORED_BASE_COLUMN |
    Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |
    Alter_inplace_info::DROP_STORED_COLUMN;

/** Operations on foreign key definitions (changing the schema only) */
static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_FOREIGN_OPERATIONS =
    Alter_inplace_info::DROP_FOREIGN_KEY | Alter_inplace_info::ADD_FOREIGN_KEY;

/** Operations that dstore does not care about.

ALTER_COLUMN_INDEX_LENGTH is now only set for varchar expansion when pack length
doesn't change(ALTER_COLUMN_EQUAL_PACK_LENGTH is also set), this operation can
be ignored because varchar expansion in this case in dstore is INSTANT. */
static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_INPLACE_IGNORE =
    Alter_inplace_info::ALTER_COLUMN_DEFAULT |
    Alter_inplace_info::ALTER_COLUMN_COLUMN_FORMAT |
    Alter_inplace_info::ALTER_COLUMN_STORAGE_TYPE |
    Alter_inplace_info::ALTER_RENAME | Alter_inplace_info::CHANGE_INDEX_OPTION |
    Alter_inplace_info::ADD_CHECK_CONSTRAINT |
    Alter_inplace_info::DROP_CHECK_CONSTRAINT |
    Alter_inplace_info::SUSPEND_CHECK_CONSTRAINT |
    Alter_inplace_info::ALTER_COLUMN_VISIBILITY |
    Alter_inplace_info::ALTER_COLUMN_INDEX_LENGTH;

/** Operations that dstore cares about and can perform without rebuild */
static const Alter_inplace_info::HA_ALTER_FLAGS DSTORE_ALTER_NOREBUILD =
    DSTORE_ONLINE_CREATE | DSTORE_FOREIGN_OPERATIONS |
    Alter_inplace_info::DROP_INDEX | Alter_inplace_info::DROP_UNIQUE_INDEX |
    Alter_inplace_info::DROP_PK_INDEX | Alter_inplace_info::RENAME_INDEX |
    Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH |
    Alter_inplace_info::ALTER_INDEX_COMMENT |
    Alter_inplace_info::ADD_VIRTUAL_COLUMN |
    Alter_inplace_info::DROP_VIRTUAL_COLUMN |
    Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER |
    Alter_inplace_info::ALTER_COLUMN_NAME;

/** Adjust the create index column number from "New table" to
"old InnoDB table" while we are doing adding/dropping virtual column. Since we
do not create separate new table for the dropping/adding virtual columns. To
correctly find the indexed column, we will need to find its dict_col_no in the
"Old Table", not the "New table".
@param[in]      haAlterInfo    Data used during in-place alter
@param[in]      oldTable       MySQL table as it is before the ALTER operation
@param[in]      alteredTable   altered MySQL table
@param[in,out]  index          index definition
@param[in,out]  ctx            alter context */
static void AdjustIndexFieldsDictCol(const Alter_inplace_info *haAlterInfo
                                     [[maybe_unused]],
                                     const TABLE *oldTable,
                                     const TABLE *alteredTable,
                                     cde_index_def *indexDef,
                                     ha_cde_inplace_ctx *ctx) {
  for (uint32_t i = 0; i < indexDef->n_fields; ++i) {
    cde_field_def &indexField = indexDef->fields[i];
    /** new added virtual column as index field, we already handle it in
     * CdeCreateIndexDef */
    if (indexField.ddCol) continue;

    Field *newField = alteredTable->field[indexField.col_no];
    for (uint32_t old_i = 0; oldTable->field[old_i]; old_i++) {
      if (CdeStrcasecmp(oldTable->field[old_i]->field_name,
                        newField->field_name) == 0) {
        indexField.dict_col_no = old_i;
        break;
      }
    }

    if (IsVirtualGeneratedField(newField)) {
      DictVirtualCol virtualCol;
      virtualCol.ind = indexField.col_no;
      setupBaseColumns(ctx->old_table, newField, &virtualCol);
      ctx->old_existVirtualCols.emplace(indexField.col_no, virtualCol);
    }
  }
}

static void CdeCreateIndexDef(const Alter_inplace_info *haAlterInfo,
                              const TABLE *old_table,
                              const TABLE *altered_table,
                              const dd::Table *new_dd_tab, const KEY *keys,
                              uint32_t key_number, cde_index_def *index,
                              THD *thd, ha_cde_inplace_ctx *ctx) {
  const KEY *key = &keys[key_number];
  uint32_t n_fields = key->user_defined_key_parts;
  index->fields = (cde_field_def *)CdeZalloc(sizeof(cde_field_def) * n_fields);
  if (index->fields == nullptr) {
    CDE_LOG_ERROR("CdeZalloc fail!");
    CDE_ASSERT(0);
  }

  auto &addedVirtualCols = ctx->new_addedVirtualCols;
  index->indisunique = (key->flags & HA_NOSAME) ? 1 : 0;
  index->name = key->name;
  for (uint i = 0; i < n_fields; i++) {
    KEY_PART_INFO *key_part;
    key_part = &key->key_part[i];
    index->key_length += key_part->length;
    Field *field = key_part->field;
    if (field->is_nullable()) {
      index->key_length += HA_KEY_NULL_LENGTH;
    }
    if (field->type() == MYSQL_TYPE_BLOB ||
        field->real_type() == MYSQL_TYPE_VARCHAR ||
        field->type() == MYSQL_TYPE_GEOMETRY) {
      index->key_length += HA_KEY_BLOB_LENGTH;
    }
  }
  index->n_fields = n_fields;
  index->fillfactor = DstoreParseFillfactor(key->comment.str, thd);
  index->m_keyNumber = key_number;
  for (uint32_t i = 0; i < n_fields; i++) {
    cde_field_def &indexField = index->fields[i];
    indexField.col_no = key->key_part[i].fieldnr;
    indexField.dict_col_no = indexField.col_no;
    indexField.is_ascending =
        !(key->key_part[i].key_part_flag & HA_REVERSE_SORT);
    if (!ctx->needRebuild) {
      auto newAddedVcol = addedVirtualCols.find(indexField.col_no);
      if (newAddedVcol != addedVirtualCols.end()) {
        indexField.virtualPos = indexField.col_no - ctx->minAddedVirtualInd;
        indexField.ddCol = DDFindColumn(
            new_dd_tab, altered_table->field[indexField.col_no]->field_name);
        CDE_ASSERT_DEBUG(indexField.ddCol);
        indexField.vCol = &(newAddedVcol->second);
      }
    }
  }

  if (!ctx->needRebuild &&
      ((haAlterInfo->handler_flags & Alter_inplace_info::ADD_VIRTUAL_COLUMN) ||
       (haAlterInfo->handler_flags &
        Alter_inplace_info::DROP_VIRTUAL_COLUMN))) {
    AdjustIndexFieldsDictCol(haAlterInfo, old_table, altered_table, index, ctx);
  }
}

static std::unique_ptr<cde_index_def[]> CdeCreateIndexDefs(
    const Alter_inplace_info *ha_alter_info, const TABLE *old_table,
    const TABLE *altered_table, const dd::Table *new_dd_table, uint32_t &n_add,
    THD *thd) {
  const uint32_t *const add = ha_alter_info->index_add_buffer;
  const KEY *const key_info = ha_alter_info->key_info_buffer;

  auto indexdefs = std::make_unique<cde_index_def[]>(ha_alter_info->key_count);
  if (indexdefs == nullptr) {
    CDE_LOG_ERROR("CdeZalloc fail!");
    CDE_ASSERT(0);
  }
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(ha_alter_info->handler_ctx);
  for (uint32_t i = 0; i < n_add; i++) {
    CdeCreateIndexDef(ha_alter_info, old_table, altered_table, new_dd_table,
                      key_info, add[i], &indexdefs[i], thd, ctx);
  }
  return indexdefs;
}

/*
Report a dstore error to the client by invoking my_error().

@param[in]  error       the error code.
@param[in]  thd         the thd of worker thread.
*/
static void ReportInplaceAlterError(HaErrorCode error, THD *thd = nullptr) {
  switch (error) {
    case HA_ERR_LOCK_DEADLOCK:
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      break;
    case HA_ERR_LOCK_WAIT_TIMEOUT:
      my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      break;
    case HA_ERR_QUERY_INTERRUPTED:
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      break;
    case HA_ERR_OUT_OF_MEM:
      CDE_LOG_ERROR(
          "Dstore inplace alter table fail due to out of resources, error code "
          "is %lld, error message: %s",
          GetDstoreErrcode(), GetDstoreErrmsg());
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      break;
    case HA_ERR_TEMP_FILE_WRITE_FAILURE:
      my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
      break;
    case HA_ERR_LOCK_TABLE_FULL:
      my_error(ER_LOCK_TABLE_FULL, MYF(0));
      break;
    case (int)HaDstoreErrE::HA_DSTORE_ERR_TUPLE_TOO_BIG:
      my_error(ER_DSTORE_ERR_TUPLE_TOO_BIG, MYF(0), MaxAllocSize);
      break;
    case (int)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT:
      CDE_LOG_ERROR(
          "Dstore inplace alter table fail due to tuple sort error, error code "
          "is %lld, error message: %s",
          GetDstoreErrcode(), GetDstoreErrmsg());
      my_error(ER_DSTORE_INPLACE_GENERIC_ERR, MYF(0), "when sorting tuple",
               GetDstoreErrcode());
      break;
    case (int)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_OUT_OF_RESOURCES:
      CDE_LOG_ERROR(
          "Dstore inplace alter table fail due to out of resources, error code "
          "is %lld, error message: %s",
          GetDstoreErrcode(), GetDstoreErrmsg());
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      break;
    case (int)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_GET_EXPRESSION_VAL:
      CDE_LOG_ERROR(
          "Dstore inplace alter table fail due to caculate expression value, "
          "error code is %lld, error message: %s",
          GetDstoreErrcode(), GetDstoreErrmsg());
      my_error(ER_DSTORE_INPLACE_GENERIC_ERR, MYF(0),
               "when getting expression val", GetDstoreErrcode());
      break;
    case HA_ERR_AUTOINC_READ_FAILED:
      my_error(ER_AUTOINC_READ_FAILED, MYF(0));
      break;
    case (int)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_INVALID_USE_OF_NULL:
      my_error(ER_INVALID_USE_OF_NULL, MYF(0));
      break;
    case (int)HaDstoreErrE::HA_DSTORE_ERR_TABLESPACE_FULL:
      my_error(ER_DSTORE_ERR_TABLESPACE_REACH_MAXSIZE, MYF(0));
      break;
    default:
      if (thd && thd->is_killed()) {
        /** Now mainly used to report the "Query execution was interrupted"
        error when the ddl log record stage was interrupted. */
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        break;
      }
      CDE_LOG_ERROR(
          "Dstore inplace alter table fail, error code is %lld, error message: "
          "%s",
          GetDstoreErrcode(), GetDstoreErrmsg());
      my_error(ER_DSTORE_INPLACE_GENERIC_ERR, MYF(0), "", GetDstoreErrcode());
      break;
  }
}

static void InplaceDdlCleanEnv(
    uint32_t dropIndexNum, cde_dict_index_t **dropIndex,
    uint32_t renameIndexNum, cde_dict_index_t **renameIndex,
    const char **colNames, cde_dict_foreign_t **dropForeign,
    ulint addForeignNum, cde_dict_foreign_t **addForeign) {
  if (dropIndexNum > 0) {
    /* Clear the to_be_dropped flags, which might
    have been set at this point. */
    for (uint32_t i = 0; i < dropIndexNum; i++) {
      CDE_ASSERT_DEBUG(dropIndex[i]->IsCommitted());
      dropIndex[i]->toBeDropped = 0;
    }

    if (dropIndex) {
      CdeFree(dropIndex);
    }
  }

  if (renameIndexNum > 0 && renameIndex) {
    CdeFree(renameIndex);
  }

  if (colNames != nullptr) {
    CdeFree(colNames);
  }

  if (dropForeign) {
    CdeFree(dropForeign);
  }

  if (addForeignNum > 0) {
    for (ulint i = 0; i < addForeignNum; i++) {
      if (addForeign[i]) {
        delete addForeign[i];
        addForeign[i] = nullptr;
      }
    }
  }
  CdeFree(addForeign);
}

bool ha_cde::CdeInplaceBuildIndexes(TABLE *alteredTable,
                                    Alter_inplace_info *haAlterInfo) {
  Huawei::Common::LatencyCounter::Timer t(
      cde_perf_counters::getInstance()->_inplace_build_indexes_latency);

  static constexpr uint32_t INVALID_KEY_NUMBER =
      std::numeric_limits<uint32_t>::max();

  int ret = DSTORE_SUCC;
  uint32_t errKeyNum = INVALID_KEY_NUMBER;
  DSTORE::IndexTuple *duplicateTuple = nullptr;
  cde_dict_index_t *errDictIndex = nullptr;

  THD *thd = ha_thd();
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);

  /* Note that the TupleDesc of the heap relation is modified during the
  parallel index building process, causing a change in attr->attrs->attcacheoff.
  Therefore, a deep copy is needed to create a temporary relation. Refer to the
  function call CollectTuplesFromTable->FormTuple->...->CalculateOffset for
  details.*/
  uint32_t numAddedIndex = ctx->needRebuild
                               ? ctx->new_table->index_dict_vec.size()
                               : ctx->num_to_add_index;
  if (numAddedIndex == 0) {
    return false;
  }

  cde_dict_t *useTable = ctx->needRebuild ? ctx->new_table : ctx->old_table;
  auto relInfo = std::make_unique<session_rel_info>(useTable->m_id);
  cde_dict_index_t **add_index =
      ctx->needRebuild ? &(ctx->new_table->index_dict_vec[0]) : ctx->add_index;
  ret = CdeConstructRelInfoForInplaceAlter(useTable, relInfo.get(), add_index,
                                           numAddedIndex);
  if (CDE_OK != ret) {
    CDE_LOG_ERROR("Construct rel info for inplace alter fail.");
    ReportInplaceAlterError(HA_ERR_OUT_OF_MEM);
    return true;
  }
  auto heapRel = relInfo->rd_storage_releation;
  auto attrFull = heapRel->attr;
  IndexBuildInfo buildInfo;
  buildInfo.heapRels = nullptr;

  OnlineDdlMgr *onlineMgr = m_dstore->table_handler->m_onlineDdlMgr;
  if (haAlterInfo->online) {
    CDE_ASSERT(onlineMgr != nullptr);
    CDE_ASSERT(onlineMgr->GetStatus() == OnlineDdlStatus::CREATING_INDEX);
    onlineMgr->SetIndexBuildSnapShot();
  }

  for (uint32_t i = 0; i < numAddedIndex; i++) {
    buildInfo =
        CreateIndexBuildInfo(heapRel, attrFull, add_index[i]->index_col_num,
                             add_index[i]->attr_cols, add_index[i]->fields);
    buildInfo.baseInfo = *(relInfo->cde_index_vec[i]->rel->index);
    DSTORE::StorageRelation indexRel = relInfo->cde_index_vec[i]->rel;
    DSTORE::ScanKey scanKey = relInfo->cde_index_vec[i]->scan_key;
    buildInfo.baseInfo.keyLength =
        CdeExpandKeyLenByTD() ? add_index[i]->key_length : 0;
    buildInfo.baseInfo.indexRelId = relInfo->cde_index_vec[i]->rel->relOid;

    CDE_ASSERT(indexRel != nullptr);
    CDE_ASSERT(scanKey != nullptr);

    ConvertHeapValueToIndexValue exprCallBackFunctor(
        table, alteredTable, ctx->old_table,
        ctx->old_table->dstore_relation->attr, ctx->new_table, add_index[i],
        &m_blob_mem_root, ctx->new_addedVirtualCols, ctx->old_existVirtualCols,
        ctx->needRebuild);
    if (!exprCallBackFunctor.m_canMultiThread) {
      exprCallBackFunctor.init();
      buildInfo.baseInfo.exprCallback = exprCallBackFunctor;
      buildInfo.baseInfo.exprInitCallback = [&](FunctionCallInfo fcinfo
                                                [[maybe_unused]]) -> Datum {
        current_thd = thd;
        return PointerGetDatum(thd);
      };
    }

    if (ThdDdlThreads(thd) > 1 && exprCallBackFunctor.m_canMultiThread) {
      /* When the configured ddl threads is greater than 0, we will use parallel
      index building. */
      DEBUG_SYNC_C("WaitRowLogAppendAndApply");
      ret = IndexInterface::BuildParallel(indexRel, scanKey, &buildInfo,
                                          ThdDdlThreads(thd));
    } else {
      ret = IndexInterface::Build(indexRel, scanKey, &buildInfo);
    }

    uint64_t heapTupleNum = static_cast<uint64_t>(buildInfo.heapTuples);

    if (!exprCallBackFunctor.m_canMultiThread) {
      exprCallBackFunctor.destroy();
    }
    DBUG_EXECUTE_IF("dstore_build_index_fail", ret = DSTORE_FAIL;);
    if (ret != DSTORE_SUCC) {
      add_index[i]->SetOnlineStatus(OnlineIndexStatus::ONLINE_INDEX_ABORTED);
      errKeyNum = ctx->needRebuild ? (&add_index[i] - add_index)
                                   : ctx->add_key_numbers[i];
      duplicateTuple = buildInfo.duplicateTuple;
      errDictIndex = add_index[i];
      CdeFree(buildInfo.heapRels);
      if (haAlterInfo->online) {
        onlineMgr->SetAbort(OnlineDdlErrcode::ONLINE_OK);
      }
      break;
    }

    if (haAlterInfo->online) {
      if (onlineMgr->StartRowLogReplayForIndex(ctx->add_index[i], heapTupleNum,
                                               i) == CDE_FAIL) {
        break;
      }
    }

    add_index[i]->SetOnlineStatus(OnlineIndexStatus::ONLINE_INDEX_COMPLETE);
    CdeFree(buildInfo.heapRels);
    DEBUG_SYNC_C("dstore_after_one_rowlog_replay_thread_created");
  }

  if (haAlterInfo->online && !onlineMgr->NeedAbort()) {
    onlineMgr->WaitUntilRowLogReplayNearlyFinish();
  }

  /* The result of handling row log will be dealed at
   * CommitInplaceAlterTableImpl. */
  if (ret == DSTORE_SUCC) {
    return false;
  }

  CDE_ASSERT(errKeyNum != INVALID_KEY_NUMBER);
  // error handling
  auto err = GetAndConvertDstoreErrcodeToMysql();
  DBUG_EXECUTE_IF("dstore_OOM_inplace_alter", err = HA_ERR_OUT_OF_MEM;);
  DBUG_EXECUTE_IF(
      "dstore_inplace_tuple_sort_err",
      err = (HaErrorCode)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_TUPLESORT;);
  DBUG_EXECUTE_IF(
      "dstore_inplace_get_expr_val_err",
      err =
          (HaErrorCode)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_GET_EXPRESSION_VAL;);
  DBUG_EXECUTE_IF(
      "dstore_inplace_out_of_resource_err",
      err = (HaErrorCode)HaDstoreErrE::HA_DSTORE_ERR_INPLACE_OUT_OF_RESOURCES;);
  DBUG_EXECUTE_IF("dstore_inplace_generic_err", err = HA_ERR_GENERIC;);
  switch (err) {
    case CDE_SUCC:
      return false;
    case HA_ERR_FOUND_DUPP_KEY: {
      CDE_ASSERT(duplicateTuple != nullptr);
      CDE_ASSERT(errDictIndex != nullptr);
      KEY *dup_key{};
      dup_key = ctx->needRebuild ? &alteredTable->key_info[errKeyNum]
                                 : &haAlterInfo->key_info_buffer[errKeyNum];
      CdeInplaceAlterReportDupKey(alteredTable, errDictIndex, duplicateTuple,
                                  useTable->attrFull, dup_key,
                                  table_share->table_name.str);
      break;
    }
    default: {
      if (err == HA_ERR_QUERY_INTERRUPTED && haAlterInfo->online &&
          onlineMgr->IsThrdAbortedByDdlMgr()) {
        onlineMgr->ReportErrorInfo(alteredTable, m_dstore, haAlterInfo);
      } else {
        ReportInplaceAlterError(err, thd);
      }
    }
  }

  DSTORE::DestroyObject((void **)(&duplicateTuple));

  return true;
}

void CdeInplaceAlterReportDupKey(TABLE *alteredTable,
                                 cde_dict_index_t *dictIndex,
                                 DSTORE::IndexTuple *dupIndexTuple,
                                 DSTORE::TupleDesc attrFull, KEY *dupKey,
                                 const char *msg) {
  auto valuesWithoutVarlen =
      std::make_unique<Datum[]>(dictIndex->index_col_num);
  auto nulls = std::make_unique<bool[]>(dictIndex->index_col_num);
  TupleInterface::DeformIndexTuple(dupIndexTuple, dictIndex->rel->attr,
                                   valuesWithoutVarlen.get(), nulls.get());
  DstoreIndexDataToMysql(alteredTable, alteredTable->record[0],
                         valuesWithoutVarlen.get(), nulls.get(),
                         dictIndex->index_cols, dictIndex->attr_cols,
                         dictIndex->index_col_num, attrFull);
  print_keydup_error(alteredTable, dupKey, MYF(0), msg);
}

/** Check if is allowed to rename column
@param[in]  ha_alter_info   handler alter table info
@param[in]  old_table       old TABLE
@param[in]  altered_table   new TABLE
@param[in]  dict_table      dict_table
@param[in]  instant         is instant algorithm
@param[in]  report_error    whether need report error
@return true is allow to rename column, false not
*/
static bool CdeAllowToRenameColumn(const Alter_inplace_info *ha_alter_info,
                                   const TABLE *old_table,
                                   const TABLE *altered_table [[maybe_unused]],
                                   const cde_dict_t *dict_table,
                                   bool instant [[maybe_unused]],
                                   bool report_error) {
  /** All children are locked using MDL lock on server layer. We don't allow
   * eviction of children tables.
   * Thus, it is not possible for referenced_dict_set to be changed by other
   * threads while we access it here.
   */
  /* referenced set is empty, no need process */
  if (dict_table->referenced_dict_set
          .empty()) {  // this can updated by open - no it can't
    return true;
  }

  /* if instant renamed column referenced by other table */
  List_iterator_fast<Create_field> cf_it(
      ha_alter_info->alter_info->create_list);
  for (Field **fp = old_table->field; *fp; fp++) {
    /* skip columns which not renamed */
    if (!(*fp)->is_flag_set(FIELD_IS_RENAMED)) {
      continue;
    }

    const char *col_name = (*fp)->field_name;
    for (const auto &foreign : dict_table->referenced_dict_set) {
      for (uint32_t i = 0; i < foreign->col_nums; i++) {
        const char *name = foreign->referenced_col_names[i];
        if (!my_strcasecmp(system_charset_info, name, col_name)) {
          if (report_error) {
            my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
                     "ALGORITHM=INSTANT",
                     my_get_err_msg(
                         ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FK_RENAME),
                     "ALGORITHM=INPLACE");
          }
          return false;
        }
      } /* each column in referenced entry */
    }   /* each referenced set */
  }     /* each column being renamed */

  return true;
}

/** Check whether the foreign key options is legit
 @return true if it is */
[[nodiscard]] static bool CheckFkColumnOption(
    const cde_dict_foreign_t *foreign) /*!< in: foreign key */
{
  if (!foreign->foreign_index) {
    return true;
  }

  if (foreign->fk_rule & (CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL |
                          CDE_DDL_FK_RULE_ON_DELETE_SET_NULL)) {
    cde_dict_t *table = foreign->foreign_table;
    cde_dict_index_t *index = foreign->foreign_index;
    for (uint32_t j = 0; j < foreign->col_nums; j++) {
      if (table->attrFull->attrs[index->attr_cols[j]]->attnotnull)
        /* It is not sensible to define SET NULL if the column is not allowed to
         * be NULL! */
        return false;
    }
  }

  return true;
}

/** Set foreign key options
 @return true if successfully set */
[[nodiscard]] static bool CdeSetForeignRule(
    cde_dict_foreign_t *foreign,    /*!< in:DStore Foreign key */
    const Foreign_key_spec *fk_key) /*!< in: Foreign key info from MySQL */
{
  switch (fk_key->delete_opt) {
    case FK_OPTION_NO_ACTION:
    case FK_OPTION_RESTRICT:
    case FK_OPTION_DEFAULT:
      foreign->fk_rule = CDE_DDL_FK_RULE_ON_DELETE_NO_ACTION;
      break;
    case FK_OPTION_CASCADE:
      foreign->fk_rule = CDE_DDL_FK_RULE_ON_DELETE_CASCADE;
      break;
    case FK_OPTION_SET_NULL:
      foreign->fk_rule = CDE_DDL_FK_RULE_ON_DELETE_SET_NULL;
      break;
    case FK_OPTION_UNDEF:
      break;
  }

  switch (fk_key->update_opt) {
    case FK_OPTION_NO_ACTION:
    case FK_OPTION_RESTRICT:
    case FK_OPTION_DEFAULT:
      foreign->fk_rule |= CDE_DDL_FK_RULE_ON_UPDATE_NO_ACTION;
      break;
    case FK_OPTION_CASCADE:
      foreign->fk_rule |= CDE_DDL_FK_RULE_ON_UPDATE_CASCADE;
      break;
    case FK_OPTION_SET_NULL:
      foreign->fk_rule |= CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL;
      break;
    case FK_OPTION_UNDEF:
      break;
  }

  return CheckFkColumnOption(foreign);
}

/** Check whether the foreign key constraint is on base of any stored columns.
@param[in]      foreign         Foreign key constraint information
@param[in]      table           Mysql table
@param[in]      dictTable       DStore table to which the foreign key objects to
be added
@return true if yes, otherwise false. */
static bool CheckFkBaseCol(const cde_dict_foreign_t *foreign,
                           const TABLE *alteredTable,
                           const cde_dict_t *dictTable [[maybe_unused]]) {
  uint8_t rule = foreign->fk_rule;

  rule &= ~(CDE_DDL_FK_RULE_ON_DELETE_NO_ACTION |
            CDE_DDL_FK_RULE_ON_UPDATE_NO_ACTION);

  if (rule == 0) {
    return false;
  }

  for (uint32_t i = 0; i < foreign->col_nums; i++) {
    if (IsBaseStoredColumn(alteredTable, foreign->foreign_col_names[i])) {
      return true;
    }
  }

  return false;
}

/** Initialize the cde_dict_foreign_t structure with supplied info
 @return true if added, false if duplicate foreign->unique_name */
static bool CdeInitForeign(
    cde_dict_foreign_t *foreign,          /*!< in/out: structure to
                                      initialize */
    const char *constraint_name,          /*!< in/out: constraint name if
                                          exists */
    cde_dict_t *table,                    /*!< in: foreign table */
    cde_dict_index_t *index,              /*!< in: foreign key index */
    const char **column_names,            /*!< in: foreign key column
                                          names */
    ulint num_field,                      /*!< in: number of columns */
    const char *referenced_table_name,    /*!< in: referenced table
                                          name */
    cde_dict_t *referenced_table,         /*!< in: referenced table */
    cde_dict_index_t *referenced_index,   /*!< in: referenced index */
    const char **referenced_column_names, /*!< in: referenced column
                                          names */
    ulint referenced_num_field)           /*!< in: number of referenced
                                          columns */
{
  if (constraint_name) {
    char buf[FN_REFLEN + 1];
    char norm_name[FN_REFLEN * 2];
    uint32_t dbNameSize = 0;
    const char *dbNamePtr = GetDBName(table->name.c_str(), dbNameSize);
    if (nullptr == dbNamePtr || 0 == dbNameSize) {
      CDE_LOG_ERROR("GetDBName failed when creating unique_name!");
      return false;
    }

    if (0 != strncpy_s(buf, sizeof(buf), dbNamePtr, dbNameSize)) {
      CDE_LOG_ERROR("strncpy_s failed during copying db name.");
      return false;
    }

    snprintf_s(norm_name, sizeof(norm_name), sizeof(norm_name) - 1, "%s/%s",
               buf, constraint_name);
    foreign->unique_name = safe_strdup_root(&foreign->m_mem_root, norm_name);
    /* Check if any existing foreign key has the same constraint,
    this is needed only if user supplies the constraint name */

    if (table->foreign_dict_set.find(foreign) !=
        table->foreign_dict_set.end()) {
      return false;
    }
  }

  foreign->foreign_table = table;
  foreign->foreign_table_name =
      safe_strdup_root(&foreign->m_mem_root, table->name.c_str());

  foreign->foreign_index = index;
  foreign->col_nums = (unsigned int)num_field;

  foreign->foreign_col_names =
      new (&foreign->m_mem_root) const char *[foreign->col_nums];

  for (ulint i = 0; i < foreign->col_nums; i++) {
    foreign->foreign_col_names[i] =
        safe_strdup_root(&foreign->m_mem_root, column_names[i]);
  }

  foreign->referenced_index = referenced_index;
  foreign->referenced_table = referenced_table;

  foreign->referenced_table_name =
      safe_strdup_root(&foreign->m_mem_root, referenced_table_name);

  foreign->referenced_col_names =
      new (&foreign->m_mem_root) const char *[referenced_num_field];
  for (ulint i = 0; i < foreign->col_nums; i++) {
    foreign->referenced_col_names[i] =
        safe_strdup_root(&foreign->m_mem_root, referenced_column_names[i]);
  }

  return true;
}

/** Check an index to see whether its first fields are the columns in the array,
 in the same order and is not marked for deletion and is not the same
 as types_idx.
 @param[in] table table to find index
 @param[in] indexColNames column names if we rename column in alter table
                          statement, or NULL to use table->col_names
 @param[in] fkColName array of foreign key related index column names
 @param[in] nCols number of columns
 @param[in] index index to check
 @param[in] typesIdx temporary not used in our engine
 @param[in] check_charsets whether to check charsets.
 @param[in] check_null true if none of the columns must be declared NOT NULL
 @return true if the index qualifies, otherwise false */
static bool CdeDictForeignQualifyIndex(
    const cde_dict_t *table, const char **indexColNames, const char **fkColName,
    uint32_t nCols, const cde_dict_index_t *index,
    const cde_dict_index_t *typesIdx [[maybe_unused]],
    bool check_charsets [[maybe_unused]], bool check_null) {
  if (index->index_col_num < nCols) {
    return false;
  }

  for (uint32_t i = 0; i < nCols; i++) {
    cde_dict_field_t *field;
    const char *colName;
    uint32_t colNo;
    uint32_t attrNo;

    field = index->fields + i;
    colNo = index->index_cols[i];
    attrNo = index->attr_cols[i];
    if (field->prefix_len != 0) {
      /** We do not accept column prefix indexes here */
      return false;
    }

    // check_null allow field null and need check field's nullable
    if (check_null && table->attrFull->attrs[attrNo]->attnotnull) {
      return false;
    }

    colName = indexColNames ? indexColNames[colNo] : field->m_name;

    /** Ensure the ith column in index has the same name with ith foreign key
    related index column name */
    if (CdeStrcasecmp(fkColName[i], colName) != 0) {
      return false;
    }

    // Todo: check charset cmp_cols_are_equal, something like
    // CdeCheckFkColumnCompat
  }

  return true;
}

/** Find an index whose first fields are the columns in the array
 in the same order and is not marked for deletion
 @return matching index, NULL if not found */
[[nodiscard]] static cde_dict_index_t *CdeFindFkIndex(
    cde_dict_t *table, /*!< in: table */
    const char **colNames,
    /*!< in: column names, or NULL
    to use table->col_names */
    cde_dict_index_t **drop_index, /*!< in: indexes to be dropped */
    ulint n_drop_index,            /*!< in: size of drop_index[] */
    const char **fkColumns,        /*!< in: array of column names */
    ulint nCols)                   /*!< in: number of columns */
{
  bool find = false;
  for (auto index : table->index_dict_vec) {
    if (CdeDictForeignQualifyIndex(table, colNames, fkColumns, nCols, index,
                                   nullptr, true, false)) {
      for (ulint i = 0; i < n_drop_index; i++) {
        if (index == drop_index[i]) {
          /* Skip to-be-dropped indexes. */
          find = true;
          break;
        }
      }

      if (!find) return index;
      find = false;
    }
  }

  return nullptr;
}

/**
Get instant alter table operations from alter_info, note that for instant
add/drop get anyone is enough, since add/drop has the same process.

@param[in]      alter_inplace_flags

@return DSTORE_INSTANT_OP accordingly
*/
static inline DSTORE_INSTANT_OP CdeGetHandlerInstantOps(
    Alter_inplace_info::HA_ALTER_FLAGS alter_inplace_flags) {
  DSTORE_INSTANT_OP op = DSTORE_INSTANT_OP::NONE;
  if (!(alter_inplace_flags & ~Alter_inplace_info::ALTER_COLUMN_NAME)) {
    op = DSTORE_INSTANT_OP::RENAME_COLUMN_ONLY;
  } else if (!(alter_inplace_flags &
               ~Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH)) {
    op = DSTORE_INSTANT_OP::VARCHAR_RESIZE_ONLY;
  } else if (!(alter_inplace_flags &
               ~(Alter_inplace_info::ALTER_COLUMN_NAME |
                 Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH))) {
    op = DSTORE_INSTANT_OP::RENAME_COLUMN_WITH_VARCHAR_RESIZE;
  } else if (!(alter_inplace_flags &
               ~(Alter_inplace_info::ADD_VIRTUAL_COLUMN |
                 Alter_inplace_info::DROP_VIRTUAL_COLUMN))) {
    op = DSTORE_INSTANT_OP::VIRTUAL_ADD_DROP_ONLY;
  } else if (!(alter_inplace_flags &
               ~(Alter_inplace_info::ADD_VIRTUAL_COLUMN |
                 Alter_inplace_info::DROP_VIRTUAL_COLUMN |
                 Alter_inplace_info::ALTER_COLUMN_NAME))) {
    op = DSTORE_INSTANT_OP::VIRTUAL_ADD_DROP_WITH_RENAME;
  } else if (alter_inplace_flags & Alter_inplace_info::ADD_STORED_BASE_COLUMN &&
             !(alter_inplace_flags & Alter_inplace_info::DROP_VIRTUAL_COLUMN)) {
    op = DSTORE_INSTANT_OP::INSTANT_ADD;
  } else if (alter_inplace_flags & Alter_inplace_info::DROP_STORED_COLUMN) {
    op = DSTORE_INSTANT_OP::INSTANT_DROP;
  }
  return op;
}

static uint CdeInstantTypeToUint(dstore_instant_type type) {
  return static_cast<uint>(type);
}

/**
Determine if this is an instant ALTER TABLE.

@param[in]      ha_alter_info

@return true means it's an instant ALTER TABLE and false not.
*/
static bool CdeIsInstantAlter(const Alter_inplace_info *ha_alter_info) {
  return (ha_alter_info->handler_trivial_ctx !=
          CdeInstantTypeToUint(dstore_instant_type::INSTANT_IMPOSSIBLE));
}

/** Check if virtual column in old and new table are in order, excluding
those dropped column. This is needed because when we drop a virtual column,
ALTER_VIRTUAL_COLUMN_ORDER is also turned on, so we can't decide if this
is a real ORDER change or just DROP COLUMN
@param[in]      table           old TABLE
@param[in]      altered_table   new TABLE
@param[in]      ha_alter_info   Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@return true is all columns in order, false otherwise. */
static bool CheckVirtualColInOrder(const TABLE *table,
                                   const TABLE *altered_table,
                                   const Alter_inplace_info *ha_alter_info) {
  ulint j = 0;

  /* We don't support any adding new virtual column before
  existed virtual column. */
  if (ha_alter_info->handler_flags & Alter_inplace_info::ADD_VIRTUAL_COLUMN) {
    bool has_new = false;

    List_iterator_fast<Create_field> cf_it(
        ha_alter_info->alter_info->create_list);

    cf_it.rewind();

    while (const Create_field *new_field = cf_it++) {
      if (!new_field->is_virtual_gcol()) {
        /* We do not support add virtual col
        before autoinc column */
        if (has_new && (new_field->flags & AUTO_INCREMENT_FLAG)) {
          return (false);
        }
        continue;
      }

      /* Found a new added virtual column. */
      if (!new_field->field) {
        has_new = true;
        continue;
      }

      /* If there's any old virtual column
      after the new added virtual column,
      order must be changed. */
      if (has_new) {
        return (false);
      }
    }
  }

  /* directly return true if ALTER_VIRTUAL_COLUMN_ORDER is not on */
  if (!(ha_alter_info->handler_flags &
        Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER)) {
    return (true);
  }

  for (ulint i = 0; i < table->s->fields; i++) {
    Field *field = table->s->field[i];
    bool dropped = false;

    if (field->stored_in_db) {
      continue;
    }

    /* Check if this column is in drop list */
    for (const Alter_drop *drop : ha_alter_info->alter_info->drop_list) {
      if (drop->type == Alter_drop::COLUMN &&
          my_strcasecmp(system_charset_info, field->field_name, drop->name) ==
              0) {
        dropped = true;
        break;
      }
    }

    if (dropped) {
      continue;
    }

    /* Now check if the next virtual column in altered table
    matches this column */
    while (j < altered_table->s->fields) {
      Field *new_field = altered_table->s->field[j];

      if (new_field->stored_in_db) {
        j++;
        continue;
      }

      if (my_strcasecmp(system_charset_info, field->field_name,
                        new_field->field_name) != 0) {
        /* different column */
        return (false);
      } else {
        j++;
        break;
      }
    }

    if (j > altered_table->s->fields) {
      /* there should not be less column in new table
      without them being in drop list */
      return false;
    }
  }

  return (true);
}

/**
Find the index of `alteredTable->key_info[]` whose KEY name matches `name`.

@param[in] alteredTable  TABLE object after ALTER.
@param[in] name           KEY name to look up.
@return key_info index if found; otherwise `UINT32_MAX`.
*/
[[nodiscard]] static uint32_t CdeFindAlterKeyIndexByName(
    const TABLE *alteredTable, const char *keyName) {
  if (keyName == nullptr) {
    return UINT32_MAX;
  }
  for (uint32_t k = 0; k < alteredTable->s->keys; ++k) {
    const char *kn = alteredTable->key_info[k].name;
    if (kn != nullptr && !my_strcasecmp(system_charset_info, kn, keyName)) {
      return k;
    }
  }
  return UINT32_MAX;
}

/**
Find if any `IS_EQUAL_PACK_LENGTH` varchar resize column is used by
an index in `oldTable`.

@param[in] resized_varchar_fields  List of old Field pointers that were
                                    compatible-resized by the ALTER.
@param[in] oldTable               TABLE object before ALTER.
@return true if at least one resized varchar field belongs to any
        index part in oldTable; false otherwise.
*/
[[nodiscard]] static bool CdeLegacyIndexedVarcharResize(
    const std::vector<const Field *> &resizedVarcharFields,
    const TABLE *oldTable) {
  for (uint32_t i = 0; i < oldTable->s->keys; ++i) {
    const KEY &key = oldTable->key_info[i];
    for (uint32_t j = 0; j < key.user_defined_key_parts; ++j) {
      const Field *keyField = key.key_part[j].field;
      for (const Field *resizedField : resizedVarcharFields) {
        if (keyField == resizedField) {
          return true;
        }
      }
    }
  }
  return false;
}

/**
Check whether should inplace rebuild table when indexed varchar resize.

If match the following rules, then this ALTER must rebuild:
1) The ddl query use the default algorithm.
2) If a key part referencing an `IS_EQUAL_PACK_LENGTH` VARCHAR feild's
   `key_part.length` increases, and
3) The index's new `KEY::key_length` exceeds Dstore's max key length threshold
  (currently 910 bytes),

@param[in] haAlterInfo  DDL operation.
@param[in] oldTable      Table before ALTER.
@param[in] alteredTable  Table after ALTER
@return true if the ALTER must be forced to rebuild for policy reasons;
        false otherwise.
*/
[[nodiscard]] static bool CdeNeedRebuildForIndexedVarcharResize(
    const Alter_inplace_info *haAlterInfo, const TABLE *oldTable,
    const TABLE *alteredTable) {
  CDE_ASSERT(oldTable != nullptr);
  if (alteredTable == nullptr ||
      haAlterInfo->alter_info->requested_algorithm ==
          Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
    return false;
  }
  constexpr uint32_t KEY_MAX_LENGTH_FOR_INSTANT = 910;
  if (!(haAlterInfo->handler_flags &
        Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH)) {
    return false;
  }

  std::vector<const Field *> resizedVarcharFields;
  List_iterator_fast<Create_field> createFieldIt(
      haAlterInfo->alter_info->create_list);
  while (const Create_field *newField = createFieldIt++) {
    const Field *oldField = newField->field;
    if (oldField == nullptr) {
      continue;
    }

    if (oldField->is_equal(newField) != IS_EQUAL_PACK_LENGTH) {
      continue;
    }

    if (oldField->type() != MYSQL_TYPE_VARCHAR) {
      continue;
    }

    resizedVarcharFields.push_back(oldField);
  }

  if (resizedVarcharFields.empty()) {
    return false;
  }

  if (oldTable->s->keys != alteredTable->s->keys) {
    return CdeLegacyIndexedVarcharResize(resizedVarcharFields, oldTable);
  }

  for (uint32_t i = 0; i < oldTable->s->keys; ++i) {
    const KEY &oldKey = oldTable->key_info[i];
    const uint32_t newIdx =
        CdeFindAlterKeyIndexByName(alteredTable, oldKey.name);
    if (newIdx == UINT32_MAX) {
      return CdeLegacyIndexedVarcharResize(resizedVarcharFields, oldTable);
    }
    const KEY &newKey = alteredTable->key_info[newIdx];
    if (oldKey.user_defined_key_parts != newKey.user_defined_key_parts) {
      return CdeLegacyIndexedVarcharResize(resizedVarcharFields, oldTable);
    }

    bool resizedPartGrew = false;
    for (uint32_t j = 0; j < oldKey.user_defined_key_parts; ++j) {
      const Field *keyField = oldKey.key_part[j].field;
      bool inResizeSet = false;
      for (const Field *resizedField : resizedVarcharFields) {
        if (keyField == resizedField) {
          inResizeSet = true;
          break;
        }
      }
      if (!inResizeSet) {
        continue;
      }

      /* KEY_PART_INFO is built against table field ordering.
      When the column ordering between old/new tables changes,
      the key_part index j might not refer to the same underlying
      column. In that case, we cannot reliably compare key_part.length
      by position. */
      if (newKey.key_part[j].fieldnr != oldKey.key_part[j].fieldnr) {
        if (newKey.key_length > KEY_MAX_LENGTH_FOR_INSTANT) {
          return true;
        }
        continue;
      }

      if (newKey.key_part[j].length > oldKey.key_part[j].length) {
        resizedPartGrew = true;
        break;
      }
    }

    if (resizedPartGrew && newKey.key_length > KEY_MAX_LENGTH_FOR_INSTANT) {
      return true;
    }
  }

  return false;
}

/**
Determine if ALTER TABLE needs to rebuild the table.

@param[in]      ha_alter_info   The DDL operation
@param[in]      old_table       Table before ALTER
@param[in]      altered_table   Table after ALTER (may be nullptr)
@return false if not need rebuild, true for need rebuild
*/
[[nodiscard]] static bool CdeNeedRebuild(
    const Alter_inplace_info *ha_alter_info, const TABLE *old_table,
    const TABLE *altered_table) {
  if (CdeIsInstantAlter(ha_alter_info)) {
    return (false);
  }

  Alter_inplace_info::HA_ALTER_FLAGS alter_inplace_flags =
      ha_alter_info->handler_flags & ~(DSTORE_INPLACE_IGNORE);

  if (alter_inplace_flags == Alter_inplace_info::CHANGE_CREATE_OPTION &&
      !(ha_alter_info->create_info->used_fields &
        (HA_CREATE_USED_TABLESPACE))) {
    /* Any other CHANGE_CREATE_OPTION than changing TABLESPACE can be done
    without rebuilding the table. */
    return (false);
  }

  if (CdeNeedRebuildForIndexedVarcharResize(ha_alter_info, old_table,
                                            altered_table)) {
    return true;
  }

  return (!!(ha_alter_info->handler_flags & DSTORE_ALTER_REBUILD));
}

/**
Determine if ALTER TABLE ops can be done instantly and return the instant type
supported.

@param[in]      ha_alter_info
@param[in]      dict_table     dict table in CDE
@param[in]      old_table      old TABLE object
@param[in]      altered_table  new TABLE object

@return dstore_instant_type
*/
static dstore_instant_type CdeGetInstantType(
    const Alter_inplace_info *ha_alter_info, const cde_dict_t *dict_table,
    const TABLE *old_table, const TABLE *altered_table) {
  if (!(ha_alter_info->handler_flags & ~DSTORE_INPLACE_IGNORE)) {
    return (dstore_instant_type::INSTANT_NO_CHANGE);
  }
  /*
    ignore flags which not cared about, eg, rename column instant not only
    set ALTER_COLUMN_NAME flag, ALTER_COLUMN_DEFAULT will be set too.
  */
  Alter_inplace_info::HA_ALTER_FLAGS alter_inplace_flags =
      ha_alter_info->handler_flags & ~DSTORE_INPLACE_IGNORE;
  if (alter_inplace_flags & ~DSTORE_INSTANT_ALLOWED) {
    return dstore_instant_type::INSTANT_IMPOSSIBLE;
  }

  DSTORE_INSTANT_OP op = CdeGetHandlerInstantOps(alter_inplace_flags);

  switch (op) {
    case DSTORE_INSTANT_OP::RENAME_COLUMN_ONLY:
    case DSTORE_INSTANT_OP::RENAME_COLUMN_WITH_VARCHAR_RESIZE: {
      bool report_error = (ha_alter_info->alter_info->requested_algorithm ==
                           Alter_info::ALTER_TABLE_ALGORITHM_INSTANT);
      if (CdeAllowToRenameColumn(ha_alter_info, old_table, altered_table,
                                 dict_table, true, report_error)) {
        return (op == DSTORE_INSTANT_OP::RENAME_COLUMN_ONLY)
                   ? dstore_instant_type::INSTANT_RENAME_COLUMN
                   : dstore_instant_type::
                         INSTANT_RENAME_COLUMN_WITH_VARCHAR_RESIZE;
      } else if (op == DSTORE_INSTANT_OP::RENAME_COLUMN_WITH_VARCHAR_RESIZE) {
        /** temporary strict for this condition */
        return dstore_instant_type::INSTANT_IMPOSSIBLE;
      }
    } break;
    case DSTORE_INSTANT_OP::VARCHAR_RESIZE_ONLY:
      return dstore_instant_type::INSTANT_VARCHAR_RESIZE;
    case DSTORE_INSTANT_OP::VIRTUAL_ADD_DROP_ONLY:
      if (CheckVirtualColInOrder(old_table, altered_table, ha_alter_info)) {
        return (dstore_instant_type::INSTANT_VIRTUAL_ONLY);
      }
      break;
    case DSTORE_INSTANT_OP::VIRTUAL_ADD_DROP_WITH_RENAME:
      /* Not supported yet in INPLACE. So not supporting here as well. */
      break;
    case DSTORE_INSTANT_OP::INSTANT_DROP:
      if (!CheckVirtualColInOrder(old_table, altered_table, ha_alter_info)) {
        break;
      }
      [[fallthrough]];
    case DSTORE_INSTANT_OP::INSTANT_ADD:
      if (!dict_table
               ->is_temporary()) {  // Refer innodb support_instant_add_drop,
                                    // but now we only check temp table
        return (dstore_instant_type::INSTANT_ADD_DROP_COLUMN);
      }
      break;
    default:
      break;
  }

  return dstore_instant_type::INSTANT_IMPOSSIBLE;
}

/* Check that the same column does not appear twice in the index.
@param[in] key    the key to be check
@return false if index column appera twice.
*/
static bool CheckIndexColumnUnique(const KEY &key) {
  for (uint32_t i = 0; i < key.user_defined_key_parts; i++) {
    const KEY_PART_INFO &keyPart1 = key.key_part[i];
    const Field *field = keyPart1.field;

    for (uint32_t j = 0; j < i; j++) {
      const KEY_PART_INFO &keyPart2 = key.key_part[j];

      if (keyPart1.fieldnr != keyPart2.fieldnr) {
        continue;
      }

      my_error(ER_WRONG_KEY_COLUMN, MYF(0), field->field_name);
      return false;
    }
  }

  return true;
}

/**
Get the new non-virtual column names if any columns were renamed

@param ha_alter_info    Data used during in-place alter
@param altered_table    MySQL table that is being altered
@param table            MySQL table as it is before the ALTER operation
@param user_table       InnoDB table as it is before the ALTER operation
@param heap             Memory heap for the allocation
@return array of new column names in rebuilt_table, or NULL if not renamed
*/
[[nodiscard]] static const char **CdeGetColNames(
    Alter_inplace_info *haAlterInfo, const TABLE *table,
    const cde_dict_t *userTable) {
  const char **cols;
  cols = static_cast<const char **>(
      CdeZalloc(userTable->m_totalColCount * sizeof *cols));

  List_iterator_fast<Create_field> cfIt(haAlterInfo->alter_info->create_list);
  while (const Create_field *newField = cfIt++) {
    for (uint32_t old_i = 0; table->field[old_i]; old_i++) {
      if (newField->field == table->field[old_i]) {
        cols[old_i] = newField->field_name;
        break;
      }
    }
  }

  return cols;
}

/** Check whether exist first added blob/json type virtual column
@param[in] haAlterInfo          Data used during in-place alter
@param[in] alteredTable         MySQL table that is being altered to
@param[in] oldTable             MySQL table as it is before the ALTER operation
@param[in] oldDictTable         dstore table as it is before the ALTER operation
@retval false Failure
@retval true Success */
static bool CdeCheckAddVirtualColumn(Alter_inplace_info *haAlterInfo,
                                     const TABLE *alteredTable,
                                     const TABLE *oldTable,
                                     const cde_dict_t *oldDictTable) {
  uint32_t i = 0;
  const Create_field *new_field;
  List_iterator_fast<Create_field> cf_it(haAlterInfo->alter_info->create_list);
  while ((new_field = (cf_it++)) != nullptr) {
    const Field *field = new_field->field;
    uint32_t old_i;

    for (old_i = 0; oldTable->field[old_i]; old_i++) {
      const Field *n_field = oldTable->field[old_i];
      if (field == n_field) {
        break;
      }
    }

    i++;

    if (oldTable->field[old_i]) {
      continue;
    }

    field = alteredTable->field[i - 1];
    if (IsVirtualGeneratedField(field)) {
      auto type = field->type();
      /** We do not support this scenario in the short term. */
      if ((is_blob(type) || type == MYSQL_TYPE_JSON) &&
          !oldDictTable->dstore_relation->attr->tdhaslob) {
        return false;
      }
    }
  }

  return true;
}

/** Collect virtual column info for its addition
@param[in] haAlterInfo          Data used during in-place alter
@param[in] alteredTable         MySQL table that is being altered to
@param[in] oldTable             MySQL table as it is before the ALTER operation
@retval true Failure
@retval false Success */
static bool CdePrepareCheckAddVirtualColumn(Alter_inplace_info *haAlterInfo,
                                            const TABLE *alteredTable,
                                            const TABLE *oldTable) {
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);
  uint32_t i = 0;
  const Create_field *new_field;

  List_iterator_fast<Create_field> cf_it(haAlterInfo->alter_info->create_list);

  while ((new_field = (cf_it++)) != nullptr) {
    const Field *field = new_field->field;
    uint32_t old_i;

    for (old_i = 0; oldTable->field[old_i]; old_i++) {
      const Field *n_field = oldTable->field[old_i];
      if (field == n_field) {
        break;
      }
    }

    i++;

    if (oldTable->field[old_i]) {
      continue;
    }

    field = alteredTable->field[i - 1];
    if (!field->gcol_info || field->stored_in_db) {
      my_error(ER_WRONG_KEY_COLUMN, MYF(0), field->field_name);
      return true;
    }

    /** we should pre build new added DictVirtualCol into ctx memory, maybe used
     * in add index */
    DictVirtualCol virtualCol;
    virtualCol.ind = i - 1;
    setupBaseColumns(ctx->old_table, field, &virtualCol);
    ctx->new_addedVirtualCols.emplace(virtualCol.ind, virtualCol);
    ctx->minAddedVirtualInd = std::min(ctx->minAddedVirtualInd, virtualCol.ind);
  }

  return false;
}

/** Collect virtual column info for its addition
@param[in] haAlterInfo             Data used during in-place alter
@param[in] oldTable                MySQL table as it is before the ALTER
operation
@retval true Failure
@retval false Success */
static bool CdePrepareCheckDropVirtualColumn(Alter_inplace_info *haAlterInfo,
                                             const TABLE *oldTable) {
  for (const Alter_drop *drop : haAlterInfo->alter_info->drop_list) {
    const Field *field;
    ulint old_i;

    if (drop->type != Alter_drop::COLUMN) continue;

    for (old_i = 0; oldTable->field[old_i]; old_i++) {
      const Field *n_field = oldTable->field[old_i];
      if (!my_strcasecmp(system_charset_info, n_field->field_name,
                         drop->name)) {
        break;
      }
    }
    /* SQL-layer already has checked that all columns to be dropped exist. */
    field = oldTable->field[old_i];
    if (!field->gcol_info || field->stored_in_db) {
      my_error(ER_WRONG_KEY_COLUMN, MYF(0), field->field_name);
      return true;
    }
  }

  return false;
}

/**
This function checks that index keys are sensible.

@return 0 or error number */
static int CdeCheckIndexKeys(Alter_inplace_info *info, cde_dict_t *cdeTable) {
  for (uint32_t keyNum = 0; keyNum < info->index_add_count; keyNum++) {
    const KEY &key = info->key_info_buffer[info->index_add_buffer[keyNum]];

    /* Check that the same index name does not appear
    twice in indexes to be created. */

    for (uint32_t i = 0; i < keyNum; i++) {
      const KEY &key2 = info->key_info_buffer[info->index_add_buffer[i]];

      if (0 == strcmp(key.name, key2.name)) {
        my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0), key.name);

        return CDE_FAIL;
      }
    }

    /* Check that the same index name does not already exist. */
    const cde_dict_index_t *index = nullptr;
    for (auto dictIndex : cdeTable->index_dict_vec) {
      if (!dictIndex->IsCommitted()) {
        continue;
      }

      if (!strcmp(key.name, dictIndex->name.c_str())) {
        index = dictIndex;
        break;
      }
    }

    if (index == nullptr) {
      continue;
    }

    /* Now we are in a situation where we have "ADD INDEX x"
    and an index by the same name already exists. We have 4
    possible cases:
    1. No further clauses for an index x are given. Should reject
    the operation.
    2. "DROP INDEX x" is given. Should allow the operation.
    3. "RENAME INDEX x TO y" is given. Should allow the operation.
    4. "DROP INDEX x, RENAME INDEX x TO y" is given. Should allow
    the operation, since no name clash occurs. In this particular
    case MySQL cancels the operation without calling InnoDB
    methods. */

    /* If a key by the same name is being created and
    dropped, the name clash is OK. */
    uint32_t i = 0;
    for (; i < info->index_drop_count; i++) {
      const KEY *dropKey = info->index_drop_buffer[i];

      if (0 == strcmp(key.name, dropKey->name)) {
        if (!CheckIndexColumnUnique(key)) {
          return CDE_FAIL;
        }
        break;
      }
    }

    if (i < info->index_drop_count) {
      continue;
    }

    /* If a key by the same name is being created and
    renamed, the name clash is OK. E.g.
    ALTER TABLE t ADD INDEX i (col), RENAME INDEX i TO x
    where the index "i" exists prior to the ALTER command.
    In this case we:
    1. rename the existing index from "i" to "x"
    2. add the new index "i" */
    uint32_t j = 0;
    for (; j < info->index_rename_count; j++) {
      const KEY_PAIR *pair = &info->index_rename_buffer[j];

      if (0 == strcmp(key.name, pair->old_key->name)) {
        if (!CheckIndexColumnUnique(key)) {
          return CDE_FAIL;
        }
        break;
      }
    }

    if (j < info->index_rename_count) {
      continue;
    }

    my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0), key.name);

    return CDE_FAIL;
  }

  return CDE_SUCC;
}

static cde_dict_index_t *CdeInplacePrepareCreateIndex(cde_dict_t *table,
                                                      cde_index_def *indexDef) {
  Oid indexOid =
      SystableInterface::GetNewObjectId(DSTORE::g_defaultPdbId, false, false);

  bool isTempTable = table->is_temporary();
  DSTORE::TablespaceId tableSpaceId =
      isTempTable ? CDE_TMP_TABLE_SPACE_ID : table->m_spaceId;

  DEBUG_SYNC_C("WaitInterruptBeforeAllocSeg");
  /* The index_rel and heap_rel use the same tablespace. */
  PageId segmentId = CdeAllocSegmentWrapper::Alloc(
      ChooseIndexSegmentType(isTempTable), tableSpaceId);

  if (segmentId == DSTORE::INVALID_PAGE_ID) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Alloc index segment failed.");
    return nullptr;
  }

  // get index column vector, cde_dict_index_t->index_cols need, from 0
  uint32_t *indexCols =
      static_cast<uint32_t *>(CdeZalloc(sizeof(uint32_t) * indexDef->n_fields));
  CDE_ASSERT(indexCols != nullptr);
  uint32_t *attrCols =
      static_cast<uint32_t *>(CdeZalloc(sizeof(uint32_t) * indexDef->n_fields));
  CDE_ASSERT(attrCols != nullptr);

  uint64_t *statNDiffKeyVals =
      static_cast<uint64_t *>(CdeZalloc(sizeof(uint64_t) * indexDef->n_fields));
  CDE_ASSERT(statNDiffKeyVals != nullptr);

  uint64_t *statNSampleSize =
      static_cast<uint64_t *>(CdeZalloc(sizeof(uint64_t) * indexDef->n_fields));
  CDE_ASSERT(statNSampleSize != nullptr);

  cde_dict_index_t *dictIndex = new (std::nothrow) cde_dict_index_t();
  CDE_ASSERT(dictIndex != nullptr);

  ScanKey keyInfos;
  dictIndex->fillfactor = indexDef->fillfactor;

  CdeCreateTableInfo tableInfo(tableSpaceId);
  StorageRelation newIndexRel = tableInfo.CreateIndexStorRel(
      table->dstore_relation, table->attrFull, indexOid, segmentId, indexDef,
      table->m_dictCols, indexCols, attrCols, &keyInfos);

  if (newIndexRel == nullptr) {
    CdeFree(indexCols);
    CdeFree(attrCols);
    CdeFree(statNDiffKeyVals);
    CdeFree(statNSampleSize);
    delete dictIndex;
    CDE_LOG_ERROR("CdeInplacePrepareCreateIndex fail! key name = %s",
                  indexDef->name);
    return nullptr;
  }

  dictIndex->name = indexDef->name;
  dictIndex->key_length = indexDef->key_length;
  dictIndex->oid = newIndexRel->relOid;
  dictIndex->rel = newIndexRel;
  dictIndex->scan_key = keyInfos;
  dictIndex->index_cols = indexCols;
  dictIndex->attr_cols = attrCols;
  dictIndex->stat_n_diff_key_vals = statNDiffKeyVals;
  dictIndex->stat_n_sample_size = statNSampleSize;
  dictIndex->index_col_num = indexDef->n_fields;
  dictIndex->index_csn = TransactionInterface::GetTransactionSnapshotCsn();
  dictIndex->table = table;
  dictIndex->SetCommitted(false);
  CdeDictStatsResetIndex(dictIndex);

  return dictIndex;
}

static bool GenerateRebuildFieldExtraInfo(const TABLE *oldTable,
                                          const TABLE *alteredTable,
                                          Alter_inplace_info *haAlterInfo,
                                          ha_cde_inplace_ctx *ctx) {
  ctx->colMap.resize(ctx->old_table->dstore_relation->attr->natts, ~0U);
  ctx->addedFieldsInfos.resize(ctx->new_table->dstore_relation->attr->natts);

  bool isBlobAlloc = false;
  uint32_t j = 0;
  uint32_t newAddColNum = 0;
  List_iterator_fast<Create_field> cf_it(haAlterInfo->alter_info->create_list);
  while (const Create_field *new_field = cf_it++) {
    if (new_field->is_virtual_gcol()) {
      ++j;
      continue;
    }

    Field *field = alteredTable->field[j];
    if (!new_field->field) {
      AddedFieldInfo &addColumnFieldInfo =
          ctx->addedFieldsInfos[newAddColNum++];
      addColumnFieldInfo.m_defaultValueWithVarlen.data = nullptr;
      if (field->is_real_null()) {
        addColumnFieldInfo.m_isDefaultNull = true;
      } else {
        uint32_t offset =
            static_cast<uint32_t>(field->offset(alteredTable->record[0]));
        uint32_t attrPos = ctx->new_table->m_dictCols[j].m_phyPos;
        Oid type_oid =
            ctx->new_table->dstore_relation->attr->attrs[attrPos]->atttypid;
        int ret = StoreMysqlFieldToDstoreFormat(
            alteredTable->record[0] + offset, type_oid, field, false,
            addColumnFieldInfo.m_defaultValueWithoutrVarlen,
            addColumnFieldInfo.m_defaultValueWithVarlen, &isBlobAlloc);
        if (ret != CDE_OK) {
          CDE_LOG_ERROR(
              "GenerateRebuildFieldExtraInfo get field:%s default value failed "
              "for table:%s",
              field->field_name, ctx->new_table->name.c_str());
          return true;
        }
        addColumnFieldInfo.m_typeOid = CdeConvertDatatypeOid(type_oid);
      }
      addColumnFieldInfo.m_Pos = ctx->new_table->m_dictCols[j].m_phyPos;
      ++j;
      continue;
    }

    for (uint32_t old_i = 0; oldTable->field[old_i]; old_i++) {
      const Field *oldField = oldTable->field[old_i];
      if (oldField == new_field->field) {
        uint32_t oldPhyPos = ctx->old_table->m_dictCols[old_i].m_phyPos;
        ctx->colMap[oldPhyPos] = ctx->new_table->m_dictCols[j].m_phyPos;
        if (!field->is_nullable()) {
          ctx->colNotNull.push_back(oldPhyPos);
        }
        break;
      }
    }

    ++j;
  }

  ctx->addedFieldsInfos.resize(newAddColNum);
  return false;
}

/** Update internal structures with concurrent writes blocked,
while preparing ALTER TABLE.

@param ha_alter_info Data used during in-place alter
@param altered_table MySQL table that is being altered
@param old_table MySQL table as it is before the ALTER operation
@param old_dd_tab old dd table
@param new_dd_tab new dd table
@param table_name Table name in MySQL

@retval true Failure
@retval false Success
*/
static bool CdePrepareInplaceAlterTableDict(Alter_inplace_info *haAlterInfo,
                                            const TABLE *alteredTable,
                                            const TABLE *oldTable,
                                            dd::Table *newDdTable,
                                            const char *tableName, THD *thd) {
  (void)tableName;
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);

  CDE_ASSERT_DEBUG(!ctx->add_index);
  CDE_ASSERT_DEBUG(!ctx->num_to_add_index);
  CDE_ASSERT_DEBUG(!ctx->num_to_drop_index == !ctx->drop_index);
  CDE_ASSERT_DEBUG(!ctx->num_to_drop_fk == !ctx->drop_fk);

  if (haAlterInfo->handler_flags & Alter_inplace_info::DROP_VIRTUAL_COLUMN) {
    if (CdePrepareCheckDropVirtualColumn(haAlterInfo, oldTable)) {
      return true;
    }
  }

  if (haAlterInfo->handler_flags & Alter_inplace_info::ADD_VIRTUAL_COLUMN) {
    if (CdePrepareCheckAddVirtualColumn(haAlterInfo, alteredTable, oldTable)) {
      return true;
    }
  }

  cde_dict_t *userTable = ctx->new_table;

  cde_session_t *session = CdeCreateOrGetSession(thd);

  ctx->needRebuild = CdeNeedRebuild(haAlterInfo, oldTable, alteredTable);
  if (ctx->needRebuild) {
    {
      DictSysLockGuard dictSysLockGuard;
      ctx->new_table->OnlineRetryDropDictIndexes();
    }
    HA_CREATE_INFO sqlCreateInfo;
    sqlCreateInfo.tablespace = haAlterInfo->create_info->tablespace;
    std::string newTableName = ctx->old_table->name;
    CdeCreateTableInfo createInfo(thd, newTableName.c_str(),
                                  const_cast<TABLE *>(alteredTable),
                                  &sqlCreateInfo, newDdTable);
    ctx->new_table = createInfo.CdeCreateInplaceRebuildTable(
        thd, newTableName.c_str(), alteredTable, newDdTable);
    if (!ctx->new_table) {
      auto err = GetAndConvertDstoreErrcodeToMysql();
      if (err == (int)HaDstoreErrE::HA_DSTORE_ERR_TABLESPACE_FULL) {
        my_error(ER_DSTORE_ERR_TABLESPACE_REACH_MAXSIZE, MYF(0));
        return true;
      }

      my_error(ER_DSTORE_INPLACE_GENERIC_ERR, MYF(0), "when rebuild table",
               GetDstoreErrcode());
      return true;
    }

    CDE_ASSERT(ctx->new_table);
    CDE_ASSERT(ctx->old_table != ctx->new_table);

    return GenerateRebuildFieldExtraInfo(oldTable, alteredTable, haAlterInfo,
                                         ctx);
  }

  ctx->num_to_add_index = haAlterInfo->index_add_count;
  std::unique_ptr<cde_index_def[]> indexDefs =
      CdeCreateIndexDefs(haAlterInfo, oldTable, alteredTable, newDdTable,
                         ctx->num_to_add_index, thd);

  ctx->add_index = static_cast<cde_dict_index_t **>(
      CdeZalloc(ctx->num_to_add_index * sizeof *ctx->add_index));
  ctx->add_key_numbers = static_cast<uint32_t *>(
      CdeZalloc(ctx->num_to_add_index * sizeof *ctx->add_key_numbers));

  if (!haAlterInfo->online) {
    int error = CdeLockTable(LOCK_S, userTable->dstore_relation);
    if (error) {
      CDE_LOG_ERROR(
          "Lock table fail when prepare inplace alter table,"
          "table name: %s",
          userTable->name.c_str());
      auto err = GetAndConvertDstoreErrcodeToMysql();
      ReportInplaceAlterError(err);
      return CDE_FAIL;
    }
  }

  userTable->WaitIfHoldByBgThread();

  DictSysLockGuard dictSysLockGuard;
  ctx->new_table->OnlineRetryDropDictIndexes();

  if (ctx->num_to_add_index > 0) {
    /* Create the indexes and load into dictionary. */
    cde_dict_t *indexTable = userTable;
    if (ctx->needRebuild) {
      indexTable = ctx->new_table;
    }
    for (uint32_t a = 0; a < ctx->num_to_add_index; a++) {
      ctx->add_index[a] =
          CdeInplacePrepareCreateIndex(indexTable, &indexDefs[a]);

      if (!ctx->add_index[a]) {
        CDE_LOG_ERROR("Inplace prepare create index fail, index name: %s",
                      indexDefs[a].name);
        auto err = GetAndConvertDstoreErrcodeToMysql();
        DBUG_EXECUTE_IF("dstore_OOM_prepare_inplace_alter",
                        err = HA_ERR_OUT_OF_MEM;);
        ReportInplaceAlterError(err, thd);

        userTable->UnMarkBgStatQuit();
        return CDE_FAIL;
      }
      ctx->add_index[a]->SetOnlineStatus(
          OnlineIndexStatus::ONLINE_INDEX_CREATION);
      ctx->add_key_numbers[a] = indexDefs[a].m_keyNumber;
      const KEY *const key_info = haAlterInfo->key_info_buffer;
      const KEY *key = &key_info[ctx->add_key_numbers[a]];
      CdeCreateTableInfo::CreateFieldsForIndex(ctx->add_index[a], key,
                                               alteredTable, true);
      indexTable->add_index_dict(ctx->add_index[a]);
    }
  }

  if (haAlterInfo->online) {
    OnlineDdlStatus status = OnlineDdlStatus::INVALID;
    if (haAlterInfo->handler_flags & DSTORE_ONLINE_CREATE) {
      status = OnlineDdlStatus::CREATING_INDEX;
    } else if (haAlterInfo->handler_flags & DSTORE_ALTER_REBUILD) {
      /*Not support online rebuild table yet. */
      CDE_ASSERT(false);
      status = OnlineDdlStatus::REBUILDING_TABLE;
    }

    /* Construct online ddl manager. */
    auto thrd = (DSTORE::ThreadContext *)session->dstore_thrd;
    if (ctx->old_table->CreateOnlineDdlMgr(thd, oldTable, alteredTable,
                                           newDdTable, ctx, status,
                                           thrd) == CDE_FAIL) {
      CDE_LOG_ERROR("Create online ddl manager for table:%s fail.",
                    ctx->old_table->name.c_str());
      ReportInplaceAlterError(HA_ERR_OUT_OF_MEM);
      userTable->UnMarkBgStatQuit();
      return CDE_FAIL;
    }

    /* Set snapshot for IndexInterface::BuildParallel or IndexInterface::Build.
     */
    RetStatus ret = TransactionInterface::SetSnapShot();
    CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  }

  userTable->UnMarkBgStatQuit();
  return CDE_SUCC;
}

cde_dict_index_t *CdeForeignKeyFindIndex(cde_dict_t *table,
                                         const char **colNames,
                                         const char **fkColumns, uint32_t nCols,
                                         const cde_dict_index_t *typesIdx,
                                         bool check_charsets, bool check_null) {
  for (cde_dict_index_t *index : table->index_dict_vec) {
    if (typesIdx == index || index->toBeDropped) {
      continue;
    }

    if ((!index->IsCommitted()) &&
        ((index->GetOnlineStatus() ==
          OnlineIndexStatus::ONLINE_INDEX_ABORTED_DROPPED) ||
         (index->GetOnlineStatus() ==
          OnlineIndexStatus::ONLINE_INDEX_ABORTED))) {
      continue;
    }

    if (CdeDictForeignQualifyIndex(table, colNames, fkColumns, nCols, index,
                                   typesIdx, check_charsets, check_null)) {
      return index;
    }
  }

  return nullptr;
}

/** Check if the columns of an index being created that a foreign key constraint
 will used is matched.
 @param[in] key the candidate equivalent index
 @param[in] fkColNames foreign key related column name
 @param[in] nCols number of fk columns
 @return true if mateched else other.
*/
static bool CheckEquivIndexColumIsMatched(const KEY *key,
                                          const char *const *fkColNames,
                                          uint16_t nCols) {
  for (uint32_t j = 0; j < nCols; j++) {
    const KEY_PART_INFO &key_part = key->key_part[j];
    uint32_t col_len = key_part.field->pack_length();

    /* Any index on virtual columns cannot be used
    for reference constaint */
    if (IsVirtualGeneratedField(key_part.field)) {
      return false;
    }

    /* The MySQL pack length contains 1 or 2 bytes
    length field for a true VARCHAR. */
    if (key_part.field->type() == MYSQL_TYPE_VARCHAR) {
      col_len -= key_part.field->get_length_bytes();
    }

    if (key_part.length < col_len) {
      /* Column prefix indexes cannot be
      used for FOREIGN KEY constraints. */
      return false;
    }

    if (CdeStrcasecmp(fkColNames[j], key_part.field->field_name) != 0) {
      /* Name mismatch */
      return false;
    }
  }

  return true;
}

/** Check if a foreign key constraint can make use of an index
 that is being created.
 @param[in] fk_col_names foreign key related column name
 @param[in] n_cols number of fk columns
 @param[in] keys array of KEYs for a table - including KEYs to be added.
 @param[in] add indexes being created
 @param[in] n_add number of indexes to create
 @return usable index, or NULL if none found
*/
static const KEY *CdeFindEquivIndex(
    const char *const *fk_col_names, uint32_t n_cols,
    const KEY *keys, /*!< in: index information */
    const uint *add, /*!< in: indexes being created */
    uint n_add)      /*!< in: number of indexes to create */
{
  for (uint32_t i = 0; i < n_add; i++) {
    const KEY *key = &keys[add[i]];

    if (key->user_defined_key_parts < n_cols) {
      continue;
    }

    if (CheckEquivIndexColumIsMatched(key, fk_col_names, n_cols)) {
      return key;
    }
  }

  return nullptr;
}

/** Determines if DStore is dropping a foreign key constraint.
@param foreign the constraint
@param drop_fk constraints being dropped
@param n_drop_fk number of constraints that are being dropped
@return whether the constraint is being dropped */
[[nodiscard]] inline bool CdeDroppingForeign(const cde_dict_foreign_t *foreign,
                                             cde_dict_foreign_t **drop_fk,
                                             ulint n_drop_fk) {
  while (n_drop_fk--) {
    if (*drop_fk++ == foreign) {
      return (true);
    }
  }

  return (false);
}

/* Check whether an index is needed for the foreign key constraint.
If so, if it is dropped, is there an equivalent index can play its role.

@param haAlterInfo Data used during in-place alter
@param colNames column names if we rename column in alter table statement,
                or NULL to use table->col_names
@param index index to be dropped
@param indexedTable table of index

@retval true if index is needed and can't be dropped
@retval false can drop the index
*/
static bool CdeCheckForeignKeyIndex(
    Alter_inplace_info *haAlterInfo, const char **colNames,
    cde_dict_index_t *droppingIndex, cde_dict_t *indexedTable,
    cde_dict_foreign_t **drop_fk, /*!< in: Foreign key constraints to drop */
    ulint n_drop_fk)              /*!< in: Number of foreign keysto drop */
{
  cde_trxinfo_t *trxInfo = CdeGetTrxinfo(current_thd);
  const cde_dict_foreign_set *fks = &indexedTable->referenced_dict_set;

  /* Check for all FK references from other tables to the index. */
  for (cde_dict_foreign_set::iterator it = fks->begin(); it != fks->end();
       it++) {
    cde_dict_foreign_t *foreign = *it;
    if (foreign->referenced_index != droppingIndex) {
      continue;
    }

    CDE_ASSERT(indexedTable == foreign->referenced_table);
    if (nullptr == CdeForeignKeyFindIndex(indexedTable, colNames,
                                          foreign->referenced_col_names,
                                          foreign->col_nums, droppingIndex,
                                          /*check_charsets=*/true,
                                          /*check_null=*/false) &&
        nullptr == CdeFindEquivIndex(foreign->referenced_col_names,
                                     foreign->col_nums,
                                     haAlterInfo->key_info_buffer,
                                     haAlterInfo->index_add_buffer,
                                     haAlterInfo->index_add_count)) {
      trxInfo->err_index = droppingIndex;
      return true;
    }
  }

  /* Check for all FK references in current table using the index. */
  fks = &indexedTable->foreign_dict_set;
  for (cde_dict_foreign_set::iterator it = fks->begin(); it != fks->end();
       it++) {
    cde_dict_foreign_t *foreign = *it;
    if (foreign->foreign_index != droppingIndex) {
      continue;
    }

    CDE_ASSERT_DEBUG(indexedTable == foreign->foreign_table);

    if (!CdeDroppingForeign(foreign, drop_fk, n_drop_fk) &&
        nullptr == CdeForeignKeyFindIndex(
                       indexedTable, colNames, foreign->foreign_col_names,
                       foreign->col_nums, droppingIndex, true, false) &&
        nullptr == CdeFindEquivIndex(foreign->foreign_col_names,
                                     foreign->col_nums,
                                     haAlterInfo->key_info_buffer,
                                     haAlterInfo->index_add_buffer,
                                     haAlterInfo->index_add_count)) {
      trxInfo->err_index = droppingIndex;
      return true;
    }
  }

  return CDE_SUCC;
}

/** Create DStore foreign key structure from MySQL alter_info
@param[in]      ha_alter_info   alter table info
@param[in]      table Mysql     altered table
@param[in]      table_share     TABLE_SHARE
@param[in]      dictTable       dstore table object
@param[in]      col_names       column names, or NULL to use
table->col_names
@param[in]      drop_index      indexes to be dropped
@param[in]      n_drop_index    size of drop_index
@param[out]     add_fk          foreign constraint added
@param[out]     n_add_fk        number of foreign constraints
added
@param[in]      trx             user transaction
@param[in]      s_cols          list of stored column information
@retval true if successful
@retval false on error (will call my_error()) */
[[nodiscard]] static bool CdeFillForeignKeyInfo(
    Alter_inplace_info *ha_alter_info, const TABLE *alteredTable,
    const TABLE_SHARE *table_share, cde_dict_t *dictTable,
    const char **col_names, cde_dict_index_t **drop_index, ulint n_drop_index,
    cde_dict_foreign_t **add_fk, uint32_t *n_add_fk) {
  const Foreign_key_spec *fk_key;
  cde_dict_t *referenced_table = nullptr;
  char *referenced_table_name = nullptr;
  uint32_t num_fk = 0;
  Alter_info *alter_info = ha_alter_info->alter_info;

  DBUG_TRACE;

  *n_add_fk = 0;

  constexpr uint32_t MAX_NUM_FK_COLUMNS = 500;
  for (const Key_spec *key : alter_info->key_list) {
    MDL_ticket *mdl{nullptr};
    if (key->type != KEYTYPE_FOREIGN) {
      continue;
    }

    const char *column_names[MAX_NUM_FK_COLUMNS];
    cde_dict_index_t *index = nullptr;
    const char *referenced_column_names[MAX_NUM_FK_COLUMNS];
    cde_dict_index_t *referenced_index = nullptr;
    ulint num_col = 0;
    ulint referenced_num_col = 0;
    bool correct_option;
    char *db_namep = nullptr;
    char *tbl_namep = nullptr;
    char db_name[CDE_MAX_DATABASE_NAME_LEN];
    char tbl_name[CDE_MAX_TABLE_NAME_LEN];

    fk_key = down_cast<const Foreign_key_spec *>(key);

    if (fk_key->columns.size() > 0) {
      size_t i = 0;

      /* Get all the foreign key column info for the
      current table */
      while (i < fk_key->columns.size()) {
        column_names[i] = fk_key->columns[i]->get_field_name();
        i++;
      }

      index = CdeFindFkIndex(dictTable, col_names, drop_index, n_drop_index,
                             column_names, i);

      /** MySQL would add a index in the creation
      list if no such index for foreign table,
      so we have to use DBUG_EXECUTE_IF to simulate
      the scenario */
      DBUG_EXECUTE_IF("dstore_test_no_foreign_idx", index = nullptr;);

      /* Check whether there exist such
      index in the the index create clause */
      if (!index && !CdeFindEquivIndex(column_names, static_cast<uint>(i),
                                       ha_alter_info->key_info_buffer,
                                       ha_alter_info->index_add_buffer,
                                       ha_alter_info->index_add_count)) {
        my_error(ER_FK_NO_INDEX_CHILD, MYF(0),
                 fk_key->name.str ? fk_key->name.str : "",
                 table_share->table_name.str);
        return false;
      }

      num_col = i;
    }

    add_fk[num_fk] = new (std::nothrow) cde_dict_foreign_t;
    if (!add_fk[num_fk]) return false;

#ifndef _WIN32
    if (fk_key->ref_db.str) {
      tablename_to_filename(fk_key->ref_db.str, db_name,
                            CDE_MAX_DATABASE_NAME_LEN);
      db_namep = db_name;
    }
    if (fk_key->ref_table.str) {
      tablename_to_filename(fk_key->ref_table.str, tbl_name,
                            CDE_MAX_TABLE_NAME_LEN);
      tbl_namep = tbl_name;
    }
#else
    tablename_to_filename(fk_key->ref_table.str, tbl_name,
                          CDE_MAX_TABLE_NAME_LEN);
    my_casedn_str(tbl_name);
    tbl_namep = &tbl_name[0];

    if (fk_key->ref_db.str != NULL) {
      tablename_to_filename(fk_key->ref_db.str, db_name,
                            CDE_MAX_DATABASE_NAME_LEN);
      my_casedn_str(db_name);
      db_namep = &db_name[0];
    }
#endif
    referenced_table_name = DDGetRefrencedTable(
        dictTable->name.c_str(), db_namep, tbl_namep, &referenced_table, &mdl,
        &(add_fk[num_fk]->m_mem_root));

    DictTableRefGuard dictRefGuard(referenced_table, mdl, current_thd);

    if (!referenced_table &&
        !thd_test_options(current_thd, OPTION_NO_FOREIGN_KEY_CHECKS)) {
      my_error(ER_FK_CANNOT_OPEN_PARENT, MYF(0), tbl_namep);
      return false;
    }

    if (fk_key->ref_columns.size() > 0) {
      size_t i = 0;

      while (i < fk_key->ref_columns.size()) {
        referenced_column_names[i] = fk_key->ref_columns[i]->get_field_name();
        i++;
      }

      if (referenced_table) {
        referenced_index = CdeForeignKeyFindIndex(referenced_table, nullptr,
                                                  referenced_column_names, i,
                                                  index, true, false);
        DBUG_EXECUTE_IF("dstore_test_no_reference_idx",
                        referenced_index = nullptr;);

        /* Check whether there exist such
        index in the the index create clause */
        if (!referenced_index) {
          my_error(ER_FK_NO_INDEX_PARENT, MYF(0),
                   fk_key->name.str ? fk_key->name.str : "", tbl_namep);
          return false;
        }
      } else {
        CDE_ASSERT(thd_test_options(current_thd, OPTION_NO_FOREIGN_KEY_CHECKS));
      }

      referenced_num_col = i;
    } else {
      /* Not possible to add a foreign key without a
      referenced column */
      my_error(ER_CANNOT_ADD_FOREIGN, MYF(0), tbl_namep);
      return false;
    }

    if (!CdeInitForeign(add_fk[num_fk], fk_key->name.str, dictTable, index,
                        column_names, num_col, referenced_table_name,
                        referenced_table, referenced_index,
                        referenced_column_names, referenced_num_col)) {
      my_error(ER_FK_DUP_NAME, MYF(0), add_fk[num_fk]->unique_name);
      return false;
    }

    correct_option = CdeSetForeignRule(add_fk[num_fk], fk_key);

    DBUG_EXECUTE_IF("dstore_test_wrong_fk_option", correct_option = false;);

    if (!correct_option) {
      my_error(ER_FK_INCORRECT_OPTION, MYF(0), table_share->table_name.str,
               add_fk[num_fk]->unique_name);
      return false;
    }

    if (CheckFkBaseCol(add_fk[num_fk], alteredTable, dictTable)) {
      my_error(ER_CANNOT_ADD_FOREIGN_BASE_COL_STORED, MYF(0));
      return false;
    }

    num_fk++;
  }

  *n_add_fk = num_fk;

  return true;
}

/** Find the added autoincrement column in the altered table.
@param[in]  haAlterInfo   Structure describing changes to be done by
@param[in]  oldTable       TABLE object for old version of table.
@param[in]  alteredTable   TABLE object for new version of table.
@param[out]  addAutoIncColNo   The finded autoincement column index.
@param[out]  autoincColMaxValue   The defined max autoincrement value of the
autoincrement column.
*/
static void FindAddedAutoincColumn(Alter_inplace_info *haAlterInfo,
                                   TABLE *oldTable, TABLE *alteredTable,
                                   uint32_t &addAutoIncColNo,
                                   uint64_t &autoincColMaxValue) {
  uint32_t i = 0;
  uint32_t numV = 0;
  List_iterator_fast<Create_field> cfIt(haAlterInfo->alter_info->create_list);

  while (const Create_field *newField = cfIt++) {
    CDE_ASSERT_DEBUG(i < alteredTable->s->fields);

    uint32_t oldIdx = 0;
    for (; oldTable->field[oldIdx] != nullptr; oldIdx++) {
      if (newField->field == oldTable->field[oldIdx]) {
        /* This is an exist column. */
        break;
      }
    }

    if (oldIdx == oldTable->s->fields) {
      /* This is an added column. */
      CDE_ASSERT_DEBUG(!newField->field);
      CDE_ASSERT_DEBUG(haAlterInfo->handler_flags &
                       Alter_inplace_info::ADD_COLUMN);
      const Field *field = alteredTable->field[i];

      if (field->is_flag_set(AUTO_INCREMENT_FLAG)) {
        if (addAutoIncColNo != (uint32_t)(~0)) {
          /* This should have been blocked earlier. */
          CDE_ASSERT(false);
        }
        addAutoIncColNo = i - numV;
        autoincColMaxValue = field->get_max_int_value();
      }
    }

    /* virtual column */
    if (newField->gcol_info != nullptr && !(newField->stored_in_db)) {
      ++numV;
    }
    i++;
  }
}

void ha_cde::CommitGetAutoinc(Alter_inplace_info *haAlterInfo,
                              const TABLE *alteredTable,
                              const TABLE *oldTable) {
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);

  if (!alteredTable->found_next_number_field) {
    /* There is no AUTO_INCREMENT column in the table
    after the ALTER operation. */
    return;
  }

  if (ctx->addAutoinc != (uint32_t)(~0)) {
    /* An AUTO_INCREMENT column was added. Get the last
    value from the sequence, which may be based on a
    supplied AUTO_INCREMENT value. */
    ctx->maxAutoinc = ctx->sequence.Last();
    return;
  }

  if ((haAlterInfo->handler_flags & Alter_inplace_info::CHANGE_CREATE_OPTION) &&
      (haAlterInfo->create_info->used_fields & HA_CREATE_USED_AUTO)) {
    /* An AUTO_INCREMENT value was supplied, but the table was not
    rebuilt. Get the user-supplied value or the last value from the
    sequence. */
    ctx->maxAutoinc = haAlterInfo->create_info->auto_increment_value;

    ctx->old_table->autoinc_dict.MutexEnter();
    uint64_t maxValueTable = ctx->old_table->autoinc_dict.GetAutoinc();

    /* We still have to search the index here when we want to
    set the AUTO_INCREMENT value to a smaller or equal one.

    Here is an example:
    Let's say we have a table t1 with one AUTOINC column, existing
    rows (1), (2), (100), (200), (1000), after following SQLs:
    DELETE FROM t1 WHERE a > 200;
    ALTER TABLE t1 AUTO_INCREMENT = 150;
    we expect the next value allocated from 201, but not 150.

    We could only search the tree to know current max counter
    in the table and compare. */
    if (ctx->maxAutoinc <= maxValueTable) {
      session_index_info *autoincIndex =
          cde_index_lookup(oldTable->s->next_number_index);
      if (LoadMaxAutoincFromIndex(oldTable, autoincIndex->rel,
                                  autoincIndex->shared_dict->index_col_num,
                                  &maxValueTable)) {
        CDE_ASSERT(false);
      }

      if (ctx->maxAutoinc <= maxValueTable) {
        Field *autoincField = oldTable->found_next_number_field;
        uint64_t colMaxValue = autoincField->get_max_int_value();
        uint64_t offset = m_dstore->autoincCtx.m_autoincIncrement;
        ctx->maxAutoinc =
            CalcNextAutoinc(maxValueTable, 1, 1, offset, colMaxValue);
      }
    }

    ctx->old_table->autoinc_dict.MutexExit();
    return;
  }

  /* An AUTO_INCREMENT value was not specified.
  Read the old counter value from the table. */
  CDE_ASSERT_DEBUG(oldTable->found_next_number_field);
  ctx->old_table->autoinc_dict.MutexEnter();
  ctx->maxAutoinc = ctx->old_table->autoinc_dict.GetAutoinc();
  ctx->old_table->autoinc_dict.MutexExit();
}

/** Check if dstore supports a particular alter table in-place.
@param[in]	altered_table	TABLE object for new version of table.
@param[in,out]	ha_alter_info	Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.

@retval	HA_ALTER_INPLACE_NOT_SUPPORTED	Not supported
@retval	HA_ALTER_INPLACE_NO_LOCK	Supported
@retval	HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE	Supported, but
requires lock during main phase and exclusive lock during prepare phase.
@retval	HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE	Supported, prepare phase
requires exclusive lock. */
enum_alter_inplace_result ha_cde::check_if_supported_inplace_alter(
    TABLE *altered_table, Alter_inplace_info *ha_alter_info) {
  if (altered_table->s->fields > DSTORE::MAX_TUPLE_ATTR) {
    /* Deny the inplace ALTER TABLE. MySQL will try to
    re-create the table and ha_cde::create() will
    return an error too. This is how we effectively
    deny adding too many columns to a table. */
    ha_alter_info->unsupported_reason = my_get_err_msg(ER_TOO_MANY_FIELDS);
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  THD *thd = ha_thd();
  CdeCreateOrGetSession(thd);

  // Check whether the current DDL operation supports neither inplace nor
  // instant.
  if (ha_alter_info->handler_flags &
      ~(DSTORE_INPLACE_IGNORE | DSTORE_ALTER_NOREBUILD |
        DSTORE_ALTER_REBUILD)) {
    if (ha_alter_info->handler_flags &
        Alter_inplace_info::ALTER_STORED_COLUMN_TYPE) {
      if (ha_alter_info->alter_info->requested_algorithm ==
          Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
        ha_alter_info->unsupported_reason = my_get_err_msg(
            ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_COLUMN_TYPE_INSTANT);
      } else {
        ha_alter_info->unsupported_reason =
            my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_COLUMN_TYPE);
      }
    }
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  /* Only support online add foreign key constraint when check_foreigns is
  turned off */
  if ((ha_alter_info->handler_flags & Alter_inplace_info::ADD_FOREIGN_KEY) &&
      !thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS)) {
    ha_alter_info->unsupported_reason =
        my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FK_CHECK);
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  if (altered_table->file->ht != ht) {
    /* Non-native partitioning table engine. No longer supported, due to
    implementation of native dstore partitioning. */
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  // Check whether the current DDL operation supports instant. For
  // DSTORE_INPLACE_IGNORE, we treat it as instant.
  auto inst_type = CdeGetInstantType(
      ha_alter_info, this->m_dstore->table_handler, this->table, altered_table);

  if ((inst_type == dstore_instant_type::INSTANT_VARCHAR_RESIZE ||
       inst_type ==
           dstore_instant_type::INSTANT_RENAME_COLUMN_WITH_VARCHAR_RESIZE) &&
      CdeNeedRebuildForIndexedVarcharResize(ha_alter_info, this->table,
                                            altered_table)) {
    inst_type = dstore_instant_type::INSTANT_IMPOSSIBLE;
  }

  switch (inst_type) {
    case dstore_instant_type::INSTANT_VIRTUAL_ONLY:
      if (m_dstore->table_handler->m_totalColCount +
              GetNumColsAdded(ha_alter_info) >
          DSTORE::MAX_TUPLE_ATTR) {
        if (ha_alter_info->alter_info->requested_algorithm ==
            Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
          my_error(ER_DSTORE_INSTANT_ADD_NOT_SUPPORTED_MAX_FIELDS, MYF(0),
                   m_dstore->table_handler->name.c_str());
          return HA_ALTER_ERROR;
        }
        break;
      } else if (!IsInstantAddDropPossible(ha_alter_info, table,
                                           altered_table)) {
        if (ha_alter_info->alter_info->requested_algorithm ==
            Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
          /* Blob column can't be added with instant algorithm. */
          my_error(ER_DSTORE_INSTANT_ADD_NOT_SUPPORTED_BLOB, MYF(0));
          return HA_ALTER_ERROR;
        }
        break;
      }
      ha_alter_info->handler_trivial_ctx = CdeInstantTypeToUint(inst_type);
      return HA_ALTER_INPLACE_INSTANT;
    case dstore_instant_type::INSTANT_ADD_DROP_COLUMN:
      if (ha_alter_info->alter_info->requested_algorithm ==
          Alter_info::ALTER_TABLE_ALGORITHM_INPLACE) {
        /* Still fall back to INPLACE since the behaviour is different */
        break;
      }

      if (m_dstore->table_handler->m_currentRowVersion >
          DSTORE_MAX_ROW_VERSION) {
        /* Has reached max row version. */
        if (ha_alter_info->alter_info->requested_algorithm ==
            Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
          my_error(ER_DSTORE_MAX_ROW_VERSION, MYF(0),
                   m_dstore->table_handler->name.c_str());
          return HA_ALTER_ERROR;
        }
        break;
      }

      if (m_dstore->table_handler->m_totalColCount +
              GetNumColsAdded(ha_alter_info) >
          DSTORE::MAX_TUPLE_ATTR) {
        if (ha_alter_info->alter_info->requested_algorithm ==
            Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
          my_error(ER_DSTORE_INSTANT_ADD_NOT_SUPPORTED_MAX_FIELDS, MYF(0),
                   m_dstore->table_handler->name.c_str());
          return HA_ALTER_ERROR;
        }
        break;
      }

      if (ha_alter_info->error_if_not_empty) {
        break;
      }

      if (!IsInstantAddDropPossible(ha_alter_info, table, altered_table)) {
        if (ha_alter_info->alter_info->requested_algorithm ==
            Alter_info::ALTER_TABLE_ALGORITHM_INSTANT) {
          /* Blob column can't be added with instant algorithm. */
          my_error(ER_DSTORE_INSTANT_ADD_NOT_SUPPORTED_BLOB, MYF(0));
          return HA_ALTER_ERROR;
        }

        break;
      }
      [[fallthrough]];
    case dstore_instant_type::INSTANT_RENAME_COLUMN:
    case dstore_instant_type::INSTANT_NO_CHANGE:
    case dstore_instant_type::INSTANT_VARCHAR_RESIZE:
    case dstore_instant_type::INSTANT_RENAME_COLUMN_WITH_VARCHAR_RESIZE:
      ha_alter_info->handler_trivial_ctx = CdeInstantTypeToUint(inst_type);
      return HA_ALTER_INPLACE_INSTANT;
    default:
      break;
  }

  /* Only support NULL -> NOT NULL change if strict table sql_mode
  is set. Fall back to COPY for conversion if not strict tables.
  In-Place will fail with an error when trying to convert
  NULL to a NOT NULL value. */
  if ((ha_alter_info->handler_flags &
       Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE) &&
      !thd_is_strict_mode(thd)) {
    ha_alter_info->unsupported_reason =
        my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_NOT_NULL);
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  /* If there is add or drop virtual columns, we will support operations
  with these 3 options alone with inplace interface for now */
  if (ha_alter_info->handler_flags &
      (Alter_inplace_info::ADD_VIRTUAL_COLUMN |
       Alter_inplace_info::DROP_VIRTUAL_COLUMN |
       Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER)) {
    ulonglong flags = ha_alter_info->handler_flags;

    /* TODO: uncomment the flags below, once we start to
    support themm, now only keep  */
    flags &=
        ~(Alter_inplace_info::ADD_VIRTUAL_COLUMN |
          Alter_inplace_info::DROP_VIRTUAL_COLUMN |
          Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER
          /*
          | Alter_inplace_info::ALTER_STORED_COLUMN_ORDER
          | Alter_inplace_info::ADD_STORED_BASE_COLUMN
          | Alter_inplace_info::DROP_STORED_COLUMN
          | Alter_inplace_info::ALTER_STORED_COLUMN_ORDER
          | Alter_inplace_info::ADD_UNIQUE_INDEX
          */
          | Alter_inplace_info::ADD_INDEX | Alter_inplace_info::DROP_INDEX);

    if (flags != 0 ||
        (altered_table->s->partition_info_str &&
         altered_table->s->partition_info_str_len) ||
        (!CheckVirtualColInOrder(this->table, altered_table, ha_alter_info))) {
      ha_alter_info->unsupported_reason =
          my_get_err_msg(ER_UNSUPPORTED_ALTER_INPLACE_ON_VIRTUAL_COLUMN);
      return HA_ALTER_INPLACE_NOT_SUPPORTED;
    }

    if (ha_alter_info->handler_flags & Alter_inplace_info::ADD_VIRTUAL_COLUMN) {
      if (!CdeCheckAddVirtualColumn(ha_alter_info, altered_table, this->table,
                                    m_dstore->table_handler)) {
        ha_alter_info->unsupported_reason = my_get_err_msg(
            ER_DSTORE_UNSUPPORTED_INPLACE_FIRST_ADD_BLOB_VIRTUAL_COLUMN);
        return HA_ALTER_INPLACE_NOT_SUPPORTED;
      }
    }
  }

  // If the current DDL operation does not support instant, then check wether it
  // support inplace.
  bool online = true;

  /* Fix the key parts. */
  List_iterator_fast<Create_field> cfIt(ha_alter_info->alter_info->create_list);

  for (KEY *newKey = ha_alter_info->key_info_buffer;
       newKey < ha_alter_info->key_info_buffer + ha_alter_info->key_count;
       newKey++) {
    for (KEY_PART_INFO *keyPart = newKey->key_part;
         keyPart < newKey->key_part + newKey->user_defined_key_parts;
         keyPart++) {
      const Create_field *newField = nullptr;

      CDE_ASSERT(keyPart->fieldnr < altered_table->s->fields);

      cfIt.rewind();
      for (auto fieldNr = 0; fieldNr != keyPart->fieldnr + 1; fieldNr++) {
        newField = cfIt++;
        CDE_ASSERT(newField);
      }

      keyPart->field = altered_table->field[keyPart->fieldnr];
      /* In some special cases dstore emits "false"
      duplicate key errors with NULL key values. Let
      us play safe and ensure that we can correctly
      print key values even in such cases. */
      keyPart->null_offset = keyPart->field->null_offset();
      keyPart->null_bit = keyPart->field->null_bit;

      if (newField->field) {
        /* This is an existing column. */
        continue;
      }

      if (keyPart->field->is_flag_set(AUTO_INCREMENT_FLAG)) {
        /* For inplace rebuild table scene, AUTO_INCREMENT column and
        it's related index can't be added as online, because we cannot
        assign an AUTO_INCREMENT column values during online ALTER. */
        CDE_ASSERT(keyPart->field == altered_table->found_next_number_field);
        ha_alter_info->unsupported_reason =
            my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_AUTOINC);
        online = false;
      }

      if (keyPart->field->is_virtual_gcol()) {
        /* Do not support adding index on newly added
        virtual column, while there is also a drop
        virtual column in the same clause */
        if (ha_alter_info->handler_flags &
            Alter_inplace_info::DROP_VIRTUAL_COLUMN) {
          ha_alter_info->unsupported_reason =
              my_get_err_msg(ER_UNSUPPORTED_ALTER_INPLACE_ON_VIRTUAL_COLUMN);

          return HA_ALTER_INPLACE_NOT_SUPPORTED;
        }

        /* We are not sure why InnoDB doesn't support online adding index on
        newly added virtual column, such as functional index. For implementation
        of online DDL of dstore, it can be supported. */
        ha_alter_info->unsupported_reason =
            my_get_err_msg(ER_UNSUPPORTED_ALTER_ONLINE_ON_VIRTUAL_COLUMN);
        if (!g_onlineAddIndexOnNewVcolEnable) {
          online = false;
        }
      }
    }
  }

  if (CdeNeedRebuild(ha_alter_info, this->table, altered_table)) {
    /* Not support online rebuild table yet. */
    online = false;
  }

  if (!online) {
    /* We already determined that only a non-locking
    operation is possible. */
  } else if (ha_alter_info->handler_flags & DSTORE_ONLINE_CREATE) {
    /* Building a full-text index requires a lock.
    We could do without a lock if the table already contains
    an FTS_DOC_ID column, but in that case we would have
    to apply the modification log to the full-text indexes. */

    for (uint i = 0; i < ha_alter_info->index_add_count && online; i++) {
      const KEY *newKey =
          &ha_alter_info->key_info_buffer[ha_alter_info->index_add_buffer[i]];
      for (KEY_PART_INFO *keyPart = newKey->key_part;
           keyPart < newKey->key_part + newKey->user_defined_key_parts;
           keyPart++) {
        /* Not support adding index on virtual column while any base
        column of this virtual column is lob or json type. */
        if (keyPart->field->is_virtual_gcol()) {
          const MY_BITMAP *baseColMap =
              &keyPart->field->gcol_info->base_columns_map;
          Field **tableFields = keyPart->field->table->field;
          for (uint i = 0; i < keyPart->field->table->s->fields; ++i) {
            if (!tableFields[i]->is_virtual_gcol() &&
                bitmap_is_set(baseColMap, i) &&
                (is_blob(tableFields[i]->type()) ||
                 tableFields[i]->type() == MYSQL_TYPE_JSON)) {
              ha_alter_info->unsupported_reason =
                  my_get_err_msg(ER_DSTORE_ONLINE_CREATE_INDEX_UNSUPPORTED_LOB);
              online = false;
              break;
            }
          }
        }
      }
    }
  }

  online &= g_onlineDdlEnable;
  return online ? HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE
                : HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE;
}

/** Prepare in-place ALTER for table.
@param[in]  alteredTable   TABLE object for new version of table.
@param[in,out]  haAlterInfo   Structure describing changes to be done by
ALTER TABLE and holding data used during in-place alter.
@param[in]  oldDdTab   dd::Table object describing old version of the
table.
@param[in,out]  new_table_def   dd::Table object for the new version of the
table. Can be adjusted by this call. Changes to the table definition will be
persisted in the data-dictionary at statement commit time.
@retval true    Failure.
@retval false   Success. */
bool ha_cde::prepare_inplace_alter_table(TABLE *alteredTable,
                                         Alter_inplace_info *haAlterInfo,
                                         const dd::Table *oldDdTab,
                                         dd::Table *newDdTab) {
  cde_dict_index_t **dropIndex = nullptr;   /*!< Index to be dropped */
  uint32_t dropIndexNum;                    /*!< Number of indexes to drop */
  cde_dict_index_t **renameIndex = nullptr; /*!< Indexes to be dropped */
  uint32_t renameIndexNum;                  /*!< Number of indexes to rename */

  cde_dict_t *indexedTable; /*!< Table where indexes are created */
  const char **colNames;    /*!< The new table col names when we rename column*/

  uint32_t addAutoincColNo = (uint32_t)(~0);
  uint64_t autoincColMaxValue = 0;

  if (alteredTable->found_next_number_field != nullptr) {
    DDCopyAutoinc(oldDdTab->se_private_data(), newDdTab->se_private_data());
    CdeDdTableSetSePrivateDataAutoInc(
        newDdTab, haAlterInfo->create_info->auto_increment_value);
  }

  if (CdeIsInstantAlter(haAlterInfo)) {
    return false;
  }

  indexedTable = m_dstore->table_handler;

  /* Check that index keys are sensible */
  THD *thd = ha_thd();
  int error = CdeCheckIndexKeys(haAlterInfo, indexedTable);

  if (error) {
    return true;
  }

  dropIndexNum = 0;

  if (haAlterInfo->handler_flags &
      (DSTORE_ALTER_NOREBUILD | DSTORE_ALTER_REBUILD)) {
    if (haAlterInfo->handler_flags & Alter_inplace_info::ALTER_COLUMN_NAME) {
      colNames = CdeGetColNames(haAlterInfo, table, indexedTable);
    } else {
      colNames = nullptr;
    }
  } else {
    colNames = nullptr;
  }

  /** Foreign key constraints to drop */
  cde_dict_foreign_t **toBeDroppedFk = nullptr;
  /** Number of foreign keys to drop */
  uint32_t toBeDroppedFkNum = 0;
  if (haAlterInfo->handler_flags & Alter_inplace_info::DROP_FOREIGN_KEY) {
    CDE_ASSERT_DEBUG(haAlterInfo->alter_info->drop_list.size() > 0);

    toBeDroppedFk = static_cast<cde_dict_foreign_t **>(
        CdeZalloc(haAlterInfo->alter_info->drop_list.size() *
                  sizeof(cde_dict_foreign_t *)));

    for (const Alter_drop *drop : haAlterInfo->alter_info->drop_list) {
      if (drop->type != Alter_drop::FOREIGN_KEY) {
        continue;
      }

      for (cde_dict_foreign_set::iterator it =
               indexedTable->foreign_dict_set.begin();
           it != indexedTable->foreign_dict_set.end(); ++it) {
        cde_dict_foreign_t *foreign = *it;
        const char *fid = strchr(foreign->unique_name, '/');

        CDE_ASSERT(fid);
        /* If no database/ prefix was present in
        the FOREIGN KEY constraint name, compare
        to the full constraint name. */
        fid = fid ? fid + 1 : foreign->unique_name;

        if (!my_strcasecmp(system_charset_info, fid, drop->name)) {
          toBeDroppedFk[toBeDroppedFkNum++] = foreign;
          goto found_fk;
        }
      }

      /*
        Since we check that foreign key to be dropped exists on SQL-layer,
        we should not come here unless there is some bug and data-dictionary
        and InnoDB dictionary cache got out of sync.
      */
      my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0), drop->name);
      InplaceDdlCleanEnv(dropIndexNum, dropIndex, 0, nullptr, colNames,
                         toBeDroppedFk, 0, nullptr);
      return true;
    found_fk:
      continue;
    }

    CDE_ASSERT(toBeDroppedFkNum > 0);
  }

  /* Add the index to be dropped in a vector */
  if (haAlterInfo->index_drop_count) {
    CDE_ASSERT_DEBUG(haAlterInfo->handler_flags &
                     (Alter_inplace_info::DROP_INDEX |
                      Alter_inplace_info::DROP_UNIQUE_INDEX |
                      Alter_inplace_info::DROP_PK_INDEX));

    /* Check which indexes to drop. */
    dropIndex = static_cast<cde_dict_index_t **>(
        CdeZalloc((haAlterInfo->index_drop_count + 1) * sizeof *dropIndex));

    for (uint32_t i = 0; i < haAlterInfo->index_drop_count; i++) {
      const KEY *key = haAlterInfo->index_drop_buffer[i];
      cde_dict_index_t *index = indexedTable->get_index_on_name(key->name);

      if (!index) {
        push_warning_printf(thd, Sql_condition::SL_WARNING, HA_ERR_WRONG_INDEX,
                            "DStore could not find key"
                            " with name %s",
                            key->name);
      } else {
        dropIndex[dropIndexNum++] = index;
      }
    }

    /* Check if the indexes can be dropped. */

    /* Prevent a race condition between DROP INDEX and
    CREATE TABLE adding FOREIGN KEY constraints. */
    {
      DictSysLockGuard dictSysLockGuard;
      for (uint32_t i = 0; i < dropIndexNum; i++) {
        CDE_ASSERT_DEBUG(!dropIndex[i]->toBeDropped);
        dropIndex[i]->toBeDropped = true;
      }

      for (uint32_t i = 0; i < dropIndexNum; i++) {
        cde_dict_index_t *index = dropIndex[i];

        if (CdeCheckForeignKeyIndex(haAlterInfo, colNames, index, indexedTable,
                                    toBeDroppedFk, toBeDroppedFkNum)) {
          print_error(HA_ERR_DROP_INDEX_FK, MYF(0));
          InplaceDdlCleanEnv(dropIndexNum, dropIndex, 0, nullptr, colNames,
                             toBeDroppedFk, 0, nullptr);
          cde_trxinfo_t *trxInfo = CdeGetTrxinfo(thd);
          trxInfo->err_index = index;
          return true;
        }
      }
    }
  }

  renameIndexNum = haAlterInfo->index_rename_count;
  renameIndex = nullptr;

  /* Create a list of cde_dict_index_t objects that are to be renamed,
  also checking for requests to rename nonexistent indexes. */
  if (renameIndexNum > 0) {
    renameIndex = static_cast<cde_dict_index_t **>(
        CdeZalloc((renameIndexNum) * sizeof(*renameIndex)));
    for (uint32_t i = 0; i < renameIndexNum; i++) {
      cde_dict_index_t *index;
      const char *old_name = haAlterInfo->index_rename_buffer[i].old_key->name;

      index = indexedTable->get_index_on_name(old_name);
      DBUG_EXECUTE_IF("inplace_rename_index_error", index = nullptr;);
      if (index == nullptr) {
        my_error(ER_KEY_DOES_NOT_EXITS, MYF(0), old_name,
                 indexedTable->name.c_str());
        InplaceDdlCleanEnv(dropIndexNum, dropIndex, renameIndexNum, renameIndex,
                           colNames, toBeDroppedFk, 0, nullptr);
        return true;
      }

      renameIndex[i] = index;
    }
  }

  /** Foreign key constraints to be added */
  cde_dict_foreign_t **toBeAddedFk = nullptr;
  /** number of foreign key constraints to be added */
  uint32_t toBeAddedFkNum = 0;
  if (haAlterInfo->handler_flags & Alter_inplace_info::ADD_FOREIGN_KEY) {
    toBeAddedFk = static_cast<cde_dict_foreign_t **>(
        CdeZalloc(haAlterInfo->alter_info->key_list.size() *
                  sizeof(cde_dict_foreign_t *)));
    if (!CdeFillForeignKeyInfo(haAlterInfo, alteredTable, table_share,
                               indexedTable, colNames, dropIndex, dropIndexNum,
                               toBeAddedFk, &toBeAddedFkNum)) {
      InplaceDdlCleanEnv(dropIndexNum, dropIndex, renameIndexNum, renameIndex,
                         colNames, toBeDroppedFk,
                         haAlterInfo->alter_info->key_list.size(), toBeAddedFk);
      return true;
    }
  }

  if ((!(haAlterInfo->handler_flags & DSTORE_ALTER_DATA) ||
       ((haAlterInfo->handler_flags & ~DSTORE_INPLACE_IGNORE) ==
        Alter_inplace_info::CHANGE_CREATE_OPTION)) &&
      !CdeNeedRebuild(haAlterInfo, table, alteredTable)) {
    haAlterInfo->handler_ctx = new (*THR_MALLOC) ha_cde_inplace_ctx(
        m_dstore, dropIndex, dropIndexNum, renameIndex, renameIndexNum,
        toBeDroppedFk, toBeDroppedFkNum, toBeAddedFk, toBeAddedFkNum,
        indexedTable, colNames, addAutoincColNo, 0, 0, nullptr);
    ha_cde_inplace_ctx *ctx =
        static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);

    if (haAlterInfo->handler_flags & ~DSTORE_INPLACE_IGNORE) {
      /* If there is only DSTORE_INPLACE_IGNORE operations, Nothing to do.
      Since there is no MDL protected, don't try to drop aborted indexes here.
      Only when there is at least one not DSTORE_INPLACE_IGNORE operation can we
      drop aborted indexes here. */
      DictSysLockGuard dictSysLockGuard;
      m_dstore->table_handler->OnlineRetryDropDictIndexes();
    }

    if ((haAlterInfo->handler_flags &
         Alter_inplace_info::DROP_VIRTUAL_COLUMN) &&
        CdePrepareCheckDropVirtualColumn(haAlterInfo, table)) {
      return true;
    }

    if ((haAlterInfo->handler_flags & Alter_inplace_info::ADD_VIRTUAL_COLUMN) &&
        CdePrepareCheckAddVirtualColumn(haAlterInfo, alteredTable, table)) {
      return true;
    }

    if (ctx->old_table == ctx->new_table) {
      return false;
    }
  }

  FindAddedAutoincColumn(haAlterInfo, table, alteredTable, addAutoincColNo,
                         autoincColMaxValue);

  haAlterInfo->handler_ctx = new (thd->mem_root) ha_cde_inplace_ctx(
      m_dstore, dropIndex, dropIndexNum, renameIndex, renameIndexNum,
      toBeDroppedFk, toBeDroppedFkNum, toBeAddedFk, toBeAddedFkNum,
      m_dstore->table_handler, colNames, addAutoincColNo,
      haAlterInfo->create_info->auto_increment_value, autoincColMaxValue, thd);
  return CdePrepareInplaceAlterTableDict(haAlterInfo, alteredTable, table,
                                         newDdTab, table_share->table_name.str,
                                         thd);
}

/** Alter the table structure in-place with operations specified using
HA_ALTER_FLAGS and Alter_inplace_information. The level of concurrency allowed
during this operation depends on the return value from
check_if_supported_inplace_alter().
@param[in]	altered_table	TABLE object for new version of table.
@param[in,out]	ha_alter_info	Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@param[in]	 old_dd_tab	dd::Table object describing old version of the
table.
@param[in,out]	 new_dd_tab	dd::Table object for the new version of
the table. Can be adjusted by this call. Changes to the table definition will
be persisted in the data-dictionary at statement commit time.
@retval true Failure
@retval false Success
*/
bool ha_cde::inplace_alter_table(TABLE *altered_table,
                                 Alter_inplace_info *ha_alter_info,
                                 const dd::Table *old_dd_tab [[maybe_unused]],
                                 dd::Table *new_dd_tab [[maybe_unused]]) {
  CDE_ASSERT_DEBUG(old_dd_tab != nullptr);
  CDE_ASSERT_DEBUG(new_dd_tab != nullptr);
  return InplaceAlterTableImpl<dd::Table>(altered_table, ha_alter_info);
}

/**
Set discard after ddl flag, handler will reopen dict table and get newest
info from dd.

@param[in,out] table dict table to modify
@param[in,out] form  table form to modify
*/
void SetDiscardAfterDdl(cde_dict_t *table, TABLE *form) {
  {
    DictSysLockGuard guard;
    table->m_discardAfterDDL = true;
  }
  form->invalidate_dict();
}

void UpdateDictTableAndIndexCsn(cde_dict_t *table, dd::Table *table_def) {
  DictSysLockGuard guard;
  CommitSeqNo csn = TransactionInterface::GetTransactionSnapshotCsn();
  table->m_rebuild_csn = csn;

  for (cde_dict_index_t *index : table->index_dict_vec) {
    if (!index->IsCommitted()) {
      continue;
    }
    index->index_csn = csn;
  }

  for (auto dd_index : *table_def->indexes()) {
    dd_index->se_private_data().set(object_indexcreatecsn, csn);
  }

  table_def->se_private_data().set(object_tablecreatecsn, csn);
}

/**
  Keep all needed info for alter table instant
 */
template <typename Table>
class Instant_ddl_ctx {
 public:
  Instant_ddl_ctx(Alter_inplace_info *alter_info, cde_dict_t *dict_table,
                  const TABLE *old_table, TABLE *altered_table,
                  const Table *old_dd_tab, Table *new_dd_tab)
      : ha_alter_info(alter_info),
        dict_table(dict_table),
        old_table(old_table),
        altered_table(altered_table),
        old_dd(old_dd_tab),
        new_dd(new_dd_tab) {}
  /** Commit instant ddl */
  bool commit();

  /**
  Populate the alter info and collect the columns to be add or drop.

  @param[in]      haAlterInfo  the alter info
  @param[in]      oldTable     the old table.
  @param[in]      alteredTable the altered table.
  @param[in]      skipVirtual  whether skip virtual column.
  @param[out]     colsToAdd    the columns to be add.
  @param[out]     colsToDrop   the columns to be drop.
  */
  static void PopulateToBeInstantColumns(const Alter_inplace_info *haAlterInfo,
                                         const TABLE *oldTable,
                                         const TABLE *alteredTable,
                                         bool skipVirtual, Columns &colsToAdd,
                                         Columns &colsToDrop);

 private:
  /**
  Update metadata in commit phase

  @return true if success.
  */
  bool dd_commit_inplace_no_change();

  bool CommitInstantDropCol();

  void CommitInstantAddCol();

 private:
  /* Inplace alter info */
  Alter_inplace_info *ha_alter_info;

  /* Dstore dict table object */
  cde_dict_t *dict_table;

  /* MySQL table before the ALTER operation */
  const TABLE *old_table;

  /* MySQL table that is being altered */
  TABLE *altered_table;

  /* Old dd table definition */
  const Table *old_dd;

  /* New dd table definition, needs to be updated */
  Table *new_dd;

  /* Columns which are to be added instantly */
  Columns m_colsToAdd;

  /* Columns which are to be dropped instantly */
  Columns m_colsToDrop;
};

template <typename Table>
bool Instant_ddl_ctx<Table>::dd_commit_inplace_no_change() {
  if (TableHasDroppedColumn(*old_dd)) {
    if (!DDCopyDroppedColumns(*old_dd, *new_dd)) {
      return false;
    }
  }

  CdeDdTableCopyPrivate(*new_dd, *old_dd);

  DDCopyColumnPrivate(*ha_alter_info, *old_dd, *new_dd, nullptr);

  return true;
}

using RenamedFieldsVec = std::vector<std::pair<std::string, std::string>>;

/**
Collect all renamed columns.

@param[in]      table       the table to be collected
@param[in]      haAlterInfo alter info

@return the renamed column set.
*/
static RenamedFieldsVec CollectRenamedColumns(
    const TABLE *table, const Alter_inplace_info &haAlterInfo) {
  RenamedFieldsVec renamedFields;
  for (size_t i = 0; i < table->s->fields; i++) {
    const char *fieldName = table->field[i]->field_name;
    std::string newName;
    if (IsFieldToRename(haAlterInfo, fieldName, newName)) {
      renamedFields.push_back(std::pair(fieldName, newName));
    }
  }

  return renamedFields;
}

/**
Update dict table col count after instant ddl.

@param[in]      dictTable        the dict table
@param[in]      addedColumnNum   instant added column.
@param[in]      droppedColumnNum instant dropped column.
*/
static void InstantUpdateTableColsCount(cde_dict_t *dictTable,
                                        uint32_t addedColumnNum,
                                        uint32_t droppedColumnNum) {
  dictTable->m_currentColCount += addedColumnNum;
  dictTable->m_currentColCount -= droppedColumnNum;
  dictTable->m_totalColCount += addedColumnNum;
}

bool IsInstantAddDropPossible(const Alter_inplace_info *haAlterInfo,
                              const TABLE *table, const TABLE *alteredTable) {
  Columns colsToAdd;
  Columns colsToDrop;
  Instant_ddl_ctx<dd::Table>::PopulateToBeInstantColumns(
      haAlterInfo, table, alteredTable, false, colsToAdd, colsToDrop);
  for (const auto &field : colsToAdd) {
    auto type = field->type();
    if (is_blob(type) || type == MYSQL_TYPE_GEOMETRY ||
        type == MYSQL_TYPE_JSON) {
      return false;
    }
  }

  return true;
}

template <typename Table>
void Instant_ddl_ctx<Table>::PopulateToBeInstantColumns(
    const Alter_inplace_info *haAlterInfo, const TABLE *oldTable,
    const TABLE *alteredTable, bool skipVirtual, Columns &colsToAdd,
    Columns &colsToDrop) {
  auto renamedFields = CollectRenamedColumns(oldTable, *haAlterInfo);

  for (size_t i = 0; i < oldTable->s->fields; i++) {
    Field *oldTableField = oldTable->field[i];
    const char *oldFieldName = oldTableField->field_name;

    /* Skip virtual column from old table */
    if (skipVirtual && IsVirtualGeneratedField(oldTableField)) {
      continue;
    }

    /* Skip if this column is being renamed */
    auto it = std::find_if(
        renamedFields.begin(), renamedFields.end(),
        [&oldFieldName](std::pair<std::string, std::string> &element) {
          return (strcmp(element.first.c_str(), oldFieldName) == 0);
        });
    if (it != renamedFields.end()) {
      continue;
    }

    /* Look for this column in new table */
    bool found = false;
    for (size_t j = 0; j < alteredTable->s->fields; j++) {
      Field *newTableField = alteredTable->field[j];

      /* Skip virtual column from altered table */
      if (skipVirtual && IsVirtualGeneratedField(newTableField)) {
        continue;
      }

      const char *newFieldName = newTableField->field_name;

      if (strcmp(oldFieldName, newFieldName) == 0) {
        /* This column is present in both the tables. Stop iteration. */

        /* Check if this column is in drop list of alter_info. */
        if (IsFieldToDrop(*haAlterInfo, oldFieldName)) {
          /* This column is being dropped */
          colsToDrop.push_back(oldTableField);

          /* But this column is present in new table as well which is possible
          if a new column with same name is being added or an existing column
          is being renamed to this name. */
          auto it = std::find_if(
              renamedFields.begin(), renamedFields.end(),
              [&newFieldName](std::pair<std::string, std::string> &element) {
                return (strcmp(element.second.c_str(), newFieldName) == 0);
              });
          if (it == renamedFields.end()) {
            /* Not renamed, so must be being added */
            colsToAdd.push_back(newTableField);
          }
        }

        found = true;
        break;
      }
    }

    /* Could not find this column in new table. So it is being dropped. */
    if (!found) {
      colsToDrop.push_back(oldTableField);
    }
  } /* For */

  for (size_t i = 0; i < alteredTable->s->fields; i++) {
    Field *newTableField = alteredTable->field[i];

    /* Skip virtual column from altered table */
    if (skipVirtual && IsVirtualGeneratedField(newTableField)) {
      continue;
    }

    const char *newFieldName = newTableField->field_name;
    /* Skip if it is renamed field */
    auto it = std::find_if(
        renamedFields.begin(), renamedFields.end(),
        [&newFieldName](std::pair<std::string, std::string> &element) {
          return (strcmp(element.second.c_str(), newFieldName) == 0);
        });
    if (it != renamedFields.end()) {
      continue;
    }

    /* Look for this column in old table */
    bool found = false;
    for (size_t j = 0; j < oldTable->s->fields; j++) {
      Field *oldTableField = oldTable->field[j];

      /* Skip virtual column from old table */
      if (skipVirtual && IsVirtualGeneratedField(oldTableField)) {
        continue;
      }

      const char *oldFieldName = oldTableField->field_name;

      /* This column is present in both the tables. Stop iteration. */
      if (strcmp(oldFieldName, newFieldName) == 0) {
        /* If the old column is renamed, then this new column is being added */
        auto it = std::find_if(
            renamedFields.begin(), renamedFields.end(),
            [&oldFieldName](std::pair<std::string, std::string> &element) {
              return (strcmp(element.first.c_str(), oldFieldName) == 0);
            });
        if (it != renamedFields.end()) {
          colsToAdd.push_back(newTableField);
        }

        found = true;
        break;
      }
    }

    /* Could not find this column in old table. So it is being added. */
    if (!found) {
      colsToAdd.push_back(newTableField);
    }
  }
}

template <typename Table>
bool Instant_ddl_ctx<Table>::CommitInstantDropCol() {
  for (const auto &column : m_colsToDrop) {
    /* Get column to be dropped from old table def */
    dd::Column *colToDrop =
        const_cast<dd::Column *>(DDFindColumn(old_dd, column->field_name));
    CDE_ASSERT_DEBUG(colToDrop != nullptr);

    dd::Properties &private_data = colToDrop->se_private_data();
    uint32_t phy_pos = ~0;
    const char *s = ddColumnKeyStrings[DD_INSTANT_PHYSICAL_POS];
    if (!private_data.exists(s)) {
      const DictCol *col = dict_table->get_col_by_name(column->field_name);
      phy_pos = col->m_phyPos;
    } else {
      private_data.get(s, &phy_pos);
    }

    std::string droppedColName(colToDrop->name().c_str());
    BuildDroppedColumnName(droppedColName, dict_table->m_currentRowVersion + 1,
                           phy_pos);

    dd::Column *droppedCol =
        DDCopyHiddenColumn(new_dd, colToDrop, droppedColName.c_str());
    if (droppedCol == nullptr) {
      /* Table already has column with name same as dropped_col_name */
      CDE_LOG_INFO(
          "Column name %s and internally generated INSTANT DROP column name %s "
          "is causing a conflict",
          droppedColName.c_str(), droppedColName.c_str());
      return false;
    }
    CDE_ASSERT_DEBUG(droppedCol != nullptr);

    {
      /* Set metadata of dropped column */
      dd::Properties &privateData = droppedCol->se_private_data();
      if (DDColumnIsInstantAdded(colToDrop)) {
        uint32_t v_added = DDColumnGetVersionAdded(colToDrop);
        privateData.set(ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED], v_added);
      }
      privateData.set(ddColumnKeyStrings[DD_INSTANT_VERSION_DROPPED],
                      dict_table->m_currentRowVersion + 1);
      privateData.set(ddColumnKeyStrings[DD_INSTANT_PHYSICAL_POS], phy_pos);

      privateData.set(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT_NULL], true);

      /* For instant added column, remove the default value if exists. */
      privateData.remove(ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT]);
    }
  }

  InstantUpdateTableColsCount(dict_table, 0, m_colsToDrop.size());
  return true;
}

template <typename Table>
void Instant_ddl_ctx<Table>::CommitInstantAddCol() {
  uint32_t cols_added = 0;
  for (const auto newField : m_colsToAdd) {
    CDE_ASSERT_DEBUG(!IsVirtualGeneratedField(newField));
    CDE_ASSERT_DEBUG(newField->charset() == nullptr ||
                     newField->charset()->number > 0);

    auto col =
        const_cast<dd::Column *>(DDFindColumn(new_dd, newField->field_name));
    CDE_ASSERT_DEBUG(col != nullptr);

    dd::Properties &sePrivate = col->se_private_data();
    /* Set Version Added */
    sePrivate.set(ddColumnKeyStrings[DD_INSTANT_VERSION_ADDED],
                  dict_table->m_currentRowVersion + 1);

    cols_added++;
    sePrivate.set(ddColumnKeyStrings[DD_INSTANT_PHYSICAL_POS],
                  dict_table->m_maxPos + cols_added);

    /* Set Default values */
    SetColumnDefaultVal(sePrivate, *newField);
  }
}

/**
  @brief Commit instant ddl
  @retval true  Success
  @retval false Failure
 */
template <typename Table>
bool Instant_ddl_ctx<Table>::commit() {
  auto inst_type =
      static_cast<dstore_instant_type>(ha_alter_info->handler_trivial_ctx);
  switch (inst_type) {
    case dstore_instant_type::INSTANT_RENAME_COLUMN:
    case dstore_instant_type::INSTANT_VARCHAR_RESIZE:
    case dstore_instant_type::INSTANT_RENAME_COLUMN_WITH_VARCHAR_RESIZE:
    case dstore_instant_type::INSTANT_VIRTUAL_ONLY:
      if (!dd_commit_inplace_no_change()) {
        return false;
      }
      SetDiscardAfterDdl(dict_table, altered_table);
      return true;
    case dstore_instant_type::INSTANT_NO_CHANGE:
      return dd_commit_inplace_no_change();
    case dstore_instant_type::INSTANT_ADD_DROP_COLUMN:
      CdeDdTableCopyPrivate(*new_dd, *old_dd);
      DDCopyColumnPrivate(*ha_alter_info, *old_dd, *new_dd, dict_table);
      PopulateToBeInstantColumns(ha_alter_info, old_table, altered_table, true,
                                 m_colsToAdd, m_colsToDrop);
      CDE_ASSERT(!m_colsToDrop.empty() || !m_colsToAdd.empty());
      if (TableHasDroppedColumn(*old_dd)) {
        if (!DDCopyDroppedColumns(*old_dd, *new_dd)) {
          return false;
        }
      }

      if (!m_colsToDrop.empty()) {
        if (!CommitInstantDropCol()) return false;
      }

      if (!m_colsToAdd.empty()) {
        CommitInstantAddCol();
      }

      dict_table->m_currentRowVersion++;
      SetDiscardAfterDdl(dict_table, altered_table);

      UpdateDictTableAndIndexCsn(dict_table, new_dd);
      return true;
    default:
      return false;
  }
}

/** Update table level instant metadata in commit phase of INPLACE ALTER
@param[in]      table           dstore table object
@param[in]      oldDDTab        old dd::Table
@param[in]      newDDTab        new dd::Table */
static void DDCommitInplaceUpdateInstantMeta(const cde_dict_t *table,
                                             const dd::Table *oldDDTab,
                                             dd::Table *newDDTab) {
  if (!DDTableHasRowVersions(*oldDDTab)) {
    return;
  }

  /* Copy instant default values of columns if exists */
  for (uint32_t i = 0; i < table->m_totalColCount; ++i) {
    const DictCol *col = table->m_dictCols + i;

    if ((!col->m_isVisible) || (col->m_versionAdded == 0)) {
      continue;
    }

    dd::Column *ddCol = const_cast<dd::Column *>(
        DDFindColumn(newDDTab, table->col_names[i].c_str()));
    CDE_ASSERT_DEBUG(ddCol != nullptr);

    DDwriteDefaultValue(col, ddCol);
  }
}

/** Copy metadata of dd::Table and dd::Columns from old table to new table.
This is done during inplce alter table when table is not rebuilt.
@param[in]      haAlterInfo   inplace alter info
@param[in]      dictTable     cde dict table
@param[in]      oldDDTab      old table definition
@param[in,out]  newDDTab      new table definition */
static void DDInplaceAlterCopyInstantMetadata(
    const Alter_inplace_info *haAlterInfo, cde_dict_t *dictTable,
    const dd::Table *oldDDTab, dd::Table *newDDTab) {
  if (!DDTableHasRowVersions(*oldDDTab)) {
    return;
  }

  /* Copy col phy pos from old DD table to new DD table */
  DDCopyColumnPrivate(*haAlterInfo, *oldDDTab, *newDDTab, dictTable);

  /* Add INSTANT dropped column from oldDDTab to newDDTab */
  if (TableHasDroppedColumn(*oldDDTab)) {
    if (!DDCopyDroppedColumns(*oldDDTab, *newDDTab)) {
      CDE_ASSERT(false);
    }
  }
}

/** Update metadata in commit phase. Note this function should only update
the metadata which would not result in failure
@param[in,out]  ddTable      New dd::Table or dd::Partition
@param[in]      dictTable    cde dict table */
template <typename Table>
static void DDCommitInplaceAlterTable(Table *ddTable, cde_dict_t *dictTable) {
  // step 1. Update the dd:table private data
  CdeDdTableSetSePrivateData(ddTable, dictTable->dstore_relation, dictTable);

  // step 2. Update the dd::index private data
  CdeDdIndexSetSePrivateData(ddTable, dictTable);
}

bool ha_cde::CdeInplaceRebuildTable(TABLE *alteredTable,
                                    Alter_inplace_info *haAlterInfo) {
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);
  MEM_ROOT rebuildMemRoot(PSI_NOT_INSTRUMENTED, 512);
  DSTORE::StorageRelation oldTableCopyRelation = nullptr;
  DSTORE::StorageRelation newTableCopyRelation = nullptr;
  auto autoDestroyClonedRels = create_scope_guard([&]() {
    if (newTableCopyRelation != nullptr) {
      newTableCopyRelation->Destroy();
      newTableCopyRelation = nullptr;
    }
    if (oldTableCopyRelation != nullptr) {
      oldTableCopyRelation->Destroy();
      oldTableCopyRelation = nullptr;
    }
  });

  auto ret =
      cde_relation::Clone(ctx->old_table->dstore_relation, &rebuildMemRoot,
                          ctx->old_table->fillfactor, oldTableCopyRelation);
  if (ret != CDE_OK) return false;
  ret = cde_relation::Clone(ctx->new_table->dstore_relation, &rebuildMemRoot,
                            ctx->new_table->fillfactor, newTableCopyRelation);
  if (ret != CDE_OK) return false;

  /*Since g_guc.maintenanceWorkMem is in KB,
  it needs to be multiplied by 1024 to be converted to bytes when using this
  parameter.*/
  auto maxBuffSize = g_guc.maintenanceWorkMem * 1024;
  THD *thd = ha_thd();
  HeapBuilder heapBuilder(table, alteredTable, ctx->old_table, ctx->new_table,
                          oldTableCopyRelation, newTableCopyRelation,
                          ctx->colMap, ctx->colNotNull, ctx->addedFieldsInfos,
                          ctx->addAutoinc, maxBuffSize, ThdDdlThreads(thd),
                          ctx->sequence, ThdEnableBatchInsertForRebuild(thd));
  int buildHeapResult = heapBuilder.BuildHeap();
  if (buildHeapResult != CDE_OK) {
    ReportInplaceAlterError(buildHeapResult, thd);
    return true;
  }

  /* Since we insert data into the new heap by starting new transactions
  when rebuilding the heap, we need to get the latest snapshot so
  that the data can be scanned from the new heap when building the indexes. */
  TransactionInterface::SetTransactionSnapshotCsn(
      TransactionInterface::GetLatestSnapshotCsn());
  TransactionInterface::SetTransactionSnapshotCid(
      TransactionInterface::GetTransactionSnapshotCid());

  if (CdeInplaceBuildIndexes(alteredTable, haAlterInfo)) {
    return true;
  }

  return false;
}

/** Implementation of inplace_alter_table()
@tparam               Table           dd::Table or dd::Partition
@param[in]            alteredTable    TABLE object for new version of table.
@param[in,out]        haAlterInfo     Structure describing changes to be done
                                      by ALTER TABLE and holding data used
                                      during in-place alter.
the table. Can be adjusted by this call. Changes to the table definition will
be persisted in the data-dictionary at statement commit time.
@retval true Failure
@retval false Success
*/
template <typename Table>
bool ha_cde::InplaceAlterTableImpl(TABLE *alteredTable,
                                   Alter_inplace_info *haAlterInfo) {
  CDE_ASSERT_DEBUG(alteredTable != nullptr);
  // The DSTORE_INPLACE_IGNORE operation will be recognized as INSTANT_NO_CHANGE
  // and ignored in this branch.
  if (CdeIsInstantAlter(haAlterInfo)) {
    return false;
  }

  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);
  if (ctx->needRebuild) {
    return CdeInplaceRebuildTable(alteredTable, haAlterInfo);
  }

  // no rebuild
  if (((haAlterInfo->handler_flags & ~DSTORE_INPLACE_IGNORE) ==
       Alter_inplace_info::CHANGE_CREATE_OPTION)) {
    CDE_ASSERT(!CdeNeedRebuild(haAlterInfo, table, alteredTable));
    return false;
  }

  if (haAlterInfo->handler_flags & DSTORE_ONLINE_CREATE) {
    return CdeInplaceBuildIndexes(alteredTable, haAlterInfo);
  }

  if (haAlterInfo->handler_flags & DSTORE_ALTER_NOREBUILD) {
    return false;
  }

  return true;
}

/** Add or drop foreign key constraints to the data dictionary tables,
but do not touch the data dictionary cache.
@param ctx In-place ALTER TABLE context
@retval true Success
@retval false Failure
*/
[[nodiscard]] static bool CdeUpdateForeignTry(ha_cde_inplace_ctx *ctx) {
  for (uint32_t i = 0; i < ctx->num_to_add_fk; i++) {
    cde_dict_foreign_t *fk = ctx->add_fk[i];

    CDE_ASSERT(fk->foreign_table == ctx->new_table ||
               fk->foreign_table == ctx->old_table);

    if (!fk->foreign_index) {
      fk->foreign_index = CdeForeignKeyFindIndex(
          ctx->new_table, ctx->col_names, fk->foreign_col_names, fk->col_nums,
          fk->referenced_index, true,
          fk->fk_rule & (CDE_DDL_FK_RULE_ON_DELETE_SET_NULL |
                         CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL));
      if (!fk->foreign_index) {
        my_error(ER_FK_INCORRECT_OPTION, MYF(0),
                 fk->foreign_table->name.c_str(), fk->unique_name);
        return false;
      }
    }
  }

  DBUG_EXECUTE_IF("dstore_test_cannot_add_fk_system", return false;);
  return true;
}

/** Update the foreign key constraint definitions in the data dictionary cache
after the changes to data dictionary tables were committed. */
static void CdeUpdateForeignCache(ha_cde_inplace_ctx *ctx) {
  if (ctx->num_to_drop_fk || ctx->num_to_add_fk) {
    for (uint32_t i = 0; i < ctx->num_to_drop_fk; i++) {
      cde_dict_foreign_t *foreign = ctx->drop_fk[i];
      if (foreign->referenced_table != nullptr) {
        foreign->referenced_table->referenced_dict_set.erase(foreign);
      }
      if (foreign->foreign_table != nullptr) {
        foreign->foreign_table->foreign_dict_set.erase(foreign);
      }

      delete foreign;
      foreign = nullptr;
    }

    for (uint32_t i = 0; i < ctx->num_to_add_fk; i++) {
      cde_dict_foreign_t *foreign = ctx->add_fk[i];
      if (foreign->referenced_table) {
        foreign->referenced_table->referenced_dict_set.insert(foreign);
        DictRemoveFromEvictLRULocked(foreign->referenced_table);
      }

      foreign->foreign_table->foreign_dict_set.insert(foreign);
      DictRemoveFromEvictLRULocked(foreign->foreign_table);
      ctx->add_fk[i] = nullptr;
    }
  }

  // util now will not rollback again, reset this to mark not free it in ctx's
  // destructor
  ctx->num_to_add_fk = 0;
}

/** Rename or enlarge columns in the data dictionary cache
as part of CommitCacheNoRebuild().
@param haAlterInfo Data used during in-place alter.
@param table the TABLE
@param userTable cde table that was being altered */
static void RenameOrEnlargeColumnsCache(Alter_inplace_info *haAlterInfo,
                                        const TABLE *table,
                                        cde_dict_t *userTable) {
  if (!(haAlterInfo->handler_flags &
        (Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH |
         Alter_inplace_info::ALTER_COLUMN_NAME))) {
    return;
  }

  List_iterator_fast<Create_field> cfIt(haAlterInfo->alter_info->create_list);
  uint32_t i = 0;
  for (Field **fp = table->field; *fp; fp++, i++) {
    cfIt.rewind();
    while (const Create_field *cf = cfIt++) {
      if (cf->field != *fp) {
        continue;
      }

      bool isVirtual = IsVirtualGeneratedField(*fp);

      /** If we Change column datatype in such way that new type has compatible
      packed representation with old type, such as extending VARCHAR column
      size. In this case, we need only update cde_dict_field_t. */
      if ((*fp)->is_equal(cf) == IS_EQUAL_PACK_LENGTH && !isVirtual) {
        if ((*fp)->type() == MYSQL_TYPE_STRING ||
            (*fp)->type() == MYSQL_TYPE_VARCHAR) {
          // TODO: Wether we need to calculate the length
          userTable->m_fieldLenInfos[i].m_fieldLenBytes =
              ((*fp)->type() == MYSQL_TYPE_STRING)
                  ? 0
                  : cf->max_display_width_in_bytes() > 255 ? 2 : 1;
          userTable->m_fieldLenInfos[i].m_packLength = cf->pack_length();
          userTable->m_fieldLenInfos[i].m_mbmaxlen = cf->charset->mbmaxlen;
          userTable->m_fieldLenInfos[i].m_mbminlen = cf->charset->mbminlen;
        }
      }

      /** If we alter column name, we need to update dict sys. */
      if ((*fp)->is_flag_set(FIELD_IS_RENAMED)) {
        userTable->RenameDictColumn(i, cf->field->field_name, cf->field_name,
                                    isVirtual);
      }
    }
  }
}

/** Adjust the persistent statistics after non-rebuilding ALTER TABLE.
Remove statistics for dropped indexes, add statistics for created indexes
and rename statistics for renamed indexes.
@param haAlterInfo  Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param dictTable   Table name in MySQL
@param thd MySQL connection
*/
static void AlterStatsNoRebuild(Alter_inplace_info *haAlterInfo,
                                ha_cde_inplace_ctx *ctx, cde_dict_t *dictTable,
                                THD *thd) {
  if (dictTable != nullptr) {
    dictTable->UnMarkBgStatQuit();

    if (dictTable->auto_recalc_enabled()) {
      TrxTable tmpTrxTable{
          dictTable->m_id, dictTable->name, dictTable->stat_n_rows,
          dictTable->stat_modified_counter, dictTable->auto_recalc_enabled()};
      CdeAddStatTable(tmpTrxTable, true);
    }
  }

  if (!ctx->new_table->dict_stats_persist_enabled()) {
    return;
  }

  int ret = CDE_SUCC;
  /* For indexes that have been dropped, delete the corresponding records in the
   * dstore_index_stats table. */
  for (uint32_t i = 0; i < haAlterInfo->index_drop_count; i++) {
    const KEY *key = haAlterInfo->index_drop_buffer[i];
    std::string indexName(key->name);
    ctx->new_table->dict_stat_wr_lock();
    ret = DictStatDropIndex(thd, ctx->new_table, indexName);
    ctx->new_table->dict_stat_unlock();
    if (ret != CDE_SUCC) {
      std::ostringstream oss;
      oss << "Unable to delete statistics index " << key->name
          << " from mysql.dstore_index_stats table.";
      push_warning(thd, Sql_condition::SL_WARNING, ER_LOCK_WAIT_TIMEOUT,
                   oss.str().c_str());
    }
  }

  /* For indexes that have been renamed, update the corresponding records in the
   * dstore_index_stats table. */
  for (uint32_t i = 0; i < haAlterInfo->index_rename_count; i++) {
    KEY_PAIR *pair = &haAlterInfo->index_rename_buffer[i];
    std::string oldIndexName(pair->old_key->name);
    std::string newIndexName(pair->new_key->name);
    ctx->new_table->dict_stat_wr_lock();
    ret = DictStatRenameIndex(thd, ctx->new_table->name, oldIndexName,
                              newIndexName);
    ctx->new_table->dict_stat_unlock();
    if (ret != CDE_SUCC) {
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_ERROR_ON_RENAME,
          "Error renaming an index of table '%s'"
          " from '%s' to '%s' in dstore persistent statistics storage",
          dictTable->name.c_str(), pair->old_key->name, pair->new_key->name);
    }
  }

  /* For newly added indexes, calculate statistics and persist them in the
   * dstore_index_stats table. */
  if (dictTable != nullptr && dictTable->auto_recalc_enabled()) {
    ctx->new_table->stat_initialized = false;
    CdeDictTableStatsInit(ctx->new_table, thd);
    for (uint32_t i = 0; i < ctx->num_to_add_index; i++) {
      cde_dict_index_t *index = ctx->add_index[i];
      DstoreDictStatsUpdateForIndex(ctx->new_table, index, thd);
    }
  }
}

/**
Rename all indexes in data dictionary cache of a given table that are
specified in haAlterInfo.

@param ctx alter context, used to fetch the list of indexes to rename
@param haAlterInfo fetch the new names from here
*/
static void RenameIndexesInCache(const ha_cde_inplace_ctx *ctx,
                                 const Alter_inplace_info *haAlterInfo) {
  CDE_ASSERT_DEBUG(ctx->num_to_rename == haAlterInfo->index_rename_count);
  for (uint32_t i = 0; i < ctx->num_to_rename; i++) {
    KEY_PAIR *pair = &haAlterInfo->index_rename_buffer[i];

    auto index = ctx->rename[i];

    CDE_ASSERT_DEBUG(strcmp(index->name.c_str(), pair->old_key->name) == 0);
    ctx->new_table->RenameDictIndex(index, pair->new_key->name);
  }
}

/**
Drop all indexes in data dictionary cache of a given table that are
specified in haAlterInfo.

@param ctx alter context, used to fetch the list of indexes to drop
@param thd the current THD used to write ddl log
*/
static bool DropIndexesInCache(const ha_cde_inplace_ctx *ctx, THD *thd) {
  /* Step 1. if the index to be dropped is referenced by foreign key, we need to
  replace the foreign key to another index. */
  for (uint32_t i = 0; i < ctx->num_to_drop_index; i++) {
    cde_dict_index_t *index = ctx->drop_index[i];
    if (!index->table->ReplaceDictForeignIndex(ctx->col_names, index)) {
      CDE_ASSERT(!(CdeGetTrxinfo(thd)->check_foreigns));
    }
  }

  /* Step 2. Drop indexes in data dictionary cache and write DDL log for them */
  for (uint32_t i = 0; i < ctx->num_to_drop_index; i++) {
    cde_dict_index_t *index = ctx->drop_index[i];
    DSTORE::SysClassTupDef *tupDef = index->rel->rel;
    CDE_ASSERT(tupDef != nullptr);
    DSTORE::PageId idxSegmentId = {tupDef->relfileid, tupDef->relblknum};
    CDE_ASSERT(!idxSegmentId.IsInvalid());
    auto ret = CdeDdlLog::GetInstance()->LogDropSegment(
        thd, idxSegmentId, DSTORE::SegmentType::INDEX_SEGMENT_TYPE,
        index->table->m_spaceId, false);
    if (ret == CDE_FAIL) {
      CDE_LOG_ERROR(
          "Drop index: %s in table: %s fail due to LogDropSegment fail.",
          index->name.c_str(), ctx->new_table->name.c_str());
      CDE_ASSERT(0);
    }

    auto it = std::find(ctx->new_table->index_dict_vec.begin(),
                        ctx->new_table->index_dict_vec.end(), index);
    CDE_ASSERT(it != ctx->new_table->index_dict_vec.end());

    /* update the table->stat_all_indexs_page_count */
    ctx->new_table->dict_stat_wr_lock();
    ctx->new_table->stat_all_indexs_page_count -= (*it)->stat_index_page_cnt;
    ctx->new_table->dict_stat_unlock();
    (*it)->destroy_from_cache();
    delete (*it);
    ctx->new_table->index_dict_vec.erase(it);
  }

  return false;
}

/** Parse hint for table and its indexes, and update the information
in dictionary.
@param[in]      thd             Connection thread
@param[in,out]  table           Target table
@param[in]      tableShare      Table definition */
void ParseHintFromComment(THD *thd, cde_dict_t *table,
                          const TABLE_SHARE *tableShare) {
  int32_t tableFillFactor = 0;
  std::vector<int32_t> indexFillFactor(MAX_KEY);
  std::vector<bool> isFound(MAX_KEY, false);
  if (tableShare->comment.str != nullptr) {
    tableFillFactor = DstoreParseFillfactor(tableShare->comment.str, thd);
  } else {
    tableFillFactor = g_defaultFillfactor;
  }

  if (tableFillFactor == 0) {
    tableFillFactor = g_defaultFillfactor;
  }

  for (uint32_t i = 0; i < tableShare->keys; i++) {
    KEY *keyInfo = &tableShare->key_info[i];

    if (keyInfo->flags & HA_USES_COMMENT && keyInfo->comment.str != nullptr) {
      indexFillFactor[i] = DstoreParseFillfactor(keyInfo->comment.str, thd);
    } else {
      indexFillFactor[i] = tableFillFactor;
    }

    if (indexFillFactor[i] == 0) {
      indexFillFactor[i] = tableFillFactor;
    }
  }

  for (auto index : table->index_dict_vec) {
    if (!index->IsCommitted()) {
      continue;
    }

    for (uint32_t i = 0; i < tableShare->keys; i++) {
      if (isFound[i]) {
        continue;
      }

      KEY *keyInfo = &tableShare->key_info[i];
      if (my_strcasecmp(system_charset_info, index->name.c_str(),
                        keyInfo->name) == 0) {
        // TODO: Wether need to lock index.
        index->fillfactor = indexFillFactor[i];
        isFound[i] = true;
        break;
      }
    }
  }
}

/** Commit the changes to the data dictionary cache
after a successful CommitTryNorebuild() call.
@param ctx In-place ALTER TABLE context
@param haAlterInfo the altered info
@param thd the current THD
@return whether all replacements were found for dropped indexes */
[[nodiscard]] inline bool CommitCacheNoRebuild(
    ha_cde_inplace_ctx *ctx, const Alter_inplace_info *haAlterInfo, THD *thd) {
  for (uint32_t i = 0; i < ctx->num_to_add_index; i++) {
    cde_dict_index_t *index = ctx->add_index[i];
    index->SetCommitted(true);
  }

  if (DropIndexesInCache(ctx, thd)) {
    return true;
  }

  RenameIndexesInCache(ctx, haAlterInfo);

  return false;
}

/** Roll back the changes made during prepare_inplace_alter_table()
and inplace_alter_table() inside the storage engine. Note that the
allowed level of concurrency during this operation will be the same as
for inplace_alter_table() and thus might be higher than during
prepare_inplace_alter_table(). (E.g concurrent writes were blocked
during prepare, but might not be during commit).

@param[in]      haAlterInfo   Data used during in-place alter.
@retval true Failure
@retval false Success
*/
[[nodiscard]] inline bool RollbackInplaceAlterTable(
    const Alter_inplace_info *haAlterInfo) {
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);

  if (ctx == nullptr) {
    return CDE_SUCC;
  }

  if (ctx->needRebuild) {
    if (ctx->new_table) {
      ctx->new_table->destroy_from_cache(false);
      delete ctx->new_table;
      ctx->new_table = nullptr;
    }
    return CDE_SUCC;
  }

  DEBUG_SYNC_C("rollback_inplace_alter_before");

  DictSysLockGuard guard;

  CDE_ASSERT_DEBUG(ctx->new_table->getRefCnt() >= 1);

  DEBUG_SYNC_C("rollback_inplace_alter_end");
  OnlineDdlMgr *onlineDdlMgr = ctx->old_table->m_onlineDdlMgr;
  if (haAlterInfo->online && onlineDdlMgr != nullptr) {
    CdeRwlockGuard onlineMgrGuard(&ctx->old_table->m_onlineDdlMgrLock,
                                  CdeRwlockOp::WR_LOCK);
    onlineDdlMgr->SetStatus(OnlineDdlStatus::ABORTED);
    onlineDdlMgr->NotifyCommitOrRollback();
    onlineDdlMgr->WaitAllRowLogReplayThreadsExit();
    ctx->old_table->DestroyOnlineDdlMgr();
  }

  if (ctx->new_table->getRefCnt() > 1) {
    ctx->new_table->MarkSecondaryIndexes();
  } else {
    ctx->new_table->DropSecondaryIndexes();
  }

  /* Clear the to_be_dropped flags in the data dictionary cache.
  The flags may already have been cleared, in case an error was detected in
  commit_inplace_alter_table(). */
  for (uint32_t i = 0; i < ctx->num_to_drop_index; i++) {
    cde_dict_index_t *index = ctx->drop_index[i];
    index->toBeDropped = false;
  }

  DEBUG_SYNC_C("rollback_inplace_alter_end");

  return CDE_SUCC;
}

/** Implementation of commit_inplace_alter_table()
@tparam               Table           dd::Table or dd::Partition
@param[in]            alteredTable    TABLE object for new version of table.
@param[in,out]        haAlterInfo     Structure describing changes to be done
                                      by ALTER TABLE and holding data used
                                      during in-place alter.
@param[in,out]        newDDTab        Table object for the new version of the
                                      table. Can be adjusted by this call.
                                      Changes to the table definition
                                      will be persisted in the data-dictionary
                                      at statement version of it.
@retval               true Failure
@retval               false Success */
template <typename Table>
bool ha_cde::CommitInplaceAlterTableImpl(TABLE *alteredTable,
                                         Alter_inplace_info *haAlterInfo,
                                         Table *newDDTab) {
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);
  CDE_ASSERT_DEBUG(ctx != nullptr);

  auto thd = ha_thd();
  CdeTrxStartIfNotStarted(thd, true);

  OnlineDdlMgr *onlineDdlMgr = ctx->old_table->m_onlineDdlMgr;
  if (haAlterInfo->online && onlineDdlMgr) {
    CdeRwlockGuard onlineMgrGuard(&ctx->old_table->m_onlineDdlMgrLock,
                                  CdeRwlockOp::WR_LOCK);
    onlineDdlMgr->SetStatus(OnlineDdlStatus::COMMITTING);
    onlineDdlMgr->NotifyCommitOrRollback();
    onlineDdlMgr->WaitAllRowLogReplayThreadsExit();
    if (onlineDdlMgr->NeedAbort()) {
      /* Apply last remaining row logs failed at commit stage. */
      onlineDdlMgr->ReportErrorInfo(alteredTable, m_dstore, haAlterInfo);
      ctx->old_table->DestroyOnlineDdlMgr();
      return CDE_FAIL;
    }
    ctx->old_table->DestroyOnlineDdlMgr();
  }

  /* For Innodb, the table lock is use for prevent FOREIGN KEY constraints
     checks and any transactions collected during crash recovery could be
     holding InnoDB locks only while we change the table definition. May be
     there is no need for us to lock table for dstore. */
  if (CdeLockTable(LOCK_X, ctx->old_table->dstore_relation) !=
      DSTORE::DSTORE_SUCC) {
    auto err = GetAndConvertDstoreErrcodeToMysql();
    CDE_ASSERT(err != DSTORE::DSTORE_SUCC);
    ReportInplaceAlterError(err);
    return true;
  }

  // Prevent the background statistics collection from accessing the tables
  ctx->new_table->WaitIfHoldByBgThread();

  CommitGetAutoinc(haAlterInfo, alteredTable, table);

  if (!CdeUpdateForeignTry(ctx)) {
    return true;
  }

  OperationalLockGuard opWGuard;
  DictSysLockGuard guard;

  // update persist stat flag
  CdeSetTableFlagsFromTableShare(ctx->new_table, alteredTable->s);

  cde_session_t *session = CdeGetSession(thd, false);
  CdeMutexGuard mutexGuard(&session->m_mutex, CDE_LOCATION_HERE);
  if (session->m_setInterrupt) {
    // session is mark killed, inplace canceled and no need go further
    return true;
  }

  CdeUpdateForeignCache(ctx);

  bool ret = CommitCacheNoRebuild(ctx, haAlterInfo, thd);
  if (ret) {
    // TODO implement innobase_rollback_sec_index
    auto err = GetAndConvertDstoreErrcodeToMysql();
    CDE_ASSERT(err != DSTORE::DSTORE_SUCC);
    ReportInplaceAlterError(err);
    return true;
  }

  // TODO guard's scope need confirm
  mutexGuard.Clear();

  // Handling scenarios such as alter table rename column and extend varchar
  // size
  RenameOrEnlargeColumnsCache(haAlterInfo, table, ctx->new_table);

  // update autoincrement values
  if (alteredTable->found_next_number_field != nullptr) {
    ctx->new_table->autoinc_dict.MutexEnter();
    ctx->new_table->autoinc_dict.SetAutoinc(ctx->maxAutoinc);
    ctx->new_table->autoinc_dict.SetInitialized(true);
    ctx->new_table->autoinc_dict.MutexExit();
    CdeDdTableSetSePrivateDataAutoInc(newDDTab, ctx->maxAutoinc);
  }

  AlterStatsNoRebuild(haAlterInfo, ctx, ctx->old_table, thd);

  /* TODO: wether need to Invalidate the index translation table. */

  ParseHintFromComment(thd, ctx->new_table, alteredTable->s);

  if (haAlterInfo->virtual_column_drop_count ||
      haAlterInfo->virtual_column_add_count) {
    ctx->old_table->m_discardAfterDDL = true;
  }

  return false;
}

/**
Discard the foreign key info which contains the renamed columns.

@param ha_alter_info    Data used during in-place alter
@param mysqlTable       MySQL table that is being altered
@param oldTable         Dstore table as it is before the ALTER operation
*/
static void CdeRenameColDiscardForeign(Alter_inplace_info *haAlterInfo,
                                       const TABLE *mysqlTable,
                                       cde_dict_t *oldTable) {
  List_iterator_fast<Create_field> cf_it(haAlterInfo->alter_info->create_list);

  CDE_ASSERT_DEBUG(haAlterInfo->handler_flags &
                   Alter_inplace_info::ALTER_COLUMN_NAME);

  for (Field **fp = mysqlTable->field; *fp; fp++) {
    if (!(*fp)->is_flag_set(FIELD_IS_RENAMED)) {
      continue;
    }

    cf_it.rewind();

    while (Create_field *cf = cf_it++) {
      if (cf->field != *fp) {
        continue;
      }

      /* Now cf->field->field_name is the old name, check the foreign key
      information to see any one gets affected by this rename, and discard
      them from cache */

      std::set<cde_dict_foreign_t *> fkEvict;

      for (auto fk : oldTable->foreign_dict_set) {
        cde_dict_foreign_t *foreign = fk;

        for (unsigned i = 0; i < foreign->col_nums; i++) {
          if (strcmp(foreign->foreign_col_names[i], cf->field->field_name) !=
              0) {
            continue;
          }

          fkEvict.insert(foreign);
          break;
        }
      }

      for (auto fk : oldTable->referenced_dict_set) {
        cde_dict_foreign_t *foreign = fk;

        for (unsigned i = 0; i < foreign->col_nums; i++) {
          if (strcmp(foreign->referenced_col_names[i], cf->field->field_name) !=
              0) {
            continue;
          }

          fkEvict.insert(foreign);
          break;
        }
      }

      std::for_each(fkEvict.begin(), fkEvict.end(), DictForeignRemoveFromCache);
    }
  }
}
/** Implementation of commit_inplace_alter_table()
@tparam               Table           dd::Table or dd::Partition
@param[in]            alteredTable    TABLE object for new version of table.
@param[in,out]        haAlterInfo     Structure describing changes to be done
                                      by ALTER TABLE and holding data used
                                      during in-place alter.
@param[in,out]        newDDTab        Table object for the new version of the
                                      table. Can be adjusted by this call.
                                      Changes to the table definition
                                      will be persisted in the data-dictionary
                                      at statement version of it.
@retval               true Failure
@retval               false Success */
template <typename Table>
bool ha_cde::CommitInplaceAlterRebuildTableImpl(TABLE *alteredTable,
                                                Alter_inplace_info *haAlterInfo,
                                                const Table *oldDDTab,
                                                Table *newDDTab) {
  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(haAlterInfo->handler_ctx);
  CDE_ASSERT_DEBUG(ctx != nullptr);

  auto thd = ha_thd();
  CdeTrxStartIfNotStarted(thd, true);

  cde_session_t *session = CdeGetSession(thd, false);
  CdeMutexGuard mutexGuard(&session->m_mutex, CDE_LOCATION_HERE);
  if (session->m_setInterrupt) {
    // session is mark killed, inplace canceled and no need go further
    return true;
  }

  // Prevent the background statistics collection from accessing the tables
  ctx->old_table->WaitIfHoldByBgThread();

  CommitGetAutoinc(haAlterInfo, alteredTable, table);
  // update autoincrement values
  if (alteredTable->found_next_number_field != nullptr) {
    ctx->new_table->autoinc_dict.MutexEnter();
    ctx->new_table->autoinc_dict.SetAutoinc(ctx->maxAutoinc);
    ctx->new_table->autoinc_dict.SetInitialized(true);
    ctx->new_table->autoinc_dict.MutexExit();
    CdeDdTableSetSePrivateDataAutoInc(newDDTab, ctx->maxAutoinc);
  }

  if (haAlterInfo->handler_flags & Alter_inplace_info::ALTER_COLUMN_NAME) {
    CdeRenameColDiscardForeign(haAlterInfo, table, ctx->old_table);
  }

  ctx->new_table->m_discardAfterDDL = true;
  /* We should copy the old table name before freeing the table to avoid
  the error behavior of the bg evict thread evicting the table cache of
  the old table and the CdeDropTable using the old memory. */
  std::string oldTableName = ctx->old_table->name;
  CDE_ASSERT(ctx->old_table->getRefCnt() == 1);
  if (haAlterInfo->handler_flags & Alter_inplace_info::ALTER_RENAME) {
    ctx->new_table->setOldName(oldTableName.c_str());
  }
  ctx->old_table->release();
  CdeDropTable(thd, oldTableName.c_str(), oldDDTab);
  cde_dict_t *existPtr = nullptr;
  DictSysAddTable(ctx->new_table->name.c_str(), ctx->new_table, &existPtr,
                  true);
  CDE_ASSERT(existPtr == nullptr);
  m_dstore->table_handler = ctx->new_table;

  // update persist stat flag
  CdeSetTableFlagsFromTableShare(ctx->new_table, alteredTable->s);
  /*
  Release the CdeMutexGuard lock here to prevent a type A->B->A deadlock caused
  by the LOCK_thd_data lock and CdeMutexGuard lock when other threads attempt to
  kill the current thread.
  */
  mutexGuard.Clear();
  CdeDictTableStatsUpdate(ctx->new_table, RECALC_PERSIST, thd);

  DDCommitInplaceAlterTable<dd::Table>(newDDTab, ctx->new_table);
  return false;
}

/** Commit or rollback.
Commit or rollback the changes made during
prepare_inplace_alter_table() and inplace_alter_table() inside
the storage engine. Note that the allowed level of concurrency
during this operation will be the same as for
inplace_alter_table() and thus might be higher than during
prepare_inplace_alter_table(). (E.g concurrent writes were
blocked during prepare, but might not be during commit).
@param[in]      altered_table   TABLE object for new version of table.
@param[in,out]  ha_alter_info   Structure describing changes to be done
                                by ALTER TABLE and holding data used during
in-place alter.
@param[in]      commit          true => Commit, false => Rollback.
@param[in]      old_table_def   dd::Table object describing old
version of the table.
@param[in,out]  new_table_def   dd::Table object for the new version
of the table. Can be adjusted by this call. Changes to the table
definition will be persisted in the data-dictionary at statement
commit time.
@retval true    Failure.
@retval false   Success. */
bool ha_cde::commit_inplace_alter_table(TABLE *altered_table,
                                        Alter_inplace_info *ha_alter_info,
                                        bool commit,
                                        const dd::Table *old_dd_tab,
                                        dd::Table *new_dd_tab) {
  DEBUG_SYNC_C("dstore_commit_inplace_alter_table_enter");

  bool ret = false;
  if (!commit) {
    /* A rollback is being requested. So far we may at
    most have created some indexes. If any indexes were to
    be dropped, they would actually be dropped in this
    method if commit=true. */
    ret = RollbackInplaceAlterTable(ha_alter_info);
    return ret;
  }

  // For instant ddl
  if (CdeIsInstantAlter(ha_alter_info)) {
    Instant_ddl_ctx<dd::Table> inst_ddl(ha_alter_info, m_dstore->table_handler,
                                        table, altered_table, old_dd_tab,
                                        new_dd_tab);

    return !inst_ddl.commit();
  }

  ha_cde_inplace_ctx *ctx =
      static_cast<ha_cde_inplace_ctx *>(ha_alter_info->handler_ctx);
  CDE_ASSERT_DEBUG(ctx != nullptr);
  if (ctx->needRebuild) {
    return CommitInplaceAlterRebuildTableImpl(altered_table, ha_alter_info,
                                              old_dd_tab, new_dd_tab);
  }

  ret = CommitInplaceAlterTableImpl(altered_table, ha_alter_info, new_dd_tab);
  if (ret) {
    return true;
  }

  // update DD
  DDInplaceAlterCopyInstantMetadata(ha_alter_info, ctx->new_table, old_dd_tab,
                                    new_dd_tab);

  DDCommitInplaceAlterTable<dd::Table>(new_dd_tab, ctx->new_table);

  DDCommitInplaceUpdateInstantMeta(ctx->new_table, old_dd_tab, new_dd_tab);

  return false;
}
} /* namespace CDE */
