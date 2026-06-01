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

#include <sql_thd_internal_api.h>
#include <stdlib.h>
#include <map>
#include <memory>
#include "securec.h"
// clang-format off
#include "sql/table.h"
#include "sql/sql_show.h"
#include "sql/item.h"
#include "my_icp.h"

#include "errorcode/dstore_tuple_error_code.h"
#include "errorcode/dstore_index_error_code.h"
#include "heap/dstore_heap_interface.h"
#include "heap/dstore_heap_struct.h"
#include "index/dstore_index_interface.h"
#include "systable/dstore_relation.h"
#include "tuple/dstore_tuple_interface.h"
#include "tuple/dstore_memheap_tuple.h"
#include "tuple/dstore_index_tuple.h"
#include "transaction/dstore_transaction_interface.h"

#include "common/cde_alloc.h"
#include "common/cde_def.h"
#include "common/cde_typecache.h"
#include "common/cde_errorcode.h"
#include "dml/cde_dml.h"
#include "dml/cde_heap.h"
#include "dml/cde_btree.h"
#include "dict/cde_dict.h"
#include "common/cde_srv.h"
#include "common/cde_session.h"
#include "my_icp.h"
#include "dict/cde_relation.h"
#include "common/cde_perf.h"
#include "ddl/cde_ddl.h"    // todo: remove dependent on ddl
#include "ddl/cde_dd_table.h"
#include "ddl/cde_online_ddl.h"
#include "ddl/ha_cde_alter.h"

#include "common/cde_trxmgr.h"
#include "scope_guard.h"

// clang-format on
using DSTORE::BtreeInsertAndDeleteCommonData;
using DSTORE::DSTORE_SUCC;
using DSTORE::FmgrInfo;
using DSTORE::FuncCache;
using DSTORE::FunctionCallInfoData;
using DSTORE::HeapDeleteContext;
using DSTORE::HeapTuple;
using DSTORE::HeapUpdateContext;
using DSTORE::INDEX_ERROR_INSERT_UNIQUE_CHECK;
using DSTORE::IndexTuple;
using DSTORE::INVALID_ITEM_POINTER;
using DSTORE::ItemPointer;
using DSTORE::ItemPointerData;
using DSTORE::RetStatus;
using DSTORE::ScanKey;
using DSTORE::StorageRelation;
using DSTORE::TupleDesc;
using DSTORE::TupleDescData;

