/* -------------------------------------------------------------------------
 *  This file is part of the cde-dstore project.
 * Copyright (c) 2024 Huawei Technologies Co.,Ltd.
 *
 * -------------------------------------------------------------------------
 *
 * ha_cde.cc
 *
 *
 * IDENTIFICATION
 * src/ha_cde.cc
 *
 * -------------------------------------------------------------------------
 */
// clang-format off
#include <algorithm>
#include <iomanip>
#include <pthread.h>
#include <scope_guard.h>
#include "dml/cde_heap.h"
#include "dml/cde_btree.h"
#include "handler/ha_cde.h"
#include "handler/ha_cde_parallel.h"
#include "common/cde_alloc.h"
#include "common/cde_session.h"
#include "dict/cde_dict.h"
#include "dict/cde_dict_bg_evict.h"
#include "dict/cde_dict_bg_stat.h"
#include "boot/cde_instance.h"
#include "common/cde_perf.h"
#include "common/cde_typecache.h"
#include "transaction/dstore_transaction_interface.h"
#include "errorcode/dstore_transaction_error_code.h"
#include "sql/ddl_info_file.h"
#include "sql/item.h"
#include "sql/recyclebin/recycle.h"
#include "sql/recyclebin/recycle_table.h"
#include "sql/debug_sync.h"
#include "sql/item_timefunc.h"
#include "sql/range_optimizer/tree.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/sql_lex.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/mysqld.h"
#include "sql/sql_table.h"
#include "sql/tztime.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/types/tablespace.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "sql/binlog.h"
#include "typelib.h"
#include "my_inttypes.h"
#include "my_base.h"
#include "m_string.h"
#include "my_sqlcommand.h"
#include "m_ctype.h"
#include "sql/ddl_info.h"
#include "sql/mysqld_thd_manager.h"
#include "dd/types/table.h"

#include "common/cde_def.h"
#include "common/cde_diagnose.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/cde_online_ddl.h"
#include "ddl/cde_tablespace.h"
#include "dml/cde_dml.h"
#include "common/cde_errorcode.h"
#include "handler/cde_information_schema.h"
#include "transaction/dstore_transaction_interface.h"
#include "my_dbug.h"

#include "tuple/dstore_memheap_tuple.h"
#include "tuple/dstore_tuple_interface.h"
#include "tuple/dstore_index_tuple.h"
#include "page/dstore_index_page.h"
#include "dml/cde_btree.h"
#include "perfcounters.h"
#include "perfpublisher.h"

#include "dml/cde_heap.h"
#include "perfpublisherproto.h"
#include "lock/dstore_lock_interface.h"
#include "sql/histograms/histogram.h"
#include "pdb/dstore_pdb_interface.h"
#include "backup/local_backup.h"
#include "rpl_wal/cde_pdb_replica.h"

#include "framework/dstore_instance.h"
#include "framework/dstore_parallel.h"
#include "ddl/cde_mysql_ops.h"
#include "buffer/dstore_buf_interface.h"
#include "transaction/dstore_transaction.h"
#include "sql/local_backup/local_backup_file_utils.h"

#include "ha_cdepart.h"

// clang-format on
using DSTORE::DSTORE_FAIL;
using DSTORE::g_storageInstance;
using DSTORE::HeapScanHandler;
using DSTORE::HeapTuple;
using DSTORE::INVALID_ITEM_POINTER;
using DSTORE::ItemPointer;
using DSTORE::ItemPointerData;
using DSTORE::PdbId;
using DSTORE::SCAN_KEY_ISNULL;
using DSTORE::SCAN_KEY_SEARCHNOTNULL;
using DSTORE::SCAN_KEY_SEARCHNULL;
using DSTORE::ScanDirection;
using DSTORE::ScanKey;
using DSTORE::ScanKeyData;
using DSTORE::ThreadContext;
using DSTORE::ThreadContextInterface;

namespace CDE {

struct handlerton *cde_hton_ptr = nullptr;
static unsigned long cde_default_row_format = ROW_TYPE_COMPACT;
static bool dstore_enable_perf_timers = false;
static bool dstore_enable_index_sample = true;
static bool g_calculateExactRowsForRange = false;
static ulong dstore_lock_wait_timeout = 50;
/** the max key len supported by DStore is 2569 in 4 TD, calculated through
* ((DATA_AND_TD_SIZE_ON_BTREE_PAGE - 4 * TDSize) / MIN_TUPLE_PER_BTREE_PAGE)
    - sizeof(ItemId) - sizeof(ItemPointerData) - sizeof(IndexTuple)
    - (HA_KEY_BLOB_LENGTH + HA_KEY_NULL_LENGTH) * MAX_REF_PARTS
* (((8096 - 4 * 40) / 3) - 4 - 8 - 16) - 3*16 = 2569
* for safety, we set cde_max_key_length to 2560.
*/
/** the max key len supported by DStore is 916 in 128 TD, calculated through
* ((DATA_AND_TD_SIZE_ON_BTREE_PAGE - 128 * TDSize) / MIN_TUPLE_PER_BTREE_PAGE)
    - sizeof(ItemId) - sizeof(ItemPointerData) - sizeof(IndexTuple)
    - (HA_KEY_BLOB_LENGTH + HA_KEY_NULL_LENGTH) * MAX_REF_PARTS
* (((8096 - 128 * 40) / 3) - 4 - 8 - 16) - 3*16 = 916
* for safety, we set cde_max_key_length to 910.
*/
constexpr static uint32_t max_tuple_length_by_limit_td = 2560;
constexpr static uint32_t max_tuple_length = 910;

/** File name to identify file which store show status info temporaily. */
const char *g_showStatusFileName = "engine_dstore_status";
/** Mutex to prevent show dstore status access file concurrently. */
static std::mutex g_showStatusFileMtx;
/** Max buffer size for show dstore status info to store. */
constexpr uint32_t g_maxShowStatusBufSize = (uint32_t)(1024 * 1024);

/** Possible values for system variable "cde_default_row_format". */
static const char *cde_default_row_format_names[] = {"redundant", "compact",
                                                     nullptr};
#define CDE_DEFAULT_ROW_FORMAT_CNT               \
  (sizeof(cde_default_row_format_names) /        \
       sizeof(cde_default_row_format_names[0]) - \
   1)

/** Dstore row format. */
enum dstore_row_format_enum {
  DSTORE_ROW_FORMAT_REDUNDANT = 0,
  DSTORE_ROW_FORMAT_COMPACT = 1
};

/** Used to define an enumerate type of the system variable
innodb_default_row_format. */
static TYPELIB cde_default_row_format_typelib = {
    CDE_DEFAULT_ROW_FORMAT_CNT, "cde_default_row_format_typelib",
    cde_default_row_format_names, nullptr};

/** Frees a Dstore transaction associated with the current THD.
 @return 0 or error number */
static int CdeCloseConnection(
    handlerton *hton, /*!< in/out: Dstore handlerton */
    THD *thd);        /*!< in: MySQL thread handle for
        which to close the connection */

/**
Cancel any pending lock request associated with the specified THD.

@param[in,out]  hton   handlerton for Dstore
@param[in]      thd    MySQL thread handle for which to close the connection
*/
static void CdeKillConnection(handlerton *hton, THD *thd);

/** Sets a transaction savepoint.
 @return always 0, that is, always succeeds */
static int CdeSavepoint(handlerton *hton, /*!< in/out: Dstore handlerton */
                        THD *thd,         /*!< in: handle to the MySQL thread */
                        void *savepoint); /*!< in: savepoint data */

/** Rolls back a transaction to a savepoint.
 @return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
 given name */
static int CdeRollbackToSavepoint(
    handlerton *hton, /*!< in/out: Dstore handlerton */
    THD *thd,         /*!< in: handle to the MySQL thread */
    void *savepoint); /*!< in: savepoint data */

/** Release transaction savepoint name.
 @return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
 given name */
static int CdeReleaseSavepoint(
    handlerton *hton, /*!< in/out: handlerton for Dstore */
    THD *thd,         /*!< in: handle to the MySQL thread */
    void *savepoint); /*!< in: savepoint data */

/** Prepare a transaction in DStore, currently it's just a mock fuction.
 @return 0 */
static int CdePrepare(handlerton *, THD *, bool) { return 0; }

/** Commits a transaction in an Dstore database or marks an SQL statement
 ended.
 @return 0 */
static int CdeCommit(handlerton *hton, /*!< in/out: Dstore handlerton */
                     THD *thd,         /*!< in: MySQL thread handle of the
                       user for whom the transaction should
                       be committed */
                     bool is_commit,   /*!< in: true
                                        - commit transaction false - the current
                                        SQL statement ended */
                     bool checkDdlInfo = false); /*!< in: true if we want to
                                                     assert ddl info exists.*/

/** Rolls back a transaction to a savepoint.
 @return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
 given name */
static int CdeRollback(handlerton *hton,  /*!< in/out: Dstore handlerton */
                       THD *thd,          /*!< in: handle to the MySQL thread
                          of the user whose transaction should
                          be rolled back */
                       bool is_rollback); /*!< in: TRUE - rollback entire
                                          transaction FALSE - rollback
                                          the current statement only */

/**
Rolls back ddl operation for transaction.

@param[in]  thd       The THD session object holding the transaction to be
*/
static void CdePostDDL(THD *thd);

/**
Delete statistics after some DDL that table file has been removed.

@param[in]  thd       The THD session object holding the transaction to be
@param[in]  name      Table name
*/
static void CdePostDeleteStatistics(THD *thd, const char *name);

/** Function for constructing an Dstore table handler instance.
@param[in,out]  hton        handlerton for Dstore
@param[in]  table       MySQL table
@param[in]  partitioned Indicates whether table is partitioned
@param[in]  mem_root    memory context */
static handler *CdeCreateHandler(handlerton *hton, TABLE_SHARE *table,
                                 bool partitioned, MEM_ROOT *mem_root);

/** Check tablespace name validity.
@param[in]     ts_cmd  whether this is tablespace DDL or not
@param[in]     name    name to check
@retval false  invalid name
@retval true   valid name */
bool CdeIsValidTablespaceName(ts_command_type ts_cmd, const char *name);

/** This API handles CREATE, ALTER & DROP commands for DStore tablespaces.
@param[in]      hton            Handlerton of DStore
@param[in]      thd             Connection
@param[in]      alter_info      Describes the command and how to do it.
@param[in]      old_ts_def      Old version of dd::Tablespace object for the
tablespace.
@param[in,out]  new_ts_def      New version of dd::Tablespace object for the
tablespace. Can be adjusted by SE. Changes will be persisted in the
data-dictionary at statement commit.
@return MySQL error code*/
int CdeAlterTablespace(handlerton *hton, THD *thd,
                       st_alter_tablespace *alter_info,
                       const dd::Tablespace *old_ts_def,
                       dd::Tablespace *new_ts_def);

/** Starts a new Dstore transaction if a transaction is not yet started. And
 assigns a new snapshot for a consistent read if the transaction does not yet
 have one.
 @return 0 */
static int CdeStartTrxAndConsistentSnapshot(
    handlerton *hton, /* in: Dstore handlerton */
    THD *thd);        /* in: MySQL thread handle of the
                      user for whom the transaction should
                      be committed */

static bool DstoreShowStatus(handlerton *hton, THD *thd,
                             stat_print_fn *statPrint,
                             enum ha_stat_type statType);
int DstoreShowStatus(handlerton *hton, THD *thd, stat_print_fn *statPrint);
/** Enable or Disable Dstore write ahead logging.
@param[in]  thd connection THD
@param[in]  enable  enable/disable redo logging
@return true iff failed. */
static bool CdeRedoSetState(THD *thd, bool enable);

static bool CdeGetTablespaceStatistics(const char *tablespace_name,
                                       const char *file_name,
                                       const dd::Properties &ts_se_private_data,
                                       ha_tablespace_statistics *stats);

/** Retrieve the tablespace type.
@param space        Tablespace object.
@param[out] space_type  Tablespace category.
@return true on success, true on failure */
// static bool cde_get_tablespace_type(const dd::Tablespace &space,
//                                          Tablespace_type *space_type);

/** Get the tablespace type given the name.
@param[in]  tablespace_name tablespace name
@param[out] space_type      type of space

@return Operation status.
@retval false on success and true for failure.
*/
// static bool cde_get_tablespace_type_by_name(const char *tablespace_name,
//                                                  Tablespace_type
//                                                  *space_type);

/** Check if types of child and parent columns in foreign key are compatible.
@param[in]  child_column_type   Child column type description.
@param[in]  parent_column_type  Parent column type description.
@param[in]  check_charsets      Indicates whether we need to check that charsets
of string columns match. Which is true in most cases.
@return True if types are compatible, False if not. */
static bool CdeCheckFkColumnCompat(const Ha_fk_column_type *child_column_type,
                                   const Ha_fk_column_type *parent_column_type,
                                   bool check_charsets);

static void CdeSetExtraCommitWal(THD *thd, const unsigned char *buffer,
                                 unsigned long length, bool write_extra_wal);

/**
 * Try to find parts of queries which can be pushed down to
 * storage engines for faster execution. This is typically
 * conditions which can filter out result rows on the SE,
 * and/or entire joins between tables.
 *
 * @param  thd         Thread context
 * @param  root        The AccessPath for the entire query.
 * @param  join        The JOIN struct built for the main query.
 *
 * @return Possible error code, '0' if no errors.
 */
static int CdePushCondToEngine(THD *thd, AccessPath *root, JOIN *join);

static void CdeRegisterDummyTrx(THD *thd);

static bool CdeStartFullLocalBackup(local_backup_config *config,
                                    local_backup_point *backup_point,
                                    bool involve_wal_archive,
                                    bool involve_standby);
static bool CdeStopFullLocalBackup();
static bool CdeStartWalArchive(local_backup_config *config);
static bool CdeStopWalArchive();
static bool CdeFinishFullBackupBinlog(local_backup_point &backupPoint);
static bool CdeWriteFullBackupMetaInfo(local_backup_point &backupPoint,
                                       std::string &path);
static bool CdeWriteFullBackupRestoreMeta(local_backup_point &backup_point,
                                          std::string &meta_path,
                                          std::string &restore_meta_path);
static bool CdeWriteFullBackupMetaJson(std::string *errMsg,
                                       local_backup_point *backupPoint,
                                       std::string &path);

static uint64_t CdeGetCurrentFlushedPlsn();
static int CdeSetRecoveryPlsnForTaurus(uint64_t plsn);

/**
 * Do single pdb promote
 *
 * @param  thd           Thread context
 * @param[out]  promote_info  result info of pdb promote
 *
 * @return result of pdb promote
 */
static bool CdePdbPromote(THD *thd, char *&promote_info);

/**
 * Do single pdb demote
 *
 * @param  thd         Thread context
 * @param[out]  promote_info  result info of pdb demote
 *
 * @return result of pdb demote
 */
static bool CdePdbDemote(THD *thd, char *&demote_info);

static void CdeRegisterTrx(THD *thd) /* in: MySQL thd (connection) object */
{
  const ulonglong trx_id =
      static_cast<ulonglong>(TransactionInterface::GetCurrentXid());

  trans_register_ha(thd, false, cde_hton_ptr, &trx_id);

  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
    trans_register_ha(thd, true, cde_hton_ptr, &trx_id);
  }
}

int CdeLockTable(lock_mode mode, DSTORE::StorageRelation relation,
                 bool dontWait) {
  // temporary table not lock table
  if (relation->rel->relpersistence == DSTORE::SYS_RELPERSISTENCE_GLOBAL_TEMP ||
      relation->rel->relpersistence == DSTORE::SYS_RELPERSISTENCE_TEMP) {
    return DSTORE::DSTORE_SUCC;
  }

  LockInterface::TableLockContext context;
  context.dbId = DSTORE::g_defaultPdbId;
  context.partId = 0;
  context.relId = relation->relOid;
  context.dontWait = dontWait;
  context.isSessionLock = false;
  context.isPartition = false;

  // when sql are select ... for update -> IX and select in share mode -> IS
  // lock table read -> S lock table write-> X

  DSTORE::LockMode dstore_lock_mod = DSTORE::LockMode::DSTORE_NO_LOCK;

  switch (mode) {
    case LOCK_IS:
      dstore_lock_mod = DSTORE::LockMode::DSTORE_ACCESS_SHARE_LOCK;
      break;
    case LOCK_IX:
      dstore_lock_mod = DSTORE::LockMode::DSTORE_ROW_EXCLUSIVE_LOCK;
      break;
    case LOCK_S:
      dstore_lock_mod = DSTORE::LockMode::DSTORE_SHARE_LOCK;
      break;
    case LOCK_X:
      dstore_lock_mod = DSTORE::LockMode::DSTORE_ACCESS_EXCLUSIVE_LOCK;
      break;
    case LOCK_AUTO_INC:
    case LOCK_NONE:
    case LOCK_NONE_UNSET:
    default:
      break;
  }

  context.mode = dstore_lock_mod;

  DSTORE::RetStatus ret = LockInterface::LockTable(&context);
  LockInterface::LockAcquireResult lock_result = context.result;

  switch (lock_result) {
    case LockInterface::LOCKACQUIRE_OK:
    case LockInterface::LOCKACQUIRE_ALREADY_HELD:
      CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
      break;
    case LockInterface::LOCKACQUIRE_NOT_AVAIL:
      CDE_LOG_DEBUG("lock not available, and dontWait=true");
      break;
    case LockInterface::LOCKACQUIRE_OTHER_ERROR:
      CDE_LOG_DEBUG("lock failed for some other error");
      break;
    default:
      break;
  }

  return ret;
}

uint64_t GetWalFlushedLsn(THD *thd) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);
  DSTORE::PdbId pdbId =
      ThreadContextInterface::GetCurrentThreadContext()->GetXactPdbId();
  DSTORE::WalInfo walInfo;
  walInfo.flushedPlsn = 0;
  walInfo.walId = UINT64_MAX;
  walInfo.replayedPlsn = 0;
  StoragePdbInterface::GetWalInfo(pdbId, &walInfo);
  return walInfo.flushedPlsn;
}

uint64_t GetWalStreamId(THD *thd) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);
  DSTORE::PdbId pdbId =
      ThreadContextInterface::GetCurrentThreadContext()->GetXactPdbId();
  DSTORE::WalInfo walInfo;
  walInfo.flushedPlsn = 0;
  walInfo.walId = UINT64_MAX;
  walInfo.replayedPlsn = 0;
  StoragePdbInterface::GetWalInfo(pdbId, &walInfo);
  return walInfo.walId;
}

/**
   Get the start lsn(stream state) from replica and send it to source as the
   starting position for transmitting WAL.

   @param[in]  thd  THD object.
   @return          the flushed lsn(stream state) of the wal log received on
   replica before replication is interrupted.
*/
uint64_t CdeGetReplicaWalStreamStartLsn(THD *thd) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);
  DSTORE::PdbId pdbId =
      ThreadContextInterface::GetCurrentThreadContext()->GetXactPdbId();
  uint64_t startLsn = UINT64_MAX;
  StoragePdbInterface::GetStandbyStreamStartPlsn(pdbId, &startLsn);
  return startLsn;
}

void AppendWal(THD *thd, uint64_t wal_stream_id, const uint8_t *data,
               uint32_t len, uint64_t start_lsn, uint64_t end_lsn) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);
  DSTORE::PdbId pdbId =
      ThreadContextInterface::GetCurrentThreadContext()->GetXactPdbId();
  StoragePdbInterface::StandbyAppendWal(pdbId, wal_stream_id, data, len,
                                        start_lsn, end_lsn);
}

int ReadWal(THD *thd, uint64_t wal_stream_id, uint64_t lsn, uint8_t *data,
            uint64_t read_len, uint64_t *result_len) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);
  DSTORE::PdbId pdbId =
      ThreadContextInterface::GetCurrentThreadContext()->GetXactPdbId();
  DSTORE::RetStatus ret = StoragePdbInterface::MasterReadWal(
      pdbId, wal_stream_id, lsn, data, read_len, result_len);
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    CDE_LOG_ERROR("master read wal fail, error: %d", ret);
    return CDE_ERROR;
  }
  return CDE_OK;
}

static void CdeShowDstoreInfo(THD *thd, const char *func_name,
                              const char *func_arg, String *res) {
  CdeCreateOrGetSession(thd);
  std::string r;
  DstoreShowInfo(func_name, func_arg, r);
  res->append(r.c_str());
  return;
}

static bool CdePdbPromote(THD *thd, char *&promote_info) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);

  DSTORE::RetStatus ret = CDE::CdeDoSinglePdbPromote(promote_info);
  if (ret != DSTORE::RetStatus::DSTORE_SUCC) {
    CDE_LOG_ERROR("CdePdbPromote fail: %s", promote_info);
    return true;
  }
  g_guc.enableStandbyRole = false;
  /* Initialize atomic DDL during master promotion,
  and process the records in the dstore_ddl_log table. */
  if (CdeDdlLogInit(false) != CDE_SUCC) {
    CDE_LOG_ERROR("CdeDdlLogInit fail when promote");
    return true;
  }
  CdeDdlLog::GetInstance()->ReplayAll();

  // pdb promote success.
  return false;
}

static bool CdePdbDemote(THD *thd, char *&demote_info) {
  cde_session_t *cde_sess = CdeCreateOrGetSession(thd);
  CDE_ASSERT(cde_sess != nullptr);

  DSTORE::RetStatus ret = CDE::CdeDoSinglePdbDemote(demote_info);
  if (ret != DSTORE::RetStatus::DSTORE_SUCC) {
    CDE_LOG_ERROR("CdePdbDemote fail: %s", demote_info);
    return true;
  }
  g_guc.enableStandbyRole = true;
  /* The physical standby does not need to record the dstore_ddl_log,
  it only replays modifications based on the WAL logs sent by the primary.*/
  CdeDdlLogDeint();

  // pdb demote success.
  return false;
}

/**
   Get dstore lock wait timeout(ms).

   @param[in]  thd  THD object.
   @return          lock wait timeout(ms).
*/
static int CdeGetLockWaitTimeout(THD *) {
  return dstore_lock_wait_timeout * SECONDS_TO_MILLISECONDS;
}

bool CheckWalFileSizeHwtValid(uint64_t walFileSizeHwtNewVal,
                              uint64_t &walFileSizeHwtMin) {
  constexpr int WALFILE_NUM_MINIMUM = 2;  // At least 2 walfile.
  walFileSizeHwtMin = std::max(WALFILE_NUM_MINIMUM, g_guc.walKeepSegments);
  uint64_t initWalFileCount =
      (uint64_t)g_storageInstance->GetInitWalFileCount();
  walFileSizeHwtMin = std::max(walFileSizeHwtMin, initWalFileCount);
  walFileSizeHwtMin *= static_cast<uint64_t>(g_guc.walFileSize);
  if (g_storageInstance->GetType() == DSTORE::StorageInstanceType::SINGLE &&
      walFileSizeHwtNewVal != DSTORE::DSTORE_WALFILE_SIZE_LIMIT_CHECK_DISABLE &&
      (uint64_t)(DSTORE::DSTORE_WALFILE_SIZE_HWT_RATE * walFileSizeHwtNewVal) <=
          walFileSizeHwtMin) {
    walFileSizeHwtMin = (uint64_t)(std::ceil(
        (walFileSizeHwtMin + 1) / DSTORE::DSTORE_WALFILE_SIZE_HWT_RATE));
    return false;
  }

  return true;
}

/**
   Stop dstore log thread.
*/
static void StopDstoreLog() { StorageLogInterface::StopLogAdapterInstance(); }

/** Initialize, validate and normalize the CDE startup parameters.
@return failure code
@retval 0 on success */
static int CdeInitParams() {
  /* Init the lock wait timeout threadshold in start up. */
  g_storageInstance->RegisterGetLockWaitTimeoutCallback([]() -> int {
    return dstore_lock_wait_timeout * SECONDS_TO_MILLISECONDS;
  });

  g_storageInstance->RegisterGetWalFileSizeHwtCallback(
      []() -> uint64_t { return g_guc.walFileSizeHwt; });

  return 0;
}

static void CdePostRecover() {
  if (CdeDdlLog::GetInstance()->IsServerInit()) {
    CdeDdlLog::GetInstance()->CreateTable();
  } else {
    CdeDdlLog::GetInstance()->ReplayAll();
  }
}

/** Invalidate an entry or entries for partitioned table from the dict cache.
@param[in]      schema_name     Schema name
@param[in]      table_name      Table name */
static void CdeDictCacheReset(const char *schemaName, const char *tableName) {
  char name[FN_REFLEN];
  build_table_filename(name, sizeof(name) - 1, schemaName, tableName, "", 0);
  OperationalLockGuard opWGuard;
  DictSysLockGuard lockGuard;
  cde_dict_t *dictTable = DictSysGetTableLocked(name, false);
  DictTableRefGuard dictRefGuard(dictTable);
  if (dictTable != nullptr) {
    dictRefGuard.Release();
    DictSysRemoveTableLocked(name, lockGuard);
  }
}

enum class DbugOpType : int { KEYWORD_EXIST = 0, DBUG_SET };

/**
dbug pushdown callback

@param[in]      name    keyword to judge if type==KEYWORD_EXIST,
                        dbug str to set if type==DBUG_SET
@param[in]      opType  KEYWORD_EXIST means check whether keyword exist,
                        DBUG_SET means set new dbug str
@param[in]      flag    dbug strict mode if opType == KEYWORD_EXIST

@return true if keyword is exist and false for other
*/
#ifndef NDEBUG
static bool CdeDbugCallback(const char *name, int opType, int flag) {
  if (opType == (int)DbugOpType::KEYWORD_EXIST) {
    return _db_keyword_(nullptr, name, flag);
  } else if (opType == (int)DbugOpType::DBUG_SET) {
    _db_set_(name);
  }
  return false;
}
#else
static inline bool CdeDbugCallback(const char *, int, int) { return false; }
#endif /* NDEBUG */

/** Find thd with pthread id whose query is blocked and generate a deadlock,
 dstore only have pthread_id, and we need filter thd whose query is empty,
 because in threadpool mode, multiple sessions/thd are mapping to one thread. */
class FindThdWithRealId : public Find_THD_Impl {
 public:
  explicit FindThdWithRealId(my_thread_t value) : mRealId(value) {}
  bool operator()(THD *thd) override {
    if (thd->real_id == mRealId) {
      mysql_mutex_lock(&thd->LOCK_thd_query);
      if (thd->query().length > 0 && thd->query().str != nullptr) {
        mysql_mutex_unlock(&thd->LOCK_thd_query);
        return true;
      }
      mysql_mutex_unlock(&thd->LOCK_thd_query);

      return false;
    }
    return false;
  }

 private:
  /** Pthread id to lookup thd */
  const my_thread_t mRealId;
};

char *GetSqlStmt(pthread_t tid, DSTORE::AllocMemFunc allocFunc) {
  FindThdWithRealId findWithRealId(tid);
  THD_ptr thdPtr =
      Global_THD_manager::get_instance()->find_thd(&findWithRealId);
  if (!thdPtr) {
    return nullptr;
  }

  const size_t extralen = 512;
  const size_t maxStmtLen = 512;
  size_t totalLen = extralen + maxStmtLen;
  /* Memory will be released in dstore after cb */
  char *infoStr = (char *)allocFunc(totalLen);
  if (!infoStr) {
    return nullptr;
  }

  /* THD_ptr already hold LOCK_thd_data in constractor, kill query or
  kill conn must acquire LOCK_thd_data first, so here we get thd raw ptr
  is safe. */
  char *ret = thd_security_context(thdPtr.get(), infoStr, totalLen, maxStmtLen);
  CDE_ASSERT(ret == infoStr);

  return infoStr;
}

/** Return partitioning flags. */
static uint dstore_partition_flags() {
  return (HA_CAN_EXCHANGE_PARTITION | HA_CANNOT_PARTITION_FK |
          HA_TRUNCATE_PARTITION_PRECLOSE);
}

/** Initialize the cde storage engine plugin.
@param[in,out]  p   cde handlerton
@return error code
@retval 0 on success */
static int CdeInit(void *p) {
  DBUG_TRACE;

  handlerton *cde_hton = (handlerton *)p;
  cde_hton_ptr = cde_hton;

  cde_hton->state = SHOW_OPTION_YES;
  // note:need to define in legacy_db_type enum firstly
  cde_hton->db_type = DB_TYPE_DSTORE;

  // cde_hton->savepoint_offset = 0;

  cde_hton->close_connection = CdeCloseConnection;
  cde_hton->kill_connection = CdeKillConnection;

  cde_hton->savepoint_set = CdeSavepoint;
  cde_hton->savepoint_rollback = CdeRollbackToSavepoint;
  // todo: need to check if Dstore can support in future
  // cde_hton->savepoint_rollback_can_release_mdl =
  //    cde_rollback_to_savepoint_can_release_mdl;
  cde_hton->savepoint_release = CdeReleaseSavepoint;
  cde_hton->prepare = CdePrepare;
  cde_hton->commit = CdeCommit;
  cde_hton->rollback = CdeRollback;

  cde_hton->post_ddl = CdePostDDL;

  cde_hton->post_delete_statistics = CdePostDeleteStatistics;

  /*todo: XA interface is not supported by dstore now, register XA interfaces
   * later*/

  cde_hton->create = CdeCreateHandler;
  cde_hton->is_valid_tablespace_name = CdeIsValidTablespaceName;
  cde_hton->alter_tablespace = CdeAlterTablespace;

  /*note:these APIs are not needed by Dstore*/
  // cde_hton->get_tablespace_filename_ext =
  //    cde_get_tablespace_filename_ext;
  // cde_hton->upgrade_tablespace = dd_upgrade_tablespace;
  // cde_hton->upgrade_space_version = upgrade_space_version;
  // cde_hton->upgrade_logs = dd_upgrade_logs;
  // cde_hton->finish_upgrade = dd_upgrade_finish;
  // cde_hton->pre_dd_shutdown = cde_pre_dd_shutdown;
  // cde_hton->panic = cdb_shutdown;

  cde_hton->partition_flags = dstore_partition_flags;

  cde_hton->start_consistent_snapshot = CdeStartTrxAndConsistentSnapshot;

  cde_hton->show_status = DstoreShowStatus;
  /*
  cde_hton->flush_logs = cde_flush_logs;

  cde_hton->lock_hton_log = cde_lock_hton_log;
  cde_hton->unlock_hton_log = cde_unlock_hton_log;
  cde_hton->collect_hton_log_info = cde_collect_hton_log_info;
  cde_hton->fill_is_table = cde_fill_i_s_table;

  cde_hton->replace_native_transaction_in_thd = cde_replace_trx_in_thd;
  cde_hton->file_extensions = ha_cde_exts;
  cde_hton->data = &cde_api_cb; */

  /* index extensions flag HTON_SUPPORTS_EXTENDED_KEYS is not supported
    because Dstore is not clustered index structure, and the leaf nodes
    of the secondary index do not store the primary key values.
  */

  cde_hton->flags = HTON_SUPPORTS_SECONDARY_ENGINE |
                    HTON_SUPPORTS_FOREIGN_KEYS | HTON_SUPPORTS_ATOMIC_DDL |
                    HTON_CAN_RECREATE | HTON_SUPPORTS_GENERATED_INVISIBLE_PK |
                    HTON_SUPPORTS_RECYCLE_BIN;

  /*to do:ddse and dict interfaces are considered later*/

  cde_hton->get_tablespace_statistics = CdeGetTablespaceStatistics;
  //   cde_hton->get_tablespace_type = cde_get_tablespace_type;
  //   cde_hton->get_tablespace_type_by_name =
  //       cde_get_tablespace_type_by_name;

  cde_hton->redo_log_set_state = CdeRedoSetState;

  /* to do: handler clone interfaces are not considered now. */

  cde_hton->foreign_keys_flags =
      HTON_FKS_WITH_PREFIX_PARENT_KEYS |
      HTON_FKS_NEED_DIFFERENT_PARENT_AND_SUPPORTING_KEYS |
      HTON_FKS_WITH_EXTENDED_PARENT_KEYS;

  cde_hton->check_fk_column_compat = CdeCheckFkColumnCompat;
  cde_hton->get_wal_flushed_lsn = GetWalFlushedLsn;
  cde_hton->get_wal_stream_id = GetWalStreamId;
  cde_hton->get_replica_wal_stream_start_lsn = CdeGetReplicaWalStreamStartLsn;
  cde_hton->append_wal = AppendWal;
  cde_hton->read_wal = ReadWal;
  cde_hton->set_extra_commit = CdeSetExtraCommitWal;
  cde_hton->register_dummy_trx = CdeRegisterDummyTrx;
  cde_hton->get_table_dd_info = CdeGetDstoreTableDDInfo;
  cde_hton->get_index_dd_info = CdeGetDstoreIndexDDInfo;
  cde_hton->push_to_engine = CdePushCondToEngine;

  cde_hton->start_full_local_backup = CdeStartFullLocalBackup;
  cde_hton->stop_full_local_backup = CdeStopFullLocalBackup;
  cde_hton->start_wal_archive = CdeStartWalArchive;
  cde_hton->stop_wal_archive = CdeStopWalArchive;
  cde_hton->finish_full_backup_binlog = CdeFinishFullBackupBinlog;
  cde_hton->write_full_backup_meta_info = CdeWriteFullBackupMetaInfo;
  cde_hton->write_full_backup_restore_meta = CdeWriteFullBackupRestoreMeta;
  cde_hton->write_full_backup_meta_json = CdeWriteFullBackupMetaJson;
  cde_hton->post_recover = CdePostRecover;
  cde_hton->get_current_flushed_plsn = CdeGetCurrentFlushedPlsn;
  cde_hton->set_recovery_plsn_for_taurus = CdeSetRecoveryPlsnForTaurus;

  cde_hton->show_dstore_info = CdeShowDstoreInfo;
  /** pdb switchover: promote and demote */
  cde_hton->pdb_promote = CdePdbPromote;
  cde_hton->pdb_demote = CdePdbDemote;

  /** get lock wait timeout */
  cde_hton->get_lock_wait_timeout = CdeGetLockWaitTimeout;

  cde_hton->stop_dstore_log = StopDstoreLog;

  cde_hton->dict_cache_reset = CdeDictCacheReset;

  DstoreAllocBoot();

  int ret = CdeStartupDstoreInstance(opt_initialize);

  if (ret != CDE_OK) {
    CDE_LOG_ERROR("boot dstore failed, ret=%d", ret);
    return CDE_ERROR;
  }
  ret = CdeModuleInit();
  if (ret != CDE_OK) {
    CDE_LOG_ERROR("CdeModuleInit fail.");
    return ret;
  }
  if (rds_write_binlog_into_redo) {
    DSTORE::WriteExtraWalCallback write_extra_commit_wal_cb =
        reinterpret_cast<DSTORE::WriteExtraWalCallback>(
            write_extra_commit_wal_callback);
    CdeRegisterWriteExtraCommitWalCallback(write_extra_commit_wal_cb);
  }

  CdeRegisterDbugPushdownCallback(CdeDbugCallback);
  CdeRegisterUndoExtraCallback(CdeRowLogForRollback);

  LockInterface::RegisterGetThreadSqlStatementCallback(GetSqlStmt);

  if (int error = CdeInitParams()) {
    return error;
  }

  // bg stat thread service init only in startup?
  if (!opt_initialize) {
    ret = CdeBgStatSrvInit();
    if (ret != CDE_OK) {
      return ret;
    }
    CdeBgEvictSrvInit();
  }
  CdeThreadlocalCreateKey();

  /* No need to init DDL log for replica. */
  if (!g_guc.enableStandbyRole && CdeDdlLogInit(opt_initialize) != CDE_SUCC) {
    return CDE_ERROR;
  }

  DBUG_EXECUTE_IF("crash_after_xid_is_written_and_dstore_recover_done",
                  { DBUG_SUICIDE(); });

  return ret;
}

static int CdeDeinit(void *p) {
  (void)p;
  CdeDdlLogDeint();
  relationCacheManager.destroy();

  int ret = CdeBgStatSrvDeinit();
  if (ret != CDE_OK) {
    CDE_LOG_ERROR("cde deinit failed, ret=%d", ret);
  }
  CdeBgEvictSrvDeinit();
  ret = CdeModuleExit();
  if (ret != CDE_OK) {
    CDE_LOG_ERROR("cde module exit failed, ret=%d", ret);
  }
  CdeThreadlocalDeleteKey();
  CdeShutdownDstoreInstance(opt_initialize);
  CDE_LOG_INFO("cde deinit OK");
  return ret;
}

/** close connection and release dstore session resource.
@param[in/out]  hton   Dstore handlerton
@param[in]      thd    MySQL thread handle for which to close the connection
@return 0 if close success. */
static int CdeCloseConnection(handlerton *hton, THD *thd) {
  CDE_ASSERT(hton == cde_hton_ptr);

  CDE_LOG_INFO("cde close connection, in_thd=%p, current_thd=%p", thd,
               current_thd);
  if (g_guc.enableStandbyRole && thd->for_ddl_info_replay) {
    cde_session_t *&cdeSessRef = CdeGetSessionRef(thd);
    CDE_ASSERT(cdeSessRef == nullptr);
    return 0;
  }
  /*The close_connection and kill_connection operations may execute
   concurrently. During kill_connection's execution, close_connection could
   prematurely release resources being used by kill_connection, potentially
   causing core dumps. Therefore, mutual exclusion locking must be implemented
   between these operations. Since each thread maintains its own mutex, this
   synchronization mechanism won't impact overall performance.*/

  /*During execution, kill_connection holds the LOCK_thd_data mutex. Therefore,
   acquiring LOCK_thd_data here will enforce mutually exclusive execution with
   kill_connection*/
  mysql_mutex_lock(&thd->LOCK_thd_data);
  cde_session_t *&cdeSessRef = CdeGetSessionRef(thd);
  /* close_connection needs to roll back all trx and free it.
     StorageInstance::UnregisterThread will do this clean. */
  CdeDestorySession(cdeSessRef);
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  cde_perf_counters::getInstance()->_alive_dstore_sessions.decrement();
  return 0;
}

