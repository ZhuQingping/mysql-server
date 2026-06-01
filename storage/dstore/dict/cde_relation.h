/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * cde_relation.h
 *
 *
 * -------------------------------------------------------------------------
 */

#ifndef __CDE_RELATION_H__
#define __CDE_RELATION_H__

#include "systable/dstore_relation.h"

#include "dict/cde_dict.h"

#include "my_alloc.h"

using DSTORE::ScanKey;
using DSTORE::StorageRelation;
namespace CDE {

// adaptor func use for dstore
class cde_relation {
 public:
  /**
  Clone new StorageRelation from src_rel, allocated mem from MEM_ROOT.
  todo: need to remove parameter 'fillfactor' once StorageRelationData
  provide an interface to get fillfactor information.

  @param[in]  src_rel DStore relation data.
  @param[in]  mem     mem_root to allocate from.
  @param[in]  fillfactor table or index fillfactor value.
  @param[out] dst new DStore ralation data.

  @return CDE_SUCC if success, otherwise CDE_FAIL.
  */
  static bool Clone(StorageRelation src, MEM_ROOT *memRoot,
                    const int32_t fillfactor, StorageRelation &dst);
  static void GetScanFuncByColAttr(ScanKey scan_key,
                                   DSTORE::Form_pg_attribute attr);
};

// StorageRelation is not thread-safe
// this struct is session specific, all fields are deep-copied from
// cde_dict_table
struct session_index_info {
  DSTORE::StorageRelation rel;
  DSTORE::ScanKey scan_key;
  cde_dict_index_t *shared_dict;
};

struct session_rel_info {
  DSTORE::StorageRelationData *rd_storage_releation = nullptr;
  std::vector<session_index_info *> cde_index_vec;
  session_rel_info(uint64_t tableId) : m_tableId(tableId) {}
  ~session_rel_info() { Destroy(); }
  // only release dstore resource
  void Destroy();
  /** mem_root for allocating relation info.*/
  uint64_t m_tableId;
  MEM_ROOT m_relInfoMemRoot;
};

bool CdeGenSessionIndexInfo(cde_dict_index_t *origin_idx, MEM_ROOT *mem_root,
                            session_index_info *&idx_info);

bool CdeCopyIndexes(cde_dict_t *dict, MEM_ROOT *mem_root,
                    std::vector<session_index_info *> &out_index);

bool CdeCopyRelInfoToLocalStorage(cde_dict_t *table, session_rel_info *relInfo);

/**
Construct relation info for inpalce add indexes.

@param[in]      table    the altered cde_dict_table object
@param[in/out]  relInfo  the constructed heap relation and indexes relation
@param[in]      addIndexes  the indexes array to be added
@param[in]      addNum      the number of indexes to be added
@return CDE_OK if success
*/
int64_t CdeConstructRelInfoForInplaceAlter(cde_dict_t *table,
                                           session_rel_info *relInfo,
                                           cde_dict_index_t **addIndexes,
                                           uint32_t addNum);

session_index_info *CdeGetRelationIndexInfo(cde_dict_index_t *shared_dict,
                                            session_rel_info *rel_info);

/**
Construct array of sruct ScanKeyData, alloc memory in the function.

@param[in]  attr    Attribute information of the tuple.
@param[in]  memRoot MEM_ROOT to use for memory allocation. Alloc memory
                    by this function self if memRoot is nullptr.
@return address of the alloced array of sruct ScanKeyData, nullptr if failed.
*/
ScanKey ConstructScanKey(DSTORE::TupleDesc attr, MEM_ROOT *memRoot = nullptr);

} /* namespace CDE */
#endif  //__CDE_RELATION_H__
