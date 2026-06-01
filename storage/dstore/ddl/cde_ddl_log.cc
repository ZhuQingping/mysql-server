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

#include <memory>
#include <type_traits>

#include "lex_string.h"
#include "mysql/plugin.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/types/table.h"
#include "sql/dd_table_share.h"
#include "sql/ddl_info.h"
#include "sql/debug_sync.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_prepare.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/table.h"
#include "sql/thd_raii.h"

#include "boot/cde_instance.h"
#include "common/cde_compare_utils.h"
#include "common/cde_errorcode.h"
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/cde_mysql_ops.h"
#include "ddl/cde_tablespace.h"
#include "dict/cde_dict_stat.h"
#include "dml/cde_dml.h"
#include "dml/cde_dml_ctx.h"
#include "dml/cde_heap.h"
#include "handler/ha_cde.h"

#include "common/cde_trxmgr.h"
#include "errorcode/dstore_lock_error_code.h"
#include "index/dstore_scankey.h"
#include "pdb/dstore_pdb_interface.h"
#include "tablespace/dstore_tablespace_interface.h"
#include "tuple/dstore_memheap_tuple.h"

namespace CDE {

bool g_printDdlLog = true;
int32_t g_ddlLogFillFactorHeap = 60;
int32_t g_ddlLogFillFactorIndex = 50;

constexpr uint64_t CDE_DDL_CONCURRENCY_CHECK_INTERVAL_MS = 10;

CdeDdlLog *g_cdeDdlLogPtr = nullptr;

class DisableInterruptGaurd {
 public:
  explicit DisableInterruptGaurd(cde_session_t *session) : m_session(session) {
    m_threadCtxIntr = DSTORE::ThreadContextInterface::GetCurrentThreadContext();
    CDE_ASSERT(m_threadCtxIntr != nullptr);
    /* If current session is killed, we need temporarily remove interrupt flag
    of current thread context to finish an uninterruptable phase. */
    CdeMutexGuard mutexGuard(&m_session->m_mutex, CDE_LOCATION_HERE);
    m_handlerInterruptSaved = m_session->m_setInterrupt;
    /* Interrupt flag may has been cleared at
     * ThreadContext::CheckforInterrupts*/
    m_dstoreInterruptSaved = m_threadCtxIntr->GetInterruptPending();
    /* Restore m_setInterrupt to false to judge if current session is re-killed
    during uninterruptable phase. */
    m_session->m_setInterrupt = false;
    m_threadCtxIntr->ClearInterruptPending();
    m_session->m_inUninterruptablePhase = true;
  }

  ~DisableInterruptGaurd() {
    CDE_ASSERT(m_threadCtxIntr ==
               DSTORE::ThreadContextInterface::GetCurrentThreadContext());
    CDE_ASSERT_DEBUG(m_threadCtxIntr->GetInterruptPending() == false);
    CdeMutexGuard mutexGuard(&m_session->m_mutex, CDE_LOCATION_HERE);
    m_session->m_inUninterruptablePhase = false;
    if (m_session->m_setInterrupt || m_dstoreInterruptSaved) {
      /* If current session is re-killed during uninterruptable phase or
      dstore interrupt flag is already set before uninterruptable phase. */
      m_threadCtxIntr->SetInterruptPending();
    }
    if (m_handlerInterruptSaved) {
      m_session->m_setInterrupt = true;
    }
    m_session = nullptr;
    m_threadCtxIntr = nullptr;
  }