/**
Cancel any pending lock request associated with the specified THD.

Abnormal Scenario should be skiped:
??1. Thread Exit and thrd has been destroyed, but Session Still References
the invalid thrd??
2. thd has been scheduled for execution, but has not yet accessed the dstore
API, so thrd has not been updated to current thrd.
3. After a thd finishes execution and is scheduled out, the session continues to
reference the ??old thrd. kill this thd may interrupt other thd

@param[in,out]  hton   handlerton for Dstore
@param[in]      thd    MySQL thread handle for which to close the connection
*/
static void CdeKillConnection(handlerton *hton, THD *thd) {
  CDE_ASSERT(hton == cde_hton_ptr);
  CDE_LOG_INFO("cde kill connection, in_thd=%p, current_thd=%p", thd,
               current_thd);
  // protected by LOCK_thd_data
  if (!thd->thrd_is_killable()) {
    return;
  }
  cde_session_t *session = CdeGetSession(thd, false);
  DBUG_EXECUTE_IF("thd_to_cde_session_null_cde_session_injection",
                  session = nullptr;);
  if (session == nullptr) return;
  CdeMutexGuard mutexGuard(&session->m_mutex, CDE_LOCATION_HERE);
  session->m_setInterrupt = true;
  if (session->m_inUninterruptablePhase) {
    return;
  }

  if (!CdeCheckThrdValid(session->dstore_thrd)) {
    return;
  }

  auto thread_ctx_inf = session->dstore_thrd;
  DBUG_EXECUTE_IF("kill_connection_null_thread_context_injection",
                  thread_ctx_inf = nullptr;);
  // wake up the thread to make them exit asap
  auto thread_ctx = (DSTORE::ThreadContext *)thread_ctx_inf;
  if (thread_ctx != nullptr && thread_ctx->m_session != nullptr) {
    auto thread_core = thread_ctx->GetCore();
    DBUG_EXECUTE_IF("kill_connection_null_thread_core_injection",
                    thread_core = nullptr;);
    if (thread_core != nullptr) {
      CDE_LOG_INFO("cde set interrupt pending, session id:%u, thread_ctx:%p",
                   thd->thread_id(), thread_ctx);
      thread_ctx->SetInterruptPending();
    }
  }
}

#define MAX_SAVEPOINT_NAME_LEN 64
#define CDE_BASE36 \
  36  // 36 is convenient in that the digits can be represented using the Arabic
      // numerals 0-9 and the Latin letters A-Z

static void CdeTrxGetNameOfSavepoint(uint64_t savepoint, char *name) {
  longlong2str(savepoint, name, CDE_BASE36);
}

static int CdeTrxCreateSavepoint(char *name) {
  DSTORE::RetStatus ret = TransactionInterface::CreateSavepoint(name);
  DBUG_EXECUTE_IF("cde_create_savepoint_injection", ret = DSTORE::DSTORE_FAIL;);
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    CDE_LOG_ERROR("create_savepoint fail, error: %d", ret);
    return GetAndConvertDstoreErrcodeToMysql();
  }
  return CDE_OK;
}

static int CdeTrxRollbackToSavepoint(char *name) {
  DSTORE::RetStatus ret = TransactionInterface::RollbackToSavepoint(name);
  DBUG_EXECUTE_IF("cde_rbk_savepoint_injection", ret = DSTORE::DSTORE_FAIL;);
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    CDE_LOG_ERROR("rollback_to_savepoint fail, error: %d", ret);
    return HA_ERR_NO_SAVEPOINT;
  }
  return CDE_OK;
}

static int CdeTrxReleaseSavepoint(char *name) {
  DSTORE::RetStatus ret = TransactionInterface::ReleaseSavepoint(name);
  DBUG_EXECUTE_IF("cde_rel_savepoint_injection", ret = DSTORE::DSTORE_FAIL;);
  if (unlikely(ret != DSTORE::DSTORE_SUCC)) {
    CDE_LOG_ERROR("release_savepoint fail, error: %d", ret);
    return HA_ERR_NO_SAVEPOINT;
  }
  return CDE_OK;
}

/** Sets a transaction savepoint.
 @return always 0, that is, always succeeds */
static int CdeSavepoint(handlerton *hton, THD *thd, void *savepoint) {
  DBUG_TRACE;
  DBUG_ASSERT(hton == cde_hton_ptr);

  (void)(hton);
  (void)CdeGetSession(thd);

  char name[MAX_SAVEPOINT_NAME_LEN];

  CdeTrxGetNameOfSavepoint((uint64_t)savepoint, name);
  int ret = CdeTrxCreateSavepoint(name);
  if (unlikely(ret != CDE_OK)) {
    return ConvertErrcodeToMysql(ret, 0, thd);
  }

  return 0;
}

/** Rolls back a transaction to a savepoint.
 @return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
 given name */
static int CdeRollbackToSavepoint(handlerton *hton, THD *thd, void *savepoint) {
  DBUG_TRACE;
  DBUG_ASSERT(hton == cde_hton_ptr);

  (void)(hton);
  (void)CdeGetSession(thd);

  char name[MAX_SAVEPOINT_NAME_LEN];

  CdeTrxGetNameOfSavepoint((uint64_t)savepoint, name);
  int ret = CdeTrxRollbackToSavepoint(name);
  if (unlikely(ret != CDE_OK)) {
    return ConvertErrcodeToMysql(ret, 0, thd);
  }

  return 0;
}

/** Release transaction savepoint name.
 @return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
 given name */
static int CdeReleaseSavepoint(handlerton *hton, THD *thd, void *savepoint) {
  DBUG_TRACE;
  DBUG_ASSERT(hton == cde_hton_ptr);

  (void)hton;
  (void)CdeGetSession(thd);

  char name[MAX_SAVEPOINT_NAME_LEN];

  CdeTrxGetNameOfSavepoint((uint64_t)savepoint, name);

  int ret = CdeTrxReleaseSavepoint(name);
  if (unlikely(ret != CDE_OK)) {
    return ConvertErrcodeToMysql(ret, 0, thd);
  }

  return 0;
}

/**
 * Update statistics after transaction commit or rollback
 *
 * When data is sampled on the dstore page, uncommitted data is not read like
 * innodb. As a result, the dstore encounters a problem: When data is inserted
 * in batches, the background recollects statistics. In this case, the
 * background statistics cannot collect the latest data information, and the
 * number of rows deviates greatly. Refer to the implementation of pg. pg
 * recollects statistics on table information only when a transaction is
 * submitted. Ensure that table statistics are executed only after a
 * transaction is committed.
 *
 * @param trx Transaction information
 */
void UpdateStatsAfterTrxCommitOrRoll(cde_trxinfo_t *trx) {
  for (const auto &table : trx->m_modTables) {
    CdeAddStatTable(table);
  }
  trx->m_modTables.clear();
}

/**
 * Update trxTable statModifiedCounter of specific dstore table
 *
 * @param trx Transaction information
 * @param tableId Dstore table id
 * @param statModifiedCounter Dstore table stat modified counter
 * @param statRows Dstore table stat rows
 */
void UpdateTrxTable(cde_trxinfo_t *trx, uint64_t tableId,
                    uint64_t statModifiedCounter, uint64_t statRows) {
  for (auto it = trx->m_modTables.begin(); it != trx->m_modTables.end(); ++it) {
    if (it->m_tableId == tableId) {
      it->m_statModifiedCounter = statModifiedCounter;
      it->m_statNRows = statRows;
      return;
    }
  }
}

/**
 * Update statistics of specific dstore table
 *
 * @param trx Transaction information
 * @param tableId Dstore table id
 * @param statModifiedCounter Dstore table stat modified counter
 * @param statRows Dstore table stat rows
 */
void UpdateStatsByOid(cde_trxinfo_t *trx, uint64_t tableId,
                      uint64_t statModifiedCounter, uint64_t statRows) {
  for (auto it = trx->m_modTables.begin(); it != trx->m_modTables.end();) {
    if (it->m_tableId == tableId) {
      it->m_statModifiedCounter = statModifiedCounter;
      it->m_statNRows = statRows;
      CdeAddStatTable(*it);
      it = trx->m_modTables.erase(it);
      break;
    } else {
      ++it;
    }
  }
}

/**
Is multistament trx run in process.

@return true if it is otherwise false.
*/
inline bool IsMultiStmtsTrxRunInProcess() {
  return TransactionInterface::IsTrxBlock();
}

/**
Is multi stmts trx starting.

@param[in]  thd  thread context.

@return true if it is otherwise false.
*/
inline bool IsMultiStmtsTrxStart(THD *thd) {
  return thd->in_multi_stmt_transaction_mode();
}

/**
Is singlestament trx run in process.

@return true if it is otherwise false.
*/
inline bool IsSingleStmtTrxInProcess() {
  return TransactionInterface::GetCurrentTBlockState() ==
         DSTORE::TBlockState::TBLOCK_STARTED;
}

/**
Is single stmt trx committing.

@param[in]  thd  thread context.

@return true if it is otherwise false.
*/
inline bool IsSingleStmtTrxCommit(THD *thd) {
  return !thd->in_multi_stmt_transaction_mode();
}

/**
Is single stmt trx rollbacking.

@param[in]  thd  thread context.

@return true if it is otherwise false.
*/
inline bool IsSingleStmtTrxRollback(THD *thd) {
  return !thd->in_multi_stmt_transaction_mode();
}

void CdeTrxStartIfNotStarted(THD *thd, bool rw) {
  /* The current physical standby server does not support transaction
   * initiation. */
  if (g_guc.enableStandbyRole) {
    return;
  }
  const cde_isolation_level_t isoLevel =
      CdeTrxMapIsolationLevel(thd->tx_isolation);
  cde_trxinfo_t *trx = CdeCreateOrGetTrxinfo(thd);

  /* Check status. */
  const DSTORE::TBlockState currentStat =
      TransactionInterface::GetCurrentTBlockState();
  CDE_ASSERT((!(currentStat == DSTORE::TBlockState::TBLOCK_BEGIN ||
                currentStat == DSTORE::TBlockState::TBLOCK_END ||
                currentStat == DSTORE::TBlockState::TBLOCK_ABORT_PENDING ||
                currentStat == DSTORE::TBlockState::TBLOCK_ABORT ||
                currentStat == DSTORE::TBlockState::TBLOCK_ABORT_END)));

  /* case1. multi statement trx run in process. */
  if (IsMultiStmtsTrxRunInProcess()) {
    MultiStmtsTrxRunInProcess(isoLevel, trx);
  } else if (IsSingleStmtTrxInProcess()) {
    /* case2. single statement trx run in process. for example create table
     * select.*/
  } else if (IsMultiStmtsTrxStart(thd)) {
    /* case3. multi statement trx start. */
    MultiStmtsTrxStart(isoLevel);
    TransactionInterface::SetTransactionExtraResPtr(thd);
  } else {
    /* case4. single statement trx start. */
    SingleStmtTrxStart(isoLevel);
    TransactionInterface::SetTransactionExtraResPtr(thd);
  }

  CdeRegisterTrx(thd);
  auto &ha_info = thd->get_ha_data(cde_hton_ptr->slot)->ha_info[0];
  CDE_ASSERT(ha_info.ht() == cde_hton_ptr);
  if (rw) {
    ha_info.set_trx_read_write();
  }
}

static bool s_checkDdlTrxExtralWalSize = true;

/**
Is called when one statement success.

@param[in]  hton        handlerton.
@param[in]  thd         thread context.
@param[in]  is_commit   ture if multi statement trx commit otherwise false.
@param[in]  checkDdlInfo  true if we want to assert ddl info exists.

@return CDE_OK if sucess.
*/
static int CdeCommit(handlerton *hton, THD *thd, bool is_commit,
                     bool checkDdlInfo) {
  DBUG_TRACE;
  DBUG_ASSERT(hton == cde_hton_ptr);
  (void)hton;

  if (unlikely(checkDdlInfo && s_checkDdlTrxExtralWalSize &&
               rds_dstore_enable_atomic_ddl)) {
    TransactionInterface::SetCheckExtraWalSizeFlag();
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
  cde_session_t *session = CdeGetSession(thd);

  for (cde_dict_t *dictTable : trx->m_autoincLockedTables) {
    dictTable->autoinc_dict.TableAutoincUnLock();
  }
  trx->m_autoincLockedTables.clear();
  /* Reset the number AUTO-INC rows required */
  trx->autoinc_row_num = 0;

  /* Do not update statistics if table is locked, delay update statistics
  when table is unlocked. */
  bool in_lock_tables = thd->variables.option_bits & OPTION_TABLE_LOCK;
  /* case1. simple statement trx commit(include ddl). */
  if (IsSingleStmtTrxCommit(thd)) {
    DBUG_ASSERT(is_commit == false);
    SingleStmtTrxCommit(session);
    ThreadContextInterface::GetCurrentThreadContext()->ResetQueryMemory();
    if (!in_lock_tables) {
      UpdateStatsAfterTrxCommitOrRoll(trx);
    }
  } else if (is_commit) {
    /* case2. multi statement trx commit. */
    MultiStmtsTrxCommit(session);
    if (!in_lock_tables) {
      UpdateStatsAfterTrxCommitOrRoll(trx);
    }
  } else {
    /* case3. multi statement trx sql end.*/
    MultiStmtsTrxOneStmtEnd(session);
    ThreadContextInterface::GetCurrentThreadContext()->ResetQueryMemory();
  }

  return CDE_OK;
}

/**
Is called when one statement failed.

@param[in]  hton        handlerton.
@param[in]  thd         thread context.
@param[in]  is_rollback   ture if multi statement trx rolback otherwise false.

@return CDE_OK if sucess.
*/
static int CdeRollback(handlerton *hton, THD *thd, bool is_rollback) {
  DBUG_TRACE;
  DBUG_ASSERT(hton == cde_hton_ptr);
  (void)hton;

  cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
  cde_session_t *session = CdeGetSession(thd);

  for (cde_dict_t *dictTable : trx->m_autoincLockedTables) {
    dictTable->autoinc_dict.TableAutoincUnLock();
  }
  trx->m_autoincLockedTables.clear();
  /* Reset the number AUTO-INC rows required */
  trx->autoinc_row_num = 0;

  /* Do not update statistics if table is locked, delay update statistics
  when table is unlocked. */
  bool in_lock_tables = thd->variables.option_bits & OPTION_TABLE_LOCK;
  /* case1. simple statement trx rollback(include ddl). */
  if (IsSingleStmtTrxRollback(thd)) {
    CDE_ASSERT(is_rollback == false);
    SingleStmtTrxRollback(session);
    if (!in_lock_tables) {
      UpdateStatsAfterTrxCommitOrRoll(trx);
    }
  } else if (is_rollback) {
    /* case2. multi statement trx rollback. */
    MultiStmtsTrxRollback(session);
    if (!in_lock_tables) {
      UpdateStatsAfterTrxCommitOrRoll(trx);
    }
  } else {
    /* case3. multi statement trx sql rollback.*/
    MultiStmtsTrxOneStmtRollback(session);
  }

  return CDE_OK;
}

static void CdePostDDL(THD *thd) { CdeDdlLog::GetInstance()->PostDdl(thd); }

static void CdePostDeleteStatistics(THD *thd, const char *name) {
  (void)DstoreDictStatDropTable(name, thd);
}

/** Function for constructing an Dstore table handler instance.
Dstore engine do not support partition table yet.
@param[in,out]  hton        handlerton for Dstore
@param[in]  table       MySQL table
@param[in]  partitioned Indicates whether table is partitioned
@param[in]  mem_root    memory context */
static handler *CdeCreateHandler(handlerton *hton, TABLE_SHARE *table,
                                 bool partitioned, MEM_ROOT *mem_root) {
  assert(mem_root != nullptr);

  if (partitioned) {
    haCdepart *file = new (mem_root) haCdepart(hton, table);
    if (file && file->init_partitioning(mem_root)) {
      destroy(file);
      return (nullptr);
    }
    return (file);
  }

  return (new (mem_root) ha_cde(hton, table));
}

int CdeKeypartMapToN(key_part_map keypart_map, int keypart_max) {
  int keypart_n = 0;
  while (keypart_map) {
    keypart_n++;
    keypart_map >>= 1;
  }
  return std::min(keypart_n, keypart_max);
}

#ifndef NDEBUG
/**
Print scan key info, include: index column number, data to compare,
strategy for index read, is null info.

@param[in]  key_info      scan key data
@param[in]  key_info_name scan key name

@return void.
*/
static void PrintScanKeyInfo(ScanKey key_info, std::string key_info_name) {
  CDE_ASSERT_DEBUG(key_info != nullptr);

  std::string extra_info = key_info_name + ": {";
  std::string strategy;
  switch (key_info->skStrategy) {
    case CDE_SCAN_ORDER_INVALID:
      strategy = "CDE_SCAN_ORDER_INVALID";
      break;
    case CDE_SCAN_ORDER_LESS:
      strategy = "CDE_SCAN_ORDER_LESS";
      break;
    case CDE_SCAN_ORDER_LESSEQUAL:
      strategy = "CDE_SCAN_ORDER_LESSEQUAL";
      break;
    case CDE_SCAN_ORDER_EQUAL:
      strategy = "CDE_SCAN_ORDER_EQUAL";
      break;
    case CDE_SCAN_ORDER_GREATEREQUAL:
      strategy = "CDE_SCAN_ORDER_GREATEREQUAL";
      break;
    case CDE_SCAN_ORDER_GREATER:
      strategy = "CDE_SCAN_ORDER_GREATER";
      break;
  }
  extra_info += "skAttno:" + std::to_string(key_info->skAttno) + ", ";
  extra_info += "skArgument:" + std::to_string(key_info->skArgument) + ", ";
  extra_info += "skStrategy:" + strategy + ", ";
  extra_info +=
      "SCAN_KEY_ISNULL:" + std::to_string(key_info->skFlags & SCAN_KEY_ISNULL) +
      ", ";
  extra_info +=
      "SCAN_KEY_SEARCHNULL:" +
      std::to_string(key_info->skFlags & SCAN_KEY_SEARCHNULL ? 1 : 0) + ", ";
  extra_info +=
      "SCAN_KEY_SEARCHNOTNULL:" +
      std::to_string(key_info->skFlags & SCAN_KEY_SEARCHNOTNULL ? 1 : 0) + "}";
  CDE_LOG_INFO("%s", extra_info.c_str());
}

/**
Print scan key info for index read, help developer to understand the scan key
info that change from ref scan condition or range scan condition.

@param[in]  handler  dstore handler
@param[in]  table    table object

@return void.
*/
static void PrintIndexScanKeyInfo(const dstore_handler_t *handler,
                                  const TABLE *table) {
  CDE_ASSERT_DEBUG(handler != nullptr);

  /* print first 100 characters of query to avoid use too many memory */
  CDE_LOG_INFO("Dstore index read info, read only index: %d, SQL Query: %.100s",
               (int)handler->read_just_key, table->in_use->query().str);

  /* return if no scan key */
  if (handler->keys_prefix_counts == 0) {
    return;
  }
  /* print index read key info */
  if (handler->m_keyInfos) {
    for (uint32_t i = 0; i < handler->keys_prefix_counts; ++i) {
      PrintScanKeyInfo(handler->m_keyInfos + i, handler->table_handler->name +
                                                    " ScanKey_IndexScan" +
                                                    std::to_string(i));
    }
  }
  /* print index rescan key info, do some explanation for rescan, for example:
  create table t(a int, key(a)); insert into t values(null),(1),(2),(3),(4);
  query is 'select * from t where a < 2 or a is null', sql layer create range
  less than 2, for InnoDB it will return back all data that less than 2 and
  include null data, then return "HA_ERR_END_OF_FILE", sql layer know that get
  all data from storage engine. For DStore, need to transfer start range into
  ScanKey for index read, index 'a' only include one cloumn, so need to create
  one scan key, scan key need to know access null value or not access null
  value, currently do not support access both null and not null in one pass.
  In order to shield the difference, DStore handler access dstore twice for
  above scenario, first access 2 and 1, dstore return "end_of_file", then
  handler rescan null value, then sql layer get all the data that needed. */
  if (handler->m_scanAgainKeyInfos) {
    for (uint32_t i = 0; i < handler->keys_prefix_counts; ++i) {
      PrintScanKeyInfo(handler->m_scanAgainKeyInfos + i,
                       handler->table_handler->name + " ScanKey_ScanAgain" +
                           std::to_string(i));
    }
  }
}
#endif /* NDEBUG */

/** Starts a new Dstore transaction if a transaction is not yet started. And
 assigns a new snapshot for a consistent read if the transaction does not yet
 have one.
 @return 0 */
static int CdeStartTrxAndConsistentSnapshot(
    handlerton *hton [[maybe_unused]], /* in: Dstore handlerton */
    THD *thd)                          /* in: MySQL thread handle of the
 user for whom the transaction should
 be committed */
{
  DBUG_TRACE;
  DBUG_ASSERT(hton == cde_hton_ptr);

  DSTORE::RetStatus ret = DSTORE::DSTORE_SUCC;
  CdeCreateOrGetTrxinfo(thd);

  int iso = CdeTrxMapIsolationLevel(thd->tx_isolation);
  if (iso != TRANSACTION_SNAPSHOT) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, HA_ERR_UNSUPPORTED,
                        "Dstore: WITH CONSISTENT SNAPSHOT"
                        " was ignored because this phrase"
                        " can only be used with"
                        " REPEATABLE READ isolation level.");
  }

  // SQL statement always call StartTrxCommand to TRANS_START
  ret = TransactionInterface::StartTrxCommand();
  DBUG_EXECUTE_IF("StartTrxCommand_injection", ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    DBUG_EXECUTE_IF("StartTrxCommand_injection",
                    TransactionInterface::CommitTrxCommand(););
    auto err_code = GetDstoreErrcode();
    return ConvertErrcodeToMysql(err_code, 0, thd);
  }
  /* Save current thd of this transaction to get trx i_s info */
  TransactionInterface::SetTransactionExtraResPtr(thd);

  // CdeStartTrxAndConsistentSnapshot is called by begin/start transaction
  // command enter into Dstore transactin block
  ret = TransactionInterface::BeginTrxBlock();
  DBUG_EXECUTE_IF("BeginTrxBlock_injection", ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    DBUG_EXECUTE_IF("BeginTrxBlock_injection",
                    TransactionInterface::CommitTrxCommand();
                    TransactionInterface::StartTrxCommand();
                    TransactionInterface::EndTrxBlock();
                    TransactionInterface::CommitTrxCommand(););
    auto err_code = GetDstoreErrcode();
    return ConvertErrcodeToMysql(err_code, 0, thd);
  }

  ret = TransactionInterface::CommitTrxCommand();
  DBUG_EXECUTE_IF("CommitTrxCommand_injection", ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    DBUG_EXECUTE_IF("CommitTrxCommand_injection",
                    TransactionInterface::StartTrxCommand();
                    TransactionInterface::EndTrxBlock();
                    TransactionInterface::CommitTrxCommand(););
    auto err_code = GetDstoreErrcode();
    return ConvertErrcodeToMysql(err_code, 0, thd);
  }

  TransactionInterface::SetIsolationLevel(iso);
  ret = TransactionInterface::SetSnapShot(false);
  DBUG_EXECUTE_IF("SetSnapShot_injection", ret = DSTORE::DSTORE_FAIL;);
  if (ret == DSTORE::DSTORE_FAIL) {
    DBUG_EXECUTE_IF("SetSnapShot_injection",
                    TransactionInterface::StartTrxCommand();
                    TransactionInterface::EndTrxBlock();
                    TransactionInterface::CommitTrxCommand(););
    auto err_code = GetDstoreErrcode();
    return ConvertErrcodeToMysql(err_code, 0, thd);
  }

  CdeRegisterTrx(thd);

  return 0;
}

/**
Dstore show status func

@param[in]      hton            the DStore handlerton
@param[in]      thd             the MySQL query thread of the caller
@param[in]      statPrint       print function
@param[in]      statType        status to show

Return 0 on success and non-zero on failure.
*/
static bool DstoreShowStatus(handlerton *hton, THD *thd,
                             stat_print_fn *statPrint,
                             enum ha_stat_type statType) {
  switch (statType) {
    case HA_ENGINE_STATUS:
      return DstoreShowStatus(hton, thd, statPrint);
    default:
      break;
  }
  return false;
}

/** Implements the SHOW ENGINE DSTORE STATUS command. Sends the output of the
DStore diagnose info to the client.
@param[in]      hton           the DStore handlerton
@param[in]      thd            the MySQL query thread of the caller
@param[in]      statPrint      print function
@return 0 on success */
int DstoreShowStatus(handlerton *hton [[maybe_unused]], THD *thd,
                     stat_print_fn *statPrint) {
  (void)CdeCreateOrGetSession(thd);

  std::unique_lock<std::mutex> lock(g_showStatusFileMtx);

  FILE *fp = fopen(g_showStatusFileName, "w+");
  if (fp == nullptr) {
    return -1;
  }

  rewind(fp);
  CollectDstoreStatus(fp);
  int result = ftruncate(fileno(fp), ftell(fp));
  DBUG_EXECUTE_IF("show_status_ftruncate_error", result = -1;);
  if (result != 0) {
    fclose(fp);
    /* Normally should return non-zero value, which will lead to mysqld
    crash in debug mode, so here just return success and content is empty. */
    return 0;
  }

  /* Calc valid file size. */
  size_t flen = ftell(fp);
  if (flen <= 0) {
    flen = 0;
  }
  if (flen > g_maxShowStatusBufSize) {
    flen = g_maxShowStatusBufSize;
  }
  char *str = (char *)malloc(flen + 1);

  /* Read data from file to buffer */
  rewind(fp);
  flen = fread(str, 1, flen, fp);

  lock.unlock();

  bool ret = statPrint(thd, DSTORE_ENGINE_NAME, strlen(DSTORE_ENGINE_NAME), "",
                       0, str, static_cast<uint>(flen));

  /* Release resource */
  fclose(fp);
  free(str);
  return ret;
}

/** Enable or Disable Dstore write ahead logging.
@param[in]  thd connection THD
@param[in]  enable  enable/disable redo logging
@return true iff failed. */
static bool CdeRedoSetState(THD *thd, bool enable) {
  (void)thd;
  (void)enable;
  return true;
}

static bool CdeGetTablespaceStatistics(const char *tablespace_name,
                                       const char *file_name,
                                       const dd::Properties &ts_se_private_data,
                                       ha_tablespace_statistics *stats) {
  (void)tablespace_name;
  (void)file_name;
  (void)ts_se_private_data;
  (void)stats;
  return true;
}

/** Retrieve the tablespace type.
@param space        Tablespace object.
@param[out] space_type  Tablespace category.
@return true on success, true on failure */
// static bool cde_get_tablespace_type(const dd::Tablespace &space,
//                                          Tablespace_type *space_type)
// {
//     (void)space;
//     (void)space_type;
//     return true;
// }

/** Get the tablespace type given the name.
@param[in]  tablespace_name tablespace name
@param[out] space_type      type of space

@return Operation status.
@retval false on success and true for failure.
*/
// static bool cde_get_tablespace_type_by_name(const char *tablespace_name,
//                                                  Tablespace_type *space_type)
// {
//     (void)tablespace_name;
//     (void)space_type;
//     return true;
// }

/** Check if types of child and parent columns in foreign key are compatible.
@param[in]	child_column_type	Child column type description.
@param[in]	parent_column_type	Parent column type description.
@param[in]	check_charsets		Indicates whether we need to check that
charsets of string columns match. Which is true in most cases.
@return True if types are compatible, False if not. */
static bool CdeCheckFkColumnCompat(const Ha_fk_column_type *child_column_type,
                                   const Ha_fk_column_type *parent_column_type,
                                   bool check_charsets) {
  (void)check_charsets;

  if ((child_column_type->type == dd::enum_column_types::STRING ||
       child_column_type->type == dd::enum_column_types::VARCHAR) &&
      (parent_column_type->type == dd::enum_column_types::STRING ||
       parent_column_type->type == dd::enum_column_types::VARCHAR)) {
    // It's strict now. we'll revise him in the future.
  } else if (child_column_type->type != parent_column_type->type) {
    return false;
  }

  /* todo: currently, cde do not support create fk with different size
   * for the following datatypes.
   * todo: for string types, we should take collation into consider
   */
  switch (parent_column_type->type) {
    case dd::enum_column_types::TINY:
    case dd::enum_column_types::SHORT:
    case dd::enum_column_types::LONG:
    case dd::enum_column_types::FLOAT:
    case dd::enum_column_types::DOUBLE:
    case dd::enum_column_types::LONGLONG:
    case dd::enum_column_types::INT24:
    case dd::enum_column_types::DECIMAL:
    case dd::enum_column_types::NEWDECIMAL:
      return child_column_type->is_unsigned == parent_column_type->is_unsigned;
    case dd::enum_column_types::TIMESTAMP:
    case dd::enum_column_types::DATE:
    case dd::enum_column_types::TIME:
    case dd::enum_column_types::DATETIME:
    case dd::enum_column_types::YEAR:
    case dd::enum_column_types::NEWDATE:
    case dd::enum_column_types::BIT:
    case dd::enum_column_types::TIMESTAMP2:
    case dd::enum_column_types::DATETIME2:
    case dd::enum_column_types::TIME2:
    case dd::enum_column_types::ENUM:
      return child_column_type->char_length == parent_column_type->char_length;
    case dd::enum_column_types::SET: {
      ulint child_col_len = calc_pack_length(
          child_column_type->type, child_column_type->char_length,
          child_column_type->elements_count,
          /* Should we treat BIT as char? */
          true, child_column_type->numeric_scale,
          child_column_type->is_unsigned);

      ulint parent_col_len = calc_pack_length(
          parent_column_type->type, parent_column_type->char_length,
          parent_column_type->elements_count,
          /* Should we treat BIT as char? */
          true, parent_column_type->numeric_scale,
          parent_column_type->is_unsigned);

      return child_col_len == parent_col_len;
    }
    case dd::enum_column_types::VAR_STRING:
    case dd::enum_column_types::VARCHAR:
    case dd::enum_column_types::STRING:
      // if one is binary - the other has to be binary too
      if (child_column_type->field_charset == &my_charset_bin ||
          parent_column_type->field_charset == &my_charset_bin) {
        check_charsets = true;
      }
      return false == check_charsets || (child_column_type->field_charset ==
                                         parent_column_type->field_charset);
    default:
      return true;
  }
  return false;
}

int ExtraCommitWalDdlInfoHandle(uint32_t controlFlag, uint64 endPlsn,
                                const unsigned char *buffer, uint32_t length) {
  if (length < FLAG_LEN) return DSTORE::DSTORE_FAIL;
  uint32_t flag = uint4korr(buffer);
  if (flag & HAS_DDL_INFO) {
    if (storage_engine_mode != static_cast<ulong>(ONLY_DSTORE)) {
      CDE_LOG_ERROR(
          "DDL info found in DStore WAL requires recovery. Please set "
          "storage_engine_mode=ONLY_DSTORE and perform a clean restart before "
          "changing to other modes.");
      return DSTORE::DSTORE_FAIL;
    }
    uint32_t startPosition = get_ddl_info_start_postion(buffer, length);
    if (startPosition == 0) return DSTORE::DSTORE_FAIL;
    uint64_t xid = uint8korr(buffer + startPosition);
    startPosition += BUF_SIZE_FOR_TRX_ID;
    if (startPosition >= length) return DSTORE::DSTORE_FAIL;
    uint16_t sqlLen = uint2korr(buffer + startPosition);
    startPosition += BUF_SIZE_FOR_DDL_SQL_LEN;
    if (startPosition > length) return DSTORE::DSTORE_FAIL;

    /* The physical standby replays directly from the WAL, without relying on
    the DDLInfoRecord file, and returns immediately after replay completion. */
    if (g_guc.enableStandbyRole) {
      if (startPosition >= length) return DSTORE::DSTORE_FAIL;
      CDE_LOG_INFO(
          "The handler layer detects pending DDL replay operations, sqlLen: "
          "%hu, endPlsn: %lu.",
          sqlLen, endPlsn);
      standby_replay_dd_query_info(endPlsn, buffer + startPosition, sqlLen);
      return DSTORE::DSTORE_SUCC;
    }

    if (controlFlag & DRAW_DDL_INFO) {
      if (put_ddl_info(endPlsn, buffer + startPosition, sqlLen, xid) !=
          DDL_INFO_OP_SUCCESS) {
        return DSTORE::DSTORE_FAIL;
      }
    }
    if (controlFlag & REPLAY_DDL_INFO_DIRECTLY) {
      replay_dd_query_info(endPlsn, buffer + startPosition, sqlLen);
    }
  }

  return DSTORE::DSTORE_SUCC;
}

int ExtraCommitWalBinlogHandle(uint32_t controlFlag,
                               const unsigned char *buffer, uint32_t length) {
  if ((controlFlag & PERSIST_GTIDS_IN_WAL_TO_FILE_FLAG) &&
      rds_dstore_support_binlog_check) {
    Gtid_set *gtid_set_read_from_file = read_gtids_in_wal_from_file();
    Gtid_set *gtids_in_wal =
        const_cast<Gtid_set *>(gtid_state->get_gtids_in_wal());

    if (gtid_set_read_from_file != nullptr) {
      global_sid_lock->wrlock();
      gtids_in_wal->add_gtid_set(gtid_set_read_from_file);
      global_sid_lock->unlock();
      Sid_map *sid_map = gtid_set_read_from_file->get_sid_map();
      delete gtid_set_read_from_file;
      delete sid_map;
    }

    persist_gtids_in_wal_to_file(gtids_in_wal);
    return DSTORE::DSTORE_SUCC;
  }
  if ((controlFlag & PERSIST_GTID_EXECUTED_TO_FILE_FLAG) &&
      rds_dstore_support_binlog_check) {
    Gtid_set *executed_gtids =
        const_cast<Gtid_set *>(gtid_state->get_executed_gtids());
    persist_gtids_in_wal_to_file(executed_gtids);
    return DSTORE::DSTORE_SUCC;
  }
  if (length < FLAG_LEN) {
    return DSTORE::DSTORE_FAIL;
  }
  uint32_t flag = uint4korr(buffer);
  uint32_t startPosition = FLAG_LEN;
  if (controlFlag & DRAW_GTID_INFO) {
    if (flag & HAS_GTID_INFO) {
      DrawGtidInfo(buffer, startPosition);
    } else if (flag & HAS_GTID_EXECUTED) {
      DrawGtidExecutedInfo(buffer, startPosition);
      return DSTORE::DSTORE_SUCC;
    }
  }

  if ((controlFlag & DRAW_BINLOG_INDEPENDENT) ||
      (controlFlag & DRAW_BINLOG_NON_INDEPENDENT)) {
    if (flag & HAS_BINLOG_EVENT) {
      // To Do handle binlog
    }
  }
  return DSTORE::DSTORE_SUCC;
}

/**
Callback function to handle reading out ddl info from wal buffer and perform
next action according to controlFlag:

DRAW_DDL_INFO  --> call put_dd_query_info to save ddl info in a file;

REPLAY_DDL_INFO_DIRECTLY  --> call replay_dd_query_info to directly replay this
ddl info.

The data stored in wal buffer:
case1 master enabled binlog:  write both binlog events and ddl info into wal as
one string as following:

- flags 32 bits
  1  has_binlog_events 1
  2  is_big_trx 0
  3  has_ddl_info 1
  4  ...
[
- gno
- gtid_len
- gtid
- trx_len
- trx
]  --- from binlog event
[
- trx_xid
- ddl_sql_len
- ddl_sql
]  --- for ddl info block
case2 master skip binlog: write only ddl info into wal as one string as
following:

- flags 32 bits
  1  has_binlog_events 0
  2  is_big_trx 0
  3  has_ddl_info 1
  4  ...
[
- trx_xid
- ddl_sql_len
- ddl_sql
] --- for ddl info block

@param[in] controlFlag Control flags determining the behavior of following
action for the parsed info.
@param[in] endPlsn     The end LSN (Log Sequence Number) associated with the
commit.
@param[in] buffer       Pointer to the buffer containing the data.
@param[in] len      Length of the buffer.

@retval DSTORE::DSTORE_SUCC Success.
@retval DSTORE::DSTORE_FAIL Failure.
*/
int CdeDdWriteExtraCommitWalCallback(uint32_t controlFlag, uint64 endPlsn,
                                     const unsigned char *buffer,
                                     uint32_t length) {
  if (controlFlag & SKIP_HANDLE_DDL_INFO) {
    return ExtraCommitWalBinlogHandle(controlFlag, buffer, length);
  }

  if (controlFlag & SKIP_HANDLE_BINLOG) {
    return ExtraCommitWalDdlInfoHandle(controlFlag, endPlsn, buffer, length);
  }

  int res = ExtraCommitWalBinlogHandle(controlFlag, buffer, length);
  if (res == DSTORE::DSTORE_FAIL) {
    return DSTORE::DSTORE_FAIL;
  }

  return ExtraCommitWalDdlInfoHandle(controlFlag, endPlsn, buffer, length);
}