namespace CDE {

int CdeUpdateRowStep(dml_upd_task *upd_task);
int CdeInsCheckForeignConstraint(cde_dict_foreign_t *foreign, cde_dict_t *table,
                                 dml_ins_ctx *ctx);
void CdeInsForeignReportErr(cde_trxinfo_t *trxinfo,
                            cde_dict_foreign_t *foreign);

/* todo: we should refactoring cde_compare_utils.h,
 * here we temporarily realize datatype comparison
 */
Datum CdeCmpDstoreData(Datum a, Datum b, DSTORE::Form_pg_attribute attr,
                       uint16_t proc) {
  const FuncCache *funcCache =
      CdeGetDstoreFuncCache(attr->atttypid, attr->atttypid, proc);
  if (funcCache->fnAddr == nullptr) {
    funcCache = CdeGetFuncCache(attr->atttypid, attr->atttypid, proc);
  }
  CDE_ASSERT(funcCache->fnAddr != nullptr);
  FunctionCallInfoData fcinfo;
  Datum args[2] = {a, b};
  fcinfo.arg = args;
  FmgrInfo info;
  info.fnExtra = DSTORE::FnExtraMake(attr->atttypid, attr->extAttrs);
  fcinfo.flinfo = &info;
  return funcCache->fnAddr(&fcinfo);
}

int cde_dml_api::insert_heap(StorageRelation heap_rel, Datum *values,
                             bool *is_nulls, ItemPointerData &ctid) {
  HeapTuple *mem_tuple = TupleInterface::FormHeapTuple(
      heap_rel->attr, values, is_nulls, AllocMemZeroForDtuple);
  DBUG_EXECUTE_IF("FormHeapTuple_injection", {
    TupleInterface::DestroyTuple(mem_tuple, CdeFreeMemForDtuple);
    mem_tuple = nullptr;
    static int loop_num = 0;
    if (loop_num == 0) {
      DSTORE::StorageSetErrorCodeOnly(DSTORE::TUPLE_ERROR_NATTRS_EXCEEDS_LIMIT);
    }
    if (loop_num == 1) {
      DSTORE::StorageSetErrorCodeOnly(DSTORE::TUPLE_ERROR_TOO_MANY_COLUMNS);
    }
    loop_num++;
  });
  if (mem_tuple == nullptr) {
    CDE_LOG_ERROR("form dstore heap tuple fail");
    return GetAndConvertDstoreErrcodeToMysql();
  }
  RetStatus ret = HeapInterface::Insert(heap_rel, mem_tuple, ctid,
                                        TransactionInterface::GetCurCid());
  TupleInterface::DestroyTuple(mem_tuple, CdeFreeMemForDtuple);

  if (ret != DSTORE_SUCC) {
    CDE_LOG_ERROR("insert dstore heap tuple fail");
    return GetAndConvertDstoreErrcodeToMysql();
  }
  return CDE_OK;
}

void CdeSetBtreeCtxForInsAndDel(session_index_info *index, Datum *index_values,
                                bool *index_is_nulls, ItemPointer heap_ctid,
                                BtreeInsertAndDeleteCommonData &btree_context) {
  StorageRelation index_rel = index->rel;
  ScanKey scan_key = index->scan_key;
  cde_dict_field_t *cde_fields = index->shared_dict->fields;

  btree_context.indexRel = index_rel;
  btree_context.indexInfo = index_rel->index;
  btree_context.skey = scan_key;
  btree_context.values = index_values;
  btree_context.isnull = index_is_nulls;
  btree_context.heapCtid = heap_ctid;
  btree_context.cid = TransactionInterface::GetCurCid();

  for (int i = 0; i < index_rel->attr->natts; i++) {
    btree_context.skey[i].skArgument = index_values[i];
    btree_context.skey[i].skFunc.fnExtra =
        DSTORE::FnExtraMake(index_rel->attr->attrs[i]->atttypid,
                            index_rel->attr->attrs[i]->extAttrs);
    cde_dict_field_t field = cde_fields[i];
    Oid type_oid = index_rel->index->opcinType[i];
    Oid base_type_oid = CdeConvertDatatypeOid(type_oid);
    if (field.prefix_len > 0 && base_type_oid == CDE_CHAR_OID) {
      // TODO: Currently, the CDE_CHAR_OID type is not effective. This code
      // branch is unreachable. Temporarily skipping the adaptation for passing
      // column attribute information to fnExtra to avoid unnecessary
      // development time. If this scenario is supported in the future, the
      // column information passing mechanism will need to be implemented.
      CDE_ASSERT(0);
      DSTORE::Oid oid = static_cast<DSTORE::Oid>(
          reinterpret_cast<uintptr_t>(btree_context.skey[i].skFunc.fnExtra));
      uint32_t charset_no = (oid >> 16) & MAX_COLLATION_NUM;
      btree_context.skey[i].skFunc.fnExtra = reinterpret_cast<void *>(
          CdeGetCharOid(field.prefix_len) | (charset_no << OID_COLL_SHIFT));
    }
  }
}

int cde_dml_api::insert_index(session_index_info *index, Datum *index_values,
                              bool *index_is_nulls, ItemPointer heap_ctid) {
  BtreeInsertAndDeleteCommonData btree_context;
  CdeSetBtreeCtxForInsAndDel(index, index_values, index_is_nulls, heap_ctid,
                             btree_context);

  RetStatus ret = IndexInterface::Insert(btree_context, false, true);
  DBUG_EXECUTE_IF("insert_index_errorcode_injection",
                  ret = DSTORE::DSTORE_FAIL;);
  if (ret != DSTORE_SUCC) {
    uint32_t errorCode = GetAndConvertDstoreErrcodeToMysql();
    DBUG_EXECUTE_IF("insert_index_errorcode_injection",
                    errorCode = HA_ERR_OUT_OF_MEM;);
    if (errorCode != HA_ERR_FOUND_DUPP_KEY) {
      CDE_LOG_ERROR("insert dstore index tuple fail, error_code is %u",
                    errorCode);
    }
    return errorCode;
  }
  return CDE_OK;
}

int CdeInsCheckForeignConstraints(session_index_info *index, cde_dict_t *table,
                                  dml_ins_ctx *ctx) {
  int ret;
  for (cde_dict_foreign_t *foreign : table->foreign_dict_set) {
    if (foreign->foreign_index != index->shared_dict) {
      continue;
    }

    ret = CdeInsCheckForeignConstraint(foreign, table, ctx);

    if (ret != CDE_OK) {
      return ret;
    }
  }
  return CDE_OK;
}

int CdeDmlWriteRow(const TABLE *table, const unsigned char *mysql_ptr,
                   dml_ins_ctx *ins_ctx) {
  DBUG_EXECUTE_IF("cde_dml_write_row_fault_injection", { return CDE_ERROR; });
  cde_dict_t *table_handler = ins_ctx->get_table();
  session_rel_info *rel_info = ins_ctx->get_rel();
  bool hasIndex = ins_ctx->get_rel()->cde_index_vec.size() != 0;
  /** In case table has virtual columns we need to build a full tuple with
   * all columns - including the virtual ones. We need to insert into index
   * that might have be indexing such a virtual column. */
  bool buildFullTuple = hasIndex && table_handler->m_vColCount > 0;

  int ret =
      MysqlRecordToDstoreForInsert(table, mysql_ptr, ins_ctx->tuple(), ins_ctx,
                                   rel_info->rd_storage_releation->attr, true);
  if (unlikely(ret != CDE_OK)) return ret;

  if (buildFullTuple) {
    CDE_ASSERT_DEBUG(nullptr != table_handler->attrFull);
    ret = MysqlRecordToDstoreForInsert(table, mysql_ptr, ins_ctx->full_tuple(),
                                       ins_ctx, table_handler->attrFull, false);
    if (unlikely(ret != CDE_OK)) return ret;
  } else {
  }

  Datum *values = ins_ctx->tuple()->values;
  bool *is_nulls = ins_ctx->tuple()->is_nulls;
  Datum *index_values = ins_ctx->index_tuple()->index_values;
  bool *index_is_nulls = ins_ctx->index_tuple()->index_is_nulls;
  ItemPointerData &ctid = ins_ctx->tuple()->ctid;
  if (unlikely(ins_ctx->NeedCheckOnlineDdl())) {
    CdeRwlockGuard onlineMgrGuard(&ins_ctx->get_table()->m_onlineDdlMgrLock,
                                  CdeRwlockOp::RD_LOCK);
    OnlineDdlMgr *onlineDdlMgr = ins_ctx->get_table()->m_onlineDdlMgr;
    if (onlineDdlMgr && !onlineDdlMgr->NeedAbort()) {
      CDE_ASSERT(onlineDdlMgr->IsStatusValidForDml());
      rel_info->rd_storage_releation->m_undoExtra = true;
    } else {
      ins_ctx->SetNeedCheckOnlineDdl(false);
    }
  }

  ret = cde_dml_api::insert_heap(rel_info->rd_storage_releation, values,
                                 is_nulls, ctid);
  rel_info->rd_storage_releation->m_undoExtra = false;
  if (unlikely(ret != CDE_OK)) {
    CDE_LOG_ERROR("insert_heap fail");
    return ret;
  }

  bool need_check_fk = (!table_handler->foreign_dict_set.empty()) &&
                       ins_ctx->need_check_foreign();

  Datum *fullValues = ins_ctx->full_tuple()->values;
  bool *fullValuesIsNull = ins_ctx->full_tuple()->is_nulls;

  for (session_index_info *index_dict : rel_info->cde_index_vec) {
    ins_ctx->set_index(index_dict);
    if (unlikely(CdeDatumHeapToIndex(fullValues, fullValuesIsNull, ins_ctx) ==
                 CDE_FAIL)) {
      return CDE_ERROR;
    }

    if (need_check_fk && (CDE_OK != (ret = CdeInsCheckForeignConstraints(
                                         index_dict, table_handler, ins_ctx))))
      return ret;

    ret = cde_dml_api::insert_index(index_dict, index_values, index_is_nulls,
                                    &ctid);
    if (ret != CDE_OK) {
      ins_ctx->set_err_index(index_dict->shared_dict);
      return ret;
    }
  }

  /* Write rowlog after normal row operation success. */
  if (unlikely(ins_ctx->NeedCheckOnlineDdl())) {
    CdeRwlockGuard onlineMgrGuard(&ins_ctx->get_table()->m_onlineDdlMgrLock,
                                  CdeRwlockOp::RD_LOCK);
    OnlineDdlMgr *onlineDdlMgr = ins_ctx->get_table()->m_onlineDdlMgr;
    if (onlineDdlMgr && !onlineDdlMgr->NeedAbort()) {
      CDE_ASSERT(onlineDdlMgr->IsStatusValidForDml());
      CDE_ASSERT(ins_ctx->get_table() ==
                 onlineDdlMgr->GetInplaceCtx()->old_table);
      onlineDdlMgr->RowlogDmlForIndex(ROW_IDX_INSERT, values, is_nulls, &ctid);
    } else {
      ins_ctx->SetNeedCheckOnlineDdl(false);
    }
  }
  TransactionInterface::IncreaseCommandCounter();
  return CDE_OK;
}

int cde_dml_api::update_heap(StorageRelation heap_rel, Datum *values,
                             bool *is_nulls, ItemPointerData &old_ctid,
                             ItemPointerData &new_ctid) {
  HeapTuple *mem_tuple = TupleInterface::FormHeapTuple(
      heap_rel->attr, values, is_nulls, AllocMemZeroForDtuple);
  if (mem_tuple == nullptr) {
    CDE_LOG_ERROR("form dstore heap tuple fail");
    return GetAndConvertDstoreErrcodeToMysql();
  }

  HeapUpdateContext update_context;
  update_context.oldCtid = old_ctid;
  update_context.newTuple = mem_tuple;
  CDESetSnapshotByCurrent(update_context.snapshot);
  update_context.cid = TransactionInterface::GetCurCid();

  RetStatus ret = HeapInterface::Update(heap_rel, &update_context);
  TupleInterface::DestroyTuple(update_context.newTuple, CdeFreeMemForDtuple);
  if (ret != DSTORE_SUCC) {
    CDE_LOG_ERROR("dstore heap update fail for heap ctid {%d, %u} %d",
                  old_ctid.GetFileId(), old_ctid.GetBlockNum(),
                  old_ctid.GetOffset());
    return GetAndConvertDstoreErrcodeToMysql();
  }

  /* todo: how to determine if an index can skip update */
  new_ctid = update_context.newCtid;
  return CDE_OK;
}

int cde_dml_api::delete_index(session_index_info *index, Datum *index_values,
                              bool *index_is_nulls, ItemPointer heap_ctid) {
  BtreeInsertAndDeleteCommonData btree_context;
  CdeSetBtreeCtxForInsAndDel(index, index_values, index_is_nulls, heap_ctid,
                             btree_context);

  RetStatus ret = IndexInterface::Delete(btree_context);
  if (ret != DSTORE_SUCC) {
    CDE_LOG_ERROR("delete index tuple fail, table : %s index %s.",
                  index->shared_dict->table->name.c_str(),
                  index->shared_dict->name.c_str());
    return GetAndConvertDstoreErrcodeToMysql();
  }
  return CDE_OK;
}

int CdeInsCascadeCheckCycleAndDepth(dml_upd_ctx *upd_ctx, cde_dict_t *table) {
  dml_upd_ctx *cur_ctx = upd_ctx;
  uint32_t token = MAX_CASCADE_DEPTH;

  for (;;) {
    cur_ctx = cur_ctx->get_parent();
    if ((!cur_ctx) || (token == 0)) {
      break;
    }
    if ((cur_ctx->get_table() == table) && (!cur_ctx->is_delete())) {
      return HA_ERR_ROW_IS_REFERENCED;
    }
    token--;
  }
  return (token == 0) ? HA_ERR_FK_DEPTH_EXCEEDED : CDE_OK;
}

int CdeUpdateRowCascade(dml_upd_ctx *upd_ctx, dml_upd_task *upd_task) {
  upd_task->inc_cascade_depth();
  if (upd_task->check_cascade_depth()) {
    return HA_ERR_FK_DEPTH_EXCEEDED;
  }
  upd_task->reset_ctx(upd_ctx);
  int ret = CdeUpdateRowStep(upd_task);
  upd_task->reset_cascade_depth();
  return ret;
}

int CdeUpdForeignCheckOnConstraint(cde_dict_foreign_t *foreign,
                                   dml_upd_ctx *upd_ctx, ItemPointer p_ctid) {
  if ((upd_ctx->is_delete() &&
       0 == (foreign->fk_rule & (CDE_DDL_FK_RULE_ON_DELETE_CASCADE |
                                 CDE_DDL_FK_RULE_ON_DELETE_SET_NULL))) ||
      ((!upd_ctx->is_delete()) &&
       0 == (foreign->fk_rule & (CDE_DDL_FK_RULE_ON_UPDATE_CASCADE |
                                 CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL)))) {
    CdeInsForeignReportErr(upd_ctx->get_trxinfo(), foreign);
    return HA_ERR_ROW_IS_REFERENCED;
  }

  cde_dict_t *table = foreign->foreign_table;  // TODO:Increase ref count?
  int ret;

  dml_upd_ctx *cascade_ctx = upd_ctx->get_cascade();
  CDE_ASSERT(cascade_ctx != nullptr);

  ret = CdeInsCascadeCheckCycleAndDepth(cascade_ctx, table);
  if (ret != CDE_OK) {
    CdeInsForeignReportErr(upd_ctx->get_trxinfo(), foreign);
    return ret;
  }

  session_rel_info *table_rel_info = upd_ctx->getRelation(table);
  if (nullptr == table_rel_info) return CDE_ERROR;

  int lock_table_ret =
      CdeLockTable(LOCK_IX, table_rel_info->rd_storage_releation);
  if (lock_table_ret != DSTORE::DSTORE_SUCC) {
    CDE_LOG_ERROR("lock table failed");
    return lock_table_ret;
  }

  cascade_ctx->create_heap_scan();
  HeapTuple *heap_tuple = nullptr;
  HeapTuple *realTuple = nullptr;
  HeapTuple *tupleWithLob = nullptr;
  ret = cascade_ctx->fetch_tuple(p_ctid, heap_tuple, realTuple, tupleWithLob);
  // could be deleted by others?
  if (heap_tuple == nullptr) {
    // todo: why can dstore scan out tuple deleted in the same transcation?
    return ret;
  }

  ret = cascade_ctx->fill_tuple(realTuple, foreign->col_nums, foreign->fk_rule,
                                upd_ctx);
  if (unlikely(ret != CDE_OK)) {
    CdeInsForeignReportErr(upd_ctx->get_trxinfo(), foreign);
    goto func_exit;
  }

  cascade_ctx->set_old_ctid(p_ctid);
  ret = CdeUpdateRowCascade(cascade_ctx, upd_ctx->get_task());
func_exit:
  if (tupleWithLob != nullptr) {
    TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
  }
  TupleInterface::DestroyTuple(heap_tuple, CdeFreeMemForDtuple);
  return ret;
}

bool CdeCheckIndexNeedUpdate(uint32_t *attr_cols, uint32_t index_col_num,
                             bool *is_changed) {
  for (uint32_t i = 0; i < index_col_num; i++) {
    if (is_changed[attr_cols[i]]) {
      return true;
    }
  }
  return false;
}

static inline bool CdeNeedDoInsCheckForForeignConstraint(
    cde_dict_foreign_t *foreign, session_index_info *index, bool is_delete,
    bool *is_changed) {
  cde_dict_index_t *shared_dict = index->shared_dict;
  return (foreign->referenced_index == shared_dict) &&
         (is_delete || CdeCheckIndexNeedUpdate(shared_dict->attr_cols,
                                               foreign->col_nums, is_changed));
}

void CdeInsForeignReportErr(cde_trxinfo_t *trxinfo,
                            cde_dict_foreign_t *foreign) {
  // todo: use heap mem instead
  char for_table_name[CDE_MAX_TABLE_NAME_LEN + 1] = {0};
  char for_db_name[CDE_MAX_DATABASE_NAME_LEN + 1] = {0};
  char ref_table_name[CDE_MAX_TABLE_NAME_LEN + 1] = {0};
  char ref_db_name[CDE_MAX_DATABASE_NAME_LEN + 1] = {0};
  char fk_name[CDE_MAX_FK_NAME_LEN + 1] = {0};

  // todo: could you give the length of string after spliting?
  if ((CdeCreateTableInfo::SplitNormalizedName(
           foreign->foreign_table_name, for_table_name, for_db_name) != 0) ||
      (CdeCreateTableInfo::SplitNormalizedName(
           foreign->referenced_table_name, ref_table_name, ref_db_name) != 0) ||
      (CdeCreateTableInfo::SplitNormalizedName(foreign->unique_name, fk_name,
                                               for_db_name) != 0)) {
    CDE_LOG_ERROR("split_normalized_name fail.");
    return;
  }

  trxinfo->append_err_msg_with_quote(STRING_VAR_WITH_LEN(for_db_name));
  trxinfo->append_err_msg(STRING_CONST_WITH_LEN("."));
  trxinfo->append_err_msg_with_quote(STRING_VAR_WITH_LEN(for_table_name));

  trxinfo->append_err_msg(STRING_CONST_WITH_LEN(", CONSTRAINT "));
  trxinfo->append_err_msg_with_quote(STRING_VAR_WITH_LEN(fk_name));
  trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" FOREIGN KEY ("));

  for (uint32_t i = 0;;) {
    trxinfo->append_err_msg_with_quote(
        STRING_VAR_WITH_LEN(foreign->foreign_col_names[i]));
    if (++i < foreign->col_nums) {
      trxinfo->append_err_msg(STRING_CONST_WITH_LEN(", "));
    } else {
      break;
    }
  }

  trxinfo->append_err_msg(STRING_CONST_WITH_LEN(") REFERENCES "));

  if (strcmp(for_db_name, ref_db_name) != 0) {
    trxinfo->append_err_msg_with_quote(STRING_VAR_WITH_LEN(ref_db_name));
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN("."));
  }

  trxinfo->append_err_msg_with_quote(STRING_VAR_WITH_LEN(ref_table_name));
  trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ("));

  for (uint32_t i = 0;;) {
    trxinfo->append_err_msg_with_quote(
        STRING_VAR_WITH_LEN(foreign->referenced_col_names[i]));
    if (++i < foreign->col_nums) {
      trxinfo->append_err_msg(STRING_CONST_WITH_LEN(", "));
    } else {
      break;
    }
  }

  trxinfo->append_err_msg(STRING_CONST_WITH_LEN(")"));

  if (foreign->fk_rule & CDE_DDL_FK_RULE_ON_DELETE_CASCADE) {
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ON DELETE CASCADE"));
  }

  if (foreign->fk_rule & CDE_DDL_FK_RULE_ON_DELETE_SET_NULL) {
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ON DELETE SET NULL"));
  }

  if (!(foreign->fk_rule & CDE_DDL_FK_RULE_ON_DELETE_NO_ACTION) &&
      !(foreign->fk_rule & CDE_DDL_FK_RULE_ON_DELETE_CASCADE) &&
      !(foreign->fk_rule & CDE_DDL_FK_RULE_ON_DELETE_SET_NULL)) {
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ON DELETE RESTRICT"));
  }

  if (foreign->fk_rule & CDE_DDL_FK_RULE_ON_UPDATE_CASCADE) {
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ON UPDATE CASCADE"));
  }

  if (foreign->fk_rule & CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL) {
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ON UPDATE SET NULL"));
  }

  if (!(foreign->fk_rule & CDE_DDL_FK_RULE_ON_UPDATE_NO_ACTION) &&
      !(foreign->fk_rule & CDE_DDL_FK_RULE_ON_UPDATE_CASCADE) &&
      !(foreign->fk_rule & CDE_DDL_FK_RULE_ON_UPDATE_SET_NULL)) {
    trxinfo->append_err_msg(STRING_CONST_WITH_LEN(" ON UPDATE RESTRICT"));
  }
}

