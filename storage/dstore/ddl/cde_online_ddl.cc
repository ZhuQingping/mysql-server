/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/
#include <iomanip>
#include <thread>

#include "include/scope_guard.h"
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/sql_table.h"
#include "sql/sql_thd_internal_api.h"

#include "boot/cde_instance.h"
#include "common/cde_alloc.h"
#include "common/cde_compare_utils.h"
#include "common/cde_errorcode.h"
#include "common/cde_mutex.h"
#include "common/cde_typecache.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_online_ddl.h"
#include "ddl/ha_cde_alter.h"
#include "dict/cde_dict.h"
#include "dml/cde_dml.h"
#include "dml/cde_heap.h"
#include "my_checksum.h"
#include "mysql/plugin.h"

#include "catalog/dstore_fake_type.h"
#include "pdb/dstore_pdb_interface.h"
#include "tuple/dstore_tuple_interface.h"
#include "undo/dstore_undo_types.h"

namespace CDE {

/** Memory row log buffer size for online index creation. */
ulong g_rowLogBufSize = 1048576;
/** Maximum row log file size for online index creation. */
ulonglong g_rowLogFileMaxSize = 128 << 20;
#ifndef NDEBUG
bool g_rowLogPrint = false;
#endif
/** Array of characters of the online ddl status names */
static std::string g_onlineDdlStatusStr[] = {
    "Invalid", "Creating index", "Rebuilding table", "Committing", "Aborted"};
/* Whether check the consistency of heap and index data after using online ddl
to create index */
uint g_onlineDdlCheckDataMode = (uint)OnlineDdlCheckMode::INC_CHECK;

/** Whether enable online ddl */
bool g_onlineDdlEnable;

bool g_onlineAddIndexOnNewVcolEnable;

static void ConvertTupleDataToStr(const char *data, size_t size,
                                  std::stringstream &ss) {
#ifndef NDEBUG
  for (size_t i = 0; i < size; ++i) {
    unsigned char c = static_cast<unsigned char>(data[i]);
    if (c >= 32 && c <= 126) {
      if (c == '\\' || c == '"') {
        ss << '\\';
      }
      ss << c;
    } else {
      ss << '\\';
      ss << std::oct << std::setw(3) << std::setfill('0')
         << static_cast<int>(c);
      ss << std::dec;
    }
  }
#else
  ss << "can't print in release version";
#endif
}

TrxTuplesSaver::~TrxTuplesSaver() {
  for (uint32_t i = 0; i < MAX_HEAP_CHUNK_NUM; i++) {
    if (m_oldTuples[i]) {
      TupleInterface::DestroyTuple(m_oldTuples[i], CdeFreeMemForDtuple);
      m_oldTuples[i] = nullptr;
    }
    if (m_newTuples[i]) {
      TupleInterface::DestroyTuple(m_newTuples[i], CdeFreeMemForDtuple);
      m_newTuples[i] = nullptr;
    }
  }
  if (m_assembledOldTuple) {
    TupleInterface::DestroyTuple(m_assembledOldTuple, CdeFreeMemForDtuple);
    m_assembledOldTuple = nullptr;
  }
  if (m_assembledNewTuple) {
    TupleInterface::DestroyTuple(m_assembledNewTuple, CdeFreeMemForDtuple);
    m_assembledNewTuple = nullptr;
  }
}

void TrxTuplesSaver::GatherNoLinkedTuples(DSTORE::ExposedUndoType undoType,
                                          DSTORE::HeapTuple *oldTuple,
                                          DSTORE::HeapTuple *newTuple,
                                          TuplesGatherResult &result) {
  if (undoType ==
      DSTORE::ExposedUndoType::HEAP_ANOTHER_PAGE_APPEND_UPDATE_OLD_PAGE) {
    CDE_ASSERT(oldTuple != nullptr);
    CDE_ASSERT(newTuple == nullptr);
    CDE_ASSERT(!TupleInterface::IsLinked(oldTuple));
    CDE_ASSERT(m_oldChunkNum == 0);
    CDE_ASSERT(m_oldTuples[0] == nullptr);
    CDE_ASSERT(m_newChunkNum == 0);
    CDE_ASSERT(m_newTuples[0] == nullptr);
    m_oldTuples[0] = oldTuple;
    ++m_oldChunkNum;
  } else {
    CDE_ASSERT(
        undoType ==
        DSTORE::ExposedUndoType::HEAP_ANOTHER_PAGE_APPEND_UPDATE_NEW_PAGE);
    CDE_ASSERT(oldTuple == nullptr);
    CDE_ASSERT(newTuple != nullptr);
    CDE_ASSERT(!TupleInterface::IsLinked(newTuple));
    CDE_ASSERT(m_oldChunkNum == 1);
    CDE_ASSERT(m_oldTuples[0] != nullptr);
    CDE_ASSERT(m_newChunkNum == 0);
    CDE_ASSERT(m_newTuples[0] == nullptr);
    m_newTuples[0] = newTuple;
    ++m_newChunkNum;
    result.m_isGatherEnd = true;
    result.m_newTuple = newTuple;
    result.m_oldTuple = m_oldTuples[0];
  }
}

void TrxTuplesSaver::GatherLinkedTuples(const DSTORE::UndoExtraInfo *info,
                                        DSTORE::HeapTuple *oldTuple,
                                        DSTORE::HeapTuple *newTuple) {
  uint32_t chunkNo = info->m_chunkNo;
  CDE_ASSERT(chunkNo < MAX_HEAP_CHUNK_NUM);
  if (oldTuple) {
    CDE_ASSERT(m_oldTuples[chunkNo] == nullptr);
    m_oldTuples[chunkNo] = oldTuple;
    ++m_oldChunkNum;
  }
  if (newTuple) {
    CDE_ASSERT(m_newTuples[chunkNo] == nullptr);
    m_newTuples[chunkNo] = newTuple;
    ++m_newChunkNum;
  }
}

bool TrxTuplesSaver::IsLinkedTuplesGartherComplete() const {
  if (IsLinkedTuplesGartherCompleteInner(true) == false ||
      IsLinkedTuplesGartherCompleteInner(false) == false) {
    return false;
  }
  return true;
}

bool TrxTuplesSaver::IsLinkedTuplesGartherCompleteInner(bool isOld) const {
  DSTORE::HeapTuple *curChunk = nullptr;
  DSTORE::HeapTuple *preChunk = nullptr;
  DSTORE::HeapTuple *const *tupleArray = nullptr;
  uint32_t chunkNum = 0;
  std::string msg;
  if (isOld) {
    tupleArray = m_oldTuples;
    chunkNum = m_oldChunkNum;
    msg = "old";
  } else {
    tupleArray = m_newTuples;
    chunkNum = m_newChunkNum;
    msg = "new";
  }

  for (uint32_t i = 0; i < chunkNum; i++) {
    curChunk = tupleArray[i];
    if (curChunk == nullptr) {
      CDE_LOG_ERROR(
          "Loss of %s linked tuple, xid:[%u,%lu], chunkNo:%u, total "
          "chunknum:%u.",
          msg.c_str(), (uint32_t)m_xid.m_zoneId, m_xid.m_logicSlotId, i,
          chunkNum);
      return false;
    }
    if (preChunk) {
      if (TupleInterface::GetNextChunkCtid(preChunk) !=
          TupleInterface::GetCtid(curChunk)) {
        DSTORE::ItemPointerData preNextCtid =
            TupleInterface::GetNextChunkCtid(preChunk);
        DSTORE::ItemPointerData curCtid = TupleInterface::GetCtid(curChunk);
        CDE_LOG_ERROR(
            "Broken of %s linked tuple, xid:[%u,%lu], chunkNo:%u, total "
            "chunknum:%u, "
            "pre chunk's next ctid:[%hu,%u] %hu, cur chunk's ctid[%hu,%u] %hu.",
            msg.c_str(), (uint32_t)m_xid.m_zoneId, m_xid.m_logicSlotId, i,
            chunkNum, preNextCtid.val.m_pageid.m_fileId,
            preNextCtid.val.m_pageid.m_blockId, preNextCtid.val.m_offset,
            curCtid.val.m_pageid.m_fileId, curCtid.val.m_pageid.m_blockId,
            curCtid.val.m_offset);
        return false;
      }
    }
    preChunk = curChunk;
  }

  if (curChunk) {
    DSTORE::ItemPointerData curCtid = TupleInterface::GetCtid(curChunk);
    if (chunkNum == 1) {
      if (TupleInterface::IsLinked(curChunk)) {
        /* Expect a small tuple but is actual a linked tuple. */
        CDE_LOG_ERROR(
            "%s tuple is a linked tuple while only one chunk garthered, "
            "xid:[%u,%lu], "
            "total chunknum:%u, ctid[%hu,%u] %hu.",
            msg.c_str(), (uint32_t)m_xid.m_zoneId, m_xid.m_logicSlotId,
            chunkNum, curCtid.val.m_pageid.m_fileId,
            curCtid.val.m_pageid.m_blockId, curCtid.val.m_offset);
        return false;
      }
    } else {
      if (TupleInterface::GetNextChunkCtid(curChunk) !=
          DSTORE::INVALID_ITEM_POINTER) {
        CDE_LOG_ERROR(
            "%s tuple has a broken link, xid:[%u,%lu], total chunknum:%u, last "
            "chunk's ctid[%hu,%u] %hu, which is expected to be invalid ctid.",
            msg.c_str(), (uint32_t)m_xid.m_zoneId, m_xid.m_logicSlotId,
            chunkNum, curCtid.val.m_pageid.m_fileId,
            curCtid.val.m_pageid.m_blockId, curCtid.val.m_offset);
        return false;
      }
    }
  }
  return true;
}

bool TrxTuplesSaver::AssembleLinkedTuples(TuplesGatherResult &result) {
  result.Reset();
  if (m_newChunkNum > 1) {
    result.m_newTuple = m_assembledNewTuple =
        HeapInterface::AssembleTuples(m_newTuples, m_newChunkNum);
    if (result.m_newTuple == nullptr) {
      CDE_LOG_ERROR("Assemble new linked tuple fail.");
      return CDE_FAIL;
    }
  } else if (m_newChunkNum == 1) {
    CDE_ASSERT(m_newTuples[0] != nullptr);
    result.m_newTuple = m_newTuples[0];
  }
  if (m_oldChunkNum > 1) {
    result.m_oldTuple = m_assembledOldTuple =
        HeapInterface::AssembleTuples(m_oldTuples, m_oldChunkNum);
    if (result.m_oldTuple == nullptr) {
      CDE_LOG_ERROR("Assemble old linked tuple fail.");
      return CDE_FAIL;
    }
  } else if (m_oldChunkNum == 1) {
    CDE_ASSERT(m_oldTuples[0] != nullptr);
    result.m_oldTuple = m_oldTuples[0];
  }
  CDE_ASSERT(result.m_oldTuple != nullptr || result.m_newTuple != nullptr);
  result.m_isGatherEnd = true;
  return CDE_SUCC;
}

bool RollbackedTuplesGatherer::NeedGatherTuple(
    const DSTORE::UndoExtraInfo *info, DSTORE::ExposedUndoType undoType,
    DSTORE::HeapTuple *oldTuple, DSTORE::HeapTuple *newTuple) {
  if (undoType < DSTORE::ExposedUndoType::HEAP_INSERT ||
      undoType >
          DSTORE::ExposedUndoType::HEAP_ANOTHER_PAGE_APPEND_UPDATE_NEW_PAGE ||
      undoType == DSTORE::ExposedUndoType::HEAP_BATCH_INSERT) {
    /* Undo type is invalid, should not happen. */
    CDE_LOG_FATAL("Undo type:%u is invalid.", (uint32_t)undoType);
  }

  if (undoType !=
          DSTORE::ExposedUndoType::HEAP_ANOTHER_PAGE_APPEND_UPDATE_OLD_PAGE &&
      undoType !=
          DSTORE::ExposedUndoType::HEAP_ANOTHER_PAGE_APPEND_UPDATE_NEW_PAGE &&
      info->m_chunkNo == UINT32_MAX) {
    CDE_ASSERT(info->m_undoSeq == DSTORE::UndoSeq::INVALID);
    if (oldTuple) {
      CDE_ASSERT(!TupleInterface::IsLinked(oldTuple));
    }
    if (newTuple) {
      CDE_ASSERT(!TupleInterface::IsLinked(newTuple));
    }
    return false;
  }
  return true;
}

bool RollbackedTuplesGatherer::IsTrxTupleSaverExists(uint64_t xid) {
  int32_t err = pthread_rwlock_rdlock(&m_gathererLock);
  CDE_ASSERT(err == 0);
  auto iter = m_trxTupleSavers.find(xid);
  bool retval = iter != m_trxTupleSavers.end();
  err = pthread_rwlock_unlock(&m_gathererLock);
  CDE_ASSERT(err == 0);
  return retval;
}

TrxTuplesSaver *RollbackedTuplesGatherer::GetOrCreateTrxTupleSaver(
    uint64_t xid) {
  int32_t err = pthread_rwlock_wrlock(&m_gathererLock);
  CDE_ASSERT(err == 0);
  auto iter = m_trxTupleSavers.find(xid);
  if (iter == m_trxTupleSavers.end()) {
    m_trxTupleSavers.emplace(xid, TrxTuplesSaver{xid});
    iter = m_trxTupleSavers.find(xid);
  }
  TrxTuplesSaver &saver = iter->second;
  err = pthread_rwlock_unlock(&m_gathererLock);
  CDE_ASSERT(err == 0);
  return &saver;
}

void RollbackedTuplesGatherer::ClearTrxTupleSaver(uint64_t xid) {
  int32_t err = pthread_rwlock_wrlock(&m_gathererLock);
  CDE_ASSERT(err == 0);
  size_t eraseNum = m_trxTupleSavers.erase(xid);
  CDE_ASSERT(eraseNum == 1);
  err = pthread_rwlock_unlock(&m_gathererLock);
  CDE_ASSERT(err == 0);
}

bool RollbackedTuplesGatherer::GatherAndAssembleTuples(
    uint64_t xid, const DSTORE::UndoExtraInfo *info,
    DSTORE::ExposedUndoType undoType, DSTORE::HeapTuple *oldTuple,
    DSTORE::HeapTuple *newTuple, TuplesGatherResult &result) {
  result.Reset();
  if (info->m_chunkNo == UINT32_MAX) {
    /* Deal with nolinked-tuple. */
    CDE_ASSERT(info->m_undoSeq == DSTORE::UndoSeq::INVALID);
    TrxTuplesSaver *saver = GetOrCreateTrxTupleSaver(xid);
    saver->GatherNoLinkedTuples(undoType, oldTuple, newTuple, result);
    return CDE_SUCC;
  }

  /* Deal with linked-tuple. */
  CDE_ASSERT(info->m_undoSeq != DSTORE::UndoSeq::INVALID);
  if (info->m_undoSeq == DSTORE::UndoSeq::FIRST) {
    CDE_ASSERT(!IsTrxTupleSaverExists(xid));
  }

  TrxTuplesSaver *saver = GetOrCreateTrxTupleSaver(xid);
  saver->GatherLinkedTuples(info, oldTuple, newTuple);
  if (info->m_undoSeq == DSTORE::UndoSeq::LAST) {
    /* We have gathered all linked tuples and need to check them and
    assemble them into entire heap tuples. */
    CDE_ASSERT(saver->IsLinkedTuplesGartherComplete());
    return saver->AssembleLinkedTuples(result);
  }
  return CDE_SUCC;
}

void OnlineDdlErrInfo::SetErrInfo(OnlineDdlErrcode errcode, uint32_t errIndexNo,
                                  const DSTORE::IndexTuple *dupTuple) {
  CdeMutexGuard dictMutexGuard(&m_mtx, CDE_LOCATION_HERE);
  if (m_isSet) {
    return;
  }

  if (dupTuple) {
    CDE_ASSERT(m_errTuple == nullptr);
    uint32_t tupleSize = dupTuple->GetSize();
    m_errTuple = (DSTORE::IndexTuple *)Memory::Malloc(tupleSize);
    if (m_errTuple) {
      errno_t err = memcpy_s(static_cast<void *>(m_errTuple), tupleSize,
                             static_cast<const void *>(dupTuple), tupleSize);
      if (err != 0) {
        CDE_LOG_ERROR("Copy duplicate index tuple fail, size:%u.", tupleSize);
        Memory::Free(m_errTuple);
        m_errTuple = nullptr;
      }
    } else {
      CDE_LOG_ERROR("Alloc duplicate index tuple fail, size:%u.", tupleSize);
    }
  }
  m_errcode = errcode;
  m_errIndexNo = errIndexNo;
  m_isSet = true;
}

OnlineDdlMgr::OnlineDdlMgr(const TABLE *mysqlTableOld,
                           const TABLE *mysqlTableNew,
                           const dd::Table *ddTableNew, ha_cde_inplace_ctx *ctx,
                           DSTORE::ThreadContext *thrd, my_thread_id threadId)
    : m_mysqlTableOld(mysqlTableOld),
      m_mysqlTableNew(mysqlTableNew),
      m_ddTableNew(ddTableNew),
      m_ctx(ctx),
      m_thrd(thrd),
      m_threadId(threadId) {
  CDE_ASSERT_DEBUG(mysqlTableOld != nullptr);
  CDE_ASSERT_DEBUG(ddTableNew != nullptr);
  CDE_ASSERT_DEBUG(ctx != nullptr);
  int32_t ret = MutexInit(0, &m_heapIncDataMtx, MY_MUTEX_INIT_FAST);
  CDE_ASSERT(ret == 0);
}

bool OnlineDdlMgr::Init() {
  for (uint32_t i = 0; i < m_ctx->num_to_add_index; i++) {
    cde_dict_index_t *dictIndex = m_ctx->add_index[i];
    CDE_ASSERT_DEBUG(dictIndex != nullptr);
    RowLog *rowlogPtr = Memory::New<RowLog>(this);
    if (rowlogPtr == nullptr) {
      CDE_LOG_ERROR("Allocate row log fail for table:%s index %s.",
                    GetTableNameOld(), dictIndex->name.c_str());
      return CDE_FAIL;
    }
    if (rowlogPtr->Init(dictIndex)) {
      CDE_LOG_ERROR("Init row log fail for table:%s index %s.",
                    GetTableNameOld(), dictIndex->name.c_str());
      Memory::Delete(rowlogPtr);
      return CDE_FAIL;
    }
    dictIndex->m_rowLog = rowlogPtr;
  }
  m_checkMode = static_cast<OnlineDdlCheckMode>(g_onlineDdlCheckDataMode);
  return CDE_SUCC;
}

OnlineDdlMgr::~OnlineDdlMgr() {
  /* We must ensure no destructing between StartRowLogReplayForIndex and
  WaitAllRowLogReplayThreadsExit, thus can ensure m_workers is always empty
  at destructor of OnlineDdlMgr. */
  CDE_ASSERT(m_workers.empty());
  for (uint32_t i = 0; i < m_ctx->num_to_add_index; i++) {
    cde_dict_index_t *dictIndex = m_ctx->add_index[i];
    Memory::Delete(dictIndex->m_rowLog);
    dictIndex->m_rowLog = nullptr;
  }
  int32_t ret = MutexDestroy(&m_heapIncDataMtx);
  CDE_ASSERT(ret == 0);
}

bool OnlineDdlMgr::RowLogRollbackForIndex(DSTORE::HeapTuple *oldTuple,
                                          DSTORE::HeapTuple *newTuple) {
  CDE_ASSERT(GetStatus() == OnlineDdlStatus::CREATING_INDEX);
  /* Open a temporary TABLE object for each concurrent dml session. */
  CDE_ASSERT_DEBUG(current_thd != nullptr);

  IndexRowOp opType = ROW_IDX_OP_NONE;
  DSTORE::HeapTuple *heapTuple = nullptr;
  DSTORE::HeapTuple *heapTupleNew = nullptr;

  if (oldTuple == nullptr) {
    opType = ROW_IDX_DELETE;
    heapTuple = newTuple;
  } else if (newTuple == nullptr) {
    opType = ROW_IDX_INSERT;
    heapTuple = oldTuple;
  } else {
    CDE_ASSERT(oldTuple != nullptr && newTuple != nullptr);
    opType = ROW_IDX_DELETE;
    /* The meaning of "new/old" for RowlogForIndexInner is opposite
    of the meaning of "new/old" for undo record at rollback scene. */
    heapTuple = newTuple;
    heapTupleNew = oldTuple;
  }

  /* relInfo->rd_storage_releation->attr->attrs->attcacheoff is a member of
  heap tupleDesc, that is modified in the call-trace below:
  TupleInterface::FormIndexTuple
    IndexTuple::FormTuple
      HeapTuple::GetAttr
        HeapTuple::CalculateOffset
  Therefore, we construct a temporary session_rel_info object which will deep
  copy the tupleDesc at every dml row log. */
  auto relInfo = std::make_unique<session_rel_info>(m_ctx->old_table->m_id);
  if (CDE_OK != CdeConstructRelInfoForInplaceAlter(
                    m_ctx->old_table, relInfo.get(), m_ctx->add_index,
                    m_ctx->num_to_add_index)) {
    CDE_LOG_ERROR("Open TABLE object fail at row log rollback, table:%s.",
                  GetTableNameOld());
    return CDE_FAIL;
  }

  return RowlogForIndexInner(relInfo.get(), opType, heapTuple, heapTupleNew,
                             __func__);
}

void OnlineDdlMgr::RowlogDmlForIndex(IndexRowOp opType, DSTORE::Datum *values,
                                     bool *isNulls, DSTORE::ItemPointer ctid,
                                     DSTORE::Datum *valuesNew, bool *isNullsNew,
                                     DSTORE::ItemPointer ctidNew) {
  bool isUpdate = false;
  if (valuesNew != nullptr) {
    CDE_ASSERT(isNullsNew != nullptr);
    CDE_ASSERT(ctidNew != nullptr);
    isUpdate = true;
  }

  /* relInfo->rd_storage_releation->attr->attrs->attcacheoff is a member of
  heap tupleDesc, that is modified in the call-trace below:
  TupleInterface::FormIndexTuple
    IndexTuple::FormTuple
      HeapTuple::GetAttr
        HeapTuple::CalculateOffset
  Therefore, we construct a temporary session_rel_info object which will deep
  copy the tupleDesc at every dml row log. */
  auto relInfo = std::make_unique<session_rel_info>(m_ctx->old_table->m_id);
  if (CDE_OK != CdeConstructRelInfoForInplaceAlter(
                    m_ctx->old_table, relInfo.get(), m_ctx->add_index,
                    m_ctx->num_to_add_index)) {
    CDE_LOG_ERROR(
        "Copy relation info fail when row log dml for index, table:%s",
        GetTableNameOld());
    SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    return;
  }

  DSTORE::TupleDesc heapAttr = relInfo->rd_storage_releation->attr;
  DSTORE::HeapTuple *heapTuple =
      TupleInterface::FormHeapTuple(heapAttr, values, isNulls, nullptr);
  TupleInterface::SetCtid(heapTuple, *ctid);

  DSTORE::HeapTuple *heapTupleNew = nullptr;
  if (isUpdate) {
    heapTupleNew =
        TupleInterface::FormHeapTuple(heapAttr, valuesNew, isNullsNew, nullptr);
    TupleInterface::SetCtid(heapTupleNew, *ctidNew);
  }

  bool ret = RowlogForIndexInner(relInfo.get(), opType, heapTuple, heapTupleNew,
                                 __func__);
  if (ret == CDE_FAIL) {
    CDE_LOG_ERROR("Row log dml for index fail, table:%s", GetTableNameOld());
    SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    return;
  }

  TupleInterface::DestroyTuple(heapTuple, nullptr);
  TupleInterface::DestroyTuple(heapTupleNew, nullptr);
}

bool OnlineDdlMgr::RowlogForIndexInner(session_rel_info *relInfo,
                                       IndexRowOp opType,
                                       DSTORE::HeapTuple *heapTuple,
                                       DSTORE::HeapTuple *heapTupleNew,
                                       const char *caller) {
  bool isUpdate = false;
  DSTORE::ItemPointer ctid = TupleInterface::GetCtidPtr(heapTuple);
  CDE_ASSERT(*ctid != DSTORE::INVALID_ITEM_POINTER);
  DSTORE::ItemPointer ctidNew = nullptr;
  if (heapTupleNew != nullptr) {
    isUpdate = true;
    ctidNew = TupleInterface::GetCtidPtr(heapTupleNew);
    CDE_ASSERT(*ctidNew != DSTORE::INVALID_ITEM_POINTER);
  }

  THD *origThd = current_thd;
  THD *tmpThd = create_internal_thd();
  tmpThd->set_new_thread_id();
  TABLE *mysqlTableNew = open_table_uncached(
      tmpThd, m_mysqlTableNew->s->path.str, m_mysqlTableNew->s->db.str,
      m_mysqlTableNew->s->table_name.str, false, false, *m_ddTableNew);
  if (mysqlTableNew == nullptr) {
    CDE_LOG_ERROR("Construct new TABLE object fail, table:%s",
                  GetTableNameOld());
    return CDE_FAIL;
  }

  /*  Auto clean up. */
  auto autoCleanup = create_scope_guard([&]() {
    intern_close_table(mysqlTableNew);
    destroy_internal_thd(tmpThd);
    if (origThd) {
      origThd->store_globals();
    }
  });

  DSTORE::StorageRelationData *oldHeapRel = relInfo->rd_storage_releation;
  DSTORE::TupleDesc oldHeapAttr = relInfo->rd_storage_releation->attr;
  DSTORE::Datum indexValues[DSTORE::INDEX_MAX_KEY_NUM];
  bool indexIsNulls[DSTORE::INDEX_MAX_KEY_NUM];

  DSTORE::FunctionCallInfoData fcInfo;
  fcInfo.prealloc_arg[1] = PointerGetDatum(indexValues);
  fcInfo.prealloc_arg[2] = PointerGetDatum(indexIsNulls);

  for (uint32_t i = 0; i < m_ctx->num_to_add_index; i++) {
    cde_dict_index_t *dictIndex = m_ctx->add_index[i];
    DSTORE::IndexBuildInfo buildInfo = CreateIndexBuildInfo(
        oldHeapRel, oldHeapRel->attr, m_ctx->add_index[i]->index_col_num,
        m_ctx->add_index[i]->attr_cols, m_ctx->add_index[i]->fields);
    buildInfo.baseInfo = *(relInfo->cde_index_vec[i]->rel->index);
    buildInfo.baseInfo.indexRelId = relInfo->cde_index_vec[i]->rel->relOid;

    ConvertHeapValueToIndexValue ConvertFunctor(
        m_mysqlTableOld, mysqlTableNew, m_ctx->old_table, oldHeapAttr,
        m_ctx->new_table, dictIndex, nullptr, m_ctx->new_addedVirtualCols,
        m_ctx->old_existVirtualCols, false);
    /*  Auto clean up memory */
    auto autoCleanupInner = create_scope_guard([&]() {
      ConvertFunctor.destroy();
      CdeFree(buildInfo.heapRels);
    });
    if (!ConvertFunctor.m_canMultiThread) {
      ConvertFunctor.init();
      fcInfo.prealloc_arg[0] = PointerGetDatum(heapTuple);
      if (ConvertFunctor(&fcInfo) == (DSTORE::Datum)DSTORE::DSTORE_FAIL) {
        CDE_LOG_ERROR("Convert index tuple fail, table:%s index:%s",
                      GetTableNameOld(), dictIndex->name.c_str());
        return CDE_FAIL;
      }
    }
    DSTORE::IndexTuple *indexTuple = TupleInterface::FormIndexTuple(
        heapTuple, &buildInfo, indexValues, indexIsNulls);
    if (indexTuple == nullptr) {
      CDE_LOG_ERROR("Form index tuple faile, table:%s index:%s",
                    GetTableNameOld(), dictIndex->name.c_str());
      return CDE_FAIL;
    }
    indexTuple->SetHeapCtid(ctid);

    if (!isUpdate) {
      IndexRowLogItem logItem(opType, indexTuple);
      if (dictIndex->m_rowLog->AppendLogItem(logItem, i) == CDE_FAIL) {
        CDE_LOG_ERROR("Append row log item fail, table:%s index:%s",
                      GetTableNameOld(), dictIndex->name.c_str());
        return CDE_FAIL;
      }
#ifndef NDEBUG
      if (g_rowLogPrint) {
        CDE_LOG_INFO(
            "Append row log of index success, %s, table:%s index:%s caller:%s.",
            logItem.ToString().c_str(), GetTableNameOld(),
            dictIndex->name.c_str(), caller);
      }
#endif
    } else {
      if (!ConvertFunctor.m_canMultiThread) {
        fcInfo.prealloc_arg[0] = PointerGetDatum(heapTupleNew);
        if (ConvertFunctor(&fcInfo) == (DSTORE::Datum)DSTORE::DSTORE_FAIL) {
          CDE_LOG_ERROR("Convert index tuple fail, table:%s index:%s",
                        GetTableNameOld(), dictIndex->name.c_str());
          return CDE_FAIL;
        }
      }

      DSTORE::IndexTuple *indexTupleNew = TupleInterface::FormIndexTuple(
          heapTupleNew, &buildInfo, indexValues, indexIsNulls);
      auto autoCleanup = create_scope_guard([&]() {
        TupleInterface::DestroyIndexTuple(indexTupleNew);
        indexTupleNew = nullptr;
      });

      if (indexTupleNew == nullptr) {
        CDE_LOG_ERROR("Form new index tuple fail, table:%s index:%s",
                      GetTableNameOld(), dictIndex->name.c_str());
        return CDE_FAIL;
      }
      indexTupleNew->SetHeapCtid(ctidNew);

      if (*ctidNew != *ctid ||
          DSTORE::IndexTuple::Compare(indexTupleNew, indexTuple,
                                      &buildInfo.baseInfo) != 0) {
        IndexRowLogItem logItemOld(ROW_IDX_DELETE, indexTuple);
        if (dictIndex->m_rowLog->AppendLogItem(logItemOld, i) == CDE_FAIL) {
          CDE_LOG_ERROR("Append row log item fail, table:%s index:%s",
                        GetTableNameOld(), dictIndex->name.c_str());
          return CDE_FAIL;
        }
#ifndef NDEBUG
        if (g_rowLogPrint) {
          CDE_LOG_INFO(
              "Append row log of index success, %s, table:%s index:%s "
              "caller:%s.",
              logItemOld.ToString().c_str(), GetTableNameOld(),
              dictIndex->name.c_str(), caller);
        }
#endif
        IndexRowLogItem logItemNew(ROW_IDX_INSERT, indexTupleNew);
        if (dictIndex->m_rowLog->AppendLogItem(logItemNew, i) == CDE_FAIL) {
          CDE_LOG_ERROR("Append row log item fail, table:%s index:%s",
                        GetTableNameOld(), dictIndex->name.c_str());
          return CDE_FAIL;
        }
#ifndef NDEBUG
        if (g_rowLogPrint) {
          CDE_LOG_INFO(
              "Append row log of index success, %s, table:%s index:%s "
              "caller:%s.",
              logItemNew.ToString().c_str(), GetTableNameOld(),
              dictIndex->name.c_str(), caller);
        }
#endif
      }
    }
  }

  /** Records the heap tuple changed during the execution of DDL */
  if (m_checkMode == OnlineDdlCheckMode::INC_CHECK) {
    if (opType == ROW_IDX_DELETE) {
      AppendDeleteTupleCtid(*ctid);
      /** UPDATE SQL will record another ROW_IDX_INSERT log */
      if (heapTupleNew) {
        AppendInsertTupleCtid(*ctidNew);
      }
    } else if (opType == ROW_IDX_INSERT) {
      AppendInsertTupleCtid(*ctid);
    }
  }
  return CDE_SUCC;
}

void OnlineDdlMgr::RowLogReplayForIndex(cde_dict_index_t *dictIndex,
                                        uint64_t heapTupleNum, uint32_t indexNo,
                                        THD *mainThd) {
  std::string threadName("RowLogReplay" + std::to_string(indexNo));
  pthread_setname_np(pthread_self(), threadName.c_str());
  DEBUG_SYNC(mainThd, "dstore_row_log_replay_before");
  CDE_LOG_SYSTEM("Rowlog replay start, table:%s index:%s thread name:%s.",
                 GetTableNameOld(), dictIndex->name.c_str(),
                 threadName.c_str());
  my_thread_init();
  CdeConstructDstoreThrd();
  bool ret = CDE_SUCC;

  TempTrxGuard tempTrxGuard(__func__);
  CDE_ASSERT(tempTrxGuard.IsTrxCreated());
  RowLog *rowLog = dictIndex->m_rowLog;

  /* Check the data consistency before applying row log (i.e. after
  BuildParallel). We can only check the full data consistency here */
  if (m_checkMode == OnlineDdlCheckMode::FULL_CHECK_ONLY_AFTER_BUILD_INDEX ||
      m_checkMode == OnlineDdlCheckMode::FULL_CHECK_ALL) {
    if (CheckIndexDataConsistency(dictIndex, heapTupleNum, false) == CDE_FAIL) {
      CDE_LOG_ERROR(
          "The Index tuple value and heap tuple value of table:%s index %s is "
          "mismatch after building in parallel",
          GetTableNameOld(), dictIndex->name.c_str());
      SetAbort(OnlineDdlErrcode::ONLINE_CHECK_DATA_CONSISTENCY_FAIL);
      goto end;
    }
  }

  ret = rowLog->RowLogApplyOps(dictIndex, indexNo);
  rowLog->UnlockAppendIfNeed();
  // TODO: refer to row_log_free to release the Rowlog structure.
  if (m_needAbort || ret == CDE_FAIL) {
    NotifyAllInLastBlock();
  }

  /* Check the data consistency when all row log is applied. We can
  check both the full data consistency and incremental data consistency
  here */
  if ((m_checkMode ==
           OnlineDdlCheckMode::FULL_CHECK_ONLY_AFTER_ROW_LOG_REPLAYED ||
       m_checkMode == OnlineDdlCheckMode::FULL_CHECK_ALL ||
       m_checkMode == OnlineDdlCheckMode::INC_CHECK) &&
      ret == CDE_SUCC && !m_needAbort) {
    if (CheckIndexDataConsistency(dictIndex, heapTupleNum, true) == CDE_FAIL) {
      CDE_LOG_ERROR(
          "The Index tuple value and heap tuple value of table:%s index %s is "
          "mismatch after all row log is applied",
          GetTableNameOld(), dictIndex->name.c_str());
      SetAbort(OnlineDdlErrcode::ONLINE_CHECK_DATA_CONSISTENCY_FAIL);
      goto end;
    }
  }

end:
  tempTrxGuard.commit(__func__);
  if (ret == CDE_FAIL) {
    SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
  }
  CdeDestroyDstoreThrd();
  my_thread_end();
  CDE_LOG_SYSTEM(
      "Rowlog replay end, is abort:%d table:%s index:%s thread name:%s.",
      m_needAbort, GetTableNameOld(), dictIndex->name.c_str(),
      threadName.c_str());
}

bool OnlineDdlMgr::StartRowLogReplayForIndex(cde_dict_index_t *dictIndex,
                                             uint64_t heapTupleNum,
                                             uint32_t indexNo) {
  std::thread *newWorker =
      Memory::New<std::thread>(&OnlineDdlMgr::RowLogReplayForIndex, this,
                               dictIndex, heapTupleNum, indexNo, current_thd);
  if (newWorker == nullptr) {
    CDE_LOG_ERROR(
        "Start rowlog replay thread fail, errno:%d table:%s index:%s.", errno,
        GetTableNameOld(), dictIndex->name.c_str());
    SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    return CDE_FAIL;
  }
  m_workers.push_back(newWorker);
  return CDE_SUCC;
}

void OnlineDdlMgr::WaitAllRowLogReplayThreadsExit() {
  for (auto worker : m_workers) {
    if (worker->joinable()) {
      worker->join();
    }
    Memory::Delete<std::thread>(worker);
  }
  m_workers.clear();
}

void OnlineDdlMgr::IncCntOfReachedLastBlock() {
  std::unique_lock<std::mutex> locker(m_allInLastBlockMtx);
  m_replayAtLastBlockNum++;
  if (m_replayAtLastBlockNum == m_ctx->num_to_add_index) {
    NotifyAllInLastBlock();
  }
}

void OnlineDdlMgr::WaitUntilRowLogReplayNearlyFinish() {
  bool nearEndOrNeedAbort = false;
  while (!nearEndOrNeedAbort) {
    std::unique_lock<std::mutex> locker(m_allInLastBlockMtx);
    nearEndOrNeedAbort = m_allInLastBlockCv.wait_for(
        locker, std::chrono::milliseconds(100), [this] {
          return m_replayAtLastBlockNum == m_ctx->num_to_add_index ||
                 m_needAbort;
        });
  }
}

void OnlineDdlMgr::SetStatus(OnlineDdlStatus status) {
  std::unique_lock<std::mutex> locker(m_statusMtx);
  m_status = status;
}

bool OnlineDdlMgr::WaitCommitOrRollback() {
  std::unique_lock<std::mutex> locker(m_statusMtx);
  bool isCommitOrRollback =
      m_statusCv.wait_for(locker, std::chrono::milliseconds(100), [this] {
        return m_status == OnlineDdlStatus::COMMITTING ||
               m_status == OnlineDdlStatus::ABORTED;
      });
  return isCommitOrRollback;
}

void OnlineDdlMgr::SetAbort(OnlineDdlErrcode errcode, uint32_t errIndexNo,
                            const DSTORE::IndexTuple *dupTuple) {
  if (errcode != OnlineDdlErrcode::ONLINE_OK && !m_abortBuild && !m_needAbort) {
    auto thread_core = m_thrd->GetCore();
    if (thread_core != nullptr) {
      CDE_LOG_INFO(
          "cde online ddl set interrupt pending, session id:%u, "
          "threadCtx:%p",
          m_threadId, m_thrd);
      m_abortBuild = true;
      m_thrd->SetInterruptPending();
    }
  }
  m_needAbort = true;
  m_errInfo.SetErrInfo(errcode, errIndexNo, dupTuple);
}

void OnlineDdlMgr::ReportErrorInfo(TABLE *alteredTable,
                                   dstore_handler_t *dstoreHdl,
                                   Alter_inplace_info *haAlterInfo) const {
  switch (m_errInfo.m_errcode) {
    case OnlineDdlErrcode::ONLINE_OK:
      /* DDL is aborted due to the error of build phase. */
      break;
    case OnlineDdlErrcode::ONLINE_ERR_FILE_TOO_BIG: {
      CDE_ASSERT(m_errInfo.m_errIndexNo < m_ctx->num_to_add_index);
      cde_dict_index_t *errIndex = m_ctx->add_index[m_errInfo.m_errIndexNo];
      my_error(ER_DSTORE_ONLINE_LOG_TOO_BIG, MYF(0), errIndex->name.c_str());
      break;
    }
    case OnlineDdlErrcode::ONLINE_CHECK_DATA_CONSISTENCY_FAIL:
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_GENERIC,
               "Check data consistency fail.");
      break;
    case OnlineDdlErrcode::ONLINE_ERR_DUP_KEY:
      if (m_errInfo.m_errTuple) {
        CDE_ASSERT(m_errInfo.m_errIndexNo < m_ctx->num_to_add_index);
        cde_dict_index_t *errIndex = m_ctx->add_index[m_errInfo.m_errIndexNo];
        KEY *dupKey = &haAlterInfo->key_info_buffer[m_errInfo.m_errIndexNo];
        CdeInplaceAlterReportDupKey(
            alteredTable, errIndex, m_errInfo.m_errTuple,
            dstoreHdl->table_handler->attrFull, dupKey, GetTableNameOld());
        break;
      }
      [[fallthrough]];
    case OnlineDdlErrcode::ONLINE_ERR_OTHER:
      [[fallthrough]];
    default:
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_GENERIC,
               "Write or apply row log occur error.");
      break;
  }
}