/**
 * Parses a single GTID (Global Transaction Identifier) from the  buffer
 * and updates the GTID set in the current transaction log context.
 *
 * This function extracts the GTID number (GNO) and the UUID from the buffer,
 * maps the UUID to a SID number using the global SID map, and adds the
 * resulting GTID (SID + GNO) to the internal GTID set (`gtid_in_wals`). It also
 * ensures thread safety by acquiring a write lock on the global SID lock before
 * modifying shared data structures.
 *
 *
 * @param buffer         A pointer to the binary buffer containing GTID data.
 * @param [in,out] start_position Reference to the current position in the
 * buffer.
 */

void DrawGtidInfo(const unsigned char *buffer, uint32_t &start_position) {
  uint64_t gno = uint8korr(buffer + start_position);
  start_position += GNO_LEN;
  const unsigned char *uuid_ptr = buffer + start_position;
  start_position += UUID_LEN;
  Gtid_set *gtids_in_wal =
      const_cast<Gtid_set *>(gtid_state->get_gtids_in_wal());
  global_sid_lock->wrlock();
  binary_log::Uuid uuid;
  uuid.copy_from(uuid_ptr);
  rpl_sidno sidno = global_sid_map->add_sid(uuid);
  gtids_in_wal->ensure_sidno(sidno);
  gtids_in_wal->_add_gtid(sidno, gno);
  if (rds_binlog_dstore_trace) {
    char gtid_in_wal_str[binlog_write_redo_buffer_size];
    gtids_in_wal->to_string(gtid_in_wal_str);
    my_dstore_support_binlog_check_trace("Read gtid from wal: %s",
                                         gtid_in_wal_str);
  }
  global_sid_lock->unlock();
}

/**
 * Parses a GTID set (a collection of GTIDs) from the buffer and merges it
 * into the internal GTID set (`gtid_in_wals`) in the transaction log context.
 *
 * This function reads a GTID set from the buffer, creates a temporary
 * `Gtid_set` object using a local `Sid_map`, and adds it to the main
 * `gtid_in_wals` set. It ensures thread safety by acquiring a write lock on the
 * global SID lock during the operation.
 *
 *
 * @param buffer         A pointer to the binary buffer containing GTID set
 * data.
 * @param [in,out] start_position Reference to the current position in the
 * buffer.
 */

void DrawGtidExecutedInfo(const unsigned char *buffer,
                          uint32_t &start_position) {
  start_position += GTID_LEN;
  Gtid_set *gtids_in_wal =
      const_cast<Gtid_set *>(gtid_state->get_gtids_in_wal());
  global_sid_lock->wrlock();
  Sid_map sid_map(nullptr /*no rwlock*/);
  enum_return_status status;
  Gtid_set gtid_executed(&sid_map, (const char *)(buffer + start_position),
                         &status);
  if (rds_binlog_dstore_trace) {
    my_dstore_support_binlog_check_trace("Read gtid_executed set from wal: %s",
                                         buffer + start_position);
  }
  if (status == RETURN_STATUS_OK) {
    if (gtid_executed.is_empty()) {
      gtids_in_wal->clear();
    } else {
      gtids_in_wal->add_gtid_set(&gtid_executed);
    }
  }
  global_sid_lock->unlock();
}

/**
 * @brief Sets extra commit data in the Write-Ahead Log (WAL) for a given data.
 *
 * This function handles the process of setting extra commit data in the WAL for
 * a given data.
 *
 * @param thd The thread handler (THD) associated with the transaction.
 * @param buffer The buffer containing the extra commit data.
 * @param length The length of the extra commit data in bytes.
 */
static void CdeSetExtraCommitWal(THD *thd, const unsigned char *buffer,
                                 unsigned long length, bool write_extra_wal) {
  if (likely(rds_binlog_concurrent_commit || rds_dstore_support_binlog_check)) {
    CdeCreateOrGetSession(thd);
    if (write_extra_wal || !TransactionInterface::TrxIsInProgress()) {
      TransactionInterface::WriteBinlogExtraIntoWal(buffer, length);
    } else if (get_ddl_command_type(thd) == DDLCOM_NONE ||
               CdeIsDdlWithTrx(thd)) {
      CDE_ASSERT_DEBUG(TransactionInterface::TrxIsInProgress());
      if (TransactionInterface::SetExtraCommitWAL(buffer, length) ==
          DSTORE_FAIL) {
        my_abort();
      }
    } else {
      sql_print_warning(
          "Don't call WriteBinlogExtraIntoWal or SetExtraCommitWAL, query is "
          "%s",
          thd->query());
    }
  } else {
    cde_session_t *session = CdeCreateOrGetSession(thd);
    ThreadContextInterface *dstore_thrd = session->get_dstore_thrd();
    dstore_thrd = ThreadContextInterface::SwapCurrentThreadContext(dstore_thrd);
    auto reset_guard = create_scope_guard([dstore_thrd] {
      ThreadContextInterface::SwapCurrentThreadContext(dstore_thrd);
    });
    if (write_extra_wal || !TransactionInterface::TrxIsInProgress()) {
      TransactionInterface::WriteBinlogExtraIntoWal(buffer, length);
    } else if (get_ddl_command_type(thd) == DDLCOM_NONE ||
               CdeIsDdlWithTrx(thd)) {
      CDE_ASSERT_DEBUG(TransactionInterface::TrxIsInProgress());
      if (TransactionInterface::SetExtraCommitWAL(buffer, length) ==
          DSTORE_FAIL) {
        my_abort();
      }
    }
  }
}

static void CdeRegisterDummyTrx(THD *thd) {
  /* Do not record DDL logs during the replay phase on the physical standby. */
  if (g_guc.enableStandbyRole && thd->for_ddl_info_replay) {
    return;
  }
  if (CdeIsDdlLogDisable(thd)) {
    return;
  }

  /* Construct thrd is the session never done before. */
  cde_session_t *session = CdeCreateOrGetSession(thd);
  /* We only need register dummy trx when no trx was started at current.
  If a trx has been already started, we also tolerate it. */
  CdeTrxStartIfNotStarted(thd, true);
  if (TransactionInterface::IsReadWriteTransaction()) {
    return;
  }
  CdeDdlLog::GetInstance()->LogDummy(thd, session);
}

class ThrdCtxGuard {
 public:
  ThrdCtxGuard() {
    m_Thrd = DSTORE::ThreadContextInterface::Create();
    m_Thrd->InitializeBasic();
    m_Thrd->SetExtraCallback(ThreadTransactionCallback);
    CdeGetDstoreInstance()->AddVisibleThread(m_Thrd, DSTORE::g_defaultPdbId);
  }

  ~ThrdCtxGuard() {
    CdeGetDstoreInstance()->RemoveVisibleThread(m_Thrd);
    DSTORE::ThreadContextInterface::DestroyCurrentThreadContext();
    m_Thrd = nullptr;
  }

 private:
  ThreadContextInterface *m_Thrd;
};

DSTORE::PdbId GetCurrentPdbId() { return DSTORE::g_defaultPdbId; }

/**
Get name of PDB path and create directories for backup

@param[in,out]  config  path config of backup

@return false on success
*/
static bool PrepareFullLocalBackupDir(local_backup_config *config) {
  char path[FN_REFLEN_SE];
  StoragePdbInterface::GetPdbPath(DSTORE::g_defaultPdbId, path);
  std::string pathStr(path);
  std::string mysqlDataDir(mysql_real_data_home);

  /* Removes prefix: mysql_real_data_home. See InitGlobalGucConfig(). */
  std::string pdbPath = pathStr.substr(mysqlDataDir.length());
  size_t start = 0;
  std::string delimiter("/");
  size_t end = pdbPath.find(delimiter);
  bool error = false;

  /* Create dir: '#mysql_dstore/gs_pdb/PDB_17' */
  while (end != std::string::npos) {
    std::string substr = pdbPath.substr(start, end - start);
    config->data_base_path.append(delimiter).append(substr);

    DBUG_EXECUTE_IF("PrepareFullLocalBackupDir_mkdir_fail", {
      my_mkdir(config->data_base_path.c_str(), my_umask_dir, MYF(0));
    });

    if (!current_lb_use_obs &&
        my_mkdir(config->data_base_path.c_str(), my_umask_dir, MYF(0)) != 0) {
      error = true;
      CDE_LOG_ERROR("Prepare full backup mkdir %s failed. errno: %d",
                    config->data_base_path.c_str(), errno);
      break;
    }
    start = end + delimiter.length();
    end = pdbPath.find(delimiter, start);
  }
  if (!error) {
    std::string last = pdbPath.substr(start);
    config->data_base_path.append(delimiter).append(last);
    if (!current_lb_use_obs &&
        my_mkdir(config->data_base_path.c_str(), my_umask_dir, MYF(0)) != 0) {
      error = true;
      CDE_LOG_ERROR("Prepare full backup mkdir %s failed. errno: %d",
                    config->data_base_path.c_str(), errno);
    }
  }
  return error;
}

static bool CdeStartFullLocalBackup(local_backup_config *config,
                                    local_backup_point *backup_point,
                                    bool involve_wal_archive,
                                    bool involve_standby) {
  if (dstore_local_backup_meta_path == nullptr ||
      dstore_local_backup_meta_path[0] == '\0') {
    CDE_LOG_ERROR(
        "dstore_local_backup_meta_path is not configured. "
        "Start full local backup failed");
    return true;
  }

  if (!current_lb_use_obs &&
      access(dstore_local_backup_meta_path, F_OK | R_OK | W_OK | X_OK) != 0) {
    CDE_LOG_ERROR(
        "cannot access backup meta directory. Start full local backup failed");
    return true;
  }

  std::unique_ptr<ThrdCtxGuard> thrd_ctx(nullptr);
  if (DSTORE::ThreadContextInterface::GetCurrentThreadContext() == nullptr) {
    thrd_ctx.reset(new ThrdCtxGuard());
  }

  if (PrepareFullLocalBackupDir(config)) {
    CDE_LOG_ERROR("failed to prepare directory for full backup");
    return true;
  }
  config->meta_base_path = std::string(dstore_local_backup_meta_path);

  /* Set read buffer size as same value of OBS buffer size to get better
  performance when using OBS. */
  g_storageInstance->UpdateLocalBackupFileReadBufSize(
      current_lb_use_obs ? rds_lb_obs_buffer_size
                         : rds_dstore_lb_io_buffer_size);

  if (DSTORE::DSTORE_SUCC !=
      CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB, config,
                                  involve_wal_archive, involve_standby,
                                  backup_point)) {
    return true;
  }
  return false;
}

static bool CdeStopFullLocalBackup() {
  if (DSTORE::DSTORE_SUCC !=
      CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::STOP_FULL_LB, nullptr,
                                  false, false, nullptr)) {
    return true;
  }
  return false;
}

static bool CdeStartWalArchive(local_backup_config *config) {
  if (dstore_local_backup_meta_path == nullptr ||
      dstore_local_backup_meta_path[0] == '\0') {
    CDE_LOG_ERROR(
        "dstore_local_backup_meta_path is not configured. "
        "Start wal archive failed");
    return true;
  }

  if (access(dstore_local_backup_meta_path, F_OK | R_OK | W_OK | X_OK) != 0) {
    CDE_LOG_ERROR(
        "cannot access backup meta directory. Start wal archive failed");
    return true;
  }

  std::unique_ptr<ThrdCtxGuard> thrd_ctx(nullptr);
  if (DSTORE::ThreadContextInterface::GetCurrentThreadContext() == nullptr) {
    thrd_ctx.reset(new ThrdCtxGuard());
  }
  config->meta_base_path = std::string(dstore_local_backup_meta_path);

  if (DSTORE::DSTORE_SUCC !=
      CDE::ProcessWalArchive(CDE::LocalBackupCmd::START_WAL_ARCHIVE, config)) {
    return true;
  }
  return false;
}

static bool CdeStopWalArchive() {
  if (DSTORE::DSTORE_SUCC !=
      CDE::ProcessWalArchive(CDE::LocalBackupCmd::STOP_WAL_ARCHIVE, nullptr)) {
    return true;
  }
  return false;
}

static bool CdeFinishFullBackupBinlog(local_backup_point &backupPoint) {
  return (DSTORE::DSTORE_SUCC != CDE::FinishFullBackupBinlog(backupPoint));
}

static bool CdeWriteFullBackupMetaInfo(local_backup_point &backupPoint,
                                       std::string &path) {
  if (DSTORE::DSTORE_SUCC != CDE::WriteFullBackupMetaInfo(backupPoint, path)) {
    return true;
  }
  return false;
}

static bool CdeWriteFullBackupRestoreMeta(local_backup_point &backup_point,
                                          std::string &meta_path,
                                          std::string &restore_meta_path) {
  return CDE::WriteFullBackupRestoreMeta(backup_point, meta_path,
                                         restore_meta_path);
}

static bool CdeWriteFullBackupMetaJson(std::string *errMsg,
                                       local_backup_point *backupPoint,
                                       std::string &path) {
  if (DSTORE::DSTORE_SUCC !=
      CDE::WriteFullBackupMetaJson(errMsg, backupPoint, path)) {
    return true;
  }
  return false;
}

static uint64_t CdeGetCurrentFlushedPlsn() {
  uint64_t cur_plsn = DSTORE::INVALID_PLSN;
  StoragePdbInterface::GetCurrentFlushedPlsn(DSTORE::g_defaultPdbId, &cur_plsn);
  return cur_plsn;
}

static int CdeSetRecoveryPlsnForTaurus(uint64_t plsn) {
  return StoragePdbInterface::SetRecoveryPlsnForTaurus(DSTORE::g_defaultPdbId,
                                                       plsn);
}

/**
  Condition pushdown

  Push a condition to dstore storage engine for evaluation
  during table scans. The conditions will be cleared
  by calling handler::extra(HA_EXTRA_RESET) or handler::reset().

  The current implementation supports arbitrary AND nested conditions
  with comparisons between columns and constants (including constant
  expressions and function calls) and the following comparison operators:
  =, >, >=, <, <= and "between".

  If the condition consist of multiple AND'ed 'boolean terms',
  parts of it may be pushed, and other parts will be returned as a
  'remainder condition', which the server has to evaluate.

  handler::pushed_cond will be assigned the (part of) the condition
  which we accepted to be pushed down.

  Note that this handler call has been partly deprecated by
  handlerton::push_to_engine(), which does both join- and
  condition pushdown for the entire query AccessPath.
  The only remaining intended usage for ::cond_push() is simple
  update and delete queries, where the join part is not relevant.

  @param cond          Condition to be pushed down.

  @return Return the 'remainder' condition, consisting of the AND'ed
          sum of boolean terms which could not be pushed. A nullptr
          is returned if entire condition was supported.
*/
const Item *ha_cde::cond_push(const Item *cond) {
  /**
   * When push-down is enabled, some invalid data may be filtered out. The
   * update operation can succeed, but the replica node fails to replay because
   * it does not use push-down. To ensure replication safety, updates and
   * deletions will temporarily avoid using push-down.
   */
  return cond;
  DBUG_TRACE;
  assert(pushed_cond == nullptr);
  assert(cond != nullptr);
  const Item *remainder = cond;
  THD *const thd = table->in_use;
  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    return remainder;
  }

  if (thd->lex->all_query_blocks_list &&
      thd->lex->all_query_blocks_list->is_recursive()) {
    return remainder;
  }

  Item *term = const_cast<Item *>(cond);
  m_cond_handler.PrepareCondPush(term, thd->mem_root);

  pushed_cond = m_cond_handler.GetPushedConds();
  remainder = m_cond_handler.GetRemainderConds();
  return remainder;
}

/**
  Return extra handler specific text for EXPLAIN.
*/
std::string ha_cde::explain_extra() const {
  // The condition pushed to the engine is printed here, as is done for the
  // NDB engine (ha_ndbcluster::explain_extra()).
  std::string extra_str = "";
  extra_str = (pushed_cond != nullptr)
                  ? (", with pushed condition: " + ItemToString(pushed_cond))
                  : "";

  extra_str += (m_pushed_aggregate != nullptr) ? (", with aggregate") : "";
  return extra_str;
}

/**
  Condition pushdown

 * Push a condition to dstore storage engine for evaluation
 * during table scans. The conditions will be cleared
 * by calling handler::extra(HA_EXTRA_RESET) or handler::reset().

 * The current implementation supports arbitrary AND nested conditions
 * with comparisons between columns and constants (including constant
 * expressions and function calls) and the following comparison operators:
 * =, >, >=, <, <= and "between".

 * Try to find parts of queries which can be pushed down to
 * storage engines for faster execution. This is typically
 * conditions which can filter out result rows on the storage
 * engine.
 *
 * @param  thd         Thread context
 * @param  rootPath   The AccessPath for the entire query.
 * @param  join        The JOIN struct built for the main query.
 *
 * @return Possible error code, '0' if no errors. In practice, we cannot return
 * an error as the upper layer would assert. This return value only indicates
 * whether the function call itself succeeded, not whether the pushdown
 * operation was successful.
 */

static int CdePushCondToEngine(THD *thd, AccessPath *rootPath,
                               JOIN * /*join*/) {
  DBUG_TRACE;

  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    return CDE_OK;
  }

  if (thd->lex->all_query_blocks_list &&
      thd->lex->all_query_blocks_list->is_recursive()) {
    return CDE_OK;
  }

  Item *cond = nullptr;
  TABLE *const table = CdeGetBasicTable(rootPath, &cond);
  if (unlikely(table == nullptr) || unlikely(cond == nullptr)) {
    return CDE_OK;
  }

  if (cond->is_outer_reference()) {
    return CDE_OK;
  }
  // only support one table scan at a time
  if ((cond->used_tables() & ~PSEUDO_TABLE_BITS) !=
      table->pos_in_table_list->map()) {
    return CDE_OK;
  }

  ha_cde *const ha = dynamic_cast<ha_cde *>(table->file);
  if (unlikely(ha == nullptr)) {
    return CDE_OK;
  }
  ha->get_cond_handler().PrepareCondPush(cond, thd->mem_root);
  /* Explain will use this to show the pushed items*/
  ha->pushed_cond = ha->get_cond_handler().GetPushedConds();
  return CDE_OK;
}

void ha_cde::updateStatementSnapShot(ulint select_lock_type, cde_trxinfo_t *trx,
                                     bool forceUpdate) {
  CDE_ASSERT(trx != nullptr);

  if (trx->scanSnapshot.snapshotCsn == DSTORE::INVALID_CSN || forceUpdate) {
    CDEInitSnapshot(trx->scanSnapshot, select_lock_type);
  }
}

/** Obtain the private handler of cde session specific data.
@param[in,out]  thd MySQL thread handler.
@return reference to private handler */
cde_session_t *&CdeCreateOrGetSession(THD *thd) {
  cde_session_t *&cdeSessionRef =
      *(cde_session_t **)thd_ha_data(thd, cde_hton_ptr);
  DBUG_EXECUTE_IF("thd_to_cde_session_null_cde_session_injection",
                  cdeSessionRef = nullptr;);

  /* When a physical standby server is performing DDL replay,
  it does not require any interaction with the Dstore storage engine,
  and the instantiation of the cde_session_t structure must be prohibited. */
  if (g_guc.enableStandbyRole && thd->for_ddl_info_replay) {
    CDE_ASSERT(cdeSessionRef == nullptr);
    return cdeSessionRef;
  }
  /* Assigning a value to cde_session changes the ha_data in thd,
  and we must ensure that new_cde_session is fully initialized. */
  CdeConstructSession(cdeSessionRef);
  /*
  Only killable when the session->dstore_thrd has been updated,
  otherwise we may kill a wrong thrd
  */
  thd->set_thrd_is_killable(true);
  return cdeSessionRef;
}

cde_session_t *CdeGetSession(THD *thd, bool assign) {
  cde_session_t *session = *(cde_session_t **)thd_ha_data(thd, cde_hton_ptr);
  CDE_ASSERT(session != nullptr);
  if (unlikely(assign == false)) {
    return session;
  }
  session->dstore_thrd = ThreadContextInterface::GetCurrentThreadContext();
  /*
   Only killable when the session->dstore_thrd has been updated,
   otherwise we may kill a wrong thrd
   */
  thd->set_thrd_is_killable(true);
  DSTORE::StorageSession *ds = session->get_dstore_session();
  if (session->dstore_thrd && ds)
    session->dstore_thrd->AttachSessionToThread(ds);
  return session;
}

cde_session_t *&CdeGetSessionRef(THD *thd) {
  cde_session_t *&cdeSessionRef =
      *(cde_session_t **)thd_ha_data(thd, cde_hton_ptr);
  CDE_ASSERT(cdeSessionRef != nullptr);
  return cdeSessionRef;
}

/** Clear interruptpending flag
@param[in]  pointer to the session handle.
*/
static inline void CdeClearInterruptPending(cde_session_t *session) {
  CdeMutexGuard mutexGuard(&session->m_mutex, CDE_LOCATION_HERE);
  auto thread_ctx = static_cast<DSTORE::ThreadContext *>(session->dstore_thrd);
  thread_ctx->ClearInterruptPending();
  session->m_setInterrupt = false;
}

cde_trxinfo_t *CdeCreateOrGetTrxinfo(THD *thd) {
  cde_session_t *session = CdeCreateOrGetSession(thd);
  CdeClearInterruptPending(session);

  cde_trxinfo_t *trxinfo = session->get_trxinfo();
  if (TransactionInterface::GetCurrentTBlockState() ==
      DSTORE::TBlockState::TBLOCK_DEFAULT) {
    trxinfo->my_thd = thd;
    trxinfo->check_foreigns =
        !thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS);
  }
  return trxinfo;
}

cde_trxinfo_t *CdeGetTrxinfo(THD *thd) {
  cde_session_t *session = CdeGetSession(thd);
  cde_trxinfo_t *trxinfo = session->get_trxinfo();
  return trxinfo;
}

/** Construct ha_cde handler. */

ha_cde::ha_cde(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg),
      m_ds_mrr(this),
      m_int_table_flags(
          HA_NULL_IN_KEY | HA_ATTACHABLE_TRX_COMPATIBLE | HA_GENERATED_COLUMNS |
          HA_CAN_INDEX_VIRTUAL_GENERATED_COLUMN | HA_DESCENDING_INDEX |
          HA_SUPPORTS_DEFAULT_EXPRESSION | HA_BINLOG_ROW_CAPABLE |
          HA_REQUIRES_KEY_COLUMNS_FOR_DELETE | HA_AGGREGATE_PUSHDOWN),
      m_stored_select_lock_type(LOCK_NONE_UNSET),
      m_mysql_has_locked(),
      m_mem_root(PSI_NOT_INSTRUMENTED, 512),
      m_blob_mem_root(PSI_NOT_INSTRUMENTED, 512) {}

/** Destruct ha_cde handler. */

ha_cde::~ha_cde() {}

/** Create an cde table.
@retval 0 on success */
int ha_cde::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
                   dd::Table *table_def) {
  DBUG_TRACE;
  Huawei::Common::LatencyCounter::Timer create_latecy(
      cde_perf_counters::getInstance()->_cde_create_table_latency,
      dstore_enable_perf_timers);
  int ret = CDE_OK;
  THD *thd = ha_thd();
  if (create_info->key_block_size != 0) {
    push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                        ER_ILLEGAL_HA_CREATE_OPTION,
                        "Storage engine does not support KEY_BLOCK_SIZE. The "
                        "option is ignored.");
  }

  CdeTrxStartIfNotStarted(thd, true);

  // truncate table
  if (thd->lex->sql_command == SQLCOM_TRUNCATE) {
    ret = CdeTruncateTable(thd, name, table_def);
  } else {
    if (thd->recycle_state->is_recycle()) {
      if (thd->recycle_state->has_autoinc_col()) {
        CdeDdTableSetSePrivateDataAutoInc(table_def, 0);
      }
      if (DDTableHasRowVersions(*table_def)) {
        ret = DDClearInstantTable(*table_def);
      }
    }
    if (!ret) {
      ret = CdeCreateTable(thd, name, form, create_info, table_def);
    }
  }

  if (need_ddl_info_sql(thd) && !ret) {
    ret = generate_dstore_create_ddl_info(thd, create_info, form, table_def);
    if (ret) {
      CDE_LOG_ERROR("ddl info generating fail");
    }
  }

  DBUG_EXECUTE_IF("cde_create_table_error_before_commit", ret = CDE_ERROR;);
  DBUG_EXECUTE_IF("cde_create_table_crash_before_commit", DBUG_SUICIDE(););

  return ConvertErrcodeToMysql(ret, 0, ha_thd());
}

/** Stores a row in an CDE database, to the table specified in this
 handle.
 @return error code */

int ha_cde::write_row(uchar *record) /*!< in: a row in MySQL format */
{
  DBUG_TRACE;
  Huawei::Common::LatencyCounter::Timer write_row_latecy(
      cde_perf_counters::getInstance()->_cde_write_row_latency,
      dstore_enable_perf_timers);
  int error_result = CDE_OK;
  int error = CDE_OK;
  bool auto_inc_used = false;
  char savept_name[MAX_SAVEPOINT_NAME_LEN];
  void *temp_savept;

  /* Increase the write count of handler */
  ha_statistic_increment(&System_status_var::ha_write_count);

  THD *thd = ha_thd();
  cde_trxinfo_t *trx = CdeGetTrxinfo(thd);

  // todo: support temporary table
  if (table->next_number_field && record == table->record[0]) {
    int ret = 0;
    if (!m_skipUpdateAutoIncrement && (ret = update_auto_increment())) {
      return ConvertErrcodeToMysql(ret, 0, ha_thd());
    }

    auto_inc_used = true;
  }

  // todo: when creating table, alloc mem for m_rec_buf_data and
  // m_field_changed. && Free mem on dropping table uint32_t buf_size =
  // GetDstoreRecExtBufSize(form); m_rec_buf_data = CdeAlloc(buf_size);

  // insert must lock table in IX mode
  int lock_table_ret;
  if (m_dstore->sql_stat_start && m_dstore->select_lock_type != LOCK_NONE) {
    lock_table_ret =
        CdeLockTable(LOCK_IX, m_dstore->rel_info->rd_storage_releation);
    if (lock_table_ret != DSTORE::DSTORE_SUCC) {
      CDE_LOG_ERROR("lock table failed");
      return HA_ERR_LOCK_TABLE_FULL;
    }

    m_dstore->sql_stat_start = false;
  }

  auto colNum = m_dstore->table_handler->m_totalColCount;
  auto vColNum = m_dstore->table_handler->m_vColCount;
  dml_ins_ctx ctx(CdeGetTrxinfo(thd), m_dstore->table_handler,
                  m_dstore->rel_info, colNum, vColNum, thd);
  error = ctx.init();
  if (unlikely(error != CDE_OK)) {
    return ConvertErrcodeToMysql(error, 0, ha_thd());
  }

  if (unlikely(m_dstore->m_allowDuplicates)) {
    CdeTrxGetNameOfSavepoint((uint64_t)&temp_savept, savept_name);
    error_result = CdeTrxCreateSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());
    }
  }

  ctx.SetNeedCheckOnlineDdl(m_dstore->m_needCheckOnlineDdl);
  error = CdeDmlWriteRow(table, record, &ctx);
  m_dstore->m_needCheckOnlineDdl = ctx.NeedCheckOnlineDdl();
  /* to deal with allow duplicate key scenarios, including REPLACE INTO, INSERT
   * ON DUPLICATE KEY UPDATE, INSERT IGNORE INTO */
  if (unlikely(m_dstore->m_allowDuplicates && error != CDE_OK)) {
    error_result = CdeTrxRollbackToSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      (void)CdeTrxReleaseSavepoint(savept_name);
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());  // cannot happen
    }
  }

  if (unlikely(m_dstore->m_allowDuplicates)) {
    error_result = CdeTrxReleaseSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());  // cannot happen
    }
  }

  if (auto_inc_used) {
    if (trx->autoinc_row_num > 0) {
      --trx->autoinc_row_num;
    }
    DictAutoinc *autoinc_dict = &m_dstore->table_handler->autoinc_dict;
    SessionAutoincCtx *autoincCtx = &m_dstore->autoincCtx;
    uint64_t autoincInserted = table->next_number_field->val_int();
    uint64_t colMaxValue = table->next_number_field->get_max_int_value();
    bool needUpdateAutoinc = 0;
    if (error == HA_ERR_FOUND_DUPP_KEY) {
      /* A REPLACE command and LOAD DATA INFILE REPLACE
      handle a duplicate key error themselves, but we
      must update the autoinc counter if we are performing
      those statements. */
      switch (thd_sql_command(ha_thd())) {
        case SQLCOM_LOAD:
          needUpdateAutoinc = m_dstore->m_allowDuplicates;
          break;
        case SQLCOM_REPLACE:
        case SQLCOM_INSERT_SELECT:
        case SQLCOM_REPLACE_SELECT:
          needUpdateAutoinc = true;
          break;
        default:
          break;
      }
    } else if (error == CDE_OK) {
      /* If the actual value inserted is greater than
      the upper limit of the interval, then we try and
      update the table upper limit. Note: last_value
      will be 0 if get_auto_increment() was not called. */
      needUpdateAutoinc = (autoincInserted >= autoincCtx->m_lastVal);
    }
    if (needUpdateAutoinc) {
      UpdateTableAutoinc(autoincCtx, autoinc_dict, autoincInserted,
                         colMaxValue);
    }
  }
  if (error == CDE_OK) {
    m_dstore->table_handler->inc_n_rows();
    m_dstore->table_handler->stat_modified_counter++;
    UpdateTrxTable(trx, m_dstore->table_handler->m_id,
                   m_dstore->table_handler->stat_modified_counter,
                   m_dstore->table_handler->stat_n_rows);
  }
  write_row_latecy.end();

  DBUG_EXECUTE_IF("stmt_rollback_injection", error = HA_ERR_GENERIC;);
  DBUG_EXECUTE_IF("trx_rollback_injection", error = HA_ERR_LOCK_DEADLOCK;);
  return ConvertErrcodeToMysql(error, 0, ha_thd());
}

int ha_cde::update_row(const uchar *old_row, uchar *new_row) {
  DBUG_TRACE;
  int error_result = CDE_OK;
  int error = CDE_OK;
  char savept_name[MAX_SAVEPOINT_NAME_LEN];
  void *temp_savept;

  ha_statistic_increment(&System_status_var::ha_update_count);
  THD *thd = ha_thd();

  // update must lock table in IX mode
  int lock_table_ret;
  if (m_dstore->sql_stat_start && m_dstore->select_lock_type != LOCK_NONE) {
    lock_table_ret =
        CdeLockTable(LOCK_IX, m_dstore->rel_info->rd_storage_releation);
    if (lock_table_ret != DSTORE::DSTORE_SUCC) {
      CDE_LOG_ERROR("lock table failed");
      return HA_ERR_LOCK_TABLE_FULL;
      ;
    }

    m_dstore->sql_stat_start = false;
  }

  auto colNum = m_dstore->table_handler->GetTotalCols();
  auto vColNum = m_dstore->table_handler->m_vColCount;
  dml_upd_ctx ctx(CdeGetTrxinfo(thd), m_dstore->table_handler,
                  m_dstore->rel_info, colNum, vColNum, false, thd,
                  &m_blob_mem_root, table);
  error = ctx.init();
  if (unlikely(error != CDE_OK)) {
    return ConvertErrcodeToMysql(error, 0, ha_thd());
  }

  ctx.set_old_ctid(&m_dstore->dstore_heap_ctid);

  if (unlikely(m_dstore->m_allowDuplicates)) {
    CdeTrxGetNameOfSavepoint((uint64_t)&temp_savept, savept_name);
    error_result = CdeTrxCreateSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());
    }
  }

  ctx.SetNeedCheckOnlineDdl(m_dstore->m_needCheckOnlineDdl);
  error = CdeDmlUpdateRow(table, old_row, new_row, &ctx);
  m_dstore->m_needCheckOnlineDdl = ctx.NeedCheckOnlineDdl();
  if (unlikely(CdeEnableDamagePageManager() &&
               error ==
                   (int)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE)) {
    error_result = CdeTrxRollbackToSavepoint(savept_name);
    (void)CdeTrxReleaseSavepoint(savept_name);
    return ConvertErrcodeToMysql(error_result, 0, ha_thd());
  }
  // to deal with UPDATE IGNORE and allow DUP KEY scenarios
  if (unlikely(m_dstore->m_allowDuplicates && error != CDE_OK)) {
    error_result = CdeTrxRollbackToSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      (void)CdeTrxReleaseSavepoint(savept_name);
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());  // cannot happen
    }
  }

  if (unlikely(m_dstore->m_allowDuplicates)) {
    error_result = CdeTrxReleaseSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());  // cannot happen
    }
  }

  uint64_t autoincUpdated = 0;
  if (table->found_next_number_field) {
    /* Other storage-engine use more complex ways in ha_xxx::update_row
    to get updated autoinc value. But we checked code and found that
    get updated autoinc value from found_next_number_field is enough. */
    autoincUpdated = ParseIntFromField(table->found_next_number_field);
  }

  if (error == CDE_OK && autoincUpdated != 0) {
    /* We need the upper limit of the col type to check for
    whether we update the table autoinc value or not. */
    uint64_t colMaxValue = table->found_next_number_field->get_max_int_value();
    DictAutoinc *autoincDict = &m_dstore->table_handler->autoinc_dict;
    SessionAutoincCtx *autoincCtx = &m_dstore->autoincCtx;
    UpdateTableAutoinc(autoincCtx, autoincDict, autoincUpdated, colMaxValue);
  }

  if (error == CDE_OK) {
    m_dstore->table_handler->stat_modified_counter++;
    cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
    UpdateTrxTable(trx, m_dstore->table_handler->m_id,
                   m_dstore->table_handler->stat_modified_counter,
                   m_dstore->table_handler->stat_n_rows);
  }

  return ConvertErrcodeToMysql(error, 0, ha_thd());
}

/* see: ha_innobase::get_auto_increment
 */
void ha_cde::get_auto_increment(ulonglong offset, ulonglong increment,
                                ulonglong nb_desired_values,
                                ulonglong *first_value,
                                ulonglong *nb_reserved_values) {
  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  CDE_ASSERT_DEBUG(trx != nullptr);
  DictAutoinc *autoinc_dict = &m_dstore->table_handler->autoinc_dict;
  SessionAutoincCtx *autoincCtx = &m_dstore->autoincCtx;

  LockAutoinc(autoinc_dict, trx->m_autoincLockedTables,
              thd_sql_command(ha_thd()), m_dstore->table_handler);

  /* Determine the first value of the interval */
  uint64_t autoincValue = autoinc_dict->GetAutoinc();
  /* It should have been initialized during open. */
  CDE_ASSERT(autoincValue != 0);

  /* We need the upper limit of the col type to check for
  whether we update the table autoinc counter or not. */
  ulonglong col_max_value = table->next_number_field->get_max_int_value();

  /** The following logic is needed to avoid duplicate key error
  for autoincrement column.

  (1) cde gives the current autoincrement value with respect
  to increment and offset value.

  (2) Basically it does compute_next_insert_id() logic inside cde
  to avoid the current auto increment value changed by handler layer.

  (3) It is restricted only for insert operations. */

  if (increment > 1 && m_dstore->table_handler->skip_alter_undo == false &&
      autoincValue < col_max_value) {
    ulonglong diff = ULLONG_MAX - autoincValue;
    /* Check for overflow */
    if (increment <= diff) {
      ulonglong prev_auto_inc = autoincValue;
      // offset <= increment
      autoincValue = ((autoincValue - 1) + increment - offset) / increment;

      autoincValue = autoincValue * increment + offset;

      /* If autoinc exceeds the col_max_value then reset
      to old autoinc value. Because in case of non-strict
      sql mode, boundary value is not considered as error. */
      if (autoincValue >= col_max_value) {
        autoincValue = prev_auto_inc;
      }

      CDE_ASSERT(autoincValue > 0);
    }
  }

  CDE_ASSERT(*first_value <= col_max_value);
  /* Called for the first time ? */
  if (trx->autoinc_row_num == 0) {
    trx->autoinc_row_num = (uint32_t)nb_desired_values;

    /* It's possible for nb_desired_values to be 0:
    e.g., INSERT INTO T1(C) SELECT C FROM T2; */
    if (nb_desired_values == 0) {
      trx->autoinc_row_num = 1;
    }

    *first_value = std::max(*(uint64_t *)first_value, autoincValue);
    /* Not in the middle of a mult-row INSERT. */
  } else if (autoincCtx->m_lastVal == 0) {
    *first_value = std::max(*(uint64_t *)first_value, autoincValue);
    /* Check for -ve values. */
  }

  *nb_reserved_values = trx->autoinc_row_num;

  /* With old style AUTOINC locking we only update the table's
  AUTOINC counter after attempting to insert the row. */
  if (g_autoincLockMode != AUTOINC_LOCK_TRADITIONAL ||
      autoincCtx->m_noAutoincLocking) {
    ulonglong current;
    ulonglong next_value;

    current = *first_value > col_max_value ? autoincValue : *first_value;

    /* Compute the last value in the interval */
    next_value = CalcNextAutoinc(current, *nb_reserved_values, increment,
                                 offset, col_max_value);

    autoincCtx->m_lastVal = next_value;

    if (autoincCtx->m_lastVal < *first_value) {
      *first_value = (~(ulonglong)0);
    } else {
      /* Update the table autoinc variable */
      autoinc_dict->SetAutoincIfGreater(autoincCtx->m_lastVal);
    }
  } else {
    /* This will force write_row() into attempting an update
    of the table's AUTOINC counter. */
    autoincCtx->m_lastVal = 0;
  }

  /* The increment to be used to increase the AUTOINC value, we use
  this in write_row() and update_row() to increase the autoinc counter
  for columns that are filled by the user. We need the offset and
  the increment. */
  autoincCtx->m_autoincOffset = offset;
  autoincCtx->m_autoincIncrement = increment;

  autoinc_dict->MutexExit();
}