int CdeInsCheckForeignConstraint(cde_dict_foreign_t *foreign, cde_dict_t *table,
                                 dml_ins_ctx *ctx) {
  int ret = CDE_OK;
  if (ctx->check_has_null(foreign->col_nums)) {
    return CDE_OK;
  }

  if ((ctx->get_ctx_type() == CTX_TYPE_UPDATE ||
       ctx->get_ctx_type() == CTX_TYPE_DELETE) &&
      (ctx->get_foreign() == foreign)) {
    return CDE_OK;
  }

  cde_dict_t *check_table = foreign->referenced_table;
  /** If foreign->referenced_table is not null - it cannot be evicted.
   * Thus, we do not need to increase ref count for this table. */
  cde_dict_index_t *ref_index_shared = foreign->referenced_index;
  DictTableRefGuard dictRefGuard(nullptr);
  MDL_ticket *mdl{nullptr};

  if (nullptr == check_table) {
    check_table = CdeDdOpenTableOneOnName(foreign->referenced_table_name,
                                          ctx->get_current_thd(), &mdl, false);
    dictRefGuard.Reset(check_table, mdl, ctx->get_current_thd());
    if (nullptr != check_table) {
      CDE_ASSERT_DEBUG(nullptr == ref_index_shared);
      cde_dict_index_t *referencedIndex = check_table->get_index_on_column(
          foreign->referenced_col_names, foreign->col_nums);
      if (referencedIndex == nullptr) {
        CDE_LOG_ERROR(
            "Not Found Referenced Index! foreigin table = %s, ref table = %s, "
            "col_nums = %d",
            foreign->foreign_table_name, foreign->referenced_table_name,
            foreign->col_nums);
        return HA_ERR_NO_REFERENCED_ROW;
      }
      ref_index_shared = referencedIndex;
    }
  }

  if (nullptr == check_table) {
    CdeInsForeignReportErr(ctx->get_trxinfo(), foreign);
    return HA_ERR_NO_REFERENCED_ROW;
  }

  session_rel_info *check_rel_info = ctx->getRelation(check_table);
  if (nullptr == check_rel_info) return CDE_ERROR;

  cde_dict_index_t *for_index_shared = ctx->get_index()->shared_dict;
  session_index_info *ref_index =
      CdeGetRelationIndexInfo(ref_index_shared, check_rel_info);

  // todo: fixme
  CDE_ASSERT((check_table != nullptr) && (ref_index != nullptr));

  if (check_table != table) {
    /* We already have a LOCK_IX on table, but not necessarily
        on check_table */
    int lock_table_ret =
        CdeLockTable(LOCK_IS, check_rel_info->rd_storage_releation);
    if (lock_table_ret != DSTORE::DSTORE_SUCC) {
      CDE_LOG_ERROR("lock table failed");
      return CDE_ERROR;
    }
  }

  /* If any of the foreign key fields in entry is SQL NULL, we
  suppress the foreign key check: this is compatible with Oracle,
  for example */
  for (uint32_t i = 0; i < foreign->col_nums; i++) {
    uint32_t for_fk_pos = table->heapAttrIdx(for_index_shared->index_cols[i]);
    if (ctx->tuple()->is_nulls[for_fk_pos]) return CDE_OK;
  }

  if (check_table == table) {
    TupleDescData *tuple_desc = check_table->dstore_relation->attr;
    uint32_t for_fk_pos, ref_fk_pos;
    bool check_ok = true;
    for (uint32_t i = 0; i < foreign->col_nums; i++) {
      for_fk_pos = table->heapAttrIdx(for_index_shared->index_cols[i]);
      ref_fk_pos = check_table->heapAttrIdx(ref_index_shared->index_cols[i]);
      if (ctx->tuple()->is_nulls[for_fk_pos] !=
          ctx->tuple()->is_nulls[ref_fk_pos]) {
        check_ok = false;
        break;
      }

      if (ctx->tuple()->is_nulls[for_fk_pos]) {
        continue;
      }

      if (!CdeCmpDstoreData(ctx->tuple()->values[for_fk_pos],
                            ctx->tuple()->values[ref_fk_pos],
                            tuple_desc->attrs[ref_fk_pos],
                            CDE_SCAN_ORDER_EQUAL)) {
        check_ok = false;
        break;
      }
    }
    if (check_ok) {
      return CDE_OK;
    }
  }

  // it is a trick
  if (unlikely((ret = ctx->create_index_scan(ref_index, foreign->col_nums)) !=
               CDE_OK))
    return ret;

  ItemPointer p_ctid;
  ret = ctx->index_scan_next(p_ctid,
                             DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION);
  if (ret != CDE_OK) {
    ret = HA_ERR_NO_REFERENCED_ROW;
    CdeInsForeignReportErr(ctx->get_trxinfo(), foreign);
    return ret;
  }

  HeapTuple *tuple = nullptr;
  DSTORE::SnapshotData snapshot;
  CDESetSnapshotByTrans(snapshot);
  ret =
      CdeLockTuple(READ_COMMITTED, &snapshot,
                   check_rel_info->rd_storage_releation, p_ctid, &tuple, true);
  if (ret != CDE_OK) {
    CDE_LOG_ERROR(
        "CdeInsCheckForeignConstraint table:%s lock parent table %s failed, "
        "ret=%d",
        table->name.c_str(), check_table->name.c_str(), ret);
  }

  ctx->destroy_index_scan();
  return ret;
}