 private:
  cde_session_t *m_session = nullptr;
  DSTORE::ThreadContextInterface *m_threadCtxIntr = nullptr;
  bool m_dstoreInterruptSaved = false;
  bool m_handlerInterruptSaved = false;
};

CdeDdlLogRecord::CdeDdlLogRecord(DSTORE::Datum *values, bool *isNulls,
                                 DSTORE::ItemPointer ctid) {
  CDE_ASSERT(isNulls[(uint16_t)DdlLogField::FIELD_ID] == false);
  m_id = DatumToInt<uint64_t>(values[(uint16_t)DdlLogField::FIELD_ID]);
  CDE_ASSERT(isNulls[(uint16_t)DdlLogField::FIELD_THREAD_ID] == false);
  m_threadId =
      DatumToInt<uint64_t>(values[(uint16_t)DdlLogField::FIELD_THREAD_ID]);
  CDE_ASSERT(isNulls[(uint16_t)DdlLogField::FIELD_THREAD_ID] == false);
  m_type = DatumToInt<uint32_t>(values[(uint16_t)DdlLogField::FIELD_TYPE]);
  CDE_ASSERT(isNulls[(uint16_t)DdlLogField::FIELD_FILE_ID] == false);
  m_fileId = DatumToInt<uint16_t>(values[(uint16_t)DdlLogField::FIELD_FILE_ID]);
  CDE_ASSERT(isNulls[(uint16_t)DdlLogField::FIELD_BLOCK_ID] == false);
  m_blockId =
      DatumToInt<uint32_t>(values[(uint16_t)DdlLogField::FIELD_BLOCK_ID]);
  CDE_ASSERT(isNulls[(uint16_t)DdlLogField::FIELD_SEGMENT_TYPE] == false);
  m_segmentType =
      DatumToInt<uint8_t>(values[(uint16_t)DdlLogField::FIELD_SEGMENT_TYPE]);
  m_tableSpaceId =
      DatumToInt<uint16_t>(values[(uint16_t)DdlLogField::FIELD_TABLESPACE_ID]);
  if (isNulls[(uint16_t)DdlLogField::FIELD_TABLE_NAME] == false) {
    uint16_t dataLen = CdeReadFrom2LittleEndian(
        (uint8_t *)values[(uint16_t)DdlLogField::FIELD_TABLE_NAME]);
    char *dataPtr = (char *)values[(uint16_t)DdlLogField::FIELD_TABLE_NAME] +
                    MYSQL_STR_HEAD_LENTH2;
    std::string dataStr(dataPtr, dataLen);
    m_tableName = dataStr;
  }
  if (isNulls[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW] == false) {
    uint16_t dataLen = CdeReadFrom2LittleEndian(
        (uint8_t *)values[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW]);
    char *dataPtr =
        (char *)values[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW] +
        MYSQL_STR_HEAD_LENTH2;
    std::string dataStr(dataPtr, dataLen);
    m_tableNameNew = dataStr;
  }
  m_ctid = *ctid;
}

void CdeDdlLogRecord::RecordToDstoreDatum(dml_tuple_t *tuple) const {
  DSTORE::Datum *values = tuple->values;
  bool *isNulls = tuple->is_nulls;
  CdeVarlena *varlenas = tuple->varlenas;
  values[(uint16_t)DdlLogField::FIELD_ID] = m_id;
  isNulls[(uint16_t)DdlLogField::FIELD_ID] = false;
  values[(uint16_t)DdlLogField::FIELD_THREAD_ID] = m_threadId;
  isNulls[(uint16_t)DdlLogField::FIELD_THREAD_ID] = false;
  values[(uint16_t)DdlLogField::FIELD_TYPE] = m_type;
  isNulls[(uint16_t)DdlLogField::FIELD_TYPE] = false;
  values[(uint16_t)DdlLogField::FIELD_FILE_ID] = m_fileId;
  isNulls[(uint16_t)DdlLogField::FIELD_FILE_ID] = false;
  values[(uint16_t)DdlLogField::FIELD_BLOCK_ID] = m_blockId;
  isNulls[(uint16_t)DdlLogField::FIELD_BLOCK_ID] = false;
  values[(uint16_t)DdlLogField::FIELD_SEGMENT_TYPE] = m_segmentType;
  isNulls[(uint16_t)DdlLogField::FIELD_SEGMENT_TYPE] = false;
  values[(uint16_t)DdlLogField::FIELD_TABLESPACE_ID] = m_tableSpaceId;
  isNulls[(uint16_t)DdlLogField::FIELD_TABLESPACE_ID] = false;

  if (m_tableName.size() > 0) {
    CdeVarlena *varlena = &varlenas[(uint16_t)DdlLogField::FIELD_TABLE_NAME];
    varlena->len = m_tableName.size();
    varlena->data = (const unsigned char *)m_tableName.c_str();
    values[(uint16_t)DdlLogField::FIELD_TABLE_NAME] =
        static_cast<DSTORE::Datum>((uint64_t)varlena | DEREF_TAG);
    isNulls[(uint16_t)DdlLogField::FIELD_TABLE_NAME] = false;
  } else {
    values[(uint16_t)DdlLogField::FIELD_TABLE_NAME] = DSTORE::INVALID_DATUM;
    isNulls[(uint16_t)DdlLogField::FIELD_TABLE_NAME] = true;
  }

  if (m_tableNameNew.size() > 0) {
    CdeVarlena *varlena =
        &varlenas[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW];
    varlena->len = m_tableNameNew.size();
    varlena->data = (const unsigned char *)m_tableNameNew.c_str();
    values[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW] =
        static_cast<DSTORE::Datum>((uint64_t)varlena | DEREF_TAG);
    isNulls[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW] = false;
  } else {
    values[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW] = DSTORE::INVALID_DATUM;
    isNulls[(uint16_t)DdlLogField::FIELD_TABLE_NAME_NEW] = true;
  }
  tuple->ctid = m_ctid;
}

std::string CdeDdlLogRecord::ToString() const {
  std::stringstream ss;
  ss << "ID:" << m_id << ", "
     << "ThreadID:" << m_threadId << ", ";
  switch (static_cast<CdeDdlLogType>(m_type)) {
    case CdeDdlLogType::DROP_SEGMENT_LOG:
      ss << "Type:DROP_SEGMENT_LOG, "
         << "SegmentID:[" << m_fileId << "," << m_blockId << "], "
         << "SegmentType:" << static_cast<uint32_t>(m_segmentType) << ", ";
      break;
    case CdeDdlLogType::REMOVE_CACHE_LOG:
      ss << "Type:REMOVE_CACHE_LOG, "
         << "TableName:" << m_tableName << ", ";
      break;
    case CdeDdlLogType::RENAME_TABLE_LOG:
      ss << "Type:RENAME_TABLE_LOG, "
         << "OldName:" << m_tableName << ", "
         << "NewName: " << m_tableNameNew << ", ";
      break;
    case CdeDdlLogType::DUMMY_LOG:
      ss << "Type:DUMMY_LOG, ";
      break;
    case CdeDdlLogType::DROP_TABLESPACE_LOG:
      ss << "Type:DROP_TABLESPACE_LOG, "
         << "TableSpaceId:" << m_tableSpaceId << ", ";
      break;
    default:
      CDE_LOG_FATAL("Invalid DDL log type:%u.", m_type);
  }
  ss << "Ctid:[PageID:[" << m_ctid.val.m_pageid.m_fileId << ","
     << m_ctid.val.m_pageid.m_blockId << "],Offset:" << m_ctid.val.m_offset
     << "]";
  return ss.str();
}

CdeDdlLog *CdeDdlLog::GetInstance() {
  static CdeDdlLog instance;
  return &instance;
}

bool CdeDdlLog::IsServerInit() { return m_isServerInit; }

void CdeDdlLog::SetServerInit() { m_isServerInit = true; }

void CdeDdlLog::SetSkip(bool skip) { m_needSkip = skip; }

void CdeDdlLog::LoadFillFactor() {
  if (m_dictTable == nullptr) {
    return;
  }
  /* Note that potential non-atomic modification is safe here, because
  old value is no matter, and cde_relation::clone will copy the value
  and validate it to avoid illegal tearing value. */
  m_dictTable->fillfactor = g_ddlLogFillFactorHeap;
  for (auto &dictIndex : m_dictTable->index_dict_vec) {
    dictIndex->fillfactor = g_ddlLogFillFactorIndex;
  }
}

bool CdeDdlLog::OpenTable() {
  CDE_ASSERT(m_dictTable == nullptr);
  /* This function creates its own THD. If there exists a current THD this
  needs to be restored at the end of this function. The reason the current
  THD can not be used is that this might already have opened and closed
  tables and thus opening new tables will fail. */
  THD *origThd = current_thd;
  THD thd(false);
  thd.thread_stack = reinterpret_cast<char *>(&thd);
  thd.store_globals();

  MDL_ticket *mdl = nullptr;
  const char *tableName = "dstore_ddl_log";
  const char *dbName = "mysql";

  dd::cache::Dictionary_client *client = dd::get_dd_client(&thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(client);
  const dd::Table *ddTable = nullptr;
  cde_dict_t *dictTable = nullptr;
  bool needClose = false;
  TABLE_SHARE ts;
  TABLE td;

  try {
    if (dd::acquire_shared_table_mdl(&thd, dbName, tableName, false, &mdl)) {
      throw std::runtime_error("Acquire DDL log table MDL fail.");
    }

    if (client->acquire(dbName, tableName, &ddTable) || ddTable == nullptr) {
      throw std::runtime_error("Acquire DDL log table fail.");
    }

    if (CdeAcquireUncacheTable(&thd, ddTable, dbName, &ts, &td) == CDE_FAIL) {
      throw std::runtime_error("Get DDL log table definition fail.");
    }
    needClose = true;

    if ((dictTable = CdeDdFillDictTable(ddTable, &td, ddTable->name().c_str(),
                                        false)) == nullptr) {
      throw std::runtime_error("Fill DDL log dict table fail.");
    }

    if (CdeDdFillDictIndex(*ddTable, &td, dictTable) != CDE_OK) {
      throw std::runtime_error("Fill DDL log dict index fail.");
    }

    cde_dict_index_t *autoincIndex = dictTable->index_dict_vec[0];
    if (InitAutoinc(&dictTable->autoinc_dict, &td, 0, autoincIndex->rel,
                    autoincIndex->index_col_num)) {
      throw std::runtime_error("Init DDL log table auto-increment fail.");
    }
    m_dictTable = dictTable;
  } catch (const std::exception &e) {
    CDE_LOG_ERROR("Atomic-ddl is disabled, reason: %s", e.what());
    if (dictTable) {
      dictTable->destroy_from_cache();
      delete dictTable;
      dictTable = nullptr;
    }
  }

  if (needClose == true) {
    CdeReleaseUncachedTable(&ts, &td);
  }
  CdeDdMdlRelease(&thd, &mdl);

  thd.release_resources();
  if (origThd) {
    origThd->store_globals();
  }
  if (dictTable != nullptr) {
    LoadFillFactor();
    SetSkip(false);
  }
  return dictTable == nullptr ? CDE_FAIL : CDE_SUCC;
}

void CdeDdlLog::CloseTable() {
  m_stopFlag = true;
  if (m_recoverThread.joinable()) {
    m_recoverThread.join();
  }
  if (m_dictTable) {
    m_dictTable->destroy_from_cache();
    delete m_dictTable;
    m_dictTable = nullptr;
  }
  m_needSkip = true;
}

uint64_t CdeDdlLog::GetNextId() {
  uint64_t autoinc;
  DictAutoinc &autoincDict = m_dictTable->autoinc_dict;
  autoincDict.MutexEnter();
  CDE_ASSERT(autoincDict.GetInitialized());
  autoinc = autoincDict.GetAutoinc();
  ++autoinc;
  autoincDict.SetAutoinc(autoinc);
  autoincDict.MutexExit();
  return autoinc;
}

session_rel_info *CdeDdlLog::GetOrCreateRelationInfo(THD *thd) {
  cde_session_t *session = CdeGetSession(thd, false);
  if (session->m_ddlLogRelation == nullptr) {
    auto relInfo = std::make_unique<session_rel_info>(m_dictTable->m_id);
    bool ret = CdeCopyRelInfoToLocalStorage(m_dictTable, relInfo.get());
    if (unlikely(ret == CDE_FAIL)) {
      CDE_LOG_ERROR("Copy ddl log table relation failed.");
      return nullptr;
    }
    session->m_ddlLogRelation = relInfo.release();
  }
  return session->m_ddlLogRelation;
}

bool CdeDdlLog::LogRecordToTable(THD *thd, CdeDdlLogRecord &record,
                                 bool useAutonomousTrx, uint64_t severXid,
                                 bool *failAtWrite) {
  if (unlikely(m_needSkip)) {
    return CDE_SUCC;
  }

  session_rel_info *relationInfo = GetOrCreateRelationInfo(thd);
  if (unlikely(relationInfo == nullptr)) {
    CDE_LOG_ERROR("Copy relation infos failed at %s.", __func__);
    return CDE_FAIL;
  }

  DSTORE::Xid dstoreXidWrite = DSTORE::INVALID_XID;
  DSTORE::Xid dstoreXidDdl = DSTORE::INVALID_XID;
  bool ret = WriteRecordToTable(relationInfo, record, useAutonomousTrx,
                                dstoreXidWrite);
  if (unlikely(ret == CDE_FAIL && failAtWrite != nullptr)) {
    *failAtWrite = true;
  }
  if (ret == CDE_SUCC && useAutonomousTrx) {
    ret = DeleteRecordFromTable(relationInfo, record);
  }

  dstoreXidDdl = (DSTORE::Xid)TransactionInterface::GetCurrentXid();
  if (ret == CDE_FAIL) {
    CDE_LOG_ERROR(
        "Log DDL failed, record:[%s], server xid:%lu, dstore xid use by "
        "writting "
        "ddl log:[%lu,%lu], dstore xid of ddl trx:[%lu,%lu], use autonomous "
        "trx:%d.",
        record.ToString().c_str(), severXid, (uint64_t)dstoreXidWrite.m_zoneId,
        dstoreXidWrite.m_logicSlotId, (uint64_t)dstoreXidDdl.m_zoneId,
        dstoreXidDdl.m_logicSlotId, useAutonomousTrx);
  } else if (g_printDdlLog) {
    CDE_LOG_SYSTEM(
        "Log DDL success, record:[%s], server xid:%lu, dstore xid use by "
        "writting "
        "ddl log:[%lu,%lu], dstore xid of ddl trx:[%lu,%lu], use autonomous "
        "trx:%d.",
        record.ToString().c_str(), severXid, (uint64_t)dstoreXidWrite.m_zoneId,
        dstoreXidWrite.m_logicSlotId, (uint64_t)dstoreXidDdl.m_zoneId,
        dstoreXidDdl.m_logicSlotId, useAutonomousTrx);
  }
  return ret;
}

void CdeDdlLog::GetDdlLogInfoFromThd(const THD *thd, uint64_t &threadId,
                                     uint64_t &severXid) {
  threadId = 0;
  severXid = 0;
  /** thd is nullptr only at UTs. */
  if (unlikely(thd == nullptr)) {
    return;
  }

  threadId = thd_get_thread_id(thd);
  if (thd->get_transaction() != nullptr) {
    severXid = thd->get_transaction()->xid_state()->get_xid()->get_my_xid();
  }
}

bool CdeDdlLog::LogDropSegment(THD *thd, const DSTORE::PageId &segmentId,
                               DSTORE::SegmentType segmentType,
                               DSTORE::TablespaceId spaceId, bool isCreate,
                               bool *writeLogFail) {
  if (rds_dstore_enable_atomic_ddl == false && isCreate == false) {
    CdeAllocSegmentWrapper::Drop(segmentId, segmentType, spaceId);
    return CDE_SUCC;
  }
  uint64_t threadId = 0;
  uint64_t serverXid = 0;
  GetDdlLogInfoFromThd(thd, threadId, serverXid);
  CdeDdlLogRecord record(threadId, segmentId.m_fileId, segmentId.m_blockId,
                         static_cast<uint8_t>(segmentType), spaceId);
  return LogRecordToTable(thd, record, isCreate, serverXid, writeLogFail);
}

bool CdeDdlLog::LogRename(THD *thd, const char *oldTableName,
                          const char *newTableName) {
  uint64_t threadId = 0;
  uint64_t serverXid = 0;
  GetDdlLogInfoFromThd(thd, threadId, serverXid);
  CdeDdlLogRecord record(threadId, oldTableName, newTableName);
  return LogRecordToTable(thd, record, true, serverXid);
}

bool CdeDdlLog::LogRemoveCache(THD *thd, const char *tableName) {
  uint64_t threadId = 0;
  uint64_t serverXid = 0;
  GetDdlLogInfoFromThd(thd, threadId, serverXid);
  CdeDdlLogRecord record(threadId, tableName);
  return LogRecordToTable(thd, record, true, serverXid);
}

void CdeDdlLog::LogDummy(THD *thd, cde_session_t *session) {
  uint64_t threadId = 0;
  uint64_t serverXid = 0;
  GetDdlLogInfoFromThd(thd, threadId, serverXid);
  CdeDdlLogRecord record(threadId);

  DEBUG_SYNC(thd, "hanging_at_log_dummy");
  DisableInterruptGaurd disableInterruptGaurd(session);
  bool ret = LogRecordToTable(thd, record, true, serverXid);
  /* In theory, logging dummy record should not fail if dstore is well. And
  the failure is not tolerable, because it is only called by trans_commit_stmt
  whose retcode will be ignored, so we can't tell caller to stop and rollback
  if it failed, and the transaction of dstore will be left at readonly state,
  which will not generate commit-wal to carry ddl-info, that will cause more
  troubles. So we choose to assert if it failed.*/
  CDE_ASSERT(ret == CDE_SUCC);
}

bool CdeDdlLog::LogDropTableSpaceId(THD *thd,
                                    const DSTORE::TablespaceId tableSpaceId,
                                    bool isCreate, bool *writeLogFail) {
  if (!rds_dstore_enable_atomic_ddl && !isCreate) {
    return DeleteTableSpace(tableSpaceId);
  }
  uint64_t threadId = 0;
  uint64_t serverXid = 0;
  GetDdlLogInfoFromThd(thd, threadId, serverXid);
  CdeDdlLogRecord record(threadId, tableSpaceId);
  return LogRecordToTable(thd, record, isCreate, serverXid, writeLogFail);
}

bool CdeDdlLog::WriteRecordToTable(session_rel_info *relationInfo,
                                   CdeDdlLogRecord &record,
                                   bool useAutonomousTrx,
                                   DSTORE::Xid &dstoreXid) {
  /* Constraint: dstore transaction must be started before write ddl log.
  The constraint is satisfied in all scenarios at present. Add this assertion
  to ensure all modifications obey this constraint. */
  CDE_ASSERT(TransactionInterface::TrxIsInProgress());
  CDE_ASSERT(0 == m_dictTable->m_vColCount);
  dml_ins_ctx insertCtx(nullptr, m_dictTable, relationInfo,
                        m_dictTable->m_totalColCount, m_dictTable->m_vColCount,
                        nullptr);
  if (unlikely(insertCtx.init() != CDE_OK)) {
    CDE_LOG_ERROR("Insert context init failed at %s.", __func__);
    return CDE_FAIL;
  }

  record.m_id = GetNextId();
  CDE_ASSERT(record.m_id > m_recoverMaxId);
  record.RecordToDstoreDatum(insertCtx.tuple());
  AutonomousTrxGuard autonomousTrxGuard(useAutonomousTrx, __func__);
  if (unlikely(autonomousTrxGuard.IsAbnormal())) {
    /* Error log has been printed. */
    return CDE_FAIL;
  }

  DSTORE::Datum *values = insertCtx.tuple()->values;
  bool *isNulls = insertCtx.tuple()->is_nulls;
  DSTORE::Datum *indexValues = insertCtx.index_tuple()->index_values;
  bool *indexIsNulls = insertCtx.index_tuple()->index_is_nulls;
  DSTORE::ItemPointerData &ctid = insertCtx.tuple()->ctid;

  int32_t ret = cde_dml_api::insert_heap(relationInfo->rd_storage_releation,
                                         values, isNulls, ctid);
  dstoreXid = (DSTORE::Xid)TransactionInterface::GetCurrentXid();
  if (unlikely(ret != CDE_OK)) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Heap insert failed at %s.", __func__);
    return CDE_FAIL;
  }

  record.m_ctid = ctid;

  for (session_index_info *index_dict : relationInfo->cde_index_vec) {
    insertCtx.set_index(index_dict);
    bool retal = CdeDatumHeapToIndex(values, isNulls, &insertCtx);
    /* Because the structure of ddlLog table is fixed, the cover from
    heap datums to index datums must success.*/
    CDE_ASSERT(retal == CDE_SUCC);
    DBUG_EXECUTE_IF("insert_index_tuple_of_ddl_log_table_fail", {
      CDE_LOG_ERROR("Inject failure of insert DDL log table index tuple.");
      return CDE_FAIL;
    });
    ret =
        cde_dml_api::insert_index(index_dict, indexValues, indexIsNulls, &ctid);
    if (ret != CDE_OK) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR("Index insert failed at %s.", __func__);
      return CDE_FAIL;
    }
  }