void ha_cde::release_auto_increment() {
  /* No need to pin relinfo to session as get_auto_increment already
     did this */
  /* Clear the trx's autoinc_rows as the operation requiring auto increment
  values has been completed.

  We usually end up in this scenario when we do not know the correct estimation
  of rows in the table in the case of bulk inserts. */
  cde_session_t *session =
      *(cde_session_t **)thd_ha_data(ha_thd(), cde_hton_ptr);
  DBUG_EXECUTE_IF("cde_release_auto_increment_nullptr", { session = nullptr; });
  if (session != nullptr) {
    cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
    if (trx->autoinc_row_num > 0) {
      trx->autoinc_row_num = 0;
    }
  }
}

// todo:set flag according to dstore
handler::Table_flags ha_cde::table_flags() const {
  return (m_int_table_flags | HA_BINLOG_STMT_CAPABLE);
}

ulong ha_cde::index_flags(uint key, uint, bool) const {
  key = key;
  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
                HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;
  return flags;
}

session_index_info *ha_cde::cde_index_lookup(int keynr) {
  const auto &key_name = table->key_info[keynr].name;
  for (session_index_info *sess_idx : m_dstore->rel_info->cde_index_vec) {
    if (!strcmp(sess_idx->shared_dict->name.c_str(), key_name)) {
      return sess_idx;
    }
  }
  return nullptr;
}

uint32_t ha_cde::cde_index_lookup(const char *index_rel_name) {
  for (uint32_t keynr = 0; keynr < table->s->keys; keynr++) {
    const char *key_name = table->key_info[keynr].name;
    if (!strcmp(index_rel_name, key_name)) {
      return keynr;
    }
  }
  return (~0);
}

void CdeSetTableFlagsFromTableShare(cde_dict_t *table,
                                    const TABLE_SHARE *table_share) {
  table->set_stats_persistent(
      table_share->db_create_options & HA_OPTION_STATS_PERSISTENT,
      table_share->db_create_options & HA_OPTION_NO_STATS_PERSISTENT);

  table->set_stats_auto_recalc(
      table_share->stats_auto_recalc & HA_STATS_AUTO_RECALC_ON,
      table_share->stats_auto_recalc & HA_STATS_AUTO_RECALC_OFF);

  table->stat_sample_pages = table_share->stats_sample_pages;
}

/**
Open an cde table.
@param[in]  name        table name
@param[in]  open_flags  flags for opening table from SQL-layer.
@param[in]  table_def   dd::Table object describing table to be opened
@retval 1 if error
@retval 0 if success
*/
int ha_cde::open(const char *name, int, uint open_flags,
                 const dd::Table *table_def) {
  (void)open_flags;
  // tmp add to eliminate warning
  open_flags = open_flags;
  const dd::Table *tab_def = table_def;

  DBUG_TRACE;
  DBUG_ASSERT(table_share == table->s);

  THD *thd = ha_thd();
  if (tab_def == nullptr) {
    dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
    /* attempt to acquire table definition from DD, if still cannot successfully
    get table_def, report error */
    if (thd->dd_client()->acquire(table_share->db.str,
                                  table_share->table_name.str, &tab_def) ||
        tab_def == nullptr) {
      CDE_LOG_ERROR("can't acquire table definition: %s", name);
      return HA_ERR_NO_SUCH_TABLE;
    }
  }

  CdeCreateOrGetSession(thd);

  // do not support partition table right now, so do not need
  // convert table name
  TABLE *open_table_form = table;
  uint64_t autoincSaved = 0;

  OperationalLockGuard opWGuard;
  DictSysLockGuard lockGuard;
  cde_dict_t *cde_table = DictSysGetTableLocked(name, false);
  /* Only used to avoid opening a table twice when replaying DDL info. */
  bool ddl_info_skip_discardAfterDDL = false;

  if (cde_table != nullptr) {
    bool is_create_view_ddl_replay =
        thd->for_ddl_info_replay && (cde_table->getRefCnt() > 1) &&
        (thd->lex->sql_command == SQLCOM_CREATE_VIEW);
    ddl_info_skip_discardAfterDDL = is_create_view_ddl_replay;
  }
  DictTableRefGuard dictRefGuard(cde_table);

  std::unique_ptr<char[]> oldTableName;
  if (cde_table && cde_table->m_discardAfterDDL &&
      !ddl_info_skip_discardAfterDDL) {
    /** All the handlers that were openned during DDL are now closed.
    We are under dict sys lock - to make sure we are the first
    to discard this table. No other handler for this handler
    should get open during this operation. Otherwise, we would
    not be the only onces that hold reference to this table
    and we would not be able to discard it. */
    DEBUG_SYNC_C("before_get_refcnt");
    CDE_ASSERT_DEBUG(cde_table->getRefCnt() == 1);
    autoincSaved = cde_table->autoinc_dict.GetAutoinc();
    dictRefGuard.Release();

    if (nullptr != cde_table->oldName) {
      uint32_t oldNameLen = std::strlen(cde_table->oldName.get()) + 1;
      oldTableName = std::make_unique<char[]>(oldNameLen);
      strcpy_s(oldTableName.get(), oldNameLen, cde_table->oldName.get());
    }
    /* DictSysRemoveTableLocked will release dict sys lock */
    DictSysRemoveTableLocked(name, lockGuard);
    cde_table = nullptr;
  } else
    lockGuard.Clear();

  opWGuard.Clear();

  DBUG_EXECUTE_IF("force_reload_dict_table", {
    if (cde_table != nullptr) {
      dictRefGuard.Release();
      DictSysRemoveTable(name);
      cde_table = nullptr;
    }
  });
  if (cde_table != nullptr && cde_table->refresh_fk &&
      !ddl_info_skip_discardAfterDDL) {
    // todo: if corrupted?
    CDE_ASSERT(!cde_table->m_discardAfterDDL);
    std::deque<const char *> fk_list_names;
    CdeDdTableLoadForeignKey(thd, name, cde_table, tab_def, fk_list_names);
    cde_table->refresh_fk = false;
    cde_table->oldName.reset();
  }

  if (cde_table == nullptr) {
    cde_table = CdeDdOpenTable(thd, name, tab_def, open_table_form);
    if (cde_table == nullptr) {
      CDE_LOG_ERROR("can't open table: %s", name);
      return HA_ERR_NO_SUCH_TABLE;
    }
    if (oldTableName != nullptr &&
        0 != strcmp(oldTableName.get(), cde_table->name.c_str())) {
      cde_table->setOldName(oldTableName.get());
      cde_table->refresh_fk = true;
      std::deque<const char *> fk_list_names;
      CdeDdTableLoadForeignKey(thd, name, cde_table, tab_def, fk_list_names);
      cde_table->refresh_fk = false;
      cde_table->oldName.reset();
    }
  }

  CDE_ASSERT_DEBUG(nullptr == cde_table->oldName);

  m_dstore = new (&m_mem_root) dstore_handler_t;
  m_dstore->dstore_heap_scan = nullptr;
  /* Now the duty of guard belongs to m_dstore. So reset guard here. */
  dictRefGuard.Reset(nullptr);
  m_dstore->table_handler = cde_table;

  if ((rds_use_ddl_info_replay && thd->for_ddl_info_replay) ||
      g_guc.enableStandbyRole) {
    DiscardAfterDDL(cde_table, table);
    CdeSetTableFlagsFromTableShare(cde_table, table_share);
  } else {
    {
      auto relInfo = std::make_unique<session_rel_info>(cde_table->m_id);
      bool ret = CdeCopyRelInfoToLocalStorage(cde_table, relInfo.get());
      if (ret == CDE_FAIL) {
        close();
        return HA_ERR_OUT_OF_MEM;
      }
      /** For now the only option is to use local hander storage for relation.
       *  GlobalCache storage is to be implemented. Then the RelationStorage
       *  we create will depend on system variable.
       */
      m_dstore->rel_info = relInfo.release();
    }

    // full table scan use primary key, do we need this?
    // key_used_on_scan = table->s->primary_key;

    CdeSetTableFlagsFromTableShare(cde_table, table_share);
    CdeDictTableStatsInit(cde_table, thd);
  }
  // todo: handle tmp table later

  // construct handler buffer struct
  // todo: update dict stat first

  // todo: update table stats for optimizer
  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

  // block size in dstore, used by MySQL in query optimization
  stats.block_size = BLCKSZ;

  // Need fetch field num from dstore relation because TABLE_SHARE's fields does
  // not include columns which have been dropped instantly.
  uint32_t num_columns = cde_table->GetTotalCols();
  m_dstore->m_valuesWithVarlen = static_cast<Datum *>(
      m_mem_root.Alloc(num_columns * (sizeof(Datum) + sizeof(CdeVarlena))));
  m_dstore->m_valuesWithoutVarlen = m_mem_root.ArrayAlloc<Datum>(num_columns);
  m_dstore->m_nulls = m_mem_root.ArrayAlloc<bool>(num_columns);
  m_dstore->m_keyInfos = m_mem_root.ArrayAlloc<ScanKeyData>(num_columns);
  m_dstore->m_indexColOids = m_mem_root.ArrayAlloc<Oid>(num_columns);
  m_dstore->m_indexColCollationOids = m_mem_root.ArrayAlloc<Oid>(num_columns);
  m_dstore->m_scanAgainKeyInfos =
      m_mem_root.ArrayAlloc<ScanKeyData>(num_columns);
  m_dstore->m_columnsToDecode =
      m_mem_root.ArrayAlloc<MysqlAndDstoreIndex>(table->s->fields);

  if (table->found_next_number_field != nullptr) {
    if (autoincSaved == 0) {
      autoincSaved = CdeDdTableGetSePrivateDataAutoInc(tab_def);
    }
    if (!(thd->for_ddl_info_replay || g_guc.enableStandbyRole)) {
      session_index_info *autoincIndex =
          cde_index_lookup(table->s->next_number_index);
      if (InitAutoinc(&cde_table->autoinc_dict, table, autoincSaved,
                      autoincIndex->rel,
                      autoincIndex->shared_dict->index_col_num)) {
        close();
        return HA_ERR_AUTOINC_READ_FAILED;
      }
    }
  }
  m_dstore->m_needCheckOnlineDdl = cde_table->IsInOnlineDdl();
  ref_length = sizeof(&m_dstore->dstore_heap_ctid);
  cde_perf_counters::getInstance()->_handler_mem_allocted.increment(
      m_mem_root.allocated_size());
  return 0;
}

/* close handle to a cde table
 */
int ha_cde::close() {
  // m_dstore mem is allocted in m_mem_root, so here we can not
  // call delete, just call destructor to release inner resource
  m_dstore->~dstore_handler_t();

  cde_perf_counters::getInstance()->_handler_mem_allocted.decrement(
      m_mem_root.allocated_size());

  m_mem_root.Clear();
  m_blob_mem_root.Clear();

  return 0;
}

/**
  @brief
    Estimate the average number of records per distinct key value
    for a given key part, based on histogram statistics.

  @param[in]  dict_index      Use the index statistics data to cal rec_per_key.
  @param[in]  table           The TABLE object to which the key belongs.
  @param[in]  key_no          Key no of index.
  @param[in]  i               The column we are calculating rec per key.
  @param[in,out] prefix_records
                              Pointer to the current cumulative record count
  @param[in]  isSampleScan    Whether we should use the index statistics data.

  @return
    Estimated records per key (at least 1). Returns 0 if no histogram
    exists or if the histogram has no distinct value statistics.
*/
uint64_t dstore_rec_per_key(cde_dict_index_t *dict_index, const TABLE *table,
                            uint32_t key_no, uint32_t i,
                            ha_rows *prefix_records, bool isSampleScan) {
  uint64_t rec_per_key = 0;
  const KEY *key = &table->key_info[key_no];
  const KEY_PART_INFO *key_part = &key->key_part[i];

  if (i == (key->actual_key_parts - 1)) {
    /* Each row in the primary key index is unique,
       so the rec_per_key of the last column is set to 1. */
    if (key_no == table->s->primary_key) {
      return 1;
    }
    /* For a unique index where all columns are not null,
       each row is also unique, so the rec_per_key of the last column is set
       to 1. */
    if ((key->flags & (HA_NOSAME | HA_NULL_PART_KEY)) == HA_NOSAME) {
      return 1;
    }
  }

  size_t num_distinct;
  if (!isSampleScan) {
    const histograms::Histogram *histogram =
        table->s->find_histogram(key_part->field->field_index());
    if (histogram == nullptr) {
      return rec_per_key;
    }
    num_distinct = histogram->get_num_distinct_values();
  } else {
    if (dict_index == nullptr || dict_index->stat_n_diff_key_vals == nullptr) {
      return rec_per_key;
    }
    num_distinct = dict_index->stat_n_diff_key_vals[i];
  }

  if (num_distinct == 0) {
    return rec_per_key;
  }

  rec_per_key = (*prefix_records) / num_distinct;
  if (rec_per_key < 1) {
    rec_per_key = 1;
  }
  if (!isSampleScan) {
    *prefix_records = rec_per_key;
  }

  return rec_per_key;
}

int ha_cde::info_impl(uint flag, bool is_analyze) {
  THD *thd = ha_thd();
  cde_session_t *session = CdeCreateOrGetSession(thd);

  cde_dict_t *dict_table = m_dstore->table_handler;

  DBUG_EXECUTE_IF("info_impl_null_dict_table", dict_table = nullptr;);

  if (dict_table == nullptr) {
    return HA_ERR_INTERNAL_ERROR;
  }

  if (is_analyze) {
    /* If it's ANALYZE TABLE, then we register a rw handlerton to
    trigger a 2PC commit in Server layer. */
    CdeTrxStartIfNotStarted(thd, true);
  }

  // need recalc dict stats
  if (flag & HA_STATUS_TIME &&
      !(ha_thd()->for_ddl_info_replay || g_guc.enableStandbyRole)) {
    CDE_HOT_COUNTER_WRAPPER(
        cde_perf_counters::getInstance()->_info_flag_time.increment());

    int ret = CDE_OK;
    if (is_analyze) {
      if (dict_table->dict_stats_persist_enabled()) {
        CdeDictTableStatsUpdate(dict_table, RECALC_PERSIST, ha_thd());
      } else {
        ret = CdeDictTableStatsUpdate(dict_table, RECALC_STATS, ha_thd());
      }
    } else {
      if (dict_table->dict_stats_persist_enabled()) {
        CdeDictStatsFetchFromPs(dict_table, ha_thd());
      } else {
        ret = CdeDictTableStatsUpdate(dict_table, RECALC_STATS, ha_thd());
      }
    }
    if (ret != CDE_OK) {
      return HA_ERR_GENERIC;
    }
    // todo: need track trx modified tables to update time
    stats.update_time = 0;
  }

  // sync stats data from dict to ha_stats, no need to recalc
  if (flag & HA_STATUS_VARIABLE) {
    CDE_HOT_COUNTER_WRAPPER(
        cde_perf_counters::getInstance()->_info_flag_variable.increment());
    if (!(flag & HA_STATUS_NO_LOCK)) {
      dict_table->dict_stat_read_lock();
    }
    uint64_t heap_pages =
        dict_table->stat_heap_page_count + dict_table->stat_heap_lob_page_count;
    uint64_t index_pages = dict_table->stat_all_indexs_page_count;
    uint64_t n_rows = dict_table->stat_n_rows;
    uint64_t free_pages = dict_table->stat_heap_free_page_count;
    if (!(flag & HA_STATUS_NO_LOCK)) {
      dict_table->dict_stat_unlock();
    }

    /*
    The MySQL optimizer seems to assume in a left join that n_rows
    is an accurate estimate if it is zero. Of course, it is not,
    since we do not have any locks on the rows yet at this phase.
    Since SHOW TABLE STATUS seems to call this function with the
    HA_STATUS_TIME flag set, while the left join optimizer does not
    set that flag, we add one to a zero value if the flag is not
    set. That way SHOW TABLE STATUS will show the best estimate,
    while the optimizer never sees the table empty.
    However, if it is internal temporary table used by optimizer,
    the count should be accurate.
    */
    if (n_rows == 0 && !(flag & HA_STATUS_TIME) &&
        table_share->table_category != TABLE_CATEGORY_TEMPORARY) {
      n_rows++;
    }
    stats.records = n_rows;
    stats.data_file_length = heap_pages * BLCKSZ;
    stats.index_file_length = index_pages * BLCKSZ;
    stats.delete_length = free_pages * BLCKSZ;
    if (stats.records == 0) {
      stats.mean_rec_length = 0;
    } else {
      stats.mean_rec_length = stats.data_file_length / stats.records;
    }

    stats.check_time = 0;
    stats.deleted = 0;
  }

  // handle index mem estimate in table_share if HA_STATUS_NO_LOCK
  if (!(flag & HA_STATUS_NO_LOCK)) {
    dict_table->dict_stat_read_lock();
  }

  if (flag & HA_STATUS_CONST) {
    CDE_HOT_COUNTER_WRAPPER(
        cde_perf_counters::getInstance()->_info_flag_const.increment());
    ref_length = sizeof(ItemPointerData);
    stats.mrr_length_per_rec = ref_length + sizeof(void *);
    bool isSampleScan = dstore_enable_index_sample;

    Histogram_rdlock_guard s_histo_guard(&table->s->LOCK_m_histograms);
    for (uint i = 0; i < table->s->keys; i++) {
      KEY *key = &table->key_info[i];
      ha_rows prefix_records = stats.records;
      cde_dict_index_t *index = dict_table->get_index_on_name(key->name);
      if (index == nullptr && isSampleScan) {
        CDE_LOG_ERROR(
            "empty index dict when cal rec per key, table : %s index %s.",
            dict_table->name.c_str(), key->name);
        continue;
      }
      for (uint j = 0; j < key->actual_key_parts; ++j) {
        key->rec_per_key[j] = dstore_rec_per_key(index, table, i, j,
                                                 &prefix_records, isSampleScan);
      }
    }

    // todo: not used in CBO
    stats.create_time = 0;
    stats.max_data_file_length = 0;
    stats.max_index_file_length = 0;
  }

  if (!(flag & HA_STATUS_NO_LOCK)) {
    dict_table->dict_stat_unlock();
  }

  if (flag & HA_STATUS_ERRKEY) {
    const cde_dict_index_t *err_dict_index = session->get_trxinfo()->err_index;
    if (err_dict_index) {
      errkey = cde_index_lookup(err_dict_index->name.c_str());
    } else {
      errkey = (~0);
    }
  }

  // handle auto inc
  if ((flag & HA_STATUS_AUTO) && table->found_next_number_field &&
      !(ha_thd()->for_ddl_info_replay || g_guc.enableStandbyRole)) {
    CDE_HOT_COUNTER_WRAPPER(
        cde_perf_counters::getInstance()->_info_flag_auto.increment());
    DictAutoinc *autoinc_dict = &dict_table->autoinc_dict;
    uint64_t autoinc;
    autoinc_dict->MutexEnter();
    autoinc = autoinc_dict->GetAutoinc();
    CDE_ASSERT(autoinc > 0);
    autoinc_dict->MutexExit();
    stats.auto_increment_value = autoinc;
  }

  return CDE_OK;
}

/*

*/
int ha_cde::info(uint flag) {
  Huawei::Common::LatencyCounter::Timer info_latency(
      cde_perf_counters::getInstance()->_cde_info_latency,
      dstore_enable_perf_timers);
  return info_impl(flag, false);
}

/** Returns the exact number of records that this client can see using this
 * handler object. */
int ha_cde::records(ha_rows *num_rows) /*!< out: number of rows */
{
  if (m_dstore->select_lock_type != LOCK_NONE) {
    return handler::records(num_rows);
  }

  ulong thread_num = thd_parallel_read_threads(current_thd);
  if (thread_num > 0) {
    /* Reset thread number to 1 in the following cases:
      1. Temporary table: ThreadContext's TempLocalBuffer is private and
         cannot be shared among parallel workers.
      2. Small table: The table size is too small to benefit from parallel
         scanning (data block count / extent size <= 1). */
    if (thread_num != 1 && (m_dstore->table_handler->is_temporary() ||
                            dynamic_cast<DSTORE::DataSegment *>(
                                this->m_dstore->rel_info->rd_storage_releation
                                    ->GetTableSmgrSegment())
                                        ->GetDataBlockCount() /
                                    DSTORE::EXTENT_SIZE_ARRAY[0] <=
                                1)) {
      thread_num = 1;
    }
    struct alignas(CDE_CACHE_LINE_SIZE) AlignedCounter {
      uint64_t count;
      char padding[CDE_CACHE_LINE_SIZE - sizeof(uint64_t)];

      AlignedCounter() : count(0) {}
    };

    struct CountContext {
      std::vector<AlignedCounter> thread_counts;
      uint16_t num_threads;

      explicit CountContext(uint16_t n) : num_threads(n) {
        thread_counts.resize(n);
      }

      uint64_t get_total_count() const {
        uint64_t total = 0;
        for (uint16_t i = 0; i < num_threads; ++i) {
          total += thread_counts[i].count;
        }
#ifndef NDEBUG
        if (total > 0) {
          CDE_LOG_INFO("[Parallel reader] Total rows scanned: %" PRIu64, total);
          for (uint16_t i = 0; i < num_threads; ++i) {
            uint64_t thread_count = thread_counts[i].count;
            double percentage =
                (static_cast<double>(thread_count) / total) * 100.0;
            CDE_LOG_INFO("[Parallel reader] thread %-3d scan rows %10" PRIu64
                         " (%.2f%%).",
                         i, thread_count, percentage);
          }
        } else {
          CDE_LOG_INFO("[Parallel reader] Total rows scanned: 0");
        }
#endif
        return total;
      }
    };
    CountContext count_ctx(thread_num);
    auto lambda_callback = [](DSTORE::HeapTuple *tuple, uint16_t threadIdx,
                              void *ctx) -> int {
      if (tuple == nullptr) {
        return CDE_ERROR;
      }

      CountContext *count_ctx = static_cast<CountContext *>(ctx);
      count_ctx->thread_counts[threadIdx].count++;

      return CDE_OK;
    };

    cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
    updateStatementSnapShot(m_dstore->select_lock_type, trx, true);

    ParallelHeapScanReader reader(
        this->m_dstore->rel_info->rd_storage_releation, thread_num,
        std::move(lambda_callback), &count_ctx);
    if (reader.Init() || reader.Run()) {
      if (reader.HasFatalError()) {
        *num_rows = HA_POS_ERROR;
        return HA_ERR_INTERNAL_ERROR;
      }

      if (table->in_use->is_killed()) {
        *num_rows = HA_POS_ERROR;
        return HA_ERR_QUERY_INTERRUPTED;
      }

      CDE_LOG_WARN(
          "Parallel record counting failed. Degrading to single-threaded "
          "mode.");
    } else {
      *num_rows = count_ctx.get_total_count();
      return 0;
    }
  }

  return handler::records(num_rows);
}

/**
Clone this handler, used when needing more than one cursor to the same table.

@param[in]      name            Table name.
@param[in]      mem_root        mem_root to allocate from.

@return Pointer to clone or NULL if error.
*/
handler *ha_cde::clone(const char *name, MEM_ROOT *mem_root) {
  DBUG_TRACE;
  ha_cde *new_handler = dynamic_cast<ha_cde *>(handler::clone(name, mem_root));
  if (new_handler != nullptr) {
    CDE_ASSERT(new_handler->m_dstore != nullptr);
    new_handler->m_dstore->select_lock_type = m_dstore->select_lock_type;
    new_handler->m_pushed_offset = m_pushed_offset;
    new_handler->m_scanned_offset = m_scanned_offset;
    new_handler->m_pushed_aggregate = m_pushed_aggregate;
  }
  return new_handler;
}

/** See comment in handler.cc */
bool ha_cde::get_error_message(int error, String *buf) {
  (void)error;
  cde_trxinfo_t *trxinfo = CdeGetTrxinfo(ha_thd());
  assert(trxinfo != nullptr);
  buf->takeover(trxinfo->err_msg);
  return false;
}

bool ha_cde::get_foreign_dup_key(char *child_table_name,
                                 uint child_table_name_len,
                                 char *child_key_name,
                                 uint child_key_name_len) {
  cde_trxinfo_t *trxinfo = CdeGetTrxinfo(ha_thd());
  const cde_dict_index_t *err_index = trxinfo->err_index;
  if (!err_index) {
    return false;
  }

  uint32_t tableNameSize = 0;
  const char *p = GetTableName(err_index->table->name.c_str(), tableNameSize);
  if (0 == tableNameSize || nullptr == p) {
    CDE_LOG_ERROR("get_foreign_dup_key failed to get table name.");
    return false;
  }

  size_t len;
  len = filename_to_tablename(p, child_table_name, child_table_name_len);
  child_table_name[len] = '\0';

  /* copy index name */
  snprintf_s(child_key_name, child_key_name_len, child_key_name_len - 1, "%s",
             err_index->name.c_str());
  return true;
}

/** How many seeks it will take to read through the table. This is to be
 comparable to the number returned by records_in_range so that we can
 decide if we should scan the table or use keys.
 @return estimated time measured in disk seeks */

double ha_cde::scan_time() {
  /* For common types of disks, such as HDD and SSD, scan time(I/O latency)
  is independent of page size and only depends on the number of scan pages,
  refer to the interface "ha_innobase::scan_time", we use the number of pages
  scanned as the cost for this scan time. Note: Dstore page size is
  BLCKSZ(8192). For empty table, Dstore heap page number is 0, keep same with
  InnoDB, optimizer assume heap table must contain at least one page. For
  non-empty table, Dstore initially allocate 8 pages, with 5 pages used as
  data pages and 3 pages used as management pages. */
  double page_num =
      std::max(ulonglong2double(stats.data_file_length) / BLCKSZ, 1.0);
  return page_num;
}

/** Calculate the time it takes to read a set of ranges through an index
 This enables us to optimise reads for clustered indexes.
 @return estimated time measured in disk seeks */

double ha_cde::read_time(
    uint index,   /*!< in: key number */
    uint ranges,  /*!< in: how many ranges */
    ha_rows rows) /*!< in: estimated number of rows in the ranges */
{
  return handler::read_time(index, ranges, rows);
}

/** Return the size of the Dstore memory buffer. */
longlong ha_cde::get_memory_buffer_size() const {
  CDE_ASSERT_DEBUG(g_guc.buffer > 0);
  /* Dstore buffer pool size in bytes */
  return (longlong)g_guc.buffer * BLCKSZ;
}

/** Gives an UPPER BOUND to the number of rows in a table. This is used in
 filesort.cc.
 @return upper bound of rows */
ha_rows ha_cde::estimate_rows_upper_bound() {
  return handler::estimate_rows_upper_bound();
}

enum row_type ha_cde::get_real_row_type(
    const HA_CREATE_INFO *create_info) const {
  /* todo: handle temp table.
   * see: ha_innobase::get_real_row_type
   */
  row_type rt = create_info->row_type;
  switch (rt) {
    case ROW_TYPE_REDUNDANT:
    case ROW_TYPE_COMPACT:
      return (rt);
    case ROW_TYPE_DYNAMIC:
    case ROW_TYPE_COMPRESSED:
    case ROW_TYPE_NOT_USED:
    case ROW_TYPE_FIXED:
    case ROW_TYPE_PAGED:
    case ROW_TYPE_DEFAULT:
    default:
      if (cde_default_row_format == ROW_TYPE_COMPACT) {
        return ROW_TYPE_COMPACT;
      }
      return ROW_TYPE_REDUNDANT;
  }
}

bool ha_cde::check_if_incompatible_data(HA_CREATE_INFO *info,
                                        uint table_changes) {
  if (table_changes != IS_EQUAL_YES) {
    return (COMPATIBLE_DATA_NO);
  }

  /* Check that auto_increment value was not changed */
  if ((info->used_fields & HA_CREATE_USED_AUTO) &&
      info->auto_increment_value != 0) {
    return (COMPATIBLE_DATA_NO);
  }

  /* Check that row format didn't change */
  if ((info->used_fields & HA_CREATE_USED_ROW_FORMAT) &&
      info->row_type != table->s->real_row_type) {
    return (COMPATIBLE_DATA_NO);
  }

  /* Specifying KEY_BLOCK_SIZE requests a rebuild of the table. */
  if (info->used_fields & HA_CREATE_USED_KEY_BLOCK_SIZE) {
    return (COMPATIBLE_DATA_NO);
  }

  return (COMPATIBLE_DATA_YES);
}

/** Reads the next row from a cursor, which must have previously been
 positioned using index_read.
 @return 0, HA_ERR_END_OF_FILE, or error number */

int ha_cde::index_next(uchar *buf) /*!< in/out: buffer for next row in MySQL
                                      format */
{
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  cde_isolation_level_t isolationLevel =
      CdeTrxMapIsolationLevel(ha_thd()->tx_isolation);
  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx);

  int ret = CDE_OK;
  if (m_dstore->read_just_key == 0) {
    // Go back to heap scenario
    ret = cde_dml_api::ReadFullRow(
        isolationLevel, buf, table, this, m_dstore, &m_blob_mem_root,
        m_allowBlobMemRootClearForReuse, &trx->scanSnapshot);
  } else {
    // Cover index scenario
    ret = cde_dml_api::ReadIndexOnly(isolationLevel, buf, table, this, m_dstore,
                                     &trx->scanSnapshot);
  }
  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */
  if (HA_ERR_KEY_NOT_FOUND == ret) {
    ret = HA_ERR_END_OF_FILE;
  }
  return ConvertErrcodeToMysql(ret, 0, ha_thd());
}

/** Reads the previous row from a cursor, which must have previously been
 positioned using index_read.
 @return 0, HA_ERR_END_OF_FILE, or error number */

int ha_cde::index_prev(
    uchar *buf) /*!< in/out: buffer for previous row in MySQL format */
{
  DBUG_TRACE;
  return ha_index_next(buf);
}

/** Positions a cursor on the first record in an index and reads the
 corresponding row to buf.
 @return 0, HA_ERR_END_OF_FILE, or error code */
int ha_cde::index_first(uchar *buf) /*!< in/out: buffer for the row */
{
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_first_count);
  int err = index_read(buf, nullptr, 0, HA_READ_AFTER_KEY);
  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */
  if (err == HA_ERR_KEY_NOT_FOUND) {
    return HA_ERR_END_OF_FILE;
  }
  return err;
}

/** Positions a cursor on the last record in an index and reads the
 corresponding row to buf.
 @return 0, HA_ERR_END_OF_FILE, or error code */
int ha_cde::index_last(uchar *buf) /*!< in/out: buffer for the row */
{
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_last_count);
  int err = index_read(buf, nullptr, 0, HA_READ_BEFORE_KEY);
  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */
  if (err == HA_ERR_KEY_NOT_FOUND) {
    return HA_ERR_END_OF_FILE;
  }
  return err;
}

/** Reads the next row matching to the key value given as the parameter.
 @return 0, HA_ERR_END_OF_FILE, or error number */

int ha_cde::index_next_same(uchar *buf,       /*!< in/out: buffer for the row */
                            const uchar *key, /*!< in: key value */
                            uint keylen)      /*!< in: key value length */
{
  DBUG_TRACE;
  (void)buf;
  (void)key;
  (void)keylen;
  return ha_cde::index_next(buf);
}

int ha_cde::read_range_first(const key_range *start_key,
                             const key_range *end_key, bool eq_range_arg,
                             bool sorted) {
  if (start_key) {
    m_dstore->key_map = start_key->keypart_map;
    m_dstore->keys_prefix_counts = CdeKeypartMapToN(
        start_key->keypart_map,
        m_dstore->current_cde_index->shared_dict->index_col_num);
  } else {
    m_dstore->key_map = 0;
    m_dstore->keys_prefix_counts = 0;
  }
  return handler::read_range_first(start_key, end_key, eq_range_arg, sorted);
}

int ha_cde::read_range_next() {
  int result = handler::read_range_next();
  return ConvertErrcodeToMysql(result, 0, ha_thd());
}
/** Reads the next row in a table scan (also used to read the FIRST row
 in a table scan).
 @return 0, HA_ERR_END_OF_FILE, or error number */

int ha_cde::rnd_next(
    uchar *buf) /*!< in/out: returns the row in this buffer, in MySQL format */
{
  uint32_t error = 0;

  DBUG_TRACE;

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx);

  /* Fields from TABLE_SHARE do not include the columns which have been
  dropped instantly, so we need get total fields from dict table, which is equal
  the value in tupledesc. */
  DSTORE::TupleDesc tupleDesc = m_dstore->rel_info->rd_storage_releation->attr;
  DSTORE::HeapTuple *tuple = nullptr;
  DSTORE::HeapTuple *tupleWithLob = nullptr;
  while (true) {
    tuple = HeapInterface::SeqScan(m_dstore->dstore_heap_scan);
    if (nullptr == tuple) {
      error = GetAndConvertDstoreErrcodeToMysql();
      if (CdeEnableDamagePageManager() &&
          error == (uint32_t)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE) {
        CDE_LOG_ERROR("swat hit damage page when fetch tuple in seq scan");
        if (CdeReportErrorOnDamagePage()) {
          goto func_exit;
        }
      }
      error = HA_ERR_END_OF_FILE;
      goto func_exit;
    }

    // select ... from my_table lock in share mode/for update (without where
    // condition)
    int lock_table_ret;
    if (m_dstore->sql_stat_start && m_dstore->select_lock_type != LOCK_NONE &&
        !m_dstore->table_handler->is_temporary()) {
      lock_table_ret =
          CdeLockTable(m_dstore->select_lock_type == LOCK_S ? LOCK_IS : LOCK_IX,
                       m_dstore->rel_info->rd_storage_releation);
      if (lock_table_ret != DSTORE::DSTORE_SUCC) {
        CDE_LOG_ERROR("lock table failed");
        error = HA_ERR_LOCK_TABLE_FULL;
        goto func_exit;
      }
      m_dstore->sql_stat_start = false;
    }

    if (m_dstore->select_lock_type != LOCK_NONE &&
        !m_dstore->table_handler->is_temporary()) {
      cde_isolation_level_t isolationLevel =
          CdeTrxMapIsolationLevel(ha_thd()->tx_isolation);
      bool needWait = m_dstore->select_mode == SELECT_ORDINARY ? true : false;

      error = CdeLockTuple(isolationLevel, &trx->scanSnapshot,
                           m_dstore->rel_info->rd_storage_releation,
                           tuple->GetCtid(), &tuple, needWait);

      if (error != CDE_OK) {
        if (error ==
                static_cast<uint32_t>(HaDstoreErrE::HA_DSTORE_ERR_SKIP_WAIT) &&
            m_dstore->select_mode == SELECT_SKIP_LOCKED) {
          continue;
        }
        goto func_exit;
      }
    }

    if (skip_row_for_offset_pushdown()) {
      increment_scanned_offset();
      if (m_dstore->select_lock_type != LOCK_NONE && tuple) {
        if (!m_dstore->table_handler->is_temporary()) {
          TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        }
        tuple = nullptr;
      }
      continue;
    }

    if (aggregate_unqualified_count()) {
      if (m_dstore->select_lock_type != LOCK_NONE && tuple) {
        if (!m_dstore->table_handler->is_temporary()) {
          TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        }
        tuple = nullptr;
      }
      return CDE_OK;
    }
    DSTORE::HeapTuple *realTuple = tuple;
    if (tupleDesc->tdhaslob && m_dstore->m_needDecodeLobColumns &&
        tuple != nullptr) {
      tupleWithLob = FetchBlobFiled(m_dstore->rel_info->rd_storage_releation,
                                    tuple, &trx->scanSnapshot,
                                    m_dstore->select_lock_type == LOCK_NONE);

      DBUG_EXECUTE_IF(
          "rnd_next_lobtuple_pointer_injection",
          TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
          tupleWithLob = nullptr;);
      if (unlikely(nullptr == tupleWithLob)) {
        error = GetAndConvertDstoreErrcodeToMysql();
        CDE_LOG_ERROR(
            "TupleWithLob is nullptr in rnd_next function, error is %d.",
            error);
        if (CdeEnableDamagePageManager() &&
            (uint32_t)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE ==
                error) {
          if (CdeReportErrorOnDamagePage()) {
            CDE_LOG_ERROR(
                "swat hit damage page when fetch blob filed in seq scan");
            goto func_exit;
          }
          CDE_LOG_ERROR(
              "swat hit damage page when fetch blob filed, need to continue to "
              "seq scan");
          error = 0;
          continue;
        }
        goto func_exit;
      }
      if (tuple != tupleWithLob) {
        realTuple = tupleWithLob;
      } else {
        tupleWithLob = nullptr;
      }
    }
    if (0 == error) {
      m_dstore->dstore_heap_ctid = *realTuple->GetCtid();
      m_dstore->DecodeDstoreRow(tupleDesc, m_dstore->m_valuesWithVarlen,
                                realTuple, buf, table, &m_blob_mem_root);
      if (aggregate_rows_by_group()) {
        if (tupleDesc->tdhaslob && tupleWithLob != nullptr) {
          TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
        }
        if (m_dstore->select_lock_type != LOCK_NONE && (0 == error) && tuple &&
            !m_dstore->table_handler->is_temporary()) {
          TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
        }
        tuple = nullptr;
        tupleWithLob = nullptr;
        realTuple = nullptr;
        continue;
      }
      break;
    } else if (error != HA_ERR_RECORD_DELETED) {
      break;
      // TODO: after improve implement of cde_lock_tuple, the process of
      // realTuple nullptr should be adjusted too.
    } else if (realTuple == nullptr) {
      CDE_LOG_ERROR("lock table failed, realTuple is null");
      break;
    }
    m_dstore->dstore_heap_ctid = *realTuple->GetCtid();
    m_dstore->DecodeDstoreRow(tupleDesc, m_dstore->m_valuesWithVarlen,
                              realTuple, buf, table, &m_blob_mem_root);
    break;
  }