int CdeUpdCheckForeignConstraint(cde_dict_foreign_t *foreign, cde_dict_t *table,
                                 dml_upd_ctx *ctx) {
  int ret = CDE_OK;
  if (ctx->check_old_has_null(foreign->col_nums)) {
    return CDE_OK;
  }

  if ((ctx->get_ctx_type() == CTX_TYPE_UPDATE) &&
      (ctx->get_foreign() == foreign)) {
    return CDE_OK;
  }

  cde_dict_t *check_table = foreign->foreign_table;
  /* All child tables should have been openned by CdeDdOpenTable */
  CDE_ASSERT(nullptr != check_table);

  session_rel_info *checkRelInfo = ctx->getRelation(check_table);
  if (nullptr == checkRelInfo) return CDE_ERROR;
  session_index_info *check_index =
      CdeGetRelationIndexInfo(foreign->foreign_index, checkRelInfo);

  CDE_ASSERT((check_table != nullptr) && (check_index != nullptr));

  /* We already have a LOCK_IX on table, but not necessarily
     on check_table */
  if (check_table != table) {
    int lock_table_ret =
        CdeLockTable(LOCK_IS, checkRelInfo->rd_storage_releation);
    if (lock_table_ret != DSTORE::DSTORE_SUCC) {
      CDE_LOG_ERROR("lock table failed");
      return CDE_ERROR;  // todo: what error to return?
    }
  }

  dml_upd_ctx *cascade_ctx = ctx->get_cascade();
  if (cascade_ctx == nullptr) {
    bool is_delete = ctx->is_delete() &&
                     (foreign->fk_rule & CDE_DDL_FK_RULE_ON_DELETE_CASCADE);
    /* 0 vColumn - to be changed when foreign key support for virtual columns
       get added */
    cascade_ctx = new dml_upd_ctx(
        ctx->get_trxinfo(), check_table, checkRelInfo,
        check_table->get_n_attrs(), check_table->m_vColCount, is_delete,
        ctx->get_current_thd(), ctx->get_blob_mem_root(), nullptr);
    ret = cascade_ctx->init();
    if (unlikely(ret != CDE_OK)) return ret;
    cascade_ctx->SetNeedCheckOnlineDdl(check_table->IsInOnlineDdl());
    cascade_ctx->set_parent(ctx);
    ctx->set_cascade(cascade_ctx);
  }
  cascade_ctx->set_foreign(foreign);
  cascade_ctx->set_index(check_index);

  ret = cascade_ctx->fill_index_tuple(foreign->col_nums, ctx);
  if (unlikely(ret != CDE_OK)) return ret;

  if (unlikely((ret = cascade_ctx->create_index_scan(
                    check_index, foreign->col_nums)) != CDE_OK))
    return ret;

  ItemPointer p_ctid;
  for (;;) {
    ret = cascade_ctx->index_scan_next(
        p_ctid, DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION);
    if (ret != CDE_OK) {
      if (ret == HA_ERR_END_OF_FILE) {
        ret = CDE_OK;
      }
      break;
    }

    if (foreign->fk_rule == CDE_DDL_FK_RULE_DEFAULT) {
      ret = HA_ERR_ROW_IS_REFERENCED;
      CdeInsForeignReportErr(ctx->get_trxinfo(), foreign);
      break;
    }

    // reset the index of cascade_ctx, because the index in cascade_ctx is
    // changed during processing later.
    cascade_ctx->set_index(check_index);
    ret = CdeUpdForeignCheckOnConstraint(foreign, ctx, p_ctid);
    if (ret != CDE_OK) {
      if (ret == HA_ERR_RECORD_DELETED) {
        ret = CDE_OK; /* dstore may scan out tuple deleted by myself, it's not
                         an error */
        continue;
      }

      // ret may be handler or dstore error code.
      if (ret == HA_ERR_FOUND_DUPP_KEY &&
          (GetDstoreErrcode() == INDEX_ERROR_INSERT_UNIQUE_CHECK)) {
        ret = HA_ERR_FOREIGN_DUPLICATE_KEY;
      }
      break;
    }
  }

  cascade_ctx->destroy_index_scan();
  cascade_ctx->destroy_heap_scan();
  delete cascade_ctx;
  ctx->set_cascade(nullptr);
  return ret;
}

