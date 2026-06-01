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

#ifndef __CDE_ONLINE_DDL_H__
#define __CDE_ONLINE_DDL_H__

#include <atomic>
#include <condition_variable>
#include <thread>
#include <unordered_map>

#include "framework/dstore_instance_interface.h"
#include "heap/dstore_heap_interface.h"

class Field;
class Alter_inplace_info;
namespace dd {
class Table;
}

namespace DSTORE {
class ThreadContext;
}

namespace CDE {

struct cde_dict_t;
struct cde_dict_index_t;
struct CdeVarlena;
struct CdeSrcLocation;
struct DictVirtualCol;
struct session_index_info;
struct session_rel_info;
struct dstore_handler_t;
class ha_cde_inplace_ctx;
class ConvertHeapValueToIndexValue;

// TODO: 1)Add dstore interface to get value of PAGE_SIZE
//      2)Check HA_MAX_REC_LENGTH with MAX_HEAP_REC_LENGTH
//      3)Check MAX_INDEX_TUPLE_LENGTH with ha_cde::max_supported_key_length
/** The definition of DSTORE_PAGE_SIZE should keep consistency with
dstore_page PAGE_SIZE. */
constexpr uint32_t DSTORE_PAGE_SIZE = 8192;
/** The definition of MAX_HEAP_REC_LENGTH should keep consistency with
HA_MAX_REC_LENGTH. */
constexpr uint32_t MAX_HEAP_TUPLE_SIZE = 65536 * 2;
constexpr uint32_t MAX_HEAP_CHUNK_NUM = MAX_HEAP_TUPLE_SIZE / DSTORE_PAGE_SIZE;
constexpr uint32_t MAX_INDEX_TUPLE_SIZE = DSTORE_PAGE_SIZE / 3;
constexpr int32_t OS_FD_CLOSED = -1;
constexpr uint64_t INVALID_ROW_LOG_BUFFER_OFFSET = ~0ULL;
constexpr uint32_t ROW_LOG_OP_LEN = 1;
constexpr uint32_t ROW_LOG_TUPLE_SIZE_LEN = sizeof(uint32_t);
/** Size of the modification log entry header, op and tuple size*/
constexpr uint32_t ROW_LOG_HEADER_SIZE =
    ROW_LOG_OP_LEN + ROW_LOG_TUPLE_SIZE_LEN;
constexpr uint32_t ROW_LOG_CHECKSUM_SIZE = 4;
constexpr uint32_t MAX_ROW_LOG_INDEX_REC_SIZE =
    ROW_LOG_HEADER_SIZE + MAX_INDEX_TUPLE_SIZE;
constexpr uint32_t MERGE_BUF_SIZE_FOR_INDEX = MAX_ROW_LOG_INDEX_REC_SIZE;

/* The definition of MockXid should keep consistency with DSTORE::Xid */
union MockXid {
  uint64 m_placeHolder{UINT64_MAX};
  struct {
    uint64 m_zoneId : 20;
    uint64 m_logicSlotId : 44;
  };
} __attribute__((packed));

/** Memory row log buffer size for online index creation. */
extern ulong g_rowLogBufSize;

/** Maximum row log file size for online index creation. */
extern ulonglong g_rowLogFileMaxSize;

#ifndef NDEBUG
extern bool g_rowLogPrint;
#endif
/* Whether check the consistency of heap and index data after using online ddl
to create index */
extern uint g_onlineDdlCheckDataMode;

/** Whether enable online ddl */
extern bool g_onlineDdlEnable;
extern bool g_onlineAddIndexOnNewVcolEnable;

enum IndexRowOp : uint8_t {
  ROW_IDX_DELETE = 0,
  ROW_IDX_INSERT,
  ROW_IDX_OP_NONE
};

/** The dstore online ddl status, note that the order of
this enum must be the same as the array g_onlineDdlStatusStr
define. */
enum class OnlineDdlStatus : uint8_t {
  INVALID = 0,
  CREATING_INDEX,
  REBUILDING_TABLE,
  COMMITTING,
  ABORTED
};

enum class OnlineDdlErrcode : uint16_t {
  ONLINE_OK = 0,
  ONLINE_ERR_DUP_KEY,
  ONLINE_ERR_FILE_TOO_BIG,
  ONLINE_CHECK_DATA_CONSISTENCY_FAIL,
  ONLINE_ERR_OTHER
};

enum class OnlineDdlCheckMode : uint16_t {
  NO_CHECK = 0,
  FULL_CHECK_ONLY_AFTER_BUILD_INDEX,
  FULL_CHECK_ONLY_AFTER_ROW_LOG_REPLAYED,
  FULL_CHECK_ALL,
  INC_CHECK
};

enum class OnlineDdlIndexTupleStatus : uint8_t { INSERTED = 0, DELETED };

enum class DeserializeRet : uint8_t { SUCC = 0, FAIL, PARTIAL };

struct TuplesGatherResult {
  bool m_isGatherEnd{false};
  DSTORE::HeapTuple *m_oldTuple{nullptr};
  DSTORE::HeapTuple *m_newTuple{nullptr};
  void Reset() {
    m_isGatherEnd = false;
    m_oldTuple = nullptr;
    m_newTuple = nullptr;
  }
};

class TrxTuplesSaver {
  MockXid m_xid;
  DSTORE::HeapTuple *m_assembledOldTuple{nullptr};
  DSTORE::HeapTuple *m_oldTuples[MAX_HEAP_CHUNK_NUM]{};
  uint32_t m_oldChunkNum{0};
  DSTORE::HeapTuple *m_assembledNewTuple{nullptr};
  DSTORE::HeapTuple *m_newTuples[MAX_HEAP_CHUNK_NUM]{};
  uint32_t m_newChunkNum{0};