func_exit:
  if (tupleWithLob != nullptr) {
    TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
  }

  /*
   * SeqScan() will copy page to local buffer which m_resTuple points to,
   * so do not need to free space for m_resTuple.
   * but while select_lock_type NOT LOCK_NOE, it will call LockUnchangedTuple
   * cause our needRetTup set true and memory will allocated in
   * LockUnchangedTuple->DoLock->CopyTuple, need free by ourself asap
   * temporary table not call LockUnchangedTuple, so can't free
   */
  if (m_dstore->select_lock_type != LOCK_NONE && (0 == error) && tuple &&
      !m_dstore->table_handler->is_temporary()) {
    TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
  }

  return ConvertErrcodeToMysql(error, 0, ha_thd());
}

/** Fetches a row from the table based on a row reference.
 @return 0, HA_ERR_KEY_NOT_FOUND, or error code */

int ha_cde::rnd_pos(uchar *buf, /*!< in/out: buffer for the row */
                    uchar *pos) /*!< in: primary key value of the row in the
                                MySQL format, or the row id if the clustered
                                index was internally generated by CDE; the
                                length of data in pos has to be ref_length */
{
  DBUG_TRACE;
  DBUG_DUMP("key", pos, ref_length);

  ha_statistic_increment(&System_status_var::ha_read_rnd_count);
  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx);
  DSTORE::TupleDesc tupleDesc = m_dstore->rel_info->rd_storage_releation->attr;
  HeapTuple *tuple = nullptr;
  int error = 0;

  DSTORE::HeapTuple *tupleWithLob = nullptr;
  auto *ctid = reinterpret_cast<DSTORE::ItemPointerData *>(pos);

  if (m_dstore->select_lock_type != LOCK_NONE &&
      !m_dstore->table_handler->is_temporary()) {
    if (m_dstore->sql_stat_start) {
      if (CdeLockTable(m_dstore->select_lock_type == LOCK_S ? LOCK_IS : LOCK_IX,
                       m_dstore->rel_info->rd_storage_releation) !=
          DSTORE::DSTORE_SUCC) {
        CDE_LOG_ERROR("lock table failed");
        return HA_ERR_LOCK_TABLE_FULL;
      }
    }
    m_dstore->sql_stat_start = false;
    cde_isolation_level_t isolationLevel =
        CdeTrxMapIsolationLevel(ha_thd()->tx_isolation);
    error =
        CdeLockTuple(isolationLevel, &trx->scanSnapshot,
                     m_dstore->rel_info->rd_storage_releation, ctid, &tuple);
    DBUG_EXECUTE_IF("cde_lock_tuple_injection", error = CDE_ERROR;);
    if (error != 0 || tuple == nullptr) {
      CDE_LOG_ERROR("CdeLockTuple failed, errorcode=%d", error);
      TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
      return error;
    }
  } else {
    /* in some case, executor will invoke rnd_pos after rnd_end,
    while fetch tuple, dstore_heap_scan has been destroyed, so recreate it */
    DBUG_EXECUTE_IF(
        "rnd_pos_heap_scan_nullptr",
        HeapInterface::EndScan(m_dstore->dstore_heap_scan);
        HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
        m_dstore->dstore_heap_scan = nullptr;);
    if (!m_dstore->dstore_heap_scan) {
      m_dstore->dstore_heap_scan = HeapInterface::CreateHeapScanHandler(
          m_dstore->rel_info->rd_storage_releation);
      DBUG_ASSERT(m_dstore->dstore_heap_scan != nullptr);
      trx->appendHeapScanHandler((&m_dstore->dstore_heap_scan));
      HeapInterface::BeginScan(m_dstore->dstore_heap_scan, &trx->scanSnapshot);
    }

    /** FetchTuple() will create space and copy tuple to resTuple, so need to
    call TupleInterface::DestroyTuple(resTuple) to free the memory. */
    tuple = HeapInterface::FetchTuple(m_dstore->dstore_heap_scan, *ctid);
    DBUG_EXECUTE_IF("cde_fetch_tuple_injection", tuple = nullptr;);
    if (tuple == nullptr) {
      CDE_LOG_ERROR("FetchTuple fail");
      return GetAndConvertDstoreErrcodeToMysql();
    }
  }

  m_dstore->dstore_heap_ctid = *tuple->GetCtid();
  HeapTuple *realTuple = tuple;
  if (tupleDesc->tdhaslob && m_dstore->m_needDecodeLobColumns) {
    tupleWithLob = FetchBlobFiled(m_dstore->rel_info->rd_storage_releation,
                                  tuple, &trx->scanSnapshot,
                                  m_dstore->select_lock_type == LOCK_NONE);

    DBUG_EXECUTE_IF(
        "rnd_pos_null_pointer_injection",
        TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
        tupleWithLob = nullptr;);
    if (unlikely(!tupleWithLob)) {
      uint32_t error_code = GetAndConvertDstoreErrcodeToMysql();
      TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
      CDE_LOG_ERROR("TupleWithLob is nullptr in rnd_pos function, error is %d.",
                    error_code);
      return error_code;
    }
    if (tuple != tupleWithLob) {
      realTuple = tupleWithLob;
    } else {
      tupleWithLob = nullptr;
    }
  }
  m_dstore->DecodeDstoreRow(tupleDesc, m_dstore->m_valuesWithoutVarlen,
                            realTuple, buf, table, &m_blob_mem_root);
  if (tupleWithLob != nullptr) {
    TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
  }
  TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
  return 0;
}

int CdeGetScanCount(DSTORE::StorageRelation indexRel, ScanKey key_infos,
                    int numKeys, ScanDirection direct) {
  DSTORE::IndexScanHandler *index_scan_handler =
      IndexInterface::ScanBegin(indexRel, indexRel->index, numKeys, 0);
  DSTORE::SnapshotData snapshot;
  CDESetSnapshotByTrans(snapshot);
  IndexInterface::IndexScanSetSnapshot(index_scan_handler, &snapshot);
  IndexInterface::ScanSetWantItup(index_scan_handler, true);

  if (numKeys > 0) {
    IndexInterface::ScanRescan(index_scan_handler, key_infos);
  }
  DSTORE::IndexTuple *itup;
  DSTORE::TupleDesc td;
  ha_rows count = 0;
  bool recheck;

  while ((itup = IndexInterface::OnlyScanNext(index_scan_handler, direct, &td,
                                              &recheck)) != nullptr) {
    if (itup == nullptr) {
      IndexInterface::ScanEnd(index_scan_handler);
      return 0;
    }

    ItemPointerData ctid = itup->GetHeapCtid();

    if (ctid == INVALID_ITEM_POINTER) {
      IndexInterface::ScanEnd(index_scan_handler);
      return 0;
    }
    /*
        std::vector<Datum> values(indexRel->attr->natts);
        std::vector<char> isNulls(indexRel->attr->natts);
        TupleInterface::DeformIndexTuple(itup, td, values.data(), (bool *)
       isNulls.data());
    */
    count++;
  }
  IndexInterface::ScanEnd(index_scan_handler);
  return count;
}

/*
    check if a start key needs to scan again
    ie:

    An ascend index, scan begins at 3(not null), and scan forward.
    [NULL - NULL - 1 - 2 - 3 - 4 - 5]
                          (3)------->  in mysql only need to scan not null, so
   do not need to scan again.

    An ascend index, scan begins at null, and scan forward.
    [NULL - NULL - 1 - 2 - 3 - 4 - 5]
    (NULL)-------------------------->  in mysql need to scan both null and not
   null, so need to scan again.

    A descend index, scan begins at null, and scan backward.
    [5 - 4 - 3 - 2 - 1 - NULL - NULL]
    <--------------------------(NULL)  in mysql need to scan both null and not
   null, so need to scan again.

   TODO if the index column include not null constraint, no need to access
   null data, so we can consider optimize this scenario to avoid do useless
   rescan in the future.
*/
static bool CdeNeedScanAgainStart(bool is_asc, ScanKey key_info,
                                  ScanDirection dir,
                                  ha_rkey_function flag = HA_READ_AFTER_KEY) {
  switch (flag) {
    /* for ref or eq_ref operator, no need to do rescan */
    case HA_READ_KEY_EXACT:
    /* search using a key prefix which must match rows,
    no need to do rescan */
    case HA_READ_PREFIX_LAST:
      return false;
    default:
      break;
  }

  if (!(key_info->skFlags & DSTORE::SCAN_KEY_ISNULL)) {
    return is_asc ? dir == DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION
                  : dir == DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
  }

  if (dir == DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION) {
    return is_asc ? key_info->skStrategy == CDE_SCAN_ORDER_GREATEREQUAL : false;
  } else {
    return is_asc ? false : key_info->skStrategy == CDE_SCAN_ORDER_GREATEREQUAL;
  }
}

/*
    check if an end key needs to scan again
    ie:

    An ascend index, scan end at 3(not null), and scan backward.
    [NULL - NULL - 1 - 2 - 3 - 4 - 5]
                          (3)<-------  in mysql only need to scan not null, so
   do not need to scan again.

    An ascend index, scan end at null, and scan backward.
    [NULL - NULL - 1 - 2 - 3 - 4 - 5]
           (NULL)<-------------------  in mysql need to scan both null and not
   null, so need to scan again.

    A descend index, scankey argument is null, and scan forward.
    [5 - 4 - 3 - 2 - 1 - NULL - NULL]
    ------------------->(NULL)         in mysql need to scan both null and not
   null, so need to scan again.
*/
bool CdeNeedScanAgainEnd(bool is_asc, ScanKey key_info, ScanDirection dir) {
  if (!(key_info->skFlags & SCAN_KEY_ISNULL)) {
    return is_asc ? dir == ScanDirection::FORWARD_SCAN_DIRECTION
                  : dir == ScanDirection::BACKWARD_SCAN_DIRECTION;
  }

  if (dir == ScanDirection::FORWARD_SCAN_DIRECTION) {
    return is_asc ? false : key_info->skStrategy == CDE_SCAN_ORDER_GREATEREQUAL;
  } else {
    return is_asc ? key_info->skStrategy == CDE_SCAN_ORDER_GREATEREQUAL : false;
  }
}

/* return value used by records_in_range_by_histogram() @start { */
// Records in range estimate failure due to histogram access error.
constexpr int64_t K_REC_IN_RANGE_HISTOGRAM_ACCESS_FAILED = -1;
// Records in range estimate failure due to Out of memory.
constexpr int64_t K_REC_IN_RANGE_OOM = -2;
// Records in range estimate not support scenario.
constexpr int64_t K_REC_IN_RANGE_NOT_SUPPORT = -3;
/* } @end */

/**
 Get the key_part_nr-th key buffer start.

 @param[in] key used index
 @param[in] key_buffer non-const buffer copy from key_range::key
 @param[in] key_part_nr which part of key that we want to access
 @return start position for the requested key part.
 */
static uchar *CdeGetNthKeyPartPtrInKeyBuffer(const KEY *key, uchar *key_buffer,
                                             const uint8_t key_part_nr) {
  CDE_ASSERT_DEBUG(key_part_nr < key->user_defined_key_parts);
  CDE_ASSERT_DEBUG(key != nullptr);

  uint32_t off = 0;
  for (uint8_t i = 0; i < key_part_nr; ++i) {
    off += key->key_part[i].store_length;
  }
  return key_buffer + off;
}

/**
 Convert a key part value to Item field.

 @param[in,out] mem_root memory allocator for new Item_field
 @param[in] key used index
 @param[in] key_buffer non-const buffer copy from key_range::key.
 @param[in] key_part_nr which key part in this index we used
 @param[out] is_null whether has a NULL flag for this key part.
 @retval non-null pointer new Field object construted from key_value.
 @retval nullptr OOM or type cast error occur, or if the field value
 is NULL(which is_null will alse be set.)
 */
static Item *CdeConvertKeyRangePartToItem(MEM_ROOT *mem_root, const KEY *key,
                                          uchar *key_buffer,
                                          const uint8_t key_part_nr,
                                          bool &is_null) {
  // Get the Field obj of the key part.
  const Field *field = key->key_part[key_part_nr].field;
  auto ptr = CdeGetNthKeyPartPtrInKeyBuffer(key, key_buffer, key_part_nr);
  // Prepare basic field info for creating new Field.
  auto data_ptr = ptr;
  if (field->is_nullable()) {
    /* If the field is not defined as NOT NULL, then the value buffer
    use 1 byte to flag whether is real NULL. The key part value will
    not exist if NULL flag is 1(i.e. *data_ptr != 0), otherwise we
    will read the key's value after the flag byte(i.e. data_ptr++). */
    if (*data_ptr != 0) {
      is_null = true;
      return nullptr;
    }
    data_ptr++;
  }
  is_null = false;

  Field *clone_field = field->clone(mem_root);
  DBUG_EXECUTE_IF("histogram_stat_clone_field_failed", clone_field = nullptr;);
  if (clone_field == nullptr) {
    return nullptr;
  }
  clone_field->set_field_ptr(data_ptr);
  clone_field->set_null_ptr(ptr, clone_field->null_bit);
  clone_field->set_notnull((ptrdiff_t)0);

  Item *result = nullptr;
  switch (field->real_type()) {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      my_decimal buf;
      result = new (mem_root) Item_decimal(clone_field->val_decimal(&buf));
      break;
    }
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_BOOL:
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_BIT: {
      result = new (mem_root) Item_int(clone_field->val_int());
      break;
    }
    case MYSQL_TYPE_LONGLONG: {
      if (clone_field->is_unsigned()) {
        result = new (mem_root) Item_uint(ulonglong(clone_field->val_int()));
      } else {
        result = new (mem_root) Item_int(clone_field->val_int());
      }
      break;
    }
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      result =
          new (mem_root) Item_float(clone_field->val_real(), DECIMAL_MAX_SCALE);
      break;
    }
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2: {
      MYSQL_TIME buf;
      TIME_from_longlong_time_packed(&buf, clone_field->val_time_temporal());
      result = new (mem_root) Item_time_literal(&buf, DATETIME_MAX_DECIMALS);
      break;
    }
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE: {
      MYSQL_TIME buf;
      TIME_from_longlong_date_packed(&buf, clone_field->val_date_temporal());
      result = new (mem_root) Item_date_literal(&buf);
      break;
    }
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2: {
      MYSQL_TIME buf;
      TIME_from_longlong_datetime_packed(&buf,
                                         clone_field->val_date_temporal());
      /* MySQL will only apply time zone for TIMESTAMP, so we can
      ignore time zone info bellow. */
      result = new (mem_root)
          Item_datetime_literal(&buf, DATETIME_MAX_DECIMALS, nullptr);
      break;
    }
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      MYSQL_TIME buf;
      TIME_from_longlong_datetime_packed(
          &buf, clone_field->val_date_temporal_at_utc());
      result = new (mem_root)
          Item_datetime_literal(&buf, DATETIME_MAX_DECIMALS, my_tz_UTC);
      break;
    }
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR: {
      /* Key field always use 2 bytes to store length in the header,
      so we need use (field_ptr + 2) to skip it. @see comments from
      Field_varstring::new_key_field(). */
      result = new (mem_root) Item_string(
          pointer_cast<const char *>(clone_field->field_ptr()) + 2,
          uint2korr(clone_field->field_ptr()), clone_field->charset());
      break;
    }
    case MYSQL_TYPE_STRING: {
      auto len = current_thd->variables.sql_mode & MODE_PAD_CHAR_TO_FULL_LENGTH
                     ? clone_field->char_length()
                     : clone_field->charset()->cset->lengthsp(
                           clone_field->charset(),
                           (const char *)(clone_field->field_ptr()),
                           clone_field->field_length);
      result = new (mem_root)
          Item_string(pointer_cast<const char *>(clone_field->field_ptr()), len,
                      clone_field->charset());
      break;
    }
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_INVALID:
    default:
      result = nullptr;
  }
  return result;
}

/**
 This function TRY to estimate the total rows of a range's records
 based on Histogram.

 How we estimate a range using histogram?
 Consider a range where the min_key is [col1=x1, col2=y1] and the
 max_key is [col1=x2, col2=y2]. Since histograms can only be built
 on individual columns, we will split the range for each column into
 sub-range like [col1=x1]~[col1=x2] and [col2=y1]~[col2=y2]. Then we
 will query the histogram for each column's selectivity using the
 sub-ranges above. Last, we multiply the selectivities of multiple
 columns to calculate the total selectivity. Finally, we multiply the
 selectivity by the total number of rows in the table to estimate the
 number of rows in the entire range.

 @param[in]  keynr index number.
 @param[in]  min_key start key value of a range, may be nullptr.
 @param[in]  max_key end key value of a range, may be nulltpr.
 @retval >=0 Number of records estimated in range.
 @retval K_REC_IN_RANGE_HISTOGRAM_ACCESS_FAILED Histogram dosen't exist or
 access failed.
 @retval K_REC_IN_RANGE_OOM Out of memory when allocate new Item_field,
         K_REC_IN_RANGE_NOT_SUPPORT not support scenario.
 */
int64_t ha_cde::records_in_range_by_histogram(uint keynr, key_range *min_key,
                                              key_range *max_key,
                                              ha_rows numberOfRows) {
  MEM_ROOT *mem_root = ha_thd()->mem_root;
  CDE_ASSERT_DEBUG(mem_root);
  CDE_ASSERT_DEBUG(table->keys_in_use_for_query.is_set(keynr));

  /* When Dstore table include secondary engine, table_ref->table not equal
  with field_par->table which result in assertion failure in function
  "Item_field::set_field", we skip records in range estimation by histogram
  for Dstore table if it include secondary engine, refer to BUG2025081826741. */
  if (unlikely(table_share->has_secondary_engine())) {
    return K_REC_IN_RANGE_NOT_SUPPORT;
  }

  KEY *key = table->key_info + keynr;
  double selectivity = 1.0;
  uchar *min_key_buff = nullptr;
  uchar *max_key_buff = nullptr;
  if (min_key != nullptr) {
    min_key_buff = static_cast<uchar *>(
        memdup_root(mem_root, min_key->key, min_key->length));
  }
  if (max_key != nullptr) {
    max_key_buff = static_cast<uchar *>(
        memdup_root(mem_root, max_key->key, max_key->length));
  }
  if ((min_key != nullptr && min_key_buff == nullptr) ||
      (max_key != nullptr && max_key_buff == nullptr)) {
    return K_REC_IN_RANGE_OOM;
  }

  for (uint8_t i = 0; i < key->user_defined_key_parts; ++i) {
    // check if the column is used
    bool with_lower_bound = false;
    bool with_upper_bound = false;
    if (min_key && (bool)(min_key->keypart_map & (1 << i))) {
      with_lower_bound = true;
    }
    if (max_key && (bool)(max_key->keypart_map & (1 << i))) {
      with_upper_bound = true;
    }
    if (!with_lower_bound && !with_upper_bound) {
#ifndef NDEBUG
      /* cus MySQL index's Leftmost Prefix Rule, if the current part is
      not used, then the rest parts will definitely not be used. */
      for (uint8_t j = i + 1; j < key->user_defined_key_parts; ++j) {
        if (min_key) {
          CDE_ASSERT_DEBUG((min_key->keypart_map & (1 << j)) == 0);
        }
        if (max_key) {
          CDE_ASSERT_DEBUG((max_key->keypart_map & (1 << j)) == 0);
        }
      }
      // there is at least one value.
      CDE_ASSERT_DEBUG(i != 0);
#endif /* NDEBUG */
      break;
    }
    // convert the histogram operator
    Item *items[3];
    uchar arg_count = 0;
    bool lower_null = false;
    bool upper_null = false;
    histograms::enum_operator op;
    auto field = new (mem_root) Item_field(key->key_part[i].field);
    DBUG_EXECUTE_IF("histogram_stat_new_item_field_failed", field = nullptr;);
    if (field == nullptr) {
      return K_REC_IN_RANGE_OOM;
    }
    items[0] = field;
    arg_count++;
    // check key part sort order.
    key_range *start = min_key;
    key_range *end = max_key;
    uchar *start_key_buff = min_key_buff;
    uchar *end_key_buff = max_key_buff;
    if (key->key_part[i].key_part_flag & HA_REVERSE_SORT) {
      std::swap(with_lower_bound, with_upper_bound);
      std::swap(start, end);
      std::swap(start_key_buff, end_key_buff);
    }
    if (with_lower_bound && with_upper_bound) {
      auto item0 = CdeConvertKeyRangePartToItem(mem_root, key, start_key_buff,
                                                i, lower_null);
      auto item1 = CdeConvertKeyRangePartToItem(mem_root, key, end_key_buff, i,
                                                upper_null);
      items[1] = item0;
      items[2] = item1;
      if ((!lower_null && item0 == nullptr) ||
          (!upper_null && item1 == nullptr)) {
        return K_REC_IN_RANGE_OOM;
      }
      if (lower_null && upper_null) {
        op = histograms::enum_operator::IS_NULL;
      } else if (lower_null) {
        op = histograms::enum_operator::LESS_THAN_OR_EQUAL;
        items[1] = item1;
        arg_count++;
      } else if (upper_null) {
        op = histograms::enum_operator::GREATER_THAN_OR_EQUAL;
        arg_count++;
      } else if (items[1]->eq(items[2], false)) {
        op = histograms::enum_operator::EQUALS_TO;
        /* Code in histogram module assert that EQUALS_TO operator must
        have 2 items, so we can only ++ istead of +=2 even if we have created
        3 new items. */
        arg_count++;
      } else {
        op = histograms::enum_operator::BETWEEN;
        arg_count += 2;
      }
    } else if (with_lower_bound) {
      auto item0 = CdeConvertKeyRangePartToItem(mem_root, key, start_key_buff,
                                                i, lower_null);
      items[1] = item0;
      if (!lower_null && item0 == nullptr) {
        return K_REC_IN_RANGE_OOM;
      }
      if (lower_null) {
        /* Optimizer will translate NOT NULL to a range where the min_key is
        NULL value and the max_key is nullptr. */
        op = histograms::enum_operator::IS_NOT_NULL;
      } else {
        if (start->flag == HA_READ_AFTER_KEY) {
          op = histograms::enum_operator::GREATER_THAN;
        } else {
          op = histograms::enum_operator::GREATER_THAN_OR_EQUAL;
        }
        arg_count++;
      }
    } else {
      auto item0 = CdeConvertKeyRangePartToItem(mem_root, key, end_key_buff, i,
                                                upper_null);
      items[1] = item0;
      if (!upper_null && item0 == nullptr) {
        return K_REC_IN_RANGE_OOM;
      }
      if (upper_null) {
        /* Consider there is an index defined by 'KEY idx(c1,c2)', if we access
        the idx with WHERE condition 'WHERE c1<=? AND c2 IS NULL', then the
        optimizer will use idx to estimate rows of `c1` and the max_key will be
        (c1=?, c2=null). */
        op = histograms::enum_operator::IS_NULL;
      } else {
        if (end->flag == HA_READ_BEFORE_KEY) {
          op = histograms::enum_operator::LESS_THAN;
        } else {
          op = histograms::enum_operator::LESS_THAN_OR_EQUAL;
        }
        arg_count++;
      }
    }

    // Get the selectivity from histogram.
    double col_sel = 0.0;
    // use the column to find the histogram
    Histogram_rdlock_guard s_histo_guard(&this->table_share->LOCK_m_histograms);
    const histograms::Histogram *histo = this->table_share->find_histogram(
        key->key_part[i].field->field_index());
    if (histo == nullptr) {
      return K_REC_IN_RANGE_HISTOGRAM_ACCESS_FAILED;
    }

    if (histo->get_selectivity(items, arg_count, op, &col_sel)) {
      return K_REC_IN_RANGE_HISTOGRAM_ACCESS_FAILED;
    }
    CDE_ASSERT_DEBUG(col_sel <= 1.0);
    selectivity *= col_sel;
  }
  return std::llround(numberOfRows * selectivity);
}

ha_rows ha_cde::calculate_exact_rows_for_range(uint keynr, key_range *min_key,
                                               key_range *max_key) {
  uint64_t count = 0;
  uint32_t key_prefix_min = 0;
  uint32_t key_prefix_max = 0;

  session_index_info *current_index = cde_index_lookup(keynr);
  if (current_index == nullptr) {
    return HA_POS_ERROR;
  }
  cde_dict_index_t *index_dict = current_index->shared_dict;

  if (min_key) {
    CDE_ASSERT(min_key->flag == HA_READ_AFTER_KEY ||
               min_key->flag == HA_READ_KEY_EXACT);
    key_prefix_min =
        CdeKeypartMapToN(min_key->keypart_map, index_dict->index_col_num);
  }
  if (max_key) {
    CDE_ASSERT(max_key->flag == HA_READ_AFTER_KEY ||
               max_key->flag == HA_READ_BEFORE_KEY);
    key_prefix_max =
        CdeKeypartMapToN(max_key->keypart_map, index_dict->index_col_num);
  }

  ScanKey key_infos = static_cast<ScanKey>(
      CdeAlloc((key_prefix_min + key_prefix_max) * sizeof(ScanKeyData)));
  memset_s(key_infos, (key_prefix_min + key_prefix_max) * sizeof(ScanKeyData),
           0, (key_prefix_min + key_prefix_max) * sizeof(ScanKeyData));

  Datum *index_values_min = static_cast<Datum *>(
      CdeAlloc(key_prefix_min * (sizeof(Datum) + sizeof(CdeVarlena))));
  Datum *index_values_max = static_cast<Datum *>(
      CdeAlloc(key_prefix_max * (sizeof(Datum) + sizeof(CdeVarlena))));
  bool *is_nulls_min =
      static_cast<bool *>(CdeAlloc(key_prefix_min * sizeof(bool)));
  bool *is_nulls_max =
      static_cast<bool *>(CdeAlloc(key_prefix_max * sizeof(bool)));

  uint32_t cols = std::max(key_prefix_min, key_prefix_max);

  for (uint32_t i = 0; i < cols; i++) {
    m_dstore->m_indexColOids[i] = current_index->rel->attr->attrs[i]->atttypid;
    m_dstore->m_indexColCollationOids[i] =
        current_index->rel->attr->attrs[i]->attcollation;
  }

  bool is_asc_min =
      (key_prefix_min < 1
           ? 1
           : !(current_index->rel->index->indexOption[key_prefix_min - 1] & 1));
  bool is_asc_max =
      (key_prefix_max < 1
           ? 1
           : !(current_index->rel->index->indexOption[key_prefix_max - 1] & 1));

  int ret = 0;
  if (index_dict->has_prefix_field()) {
    ret = CdeSetPrefixOid(cols, current_index, m_dstore->m_indexColOids);
    if (ret == CDE_ERROR) {
      CDE_LOG_ERROR("set prefix length failed");
      return HA_POS_ERROR;
    }
  }
  if (min_key && key_prefix_min > 0) {
    /*
    min_key.flag and scankey strategy:
    +------+-------------------------------+--------------------------+
    |      |      HA_READ_KEY_EXACT        |     HA_READ_AFTER_KEY    |
    +------+-------------------------------+--------------------------+
    | ASC  |  CDE_SCAN_ORDER_GREATEREQUAL  |  CDE_SCAN_ORDER_GREATER  |
    +------+-------------------------------+--------------------------+
    | DESC |    CDE_SCAN_ORDER_LESSEQUAL   |    CDE_SCAN_ORDER_LESS   |
    +------+----------------------------------------------------------+
    */

    // See ha_cde::index_read
    ret = key_mysql_to_dstore(table, current_index, key_prefix_min,
                              min_key->key, min_key->length, index_values_min,
                              (bool *)is_nulls_min,
                              m_dstore->table_handler->attrFull);
    if (ret == CDE_ERROR) {
      CDE_LOG_ERROR("key mysql to dstore failed");
      return HA_POS_ERROR;
    }

    if (min_key->flag == HA_READ_AFTER_KEY) {
      cde_btree_api::BuildKeyInfos(
          key_infos, current_index->rel->attr->attrs, index_values_min,
          (bool *)is_nulls_min, key_prefix_min, HA_READ_AFTER_KEY, is_asc_min);
    } else {
      cde_btree_api::BuildKeyInfos(key_infos, current_index->rel->attr->attrs,
                                   index_values_min, (bool *)is_nulls_min,
                                   key_prefix_min, HA_READ_KEY_OR_NEXT,
                                   is_asc_min);
    }
  }

  if (max_key && key_prefix_max > 0) {
    /*
    max_key.flag and scankey strategy:
    +------+-------------------------------+--------------------------+
    |      |      HA_READ_AFTER_KEY        |    HA_READ_BEFORE_KEY    |
    +------+-------------------------------+--------------------------+
    | ASC  |    CDE_SCAN_ORDER_LESSEQUAL   |    CDE_SCAN_ORDER_LESS   |
    +------+-------------------------------+--------------------------+
    | DESC |  CDE_SCAN_ORDER_GREATEREQUAL  |  CDE_SCAN_ORDER_GREATER  |
    +------+----------------------------------------------------------+
    */

    // See ha_cde::index_read
    ret = key_mysql_to_dstore(table, current_index, key_prefix_max,
                              max_key->key, max_key->length, index_values_max,
                              (bool *)is_nulls_max,
                              m_dstore->table_handler->attrFull);
    if (ret == CDE_ERROR) {
      CDE_LOG_ERROR("key mysql to dstore failed");
      return HA_POS_ERROR;
    }
    if (max_key->flag == HA_READ_BEFORE_KEY) {
      cde_btree_api::BuildKeyInfos(
          key_infos + key_prefix_min, current_index->rel->attr->attrs,
          index_values_max, (bool *)is_nulls_max, key_prefix_max,
          HA_READ_BEFORE_KEY, is_asc_max);
    } else {
      cde_btree_api::BuildKeyInfos(
          key_infos + key_prefix_min, current_index->rel->attr->attrs,
          index_values_max, (bool *)is_nulls_max, key_prefix_max,
          HA_READ_KEY_OR_PREV, is_asc_max);
    }
  }
  ScanDirection direction = ScanDirection::FORWARD_SCAN_DIRECTION;
  // In the case of 'a <=(<) 1 or a is null', special processing is required.
  // The range of 'a >= null' is split into 'a is null' and 'a <=(<) 1' for
  // reading.
  bool scan_again_min;
  bool scan_again_max;
  scan_again_min =
      key_prefix_min >= key_prefix_max
          ? CdeNeedScanAgainStart(is_asc_min, key_infos + key_prefix_min - 1,
                                  direction)
          : true;
  scan_again_max =
      key_prefix_max >= key_prefix_min
          ? CdeNeedScanAgainEnd(is_asc_max,
                                key_infos + key_prefix_min + key_prefix_max - 1,
                                direction)
          : true;
  if (scan_again_min && scan_again_max) {
    uint32_t key_prefix = std::max(key_prefix_min, key_prefix_max);
    ScanKey scankey_first = static_cast<ScanKey>(
        CdeAlloc((key_prefix_min + key_prefix) * sizeof(ScanKeyData)));
    ScanKey scankey_again = static_cast<ScanKey>(
        CdeAlloc((key_prefix_max + key_prefix) * sizeof(ScanKeyData)));
    memmove_s(scankey_first, key_prefix_min * sizeof(ScanKeyData), key_infos,
              key_prefix_min * sizeof(ScanKeyData));
    memmove_s(scankey_again + key_prefix, key_prefix_max * sizeof(ScanKeyData),
              key_infos + key_prefix_min, key_prefix_max * sizeof(ScanKeyData));

    Datum *index_values;
    bool *is_nulls;
    if (key_prefix_min >= key_prefix_max) {
      index_values = index_values_min;
      is_nulls = is_nulls_min;
      index_values[key_prefix_min - 1] = 0;
      is_nulls[key_prefix_min - 1] = true;
    } else {
      index_values = index_values_max;
      is_nulls = is_nulls_max;
      index_values[key_prefix_max - 1] = 0;
      is_nulls[key_prefix_max - 1] = true;
    }

    bool is_asc = key_prefix_min >= key_prefix_max ? is_asc_min : is_asc_max;
    if (is_asc) {
      cde_btree_api::BuildKeyInfos(scankey_first + key_prefix_min,
                                   current_index->rel->attr->attrs,
                                   index_values, (bool *)is_nulls, key_prefix,
                                   HA_READ_KEY_OR_PREV, is_asc);
      cde_btree_api::BuildKeyInfos(
          scankey_again, current_index->rel->attr->attrs, index_values,
          (bool *)is_nulls, key_prefix, HA_READ_AFTER_KEY, is_asc);
    } else {
      cde_btree_api::BuildKeyInfos(scankey_first + key_prefix_min,
                                   current_index->rel->attr->attrs,
                                   index_values, (bool *)is_nulls, key_prefix,
                                   HA_READ_BEFORE_KEY, is_asc);
      cde_btree_api::BuildKeyInfos(
          scankey_again, current_index->rel->attr->attrs, index_values,
          (bool *)is_nulls, key_prefix, HA_READ_KEY_OR_NEXT, is_asc);
    }

    count = CdeGetScanCount(current_index->rel, scankey_first,
                            key_prefix_min + key_prefix, direction) +
            CdeGetScanCount(current_index->rel, scankey_again,
                            key_prefix_max + key_prefix, direction);

    CdeFree(scankey_first);
    CdeFree(scankey_again);
  } else {
    count = CdeGetScanCount(current_index->rel, key_infos,
                            key_prefix_min + key_prefix_max, direction);
  }

  CdeFree(index_values_min);
  CdeFree(index_values_max);
  CdeFree(is_nulls_min);
  CdeFree(is_nulls_max);
  CdeFree(key_infos);
  return count;
}

/** Estimates the number of index records in a range.
 @return estimated number of rows */

ha_rows ha_cde::records_in_range(uint keynr,         /*!< in: index number */
                                 key_range *min_key, /*!< in: start key value of
                                                     the range, may also be 0 */
                                 key_range *max_key) /*!< in: range end key val,
                                                     may also be 0 */
{
  DBUG_TRACE;
  uint64_t n_rows = 0;
  Huawei::Common::LatencyCounter::Timer records_in_range_latency(
      cde_perf_counters::getInstance()->_records_in_range_latency,
      dstore_enable_perf_timers);

  DBUG_EXECUTE_IF("dstore_records_in_range_wring_key_no",
                  keynr = table->s->keys + 1;);
  if (keynr > table->s->keys) {
    n_rows = HA_POS_ERROR;
    goto func_exit;
  }

  /* if no min_key and max_key, return stats.records directly */
  DBUG_EXECUTE_IF("dstore_records_in_range_no_min_max_key", {
    min_key = nullptr;
    max_key = nullptr;
  });
  if (min_key == nullptr && max_key == nullptr) {
    n_rows = stats.records;
    goto func_exit;
  }

  /* estimate records by histogram algorithm */
  if (false == m_dontUseHistogram &&
      current_thd->optimizer_switch_flag(
          OPTIMIZER_SWITCH_DSTORE_USE_HISTOGRAM_RANGE_ESTIMATION)) {
    int64_t records = records_in_range_by_histogram(keynr, min_key, max_key,
                                                    this->stats.records);
    if (records >= 0) {
      n_rows = static_cast<uint64_t>(records);
      goto func_exit;
    }
  }

  if (g_calculateExactRowsForRange) {
    /* TODO following code need to be optimized due to poor performance */
    n_rows = calculate_exact_rows_for_range(keynr, min_key, max_key);
  } else {
    n_rows = handler::records_in_range(keynr, min_key, max_key);
  }

func_exit:

  records_in_range_latency.end();
  /* The MySQL optimizer seems to believe an estimate of 0 rows is
  always accurate and may return the result 'Empty set' based on that.
  Add 1 to the value to make sure MySQL does not make the assumption! */
  return n_rows == 0 && !m_returnExactNrowsInRange ? 1 : n_rows;
}

/**
Store a reference to the current row to 'ref' field of the handle.
Note that in the case where we have generated the clustered index for the
table, the function parameter is illogical: we MUST ASSUME that 'record'
is the current 'position' of the handle, because if row ref is actually
the row id internally generated in CDE, then 'record' does not contain
it. We just guess that the row id must be for the record where the handle
was positioned the last time.
@param[in]  record  row in MySQL format */
void ha_cde::position(const uchar *record) {
  CDE_ASSERT((table->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX) ==
             false);
  uint len = sizeof(DSTORE::ItemPointer);
  errno_t err = memcpy_s(ref, len, &m_dstore->dstore_heap_ctid, len);
  CDE_ASSERT(err == 0);
  /* We assume that the 'ref' value len is always fixed for the same table. */
  if (len != ref_length) {
    CDE_LOG_ERROR("len and ref_length should be same: %d,  %d ", len,
                  ref_length);
  }
  (void)record;
}

/** Tells something additional to the handler about how to do things.
 @return 0 or error number */