int CdeUpdCheckReferencesConstraints(cde_dict_t *table,
                                     session_index_info *index,
                                     dml_upd_ctx *upd_ctx) {
  if ((!upd_ctx->need_check_foreign()) || table->referenced_dict_set.empty()) {
    return CDE_OK;
  }

  int ret = CDE_OK;

  for (cde_dict_foreign_t *foreign : table->referenced_dict_set) {
    if (!CdeNeedDoInsCheckForForeignConstraint(foreign, index,
                                               upd_ctx->is_delete(),
                                               upd_ctx->get_is_changed_vec())) {
      continue;
    }

    if ((!upd_ctx->is_delete()) && (upd_ctx->get_foreign() == foreign)) {
      continue;
    }

    ret = CdeUpdCheckForeignConstraint(foreign, table, upd_ctx);

    if (ret != CDE_OK) {
      return ret;
    }
  }
  return CDE_OK;
}

int CdeDeleteIndexStep(dml_upd_ctx *upd_ctx) {
  session_index_info *index = upd_ctx->get_index();
  ItemPointerData &old_ctid = upd_ctx->tuple_old()->ctid;
  Datum *values_old = upd_ctx->full_tuple_old()->values;
  bool *is_nulls_old = upd_ctx->full_tuple_old()->is_nulls;
  Datum *index_values = upd_ctx->index_tuple()->index_values;
  bool *index_is_nulls = upd_ctx->index_tuple()->index_is_nulls;

  if (unlikely(CdeDatumHeapToIndex(values_old, is_nulls_old, upd_ctx) ==
               CDE_FAIL)) {
    return CDE_ERROR;
  }

  return cde_dml_api::delete_index(index, index_values, index_is_nulls,
                                   &old_ctid);
}

int CdeUpdateIndexStep(dml_upd_ctx *upd_ctx) {
  session_index_info *index = upd_ctx->get_index();
  ItemPointerData &new_ctid = upd_ctx->tuple()->ctid;
  Datum *values_old = upd_ctx->full_tuple_old()->values;
  bool *is_nulls_old = upd_ctx->full_tuple_old()->is_nulls;
  Datum *values_new = upd_ctx->full_tuple()->values;
  bool *is_nulls_new = upd_ctx->full_tuple()->is_nulls;
  Datum *index_values = upd_ctx->index_tuple()->index_values;
  bool *index_is_nulls = upd_ctx->index_tuple()->index_is_nulls;

  if (unlikely(CdeDatumHeapToIndex(values_old, is_nulls_old, upd_ctx) ==
               CDE_FAIL)) {
    return CDE_ERROR;
  }

  cde_dict_t *table_handler = upd_ctx->get_table();
  int32_t ret = CdeUpdCheckReferencesConstraints(table_handler, index, upd_ctx);
  if ((upd_ctx->is_delete()) || (ret != CDE_OK)) {
    return ret;
  }

  if (unlikely(CdeDatumHeapToIndex(values_new, is_nulls_new, upd_ctx) ==
               CDE_FAIL)) {
    return CDE_ERROR;
  }

  if ((!table_handler->foreign_dict_set.empty()) &&
      upd_ctx->need_check_foreign() &&
      (CDE_OK !=
       (ret = CdeInsCheckForeignConstraints(index, table_handler, upd_ctx)))) {
    return ret;
  }

  ret =
      cde_dml_api::insert_index(index, index_values, index_is_nulls, &new_ctid);
  if (ret != CDE_OK) {
    upd_ctx->set_err_index(index->shared_dict);
    return ret;
  }
  return CDE_OK;
}