 public:
  TrxTuplesSaver(uint64_t xid) { m_xid.m_placeHolder = xid; }
  ~TrxTuplesSaver();
  void GatherNoLinkedTuples(DSTORE::ExposedUndoType undoType,
                            DSTORE::HeapTuple *oldTuple,
                            DSTORE::HeapTuple *newTuple,
                            TuplesGatherResult &result);
  void GatherLinkedTuples(const DSTORE::UndoExtraInfo *info,
                          DSTORE::HeapTuple *oldTuple,
                          DSTORE::HeapTuple *newTuple);
  bool IsLinkedTuplesGartherComplete() const;
  bool IsLinkedTuplesGartherCompleteInner(bool isOld) const;
  bool AssembleLinkedTuples(TuplesGatherResult &result);
};

class RollbackedTuplesGatherer {
  CDE::Memory::UnorderedMap<uint64_t, TrxTuplesSaver> m_trxTupleSavers;
  pthread_rwlock_t m_gathererLock;

 public:
  RollbackedTuplesGatherer() {
    int ret = pthread_rwlock_init(&m_gathererLock, nullptr);
    CDE_ASSERT(ret == 0);
  }
  ~RollbackedTuplesGatherer() {
    int ret = pthread_rwlock_destroy(&m_gathererLock);
    CDE_ASSERT(ret == 0);
  }
  static bool NeedGatherTuple(const DSTORE::UndoExtraInfo *info,
                              DSTORE::ExposedUndoType undoType,
                              DSTORE::HeapTuple *oldTuple,
                              DSTORE::HeapTuple *newTuple);
  bool IsTrxTupleSaverExists(uint64_t xid);
  TrxTuplesSaver *GetOrCreateTrxTupleSaver(uint64_t xid);
  void ClearTrxTupleSaver(uint64_t xid);
  bool GatherAndAssembleTuples(uint64_t xid, const DSTORE::UndoExtraInfo *info,
                               DSTORE::ExposedUndoType undoType,
                               DSTORE::HeapTuple *oldTuple,
                               DSTORE::HeapTuple *newTuple,
                               TuplesGatherResult &result);
};

struct OnlineDdlErrInfo {
  mysql_mutex_t m_mtx;
  bool m_isSet{false};
  OnlineDdlErrcode m_errcode{OnlineDdlErrcode::ONLINE_OK};
  uint32_t m_errIndexNo{UINT32_MAX};
  DSTORE::IndexTuple *m_errTuple{nullptr};

  OnlineDdlErrInfo() {
    int32_t ret = MutexInit(0, &m_mtx, MY_MUTEX_INIT_FAST);
    CDE_ASSERT(ret == 0);
  }

