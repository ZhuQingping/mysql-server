/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_relation.cc
 *
 *
 * IDENTIFICATION
 * src/cde_relation.cc
 *
 * -------------------------------------------------------------------------
 */

#include "dict/cde_relation.h"
#include "common/cde_alloc.h"
#include "common/cde_errorcode.h"
#include "common/cde_perf.h"
#include "common/cde_session.h"
#include "common/cde_typecache.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_tuple.h"
#include "dict/cde_dict.h"
#include "errorcode/dstore_heap_error_code.h"

#include "framework/dstore_instance_interface.h"
#include "heap/dstore_heap_struct.h"
#include "page/dstore_page_struct.h"
#include "systable/dstore_relation.h"

#include "my_pointer_arithmetic.h"
using DSTORE::AttrNumber;
using DSTORE::DSTORE_SUCC;
using DSTORE::IndexInfo;
using DSTORE::MAINTAIN_ORDER;
using DSTORE::RetStatus;
using DSTORE::SCAN_KEY_ROW_MEMBER;
using DSTORE::ScanKey;
using DSTORE::ScanKeyData;
using DSTORE::StorageRelation;
using DSTORE::SysClassTupDef;
using DSTORE::SysIndexTupDef;
using DSTORE::TupleDesc;

namespace CDE {
IndexInfo *CdeCreateIndexInfo(int attr_num, MEM_ROOT *mem) {
  size_t index_info_size = ALIGN_SIZE(sizeof(IndexInfo));
  size_t option_size =
      ALIGN_SIZE(sizeof(int16_t) * static_cast<uint32_t>(attr_num));
  size_t opcintype_size =
      ALIGN_SIZE(sizeof(Oid) * static_cast<uint32_t>(attr_num));
  size_t total_size = index_info_size + option_size + opcintype_size;
  char *index = (char *)mem->Alloc(total_size);
  if (unlikely(index == nullptr)) {
    return nullptr;
  }
  (void)memset_s(static_cast<void *>(index), total_size, 0, total_size);
  IndexInfo *index_info = static_cast<IndexInfo *>(static_cast<void *>(index));
  index_info->opcinType =
      static_cast<Oid *>(static_cast<void *>(index + index_info_size));
  index_info->indexOption = static_cast<int16_t *>(
      static_cast<void *>(index + index_info_size + opcintype_size));

  /* Currently, expression indexes are not supported. */
  index_info->exprCallback = nullptr;
  index_info->exprInitCallback = nullptr;
  index_info->exprDestroyCallback = nullptr;

  index_info->m_indexSupportProcInfo =
      static_cast<DSTORE::IndexSupportProcInfo *>(
          mem->Alloc(sizeof(DSTORE::IndexSupportProcInfo)));
  if (unlikely(index_info->m_indexSupportProcInfo == nullptr)) {
    return nullptr;
  }
  (void)memset_s(index_info->m_indexSupportProcInfo,
                 sizeof(DSTORE::IndexSupportProcInfo), 0,
                 sizeof(DSTORE::IndexSupportProcInfo));
  index_info->m_indexSupportProcInfo->numSupportProc = 0;
  return index_info;
}

IndexInfo *CdeBuildIndexInfo(DSTORE::StorageRelation src_rel,
                             DSTORE::StorageRelation dst_rel, uint16_t key_num,
                             bool isUnique, MEM_ROOT *mem) {
  int col_num = src_rel->attr->natts;
  IndexInfo *index_info = CdeCreateIndexInfo(col_num, mem);
  if (!index_info) {
    return nullptr;
  }
  // not use dstore IndexInfo name, index_info->indexRelName

  index_info->relKind = src_rel->rel->relkind;
  index_info->isUnique = isUnique;
  index_info->indexAttrsNum = static_cast<uint16_t>(col_num);
  index_info->indexKeyAttrsNum = key_num;
  // do not copy here, can use the same memory
  index_info->attributes = dst_rel->attr;
  for (uint16_t i = 0; i < col_num; i++) {
    index_info->indexOption[i] = src_rel->index->indexOption[i];
  }

  for (uint16_t i = 0; i < key_num; i++) {
    index_info->opcinType[i] = src_rel->attr->attrs[i]->atttypid;
  }
  return index_info;
}

bool cde_relation::Clone(StorageRelation src_rel, MEM_ROOT *mem,
                         const int32_t fillfactor, StorageRelation &dst_rel) {
  size_t alloc_len = sizeof(DSTORE::StorageRelationData);
  DSTORE::StorageRelation relation =
      (DSTORE::StorageRelation)(mem->Alloc(alloc_len));
  DBUG_EXECUTE_IF("StorageRelation_Alloc_injection", { relation = nullptr; });
  if (unlikely(relation == nullptr)) {
    CDE_LOG_ERROR("Alloc mem fail when clone relation, oid:%u.",
                  src_rel->relOid);
    return CDE_FAIL;
  }
  (void)memset_s(relation, alloc_len, 0, alloc_len);

  auto copy_attr = cde_tuple_desc::clone(src_rel->attr, mem);
  DBUG_EXECUTE_IF("tuple_desc_clone_injection", { copy_attr = nullptr; });
  if (!copy_attr) {
    CDE_LOG_ERROR("Clone tupledesc fail when clone relation, oid:%u.",
                  src_rel->relOid);
    return CDE_FAIL;
  }

  relation->attr = copy_attr;
  int32_t ff_to_use = g_defaultFillfactor;
  auto relation_kind = static_cast<DSTORE::RelationKind>(src_rel->rel->relkind);
  if (relation_kind == DSTORE::SYS_RELKIND_INDEX ||
      relation_kind == DSTORE::SYS_RELKIND_GLOBAL_INDEX) {
    ff_to_use = g_defaultFillfactorForIndex;
  }
  auto ff = IsValidFillfactor(fillfactor) ? fillfactor : ff_to_use;
  RetStatus ret = relation->Construct(
      DSTORE::g_defaultPdbId, src_rel->relOid, src_rel->rel, relation->attr, ff,
      static_cast<DSTORE::TablespaceId>(src_rel->rel->reltablespace), true);
  DBUG_EXECUTE_IF("relation_construct_fail_injection",
                  { ret = DSTORE::DSTORE_FAIL; });
  if (ret != DSTORE_SUCC) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR(
        "Construct relation fail when clone relation, oid:%u.",
        src_rel->relOid);
    return CDE_FAIL;
  }