  autonomousTrxGuard.commit(__func__);
  return CDE_SUCC;
}

bool CdeDdlLog::DeleteRecordFromTable(session_rel_info *relationInfo,
                                      const CdeDdlLogRecord &record) {
  CDE_ASSERT(TransactionInterface::TrxIsInProgress());
  CDE_ASSERT(0 == m_dictTable->m_vColCount);
  dml_ins_ctx deleteCtx(nullptr, m_dictTable, relationInfo,
                        m_dictTable->m_totalColCount, m_dictTable->m_vColCount,
                        nullptr);
  if (unlikely(deleteCtx.init() != CDE_OK)) {
    CDE_LOG_ERROR("Delete context init failed at %s.", __func__);
    return CDE_FAIL;
  }

  DSTORE::Datum *values = deleteCtx.tuple()->values;
  bool *isNulls = deleteCtx.tuple()->is_nulls;
  DSTORE::Datum *indexValues = deleteCtx.index_tuple()->index_values;
  bool *indexIsNulls = deleteCtx.index_tuple()->index_is_nulls;
  DSTORE::ItemPointerData &ctid = deleteCtx.tuple()->ctid;
  record.RecordToDstoreDatum(deleteCtx.tuple());
  if (cde_dml_api::delete_heap(relationInfo->rd_storage_releation, ctid) !=
      CDE_OK) {
    CDE_LOG_ERROR_WITH_DSTORE_ERROR("Delete DDL log table heap tuple failed.");
    return CDE_FAIL;
  }

  for (session_index_info *indexDict : relationInfo->cde_index_vec) {
    deleteCtx.set_index(indexDict);
    bool ret = CdeDatumHeapToIndex(values, isNulls, &deleteCtx);
    /* Because the structure of ddlLog table is fixed, the convert from
    heap datums to index datums will surely success.*/
    CDE_ASSERT(ret == CDE_SUCC);
    if (cde_dml_api::delete_index(indexDict, indexValues, indexIsNulls,
                                  &ctid) != CDE_OK) {
      CDE_LOG_ERROR_WITH_DSTORE_ERROR(
          "Delete DDL log table index tuple failed.");
      return CDE_FAIL;
    }
  }
  return CDE_SUCC;
}