  ~OnlineDdlErrInfo() {
    int32_t ret = MutexDestroy(&m_mtx);
    CDE_ASSERT(ret == 0);
    if (m_errTuple) {
      Memory::Free(m_errTuple);
    }
    m_errTuple = nullptr;
  }

  void SetErrInfo(OnlineDdlErrcode errcode, uint32_t errIndexNo,
                  const DSTORE::IndexTuple *dupTuple);
};

struct CtidHashHelper {
  std::size_t operator()(const DSTORE::ItemPointerData &ctid) const {
    return std::hash<uint64_t>{}(ctid.m_placeHolder);
  }
};

class OnlineDdlMgr {
  OnlineDdlStatus m_status{OnlineDdlStatus::INVALID};
  std::mutex m_statusMtx;
  std::condition_variable m_statusCv;

  /** Uesd for rebuild table: map of ctid between new-table and old-table.
  cde_dict_t *m_ctidMapTable{nullptr};  */
  /** Uesd for rebuild table: map of column-number between new-table and
  old-table. The column-number of new-table as the array-index.
  uint32_t *m_mapColNewToOld{nullptr}; */

  /** TABLE object of old table. */
  const TABLE *m_mysqlTableOld{nullptr};

  const TABLE *m_mysqlTableNew{nullptr};

  const dd::Table *m_ddTableNew{nullptr};

  /** */
  ha_cde_inplace_ctx *m_ctx{nullptr};
  DSTORE::ThreadContext *m_thrd;
  my_thread_id m_threadId;

  RollbackedTuplesGatherer m_tupleGatherer;

  /** Threads to replay rowlog. */
  Memory::Vector<std::thread *> m_workers;

  bool m_needAbort = false;

  uint64_t m_replayAtLastBlockNum{0};
  std::mutex m_allInLastBlockMtx;
  std::condition_variable m_allInLastBlockCv;

  /** The error info at row log replay phase, if multiple threads
  encounter errors, only the frist one can set the error info. */
  OnlineDdlErrInfo m_errInfo;

  /** The snapshot used to build index. */
  DSTORE::SnapshotData m_indexBuildSnapshot;

  /** Whether OnlineDdlMgr aborts Thrd. This field is used to determine
  whether the interruption of thrd is caused by the kill command or an
  internal error. */
  bool m_abortBuild = false;

  /** Mutex used to protect the incremental data map */
  mysql_mutex_t m_heapIncDataMtx;

  /** The map used to store the ctid of incremental data */
  std::unordered_map<DSTORE::ItemPointerData, OnlineDdlIndexTupleStatus,
                     CtidHashHelper>
      m_heapIncDataMap;

  /** The ddl check mode */
  OnlineDdlCheckMode m_checkMode;