// see: row_upd
int CdeUpdateRowStepInner(dml_upd_ctx *upd_ctx) {
  session_rel_info *rel_info = upd_ctx->get_rel();
  ItemPointerData &old_ctid = upd_ctx->tuple_old()->ctid;
  ItemPointerData &new_ctid = upd_ctx->tuple()->ctid;
  Datum *values_new = upd_ctx->tuple()->values;
  bool *is_nulls_new = upd_ctx->tuple()->is_nulls;
  bool *is_changed = upd_ctx->get_is_changed_vec();
  int ret;
  bool ctid_not_changed = false;

  if (upd_ctx->NeedCheckOnlineDdl()) {
    CdeRwlockGuard onlineMgrGuard(&upd_ctx->get_table()->m_onlineDdlMgrLock,
                                  CdeRwlockOp::RD_LOCK);
    OnlineDdlMgr *onlineDdlMgr = upd_ctx->get_table()->m_onlineDdlMgr;
    if (onlineDdlMgr && !onlineDdlMgr->NeedAbort()) {
      CDE_ASSERT(onlineDdlMgr->IsStatusValidForDml());
      rel_info->rd_storage_releation->m_undoExtra = true;
    } else {
      upd_ctx->SetNeedCheckOnlineDdl(false);
    }
  }

  if (upd_ctx->is_delete()) {
    ret = cde_dml_api::delete_heap(rel_info->rd_storage_releation, old_ctid);
    rel_info->rd_storage_releation->m_undoExtra = false;
    if (HA_ERR_RECORD_DELETED == ret) {
      /* dstore may scan out already deleted tuples, it's not
         an error */
      return CDE_OK;
    }
  } else {
    ret = cde_dml_api::update_heap(rel_info->rd_storage_releation, values_new,
                                   is_nulls_new, old_ctid, new_ctid);
    rel_info->rd_storage_releation->m_undoExtra = false;
    ctid_not_changed = (old_ctid == new_ctid);
  }

  if (ret != CDE_OK) {
    return ret;
  }

  for (session_index_info *index_dict : rel_info->cde_index_vec) {
    if (ctid_not_changed &&
        (!CdeCheckIndexNeedUpdate(index_dict->shared_dict->attr_cols,
                                  index_dict->shared_dict->index_col_num,
                                  is_changed))) {
      continue;
    }
    upd_ctx->set_index(index_dict);
    bool isSlave = thd_slave_thread(upd_ctx->get_current_thd());
    if (upd_ctx->get_table()->m_vColCount > 0 &&
        ((upd_ctx->get_task()->get_cascade_depth() > 0 || isSlave ||
          upd_ctx->get_current_thd()->rli_fake != nullptr) ||
         upd_ctx->CheckNeedCalculateVirtualColumn(
             index_dict->shared_dict->index_cols,
             index_dict->shared_dict->index_col_num))) {
      /* we are in cascade - we didn't get values for virtual columns from the
      server, thus, in case index covers virtual columns, we need to compute
      them here
      OR
      We are replica and for delete - only "before image" is replicated. The
      slave server never calulates virtual columns. Thus in replica we
      need to calculate the virtual column in the dstore engine - for
      index delete.
      OR
      We are executing binlog statement in client THD and we don't have virutal
      columns populated
      In case we are not in cascade - we do not need to open (TABLE*)mysqlTable
      - we can use the one provided by the handler. In case of cascade - we need
      to open TABLE* - as we are in a different table than the starting one
      (handler one).
      */
      TABLE *mysqlTable = upd_ctx->get_task()->get_cascade_depth() == 0
                              ? upd_ctx->GetMySQLTable()
                              : nullptr;
      /* For slave and binglog event we should always get a mysqlTable in the
       * context. */
      CDE_ASSERT_DEBUG(nullptr != mysqlTable ||
                       upd_ctx->get_task()->get_cascade_depth() > 0);
      ret = upd_ctx->FillVirtualColumns(index_dict->shared_dict->index_cols,
                                        index_dict->shared_dict->index_col_num,
                                        mysqlTable);
      if (CDE_OK != ret) return ret;
    }
    ret = CdeDeleteIndexStep(upd_ctx);
    if (ret != CDE_OK) {
      return ret;
    }
  }

  if ((upd_ctx->is_delete()) &&
      ((!upd_ctx->need_check_foreign()) ||
       upd_ctx->get_table()->referenced_dict_set.empty())) {
    goto func_ok_exit;
  }

  for (session_index_info *index_dict : rel_info->cde_index_vec) {
    if (ctid_not_changed &&
        (!CdeCheckIndexNeedUpdate(index_dict->shared_dict->attr_cols,
                                  index_dict->shared_dict->index_col_num,
                                  is_changed))) {
      continue;
    }
    upd_ctx->set_index(index_dict);
    if (upd_ctx->get_task()->get_cascade_depth() > 0 &&
        upd_ctx->get_table()->m_vColCount > 0) {
      /* we are in cascade - we didn't get values for virtual columns from the
      server, thus, in case index covers virtual columns, we need to compute
      them here */
      ret = upd_ctx->FillVirtualColumns(index_dict->shared_dict->index_cols,
                                        index_dict->shared_dict->index_col_num,
                                        nullptr);
      if (CDE_OK != ret) return ret;
    }
    ret = CdeUpdateIndexStep(upd_ctx);
    if (ret != CDE_OK) {
      return ret;
    }
  }
func_ok_exit:
  /* As we release the mutex before, we need to get the mutex and acquire
  it again. */
  if (unlikely(upd_ctx->NeedCheckOnlineDdl())) {
    CdeRwlockGuard onlineMgrGuard(&upd_ctx->get_table()->m_onlineDdlMgrLock,
                                  CdeRwlockOp::RD_LOCK);
    OnlineDdlMgr *onlineDdlMgr = upd_ctx->get_table()->m_onlineDdlMgr;
    if (onlineDdlMgr && !onlineDdlMgr->NeedAbort()) {
      CDE_ASSERT(onlineDdlMgr->IsStatusValidForDml());
      CDE_ASSERT(upd_ctx->get_table() ==
                 onlineDdlMgr->GetInplaceCtx()->old_table);
      if (upd_ctx->is_delete()) {
        onlineDdlMgr->RowlogDmlForIndex(
            ROW_IDX_DELETE, upd_ctx->tuple_old()->values,
            upd_ctx->tuple_old()->is_nulls, &old_ctid);
      } else {
        onlineDdlMgr->RowlogDmlForIndex(
            ROW_IDX_DELETE, upd_ctx->tuple_old()->values,
            upd_ctx->tuple_old()->is_nulls, &old_ctid, upd_ctx->tuple()->values,
            upd_ctx->tuple()->is_nulls, &new_ctid);
      }
    } else {
      upd_ctx->SetNeedCheckOnlineDdl(false);
    }
  }

  TransactionInterface::IncreaseCommandCounter();
  return CDE_OK;
}

int CdeUpdateRowStep(dml_upd_task *upd_task) {
  dml_upd_ctx *cur_ctx = upd_task->get_cur_ctx();
  cur_ctx->bind_task(upd_task);
  dml_upd_ctx *parent = cur_ctx->get_parent();
  int ret = CdeUpdateRowStepInner(cur_ctx);
  upd_task->set_cur_ctx(parent);
  return ret;
}

int CdeDmlUpdateRow(const TABLE *table, const unsigned char *mysql_ptr_old,
                    const unsigned char *mysql_ptr_new, dml_upd_ctx *upd_ctx) {
  DBUG_EXECUTE_IF("cde_dml_update_row_fault_injection", { return CDE_ERROR; });

  int ret;
  bool hasIndex = upd_ctx->get_rel()->cde_index_vec.size() != 0;
  /** In case table has virtual columns we need to build a full tuple with
   * all columns - including the virtual ones. We need to insert into index
   * that might have be indexing such a virtual column. */
  bool buildFullTuple = hasIndex && upd_ctx->get_table()->m_vColCount > 0;

  if (upd_ctx->is_delete()) {
    ret = MysqlRecordToDstoreForInsert(
        table, mysql_ptr_old, upd_ctx->tuple_old(), upd_ctx,
        upd_ctx->get_rel()->rd_storage_releation->attr, true);

    if (ret != CDE_OK) return ret;

    if (buildFullTuple) {
      ret = MysqlRecordToDstoreForInsert(table, mysql_ptr_old,
                                         upd_ctx->full_tuple_old(), upd_ctx,
                                         upd_ctx->get_table()->attrFull, false);
    }

  } else {
    ret = MysqlRecordToDstoreForUpd(
        table, mysql_ptr_old, mysql_ptr_new, upd_ctx, upd_ctx->tuple(),
        upd_ctx->tuple_old(), upd_ctx->get_rel()->rd_storage_releation->attr,
        true);

    if (ret != CDE_OK) return ret;

    if (buildFullTuple) {
      ret = MysqlRecordToDstoreForUpd(
          table, mysql_ptr_old, mysql_ptr_new, upd_ctx, upd_ctx->full_tuple(),
          upd_ctx->full_tuple_old(), upd_ctx->get_table()->attrFull, false);
    }
  }
  if (ret != CDE_OK) return ret;

  dml_upd_task task(upd_ctx);
  return CdeUpdateRowStep(&task);
}