bool OnlineDdlMgr::IsStatusValidForDml() const {
  if (m_status != OnlineDdlStatus::CREATING_INDEX &&
      m_status != OnlineDdlStatus::REBUILDING_TABLE) {
    CDE_LOG_WARN("Status:%u of online ddl manager is invalid for dml.",
                 static_cast<uint32_t>(m_status));
    return false;
  }
  return true;
}

const char *OnlineDdlMgr::GetTableNameOld() const {
  return m_mysqlTableOld->s->table_name.str;
}

std::string OnlineDdlMgr::GetStatusString() {
  return g_onlineDdlStatusStr[static_cast<uint8_t>(m_status)];
};

void OnlineDdlMgr::SetIndexBuildSnapShot() {
  CDESetSnapshotByTrans(m_indexBuildSnapshot);
}

bool OnlineDdlMgr::PrepareCheckIndexData(
    cde_dict_index_t *dictIndex, std::unique_ptr<session_rel_info> &relInfo,
    DSTORE::IndexBuildInfo &buildInfo) {
  /* Construct the session rel info for the log replay thread */
  relInfo = std::make_unique<session_rel_info>(m_ctx->old_table->m_id);
  if (CDE_OK != CdeConstructRelInfoForInplaceAlter(
                    m_ctx->old_table, relInfo.get(), m_ctx->add_index,
                    m_ctx->num_to_add_index)) {
    SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    return CDE_FAIL;
  }

  /* Construct the index build info */
  DSTORE::StorageRelationData *heapRel = relInfo->rd_storage_releation;
  buildInfo =
      CreateIndexBuildInfo(heapRel, heapRel->attr, dictIndex->index_col_num,
                           dictIndex->attr_cols, dictIndex->fields);
  buildInfo.baseInfo = *(dictIndex->rel->index);
  buildInfo.baseInfo.indexRelId = dictIndex->rel->relOid;
  return CDE_SUCC;
}