 public:
  OnlineDdlMgr(const TABLE *mysqlTableOld, const TABLE *mysqlTableNew,
               const dd::Table *ddTableNew, ha_cde_inplace_ctx *ctx,
               DSTORE::ThreadContext *thrd, my_thread_id threadId);
  ~OnlineDdlMgr();
  bool Init();
  bool RowLogRollbackForIndex(DSTORE::HeapTuple *oldTuple,
                              DSTORE::HeapTuple *newTuple);
  void RowlogDmlForIndex(IndexRowOp opType, DSTORE::Datum *values,
                         bool *isNulls, DSTORE::ItemPointer ctid,
                         DSTORE::Datum *valuesNew = nullptr,
                         bool *isNullsNew = nullptr,
                         DSTORE::ItemPointer ctidNew = nullptr);
  bool RowlogForIndexInner(session_rel_info *relInfo, IndexRowOp opType,
                           DSTORE::HeapTuple *heapTuple,
                           DSTORE::HeapTuple *heapTupleNew, const char *caller);
  bool StartRowLogReplayForIndex(cde_dict_index_t *dictIndex,
                                 uint64_t heapTupleNum, uint32_t indexNo);
  void WaitAllRowLogReplayThreadsExit();
  bool WaitCommitOrRollback();
  void NotifyCommitOrRollback() { m_statusCv.notify_all(); }
  void WaitUntilRowLogReplayNearlyFinish();
  void IncCntOfReachedLastBlock();
  void NotifyAllInLastBlock() { m_allInLastBlockCv.notify_all(); }
  void RowLogReplayForIndex(cde_dict_index_t *dictIndex, uint64_t heapTupleNum,
                            uint32_t indexNo, THD *mainThd);
  void SetAbort(OnlineDdlErrcode errcode, uint32_t errIndexNo = UINT32_MAX,
                const DSTORE::IndexTuple *dupTuple = nullptr);
  bool NeedAbort() const { return m_needAbort; }
  void ReportErrorInfo(TABLE *alteredTable, dstore_handler_t *dstoreHdl,
                       Alter_inplace_info *haAlterInfo) const;
  OnlineDdlStatus GetStatus() const { return m_status; }
  bool IsStatusValidForDml() const;
  void SetStatus(OnlineDdlStatus status);
  const TABLE *GetMysqlTableOld() const { return m_mysqlTableOld; }
  const char *GetTableNameOld() const;
  RollbackedTuplesGatherer &GetTuplesGatherer() { return m_tupleGatherer; }
  const ha_cde_inplace_ctx *GetInplaceCtx() const { return m_ctx; }
  std::string GetStatusString();
  DSTORE::ThreadContext *GetThrd() { return m_thrd; }
  my_thread_id GetThreadId() { return m_threadId; }
  OnlineDdlCheckMode GetCheckMode() { return m_checkMode; }
  bool CheckIndexDataConsistency(cde_dict_index_t *dictIndex,
                                 uint64_t heapTupleNum,
                                 bool isAllRowLogApplied);
  bool CheckIndexFullDataConsistency(
      cde_dict_index_t *dictIndex, uint64_t heapTupleNum,
      bool isAllRowLogApplied, DSTORE::IndexBuildInfo &buildInfo,
      ConvertHeapValueToIndexValue *ConvertFunctor);
  /** Check the incremental data consistency for the index */
  bool CheckIndexIncDataConsistency(
      cde_dict_index_t *dictIndex, DSTORE::IndexBuildInfo &buildInfo,
      ConvertHeapValueToIndexValue *ConvertFunctor);
  void AppendInsertTupleCtid(DSTORE::ItemPointerData ctid);
  void AppendDeleteTupleCtid(DSTORE::ItemPointerData ctid);
  bool IsTupleCtidExist(DSTORE::ItemPointerData ctid);
  bool PrepareCheckIndexData(cde_dict_index_t *dictIndex,
                             std::unique_ptr<session_rel_info> &relInfo,
                             DSTORE::IndexBuildInfo &buildInfo);
  void SetIndexBuildSnapShot();
  bool IsThrdAbortedByDdlMgr() { return m_abortBuild; }
};

struct IndexRowLogItem {
  IndexRowOp m_opType{ROW_IDX_OP_NONE};
  DSTORE::IndexTuple *m_indexTuple{nullptr};
  bool m_useExternalMem{false};
  IndexRowLogItem(unsigned char *tupleBuf, size_t bufSize) {
    CDE_ASSERT(bufSize >= MAX_INDEX_TUPLE_SIZE);
    m_useExternalMem = true;
    m_indexTuple = reinterpret_cast<DSTORE::IndexTuple *>(tupleBuf);
  }
  IndexRowLogItem(IndexRowOp op, DSTORE::IndexTuple *indexTuple)
      : m_opType(op), m_indexTuple(indexTuple) {}
  bool Serialize(unsigned char *) const;
  DeserializeRet Deserialize(const unsigned char *buf,
                             const unsigned char *bufEnd, uint32_t &itemSize);
  std::string ToString() const;
};

/** Log block for modifications during online ALTER TABLE */
struct RowLogBuf {
  /** File block buffer */
  unsigned char *m_block{nullptr};
  /** This buffer is used for writing or reading a record that spans two
  Aligned_buffer.  Thus, it must be able to hold one merge record,
  whose maximum size is the same as the minimum size of Aligned_buffer. **/
  unsigned char m_recMergeBuf[MERGE_BUF_SIZE_FOR_INDEX];
  /** Block number of Current position. */
  uint64_t m_blockNo{0};
  /** Offset in block of Current position. */
  uint64_t m_offsetInBlock{0};
};

class RowLog {
  /** The online ddl manager which the current object belongs. */
  OnlineDdlMgr *m_onlineMgr{nullptr};
  /** Total num of records */
  uint64_t m_rowCount{0};
  /** The num of records that has been replayed. */
  uint64_t m_replayRowCount{0};
  uint64_t m_rowLogBufSize{0};
  /** The size of headers and tuples contained in a block.*/
  uint64_t m_rowLogBufDataSize{0};
  /** File descriptor */
  int32_t m_fd{OS_FD_CLOSED};
  /** writer context; protected by mutex and index->lock S-latch, or by
  index->lock X-latch only */
  RowLogBuf m_tail;
  /** Reader context; protected by MDL only; modifiable by
  row_log_apply_ops() */
  RowLogBuf m_head;
  /** Used to protect m_tail. Prevent concurrent DML modifications to
  m_tail. */
  mysql_mutex_t m_appendMutex;
  /** Indicates the total number of tuples that have changed.*/
  std::atomic<int64_t> m_tupleChangedNum{0};