int cde_dml_api::delete_heap(StorageRelation heap_rel, ItemPointerData &ctid) {
  /* Step 1:  construction HeapDeleteContext */
  HeapDeleteContext delete_context;
  delete_context.ctid = ctid;  // ctid generated when inserting data
  delete_context.needReturnTup = false;
  delete_context.cid = TransactionInterface::GetCurCid();
  CDESetSnapshotByCurrent(delete_context.snapshot);

  /* Step 2: Do the delete */
  RetStatus ret = HeapInterface::Delete(heap_rel, &delete_context);
  if (ret != DSTORE_SUCC) {
    int errorCode = GetAndConvertDstoreErrcodeToMysql();
    DBUG_EXECUTE_IF("error_on_delete_heap", errorCode = HA_ERR_RECORD_CHANGED;);
    /** it's not an error if tuple was already deleted, dont log it */
    if (HA_ERR_RECORD_DELETED != errorCode)
      CDE_LOG_ERROR("Failed to delete heap of ctid {%d, %u} %d",
                    ctid.GetFileId(), ctid.GetBlockNum(), ctid.GetOffset());
    return errorCode;
  }

  return CDE_OK;
}

ICP_RESULT CdeIndexCondFunc(handler *handler) {
  DBUG_TRACE;

  if (handler->end_range && handler->compare_key_icp(handler->end_range) > 0) {
    return ICP_OUT_OF_RANGE;
  }

  return (handler->pushed_idx_cond->val_int() == 0) ? ICP_NO_MATCH : ICP_MATCH;
}

int cde_dml_api::ReadIndexOnly(cde_isolation_level_t isolationLevel, uchar *buf,
                               const TABLE *table, ha_cde *ha,
                               dstore_handler_t *dstoreHandler,
                               DSTORE::SnapshotData *snapShotData) {
  cde_dict_index_t *curIndex = dstoreHandler->current_cde_index->shared_dict;
  DSTORE::IndexScanHandler *indexScanHandler = dstoreHandler->dstore_index_scan;
  bool recheck = false;

  /** First, retrieve the index tuple. If no Index Condition Pushdown (ICP),
  directly return the index information to MySQL.
  If ICP is used, call CdeIndexCondFunc(ha) to verify if the index tuple
  meets the range requirements:
  - ICP_NO_MATCH: Continue looping and proceed to the next index scan.
  - ICP_MATCH: Exit the loop, store the CTID, and return the index information
  to MySQL.
  - ICP_OUT_OF_RANGE: If the end is reached without finding a match, terminate
  the scan. */
  while (true) {
    IndexTuple *itup = nullptr;
    TupleDesc td;

    itup = IndexInterface::OnlyScanNext(indexScanHandler,
                                        dstoreHandler->direction, &td, &recheck);

    if (itup == nullptr ||
        (itup->GetHeapCtid()) == DSTORE::INVALID_ITEM_POINTER) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    dstoreHandler->dstore_heap_ctid = itup->GetHeapCtid();

    /* handle locking read */
    if (dstoreHandler->select_lock_type != LOCK_NONE &&
        !dstoreHandler->table_handler->is_temporary()) {
      if (dstoreHandler->sql_stat_start) {
        if (CdeLockTable(
                dstoreHandler->select_lock_type == LOCK_S ? LOCK_IS : LOCK_IX,
                dstoreHandler->rel_info->rd_storage_releation) !=
            DSTORE::DSTORE_SUCC) {
          CDE_LOG_ERROR("lock table failed");
          return HA_ERR_LOCK_TABLE_FULL;
        }
      }

      dstoreHandler->sql_stat_start = false;

      HeapTuple *tuple = nullptr;
      auto ctid = dstoreHandler->dstore_heap_ctid;
      bool needWait =
          dstoreHandler->select_mode == SELECT_ORDINARY ? true : false;
      int error = CdeLockTuple(isolationLevel, snapShotData,
                               dstoreHandler->rel_info->rd_storage_releation,
                               &ctid, &tuple, needWait);
      if (error != CDE_OK) {
        if (error == HA_ERR_RECORD_DELETED) {
          continue;
        }

        if (error == static_cast<int>(HaDstoreErrE::HA_DSTORE_ERR_SKIP_WAIT) &&
            dstoreHandler->select_mode == SELECT_SKIP_LOCKED) {
          continue;
        }
        return error;
      }

      CDE_ASSERT(tuple != nullptr);
      TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
      tuple = nullptr;
    }
    // do not need to convert data if there is no idx_cond
    if (likely(ha) && ha->pushed_idx_cond == nullptr) {
      if (ha->skip_row_for_offset_pushdown()) {
        ha->increment_scanned_offset();
        continue;
      }
    }

    if (likely(ha) && ha->aggregate_unqualified_count()) {
      return CDE_OK;
    }

    TupleInterface::DeformIndexTuple(
        itup, td, dstoreHandler->m_valuesWithoutVarlen, dstoreHandler->m_nulls);
    DstoreIndexDataToMysql(table, buf, dstoreHandler->m_valuesWithoutVarlen,
                           dstoreHandler->m_nulls, curIndex->index_cols,
                           curIndex->attr_cols, curIndex->index_col_num,
                           dstoreHandler->table_handler->attrFull);

    if (likely(ha) && ha->pushed_idx_cond != nullptr) {
      ICP_RESULT res = CdeIndexCondFunc(ha);
      ++dstoreHandler->m_icpCheckCount;
      switch (res) {
        case ICP_NO_MATCH: {
          // if ICP_NO_MATCH, index scan next.
          continue;
        }
        case ICP_OUT_OF_RANGE:
          // if ICP_OUT_OF_RANGE, end scan.
          return HA_ERR_KEY_NOT_FOUND;
        case ICP_MATCH:
          ++dstoreHandler->m_icpMatchCount;
          if (ha->skip_row_for_offset_pushdown()) {
            ha->increment_scanned_offset();
            continue;
          }
          // scan heap to get the ctid.
          break;
      }
    }
    if (likely(ha) && ha->aggregate_rows_by_group_for_index()) {
      continue;
    }
    return CDE_OK;
  }
}