int ha_cde::extra(enum ha_extra_function operation)
/*!< in: HA_EXTRA_FLUSH or some other flag */
{
  m_dstore->read_just_key = 0;

  switch (operation) {
    case HA_EXTRA_FLUSH:
      break;
    case HA_EXTRA_RESET_STATE:
      m_dstore->keep_other_fields_on_keyread = 0;
      m_dstore->m_allowDuplicates = false;
      break;
    case HA_EXTRA_NO_KEYREAD:
      m_dstore->read_just_key = 0;
      break;
    case HA_EXTRA_KEYREAD:
      m_dstore->read_just_key = 1;
      break;
    case HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
      m_dstore->keep_other_fields_on_keyread = 1;
      break;

    /* IMPORTANT: m_prebuilt->trx can be obsolete in this method, because it is
    not sure that MySQL calls external_lock before this method with the
    parameters below.  We must not invoke update_thd() either, because the
    calling threads may change. CAREFUL HERE, OR MEMORY CORRUPTION MAY OCCUR! */
    case HA_EXTRA_IGNORE_DUP_KEY:
    case HA_EXTRA_INSERT_WITH_UPDATE:
    case HA_EXTRA_WRITE_CAN_REPLACE:
      m_dstore->m_allowDuplicates = true;
      break;
    case HA_EXTRA_NO_IGNORE_DUP_KEY:
    case HA_EXTRA_WRITE_CANNOT_REPLACE:
      m_dstore->m_allowDuplicates = false;
      break;
    case HA_EXTRA_NO_READ_LOCKING:
      break;
    case HA_EXTRA_BEGIN_ALTER_COPY:
      break;
    case HA_EXTRA_END_ALTER_COPY:
      break;
    case HA_EXTRA_NO_AUTOINC_LOCKING:
      m_dstore->autoincCtx.m_noAutoincLocking = true;
      break;
    case HA_EXTRA_FLUSH_STATISTICS_BY_BACKGROUND: {
      cde_dict_t *dict_table = m_dstore->table_handler;
      if (dict_table != nullptr && dict_table->auto_recalc_enabled()) {
        TrxTable tmpTrxTable{dict_table->m_id, dict_table->name,
                             dict_table->stat_n_rows,
                             dict_table->stat_modified_counter,
                             dict_table->auto_recalc_enabled()};
        CdeAddStatTable(tmpTrxTable, true);
      }
    } break;
    case HA_EXTRA_ALTER_TABLE_FLUSH_STATISTICS: {
      cde_dict_t *dict_table = m_dstore->table_handler;
      if (dict_table != nullptr && dict_table->auto_recalc_enabled()) {
        if (dict_table->dict_stats_persist_enabled()) {
          (void)CdeDictTableStatsUpdate(dict_table, RECALC_PERSIST, ha_thd());
        } else {
          (void)CdeDictTableStatsUpdate(dict_table, RECALC_STATS, ha_thd());
        }
      }
    } break;
    default: /* Do nothing */
             ;
  }

  return (0);
}

/**
called by lock table.
@param[in]  thd     handle to the user thread
@param[in]  lock_type   lock type
@return 0 or error code */
int ha_cde::start_stmt(THD *thd, thr_lock_type lock_type) {
  (void)CdeCreateOrGetSession(thd);

  m_dstore->sql_stat_start = true;
  m_dstore->hint_need_to_fetch_extra_cols = 0;
  m_dstore->keep_other_fields_on_keyread = 0;
  m_dstore->read_just_key = 0;

  // todo : intrinsic table
  // todo : temporary table
  if (!m_mysql_has_locked) {
    /* This handle is for a temporary table created inside
    this same LOCK TABLES; since MySQL does NOT call external_lock
    in this case, we must use x-row locks inside InnoDB to be
    prepared for an update of a row */

    m_dstore->select_lock_type = LOCK_X;
  } else {
    /* Not a consistent read: restore the select_lock_type value. The value of
    stored_select_lock_type was decided in:
    1) ::store_lock(),
    2) ::external_lock(),
    3) ::init_table_handle_for_HANDLER(). */

    m_dstore->select_lock_type = m_stored_select_lock_type;
  }

  // start transaction
  if (lock_type != TL_UNLOCK) {
    /* For the `LOCK TABLE` syntax, we use `start_stmt` to indicate the start
    of the statement, therefore, statement-level transactions need to be
    registered. */
    CdeRegisterTrx(thd);

    if (!IsMultiStmtsTrxRunInProcess() && !IsSingleStmtTrxInProcess()) {
      CdeTrxStartIfNotStarted(thd);
    }

    cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
    /* Reset the AUTOINC statement level counter for multi-row INSERTs. */
    trx->autoinc_row_num = 0;
  }

  return 0;
}

/** Update create_info.  Used in SHOW CREATE TABLE et al. */
void ha_cde::update_create_info(
    HA_CREATE_INFO *create_info) /*!< in/out: create info */
{
  if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
    info(HA_STATUS_AUTO);
    create_info->auto_increment_value = stats.auto_increment_value;
  }
}

/** Call this when you have opened a new table handle in HANDLER, before you
 call index_read_map() etc. Actually, we can let the cursor stay open even
 over a transaction commit! Then you should call this before every operation,
 fetch next etc. This function inits the necessary things even after a
 transaction commit. */
void ha_cde::init_table_handle_for_HANDLER(void) {}

/** Returns the table type (storage engine name).
 @return table type */
const char *ha_cde::table_type() const { return DSTORE_ENGINE_NAME; }

/** Returns the maximum number of keys.
 @return MAX_KEY */
uint ha_cde::max_supported_keys() const { return MAX_KEY; }

/** Returns the maximum key length.
 @return maximum supported key length, in bytes */
uint ha_cde::max_supported_key_length() const {
  uint cde_max_key_length =
      CdeExpandKeyLenByTD() ? max_tuple_length_by_limit_td : max_tuple_length;
  return cde_max_key_length;
}

uint ha_cde::max_supported_key_part_length(HA_CREATE_INFO *create_info) const {
  (void)create_info;
  uint cde_max_key_length =
      CdeExpandKeyLenByTD() ? max_tuple_length_by_limit_td : max_tuple_length;
  return cde_max_key_length;
}

uint ha_cde::lock_count(void) const { return 0; }

THR_LOCK_DATA **ha_cde::store_lock(
    THD *thd,           /*!< in: user thread handle */
    THR_LOCK_DATA **to, /*!< in: pointer to the current element in an array of
                        pointers to lock structs; only used as return value */
    thr_lock_type lock_type) /*!< in: lock type to store in 'lock'; this may
                                also be TL_IGNORE */
{
  DBUG_TRACE;

  const bool in_lock_tables = thd_in_lock_tables(thd);
  const uint sql_command = thd_sql_command(thd);

  // todo : check intrinsic table

  if (sql_command == SQLCOM_FLUSH && lock_type == TL_READ_NO_INSERT) {
    m_dstore->select_lock_type = LOCK_NONE;
    m_stored_select_lock_type = LOCK_NONE;
    /* Check for DROP TABLE */
  } else if (sql_command == SQLCOM_DROP_TABLE) {
    /* Check for LOCK TABLE t1,...,tn WITH SHARED LOCKS */
  } else if ((lock_type == TL_READ_HIGH_PRIORITY && in_lock_tables) ||
             (lock_type == TL_READ && in_lock_tables) ||
             lock_type == TL_READ_NO_INSERT ||
             lock_type == TL_READ_WITH_SHARED_LOCKS ||
             (lock_type != TL_IGNORE && sql_command != SQLCOM_SELECT)) {
    if (sql_command == SQLCOM_CHECKSUM ||
        (/* trx->skip_gap_locks() && */
         (lock_type == TL_READ || lock_type == TL_READ_NO_INSERT) &&
         (sql_command == SQLCOM_REPLACE_SELECT ||
          sql_command == SQLCOM_INSERT_SELECT ||
          sql_command == SQLCOM_CREATE_TABLE ||
          sql_command == SQLCOM_UPDATE))) {
      m_dstore->select_lock_type = LOCK_NONE;
      m_stored_select_lock_type = LOCK_NONE;
    } else {
      m_dstore->select_lock_type = LOCK_S;
      m_stored_select_lock_type = LOCK_S;
    }

  } else if (lock_type != TL_IGNORE) {
    /* We set possible LOCK_X value in external_lock, not yet
    here even if this would be SELECT ... FOR UPDATE */

    m_dstore->select_lock_type = LOCK_NONE;
    m_stored_select_lock_type = LOCK_NONE;
  }

  /* Set select mode for SKIP LOCKED / NOWAIT */
  if (lock_type != TL_IGNORE) {
    switch (table->pos_in_table_list->lock_descriptor().action) {
      // todo: check THR_SKIP mode
      case THR_SKIP:
        m_dstore->select_mode = SELECT_SKIP_LOCKED;
        break;
      case THR_NOWAIT:
        m_dstore->select_mode = SELECT_NOWAIT;
        break;
      default:
        m_dstore->select_mode = SELECT_ORDINARY;
        break;
    }
  }

  return to;
}

/** Determines if the primary key is clustered index.
 @return true */
bool ha_cde::primary_key_is_clustered() const { return false; }

/** Compares two 'refs'. A 'ref' is the (internal) primary key value of the row.
 If there is no explicitly declared non-null unique key or a primary key, then
 CDE internally uses the row id as the primary key.
 @return < 0 if ref1 < ref2, 0 if equal, else > 0 */
int ha_cde::cmpRef(const uchar *ref1, /*!< in: an (internal) primary key value
                                          in the MySQL key value format */
                   const uchar *ref2) /*!< in: an (internal) primary key value
                                          in the MySQL key value format */
{
  uchar *ref1_1 = const_cast<uchar *>(ref1);
  uchar *ref2_2 = const_cast<uchar *>(ref2);
  auto *ctid1 = reinterpret_cast<DSTORE::ItemPointerData *>(ref1_1);
  auto *ctid2 = reinterpret_cast<DSTORE::ItemPointerData *>(ref2_2);
  return DSTORE::ItemPointerData::Compare(ctid1, ctid2);
}

/** Compares two 'refs'. A 'ref' is the (internal) primary key value of the row.
 If there is no explicitly declared non-null unique key or a primary key, then
 CDE internally uses the row id as the primary key.
 @return < 0 if ref1 < ref2, 0 if equal, else > 0 */
int ha_cde::cmp_ref(const uchar *ref1, /*!< in: an (internal) primary key value
                                          in the MySQL key value format */
                    const uchar *ref2) /*!< in: an (internal) primary key value
                                          in the MySQL key value format */
    const {
  return cmpRef(ref1, ref2);
}

int ha_cde::post_rename_table_statistics(const char *from, const char *to,
                                         const dd::Table *fromTable
                                         [[maybe_unused]],
                                         const dd::Table *toTable
                                         [[maybe_unused]]) {
  if (!CdeIsMysqlTmpTableName(from) && !CdeIsMysqlTmpTableName(to)) {
    return DstoreDictStatRenameTable(ha_thd(), from, to);
  }
  return 0;
}

int ha_cde::rename_table(const char *from, const char *to,
                         const dd::Table *from_table_def,
                         dd::Table *to_table_def) {
  // to make sure init dstore session
  THD *thd = ha_thd();
  CdeTrxStartIfNotStarted(thd, true);

  cde_dict_t *dictTable = DictSysGetTable(from);
  if (dictTable == nullptr) {
    dictTable = CdeDdOpenTableOnDdObj(thd, from, from_table_def);
    if (dictTable == nullptr) {
      return HA_ERR_NO_SUCH_TABLE;
    }
  } else if (dictTable->refresh_fk) {
    CDE_ASSERT(!dictTable->m_discardAfterDDL);
    std::deque<const char *> fkListNames;
    CdeDdTableLoadForeignKey(thd, from, dictTable, from_table_def, fkListNames);
    dictTable->refresh_fk = false;
    dictTable->oldName.reset();
  }

  DictTableRefGuard dictRefGuard(dictTable);
  int ret = CdeRenameTable(thd, from, to, dictTable, to_table_def);
  bool isCreateTemp = dictTable->is_temporary();
  bool stat_persist_switch = dictTable->dict_stats_persist_enabled();
  dictRefGuard.Release();

  DEBUG_SYNC(thd, "after_dstore_rename_table");

  if (stat_persist_switch && !isCreateTemp && !CdeIsMysqlTmpTableName(from) &&
      !CdeIsMysqlTmpTableName(to)) {
    (void)DstoreDictStatRenameTable(thd, from, to);
  }
  return ConvertErrcodeToMysql(ret, 0, ha_thd());
}

int ha_cde::delete_table(const char *name, const dd::Table *table_def) {
  // to make sure init dstore session
  THD *thd = ha_thd();
  CdeCreateOrGetSession(thd);

  if (table_def != nullptr && table_def->is_persistent()) {
    CdeTrxStartIfNotStarted(thd, true);
  }

  if (thd->for_ddl_info_replay) {
    DictSysRemoveTable(name);
    return 0;
  }

  bool stat_persist_switch = dstore_stats_persistent;
  {
    cde_dict_t *dictTable = DictSysGetTable(name);
    DictTableRefGuard dictRefGuard(dictTable);
    if (dictTable != nullptr) {
      stat_persist_switch = dictTable->dict_stats_persist_enabled();
      if (dictTable->is_temporary()) stat_persist_switch = false;
    }
  }

  bool ret = CdeDropTable(thd, name, table_def);
  DBUG_EXECUTE_IF("cde_drop_table_error_before_commit", { ret = CDE_FAIL; });
  if (ret == CDE_FAIL) {
    my_error(ER_ENGINE_CANT_DROP_TABLE, MYF(0), name);
  }
  DBUG_EXECUTE_IF("cde_drop_table_crash_before_commit", DBUG_SUICIDE(););
  DBUG_EXECUTE_IF("cde_drop_db_crash_after_some_tables_dropped", {
    static uint16_t cnt = 0;
    if (cnt == 5) {
      DBUG_SUICIDE();
    }
    cnt++;
  });

  if (stat_persist_switch && !CdeIsMysqlTmpTableName(name)) {
    (void)DstoreDictStatDropTable(name, thd);
  }

  return ret == CDE_SUCC ? 0 : HA_ERR_GENERIC;
}

/** check the table can be dropped with recyclebin.
 @return error number */
int ha_cde::ha_check_support_recyclebin(
    const char *,             /*!< in: table name */
    bool &support_recyclebin, /*!< out: whether the table support recyclebin */
    const dd::Table *) {
  /** In the InnoDB engine, whether a table supports recyclebin needs to be
   * determined by checking whether the table has a full-text index,
   * whether the table was created as a file-per-table tablespace, and whether
   * the tablespace has been discarded. These scenarios do not exist in the
   * Drstore engine, so no additional judgment is required. Additionally,
   * partitioned tables require special handling, but currently, the Dstore
   * engine does not support partitioned tables, so no additional processing is
   * needed.
   */
  DBUG_ENTER("ha_cde::ha_check_support_recyclebin");
  support_recyclebin = true;
  DBUG_RETURN(0);
}

/** Initializes a handle to use an index.
 @return 0 or error number */
int ha_cde::index_init(uint keynr,  /*!< in: key (index) number */
                       bool sorted) /*!< in: 1 if result MUST be sorted
                                       according to index */
{
  DBUG_TRACE;
  m_scanned_offset = 0;

  if (keynr > table->s->keys) {
    CDE_LOG_ERROR(
        "keynr > table->s->keys should not happen: keynr=%d, table->s->keys "
        "=%d",
        keynr, table->s->keys);
    return 0;
  }

  auto current_index = cde_index_lookup(keynr);
  if (current_index == nullptr) {
    CDE_LOG_ERROR("can not found useful current index");
    return -1;
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx, true);
  if (trx->scanSnapshot.snapshotCsn == DSTORE::INVALID_CSN) {
    return HA_ERR_GENERIC;
  }
  bool is_visible = m_dstore->table_handler->is_temporary() ||
                    XidVisibleToSnapshot(&trx->scanSnapshot,
                                         current_index->shared_dict->index_csn);

  if (!is_visible) {
    return HA_ERR_TABLE_DEF_CHANGED;
  }

  m_dstore->current_cde_index = current_index;
  m_dstore->dstore_heap_scan = nullptr;
  m_dstore->dstore_index_scan = nullptr;
  m_dstore->key_map = 0;
  m_dstore->keys_prefix_counts = 0;
  m_dstore->m_icpCheckCount = 0;
  m_dstore->m_icpMatchCount = 0;

  active_index = keynr;
  (void)sorted;

  m_dstore->m_readVirutalColsFromIndex = false;

  set_columns_to_decode();

  /** Check if we have to read some virtual columns from the index.
   * This is useful when it turns out that we need to fallback to
   * reading whole row from a table.
   */
  if (m_dstore->table_handler->m_vColCount > 0) {
    auto indexCols = current_index->shared_dict->index_cols;
    auto indexColsNum = current_index->shared_dict->index_col_num;
    for (uint32_t i = 0; i < indexColsNum; ++i) {
      /* FIXME: cde_dict_index_t::index_cols not include instant-dropped columns
      while the m_dictCols include instant-dropped columns. */
      if (m_dstore->table_handler->m_dictCols[indexCols[i]].m_isVirtual &&
          bitmap_is_set(table->read_set, indexCols[i])) {
        m_dstore->m_readVirutalColsFromIndex = true;
        break;
      }
    }
  }

  return 0;
}

/** Currently only flushes counters accumulated during scan
 @return 0 */
int ha_cde::index_end(void) {
  DBUG_TRACE;
  DBUG_EXECUTE_IF("print_dstore_scankey_info",
                  PrintIndexScanKeyInfo(m_dstore, table););

  if (m_dstore->m_icpCheckCount > 0)
    cde_perf_counters::getInstance()->_cde_icp_check.increment(
        m_dstore->m_icpCheckCount);
  if (m_dstore->m_icpMatchCount > 0)
    cde_perf_counters::getInstance()->_cde_icp_match.increment(
        m_dstore->m_icpMatchCount);

  index_read_cleanup();
  active_index = MAX_KEY;
  m_dstore->key_map = 0;
  m_dstore->keys_prefix_counts = 0;
  m_dstore->m_icpCheckCount = 0;
  m_dstore->m_icpMatchCount = 0;

  m_dstore->m_readVirutalColsFromIndex = false;

  in_range_check_pushed_down = false;
  m_ds_mrr.dsmrr_close();
  return 0;
}

/** Initialize a table scan.
@param[in]  scan    whether this is a second call to rnd_init()
                        without rnd_end() in between
@return 0 or error number */
int ha_cde::rnd_init(bool scan) {
  DBUG_TRACE;
  m_scanned_offset = 0;

  if (m_dstore->dstore_heap_scan) {
    HeapInterface::ReScan(m_dstore->dstore_heap_scan);
    return 0;
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx, true);
  if (trx->scanSnapshot.snapshotCsn == DSTORE::INVALID_CSN) {
    return HA_ERR_GENERIC;
  }

  m_dstore->dstore_heap_scan = HeapInterface::CreateHeapScanHandler(
      m_dstore->rel_info->rd_storage_releation);
  DBUG_ASSERT(m_dstore->dstore_heap_scan != nullptr);

  trx->appendHeapScanHandler((&m_dstore->dstore_heap_scan));

  CDE_ASSERT_DEBUG(m_dstore->table_handler->is_temporary() ||
                   m_dstore->table_handler->m_rebuild_csn !=
                       DSTORE::INVALID_CSN);

  bool is_visible =
      m_dstore->table_handler->is_temporary() ||
      XidVisibleToSnapshot(&trx->scanSnapshot,
                           m_dstore->table_handler->m_rebuild_csn);

  if (!is_visible) {
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
    m_dstore->dstore_heap_scan = nullptr;
    return HA_ERR_TABLE_DEF_CHANGED;
  }

  HeapInterface::BeginScan(m_dstore->dstore_heap_scan, &trx->scanSnapshot);
  if (!m_cond_handler.GetCondList().is_empty()) {
    ScanKey scan_key = nullptr;
    uint32_t key_num = 0;

    m_cond_handler.GenScankeyFromCond(scan_key, key_num, get_dstore_handler(),
                                      table, ha_thd()->mem_root);
    if (scan_key != nullptr)
      HeapInterface::SetScanKey(m_dstore->dstore_heap_scan,
                                m_dstore->rel_info->rd_storage_releation->attr,
                                key_num, scan_key);
  }

  if (!scan) {
    m_dstore->row_read_type = ROW_READ_WITH_LOCKS;
  }

  set_columns_to_decode();
  return 0;
}

/** Ends a table scan.
 @return 0 or error number */

int ha_cde::rnd_end(void) {
  DBUG_TRACE;

  if (m_dstore->dstore_heap_scan && !CdeIsTransactionMemContextReset()) {
    HeapInterface::EndScan(m_dstore->dstore_heap_scan);
    cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
    CDE_ASSERT_DEBUG(nullptr != trx);
    trx->removeHeapScanHandler(&(m_dstore->dstore_heap_scan));
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
  }

  // m_dstore->scanSnapshot
  m_dstore->dstore_heap_scan = nullptr;
  m_ds_mrr.dsmrr_close();
  m_cond_handler.CleanUpResources();
  return 0;
}

/** Deletes a row given as the parameter.
 @return error number or 0 */
int ha_cde::delete_row(const uchar *record) /*!< in: a row in MySQL format */
{
  DBUG_TRACE;
  int error_result = CDE_OK;
  int error = CDE_OK;
  char savept_name[MAX_SAVEPOINT_NAME_LEN];
  void *temp_savept;
  // to make sure init dstore session
  THD *thd = ha_thd();

  // Update statistics, increment delete counter.
  ha_statistic_increment(&System_status_var::ha_delete_count);

  auto colNum = m_dstore->table_handler->GetTotalCols();
  auto vColNum = m_dstore->table_handler->m_vColCount;
  dml_upd_ctx ctx(CdeGetTrxinfo(thd), m_dstore->table_handler,
                  m_dstore->rel_info, colNum, vColNum, true, thd,
                  &m_blob_mem_root, table);
  error = ctx.init();
  if (unlikely(error != CDE_OK)) {
    return ConvertErrcodeToMysql(error, 0, ha_thd());
  }

  ctx.set_old_ctid(&m_dstore->dstore_heap_ctid);

  if (unlikely(m_dstore->m_allowDuplicates)) {
    CdeTrxGetNameOfSavepoint((uint64_t)&temp_savept, savept_name);
    error_result = CdeTrxCreateSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());
    }
  }

  ctx.SetNeedCheckOnlineDdl(m_dstore->m_needCheckOnlineDdl);
  error = CdeDmlUpdateRow(table, record, nullptr, &ctx);
  m_dstore->m_needCheckOnlineDdl = ctx.NeedCheckOnlineDdl();
  // to deal with damage heap data page
  if (unlikely(CdeEnableDamagePageManager() &&
               (int)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE ==
                   error)) {
    error_result = CdeTrxRollbackToSavepoint(savept_name);
    int current_error = CdeTrxReleaseSavepoint(savept_name);
    if (CDE_OK == error_result) {
      error_result = current_error;
    }
    return ConvertErrcodeToMysql(error_result, 0, ha_thd());
  }
  // to deal with DELETE IGNORE scenarios
  if (unlikely(m_dstore->m_allowDuplicates && error != CDE_OK)) {
    error_result = CdeTrxRollbackToSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      (void)CdeTrxReleaseSavepoint(savept_name);
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());  // cannot happen
    }
  }

  if (unlikely(m_dstore->m_allowDuplicates)) {
    error_result = CdeTrxReleaseSavepoint(savept_name);
    if (unlikely(error_result != CDE_OK)) {
      return ConvertErrcodeToMysql(error_result, 0, ha_thd());  // cannot happen
    }
  }

  if (error == CDE_OK) {
    m_dstore->table_handler->dec_n_rows();
    m_dstore->table_handler->stat_modified_counter++;
    cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
    UpdateTrxTable(trx, m_dstore->table_handler->m_id,
                   m_dstore->table_handler->stat_modified_counter,
                   m_dstore->table_handler->stat_n_rows);
  }

  return ConvertErrcodeToMysql(error, 0, ha_thd());
}

/**
MySQL calls this method at the end of each statement

@return 0 if success, other values if fail.
*/
int ha_cde::reset() {
  m_ds_mrr.reset();
  m_dstore->autoincCtx.m_lastVal = 0;
  m_dstore->autoincCtx.m_noAutoincLocking = false;
  m_dstore->keep_other_fields_on_keyread = 0;
  m_dstore->read_just_key = 0;
  m_blob_mem_root.Clear();
  m_cond_handler.CleanUpResources();
  pushed_cond = nullptr;
  cde_session_t *session =
      *(cde_session_t **)thd_ha_data(ha_thd(), cde_hton_ptr);
  if (session) {
    session->get_trxinfo()->removeHeapScanHandler(&m_dstore->dstore_heap_scan);
  }
  m_pushed_offset = m_scanned_offset = 0;
  m_pushed_aggregate = nullptr;
  return 0;
}

/**
 * Add a modified table to the transaction.
 * @param thd The session object.
 * @param trxTable The ref of the TrxTable object.
 *
 * This function is used to add a modified table to the transaction. First, we
 * retrieve the transaction information from the session object. Then, we
 * iterate through all the modified tables in the transaction. If the table
 * already exists, we simply return. If the table does not exist, we add it to
 * the list of modified tables.
 */
void AddModTables(THD *thd, const TrxTable &trxTable) {
  if (CdeIsMysqlTmpTableName(trxTable.m_name.c_str())) {
    return;
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
  for (const auto &table : trx->m_modTables) {
    if (table.m_tableId == trxTable.m_tableId) {
      return;
    }
  }
  trx->m_modTables.emplace_back(trxTable);
}

/** As MySQL will execute an external lock for every new table it uses when it
 starts to process an SQL statement (an exception is when MySQL calls
 start_stmt for the handle) we can use this function to store the pointer to
 the THD in the handle. We will also use this function to communicate
 to CDE that a new SQL statement has started and that we must store a
 savepoint to our transaction handle, so that we are able to roll back
 the SQL statement in case of an error.
 @return 0 */

int ha_cde::external_lock(THD *thd,      /*!< in: handle to the user thread */
                          int lock_type) /*!< in: lock type */
{
  int ret;
  /* This code path is executed when a physical standby server replays CREATE
  TRIGGER statements.
  Locking is not required during DDL replay on the physical standby server. */
  if (thd->for_ddl_info_replay && g_guc.enableStandbyRole) return 0;

  // todo: check intrinsic table
  // todo: Statement based binlogging
  // todo: read-only mode.

  m_dstore->sql_stat_start = true;
  m_dstore->hint_need_to_fetch_extra_cols = 0;
  m_dstore->keep_other_fields_on_keyread = 0;
  m_dstore->read_just_key = 0;

  const enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);

  if (lock_type == F_WRLCK) {
    /* If this is a SELECT, then it is in UPDATE TABLE ...
    or SELECT ... FOR UPDATE */
    m_dstore->select_lock_type = LOCK_X;
    m_stored_select_lock_type = LOCK_X;
  }

  assert(!(lock_type == F_RDLCK && m_dstore->select_lock_type == LOCK_X));

  if (lock_type != F_UNLCK) {
    CdeTrxStartIfNotStarted(thd);

    /* MySQL is setting a new table lock */
    if (lock_type == F_RDLCK) {
      /**
          To limit range of circumstances under which transaction's isolation
          level can be compromised, we allow disabling readlocks only for DD
          and ACL tables.
      */

      // todo : dd table should be LOCK_NONE
      // Retain value set earlier for example via store_lock()
      // which is LOCK_S or LOCK_NONE
      CDE_ASSERT_DEBUG(m_dstore->select_lock_type == LOCK_S ||
                       m_dstore->select_lock_type == LOCK_NONE);
    }

    if (m_dstore->select_lock_type != LOCK_NONE) {
      if (sql_command == SQLCOM_LOCK_TABLES /* && THDVAR(thd, table_locks) */ &&
          thd_test_options(thd, OPTION_NOT_AUTOCOMMIT) &&
          thd_in_lock_tables(thd)) {
        // LOCK TABLE
        ret = CdeLockTable(static_cast<lock_mode>(m_dstore->select_lock_type),
                           m_dstore->rel_info->rd_storage_releation);
        if (ret != DSTORE::DSTORE_SUCC) {
          CDE_LOG_ERROR("lock table error_code=%d", ret);
          return HA_ERR_LOCK_TABLE_FULL;
        }
      }
    }

    m_mysql_has_locked = true;

    // todo : begin_stmt(trx)
    if (lock_type == F_WRLCK) {
      TrxTable tmpTrxTable{m_dstore->table_handler->m_id,
                           m_dstore->table_handler->name,
                           m_dstore->table_handler->stat_n_rows,
                           m_dstore->table_handler->stat_modified_counter,
                           m_dstore->table_handler->auto_recalc_enabled()};
      AddModTables(thd, tmpTrxTable);
    }

    cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
    trx->m_sqltablesInused++;
    return CDE_OK;
  } else {
    // todo : end_stmt(trx);
    DEBUG_SYNC_C("ha_cde_end_statement");
    /* Update statistics for lock table scenario. For example:
    LOCK TABLE t1 WRITE; INSERT INTO t1 VALUES xxx; UNLOCK TABLES;
    We do not update table t1's statistics when INSERT transaction
    committed, since table t1 is locked, analyze table will be blocked,
    we choose to delay update statistics when table is unlocked. */
    bool in_lock_tables = thd->variables.option_bits & OPTION_TABLE_LOCK;
    if (sql_command == SQLCOM_UNLOCK_TABLES && in_lock_tables) {
      cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
      UpdateStatsByOid(trx, m_dstore->table_handler->m_id,
                       m_dstore->table_handler->stat_modified_counter,
                       m_dstore->table_handler->stat_n_rows);
    }
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(thd);
  trx->m_sqltablesInused--;

  /* If the MySQL lock count drops to zero we know that the current SQL
  statement has ended, we set snapshotCsn INVALID */
  if (trx->m_sqltablesInused == 0) {
    trx->scanSnapshot.snapshotCsn = DSTORE::INVALID_CSN;
  }
  /* MySQL is releasing a table lock */
  m_mysql_has_locked = false;

  // todo : transaction checking and commit.

  return 0;
}

/** Initialize sampling dstore.
  @param[out] scan_ctx  dstore scan context created by this method that has to
  be used in sample_next
  @param[in]  sampling_percentage dstore sample percentage of records that need
  to be sampled
  @param[in]  sampling_seed       dstore sample random seed that the random
  generator will use
  @param[in]  sampling_method     dstore sampling method to be used; currently
  only SYSTEM sampling is supported
  @return 0 for success, else one of the HA_cde values in case of error. */
int ha_cde::sample_init(void *&scan_ctx, double sampling_percentage,
                        int sampling_seed, enum_sampling_method, const bool) {
  DBUG_TRACE;

  cde_dict_t *dict_table = m_dstore->table_handler;
  if (dict_table == nullptr) {
    return HA_ERR_INTERNAL_ERROR;
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx);
  m_dstore->dstore_heap_scan = HeapInterface::CreateHeapScanHandler(
      m_dstore->rel_info->rd_storage_releation);
  DBUG_ASSERT(m_dstore->dstore_heap_scan != nullptr);
  trx->appendHeapScanHandler((&m_dstore->dstore_heap_scan));
  bool is_visible =
      m_dstore->table_handler->is_temporary() ||
      XidVisibleToSnapshot(&trx->scanSnapshot,
                           m_dstore->table_handler->m_rebuild_csn);

  if (!is_visible) {
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
    m_dstore->dstore_heap_scan = nullptr;
    return HA_ERR_TABLE_DEF_CHANGED;
  }

  HeapInterface::BeginScan(m_dstore->dstore_heap_scan, &trx->scanSnapshot);

  DSTORE::HeapSampleScanContext *ctx = new DSTORE::HeapSampleScanContext();
  if (unlikely(ctx == nullptr ||
               DBUG_EVALUATE_IF("simulate_sample_init_new_samplescanctx_failed",
                                true, false))) {
    HeapInterface::EndScan(m_dstore->dstore_heap_scan);
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
    m_dstore->dstore_heap_scan = nullptr;
    if (ctx != nullptr) {
      delete ctx;
    }
    return HA_ERR_INTERNAL_ERROR;
  }

  ctx->numTuples = 0;
  scan_ctx = static_cast<void *>(ctx);

  DBUG_ASSERT(sampling_percentage >= 0.0);
  DBUG_ASSERT(sampling_percentage <= 100.0);

  uint64_t heap_pages = StorageTableInterface::GetTableBlockCount(
      m_dstore->rel_info->rd_storage_releation->tableSmgr);
  int num_block_to_sample =
      static_cast<int>(std::ceil(heap_pages * sampling_percentage / 100.0));

  m_samplerCtx = std::make_unique<CDESampler>(heap_pages, num_block_to_sample,
                                              sampling_seed);
  if (unlikely(m_samplerCtx == nullptr ||
               DBUG_EVALUATE_IF("simulate_sample_init_make_cdesampler_failed",
                                true, false))) {
    HeapInterface::EndScan(m_dstore->dstore_heap_scan);
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
    m_dstore->dstore_heap_scan = nullptr;
    delete ctx;
    return HA_ERR_INTERNAL_ERROR;
  }

  m_samplerCtx->m_currBlockTupleIndex = INT_MAX;

  set_columns_to_decode();
  return 0;
}

/** Get the next record for sampling dstore.
@param[in]  scan_ctx  Scan context of the sampling dstore
@param[in]  buf       buffer to place the read dstore record
@return 0 for success, else one of the HA_cde values in case of error. */
int ha_cde::sample_next(void *scan_ctx, uchar *buf) {
  auto context = static_cast<DSTORE::HeapSampleScanContext *>(scan_ctx);
  DSTORE::HeapTuple *tupleWithLob = nullptr;
  if (m_samplerCtx->m_currBlockTupleIndex >= context->numTuples) {
    if (!m_samplerCtx->CdeSamplerHasMore()) {
      return HA_ERR_END_OF_FILE;
    }

    // release block tuple resource
    for (int tuple_num = 0; tuple_num < context->numTuples &&
                            tuple_num < DSTORE::MAX_ITEM_OFFSET_NUMBER;
         tuple_num++) {
      TupleInterface::DestroyTuple(context->tuples[tuple_num],
                                   CdeFreeMemForDtuple);
      context->tuples[tuple_num] = nullptr;
    }

    while (m_samplerCtx->CdeSamplerHasMore()) {
      context->numTuples = 0;
      context->SetSampleBlockNum(m_samplerCtx->CdeSamplerNext());
      auto status =
          HeapInterface::SampleScan(m_dstore->dstore_heap_scan, context);
      if (DSTORE::DSTORE_SUCC != status) {
        // No more data to read
        return HA_ERR_END_OF_FILE;
      }
      m_samplerCtx->m_currBlockTupleIndex = 0;

      if (context->numTuples > 0) {
        break;
      }
    }
  }

  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx);

  DSTORE::TupleDesc tupleDesc = m_dstore->rel_info->rd_storage_releation->attr;

  DSTORE::HeapTuple *tuple =
      context->tuples[m_samplerCtx->m_currBlockTupleIndex++];
  if (tuple == nullptr) {
    return HA_ERR_END_OF_FILE;
  }
  DSTORE::HeapTuple *realTuple = tuple;
  if (tupleDesc->tdhaslob && m_dstore->m_needDecodeLobColumns) {
    /* For sample next, there will only Consistent Read, so we set
    isConsistentRead ture. */
    tupleWithLob = FetchBlobFiled(m_dstore->rel_info->rd_storage_releation,
                                  tuple, &trx->scanSnapshot, true);
    if (unlikely(tupleWithLob == nullptr)) {
      uint32_t error_code = GetAndConvertDstoreErrcodeToMysql();
      return error_code;
    }
    if (tuple != tupleWithLob) {
      realTuple = tupleWithLob;
    } else {
      tupleWithLob = nullptr;
    }
  }

  m_dstore->DecodeDstoreRow(tupleDesc, m_dstore->m_valuesWithVarlen, realTuple,
                            buf, table, &m_blob_mem_root);
  if (tupleWithLob != nullptr) {
    TupleInterface::DestroyTuple(tupleWithLob, CdeFreeMemForDtuple);
  }
  return 0;
}

/** End dstore sampling.
@param[in] scan_ctx  Scan context of the sampling dstore
@return 0 for success, else one of the HA_cde values in case of error. */
int ha_cde::sample_end(void *scan_ctx) {
  DBUG_TRACE;

  auto context = static_cast<DSTORE::HeapSampleScanContext *>(scan_ctx);

  // release last block tuple resource
  for (int tuple_num = 0; tuple_num < context->numTuples &&
                          tuple_num < DSTORE::MAX_ITEM_OFFSET_NUMBER;
       tuple_num++) {
    TupleInterface::DestroyTuple(context->tuples[tuple_num],
                                 CdeFreeMemForDtuple);
  }

  if (m_dstore->dstore_heap_scan && !CdeIsTransactionMemContextReset()) {
    HeapInterface::EndScan(m_dstore->dstore_heap_scan);
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
  }
  m_dstore->dstore_heap_scan = nullptr;
  m_samplerCtx.reset();
  delete context;
  return 0;
}

/** Tries to check that an CDE table is not corrupted. If corruption is
 noticed, prints to stderr information about it. In case of corruption
 may also assert a failure and crash the server.
 @return HA_ADMIN_CORRUPT or HA_ADMIN_OK */

int ha_cde::check(THD *thd,                /*!< in: user thread handle */
                  HA_CHECK_OPT *check_opt) /*!< in: check options */
{
  (void)thd;
  (void)check_opt;
  return HA_ADMIN_NOT_IMPLEMENTED;
}