bool OnlineDdlMgr::CheckIndexDataConsistency(cde_dict_index_t *dictIndex,
                                             uint64_t heapTupleNum,
                                             bool isAllRowLogApplied) {
  std::unique_ptr<session_rel_info> relInfo = nullptr;
  DSTORE::IndexBuildInfo buildInfo;
  buildInfo.heapRels = nullptr;
  if (PrepareCheckIndexData(dictIndex, relInfo, buildInfo)) {
    return CDE_FAIL;
  }

  bool retval = CDE_SUCC;
  THD *origThd = current_thd;
  THD *tmpThd = create_internal_thd();
  TABLE *mysqlTableNew = open_table_uncached(
      tmpThd, m_mysqlTableNew->s->path.str, m_mysqlTableNew->s->db.str,
      m_mysqlTableNew->s->table_name.str, false, false, *m_ddTableNew);
  if (mysqlTableNew == nullptr) {
    CDE_LOG_ERROR("Construct new TABLE object fail, table:%s",
                  GetTableNameOld());
    return CDE_FAIL;
  }

  DSTORE::TupleDesc oldHeapAttr = relInfo->rd_storage_releation->attr;
  ConvertHeapValueToIndexValue ConvertFunctor(
      m_mysqlTableOld, mysqlTableNew, m_ctx->old_table, oldHeapAttr,
      m_ctx->new_table, dictIndex, nullptr, m_ctx->new_addedVirtualCols,
      m_ctx->old_existVirtualCols, false);
  if (!ConvertFunctor.m_canMultiThread) {
    ConvertFunctor.init();
  }

  /*  Auto clean up. */
  auto autoCleanup = create_scope_guard([&]() {
    ConvertFunctor.destroy();
    intern_close_table(mysqlTableNew);
    destroy_internal_thd(tmpThd);
    if (origThd) {
      origThd->store_globals();
    }
  });

  if (m_checkMode == OnlineDdlCheckMode::INC_CHECK) {
    retval =
        CheckIndexIncDataConsistency(dictIndex, buildInfo, &ConvertFunctor);
  } else {
    retval = CheckIndexFullDataConsistency(dictIndex, heapTupleNum,
                                           isAllRowLogApplied, buildInfo,
                                           &ConvertFunctor);
  }

  CdeFree(buildInfo.heapRels);
  return retval;
}