  /** Mutex used to protect the incremental data map */
  mysql_mutex_t m_incDataMtx;

  /** The map used to store the ctid of incremental data */
  std::unordered_map<DSTORE::ItemPointerData, OnlineDdlIndexTupleStatus,
                     CtidHashHelper>
      m_incrementalDataMap;

  bool m_isLocked{false};

  bool m_onceReachedToLastBlock{false};

 public:
  RowLog(OnlineDdlMgr *mgr);
  ~RowLog();
  bool Init(cde_dict_index_t *dictIndex);
  bool FileCreateAndOpen() noexcept;
  bool FileWrite(int32_t fd, const void *buf, uint64_t offset, uint64_t n);
  bool FileRead(int32_t fd, void *buf, uint64_t offset, uint64_t n);
  void LockAppend(const CdeSrcLocation &location);
  void LockAppendIfNeed(const CdeSrcLocation &location);
  void UnlockAppend();
  void UnlockAppendIfNeed();
  bool AppendLogItem(const IndexRowLogItem &item, uint32_t indexNo);
  /**
  Stop hanging if ddl trx is commiting or rolling back, or current dml generated
  more row log which pushed the m_tail.m_blockNo further.
  */
  void HangingIfReachedLastBlock();
  bool RowLogApplyOps(cde_dict_index_t *dictIndex, uint32_t indexNo);
  bool RowLogApplyOp(cde_dict_index_t *dictIndex, session_index_info *indexInfo,
                     IndexRowLogItem &item, DSTORE::Datum *values,
                     bool *isNulls, uint32_t indexNo);
  OnlineDdlMgr *GetOnlineDdlMgr() const { return m_onlineMgr; }
  uint64_t GetRowLogCount() { return m_rowCount; }
  uint64_t GetReplayedRowLogCount() { return m_replayRowCount; }
  void IncTupleChangedNum() {
    m_tupleChangedNum.fetch_add(1, std::memory_order_relaxed);
  }
  void DecTupleChangedNum() {
    m_tupleChangedNum.fetch_sub(1, std::memory_order_relaxed);
  }
  int64_t GetTupleChangedNum() {
    return m_tupleChangedNum.load(std::memory_order_relaxed);
  }
  void AppendInsertTupleCtid(DSTORE::ItemPointerData ctid);
  void AppendDeleteTupleCtid(DSTORE::ItemPointerData ctid);
  bool CheckDeletedDataConsistency(uint64_t &totalCheckIncDataNum);
};

class RowLogProgressMgr {
  mysql_mutex_t m_progressLock;
  /** map<index_oid, row_log_progress> */
  Memory::Map<RowLog *, cde_dict_index_t *> m_progressMap;

 public:
  RowLogProgressMgr();
  ~RowLogProgressMgr();
  static RowLogProgressMgr *GetInstance();
  void AddItem(RowLog *rowLog, cde_dict_index_t *dictIndex);
  void DeleteItem(RowLog *rowLog);
  const Memory::Map<RowLog *, cde_dict_index_t *> *GetProgressMap();
  mysql_mutex_t *GetProgressLock() { return &m_progressLock; }
};

void CdeRowLogForRollback(const DSTORE::UndoExtraInfo *info, uint64_t xidInt,
                          DSTORE::RollbackType rollbackType,
                          DSTORE::ExposedUndoType undoType,
                          DSTORE::HeapTuple *oldTuple,
                          DSTORE::HeapTuple *newTuple);

}  // namespace CDE
#endif /*__CDE_ONLINE_DDL_H__*/