Item *ha_cde::idx_cond_push(uint keyno, Item *idx_cond) {
  DBUG_ASSERT(idx_cond != nullptr);

  pushed_idx_cond = idx_cond;
  pushed_idx_cond_keyno = keyno;
  in_range_check_pushed_down = true;
  /* We will evaluate the condition entirely */
  return nullptr;
}

int ha_cde::index_read(uchar *buf, /*!< in/out: buffer for the returned row */
                       const uchar *key_ptr, /*!< in: key value; if this is NULL
                                             we position the cursor at
                                             the start or end of index; this can
                                             also contain an CDE row
                                             id, in which case key_len is the
                                             CDE row id length; the key
                                             value can also be a prefix of a
                                             full key value, and the last
                                             column can be a prefix of a full
                                             column */
                       uint key_len,         /*!< in: key value length */
                       enum ha_rkey_function find_flag) /*!< in: search flags
                                                           from my_base.h */
{
  DBUG_TRACE;
  DEBUG_SYNC_C("ha_cde_index_read_begin");

  Huawei::Common::LatencyCounter::Timer index_read_latency(
      cde_perf_counters::getInstance()->_index_read_latency,
      dstore_enable_perf_timers);

  cde_isolation_level_t isolationLevel =
      CdeTrxMapIsolationLevel(ha_thd()->tx_isolation);
  cde_trxinfo_t *trx = CdeGetTrxinfo(ha_thd());
  updateStatementSnapShot(m_dstore->select_lock_type, trx);

  ha_statistic_increment(&System_status_var::ha_read_key_count);
  cde_dict_t *table_handler = m_dstore->table_handler;
  session_index_info *current_index = m_dstore->current_cde_index;
  cde_dict_index_t *index_dict = current_index->shared_dict;
  if (current_index == nullptr) return 0;
  // for multi range read, index_read will be called multi-times without call
  // index_end, need to cleanup before apply resources.
  index_read_cleanup();

  if (m_dstore->sql_stat_start && !CanReuseColumnsDecoding())
    set_columns_to_decode();

  // set DSTORE::ScanDirection
  DSTORE::ScanDirection direction;
  direction = cde_btree_api::ConvertFindFlagToDir(find_flag);
  m_dstore->direction = direction;

  int64_t ret = CDE_OK;
  uint32_t actual_n_fields_start = m_dstore->keys_prefix_counts;

  if (actual_n_fields_start && key_ptr) {
    key_mysql_to_dstore(table, current_index, actual_n_fields_start, key_ptr,
                        key_len, m_dstore->m_valuesWithVarlen,
                        m_dstore->m_nulls, m_dstore->table_handler->attrFull);
  }

  // handling scenarios where only the leftmost matching key of a composite
  // index is used ie: there is a index key (a,b,c), and only a, b is used
  std::vector<bool> isAsc;
  for (uint32_t i = 0; i < actual_n_fields_start; i++) {
    m_dstore->m_indexColOids[i] = current_index->rel->attr->attrs[i]->atttypid;
    m_dstore->m_indexColCollationOids[i] =
        current_index->rel->attr->attrs[i]->attcollation;
    isAsc.push_back(
        !(current_index->rel->index->indexOption[i] & CDE_INDEX_OPTION_DESC));
  }

  DSTORE::IndexScanHandler *index_scan_handler = nullptr;

  if (index_dict->has_prefix_field()) {
    CdeSetPrefixOid(actual_n_fields_start, current_index,
                    m_dstore->m_indexColOids);
    /* If need to access prefixed field, need to go back to heap table
    due to prefixed field does not include the complete data. */
    if (m_dstore->read_just_key &&
        index_dict->accessColumnsHasPrefixField(
            m_dstore->m_columnsToDecode, m_dstore->m_numColumnsToDecode)) {
      m_dstore->read_just_key = false;
    }
  }

  // find_flag is used only as the condition of the last column in the prefix
  // columns. Conditions of other columns are seen as equals.
  if (actual_n_fields_start) {
    cde_btree_api::BuildKeyInfosForIndexRead(
        m_dstore->m_keyInfos, current_index->rel->attr->attrs,
        m_dstore->m_valuesWithVarlen, m_dstore->m_nulls, actual_n_fields_start,
        find_flag, isAsc);

    /* If null value comparison is invalid, no need to access DStore and no need
    to do rescan. */
    if (cde_btree_api::IsValidNullCmp(m_dstore->m_keyInfos,
                                      actual_n_fields_start) == CDE_FAIL) {
      ret = HA_ERR_END_OF_FILE;
      goto func_exit;
    }
  }

  index_scan_handler =
      IndexInterface::ScanBegin(current_index->rel, current_index->rel->index,
                                actual_n_fields_start, 0, false, true);
  if (index_scan_handler == nullptr) {
    CDE_LOG_ERROR("index_scan_handler can not be null");
    ret = GetDstoreErrcode();
    goto func_exit;
  }

  if (ret) {
    goto func_exit;
  }
  IndexInterface::IndexScanSetSnapshot(index_scan_handler, &trx->scanSnapshot);
  IndexInterface::ScanSetWantItup(index_scan_handler, true);
  m_dstore->dstore_index_scan = index_scan_handler;
  if (key_ptr) {
    DSTORE::RetStatus status =
        IndexInterface::ScanRescan(index_scan_handler, m_dstore->m_keyInfos);
    if (status == DSTORE_FAIL) {
      CDE_LOG_ERROR("IndexInterface::ScanRescan return error: %d", status);
      ret = GetDstoreErrcode();
      goto func_exit;
    }
  }

  if (m_dstore->read_just_key == 0) {
    // Go back to heap scenario
    m_dstore->dstore_heap_scan =
        HeapInterface::CreateHeapScanHandler(table_handler->dstore_relation);
    trx->appendHeapScanHandler((&m_dstore->dstore_heap_scan));
    updateStatementSnapShot(m_dstore->select_lock_type, trx);
    HeapInterface::BeginScan(m_dstore->dstore_heap_scan, &trx->scanSnapshot);
    ret = cde_dml_api::ReadFullRow(
        isolationLevel, buf, table, this, m_dstore, &m_blob_mem_root,
        m_allowBlobMemRootClearForReuse, &trx->scanSnapshot);
  } else {
    // Cover index scenario
    ret = cde_dml_api::ReadIndexOnly(isolationLevel, buf, table, this, m_dstore,
                                     &trx->scanSnapshot);
  }

func_exit:
  index_read_latency.end();
  return ConvertErrcodeToMysql(ret, 0, ha_thd());
}

int ha_cde::index_read_map(uchar *buf, const uchar *key,
                           key_part_map keypart_map,
                           enum ha_rkey_function find_flag) {
  DBUG_TRACE;
  m_dstore->key_map = keypart_map;
  m_dstore->keys_prefix_counts = CdeKeypartMapToN(
      keypart_map, m_dstore->current_cde_index->shared_dict->index_col_num);
  return handler::index_read_map(buf, key, keypart_map, find_flag);
}

int ha_cde::index_read_last_map(uchar *buf, const uchar *key,
                                key_part_map keypart_map) {
  DBUG_TRACE;
  m_dstore->key_map = keypart_map;
  m_dstore->keys_prefix_counts = CdeKeypartMapToN(
      keypart_map, m_dstore->current_cde_index->shared_dict->index_col_num);
  return handler::index_read_last_map(buf, key, keypart_map);
}

/** The following functions works like index_read, but it find the last
 row with the current key value or prefix.
 @return 0, HA_ERR_KEY_NOT_FOUND, or an error code */
int ha_cde::index_read_last(
    uchar *buf,           /*!< out: fetched row */
    const uchar *key_ptr, /*!< in: key value, or a prefix of a full key value */
    uint key_len)         /*!< in: length of the key val or prefix in bytes */
{
  DBUG_TRACE;
  return (index_read(buf, key_ptr, key_len, HA_READ_PREFIX_LAST));
}

/** Delete all rows from the table.
@retval HA_ERR_WRONG_COMMAND if the table is transactional
@retval 0 on success */
int ha_cde::delete_all_rows() { return HA_ERR_WRONG_COMMAND; }

/** This is mapped to "ALTER TABLE tablename ENGINE=Dstore", which rebuilds
 the table in MySQL. */
int ha_cde::optimize(THD *,          /*!< in: connection thread handle */
                     HA_CHECK_OPT *) /*!< in: currently ignored */
{
  return HA_ADMIN_TRY_ALTER;
}

/**
Updates index cardinalities of the table, based on random dives into
each index tree. This does NOT calculate exact statistics on the table.
@return HA_ADMIN_* error code or HA_ADMIN_OK */
int ha_cde::analyze(THD *thd, /*!< in: connection thread handle */
                    HA_CHECK_OPT *check_opt) /*!< in: currently ignored */
{
  (void)thd;
  (void)check_opt;
  Huawei::Common::LatencyCounter::Timer analyze_latency(
      cde_perf_counters::getInstance()->_cde_analyze_latency,
      dstore_enable_perf_timers);
  int ret =
      info_impl(HA_STATUS_TIME | HA_STATUS_CONST | HA_STATUS_VARIABLE, true);
  if (ret != CDE_OK) {
    return HA_ADMIN_FAILED;
  }
  return HA_ADMIN_OK;
}

/** Disable indexes.
@param[in]  mode    disable index mode.
@return HA_ERR_* error code or 0 */
int ha_cde::disable_indexes(uint mode) {
  (void)mode;
  return 0;
}

/** Enable indexes.
@param[in]  mode    enable index mode.
@return HA_ERR_* error code or 0 */
int ha_cde::enable_indexes(uint mode) {
  (void)mode;
  return 0;
}

/** Get storage-engine private data for a data dictionary table.
@param[in,out]  dd_table    data dictionary table definition
@param      reset       reset counters
@retval     true        an error occurred
@retval     false       success */
bool ha_cde::get_se_private_data(dd::Table *dd_table, bool reset) {
  (void)dd_table;
  (void)reset;
  return false;
}

/** Add hidden columns and indexes to an InnoDB table definition.
@param[in,out]  dd_table    data dictionary cache object
@return error number
@retval 0 on success */
int ha_cde::get_extra_columns_and_keys(const HA_CREATE_INFO *,
                                       const List<Create_field> *, const KEY *,
                                       uint, dd::Table *dd_table) {
  (void)dd_table;
  return 0;
}

/** Return max limits for a single set of multi-valued keys
@param[out] num_keys    number of keys to store
@param[out] keys_length total length of keys, bytes
*/
void ha_cde::mv_key_capacity(uint *num_keys, size_t *keys_length) const {
  (void)num_keys;
  (void)keys_length;
}

void ha_cde::index_read_cleanup() {
  CDE_ASSERT(m_dstore != nullptr);
  bool queryCtxReset = false;
  if (m_dstore->dstore_index_scan != nullptr) {
    // add a check to see if query memory ctx has been reset
    cde_session_t *session = CdeGetSession(current_thd);
    auto threadContext = (DSTORE::ThreadContext *)(session->dstore_thrd);
    queryCtxReset = threadContext->GetQueryMemoryContext()->is_reset;
    if (!queryCtxReset) {
      IndexInterface::ScanEnd(m_dstore->dstore_index_scan);
    }
  }
  m_dstore->dstore_index_scan = nullptr;
  if (m_dstore->dstore_heap_scan != nullptr &&
      !CdeIsTransactionMemContextReset()) {
    HeapInterface::EndScan(m_dstore->dstore_heap_scan);
    HeapInterface::DestroyHeapScanHandler(m_dstore->dstore_heap_scan);
  }
  m_dstore->dstore_heap_scan = nullptr;
}

void ha_cde::print_error(int error, myf errflag) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("error: %d", error));

  // handle self-defined HA_DSTORE_ERR error codes.
  if (error > (int)HaDstoreErrE::HA_DSTORE_ERR_FIRST &&
      error < (int)HaDstoreErrE::HA_DSTORE_ERR_LAST) {
    int textno = 0;
    switch (error) {
      case (int)HaDstoreErrE::HA_DSTORE_ERR_INDEX_FAIL_FOR_HUGE_INDEX_TUPLE:
        textno = ER_DSTORE_ERR_INDEX_FAIL_FOR_HUGE_INDEX_TUPLE;
        break;
      case (int)HaDstoreErrE::HA_DSTORE_ERR_TUPLE_TOO_MANY_COLUMNS:
        textno = ER_DSTORE_ERR_TUPLE_TOO_MANY_COLUMNS;
        break;
      case (int)HaDstoreErrE::HA_DSTORE_ERR_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY:
        textno = ER_DSTORE_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY;
        break;
      case (int)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE:
        textno = ER_DSTORE_HIT_DAMAGE_PAGE;
        break;
      case (int)HaDstoreErrE::HA_DSTORE_ERR_TUPLE_TOO_BIG:
        textno = ER_DSTORE_ERR_TUPLE_TOO_BIG;
        my_error(textno, MYF(0), MaxAllocSize);
        return;
      case (int)HaDstoreErrE::HA_DSTORE_ERR_TABLESPACE_FULL:
        textno = ER_DSTORE_ERR_TABLESPACE_REACH_MAXSIZE;
        my_error(textno, MYF(0));
        return;
    }

    if (textno != 0) {
      my_error(textno, errflag, table_share->table_name.str, error);
      return;
    }
  }

  handler::print_error(error, errflag);
}

void ha_cde::set_old_name(const char *oldName [[maybe_unused]]) {
  CDE_ASSERT_DEBUG(nullptr == oldName || 0 != strlen(oldName));
  m_dstore->table_handler->setOldName(oldName);
}

/****************************************************************************
 Multi Range Read interface, DS-MRR calls
 ***************************************************************************/

/**
Initialize multi range read @see DsMrr_impl::dsmrr_init

@param[in] seq_funcs       Range sequence to be traversed
@param[in] seq_init_param  First parameter for seq->init()
@param[in] n_ranges        Number of ranges in the sequence
@param[in] mode            Flags, see the description section for the details
@param[in,out] buf         memory buffer to be used

@return  int  0 for OK, others for error code
*/
int ha_cde::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                  uint n_ranges, uint mode,
                                  HANDLER_BUFFER *buf) {
  m_ds_mrr.init(table);

  return (m_ds_mrr.dsmrr_init(seq, seq_init_param, n_ranges, mode, buf));
}

/**
Process next multi range read @see DsMrr_impl::dsmrr_next

@param[in,out]  buf  Undefined if HA_MRR_NO_ASSOCIATION flag is in
effect.Otherwise, the opaque value associated with the range that contains the
returned record.

@return  int  0 for OK, others for error code
*/
int ha_cde::multi_range_read_next(char **range_info) {
  return (m_ds_mrr.dsmrr_next(range_info));
}

/**
Initialize multi range read and get information. @see
ha_myisam::multi_range_read_info_const @see DsMrr_impl::dsmrr_info_const

@param[in]      keyno           Index number
@param[in]      seq             Range sequence to be traversed
@param[in]      seq_init_param  First parameter for seq->init()
@param[in]      n_ranges_arg    Number of ranges in the sequence, or 0 if the
                                caller can't efficiently determine it
@param[in,out]  bufsz           IN:  Size of the buffer available for use;
                                OUT: Size of the buffer that is expected to be
                                actually used, or 0 if buffer is not needed.
@param[in,out]  flags           A combination of HA_MRR_* flags
@param[out]     force_default_mrr Force default MRR implementation
@param[out]     cost            Estimated cost of MRR access

@return HA_POS_ERROR for Error or the engine is unable to perform the requested
scan. others for OK.
*/
ha_rows ha_cde::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                            void *seq_init_param, uint n_ranges,
                                            uint *bufsz, uint *flags,
                                            bool *force_default_mrr
                                            [[maybe_unused]],
                                            Cost_estimate *cost) {
  /* See comments in ha_myisam::multi_range_read_info_const */
  m_ds_mrr.init(table);

  return (m_ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges, bufsz,
                                    flags, cost));
}

/**
Initialize multi range read and get information. @see DsMrr_impl::dsmrr_info

@param[in]      keyno           Index number
@param[in]      n_ranges        Estimated number of ranges (i.e. intervals) in
the range sequence.
@param[in]      n_rows          Estimated total number of records contained
within all of the ranges
@param[in,out]  bufsz           IN:  Size of the buffer available for use
                                OUT: Size of the buffer that will be actually
used, or 0 if buffer is not needed.
@param[in,out]  flags           A combination of HA_MRR_* flags
@param[out]     cost            Estimated cost of MRR access

@return 0 for OK, other for error or can't perform the requested scan
*/
ha_rows ha_cde::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                      uint *bufsz, uint *flags,
                                      Cost_estimate *cost) {
  m_ds_mrr.init(table);

  return (m_ds_mrr.dsmrr_info(keyno, n_ranges, keys, bufsz, flags, cost));
}

bool ha_cde::CdeIsTransactionMemContextReset() {
  return ThreadContextInterface::IsTransactionMemContextReset();
}

void ha_cde::column_bitmaps_signal() {
  // This function can be called multiple times after ***_open() has been
  // called. For instance, during deletes, this function may be called after
  // ***_init() when the server has detected that it needs certain columns due
  // to triggers.
  set_columns_to_decode();
}

void ha_cde::set_columns_to_decode() {
  if (m_dstore == nullptr) {
    // The function can be called before ::open() has been called, which is
    // responsible for allocating "m_dstore".
    return;
  }

  m_dstore->m_numColumnsToDecode = 0;

  // Updates needs all columns since the full row is written to DStore.
  // For Deletes, the search phase requires the full row to find the exact row.
  // FIXME: This is a temporary fix for
  // rpl_gipk_with_extra_auto_inc_on_replica-dstore and
  // rpl_gipk_on_replica_only-dstore test cases. If replica node has extra
  // column than master node, during applying the DELETE binlog event, dstore
  // handler's rnd_init/rnd_next interface will only retrieve fields those are
  // in master's field list. If there exists index on replica's extra fields,
  // dstore engine will fail to delete rows from those indexes because mysql
  // record sent to delete_row interface doesn't contain these fields. To fix
  // this problem temporarily, if sql_command is DELETE or DELETE_MULTI, always
  // return the whole row. InnoDB engine will always return whole row if it
  // holds a LOCK_X lock, handler team need to investigate more to see if we
  // need to do same here.
  const bool whole_row = ha_thd()->lex->sql_command == SQLCOM_UPDATE ||
                         ha_thd()->lex->sql_command == SQLCOM_UPDATE_MULTI ||
                         ha_thd()->lex->sql_command == SQLCOM_DELETE ||
                         ha_thd()->lex->sql_command == SQLCOM_DELETE_MULTI;

  // We need to keep track of both the column index in the MySQL server and the
  // column index inside DStore; they can diverge in the case of ADD/DROP column
  // with ALGORIGHTM=INSTANT. In particular, DStore keeps columns that have been
  // dropped using ALGORITHM=INSTANT, while they are removed from the MySQL
  // server layer.
  const uint32_t total_cols = m_dstore->table_handler->GetTotalCols();
  const MY_BITMAP *read_set = table->read_set;
  const bool has_virtual_cols = (m_dstore->table_handler->m_vColCount > 0);

  uint16_t mysql_idx = 0;
  uint32_t decoded_count = 0;
  for (uint32_t i = 0; i < total_cols; ++i) {
    if (has_virtual_cols > 0 &&
        m_dstore->table_handler->m_dictCols[i].m_isVirtual) {
      mysql_idx++;
      continue;
    }

    if (mysql_idx == read_set->n_bits) {
      // The read_set may be smaller than the number of columns in some cases,
      // so just bail out if we have reached the end of the read set.
      break;
    }

    // now cause m_columnsToDecode's m_dstoreIndex is not asc, so
    // we remember last column's m_dstoreIndex to pass DeformTuplePart
    if (whole_row || bitmap_is_set(read_set, mysql_idx)) {
      uint16_t dstoreIndex =
          m_dstore->table_handler->m_dictCols[mysql_idx].m_phyPos;
      m_dstore->m_columnsToDecode[decoded_count].m_dstoreIndex = dstoreIndex;
      m_dstore->m_columnsToDecode[decoded_count].m_mysqlIndex = mysql_idx;
      m_dstore->m_deformEndIndex =
          std::max(m_dstore->m_deformEndIndex, dstoreIndex);
      decoded_count++;
    }

    mysql_idx++;
  }
  m_dstore->m_numColumnsToDecode = decoded_count;
  DSTORE::TupleDesc tupleDesc = m_dstore->table_handler->dstore_relation->attr;
  m_dstore->m_needDecodeLobColumns = false;
  if (tupleDesc->tdhaslob) {
    // Checks if LOB columns exist in columns to decode
    for (size_t i = 0; i < m_dstore->m_numColumnsToDecode; ++i) {
      Oid typeOid =
          tupleDesc->attrs[m_dstore->m_columnsToDecode[i].m_dstoreIndex]
              ->atttypid;
      if (typeOid == BLOBOID || typeOid == CLOBOID) {
        m_dstore->m_needDecodeLobColumns = true;
        break;
      }
    }
  }
}

/* The following parameters are defined for DStore guc parameters. @{ */
static MYSQL_SYSVAR_UINT(
    self_node_id, g_guc.selfNodeId, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The ID that identifies each node in the cluster; 0 is an invalid value. "
    "The default starts from 1 (for standalone environments, 0 is not invalid "
    "but setting it to 1 is recommended)",
    nullptr, nullptr, 1, 1, 1024, 0);

static MYSQL_SYSVAR_INT(
    buffer, g_guc.buffer, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The size of the DStore buffer pool in BLOCKs (8kb per block now), which "
    "affects buffer pool, BgDiskPageMasterWriter and BgMemPageMasterWriter",
    nullptr, nullptr, 300000, 256, INT_MAX, 0);

static MYSQL_SYSVAR_INT(buffer_lru_partition, g_guc.bufferLruPartition,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "The number of buffer LRU partition", nullptr, nullptr,
                        100, 1, 4096, 0);

static MYSQL_SYSVAR_INT(buffer_num_of_buf_in_memchunk,
                        g_guc.bufferNumOfBufInMemChunk,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Number of buffers in each memory chunk", nullptr,
                        nullptr, 720896, 131072, 1310720, 0);

static MYSQL_SYSVAR_LONG(check_point_timeout, g_guc.checkpointTimeout,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Timeout value of checkpoint", nullptr, nullptr, 60, 1,
                         3600, 0);

static MYSQL_SYSVAR_UINT(ncores, g_guc.ncores,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "DStore max threads number.", nullptr, nullptr, 5000,
                         256, 200000, 0);

static const char *dstore_log_min_messages_names[] = {
    "DEBUG", "LOG",   "INFO",  "NOTICE", "WARNING",
    "ERROR", "FATAL", "PANIC", NullS};
static TYPELIB dstore_log_min_messages_typelib = {
    array_elements(dstore_log_min_messages_names) - 1,
    "dstore_log_min_messages_typelib", dstore_log_min_messages_names, nullptr};
static MYSQL_SYSVAR_ENUM(log_min_messages, g_logMinMessages,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The minimum log level that can be logged to file",
                         nullptr, nullptr, 2, &dstore_log_min_messages_typelib);

static MYSQL_SYSVAR_INT(
    fold_period, g_guc.foldPeriod, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Log folding interval in seconds. Each time the interval is reached, the "
    "number of repeated logs is compared to the threshold. Setting this to 0 "
    "disables folding",
    nullptr, nullptr, 20, 0, 3600, 0);

static MYSQL_SYSVAR_ENUM(
    fold_level, g_foldLevel, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Logs which level less than or equal to foldLevel will be folded", nullptr,
    nullptr, 4, &dstore_log_min_messages_typelib);

static MYSQL_SYSVAR_BOOL(
    enable_log_file_rotate, g_guc.enableLogFileRotate,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "enable log file rotate, The logic for cleaning up .log "
    "and .log.gz files is also enabled.",
    nullptr, nullptr, false);

static MYSQL_SYSVAR_UINT(lock_hash_table_size, g_guc.lockHashTableSize,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Size of lock hash table", nullptr, nullptr, 256, 1,
                         1000000, 0);

static MYSQL_SYSVAR_UINT(lock_table_partition_num, g_guc.lockTablePartitionNum,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Number of partitioned lock used by lock hash table",
                         nullptr, nullptr, 256, 1, 1000000, 0);

static MYSQL_SYSVAR_BOOL(
    enable_lazy_lock, g_guc.enableLazyLock,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "enable transaction level lazy lock to speed up weak lock", nullptr,
    nullptr, false);

static void UpdateEnableDyamicFlush(THD *, SYS_VAR *, void *,
                                    const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  g_guc.enableDynamicFlush = new_value;
  DSTORE::UpdateDynamicFlush(new_value);
}

static MYSQL_SYSVAR_BOOL(enable_dynamic_flush, g_guc.enableDynamicFlush,
                         PLUGIN_VAR_RQCMDARG, "Enable dynamic flush", nullptr,
                         UpdateEnableDyamicFlush, true);

static void UpdateEnableHeapSarssm(THD *, SYS_VAR *, void *, const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  g_guc.enableHeapSarssm = new_value;
  g_storageInstance->UpdateEnableHeapSarssm(new_value);
}

static MYSQL_SYSVAR_BOOL(enable_heap_sarssm, g_guc.enableHeapSarssm,
                         PLUGIN_VAR_RQCMDARG, "Enable heap sarssm", nullptr,
                         UpdateEnableHeapSarssm, false);

static MYSQL_SYSVAR_STR(vfs_tenant_isolation_config_path,
                        g_guc.vfsTenantIsolationConfigPath,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "VFS config path", nullptr, nullptr, nullptr);

static MYSQL_SYSVAR_UINT(
    num_obj_space_mgr_workers, g_numObjSpaceMgrWorkers,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The number of background threads that performs extension", nullptr,
    nullptr, 1, 1, 10, 0);

static MYSQL_SYSVAR_INT(prob_of_extension_threshold,
                        g_guc.probOfExtensionThreshold,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "The probability of extension threshold", nullptr,
                        nullptr, 0, 0, 1, 0);

static MYSQL_SYSVAR_INT(prob_of_recycle_btree, g_guc.probOfRecycleBtree,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "This parameter specifies the probability of btree "
                        "page recycle when splitting or deletion happens",
                        nullptr, nullptr, 10, 0, 100, 0);

static const char *dstore_wal_level_names[] = {"MINIMAL", "ARCHIVE",
                                               "HOT_STANDBY", "LOGICAL", NullS};
static TYPELIB dstore_wal_level_typelib = {
    array_elements(dstore_wal_level_names) - 1, "dstore_wal_level_typelib",
    dstore_wal_level_names, nullptr};
static MYSQL_SYSVAR_ENUM(wal_level, g_walLevel,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Wal level decide the detail of wal", nullptr, nullptr,
                         0, &dstore_wal_level_typelib);