bool OnlineDdlMgr::CheckIndexFullDataConsistency(
    cde_dict_index_t *dictIndex, uint64_t heapTupleNum, bool isAllRowLogApplied,
    DSTORE::IndexBuildInfo &buildInfo,
    ConvertHeapValueToIndexValue *ConvertFunctor) {
  CDE_LOG_SYSTEM("Full data consistency check start for index: %s",
                 dictIndex->name.c_str());
  uint64_t LOG_PRINT_INTERVAL = 1000;

  uint32_t idxColumnNum = dictIndex->index_col_num;
  bool retval = CDE_SUCC;

  DSTORE::IndexScanHandler *idxScanHandler = IndexInterface::ScanBegin(
      dictIndex->rel, dictIndex->rel->index, idxColumnNum, 0);

  /* Use the build index snapshot to check data before row log applied
  and use the newest snapshot to check data after row log applied. */
  DSTORE::Snapshot scanSnapshot = nullptr;
  DSTORE::SnapshotData newestSnapshot;
  if (!isAllRowLogApplied) {
    scanSnapshot = &m_indexBuildSnapshot;
  } else {
    CDESetSnapshotByCurrent(newestSnapshot);
    scanSnapshot = &newestSnapshot;
  }

  IndexInterface::IndexScanSetSnapshot(idxScanHandler, scanSnapshot);
  IndexInterface::ScanSetWantItup(idxScanHandler, true);

  DSTORE::HeapScanHandler *heapScanHandler =
      HeapInterface::CreateHeapScanHandler(dictIndex->table->dstore_relation);
  HeapInterface::BeginScan(heapScanHandler, scanSnapshot);

  uint64_t totalIndexTupleNum = 0;
  uint64_t totalHeapTupleNum = heapTupleNum;
  if (isAllRowLogApplied) {
    totalHeapTupleNum += dictIndex->m_rowLog->GetTupleChangedNum();
  }

  Datum indexValuesInHeap[DSTORE::INDEX_MAX_KEY_NUM];
  bool indexIsNullInHeap[DSTORE::INDEX_MAX_KEY_NUM];
  Datum indexValues[DSTORE::INDEX_MAX_KEY_NUM];
  bool indexIsNulls[DSTORE::INDEX_MAX_KEY_NUM];

  while (true) {
    bool recheck = false;
    DSTORE::IndexTuple *itup = nullptr;
    DSTORE::TupleDesc td;

    itup = IndexInterface::OnlyScanNext(
        idxScanHandler, DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION, &td,
        &recheck);

    /* Read end of index */
    if (itup == nullptr ||
        (itup->GetHeapCtid()) == DSTORE::INVALID_ITEM_POINTER) {
      retval = CDE_SUCC;
      break;
    }

    totalIndexTupleNum++;
    TupleInterface::DeformIndexTuple(itup, td, indexValues, indexIsNulls);
    DSTORE::ItemPointerData *ctid =
        IndexInterface::GetResultHeapCtid(idxScanHandler);
    if (*ctid == DSTORE::INVALID_ITEM_POINTER) {
      retval = CDE_FAIL;
      break;
    }

    DSTORE::HeapTuple *heapTuple =
        HeapInterface::FetchTuple(heapScanHandler, *ctid);
    if (unlikely(heapTuple == nullptr)) {
      CDE_LOG_ERROR(
          "Check data consistency fail due to fetch heap tuple failed for "
          "index %s, ctid ({%hu, %u}, %hu)",
          dictIndex->name.c_str(), ctid->GetFileId(), ctid->GetBlockNum(),
          ctid->GetOffset());
      retval = CDE_FAIL;
      break;
    }

    if (!ConvertFunctor->m_canMultiThread) {
      DSTORE::FunctionCallInfoData fcInfo;
      fcInfo.prealloc_arg[0] = PointerGetDatum(heapTuple);
      fcInfo.prealloc_arg[1] = PointerGetDatum(indexValuesInHeap);
      fcInfo.prealloc_arg[2] = PointerGetDatum(indexIsNullInHeap);
      if ((*ConvertFunctor)(&fcInfo) == (DSTORE::Datum)DSTORE::DSTORE_FAIL) {
        CDE_LOG_ERROR("Convert index tuple fail, table:%s index:%s",
                      GetTableNameOld(), dictIndex->name.c_str());
        return CDE_FAIL;
      }
    }

    DSTORE::IndexTuple *indexTupleFromHeap = TupleInterface::FormIndexTuple(
        heapTuple, &buildInfo, indexValuesInHeap, indexIsNullInHeap);
    auto autoCleanup = create_scope_guard([&]() {
      TupleInterface::DestroyIndexTuple(indexTupleFromHeap);
      indexTupleFromHeap = nullptr;
    });

    if (indexTupleFromHeap == nullptr) {
      CDE_LOG_ERROR("Form index tuple from heap fail.");
      retval = CDE_FAIL;
      break;
    }
    indexTupleFromHeap->SetHeapCtid(ctid);

    DBUG_EXECUTE_IF("mock_data_compare_fail",
                    DSTORE::ItemPointerData tmpctid = *ctid;
                    tmpctid.SetOffset(tmpctid.GetOffset() + 1);
                    indexTupleFromHeap->SetHeapCtid(&tmpctid););
    if (DSTORE::IndexTuple::Compare(indexTupleFromHeap, itup,
                                    &buildInfo.baseInfo) != 0) {
      std::stringstream itupSs, itupFromHeapSs;
      ConvertTupleDataToStr(itup->GetValues(), itup->GetSize(), itupSs);
      ConvertTupleDataToStr(indexTupleFromHeap->GetValues(),
                            indexTupleFromHeap->GetSize(), itupFromHeapSs);

      CDE_LOG_ERROR(
          "Data compare fail. Index value: %s, Index value read from heap: "
          "%s",
          itupSs.str().c_str(), itupFromHeapSs.str().c_str());
      retval = CDE_FAIL;
      break;
    }

    TupleInterface::DestroyTuple(heapTuple, CdeFreeMemForDtuple);
    heapTuple = nullptr;
    if (totalIndexTupleNum % LOG_PRINT_INTERVAL == 0) {
      CDE_LOG_SYSTEM(
          "%ld tuples has been checked for data consistency, "
          "total num is: %ld",
          totalIndexTupleNum, totalHeapTupleNum);
    }
  }

  if (totalHeapTupleNum != totalIndexTupleNum) {
    retval = CDE_FAIL;
    CDE_LOG_ERROR(
        "Total number of heap tuple and index tuple for index %s are "
        "mismatched. "
        "Heap tuple num: %ld, Index tuple num: %ld, Check after row log apply: "
        "%d",
        dictIndex->name.c_str(), totalHeapTupleNum, totalIndexTupleNum,
        isAllRowLogApplied);
  }

  IndexInterface::ScanEnd(idxScanHandler);
  HeapInterface::EndScan(heapScanHandler);
  HeapInterface::DestroyHeapScanHandler(heapScanHandler);

  CDE_LOG_SYSTEM("End full data consistency check for index: %s",
                 dictIndex->name.c_str());
  return retval;
}