bool CdeDdlLog::SearchRecordsFromTable(uint64_t threadId,
                                       session_rel_info *relationInfo,
                                       std::set<CdeDdlLogRecord> &records) {
  char buf[static_cast<uint16_t>(DdlLogField::FIELD_NUM) *
           (sizeof(DSTORE::Datum) + sizeof(bool))];
  DSTORE::Datum *values = (DSTORE::Datum *)buf;
  bool *isNulls = (bool *)(buf + static_cast<uint16_t>(DdlLogField::FIELD_NUM) *
                                     sizeof(DSTORE::Datum));

  session_index_info *threadIdIdx = relationInfo->cde_index_vec[1];
  DSTORE::ScanKeyData keyInfo;
  cde_btree_api::SetSingleKeyInfo(&keyInfo, threadIdIdx->rel->attr->attrs[0],
                                  static_cast<DSTORE::Datum>(threadId), false,
                                  0, DSTORE::SCAN_ORDER_EQUAL);

  /* This function may be called at the end of DDL or innobase_post_recover.
  If at innobase_post_recover there is no dstore transaction to wrap this
  function, so we need construct a temporary transaction before calling DML
  interfaces of Dstore. */
  TempTrxGuard tempTrxGuard(__func__);
  CDE_ASSERT(tempTrxGuard.IsTrxCreated());
  if (unlikely(tempTrxGuard.IsAbnormal())) {
    CDE_LOG_FATAL("Start temporary trx failed for threadId:%lu.", threadId);
    return CDE_FAIL;
  }

  DSTORE::IndexScanHandler *idxScanHandler = IndexInterface::ScanBegin(
      threadIdIdx->rel, threadIdIdx->rel->index, 1, 0);
  DSTORE::SnapshotData snapshot;
  CDESetSnapshotByCurrent(snapshot);
  IndexInterface::IndexScanSetSnapshot(idxScanHandler, &snapshot);
  IndexInterface::ScanSetWantItup(idxScanHandler, true);
  DSTORE::RetStatus ret = IndexInterface::ScanRescan(idxScanHandler, &keyInfo);
  if (unlikely(ret == DSTORE::DSTORE_FAIL)) {
    CDE_LOG_FATAL_WITH_DSTORE_ERROR(
        "DDL log table index rescan failed for threadId:%lu.", threadId);
    IndexInterface::ScanEnd(idxScanHandler);
    return CDE_FAIL;
  }

  bool retval = CDE_SUCC;
  DSTORE::HeapScanHandler *heapScanHandler =
      HeapInterface::CreateHeapScanHandler(relationInfo->rd_storage_releation);
  HeapInterface::BeginScan(heapScanHandler, &snapshot);
  while (true) {
    bool recheck = false;
    bool found = false;
    ret = IndexInterface::ScanNext(
        idxScanHandler, DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION, &found,
        &recheck);
    if (unlikely(ret == DSTORE::DSTORE_FAIL)) {
      if (GetDstoreErrcode() == DSTORE::LOCK_ERROR_WAIT_TIMEOUT) {
        CDE_LOG_WARN(
            "DDL log table index scan next timeouted and retry for "
            "threadId:%lu.",
            threadId);
        /* Need retry but currently IndexInterface::ScanNext is not reenterable
        after it return error. So we need return fail to let caller re-enter
        this function. */
        retval = CDE_FAIL;
        break;
      }
      CDE_LOG_FATAL_WITH_DSTORE_ERROR(
          "DDL log table index scan next failed for threadId:%lu.", threadId);
      retval = CDE_FAIL;
      break;
    }

    DSTORE::ItemPointerData *ctid =
        IndexInterface::GetResultHeapCtid(idxScanHandler);
    if (!found || *ctid == DSTORE::INVALID_ITEM_POINTER) {
      break;
    }

    DSTORE::HeapTuple *heapTuple =
        HeapInterface::FetchTuple(heapScanHandler, *ctid);
    if (unlikely(heapTuple == nullptr)) {
      CDE_LOG_FATAL_WITH_DSTORE_ERROR(
          "DDL log table fetch heap tuple failed for threadId:%lu.", threadId);
      retval = CDE_FAIL;
      break;
    }

    TupleInterface::DeformHeapTuple(
        heapTuple, relationInfo->rd_storage_releation->attr, values, isNulls);
    records.emplace(values, isNulls, ctid);
    TupleInterface::DestroyTuple(heapTuple, CdeFreeMemForDtuple);
  }

  IndexInterface::ScanEnd(idxScanHandler);
  HeapInterface::EndScan(heapScanHandler);
  HeapInterface::DestroyHeapScanHandler(heapScanHandler);
  if (retval == CDE_SUCC) {
    tempTrxGuard.commit(__func__);
  }
  return retval;
}