  alloc_len = sizeof(SysClassTupDef);
  relation->rel = static_cast<SysClassTupDef *>(mem->Alloc(alloc_len));
  DBUG_EXECUTE_IF("SysClassTupDef_Alloc_injection",
                  { relation->rel = nullptr; });
  if (unlikely(relation->rel == nullptr)) {
    CDE_LOG_ERROR("Mem alloc fail when clone relation, oid:%u.",
                  src_rel->relOid);
    return CDE_FAIL;
  }
  (void)memcpy_s(relation->rel, alloc_len, src_rel->rel, alloc_len);

  relation->index = nullptr;
  if (DstoreRelationIsIndex(src_rel)) {
    // todo: dstore do not use this field right now
    alloc_len = sizeof(SysIndexTupDef);
    relation->indexInfo = static_cast<SysIndexTupDef *>(mem->Alloc(alloc_len));
    DBUG_EXECUTE_IF("SysIndexTupDef_Alloc_injection",
                    { relation->rel = nullptr; });
    if (unlikely(relation->indexInfo == nullptr)) {
      CDE_LOG_ERROR("Mem alloc fail when clone relation, oid:%u.",
                    src_rel->relOid);
      return CDE_FAIL;
    }
    (void)memcpy_s(relation->indexInfo, alloc_len, src_rel->indexInfo,
                   alloc_len);

    relation->index =
        CdeBuildIndexInfo(src_rel, relation, src_rel->index->indexKeyAttrsNum,
                          src_rel->index->isUnique, mem);
    DBUG_EXECUTE_IF("build_index_info_failed_injection",
                    { relation->index = nullptr; });
    if (!relation->index) {
      CDE_LOG_ERROR("Build index info fail when clone relation, oid:%u.",
                    src_rel->relOid);
      return CDE_FAIL;
    }
  }

  dst_rel = relation;
  return CDE_SUCC;
}

void cde_relation::GetScanFuncByColAttr(ScanKey scan_key,
                                        DSTORE::Form_pg_attribute attr) {
  const DSTORE::FuncCache *cache =
      CdeGetDstoreFuncCache(attr->atttypid, attr->atttypid, MAINTAIN_ORDER);
  if (cache->fnAddr == nullptr) {
    cache = CdeGetFuncCache(attr->atttypid, attr->atttypid, MAINTAIN_ORDER);
  }
  CDE_ASSERT(cache->fnAddr != nullptr);
  auto *type_key_cache = cache;
  scan_key->skFunc.fnAddr = type_key_cache->fnAddr;
  scan_key->skFunc.fnOid = type_key_cache->fnOid;
  scan_key->skFunc.fnNargs = 2;
  scan_key->skFunc.fnStrict = true;
  scan_key->skFunc.fnRetset = false;
  scan_key->skFunc.fnExtra =
      DSTORE::FnExtraMake(attr->atttypid, attr->extAttrs);
  scan_key->skFunc.fnMcxt =
      nullptr;  // g_dstoreCurrentMemoryContext; todo: uses
  scan_key->skFlags = SCAN_KEY_ROW_MEMBER;
  scan_key->skCollation = attr->attcollation;
}