bool OnlineDdlMgr::CheckIndexIncDataConsistency(
    cde_dict_index_t *dictIndex, DSTORE::IndexBuildInfo &buildInfo,
    ConvertHeapValueToIndexValue *ConvertFunctor) {
  uint64_t LOG_PRINT_INTERVAL = 1000;
  uint64_t totalCheckIncDataNum = 0;
  uint64_t totalIncDataNum = m_heapIncDataMap.size();
  CDE_LOG_SYSTEM(
      "Begin check incremental data consistency, total inc data num is: %ld",
      totalIncDataNum);
  if (dictIndex->m_rowLog->CheckDeletedDataConsistency(totalCheckIncDataNum)) {
    return CDE_FAIL;
  }
  Datum indexValuesInHeap[DSTORE::INDEX_MAX_KEY_NUM];
  bool indexIsNullInHeap[DSTORE::INDEX_MAX_KEY_NUM];

  uint32_t idxColumnNum = dictIndex->index_col_num;
  bool retval = CDE_SUCC;

  DSTORE::IndexScanHandler *idxScanHandler = nullptr;
  DSTORE::ScanKey keyInfos =
      Memory::NewArray<DSTORE::ScanKeyData>(idxColumnNum);
  if (keyInfos == nullptr) {
    CDE_LOG_ERROR("Alloc ScanKey array fail for index %s",
                  dictIndex->name.c_str());
    return CDE_FAIL;
  }
  DSTORE::TupleDesc tupleDesc = dictIndex->rel->attr;

  std::vector<bool> isAsc;
  for (uint32_t i = 0; i < idxColumnNum; i++) {
    isAsc.push_back(
        !(dictIndex->rel->index->indexOption[i] & CDE_INDEX_OPTION_DESC));
  }

  /** Incremental data is only checked in the commit stage, so we can use
  the newest snapshot */
  DSTORE::SnapshotData scanSnapshot;
  CDESetSnapshotByCurrent(scanSnapshot);

  DSTORE::HeapScanHandler *heapScanHandler =
      HeapInterface::CreateHeapScanHandler(dictIndex->table->dstore_relation);
  HeapInterface::BeginScan(heapScanHandler, &scanSnapshot);

  for (const auto &pair : m_heapIncDataMap) {
    if (totalCheckIncDataNum % LOG_PRINT_INTERVAL == 0) {
      CDE_LOG_SYSTEM(
          "%ld incremental tuples has been checked for data consistency, "
          "total num is: %ld",
          totalCheckIncDataNum, totalIncDataNum);
    }
    DSTORE::ItemPointerData heapCtid = pair.first;
    OnlineDdlIndexTupleStatus status = pair.second;

    if (status == OnlineDdlIndexTupleStatus::DELETED) {
      continue;
    }
    totalCheckIncDataNum++;
    /** Step 1. Fetch the heap tuple according to the heap ctid */
    DSTORE::HeapTuple *heapTuple =
        HeapInterface::FetchTuple(heapScanHandler, heapCtid);

    if (unlikely(heapTuple == nullptr)) {
      CDE_LOG_ERROR(
          "Check data consistency fail due to fetch heap tuple failed for "
          "index %s, ctid ({%hu, %u}, %hu)",
          dictIndex->name.c_str(), heapCtid.GetFileId(), heapCtid.GetBlockNum(),
          heapCtid.GetOffset());
      retval = CDE_FAIL;
      break;
    }

    if (!ConvertFunctor->m_canMultiThread) {
      DSTORE::FunctionCallInfoData fcInfo;
      fcInfo.prealloc_arg[0] = PointerGetDatum(heapTuple);
      fcInfo.prealloc_arg[1] = PointerGetDatum(indexValuesInHeap);
      fcInfo.prealloc_arg[2] = PointerGetDatum(indexIsNullInHeap);
      if ((*ConvertFunctor)(&fcInfo) == (DSTORE::Datum)DSTORE::DSTORE_FAIL) {
        CDE_LOG_ERROR("Convert index tuple fail, table:%s index:%s",
                      GetTableNameOld(), dictIndex->name.c_str());
        break;
      }
    }
    /** Step 2. Form the index value from the heap tuple */
    DSTORE::IndexTuple *indexTupleFromHeap = TupleInterface::FormIndexTuple(
        heapTuple, &buildInfo, indexValuesInHeap, indexIsNullInHeap);

    auto autoCleanup = create_scope_guard([&]() {
      TupleInterface::DestroyIndexTuple(indexTupleFromHeap);
      indexTupleFromHeap = nullptr;
    });

    if (indexTupleFromHeap == nullptr) {
      CDE_LOG_ERROR("Form index tuple from heap fail.");
      retval = CDE_FAIL;
      break;
    }
    indexTupleFromHeap->SetHeapCtid(&heapCtid);

    TupleInterface::DeformIndexTuple(indexTupleFromHeap, tupleDesc,
                                     indexValuesInHeap, indexIsNullInHeap);
    /** Step 3. Use index scan to fetch the index tuple according to the index
    value */
    cde_btree_api::BuildKeyInfosForIndexRead(
        keyInfos, dictIndex->rel->attr->attrs, indexValuesInHeap,
        indexIsNullInHeap, idxColumnNum, HA_READ_KEY_EXACT, isAsc);

    idxScanHandler = IndexInterface::ScanBegin(
        dictIndex->rel, dictIndex->rel->index, idxColumnNum, 0, false, true);

    IndexInterface::IndexScanSetSnapshot(idxScanHandler, &scanSnapshot);
    IndexInterface::ScanSetWantItup(idxScanHandler, true);
    DSTORE::RetStatus ret =
        IndexInterface::ScanRescan(idxScanHandler, keyInfos);
    if (ret == DSTORE::DSTORE_FAIL) {
      CDE_LOG_ERROR("IndexInterface::ScanRescan return error: %d for index %s",
                    ret, dictIndex->name.c_str());
      IndexInterface::ScanEnd(idxScanHandler);
      break;
    }

    /** Step 4. Compare the ctid of index tuple is the same as heap ctid */
    bool found = false;
    while (true) {
      bool recheck = false;
      DSTORE::IndexTuple *itup = nullptr;
      DSTORE::TupleDesc td;

      itup = IndexInterface::OnlyScanNext(
          idxScanHandler, DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION, &td,
          &recheck);

      /* Read end of index */
      if (itup == nullptr ||
          (itup->GetHeapCtid()) == DSTORE::INVALID_ITEM_POINTER) {
        break;
      }

      DBUG_EXECUTE_IF("mock_data_compare_fail",
                      DSTORE::ItemPointerData tmpctid = itup->GetHeapCtid();
                      tmpctid.SetOffset(tmpctid.GetOffset() + 1);
                      itup->SetHeapCtid(&tmpctid););
      if (itup->GetHeapCtid() == heapCtid) {
        found = true;
        break;
      }
    }

    IndexInterface::ScanEnd(idxScanHandler);
    if (!found) {
      CDE_LOG_ERROR(
          "Not found the index tuple for index: %s, ctid:  ({%hu, %u}, %hu)",
          dictIndex->name.c_str(), heapCtid.GetFileId(), heapCtid.GetBlockNum(),
          heapCtid.GetOffset());
      retval = CDE_FAIL;
      break;
    }
  }

  Memory::DeleteArray<DSTORE::ScanKeyData>(keyInfos);
  HeapInterface::EndScan(heapScanHandler);
  HeapInterface::DestroyHeapScanHandler(heapScanHandler);
  CDE_LOG_SYSTEM("Check incremental data consistency end");
  return retval;
}