int cde_dml_api::ReadFullRow(cde_isolation_level_t isolationLevel, uchar *buf,
                             const TABLE *table, ha_cde *ha,
                             dstore_handler_t *dstore_handler,
                             MEM_ROOT *blobMemRoot,
                             bool allowBlobMemRootClearForReuse,
                             DSTORE::SnapshotData *snapShotData) {
  bool recheck = false;
  bool found = false;
  ItemPointerData *extremeCtid = nullptr;
  cde_dict_index_t *cur_index = dstore_handler->current_cde_index->shared_dict;
  DSTORE::IndexScanHandler *index_scan_handler =
      dstore_handler->dstore_index_scan;

  HeapTuple *tuple = nullptr;
  HeapTuple *tupleWithLob = nullptr;
  CDE_ASSERT(ha != nullptr);
  // it while keep loop only if CdeIndexCondFunc() returns ICP_NO_MATCH
  while (true) {
    DSTORE::RetStatus status = IndexInterface::ScanNext(
        index_scan_handler, dstore_handler->direction, &found, &recheck);
    if (status == DSTORE::DSTORE_FAIL) {
      return HA_ERR_END_OF_FILE;
    }

    extremeCtid = IndexInterface::GetResultHeapCtid(index_scan_handler);
    if (!found || (extremeCtid == nullptr) ||
        (*extremeCtid == INVALID_ITEM_POINTER)) {
      return HA_ERR_END_OF_FILE;
    }

    /* handle index condition pushdown and virtual columns read from index */
    if ((ha && ha->pushed_idx_cond != nullptr) ||
        dstore_handler->m_readVirutalColsFromIndex) {
      IndexTuple *itup =
          IndexInterface::IndexScanGetIndexTuple(index_scan_handler);
      TupleDesc td = IndexInterface::IndexScanGetTupleDesc(index_scan_handler);
      if (itup == nullptr) {
        return HA_ERR_END_OF_FILE;
      }

      bool convertOnlyVirtualColumns =
          (nullptr == ha || nullptr == ha->pushed_idx_cond);

      TupleInterface::DeformIndexTuple(itup, td,
                                       dstore_handler->m_valuesWithoutVarlen,
                                       dstore_handler->m_nulls);
      DstoreIndexDataToMysql(table, buf, dstore_handler->m_valuesWithoutVarlen,
                             dstore_handler->m_nulls, cur_index->index_cols,
                             cur_index->attr_cols, cur_index->index_col_num,
                             dstore_handler->table_handler->attrFull, nullptr,
                             convertOnlyVirtualColumns);

      if (false == convertOnlyVirtualColumns) {
        ICP_RESULT icp_res = CdeIndexCondFunc(ha);
        ++dstore_handler->m_icpCheckCount;
        switch (icp_res) {
          case ICP_NO_MATCH: {
            // if ICP_NO_MATCH, do not scan heap, and index scan next.
            continue;
          }
          case ICP_OUT_OF_RANGE:
            // if ICP_OUT_OF_RANGE, end scan.
            return HA_ERR_END_OF_FILE;
          case ICP_MATCH:
            ++dstore_handler->m_icpMatchCount;
            // scan heap to get full record.
            break;
        }
      }
    }

    /* handle locking read */
    if (dstore_handler->select_lock_type != LOCK_NONE &&
        !dstore_handler->table_handler->is_temporary()) {
      CDE_ASSERT_DEBUG(nullptr == tuple);
      if (dstore_handler->sql_stat_start) {
        if (CdeLockTable(
                dstore_handler->select_lock_type == LOCK_S ? LOCK_IS : LOCK_IX,
                dstore_handler->rel_info->rd_storage_releation) !=
            DSTORE::DSTORE_SUCC) {
          CDE_LOG_ERROR("lock table failed");
          return HA_ERR_LOCK_TABLE_FULL;
        }
      }

      dstore_handler->sql_stat_start = false;
      bool needWait =
          dstore_handler->select_mode == SELECT_ORDINARY ? true : false;

      int error = CdeLockTuple(isolationLevel, snapShotData,
                               dstore_handler->rel_info->rd_storage_releation,
                               extremeCtid, &tuple, needWait);
      if (error != CDE_OK) {
        TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        tuple = nullptr;
        if (error == HA_ERR_RECORD_DELETED) {
          continue;
        }
        if (error == static_cast<int>(HaDstoreErrE::HA_DSTORE_ERR_SKIP_WAIT) &&
            dstore_handler->select_mode == SELECT_SKIP_LOCKED) {
          continue;
        }
        if (CdeEnableDamagePageManager() &&
            error == (int)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE) {
          CDE_LOG_ERROR("swat hit damage page when lock tuple in ReadFullRow");
          if (CdeReportErrorOnDamagePage()) {
            return error;
          }
          continue;
        }
        return error;
      }

      CDE_ASSERT(tuple != nullptr);
      if (ha->skip_row_for_offset_pushdown()) {
        ha->increment_scanned_offset();
        TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        tuple = nullptr;
        continue;
      }

    } else {
      /* handle non-locking read */
      if (ha->skip_row_for_offset_pushdown()) {
        ha->increment_scanned_offset();
        continue;
      }

      tuple = HeapInterface::FetchTuple(dstore_handler->dstore_heap_scan,
                                        *extremeCtid);
      if (tuple == nullptr) {
        HaErrorCode errcode = GetAndConvertDstoreErrcodeToMysql();
        if (CdeEnableDamagePageManager() &&
            (uint32_t)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE ==
                errcode) {
          CDE_LOG_ERROR("Hit swat damage page when fetch tuple");
          if (!CdeReportErrorOnDamagePage()) {
            continue;
          }
        }
        CDE_LOG_ERROR("FetchTuple fail");
        return errcode;
      } else if (tuple->GetCtid() == nullptr) {
        TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        return HA_ERR_END_OF_FILE;
      }
    }
    dstore_handler->dstore_heap_ctid = *tuple->GetCtid();
    DSTORE::TupleDesc tupleDesc =
        dstore_handler->rel_info->rd_storage_releation->attr;
    HeapTuple *realTuple = tuple;
    if (tupleDesc->tdhaslob && dstore_handler->m_needDecodeLobColumns) {
      /** When the LOB size exceeds 2K, the returned `tupleWithLob` from
       * `FetchBlobField` is a newly allocated memory block and must be freed.
       * When the LOB size is under 2K, the returned `tupleWithLob` points to
       * the same memory block as `tuple` and should not be freed.*/
      tupleWithLob = FetchBlobFiled(
          dstore_handler->rel_info->rd_storage_releation, tuple, snapShotData,
          dstore_handler->select_lock_type == LOCK_NONE);

      DBUG_EXECUTE_IF(
          "read_full_row_null_pointer_injection",
          TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
          tupleWithLob = nullptr;);
      if (unlikely(tupleWithLob == nullptr)) {
        uint32_t error_code = GetAndConvertDstoreErrcodeToMysql();
        CDE_LOG_ERROR(
            "TupleWithLob is nullptr in ReadFullRow function, error is %u.",
            error_code);
        TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        tuple = nullptr;
        if (CdeEnableDamagePageManager() &&
            (uint32_t)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE ==
                error_code) {
          CDE_LOG_ERROR("Hit swat damage page when fetch lob tuple");
          if (!CdeReportErrorOnDamagePage()) {
            continue;
          }
        }
        return error_code;
      }
      ItemPointerData *ctid = tupleWithLob->GetCtid();
      DBUG_EXECUTE_IF("read_full_row_ctid_nullptr_injection", ctid = nullptr;);
      if (unlikely(ctid == nullptr)) {
        uint32_t error_code = GetAndConvertDstoreErrcodeToMysql();
        if (tuple != tupleWithLob) {
          TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
        }
        TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        CDE_LOG_ERROR("Ctid is nullptr in ReadFullRow function, error is %u.",
                      error_code);
        return error_code;
      }
      if (tuple != tupleWithLob) {
        realTuple = tupleWithLob;
      } else {
        tupleWithLob = nullptr;
      }
    }

    dstore_handler->DecodeDstoreRow(
        dstore_handler->rel_info->rd_storage_releation->attr,
        dstore_handler->m_valuesWithoutVarlen, realTuple, buf, table,
        blobMemRoot, allowBlobMemRootClearForReuse);

    /** FetchTuple() will create space and copy tuple to resTuple,
    so need to call DstorePfree(resTuple).
    the same as in ha_cde::index_read */
    TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
    tuple = nullptr;

    if (tupleWithLob != nullptr) {
      TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
    }

    if (likely(ha) && ha->aggregate_rows_by_group_for_index()) {
      continue;
    }
    return 0;
  }
}
} /* namespace CDE */