void CdeDdlLog::LoadAllRecordsFromTable(session_rel_info &relationInfo) {
  char buf[static_cast<uint16_t>(DdlLogField::FIELD_NUM) *
           (sizeof(DSTORE::Datum) + sizeof(bool))];
  DSTORE::Datum *values = (DSTORE::Datum *)buf;
  bool *isNulls = (bool *)(buf + static_cast<uint16_t>(DdlLogField::FIELD_NUM) *
                                     sizeof(DSTORE::Datum));

  DSTORE::HeapScanHandler *heapScanHandler =
      HeapInterface::CreateHeapScanHandler(relationInfo.rd_storage_releation);
  DSTORE::SnapshotData snapshot;
  CDESetSnapshotByCurrent(snapshot);
  HeapInterface::BeginScan(heapScanHandler, &snapshot);
  while (true) {
    DSTORE::HeapTuple *heapTuple = HeapInterface::SeqScan(heapScanHandler);
    if (nullptr == heapTuple) {
      /* Scan reach to end.*/
      break;
    }
    TupleInterface::DeformHeapTuple(
        heapTuple, relationInfo.rd_storage_releation->attr, values, isNulls);
    m_recoverRecords.emplace(values, isNulls, heapTuple->GetCtid());
  }
  HeapInterface::EndScan(heapScanHandler);
  HeapInterface::DestroyHeapScanHandler(heapScanHandler);
}