void OnlineDdlMgr::AppendInsertTupleCtid(DSTORE::ItemPointerData ctid) {
  CdeMutexGuard mapGuard(&m_heapIncDataMtx, CDE_LOCATION_HERE);
  m_heapIncDataMap[ctid] = OnlineDdlIndexTupleStatus::INSERTED;
}

void OnlineDdlMgr::AppendDeleteTupleCtid(DSTORE::ItemPointerData ctid) {
  CdeMutexGuard mapGuard(&m_heapIncDataMtx, CDE_LOCATION_HERE);
  m_heapIncDataMap[ctid] = OnlineDdlIndexTupleStatus::DELETED;
}

bool OnlineDdlMgr::IsTupleCtidExist(DSTORE::ItemPointerData ctid) {
  if (m_heapIncDataMap.find(ctid) != m_heapIncDataMap.end()) {
    return CDE_SUCC;
  }
  return CDE_FAIL;
}

RowLog::RowLog(OnlineDdlMgr *mgr) : m_onlineMgr(mgr) {
  int32_t ret = MutexInit(0, &m_appendMutex, MY_MUTEX_INIT_FAST);
  ret |= MutexInit(0, &m_incDataMtx, MY_MUTEX_INIT_FAST);
  CDE_ASSERT(ret == 0);
  CDE_ASSERT_DEBUG(mgr != nullptr);
}

RowLog::~RowLog() {
  RowLogProgressMgr::GetInstance()->DeleteItem(this);
  if (m_head.m_block) {
    Memory::Free(m_head.m_block);
    m_head.m_block = nullptr;
  }
  if (m_tail.m_block) {
    Memory::Free(m_tail.m_block);
    m_tail.m_block = nullptr;
  }
  if (m_fd != OS_FD_CLOSED) {
    ::close(m_fd);
    m_fd = OS_FD_CLOSED;
  }
  int32_t ret = MutexDestroy(&m_appendMutex);
  ret |= MutexDestroy(&m_incDataMtx);
  CDE_ASSERT(ret == 0);
}

void RowLog::LockAppend(const CdeSrcLocation &location) {
  CDE_ASSERT(!m_isLocked);
  MutexAcquireWithSource(&m_appendMutex, location.m_fileName,
                         location.m_fileLine);
  m_isLocked = true;
}

void RowLog::LockAppendIfNeed(const CdeSrcLocation &location) {
  if (!m_isLocked) {
    MutexAcquireWithSource(&m_appendMutex, location.m_fileName,
                           location.m_fileLine);
    m_isLocked = true;
  }
}

void RowLog::UnlockAppend() {
  CDE_ASSERT(m_isLocked);
  m_isLocked = false;
  MutexRelease(&m_appendMutex);
}

void RowLog::UnlockAppendIfNeed() {
  if (m_isLocked) {
    m_isLocked = false;
    MutexRelease(&m_appendMutex);
  }
}

bool RowLog::Init(cde_dict_index_t *dictIndex) {
  m_rowLogBufSize = g_rowLogBufSize;
  m_rowLogBufDataSize = m_rowLogBufSize - ROW_LOG_CHECKSUM_SIZE;
  CDE_ASSERT(m_head.m_block == nullptr);
  CDE_ASSERT(m_tail.m_block == nullptr);
  m_head.m_block = (unsigned char *)Memory::Malloc(m_rowLogBufSize);
  if (unlikely(m_head.m_block == nullptr)) {
    CDE_LOG_ERROR("Alloc %lu bytes for row log head block fail.",
                  m_rowLogBufSize);
    return CDE_FAIL;
  }
  m_tail.m_block = (unsigned char *)Memory::Malloc(m_rowLogBufSize);
  if (unlikely(m_tail.m_block == nullptr)) {
    CDE_LOG_ERROR("Alloc %lu bytes for row log tail block fail.",
                  m_rowLogBufSize);
    return CDE_FAIL;
  }
  RowLogProgressMgr::GetInstance()->AddItem(this, dictIndex);
  return FileCreateAndOpen();
}

bool RowLog::FileCreateAndOpen() noexcept {
  CDE_ASSERT(m_fd == OS_FD_CLOSED);
  auto fd = mysql_tmpfile_path(mysql_tmpdir, "dstore");
  int32_t fd2{OS_FD_CLOSED};
  if (fd >= 0) {
    /* Copy the file descriptor, so that the additional resources
    allocated by create_temp_file() can be freed by invoking
    my_close().

    Because the file descriptor returned by this function
    will be passed to fdopen(), it will be closed by invoking
    fclose(), which in turn will invoke close() instead of
    my_close(). */
    fd2 = dup(fd);
    my_close(fd, MYF(MY_WME));
  }

  DBUG_EXECUTE_IF("cde_tmpfile_creation_failure", fd2 = -1;);
  if (fd2 < 0) {
    CDE_LOG_ERROR("Create tmp file failed");
    return CDE_FAIL;
  }
  m_fd = fd2;
  return CDE_SUCC;
}

bool RowLog::FileWrite(int32_t fd, const void *buf, uint64_t offset,
                       uint64_t n) {
  CDE_ASSERT_DEBUG(n > 0);

  ssize_t nBytes = pwrite(fd, buf, n, offset);

  // TODO: if os_has_said_disk_full
  if (nBytes != static_cast<ssize_t>(n)) {
    CDE_LOG_ERROR(
        "Write to row log file failed at offset %lu,  bytes should have been "
        "written, only %ld were written. Check that your OS and file system "
        "support files of this size. Check also that the disk is not full "
        "or a disk quota exceeded.",
        offset, nBytes);
    return CDE_FAIL;

    // TODO:
    // os_has_said_disk_full = true;
  }

  return CDE_SUCC;
}

bool RowLog::FileRead(int32_t fd, void *buf, uint64_t offset, uint64_t n) {
  ssize_t nBytes;
  nBytes = pread(fd, buf, n, offset);

  if (nBytes != static_cast<ssize_t>(n)) {
    CDE_LOG_ERROR("read from file failed");
    return CDE_FAIL;
  }
  return CDE_SUCC;
}

bool RowLog::AppendLogItem(const IndexRowLogItem &item, uint32_t indexNo) {
  CdeMutexGuard appendMutexGuard(&m_appendMutex, CDE_LOCATION_HERE);
  uint32_t tupleSize = item.m_indexTuple->GetSize();
  uint32_t recSize = ROW_LOG_HEADER_SIZE + tupleSize;

  CDE_ASSERT(m_tail.m_offsetInBlock < m_rowLogBufDataSize);
  uint64_t availBufSize = m_rowLogBufDataSize - m_tail.m_offsetInBlock;
  unsigned char *bufPtrToWrite = m_tail.m_block + m_tail.m_offsetInBlock;
  DEBUG_SYNC_C("dstore_row_log_append_after_get_blockptr");

  if (recSize >= availBufSize) {
    const uint64_t curRowLogWrittenSize =
        (uint64_t)m_tail.m_blockNo * m_rowLogBufSize;
    if (curRowLogWrittenSize + m_rowLogBufSize >= g_rowLogFileMaxSize) {
      CDE_LOG_ERROR(
          "The row log file size(%lu) for will exceed the maximum"
          " value of %llu.",
          curRowLogWrittenSize + m_rowLogBufSize, g_rowLogFileMaxSize);
      m_onlineMgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_FILE_TOO_BIG, indexNo);
      return CDE_FAIL;
    }

    uint64_t recSecondPartSize = recSize - availBufSize;
    if (recSecondPartSize > 0) {
      CDE_ASSERT(recSize <= MERGE_BUF_SIZE_FOR_INDEX);
      bool ret = item.Serialize(m_tail.m_recMergeBuf);
      if (ret == CDE_FAIL) {
        CDE_LOG_ERROR("Serialize row log item fail.");
        return CDE_FAIL;
      }
      errno_t err = memcpy_s(bufPtrToWrite, availBufSize, m_tail.m_recMergeBuf,
                             availBufSize);
      if (err != EOK) {
        CDE_LOG_ERROR("Copy first part of row log record fail.");
        return CDE_FAIL;
      }
    } else {
      bool ret = item.Serialize(bufPtrToWrite);
      if (ret == CDE_FAIL) {
        CDE_LOG_ERROR("Serialize row log item fail.");
        return CDE_FAIL;
      }
    }

    ha_checksum crc = my_checksum(0L, m_tail.m_block, m_rowLogBufDataSize);
    unsigned char *blockChecksumPtr = m_tail.m_block + m_rowLogBufDataSize;
    *reinterpret_cast<ha_checksum *>(blockChecksumPtr) = crc;

    if (FileWrite(m_fd, m_tail.m_block, curRowLogWrittenSize,
                  m_rowLogBufSize) == CDE_FAIL) {
      return CDE_FAIL;
    }

    m_tail.m_blockNo++;
    m_tail.m_offsetInBlock = 0;
    if (recSecondPartSize > 0) {
      errno_t err =
          memcpy_s(m_tail.m_block, recSecondPartSize,
                   m_tail.m_recMergeBuf + availBufSize, recSecondPartSize);
      if (err != EOK) {
        CDE_LOG_ERROR("Copy second part of row log record fail.");
        return CDE_FAIL;
      }
      m_tail.m_offsetInBlock = recSecondPartSize;
    }
  } else {
    bool ret = item.Serialize(bufPtrToWrite);
    if (ret == CDE_FAIL) {
      CDE_LOG_ERROR("Serialize row log item fail.");
      return CDE_FAIL;
    }
    m_tail.m_offsetInBlock += recSize;
  }

  m_rowCount++;

  if (item.m_opType == ROW_IDX_INSERT) {
    IncTupleChangedNum();
  } else if (item.m_opType == ROW_IDX_DELETE) {
    DecTupleChangedNum();
  }

  return CDE_SUCC;
}