ScanKey ConstructScanKey(TupleDesc attr, MEM_ROOT *mem_root) {
  uint32_t index_column_num = (uint32_t)attr->natts;
  ScanKey key_infos = nullptr;
  /* FIXME: Memory allocation will be refactored in FE2025032500078. */
  if (mem_root) {
    key_infos = mem_root->ArrayAlloc<ScanKeyData>(index_column_num);
  } else {
    key_infos =
        static_cast<ScanKey>(CdeZalloc(index_column_num * sizeof(ScanKeyData)));
  }
  if (key_infos == nullptr) {
    CDE_LOG_ERROR("alloc failed for scankey!");
    return nullptr;
  }
  for (uint32_t i = 0; i < index_column_num; i++) {
    /* get attribute number of key */
    /* get scanKey func by attribute type */
    cde_relation::GetScanFuncByColAttr(&key_infos[i], attr->attrs[i]);
    key_infos[i].skAttno = static_cast<AttrNumber>(i + 1);
  }
  return key_infos;
}

bool CdeGenSessionIndexInfo(cde_dict_index_t *origin_idx, MEM_ROOT *mem_root,
                            session_index_info *&idx_info) {
  session_index_info *copy_idx = static_cast<session_index_info *>(
      mem_root->Alloc(sizeof(session_index_info)));
  DBUG_EXECUTE_IF("sess_idx_Alloc_injection", { copy_idx = nullptr; });
  if (unlikely(copy_idx == nullptr)) {
    CDE_LOG_ERROR("Mem alloc fail at %s.", __func__);
    return CDE_FAIL;
  }

  DSTORE::StorageRelation index_rel = nullptr;
  bool ret = cde_relation::Clone(origin_idx->rel, mem_root,
                                 origin_idx->fillfactor, index_rel);
  if (ret == CDE_FAIL) {
    return CDE_FAIL;
  }
  copy_idx->rel = index_rel;

  // scan key
  ScanKey copy_scan_key = ConstructScanKey(index_rel->attr, mem_root);
  DBUG_EXECUTE_IF("scan_key_cons_injection", { copy_scan_key = nullptr; });
  if (!copy_scan_key) {
    return CDE_FAIL;
  }
  copy_idx->scan_key = copy_scan_key;
  copy_idx->shared_dict = origin_idx;

  idx_info = copy_idx;
  return CDE_SUCC;
}

bool CdeCopyIndexes(cde_dict_t *dict, MEM_ROOT *mem_root,
                    std::vector<session_index_info *> &out_index) {
  // loop rel's indexes to copy
  for (const auto &entry : dict->index_dict_vec) {
    if (!entry->IsCommitted()) {
      continue;
    }

    session_index_info *idx_info = nullptr;
    bool ret = CdeGenSessionIndexInfo(entry, mem_root, idx_info);
    if (ret == CDE_FAIL) {
      return ret;
    }
    out_index.emplace_back(idx_info);
  }
  return CDE_SUCC;
}

bool CdeCopyRelInfoToLocalStorage(cde_dict_t *table,
                                  session_rel_info *relInfo) {
  if (cde_relation::Clone(table->dstore_relation, &relInfo->m_relInfoMemRoot,
                          table->fillfactor,
                          relInfo->rd_storage_releation) == CDE_FAIL) {
    return CDE_FAIL;
  }

  // copy indexes
  if (CdeCopyIndexes(table, &relInfo->m_relInfoMemRoot,
                     relInfo->cde_index_vec) == CDE_FAIL) {
    return CDE_FAIL;
  }

  cde_perf_counters::getInstance()->_session_rel_info_mem_allocted.increment(
      relInfo->m_relInfoMemRoot.allocated_size());
  return CDE_SUCC;
}

int64_t CdeConstructRelInfoForInplaceAlter(cde_dict_t *table,
                                           session_rel_info *relInfo,
                                           cde_dict_index_t **addIndexes,
                                           uint32_t addNum) {
  // copy heap relation
  bool ret =
      cde_relation::Clone(table->dstore_relation, &relInfo->m_relInfoMemRoot,
                          table->fillfactor, relInfo->rd_storage_releation);
  if (ret == CDE_FAIL) return CDE_ERROR;

  // copy indexes relation
  for (uint32_t i = 0; i < addNum; i++) {
    session_index_info *idxInfo = nullptr;
    ret = CdeGenSessionIndexInfo(addIndexes[i], &relInfo->m_relInfoMemRoot,
                                 idxInfo);
    if (ret == CDE_FAIL) {
      return CDE_ERROR;
    }
    relInfo->cde_index_vec.emplace_back(idxInfo);
  }

  return CDE_OK;
}

session_index_info *CdeGetRelationIndexInfo(cde_dict_index_t *shared_dict,
                                            session_rel_info *rel_info) {
  for (session_index_info *index_dict : rel_info->cde_index_vec) {
    if (index_dict->shared_dict == shared_dict) {
      return index_dict;
    }
  }
  return nullptr;
}

void session_rel_info::Destroy() {
  if (rd_storage_releation) {
    rd_storage_releation->Destroy();
    rd_storage_releation = nullptr;
  }
  for (const auto &entry : cde_index_vec) {
    entry->rel->Destroy();
  }
  cde_index_vec.clear();
  cde_perf_counters::getInstance()->_session_rel_info_mem_allocted.decrement(
      m_relInfoMemRoot.allocated_size());
}

} /* namespace CDE */