void CdeDdlLog::ReplayByThreadId(THD *thd, uint64_t threadId) {
  std::set<CdeDdlLogRecord> records;
  session_rel_info *relationInfo = GetOrCreateRelationInfo(thd);
  if (unlikely(relationInfo == nullptr)) {
    /* The post_ddl interface can't not tolerate failure. See post_ddl_t. */
    CDE_LOG_FATAL("Copy relation infos failed at %s for thread:%lu.", __func__,
                  threadId);
  }
  DEBUG_SYNC(thd, "cde_post_ddl_hang_before_search");
  while (true) {
    if (SearchRecordsFromTable(threadId, relationInfo, records) == CDE_SUCC) {
      for (const CdeDdlLogRecord &record : records) {
        if (record.m_id <= m_recoverMaxId) {
          continue;
        }
        ReplayRecord(record, relationInfo, false);
      }
      records.clear();
      break;
    }
    CDE_ASSERT(GetDstoreErrcode() == DSTORE::LOCK_ERROR_WAIT_TIMEOUT);
    records.clear();
  }
#ifndef NDEBUG
  SearchRecordsFromTable(threadId, relationInfo, records);
  CDE_ASSERT_DEBUG(records.empty());
#endif
}

void CdeDdlLog::ReplayRecord(const CdeDdlLogRecord &record,
                             session_rel_info *relationInfo, bool isRecovery) {
  switch (static_cast<CdeDdlLogType>(record.m_type)) {
    case CdeDdlLogType::DROP_SEGMENT_LOG:
      ReplayDropSegmentLog(relationInfo, record);
      /* Dstore tends to reuse segment_id so we must make drop segment
      and delete related ddl_log as an atomic opreation with protection
      of rwlock of segment_interface. So after ReplayDropSegmentLog end,
      we just return. */
      return;
    case CdeDdlLogType::REMOVE_CACHE_LOG:
      ReplayRemoveCacheLog(record, isRecovery);
      break;
    case CdeDdlLogType::RENAME_TABLE_LOG:
      ReplayRenameTableLog(record, isRecovery);
      break;
    case CdeDdlLogType::DROP_TABLESPACE_LOG:
      ReplayDropTableSpaceId(relationInfo, record);
      return;
    case CdeDdlLogType::DUMMY_LOG:
      break;
    default:
      CDE_LOG_FATAL("Unsupported ddl log record type: %u", record.m_type);
  }
  TempTrxGuard tempTrxGuard(__func__);
  /* PostDDL phase or recovery phase should not in any transaction. */
  CDE_ASSERT(tempTrxGuard.IsTrxCreated());
  bool ret = DeleteRecordFromTable(relationInfo, record);
  CDE_ASSERT(ret == CDE_SUCC);
  tempTrxGuard.commit(__func__);
}