void RowLog::HangingIfReachedLastBlock() {
  bool isCommitOrRollback = false;
  while (m_head.m_blockNo == m_tail.m_blockNo && !isCommitOrRollback) {
    if (m_onceReachedToLastBlock == false) {
      /* First time reach the last block */
      m_onceReachedToLastBlock = true;
      m_onlineMgr->IncCntOfReachedLastBlock();
    }
    UnlockAppend();
    isCommitOrRollback = m_onlineMgr->WaitCommitOrRollback();
    LockAppend(CDE_LOCATION_HERE);
  }
}

bool RowLog::RowLogApplyOps(cde_dict_index_t *dictIndex, uint32_t indexNo) {
  /* Variables for read rowlog. */
  unsigned char indexTupleBuf[MAX_INDEX_TUPLE_SIZE];
  IndexRowLogItem item(indexTupleBuf, MAX_INDEX_TUPLE_SIZE);
  unsigned char *curBlockReadPtr = nullptr;
  unsigned char *curBlockEndPtr = nullptr;
  uint32_t itemSize = 0;
  DeserializeRet desRet = DeserializeRet::SUCC;
  bool needMerge = false;
  uint32_t savedPartSize = 0;

  /* Variables for index insert or delete. */
  uint32_t indexColNum = dictIndex->index_col_num;
  std::unique_ptr<DSTORE::Datum[]> values(new DSTORE::Datum[indexColNum]);
  std::unique_ptr<bool[]> isNulls(new bool[indexColNum]);
  MEM_ROOT tmpMemRoot;
  session_index_info *indexInfo = nullptr;
  if (CdeGenSessionIndexInfo(dictIndex, &tmpMemRoot, indexInfo) == CDE_FAIL) {
    CDE_LOG_ERROR(
        "Generate session index info fail before replay row log, table:%s "
        "index:%s.",
        m_onlineMgr->GetTableNameOld(), dictIndex->name.c_str());
    return CDE_FAIL;
  }

  LockAppend(CDE_LOCATION_HERE);
  /* Main loop: Read row log and replay. */
  while (!m_onlineMgr->NeedAbort() && m_head.m_blockNo <= m_tail.m_blockNo) {
    /* Step1: Check if reach to the last block, if yes need hanging. */
    HangingIfReachedLastBlock();
    CDE_ASSERT(m_isLocked == true);
    if (m_onlineMgr->GetStatus() == OnlineDdlStatus::ABORTED) {
      CDE_LOG_WARN("Abort apply row log for new index, table:%s index:%s.",
                   m_onlineMgr->GetTableNameOld(), dictIndex->name.c_str());
      return CDE_FAIL;
    }

    /* Step2: Load row log data from file into block buffer if needed. */
    if (m_head.m_blockNo == m_tail.m_blockNo) {
      /*Ensure that this branch is only entered when the ddl trx is commiting.*/
      CDE_ASSERT(m_onlineMgr->GetStatus() == OnlineDdlStatus::COMMITTING);
      if (m_tail.m_offsetInBlock == 0) {
        /* No row log in tail block. */
        return CDE_SUCC;
      }
      /* Tail block buffer has not been writed to file, so skip load from
      file and just use tail block buffer. */
      curBlockReadPtr = m_tail.m_block;
      curBlockEndPtr = m_tail.m_block + m_tail.m_offsetInBlock;
    } else {
      UnlockAppend();
      uint64_t fileOffset = (uint64_t)m_head.m_blockNo * m_rowLogBufSize;
      if (FileRead(m_fd, m_head.m_block, fileOffset, m_rowLogBufSize) !=
          CDE_SUCC) {
        CDE_LOG_ERROR("Unable to read row log file for index %s",
                      dictIndex->name.c_str());
        return CDE_FAIL;
      }

      ha_checksum crc = my_checksum(0L, m_head.m_block, m_rowLogBufDataSize);
      const ha_checksum fileReadCrc = *reinterpret_cast<const ha_checksum *>(
          m_head.m_block + m_rowLogBufDataSize);
      CDE_ASSERT(crc == fileReadCrc);

#ifdef POSIX_FADV_DONTNEED
      /* Each block is read exactly once.  Free up the file cache. */
      posix_fadvise(m_fd, fileOffset, m_rowLogBufSize, POSIX_FADV_DONTNEED);
#endif /* POSIX_FADV_DONTNEED */

      curBlockReadPtr = m_head.m_block;
      curBlockEndPtr = m_head.m_block + m_rowLogBufDataSize;
      /* We must have released the lock at previous round. */
    }

    /* Step3: If a first part of a record was read from the previous block.
    Read the second part of the record from current block, merge it as a
    whole record and apply it. */
    if (needMerge) {
      /* A partial record was read from the previous block. Copy the temporary
      buffer full, as we do not know the length of the record. Parse subsequent
      records from the bigger buffer m_head.m_block or m_tail.m_block. */
      CDE_ASSERT(savedPartSize != 0);
      uint32_t copySize = MERGE_BUF_SIZE_FOR_INDEX - savedPartSize;
      unsigned char *descPtr = m_head.m_recMergeBuf + savedPartSize;
      errno_t err = memcpy_s(descPtr, copySize, curBlockReadPtr, copySize);
      if (err != EOK) {
        CDE_LOG_ERROR("Copy second part of partial row log record fail.");
        return CDE_FAIL;
      }
      desRet = item.Deserialize(m_head.m_recMergeBuf,
                                m_head.m_recMergeBuf + MERGE_BUF_SIZE_FOR_INDEX,
                                itemSize);
      if (desRet == DeserializeRet::FAIL) {
        CDE_LOG_ERROR("Deserialize for merged row log record fail.");
        return CDE_FAIL;
      }

      CDE_ASSERT(desRet == DeserializeRet::SUCC);
      CDE_ASSERT(m_head.m_offsetInBlock == 0);
      m_head.m_offsetInBlock = itemSize - savedPartSize;
      curBlockReadPtr += m_head.m_offsetInBlock;
      savedPartSize = 0;
      needMerge = false;
      bool ret = RowLogApplyOp(dictIndex, indexInfo, item, values.get(),
                               isNulls.get(), indexNo);
      if (ret == CDE_FAIL) {
        CDE_LOG_ERROR("Merged row log record apply fail.");
        /* Error info has been set at RowLogApplyOp.*/
        return CDE_FAIL;
      }
    }

    /* Step4: Read all remaining records from the current block and apply
    them. */
    while (curBlockReadPtr < curBlockEndPtr) {
      desRet = item.Deserialize(curBlockReadPtr, curBlockEndPtr, itemSize);
      if (desRet == DeserializeRet::FAIL) {
        CDE_LOG_ERROR("Deserialize for normal row log record fail.");
        return CDE_FAIL;
      }

      if (desRet == DeserializeRet::PARTIAL) {
        /* The record is incomplete in current block; it needs to be read from
        the next block to assemble the complete record. */
        needMerge = true;
        savedPartSize = curBlockEndPtr - curBlockReadPtr;
        CDE_ASSERT_DEBUG(savedPartSize < MERGE_BUF_SIZE_FOR_INDEX);
        errno_t err = memcpy_s(m_head.m_recMergeBuf, savedPartSize,
                               curBlockReadPtr, savedPartSize);
        if (err != EOK) {
          CDE_LOG_ERROR("Copy first part of partial row log record fail.");
          return CDE_FAIL;
        }
        /* When process the tail.block, it should be a complete record, because
        we are holding the lock of rowlog, thus excluding the writer. */
        CDE_ASSERT(m_isLocked == false);
        break;
      }
      bool ret = RowLogApplyOp(dictIndex, indexInfo, item, values.get(),
                               isNulls.get(), indexNo);
      if (ret == CDE_FAIL) {
        CDE_LOG_ERROR("Merged row log record apply fail.");
        /* Error info has been set at RowLogApplyOp.*/
        return CDE_FAIL;
      }
      m_head.m_offsetInBlock += itemSize;
      curBlockReadPtr += itemSize;
    }

    /* Step5: Loop to next block and read. */
    LockAppendIfNeed(CDE_LOCATION_HERE);
    m_head.m_offsetInBlock = 0;
    m_head.m_blockNo++;
    CDE_ASSERT(m_isLocked == true);
  }
  return CDE_SUCC;
}

bool RowLog::RowLogApplyOp(cde_dict_index_t *dictIndex,
                           session_index_info *indexInfo, IndexRowLogItem &item,
                           DSTORE::Datum *values, bool *isNulls,
                           uint32_t indexNo) {
  DSTORE::ItemPointerData heapCtid = item.m_indexTuple->GetHeapCtid();
  DSTORE::TupleDesc tupleDesc = dictIndex->rel->attr;
  DSTORE::BtreeInsertAndDeleteCommonData btreeContext;
  DSTORE::RetStatus dstoreRet = DSTORE::DSTORE_SUCC;

  TupleInterface::DeformIndexTuple(item.m_indexTuple, tupleDesc, values,
                                   isNulls);
  CdeSetBtreeCtxForInsAndDel(indexInfo, values, isNulls, &heapCtid,
                             btreeContext);

  if (m_onlineMgr->GetCheckMode() == OnlineDdlCheckMode::INC_CHECK) {
    if (item.m_opType == ROW_IDX_DELETE) {
      AppendDeleteTupleCtid(heapCtid);
    } else {
      AppendInsertTupleCtid(heapCtid);
    }
  }

  if (item.m_opType == ROW_IDX_DELETE) {
    dstoreRet = IndexInterface::Delete(btreeContext);
  } else {
    dstoreRet = IndexInterface::Insert(btreeContext, false, true);
  }

  if (dstoreRet == DSTORE::DSTORE_FAIL) {
    uint32_t errCode = GetAndConvertDstoreErrcodeToMysql();
    CDE_LOG_ERROR_WITH_DSTORE_ERROR(
        "Apply row log of index fail, %s, table:%s index:%s.",
        item.ToString().c_str(), m_onlineMgr->GetTableNameOld(),
        dictIndex->name.c_str());
    if (errCode == HA_ERR_FOUND_DUPP_KEY) {
      m_onlineMgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_DUP_KEY, indexNo,
                            item.m_indexTuple);
    } else {
      m_onlineMgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    }
    return CDE_FAIL;
  } else {
#ifndef NDEBUG
    if (g_rowLogPrint) {
      CDE_LOG_INFO("Apply row log of index success, %s, table:%s index:%s.",
                   item.ToString().c_str(), m_onlineMgr->GetTableNameOld(),
                   dictIndex->name.c_str());
    }
#endif
    m_replayRowCount++;
  }
  TransactionInterface::SetCurCidUsed();
  TransactionInterface::IncreaseCommandCounter();

  return CDE_SUCC;
}

void RowLog::AppendInsertTupleCtid(DSTORE::ItemPointerData ctid) {
  CdeMutexGuard mapGuard(&m_incDataMtx, CDE_LOCATION_HERE);
  m_incrementalDataMap[ctid] = OnlineDdlIndexTupleStatus::INSERTED;
}