static MYSQL_SYSVAR_UINT(recovery_worker_num, g_guc.recoveryWorkerNum,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Number of recovery worker", nullptr, nullptr, 1, 1,
                         UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(
    synchronous_commit, g_guc.synchronousCommit,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Controls whether transaction commits wait for log flushing to disk",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_INT(wal_flush_timeout, g_guc.walFlushTimeout,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Controls the frequency of flushing logs to disks",
                        nullptr, nullptr, 0, 0, 90000000, 0);

static MYSQL_SYSVAR_LONG(wal_file_size, g_guc.walFileSize,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Wal segment file size", nullptr, nullptr, 134217728,
                         0, 4294967296, 0);

static MYSQL_SYSVAR_INT(wal_buffers, g_guc.walBuffers,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Wal buffer number", nullptr, nullptr, 16384, 8320,
                        262143, 0);

static MYSQL_SYSVAR_LONG(wal_read_buffer_size, g_guc.walReadBufferSize,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Size of the read buffer for WAL log files in bytes",
                         nullptr, nullptr, 536870912, 67633152, 10737418240, 0);

static MYSQL_SYSVAR_LONG(
    wal_redo_buffer_size, g_guc.walRedoBufferSize,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Variable for the buffer size used during WAL log recovery in bytes",
    nullptr, nullptr, 268435456, 67633152, 10737418240, 0);

static MYSQL_SYSVAR_INT(
    wal_writer_cpu_bind, g_guc.walwriterCpuBind,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Indicates the number of the CPU core to which the thread is bound",
    nullptr, nullptr, 0, -1, INT_MAX, 0);

static MYSQL_SYSVAR_INT(wal_keep_segments, g_guc.walKeepSegments,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "This parameter specifies the minimum number of WAL "
                        "files that can be reserved",
                        nullptr, nullptr, 10, 0, INT_MAX, 0);

static MYSQL_SYSVAR_ULONG(
    bg_wal_writer_min_bytes, g_guc.bgWalWriterMinBytes,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Minimum bytes of WAL to be flushed by each WAL writer thread", nullptr,
    nullptr, 51200, 30, 10448576, 0);

static MYSQL_SYSVAR_ULONG(wal_each_write_lenghth_limit,
                          g_guc.walEachWriteLenghthLimit,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "The limit of communication wal size", nullptr,
                          nullptr, 524288, 0, 1044480, 0);

static MYSQL_SYSVAR_BOOL(enable_wal_record_encode, g_guc.enableWalRecordEncode,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "enable wal record internal encode", nullptr, nullptr,
                         false);

static MYSQL_SYSVAR_BOOL(enable_wal_record_encode_verify,
                         g_guc.enableWalRecordEncodeVerify,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "wal record internal encode enable verify", nullptr,
                         nullptr, false);

static MYSQL_SYSVAR_BOOL(enable_double_write, g_guc.enableDoubleWrite,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "enable dstore double write", nullptr, nullptr, false);

static MYSQL_SYSVAR_BOOL(ignore_double_write_init_error,
                         g_guc.ignoreDoubleWriteMgrError,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "ignore double write mgr initialization error",
                         nullptr, nullptr, false);

static MYSQL_SYSVAR_UINT(double_write_batch_size,
                         g_guc.dblwrSingleFlushGroupBlocks,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "number of blocks in a single flush group", nullptr,
                         nullptr, 30, 0, UINT_MAX, 0);

static MYSQL_SYSVAR_UINT(
    double_write_wait_time, g_guc.dblwrSingleFlushWaitTimes,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "sleep time (us) while waiting for the group thread to flush", nullptr,
    nullptr, 50, 0, 1000000, 0);

static MYSQL_SYSVAR_BOOL(disable_btree_page_recycle,
                         g_guc.disableBtreePageRecycle,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Disable btree page unlink", nullptr, nullptr, false);

static MYSQL_SYSVAR_INT(
    deadlock_time_interval, g_guc.deadlockTimeInterval,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The time to wait on a lock before checking for deadlock in millisecond",
    nullptr, nullptr, 1000, 1, INT_MAX, 0);

static MYSQL_SYSVAR_BOOL(enable_out_of_line_lob, g_guc.enableOutOfLineLob,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enable creation of the LOB extent for dstore",
                         nullptr, nullptr, true);

static MYSQL_SYSVAR_INT(bg_disk_writer_slave_num, g_guc.bgDiskWriterSlaveNum,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Background disk page writer slave thread number",
                        nullptr, nullptr, 4, 1, 16, 0);

static void UpdateBgPageWriterSleepMilliSecond(THD *, SYS_VAR *, void *,
                                               const void *save) {
  int new_value = *static_cast<const int *>(save);
  g_guc.bgPageWriterSleepMilliSecond = new_value;
  DSTORE::UpdateBgPageWriterSleepMilliSecond(new_value);
}

static MYSQL_SYSVAR_INT(bg_page_writer_sleep_millisecond,
                        g_guc.bgPageWriterSleepMilliSecond, PLUGIN_VAR_RQCMDARG,
                        "Background writer sleep time between rounds", nullptr,
                        UpdateBgPageWriterSleepMilliSecond, 2000, 0, 3600000,
                        0);

static const char *dstore_wal_throttling_mode_names[] = {"STRICT", "NORMAL",
                                                         NullS};
static TYPELIB dstore_wal_throttling_mode_typelib = {
    array_elements(dstore_wal_throttling_mode_names) - 1,
    "dstore_wal_throttling_mode_typelib", dstore_wal_throttling_mode_names,
    nullptr};
static MYSQL_SYSVAR_ENUM(
    wal_throttling_mode, g_walThrottlingMode,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Set throttling mode: 0 for strict mode, 1 for normal mode", nullptr,
    nullptr, 0, &dstore_wal_throttling_mode_typelib);

static MYSQL_SYSVAR_UINT(wal_throttling_size, g_guc.walThrottlingSize,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Threshold of the WAL that trigger flow control",
                         nullptr, nullptr, 1048576, 163840, UINT_MAX, 0);

static void UpdateMaxIoCapacityKb(THD *, SYS_VAR *, void *, const void *save) {
  int new_value = *static_cast<const int *>(save);
  g_guc.maxIoCapacityKb = new_value;
  DSTORE::UpdateMaxIoCapacityKb(new_value);
}

static MYSQL_SYSVAR_INT(
    max_io_capacity_kb, g_guc.maxIoCapacityKb, PLUGIN_VAR_RQCMDARG,
    "The I/O upper limit of batch flush dirty page every second", nullptr,
    UpdateMaxIoCapacityKb, 512000, 30720, 10485760, 0);

static MYSQL_SYSVAR_BOOL(enable_aio, g_guc.enableAio,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enable Asynchronous I/O", nullptr, nullptr, true);

static MYSQL_SYSVAR_UINT(aio_callback_thread_num, g_guc.aioCallbackThreadNum,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The number of threads that execute callback function "
                         "of Asynchronous I/O",
                         nullptr, nullptr, 2, 1, 128, 0);

static MYSQL_SYSVAR_UINT(batch_aio_size, g_guc.batchAioSize,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The number of pages each disk writer slave thread "
                         "can process in a batch",
                         nullptr, nullptr, 2048, 1, 1310720, 0);

static MYSQL_SYSVAR_UINT(aio_max_requests, g_guc.aioMaxRequests,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "The maximum number of Asynchronous I/O requests can "
                         "be processed concurrently, it is the nr_events "
                         "parameter passed to io_setup(), should be "
                         "not bigger than fs.aio-max-nr",
                         nullptr, nullptr, 8192, 1, 65535, 0);

static MYSQL_SYSVAR_INT(bulk_read_ring_size, g_guc.bulkReadRingSize,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Size of bulk read buffer ring", nullptr, nullptr,
                        16384, 1, INT_MAX, 0);

static MYSQL_SYSVAR_INT(bulk_write_ring_size, g_guc.bulkWriteRingSize,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Size of bulk write buffer ring", nullptr, nullptr,
                        16384, 1, INT_MAX, 0);

static MYSQL_SYSVAR_STR(tenant_config, g_tenantConfig,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "The config file path of tenants info", nullptr,
                        nullptr, nullptr);

static MYSQL_SYSVAR_INT(wal_archive_interval, g_guc.walArchiveInterval,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Interval of wal archive in seconds", nullptr, nullptr,
                        60, 1, INT_MAX, 0);

static MYSQL_SYSVAR_BOOL(
    keep_wal_stream_not_change, g_guc.keepWalStreamNotChange,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Don't drop wal stream and start a new one when shutdown and restart",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_UINT(
    local_backup_retry_num, g_guc.localBackupRetryNum,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The maximum retry num when localBackup read a valid tableSpace", nullptr,
    nullptr, 10, 0, UINT_MAX, 0);

static MYSQL_SYSVAR_INT(
    csn_thread_bind_cpu, g_guc.csnThreadBindCpu,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "NIC affinitive CPUs, usually belong to a particular NUMA", nullptr,
    nullptr, -1, -1, 1023, 0);

static MYSQL_SYSVAR_BOOL(
    recovery_skip_build_dirty_page_set, g_guc.recoverySkipBuildDirtyPageSet,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Skip build dirty page set and wal record hash table during recovery for "
    "dstore single instance to save memory",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_ULONG(
    recovery_flush_buf_wal_size_interval, g_guc.recoveryFlushBufWalSizeInterval,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Flush dirty buffers everytime this size of wal replayed. Used in local "
    "backup restore only to make restore resumable. Set to 0 to disable it.",
    nullptr, nullptr, 536870912, 0, ULONG_MAX, 0);

static MYSQL_SYSVAR_STR(wal_dir_config_path, g_walDirConfig,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "Wal vfs config path", nullptr, nullptr, nullptr);

static MYSQL_SYSVAR_BOOL(
    end_recovery_on_wal_crc_failure, g_guc.endRecoveryOnWalCrcFailure,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "For instance using normal disk there could be incompletely written wal "
    "block when kill process while it was writing wal block. Treat this as end "
    "of wal during crash recovery",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_BOOL(enable_damage_page_manager,
                         g_guc.enableDamagePageManager,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enable dstore damage page process or not", nullptr,
                         nullptr, false);

static MYSQL_SYSVAR_BOOL(report_error_on_damage_page,
                         g_guc.reportErrorOnDamagePage,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Report error when hit damage page or ignore it",
                         nullptr, nullptr, false);

static MYSQL_SYSVAR_BOOL(enable_lwlock_monitor, g_guc.enableLwlockMonitor,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enable lwlock monitor check lwlock deadlock or not",
                         nullptr, nullptr, true);

static MYSQL_SYSVAR_UINT(
    lwlock_monitor_interval, g_guc.lwlockMonitorInterval,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Interval of threads's lwlock wait state check in second", nullptr, nullptr,
    300, 1, UINT_MAX, 0);

static void UpdateLocalBackupMaxWalSize(THD *, SYS_VAR *, void *,
                                        const void *save) {
  ulong val = (*static_cast<const ulong *>(save));
  g_guc.localBackupMaxWalSize = (uint64_t)val;
  g_storageInstance->UpdateLocalBackupMaxWalSize(g_guc.localBackupMaxWalSize);
}

static MYSQL_SYSVAR_ULONG(
    local_backup_max_wal_size, g_guc.localBackupMaxWalSize, PLUGIN_VAR_RQCMDARG,
    "The upper limit of wal size in full local backup. 0 means no limit.",
    nullptr, UpdateLocalBackupMaxWalSize, 38654705664, 0, ULONG_MAX, 0);

static void UpdateEnableIndexRcr(THD *, SYS_VAR *, void *, const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  g_guc.enableIndexRcr = new_value;
  g_storageInstance->UpdateEnableIndexRcr(new_value);
}

static MYSQL_SYSVAR_BOOL(enable_index_rcr, g_guc.enableIndexRcr,
                         PLUGIN_VAR_RQCMDARG, "Enable dstore index rcr or not",
                         nullptr, UpdateEnableIndexRcr, true);

static MYSQL_SYSVAR_BOOL(
    control_file_multiple_path, g_guc.controlFileMultiplePath,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Whether use dstore_wal_dir_config_path as control file multiple path.",
    nullptr, nullptr, false);

/* @} End of DStore guc.json parameter definitions. */

bool CdeReportErrorOnDamagePage() { return g_guc.reportErrorOnDamagePage; }

bool CdeEnableDamagePageManager() { return g_guc.enableDamagePageManager; }

bool CdeEnableIndexSample() { return dstore_enable_index_sample; }

static MYSQL_SYSVAR_STR(log_path, g_dstoreLogPath, PLUGIN_VAR_READONLY,
                        "the log path for dstore kernel", nullptr, nullptr,
                        mysql_real_data_home);

ulong dstore_relation_cache_size;

static MYSQL_SYSVAR_ULONG(relation_cache_open_instances,
                          dstore_relation_cache_instances,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "The number of relation cache instances", nullptr,
                          nullptr,
                          RelationCacheManager::DEFAULT_MAX_RELATION_CACHES, 1,
                          RelationCacheManager::MAX_RELATION_CACHES, 0);

static MYSQL_THDVAR_ULONG(ddl_threads, PLUGIN_VAR_RQCMDARG,
                          "Maximum number of threads to use for DDL.", nullptr,
                          nullptr, 4, /* Default. */
                          1,          /* Minimum. */
                          64, 0);     /* Maximum. */

static MYSQL_SYSVAR_INT(ddl_buffer_size, g_guc.maintenanceWorkMem,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_RQCMDARG,
                        "Maximum size of memory to use (in KB) for DDL.",
                        nullptr, nullptr, 1024, /* Default. */
                        64,                     /* Minimum. */
                        4194304, 0);            /* Maximum. */

static MYSQL_SYSVAR_BOOL(enable_index_sample, dstore_enable_index_sample,
                         PLUGIN_VAR_RQCMDARG,
                         "Enable or disable index sample scan for Dstore table",
                         nullptr, nullptr, true);

static MYSQL_THDVAR_UINT(tmp_buffer_size, PLUGIN_VAR_RQCMDARG,
                         "number of blocks for tmp buffer.", nullptr, nullptr,
                         2048, /* Default. Keep consistent with InnoDB's tmp
                                  buffer default values. */
                         128,  /* Minimum. */
                         20480, 0); /* Maximum. */

static MYSQL_THDVAR_BOOL(ddl_enable_batch_insert_for_inplace_rebuild,
                         PLUGIN_VAR_RQCMDARG,
                         "Wether use batch insert for inplace rebuild table.",
                         nullptr, nullptr, true);

static MYSQL_SYSVAR_ULONG(
    row_log_buffer_size, g_rowLogBufSize,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Memory row log buffer size for online index creation.", nullptr, nullptr,
    1048576, 65536, 64 << 20, 0);

static MYSQL_SYSVAR_ULONGLONG(
    row_log_file_max_size, g_rowLogFileMaxSize, PLUGIN_VAR_RQCMDARG,
    "Maximum row log file size for online index creation.", nullptr, nullptr,
    128 << 20, 65536, ~0ULL, 0);

#ifndef NDEBUG
static MYSQL_SYSVAR_BOOL(row_log_print, g_rowLogPrint, PLUGIN_VAR_RQCMDARG,
                         "Whether to print row log data into error log.",
                         nullptr, nullptr, false);
#endif

static MYSQL_SYSVAR_UINT(online_ddl_check_data_mode, g_onlineDdlCheckDataMode,
                         PLUGIN_VAR_RQCMDARG,
                         "The mode for checking the consistency of heap and "
                         "index data after using online ddl to create index. "
                         "0: No check, 1: Check only after build index, 2: "
                         "Check only after all row log replayed, 3: Check "
                         "in all stage. 4: Check only incremental data",
                         nullptr, nullptr, (uint)OnlineDdlCheckMode::INC_CHECK,
                         0, 4, 0);

static MYSQL_SYSVAR_BOOL(online_ddl_enable, g_onlineDdlEnable,
                         PLUGIN_VAR_RQCMDARG, "Enable online ddl.", nullptr,
                         nullptr, true);

static MYSQL_SYSVAR_BOOL(
    online_create_functional_index_enable, g_onlineAddIndexOnNewVcolEnable,
    PLUGIN_VAR_RQCMDARG,
    "Enable online add index on newly added virtual columns.", nullptr, nullptr,
    true);

uint32 ThdTmpBufferSize(THD *thd) noexcept {
  return THDVAR(thd, tmp_buffer_size);
}

size_t ThdDdlThreads(THD *thd) noexcept { return THDVAR(thd, ddl_threads); }

bool ThdEnableBatchInsertForRebuild(THD *thd) noexcept {
  return THDVAR(thd, ddl_enable_batch_insert_for_inplace_rebuild);
}

static void UpdateExpandKeyLenByTD(THD *, SYS_VAR *, void *, const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  g_guc.expandKeyLenByTD = new_value;
  g_storageInstance->UpdateExpandKeyLenByTD(new_value);
}

static MYSQL_SYSVAR_BOOL(expand_key_len_by_td, g_guc.expandKeyLenByTD,
                         PLUGIN_VAR_RQCMDARG,
                         "Enable the creation of indexes with a length of "
                         "910-2560 bytes in Dstore table",
                         nullptr, UpdateExpandKeyLenByTD, true);

bool CdeExpandKeyLenByTD() { return g_guc.expandKeyLenByTD; }

static void fix_relation_cache_size(
    THD *,        /*!< in: thread handle */
    SYS_VAR *,    /*!< in: pointer to system variable */
    void *,       /*!< out: where the formal string goes */
    const void *) /*!< in: immediate result from check function */
{
  dstore_relation_cache_size_per_instance =
      dstore_relation_cache_size / dstore_relation_cache_instances;
}

static MYSQL_SYSVAR_ULONG(relation_open_cache, dstore_relation_cache_size,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "The number of cached open relations "
                          "(total for all relation cache instances)",
                          nullptr, fix_relation_cache_size,
                          RelationCacheManager::DEFAULT_MAX_RELATION_CACHES, 1,
                          RelationCacheManager::MAX_RELATION_CACHES, 0);

static MYSQL_SYSVAR_ULONGLONG(
    stats_transient_sample_pages, dstore_stats_transient_sample_pages,
    PLUGIN_VAR_RQCMDARG,
    "The number of heap pages to sample when calculating transient"
    " statistics (if persistent statistics are not used, default 100)",
    nullptr, nullptr, 100, 1, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    stats_transient_index_sample_pages,
    dstore_stats_transient_index_sample_pages, PLUGIN_VAR_RQCMDARG,
    "The number of leaf index pages to sample when calculating transient"
    " statistics (if persistent statistics are not used, default 20)",
    nullptr, nullptr, 20, 1, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(
    stats_persistent, dstore_stats_persistent, PLUGIN_VAR_OPCMDARG,
    "cde persistent statistics enabled for all tables unless overridden"
    " at table level",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_BOOL(
    stats_auto_recalc, dstore_stats_auto_recalc, PLUGIN_VAR_OPCMDARG,
    "cde automatic recalculation of persistent statistics enabled for all"
    " tables unless overridden at table level (automatic recalculation is only"
    " done when cde decides that the table has changed too much and needs a"
    " new statistics)",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_ULONGLONG(
    stats_persistent_sample_pages, dstore_stats_persistent_sample_pages,
    PLUGIN_VAR_RQCMDARG,
    "The number of heap pages to sample when calculating persistent"
    " statistics (by ANALYZE, default 100)",
    nullptr, nullptr, 100, 1, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    stats_persistent_index_sample_pages,
    dstore_stats_persistent_index_sample_pages, PLUGIN_VAR_RQCMDARG,
    "The number of leaf index pages to sample when calculating persistent"
    " statistics (by ANALYZE, default 20)",
    nullptr, nullptr, 20, 1, ~0ULL, 0);

static MYSQL_SYSVAR_ENUM(
    default_row_format, cde_default_row_format, PLUGIN_VAR_RQCMDARG,
    "The default ROW FORMAT for all cde tables created without explicit"
    " ROW_FORMAT. Possible values are REDUNDANT and COMPACT.",
    nullptr, nullptr, DSTORE_ROW_FORMAT_COMPACT,
    &cde_default_row_format_typelib);

static MYSQL_SYSVAR_UINT(
    autoinc_lock_mode, g_autoincLockMode,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "The AUTOINC lock modes supported by Dstore:"
    " 0 => Old style AUTOINC locking (for backward compatibility);"
    " 1 => New style AUTOINC locking;"
    " 2 => No AUTOINC locking (unsafe for SBR)",
    nullptr, nullptr, AUTOINC_LOCK_INTERLEAVED, AUTOINC_LOCK_TRADITIONAL,
    AUTOINC_LOCK_INTERLEAVED, 0);

static MYSQL_SYSVAR_INT(
    default_fillfactor, g_defaultFillfactor, PLUGIN_VAR_RQCMDARG,
    "The fillfactor option for dstore heap determines how full"
    " the space method will try to pack heap pages. Valid values"
    " are between 10 and 100, default value is 80.",
    nullptr, nullptr, DEFAULT_FILLFACTOR, 10, 100, 0);

static MYSQL_SYSVAR_INT(
    default_fillfactor_for_index, g_defaultFillfactorForIndex,
    PLUGIN_VAR_RQCMDARG,
    "The fillfactor option for dstore index determines how full"
    " the space method will try to pack index pages. Valid values"
    " are between 10 and 100, default value is 80.",
    nullptr, nullptr, DEFAULT_FILLFACTOR_FOR_INDEX, 10, 100, 0);

static MYSQL_SYSVAR_BOOL(
    open_restore_mode, dstore_open_restore_mode, PLUGIN_VAR_READONLY,
    "Whether the Restore mode enable and dstore crash recovery running"
    " in the mode.",
    nullptr, nullptr, false);

static MYSQL_SYSVAR_BOOL(can_do_wal_archive, dstore_can_do_wal_archive,
                         PLUGIN_VAR_READONLY,
                         "Whether can do wal archive for cde.", nullptr,
                         nullptr, false);

static MYSQL_SYSVAR_STR(local_backup_meta_path, dstore_local_backup_meta_path,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "the local backup meta path for dstore", nullptr,
                        nullptr, nullptr);

static MYSQL_SYSVAR_STR(restore_meta_path, dstore_restore_meta_path,
                        PLUGIN_VAR_READONLY,
                        "the restore meta path during restoring on target node",
                        nullptr, nullptr, nullptr);

static MYSQL_SYSVAR_ULONG(local_backup_progress_size_interval,
                          dstore_local_backup_progress_size_interval,
                          PLUGIN_VAR_RQCMDARG,
                          "Size interval of local backup progress update.",
                          nullptr, nullptr, 209715200, 1, ULONG_MAX, 0);

static MYSQL_SYSVAR_BOOL(meta_json_crc, dstore_meta_json_crc,
                         PLUGIN_VAR_RQCMDARG,
                         "Whether add CRC32 checksum to meta json file.",
                         nullptr, nullptr, true);

static MYSQL_SYSVAR_UINT(local_full_backup_sync_interval,
                         dstore_local_full_backup_sync_interval,
                         PLUGIN_VAR_RQCMDARG,
                         "Sync interval for full local backup, after this "
                         "value times write, do a sync operation.",
                         nullptr, nullptr, 50, 0, UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(enable_perf_timers, dstore_enable_perf_timers,
                         PLUGIN_VAR_OPCMDARG,
                         "Whether performance timers (instrumentation) should "
                         "be enabled in DStore handler",
                         nullptr, nullptr, false);

static const char *dstore_perf_level[] = {"PERF_CLOSE", "PERF_DEBUG", "RELEASE",
                                          "OFF", NullS};
static TYPELIB dstore_perf_level_typelib = {
    array_elements(dstore_perf_level) - 1, "dstore_perf_level_typelib",
    dstore_perf_level, nullptr};

static MYSQL_SYSVAR_ENUM(perf_level, g_perfLevel,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Level of perfcounters in Dstore", nullptr, nullptr, 3,
                         &dstore_perf_level_typelib);

static MYSQL_SYSVAR_UINT(perf_counter_interval, g_perfCounterInterval,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "PerfCounter reset interval", nullptr, nullptr, 30, 0,
                         UINT8_MAX, 0);

static void UpdateLastAccessMode(THD *, SYS_VAR *, void *var_ptr,
                                 const void *save) {
  const int new_lastAccessMode = *static_cast<const int *>(save);
  g_storageInstance->UpdateLastAccessMode(new_lastAccessMode);
  *static_cast<int *>(var_ptr) = new_lastAccessMode;
}

static const char *dstore_last_access_mode[] = {"DELETE", "INSERT", NullS};

static TYPELIB dstore_last_access_mode_typelib = {
    array_elements(dstore_last_access_mode) - 1,
    "dstore_last_access_mode_typelib", dstore_last_access_mode, nullptr};

static MYSQL_SYSVAR_ENUM(last_access_mode, g_lastAccessMode,
                         PLUGIN_VAR_RQCMDARG,
                         "Last access mode for heap insert in Dstore", nullptr,
                         UpdateLastAccessMode, 1,
                         &dstore_last_access_mode_typelib);

static const char *dstore_flush_data_method[] = {"O_DIRECT",
                                                 "O_DIRECT_NO_FSYNC", NullS};
static TYPELIB dstore_flush_data_method_typelib = {
    array_elements(dstore_flush_data_method) - 1,
    "dstore_flush_data_method_typelib", dstore_flush_data_method, nullptr};

static MYSQL_SYSVAR_ENUM(flush_data_method, g_flushDataMethod,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Method of flushing data", nullptr, nullptr, 0,
                         &dstore_flush_data_method_typelib);

static MYSQL_SYSVAR_BOOL(
    calculate_exact_rows_for_range, g_calculateExactRowsForRange,
    PLUGIN_VAR_OPCMDARG,
    "Calculate exact rows for range of Dstore table, used for "
    "records_in_range interface",
    nullptr, nullptr, false);

static void fix_enable_standby_role(
    THD *,           /*!< in: thread handle */
    SYS_VAR *,       /*!< in: pointer to system variable */
    void *,          /*!< out: where the formal string goes */
    const void *val) /*!< in: immediate result from check function */
{
  g_guc.enableStandbyRole = *static_cast<const bool *>(val);
}

static MYSQL_SYSVAR_BOOL(
    enable_standby_role, g_guc.enableStandbyRole, PLUGIN_VAR_RQCMDARG,
    "Used to distinguish between the master and standby roles.", nullptr,
    fix_enable_standby_role, false);

static MYSQL_SYSVAR_BOOL(exit_after_restore, dstore_exit_after_restore,
                         PLUGIN_VAR_READONLY,
                         "Exit process after dstore finish restore", nullptr,
                         nullptr, false);

static void update_dstore_lock_wait_timeout(THD *, SYS_VAR *, void *,
                                            const void *save) {
  ulong val = (*static_cast<const ulong *>(save));
  dstore_lock_wait_timeout = val;

  g_storageInstance->RegisterGetLockWaitTimeoutCallback([]() -> int {
    return dstore_lock_wait_timeout * SECONDS_TO_MILLISECONDS;
  });
}

static void update_dstore_wal_file_size_hwt(THD *, SYS_VAR *, void *,
                                            const void *save) {
  ulong val = (*static_cast<const ulong *>(save));
  g_guc.walFileSizeHwt = (uint64_t)val;

  g_storageInstance->RegisterGetWalFileSizeHwtCallback(
      []() -> uint64_t { return g_guc.walFileSizeHwt; });
}

static int check_dstore_wal_file_size_hwt(THD *, SYS_VAR *, void *save,
                                          struct st_mysql_value *value) {
  long long newVal;
  value->val_int(value, &newVal);

  uint64_t walFileSizeHwtMin = 0;
  if (!CheckWalFileSizeHwtValid((uint64_t)newVal, walFileSizeHwtMin)) {
    CDE_LOG_ERROR(
        "set guc failed, dstore_wal_file_size_hwt (%lld) must be larger or "
        "equal than %lu",
        newVal, walFileSizeHwtMin);
    *(long long *)save = newVal;
    return 1;
  }

  *(long long *)save = newVal;
  return 0;
}

/** Max dstore lock wait timeout to avoid interger overflow */
constexpr uint32_t max_dstore_lock_wait_timeout =
    INT_MAX / SECONDS_TO_MILLISECONDS;

static MYSQL_SYSVAR_ULONG(lock_wait_timeout, dstore_lock_wait_timeout,
                          PLUGIN_VAR_RQCMDARG,
                          "Timeout in seconds a DStore transaction may wait "
                          "for a lock before being rolled back. If value == 0, "
                          "we disable the timeout.",
                          nullptr, update_dstore_lock_wait_timeout, 50, 0,
                          max_dstore_lock_wait_timeout, 0);

static MYSQL_SYSVAR_BOOL(rollback_on_timeout, dstore_rollback_on_timeout,
                         PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                         "Roll back the complete transaction on lock wait "
                         "timeout. It is disabled by default)",
                         nullptr, nullptr, false);

static MYSQL_SYSVAR_BOOL(print_ddl_logs, g_printDdlLog, PLUGIN_VAR_RQCMDARG,
                         "Whether print DDl logs to MySQL error log", nullptr,
                         nullptr, true);
// deprecated later
static MYSQL_SYSVAR_ULONG(
    wal_file_size_hwt, g_guc.walFileSizeHwt,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "If the size of walfile exceeds this parameter, txn will be restricted "
    "so that the accumulation of walfiles can be reduced.",
    check_dstore_wal_file_size_hwt, update_dstore_wal_file_size_hwt, 0, 0,
    ULONG_MAX, 0);

/** Range of max_file_size is 1TB~32TB  **/
static MYSQL_SYSVAR_ULONG(max_file_size, g_guc.maxFileSize,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Max size of dstore data file.", nullptr, nullptr,
                          35184372088832, 1099511627776, 35184372088832, 0);

static void UpdateDdlLogFillFactorHeap(THD *, SYS_VAR *, void *,
                                       const void *save) {
  int32_t val = (*static_cast<const int32_t *>(save));
  g_ddlLogFillFactorHeap = val;
  CdeDdlLog::GetInstance()->LoadFillFactor();
}

static MYSQL_SYSVAR_INT(ddl_log_fillfactor_heap, g_ddlLogFillFactorHeap,
                        PLUGIN_VAR_RQCMDARG,
                        "The fillfactor of heap of ddl log table, valid values"
                        " are between 10 and 100, default value is 60.",
                        nullptr, UpdateDdlLogFillFactorHeap, 60, 10, 100, 0);

static void UpdateDdlLogFillFactorIndex(THD *, SYS_VAR *, void *,
                                        const void *save) {
  int32_t val = (*static_cast<const int32_t *>(save));
  g_ddlLogFillFactorIndex = val;
  CdeDdlLog::GetInstance()->LoadFillFactor();
}

static MYSQL_SYSVAR_ULONG(table_definition_cache, g_dstoreTableDefinitionCache,
                          PLUGIN_VAR_RQCMDARG,
                          "The number of cached table definitions.", nullptr,
                          nullptr, 400, 400, 512 * 1024, 0);

static MYSQL_SYSVAR_INT(
    ddl_log_fillfactor_index, g_ddlLogFillFactorIndex, PLUGIN_VAR_RQCMDARG,
    "The fillfactor of indexes of ddl log table, valid values"
    " are between 10 and 100, default value is 50.",
    nullptr, UpdateDdlLogFillFactorIndex, 50, 10, 100, 0);

static MYSQL_SYSVAR_BOOL(
    use_default_template_pdb, g_useDefaultTemplatePDB, PLUGIN_VAR_RQCMDARG,
    "Whether use default template pdb or user defined pdb.", nullptr, nullptr,
    false);

static MYSQL_SYSVAR_UINT(max_reflush_times, g_guc.maxReflushTimes,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Max retry times if a page need to wait for flush.",
                         nullptr, nullptr, 3, 0, 8, 0);

static MYSQL_SYSVAR_ULONG(page_reflush_plsn_gap, g_guc.pageReflushPlsnGap,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "When flushing a page, if the gap between plsn of"
                          " it and maxFlushedPlsn is large than this parameter,"
                          " the page will be skipped and flush later.",
                          nullptr, nullptr, 0, 0, ULONG_MAX, 0);

static MYSQL_SYSVAR_UINT(recovery_page_reader_num, g_guc.recoveryPageReaderNum,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Number of read-page-ahead threads for recovery",
                         nullptr, nullptr, 8, 0, 32, 0);

static MYSQL_SYSVAR_ULONG(
    recovery_page_read_ahead_wal_size_interval,
    g_guc.recoveryPageReadAheadWalSizeInterval,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Read pages to be modified in parallel everytime this size of wal replayed "
    "during recovery.",
    nullptr, nullptr, 1024, 0, INT_MAX, 0);

static MYSQL_SYSVAR_UINT(
    recovery_read_page_worker_set_interval, g_guc.readPageWorkerSetInterval,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "When the size of page id set in read page worker is "
    "smaller than this paramter,"
    "the worker thread will wait to get lock during recovery.",
    nullptr, nullptr, 1, 1, 1024, 0);

static MYSQL_SYSVAR_UINT(
    wal_recovery_dirty_page_flusher_num, g_guc.walRecoveryDirtyPageFlusherNum,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Number of workers used to flush dirty page for wal recovery.", nullptr,
    nullptr, 14, 0, 32, 0);

static MYSQL_SYSVAR_ULONG(gap_flush_wal_length, g_guc.gapFlushWalLength,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "When flushing a page, if the gap between plsn of"
                          " it and maxFlushedPlsn is large than this parameter,"
                          " the page will be skipped and flush later.",
                          nullptr, nullptr, 134217728, 0, 10737418240, 0);

static MYSQL_SYSVAR_BOOL(
    enable_ddl_trx_extral_wal_size_check, s_checkDdlTrxExtralWalSize,
    PLUGIN_VAR_RQCMDARG,
    "Whether check extral wal size(to assert it's not zero) when execute DDL.",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_ULONG(
    max_wal_bytes_after_checkpoint, g_guc.maxWalBytesAfterCheckpoint,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Used to set the threshold value of wal bytes after checkpoint.", nullptr,
    nullptr, 134217728, 1, 53687091200, 0);

static MYSQL_SYSVAR_ULONG(
    max_wal_extra_retention_bytes, g_guc.maxWalExtraRetentionbytes,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    " Used to set the threshold value of wal extra retention bytes.", nullptr,
    nullptr, 20971520, 1, 53687091200, 0);

static MYSQL_SYSVAR_INT(
    checkpoint_interval_time, g_guc.checkpointIntervalTime,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Used to set the checkpoint interval time in milliseconds.", nullptr,
    nullptr, 100, 1, INT_MAX, 0);

static MYSQL_SYSVAR_ULONG(
    recovery_create_checkpoint_interval, g_guc.recoveryCreateCheckpointInterval,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "In the process of crash recovery, if the gap between replyed wal"
    " and flushed wal is large than this parameter,"
    " the checkpoint will be created.",
    nullptr, nullptr, 67108864, 0, 1073741824, 0);

void update_btree_trace_switch(THD *,     /*!< in: thread handle */
                               SYS_VAR *, /*!< in: pointer to system variable */
                               void *, /*!< out: where the formal string goes */
                               const void *val) /*!< in: immediate result from
                                                   check function */
{
  bool new_value = *static_cast<const bool *>(val);
  g_enable_btree_trace = new_value;
  new_value ? DSTORE::OpenBtreeStatisticsTraceSwitch()
            : DSTORE::CloseBtreeStatisticsTraceSwitch();
}

static MYSQL_SYSVAR_BOOL(enable_btree_trace, g_enable_btree_trace,
                         PLUGIN_VAR_RQCMDARG,
                         "Used to open btree statistics trace switch.", nullptr,
                         update_btree_trace_switch, false);

static MYSQL_THDVAR_ULONG(
    parallel_read_threads, PLUGIN_VAR_RQCMDARG,
    "Number of threads to do parallel read, set 0 to disable parallel read.",
    nullptr, nullptr, 4, /* Default. */
    0,                   /* Minimum. */
    128,                 /* Maximum. */
    0);

static MYSQL_SYSVAR_BOOL(
    enable_physical_replication, g_guc.enablePhysicalReplication,
    PLUGIN_VAR_READONLY,
    "Used to distinguish whether it is physical replication.", nullptr, nullptr,
    false);

ulong thd_parallel_read_threads(THD *thd) {
  return THDVAR(thd, parallel_read_threads);
}

static SYS_VAR *cde_system_variables[] = {
    /* guc parameters start@{ */
    MYSQL_SYSVAR(self_node_id), MYSQL_SYSVAR(buffer),
    MYSQL_SYSVAR(buffer_lru_partition),
    MYSQL_SYSVAR(buffer_num_of_buf_in_memchunk),
    MYSQL_SYSVAR(check_point_timeout), MYSQL_SYSVAR(ncores),
    MYSQL_SYSVAR(log_min_messages), MYSQL_SYSVAR(fold_period),
    MYSQL_SYSVAR(fold_level), MYSQL_SYSVAR(enable_log_file_rotate),
    MYSQL_SYSVAR(lock_hash_table_size), MYSQL_SYSVAR(lock_table_partition_num),
    MYSQL_SYSVAR(enable_lazy_lock), MYSQL_SYSVAR(enable_heap_sarssm),
    MYSQL_SYSVAR(enable_dynamic_flush),
    MYSQL_SYSVAR(vfs_tenant_isolation_config_path),
    MYSQL_SYSVAR(num_obj_space_mgr_workers),
    MYSQL_SYSVAR(prob_of_recycle_btree),
    MYSQL_SYSVAR(prob_of_extension_threshold), MYSQL_SYSVAR(wal_level),
    MYSQL_SYSVAR(recovery_worker_num), MYSQL_SYSVAR(synchronous_commit),
    MYSQL_SYSVAR(wal_file_size), MYSQL_SYSVAR(wal_buffers),
    MYSQL_SYSVAR(wal_flush_timeout), MYSQL_SYSVAR(wal_read_buffer_size),
    MYSQL_SYSVAR(wal_redo_buffer_size), MYSQL_SYSVAR(wal_writer_cpu_bind),
    MYSQL_SYSVAR(wal_keep_segments), MYSQL_SYSVAR(bg_wal_writer_min_bytes),
    MYSQL_SYSVAR(wal_each_write_lenghth_limit),
    MYSQL_SYSVAR(enable_wal_record_encode),
    MYSQL_SYSVAR(enable_wal_record_encode_verify),
    MYSQL_SYSVAR(disable_btree_page_recycle),
    MYSQL_SYSVAR(deadlock_time_interval), MYSQL_SYSVAR(enable_out_of_line_lob),
    MYSQL_SYSVAR(bg_disk_writer_slave_num),
    MYSQL_SYSVAR(bg_page_writer_sleep_millisecond),
    MYSQL_SYSVAR(wal_throttling_mode), MYSQL_SYSVAR(wal_throttling_size),
    MYSQL_SYSVAR(max_io_capacity_kb), MYSQL_SYSVAR(enable_aio),
    MYSQL_SYSVAR(aio_callback_thread_num), MYSQL_SYSVAR(aio_max_requests),
    MYSQL_SYSVAR(batch_aio_size), MYSQL_SYSVAR(bulk_read_ring_size),
    MYSQL_SYSVAR(bulk_write_ring_size), MYSQL_SYSVAR(tenant_config),
    MYSQL_SYSVAR(wal_archive_interval),
    MYSQL_SYSVAR(keep_wal_stream_not_change),
    MYSQL_SYSVAR(local_backup_retry_num), MYSQL_SYSVAR(csn_thread_bind_cpu),
    MYSQL_SYSVAR(recovery_skip_build_dirty_page_set),
    MYSQL_SYSVAR(recovery_flush_buf_wal_size_interval),
    MYSQL_SYSVAR(wal_dir_config_path),
    MYSQL_SYSVAR(end_recovery_on_wal_crc_failure),
    MYSQL_SYSVAR(enable_damage_page_manager),
    MYSQL_SYSVAR(report_error_on_damage_page), MYSQL_SYSVAR(wal_file_size_hwt),
    MYSQL_SYSVAR(local_backup_max_wal_size), MYSQL_SYSVAR(max_file_size),
    MYSQL_SYSVAR(enable_index_rcr), MYSQL_SYSVAR(control_file_multiple_path),
    MYSQL_SYSVAR(enable_double_write),
    MYSQL_SYSVAR(ignore_double_write_init_error),
    MYSQL_SYSVAR(double_write_batch_size), MYSQL_SYSVAR(double_write_wait_time),

    /* @} guc parameters end */
    MYSQL_SYSVAR(open_restore_mode), MYSQL_SYSVAR(can_do_wal_archive),
    MYSQL_SYSVAR(local_backup_meta_path), MYSQL_SYSVAR(restore_meta_path),
    MYSQL_SYSVAR(local_backup_progress_size_interval),
    MYSQL_SYSVAR(meta_json_crc), MYSQL_SYSVAR(local_full_backup_sync_interval),
    MYSQL_SYSVAR(enable_perf_timers), MYSQL_SYSVAR(perf_level),
    MYSQL_SYSVAR(perf_counter_interval), MYSQL_SYSVAR(last_access_mode),
    MYSQL_SYSVAR(flush_data_method), MYSQL_SYSVAR(log_path),
    MYSQL_SYSVAR(stats_transient_sample_pages),
    MYSQL_SYSVAR(stats_persistent_sample_pages), MYSQL_SYSVAR(stats_persistent),
    MYSQL_SYSVAR(stats_transient_index_sample_pages),
    MYSQL_SYSVAR(stats_persistent_index_sample_pages),
    MYSQL_SYSVAR(stats_auto_recalc), MYSQL_SYSVAR(default_row_format),
    MYSQL_SYSVAR(autoinc_lock_mode), MYSQL_SYSVAR(default_fillfactor),
    MYSQL_SYSVAR(default_fillfactor_for_index),
    MYSQL_SYSVAR(calculate_exact_rows_for_range),
    MYSQL_SYSVAR(enable_standby_role),
    MYSQL_SYSVAR(relation_cache_open_instances),
    MYSQL_SYSVAR(relation_open_cache), MYSQL_SYSVAR(exit_after_restore),
    MYSQL_SYSVAR(lock_wait_timeout), MYSQL_SYSVAR(rollback_on_timeout),
    MYSQL_SYSVAR(print_ddl_logs), MYSQL_SYSVAR(ddl_log_fillfactor_heap),
    MYSQL_SYSVAR(ddl_log_fillfactor_index),
    MYSQL_SYSVAR(use_default_template_pdb),
    MYSQL_SYSVAR(table_definition_cache), MYSQL_SYSVAR(max_reflush_times),
    MYSQL_SYSVAR(page_reflush_plsn_gap),
    MYSQL_SYSVAR(enable_ddl_trx_extral_wal_size_check),
    MYSQL_SYSVAR(max_wal_bytes_after_checkpoint),
    MYSQL_SYSVAR(checkpoint_interval_time),
    MYSQL_SYSVAR(max_wal_extra_retention_bytes),
    MYSQL_SYSVAR(enable_lwlock_monitor), MYSQL_SYSVAR(lwlock_monitor_interval),
    MYSQL_SYSVAR(recovery_page_reader_num),
    MYSQL_SYSVAR(recovery_page_read_ahead_wal_size_interval),
    MYSQL_SYSVAR(recovery_read_page_worker_set_interval),
    MYSQL_SYSVAR(wal_recovery_dirty_page_flusher_num),
    MYSQL_SYSVAR(gap_flush_wal_length),
    MYSQL_SYSVAR(recovery_create_checkpoint_interval),
    MYSQL_SYSVAR(ddl_threads), MYSQL_SYSVAR(ddl_buffer_size),
    MYSQL_SYSVAR(ddl_enable_batch_insert_for_inplace_rebuild),
    MYSQL_SYSVAR(enable_btree_trace), MYSQL_SYSVAR(enable_index_sample),
    MYSQL_SYSVAR(parallel_read_threads), MYSQL_SYSVAR(expand_key_len_by_td),
    MYSQL_SYSVAR(row_log_buffer_size), MYSQL_SYSVAR(row_log_file_max_size),
#ifndef NDEBUG
    MYSQL_SYSVAR(row_log_print),
#endif
    MYSQL_SYSVAR(online_ddl_check_data_mode), MYSQL_SYSVAR(online_ddl_enable),
    MYSQL_SYSVAR(online_create_functional_index_enable),
    MYSQL_SYSVAR(tmp_buffer_size), MYSQL_SYSVAR(enable_physical_replication),
    nullptr};

struct st_mysql_storage_engine cde_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static SHOW_VAR DstoreStatusVariables[] = {
    {"local_backup_full_backup_status",
     (char *)&g_statusExport.localBackupFullBackupStatus, SHOW_CHAR,
     SHOW_SCOPE_GLOBAL},
    {"dirty_page_cnt", (char *)&g_statusExport.dstoreDirtyPageCnt, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"page_flush_cnt", (char *)&g_statusExport.dstorePageFlushCnt, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"page_get_cnt", (char *)&g_statusExport.dstorePagesGetCnt, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"page_hit_cnt", (char *)&g_statusExport.dstorePagesHitCnt, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"cr_buffer_get_cnt", (char *)&g_statusExport.dstoreCrBufferGetCnt,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"cr_buffer_hit_cnt", (char *)&g_statusExport.dstoreCrBufferHitCnt,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"max_flushed_finish_plsn", (char *)&g_statusExport.maxFlushFinishPlsn,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"max_appended_plsn", (char *)&g_statusExport.maxAppendedPlsn, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"max_written_to_file_plsn", (char *)&g_statusExport.maxWrittenToFilePlsn,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"recovery_plsn_for_dirty_page",
     (char *)&g_statusExport.pageWriteRecoveryPlsn, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"recovery_plsn_for_taurus", (char *)&g_statusExport.recoveryPlsnForTaurus,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"archive_plsn", (char *)&g_statusExport.archivePlsn, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"backedUp_plsn", (char *)&g_statusExport.backedUpPlsn, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"physical_standby_flushed_plsn",
     (char *)&g_statusExport.minWalStandbyFlushedPlsn, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"binlog_max_sync_plsn", (char *)&g_statusExport.binlogMaxSyncPlsn,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"wal_ckpt_timestamp", (char *)&g_statusExport.dstoreCkptTime, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"wal_ckpt_disk_recovery_plsn", (char *)&g_statusExport.dstoreDiskCkpt,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"lwlock_blocked_count", (char *)&g_statusExport.lwlockBlockedCount,
     SHOW_INT, SHOW_SCOPE_GLOBAL},
    {"double_write_init_normal",
     (char *)&g_statusExport.isDoubleWriteInitNormal, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_FUNC, SHOW_SCOPE_GLOBAL}};

static int ShowDstoreVars(THD *, SHOW_VAR *var, char *) {
  DstoreExportStatus();

  var->type = SHOW_ARRAY;
  var->value = (char *)&DstoreStatusVariables;
  var->scope = SHOW_SCOPE_GLOBAL;

  return 0;
}

static SHOW_VAR DstoreShowVariables[] = {
    {"Dstore", (char *)&ShowDstoreVars, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_FUNC, SHOW_SCOPE_GLOBAL}};

} /* namespace CDE */
mysql_declare_plugin(cde){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &CDE::cde_storage_engine,
    DSTORE_ENGINE_NAME,
    PLUGIN_AUTHOR_HUAWEI,
    "Dstore storage engine",
    PLUGIN_LICENSE_GPL,
    CDE::CdeInit,   /* Plugin Init */
    nullptr,        /* Plugin check uninstall */
    CDE::CdeDeinit, /* Plugin Deinit */
    0x0001,
    CDE::DstoreShowVariables,  /* status variables */
    CDE::cde_system_variables, /* system variables */
    nullptr,                   /* config options */
    0,                         /* flags */
},
    CDE::g_infomationSchemaDstoreTrx,
    CDE::g_infomationSchemaDstoreBufferPoolStats,
    CDE::g_infomationSchemaDstoreLocks, CDE::g_infomationSchemaDstoreMemStats,
    CDE::g_infomationSchemaDstoreSegStats, CDE::g_infomationSchemaDstoreUndo,
    CDE::g_infomationSchemaDstoreIndexes,
    CDE::g_infomationSchemaDstoreTableSpace,
    CDE::g_infomationSchemaDstoreOnlineDdlProgress mysql_declare_plugin_end;