void CdeDdlLog::ReplayDropSegmentLog(session_rel_info *relationInfo,
                                     const CdeDdlLogRecord &record) {
  DSTORE::PageId segmentId;
  segmentId.m_fileId = record.m_fileId;
  segmentId.m_blockId = record.m_blockId;
  TempTrxGuard tempTrxGuard(__func__);
  /* PostDDL phase or recovery phase should not in any transaction. */
  CDE_ASSERT(tempTrxGuard.IsTrxCreated());
  bool ret = DeleteRecordFromTable(relationInfo, record);
  CDE_ASSERT(ret == CDE_SUCC);
  tempTrxGuard.commit(__func__);
  CdeAllocSegmentWrapper::Drop(
      segmentId, static_cast<DSTORE::SegmentType>(record.m_segmentType),
      record.m_tableSpaceId);
  DEBUG_SYNC(current_thd, "cde_replay_drop_segment_hang_before_commit");
  if (g_printDdlLog) {
    CDE_LOG_SYSTEM("Replay DDL log success, record:[%s].",
                   record.ToString().c_str());
  }
}

void CdeDdlLog::ReplayRemoveCacheLog(const CdeDdlLogRecord &record,
                                     bool isRecovery) {
  if (isRecovery) {
    return;
  }
  DictSysRemoveTable(record.m_tableName.c_str());

  if (!CdeIsMysqlTmpTableName(record.m_tableName.c_str())) {
    (void)DstoreDictStatDropTable(record.m_tableName.c_str(), current_thd);
  }

  if (g_printDdlLog) {
    CDE_LOG_SYSTEM("Replay DDL log success, record:[%s].",
                   record.ToString().c_str());
  }
}

void CdeDdlLog::ReplayRenameTableLog(const CdeDdlLogRecord &record,
                                     bool isRecovery) {
  if (isRecovery) {
    return;
  }

  const char *nameFrom = record.m_tableNameNew.c_str();
  const char *nameTo = record.m_tableName.c_str();
  cde_dict_t *dictTable = DictSysGetTable(nameFrom);
  DictTableRefGuard dictRefGuard(dictTable);
  if (dictTable == nullptr) {
    /* In protection of MDL, dictTable must exists in dict_cache.
    If eviction of dict_cache added, this situation may occur. */
    CDE_LOG_INFO(
        "Replay DDL log failed due to dict table of %s is not found, "
        "record:[%s].",
        nameFrom, record.ToString().c_str());
    return;
  }

  bool renameForeign = true;
  if (CdeIsMysqlTmpTableName(nameFrom) || CdeIsMysqlTmpTableName(nameTo)) {
    /* FIXME: "refresh_fk" related codes are incomplete! When "refresh_fk"
    related codes are updated, it is necessary to synchronously update the
    handling here. */
    renameForeign = false;
    dictTable->refresh_fk = true;
  }

  {
    OperationalLockGuard opGuard;
    (void)DictSysRenameTable(record.m_tableNameNew.c_str(),
                             record.m_tableName.c_str(), renameForeign);
  }

  if (!CdeIsMysqlTmpTableName(nameFrom) && !CdeIsMysqlTmpTableName(nameTo)) {
    (void)DstoreDictStatRenameTable(current_thd, nameFrom, nameTo);
  }

  if (g_printDdlLog) {
    CDE_LOG_SYSTEM("Replay DDL log success, record:[%s].",
                   record.ToString().c_str());
  }
}

void CdeDdlLog::ReplayDropTableSpaceId(session_rel_info *relationInfo,
                                       const CdeDdlLogRecord &record) {
  DSTORE::TablespaceId spaceId = record.m_tableSpaceId;
  TempTrxGuard tempTrxGuard(__func__);
  /* PostDDL phase or recovery phase should not in any transaction. */
  CDE_ASSERT(tempTrxGuard.IsTrxCreated());
  bool ret = DeleteRecordFromTable(relationInfo, record);
  CDE_ASSERT(ret == CDE_SUCC);
  tempTrxGuard.commit(__func__);
  DeleteTableSpace(spaceId);
  if (g_printDdlLog) {
    CDE_LOG_SYSTEM("Replay DDL log success, record:[%s].",
                   record.ToString().c_str());
  }
}

void CdeDdlLog::PostDdl(THD *thd) {
  if (m_needSkip) {
    return;
  }

  /* Replay ddl log need to read ddl log table in dstore, so we
  need construct thrd is the session never done before. Such as
  ddl rollback triggered by sql-layer which never touched the
  dstore-handler before. */
  cde_session_t *session = CdeCreateOrGetSession(thd);
  DisableInterruptGaurd disableInterruptGaurd(session);

  uint64_t threadId = thd_get_thread_id(thd);
  if (g_printDdlLog) {
    std::string queryStr;
    if (get_ddl_command_type(thd) == DDLCOM_PRIVI &&
        thd->rewritten_query().length()) {
      queryStr.assign(thd->rewritten_query().ptr(),
                      thd->rewritten_query().length());
    } else {
      queryStr.assign(thd->query().str, thd->query().length);
    }
    CDE_LOG_SYSTEM("Post ddl for thread id %lu, query: %.512s", threadId,
                   queryStr.c_str());
  }
  ReplayByThreadId(thd, threadId);
  delete session->m_ddlLogRelation;
  session->m_ddlLogRelation = nullptr;
}

void CdeDdlLog::ReplayRecordsBackGround() {
  pthread_setname_np(pthread_self(), "DdlBgReplay");
  my_thread_init();
  cde_session_t *cdeSession = new cde_session_t();
  CDE_ASSERT(cdeSession != nullptr);
  CdeConstructSession(cdeSession);
  StoragePdbInterface::waitAllRollbackTaskFinished(GetCurrentPdbId());
  session_rel_info relationInfo(m_dictTable->m_id);
  CDE_LOG_SYSTEM("DDL log recovery replay start.");
  bool ret = CdeCopyRelInfoToLocalStorage(m_dictTable, &relationInfo);
  if (ret == CDE_FAIL) {
    CDE_LOG_FATAL("Copy ddl log table relation failed.");
    CdeDestorySession(cdeSession);
    return;
  }
  for (const CdeDdlLogRecord &record : m_recoverRecords) {
    ReplayRecord(record, &relationInfo, true);
    if (m_stopFlag) {
      CDE_LOG_SYSTEM("DDL log recovery replay interrupt.");
      break;
    }
  }
  CdeMutexGuard recoverRecordsMutexGuard(&m_recoverRecordsMutex,
                                         CDE_LOCATION_HERE);
  m_recoverRecords.clear();
  CdeDestorySession(cdeSession);
  CDE_LOG_SYSTEM("DDL log recovery replay end.");
  my_thread_end();
}