void RowLog::AppendDeleteTupleCtid(DSTORE::ItemPointerData ctid) {
  CdeMutexGuard mapGuard(&m_incDataMtx, CDE_LOCATION_HERE);
  m_incrementalDataMap[ctid] = OnlineDdlIndexTupleStatus::DELETED;
}

bool RowLog::CheckDeletedDataConsistency(uint64_t &totalCheckIncDataNum) {
  for (const auto &pair : m_incrementalDataMap) {
    DSTORE::ItemPointerData heapCtid = pair.first;
    OnlineDdlIndexTupleStatus status = pair.second;

    if (status == OnlineDdlIndexTupleStatus::DELETED) {
      totalCheckIncDataNum++;
      if (m_onlineMgr->IsTupleCtidExist(heapCtid)) {
        CDE_LOG_ERROR(
            "Heap tuple of ctid[%hu,%u] %hu is not deleted, but index tuple is "
            "deleted",
            heapCtid.val.m_pageid.m_fileId, heapCtid.val.m_pageid.m_blockId,
            heapCtid.val.m_offset);
        return CDE_FAIL;
      }
    }
  }
  return CDE_SUCC;
}

bool IndexRowLogItem::Serialize(unsigned char *buf) const {
  unsigned char *start = buf;
  *buf = m_opType;
  buf += ROW_LOG_OP_LEN;
  uint32_t tupleSize = m_indexTuple->GetSize();
  *reinterpret_cast<uint32_t *>(buf) = tupleSize;
  buf += ROW_LOG_TUPLE_SIZE_LEN;
  errno_t rc = memcpy_s(static_cast<void *>(buf), tupleSize,
                        static_cast<void *>(m_indexTuple), tupleSize);
  if (rc != EOK) {
    CDE_LOG_ERROR("memcpy_s fail when IndexRowLogItem Serialize");
    return CDE_FAIL;
  }
  buf += tupleSize;

  uint32_t recSize = ROW_LOG_HEADER_SIZE + tupleSize;
  CDE_ASSERT(buf - start == recSize);
  return CDE_SUCC;
}

DeserializeRet IndexRowLogItem::Deserialize(const unsigned char *buf,
                                            const unsigned char *bufEnd,
                                            uint32_t &itemSize) {
  if (buf + ROW_LOG_HEADER_SIZE >= bufEnd) {
    /* Even can't parse the header. */
    return DeserializeRet::PARTIAL;
  }

  const unsigned char *start = buf;
  m_opType = static_cast<CDE::IndexRowOp>(*buf);
  CDE_ASSERT(m_opType == ROW_IDX_DELETE || m_opType == ROW_IDX_INSERT);
  buf += ROW_LOG_OP_LEN;
  uint32_t tupleSize = *reinterpret_cast<const uint32_t *>(buf);
  buf += ROW_LOG_TUPLE_SIZE_LEN;
  CDE_ASSERT(tupleSize <= MAX_INDEX_TUPLE_SIZE);
  if (buf + tupleSize > bufEnd) {
    return DeserializeRet::PARTIAL;
  }

  CDE_ASSERT(m_useExternalMem == true);
  CDE_ASSERT(m_indexTuple != nullptr);
  errno_t rc = memcpy_s(static_cast<void *>(m_indexTuple), tupleSize,
                        static_cast<const void *>(buf), tupleSize);
  if (rc != EOK) {
    CDE_LOG_ERROR("Memcpy fail when deserialize row log item.");
    return DeserializeRet::FAIL;
  }
  buf += tupleSize;

  itemSize = buf - start;
  return DeserializeRet::SUCC;
}

std::string IndexRowLogItem::ToString() const {
  std::stringstream ss;
  if (m_indexTuple != nullptr) {
    DSTORE::ItemPointerData ctid = m_indexTuple->GetHeapCtid();
    ss << "ctid:[(" << ctid.val.m_pageid.m_fileId << ","
       << ctid.val.m_pageid.m_blockId << ")," << ctid.val.m_offset
       << "] tuple size:" << m_indexTuple->GetSize();
    ss << ", tuple data:";
    ConvertTupleDataToStr(m_indexTuple->GetValues(),
                          m_indexTuple->GetValueSize(), ss);
  }
  if (m_opType == ROW_IDX_DELETE) {
    ss << ", op:DELETE";
  } else if (m_opType == ROW_IDX_INSERT) {
    ss << ", op:INSERT";
  } else {
    ss << ", op:NONE";
  }
  return ss.str();
}

void CdeRowLogForRollback(const DSTORE::UndoExtraInfo *info, uint64_t xidInt,
                          DSTORE::RollbackType rollbackType,
                          DSTORE::ExposedUndoType undoType,
                          DSTORE::HeapTuple *oldTuple,
                          DSTORE::HeapTuple *newTuple) {
  CDE_ASSERT_DEBUG(info != nullptr);
  DSTORE::Xid xid;
  xid.m_placeHolder = xidInt;

  /* Get cde_dict_t by heapOid, check if it is in online ddl. */
  cde_dict_t *dictTable = DictSysGetTableById(info->m_tableId);
  CDE_ASSERT(dictTable != nullptr);
  DictTableRefGuard dictRefGuard(dictTable);
  CdeRwlockGuard onlineMgrGuard(&dictTable->m_onlineDdlMgrLock,
                                CdeRwlockOp::RD_LOCK);
  OnlineDdlMgr *mgr = dictTable->m_onlineDdlMgr;
  /* Table must be exists when trx is synchronously rollbacking. */
  if (!mgr) {
    /* If ddl-trx rollback, it will not acquire exclusive MDL of table, so
    concurrent-DML trx may rollback after the ddl-trx rollbacked. */
    CDE_LOG_WARN(
        "Online ddl mgr is null, ddl trx may has rollbacked, row log for a dml "
        "trx rollback is skipped, "
        "table:%s dml trx xid:[%u,%lu] undoType:%u rollbackType:%u.",
        dictTable->name.c_str(), (uint32_t)xid.m_zoneId, xid.m_logicSlotId,
        (uint32_t)undoType, (uint32_t)rollbackType);
    TupleInterface::DestroyTuple(oldTuple, CdeFreeMemForDtuple);
    TupleInterface::DestroyTuple(newTuple, CdeFreeMemForDtuple);
    return;
  }
  CDE_ASSERT(mgr->IsStatusValidForDml());
  CDE_ASSERT(mgr->GetInplaceCtx()->old_table == dictTable);

  if (oldTuple == nullptr && newTuple == nullptr) {
    CDE_LOG_ERROR(
        "Rollback row log for index fail, due to dstore copy tuple fail, "
        "table:%s dml trx xid:[%u,%lu] undoType:%u rollbackType:%u.",
        dictTable->name.c_str(), (uint32_t)xid.m_zoneId, xid.m_logicSlotId,
        (uint32_t)undoType, (uint32_t)rollbackType);
    mgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    return;
  }

  if (rollbackType != DSTORE::RollbackType::ROLLBACK_SYNC) {
    CDE_ASSERT(rollbackType == DSTORE::RollbackType::ROLLBACK_BY_XID);
    bool result = false;
    DSTORE::RetStatus ret = StoragePdbInterface::IsXidFromRecoveredTrx(
        GetCurrentPdbId(), xidInt, result);
    if (ret == DSTORE::DSTORE_FAIL || result == false) {
      /* Currently rollback by xid is only used for recovered trx, we add assert
      to ensure it obey this rule. If rollback by xid is supported for sync
      rollback of trx at the future, we need handle this situation
      at that time. */
      CDE_LOG_FATAL(
          "Row log for a dml trx rollback encouter unexpected situation,"
          " table:%s dml trx xid:[%u,%lu] undoType:%u rollbackType:%u "
          "dstoreRet:%d"
          " isRecoveredTrx:%u.",
          dictTable->name.c_str(), (uint32_t)xid.m_zoneId, xid.m_logicSlotId,
          (uint32_t)undoType, (uint32_t)rollbackType, ret, (uint32_t)result);
    }
  }

  CDE_ASSERT(xid.m_placeHolder == TransactionInterface::GetCurrentXid());
  RollbackedTuplesGatherer &gatherer = mgr->GetTuplesGatherer();
  if (!RollbackedTuplesGatherer::NeedGatherTuple(info, undoType, oldTuple,
                                                 newTuple)) {
    /* Excepet no tuples are leaked, otherwise it is a bug. */
    CDE_ASSERT(!gatherer.IsTrxTupleSaverExists(xid.m_placeHolder));
    if (mgr->RowLogRollbackForIndex(oldTuple, newTuple) == CDE_FAIL) {
      CDE_LOG_ERROR(
          "Row log for index fail when rollback for a no linked tuple, "
          "table:%s dml trx xid:[%u,%lu] undoType:%u.",
          dictTable->name.c_str(), (uint32_t)xid.m_zoneId, xid.m_logicSlotId,
          (uint32_t)undoType);
      mgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    }
    TupleInterface::DestroyTuple(oldTuple, CdeFreeMemForDtuple);
    TupleInterface::DestroyTuple(newTuple, CdeFreeMemForDtuple);
    return;
  }
  TuplesGatherResult result;
  if (gatherer.GatherAndAssembleTuples(xid.m_placeHolder, info, undoType,
                                       oldTuple, newTuple,
                                       result) == CDE_FAIL) {
    CDE_LOG_ERROR(
        "Gather and assemble tuple fail when rollback, table:%s dml trx "
        "xid:[%u,%lu] undoType:%u.",
        dictTable->name.c_str(), (uint32_t)xid.m_zoneId, xid.m_logicSlotId,
        (uint32_t)undoType);
    mgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    return;
  }
  if (result.m_isGatherEnd) {
    if (mgr->RowLogRollbackForIndex(result.m_oldTuple, result.m_newTuple) ==
        CDE_FAIL) {
      CDE_LOG_ERROR(
          "Row log for index fail when rollback for a linked tuple, "
          "table:%s dml trx xid:[%u,%lu] undoType:%u.",
          dictTable->name.c_str(), (uint32_t)xid.m_zoneId, xid.m_logicSlotId,
          (uint32_t)undoType);
      mgr->SetAbort(OnlineDdlErrcode::ONLINE_ERR_OTHER);
    }
    gatherer.ClearTrxTupleSaver(xid.m_placeHolder);
  }
}

RowLogProgressMgr::RowLogProgressMgr() {
  int32_t ret = MutexInit(0, &m_progressLock, MY_MUTEX_INIT_FAST);
  CDE_ASSERT(ret == 0);
}

RowLogProgressMgr::~RowLogProgressMgr() {
  int32_t ret = MutexDestroy(&m_progressLock);
  CDE_ASSERT(ret == 0);
}

RowLogProgressMgr *RowLogProgressMgr::GetInstance() {
  static RowLogProgressMgr instance;
  return &instance;
}

void RowLogProgressMgr::AddItem(RowLog *rowLog, cde_dict_index_t *dictIndex) {
  CdeMutexGuard progressMutexGuard(&m_progressLock, CDE_LOCATION_HERE);
  m_progressMap.insert({rowLog, dictIndex});
}

void RowLogProgressMgr::DeleteItem(RowLog *rowLog) {
  CdeMutexGuard progressMutexGuard(&m_progressLock, CDE_LOCATION_HERE);
  m_progressMap.erase(rowLog);
}

const Memory::Map<RowLog *, cde_dict_index_t *>
    *RowLogProgressMgr::GetProgressMap() {
#ifndef NDEBUG
  mysql_mutex_assert_owner(&m_progressLock);
#endif
  return &m_progressMap;
}

}  // namespace CDE