void CdeDdlLog::ReplayAll() {
  if (m_needSkip) {
    return;
  }
  session_rel_info relationInfo(m_dictTable->m_id);
  CDE_LOG_SYSTEM("DDL log recovery load records begin.");
  bool ret = CdeCopyRelInfoToLocalStorage(m_dictTable, &relationInfo);
  if (ret == CDE_FAIL) {
    CDE_LOG_FATAL("Copy ddl log table relation failed.");
    return;
  }
  CDE_ASSERT(m_recoverRecords.empty());
  LoadAllRecordsFromTable(relationInfo);
  m_recoverMaxId = 0;
  if (!m_recoverRecords.empty()) {
    m_recoverMaxId = m_recoverRecords.cbegin()->m_id;
  }
  CDE_ASSERT(m_recoverMaxId <= m_dictTable->autoinc_dict.GetAutoinc());
  CDE_LOG_SYSTEM(
      "DDL log recovery load records end, record num:%lu, last log ID:%lu.",
      m_recoverRecords.size(), m_recoverMaxId);
  if (m_recoverRecords.empty()) {
    return;
  }
  m_recoverThread =
      std::thread(std::bind(&CdeDdlLog::ReplayRecordsBackGround, this));
}

void CdeDdlLog::CreateTable() {
  std::string stmtStr;
  stmtStr =
      "CREATE TABLE mysql.dstore_ddl_log("
      "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
      "thread_id BIGINT UNSIGNED NOT NULL,"
      "type INT UNSIGNED NOT NULL,"
      "file_id INT UNSIGNED NOT NULL,"
      "block_id INT UNSIGNED NOT NULL,"
      "segment_type INT UNSIGNED NOT NULL,"
      "tablespace_id INT UNSIGNED NOT NULL,"
      "table_name VARCHAR(512),"
      "table_name_new VARCHAR(512),"
      "PRIMARY KEY idx_pk(id),"
      "KEY idx_1(thread_id)"
      ") engine=DSTORE CHARACTER SET latin1 COLLATE latin1_bin";
  stmtStr.append(", TABLESPACE = ").append(dstore_global_spacename);

  LEX_STRING stmtLexStr{stmtStr.data(), stmtStr.size()};
  THD *origThd = current_thd;
  THD *tmpThd = create_internal_thd();
  tmpThd->set_new_thread_id();
  (void)CdeCreateOrGetSession(tmpThd);
  {
    Ed_connection bgConnection(tmpThd);
    /* Turn off binlogging to prevent written to the binary log. */
    Disable_binlog_guard disableBinlog(tmpThd);
    Disable_sql_log_bin_guard disableSqlLogBin(tmpThd);
    if (bgConnection.execute_direct(stmtLexStr)) {
      CDE_LOG_ERROR("Create ddl log table fail.");
    } else {
      CDE_LOG_SYSTEM("Create ddl log table success.");
    }
  }
  cde_session_t *&cdeSessRef = CdeGetSessionRef(tmpThd);
  CdeDestorySession(cdeSessRef);
  destroy_internal_thd(tmpThd);
  if (origThd) {
    origThd->store_globals();
  }
}

bool CdeDdlLog::RemainTablespaceDDL(DSTORE::TablespaceId tableSpaceId) {
  CdeMutexGuard recoverRecordsMutexGuard(&m_recoverRecordsMutex,
                                         CDE_LOCATION_HERE);
  DBUG_EXECUTE_IF("insert_legacy_drop_segment_tablespaceid_op", {
    m_recoverRecords.emplace(
        1, 5132, 138,
        static_cast<uint8_t>(DSTORE::SegmentType::HEAP_SEGMENT_TYPE),
        tableSpaceId);
  });
  return std::any_of(
      m_recoverRecords.begin(), m_recoverRecords.end(),
      [&](const CdeDdlLogRecord &record) {
        return record.m_type ==
                   static_cast<uint32_t>(CdeDdlLogType::DROP_SEGMENT_LOG) &&
               record.m_tableSpaceId == tableSpaceId;
      });
}

bool CdeDdlLogInit(bool isServerInit) {
  if (isServerInit) {
    /* MySQL server is initializing, the ddl_log table has
    not been created, just skip logging ddl. */
    CdeDdlLog::GetInstance()->SetServerInit();
    return CDE_SUCC;
  }

  if (rds_dstore_enable_atomic_ddl == false) {
    CDE_LOG_SYSTEM("DDL log is disabled by configuration.");
    return CDE_SUCC;
  }
  g_cdeDdlLogPtr = CdeDdlLog::GetInstance();
  return CdeDdlLog::GetInstance()->OpenTable();
}

void CdeDdlLogDeint() { CdeDdlLog::GetInstance()->CloseTable(); }

bool CdeIsDdlLogDisable(THD *thd) {
  if (unlikely(rds_dstore_enable_atomic_ddl == false ||
               thd->is_bootstrap_system_thread() ||
               thd->system_thread == SYSTEM_THREAD_BACKGROUND)) {
    return true;
  }

  /* DDL crash safety is guaranteed only when DStore operates as the exclusive
   storage engine.

   Logically, this condition should align with the need_ddl_info predicate,
   which evaluates to 'storage_engine_mode != static_cast<ulong>(ONLY_DSTORE)'.
   However, applying this condition directly introduces a defect in MTR test
   scenarios where storage_engine_mode defaults to BOTH_STORAGE_ENGINE: certain
   DDL operations that exclusively modify the InnoDB data dictionary (e.g.,
   instant DDL) would bypass CSN updates. This would cause subsequent
   UpdateDictTableAndIndexCsn invocations to reference stale CSN values,
   resulting in operational errors.
   */
  return get_instance_storage_engine_type() != DB_TYPE_DSTORE;
}

bool CdeIsDdlWithTrx(THD *thd) {
  if (get_ddl_command_type(thd) != DDLCOM_NONE &&
      TransactionInterface::TrxIsInProgress() &&
      TransactionInterface::IsReadWriteTransaction()) {
    return true;
  }
  return false;
}

}  // namespace CDE
