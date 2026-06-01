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

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "assert.h"

// clang-format off
#include "securec.h"

#include "framework/dstore_session_interface.h"
#include "framework/dstore_config_interface.h"
#include "framework/dstore_thread_interface.h"
#include "framework/dstore_vfs_interface.h"
#include "page/dstore_page.h"
#include "pdb/dstore_pdb_interface.h"
#include "control/dstore_control_pdbinfo.h"

#include "common/cde_def.h"
#include "common/cde_session.h"
#include "common/cde_typecache.h"
#include "common/cde_compare_utils.h"
#include "common/cde_rpl_wal.h"
#include "common/cde_trxmgr.h"
#include "boot/cde_instance.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"
#include "ddl/cde_tablespace.h"
#include "dict/cde_dict.h"
#include "backup/local_backup.h"
#include "handler/cde_information_schema.h"
#include "handler/ha_cde.h"

#include "sql/debug_sync.h"
#include "sql/local_backup/local_backup_file_utils.h"

#include "common/log/dstore_log.h"

// clang-format on
using DSTORE::CopyTypeDatumCb;
using DSTORE::DSTORE_SUCC;
using DSTORE::GetCmpFuncByOidCb;
using DSTORE::GetCmpFuncByTypeCb;
using DSTORE::GetTypeInfoCb;
using DSTORE::GetUserTypeFuncCb;
using DSTORE::ParseTypeLengthCb;
using DSTORE::PdbId;
using DSTORE::SetDefaultPdbId;
using DSTORE::SQLThrdInitCtx;
using DSTORE::StorageInstanceInterface;
using DSTORE::TenantConfig;
using DSTORE::ThreadContextInterface;

/* To get current session thread default THD */
THD *thd_get_current_thd();

namespace CDE {
DSTORE::RetStatus ExprHandleHeapTupleToIndex(DSTORE::CallbackFunc fnAddr,
                                             DSTORE::FunctionCallInfo fcinfo,
                                             DSTORE::Datum *result);
int CdeDdWriteExtraCommitWalCallback(uint32_t controlFlag, uint64 endPlsn,
                                     const unsigned char *buffer,
                                     uint32_t length);
int ExtraCommitWalDdlInfoHandle(uint32_t controlFlag, uint64 endPlsn,
                                const unsigned char *buffer, uint32_t length);
int ExtraCommitWalBinlogHandle(uint32_t controlFlag,
                               const unsigned char *buffer, uint32_t length);
void CdeDebugInit();
void CdeDebugExit();
static StorageInstanceInterface *g_store_instance = nullptr;
static std::atomic<uint64_t> g_session_id{0};
static VfsLibHandle *g_vfs_lib_handle = nullptr;
unsigned int g_numObjSpaceMgrWorkers = 0;
char *g_tenantConfig = nullptr;
unsigned long int g_walLevel = 0;
unsigned long int g_logMinMessages = 0;
unsigned long int g_foldLevel = 0;
unsigned long int g_perfLevel = 0;
unsigned long int g_lastAccessMode = 1;
unsigned long int g_flushDataMethod = 0;
unsigned int g_perfCounterInterval = 0;
unsigned long int g_walThrottlingMode = 0;
DSTORE::StorageGUC g_guc{};
char *g_dstoreLogPath = nullptr;
constexpr const char *const DSTORE_DIRECTORY_NAME = "#mysql_dstore";
static std::string g_dstoredata_rootpath;
static my_thread_t main_thread_id;
static uint32 WorkingGrandVersionNum;
bool g_useDefaultTemplatePDB = false;
bool g_enable_btree_trace = false;
DSTORE::IndexGetFuncCb g_indexFuncCb;

// mutex&map for thread core interruptHoldOffCount
std::mutex g_threadInterruptCountMutex;
std::unordered_map<ThreadContextInterface *, std::unique_ptr<uint32_t>>
    g_threadInterruptCountMap;

char *g_walDirConfig = nullptr;

/** Initialize members of g_guc which cannot get value from MYSQL_SYSVAR_*
 or variables can must be initialize during startup.
*/
static int InitGlobalGucConfig() {
  /* Initialize parameters not managed by my.cnf */
  g_dstoredata_rootpath =
      std::string(mysql_real_data_home) + DSTORE_DIRECTORY_NAME;
  g_guc.dataDir = &g_dstoredata_rootpath[0];
  g_guc.defaultIsolationLevel = 0;
  static_assert(DEBUG == 1, "LOG LEVEL MAPPING ERROR!");
  /* Why we need + 1? dstore use log level defined in syslog/err_log.h and
  it's level start from 1 instead of 0 (which is the start value of
  dstore_log_min_messages_typelib).  */
  g_guc.logMinMessages = static_cast<int>(g_logMinMessages + 1);
  g_guc.foldLevel = static_cast<int>(g_foldLevel + 1);
  g_guc.foldThreshold = 10;
  g_guc.csnAssignmentIncrement = 10000000;
  g_guc.moduleLoggingConfigure = nullptr;
  g_guc.updateCsnMinInterval = 500000;
  g_guc.numObjSpaceMgrWorkers = static_cast<uint16_t>(g_numObjSpaceMgrWorkers);
  g_guc.minFreePagePercentageThreshold1 = 0;
  g_guc.minFreePagePercentageThreshold2 = 0;
  g_guc.walLevel = static_cast<int>(g_walLevel);
  g_guc.walStreamCount = 10;
  g_guc.walFileNumber = 1;
  g_guc.redoBindCpuAttr = strdup("nobind");
  g_guc.numaNodeNum = 1;
  g_guc.recycleFsmTimeInterval = 1440;
  g_guc.probOfUpdateFsmTimestamp = 10;
  g_guc.probOfRecycleFsm = 0;
  g_guc.numaInfo = nullptr;
  g_guc.tacGracePeriod = 0;
  g_guc.perfCounterLevel = (g_perfLevel == 3) ? DSTORE::PerfLevel::OFF
                                              : DSTORE::PerfLevel(g_perfLevel);
  g_guc.walThrottlingMode = g_walThrottlingMode;
  if (g_guc.walFileSizeHwt != 0) {
    CDE_LOG_WARN(
        "set guc failed, dstore_wal_file_size_hwt must be 0, this function "
        "will be deprecated in the future.");
    g_guc.walFileSizeHwt = 0;
  }
  g_guc.flushDataMethod = DSTORE::FlushMethod(g_flushDataMethod);
  g_guc.lastAccessMode = static_cast<DSTORE::LastAccessMode>(g_lastAccessMode);
  g_guc.perfCounterInterval = static_cast<uint8_t>(g_perfCounterInterval);

  /* Initialize log adapter before call Dstore interface, all
  Dstore related logs will print into g_dstoreLogPath. */
  StorageLogInterface::InitLogAdapterInstance(
      g_guc.logMinMessages, g_dstoreLogPath, g_guc.foldPeriod,
      g_guc.foldThreshold, g_guc.foldLevel, g_guc.enableLogFileRotate);

  /* Initialize object parameters */
  g_guc.tenantConfig = new TenantConfig;
  CDE_ASSERT(g_guc.tenantConfig != nullptr);
  int ret = memset_s(g_guc.tenantConfig, sizeof(TenantConfig), 0,
                     sizeof(TenantConfig));
  CDE_ASSERT(ret == EOK);

  if (g_tenantConfig == nullptr || strlen(g_tenantConfig) <= 0 ||
      access(g_tenantConfig, F_OK) != 0) {
    CDE_LOG_ERROR(
        "Value of dstore_tenant_config must refer to valid tenant "
        "configuration file!");
    return CDE_ERROR;
  }
  ret = TenantConfigInterface::GetTenantConfig(g_tenantConfig,
                                               g_guc.tenantConfig);
  if (ret != DSTORE_SUCC) {
    CDE_LOG_ERROR("Get tenant configuration failed!");
    return CDE_ERROR;
  }

  if (dstore_exit_after_restore) {
    /* recovery/restore optimize is off when pre restore */
    g_guc.recoveryPageReaderNum = 0;
    g_guc.recoveryPageReadAheadWalSizeInterval = 0;
    g_guc.walRecoveryDirtyPageFlusherNum = 0;
    g_guc.gapFlushWalLength = 0;
    /* Disable double write when pre restore. */
    g_guc.enableDoubleWrite = false;
  }

  g_guc.localBackupIoBufferSize = rds_dstore_lb_io_buffer_size;

  if (g_walDirConfig != nullptr && g_walDirConfig[0] != '\0') {
    g_guc.walDirectory = g_walDirConfig;
  } else {
    g_guc.walDirectory = nullptr;
  }

  return CDE_OK;
}

/**
 @brief Free resources allocated in g_guc by new/malloc.

 This function releases all dynamically allocated resources
 associated with the global variable g_guc.
 */
static void FreeGucMemberPointers() {
  DBUG_EXECUTE_IF("dstore_free_guc_nullptr", {
    free(g_guc.redoBindCpuAttr);
    delete g_guc.tenantConfig;
    g_guc.redoBindCpuAttr = nullptr;
    g_guc.tenantConfig = nullptr;
  });

  if (g_guc.redoBindCpuAttr != nullptr) {
    free(g_guc.redoBindCpuAttr);
    g_guc.redoBindCpuAttr = nullptr;
  }
  if (g_guc.tenantConfig != nullptr) {
    delete g_guc.tenantConfig;
    g_guc.tenantConfig = nullptr;
  }
}

uint64_t CdeGenSessionId() { return g_session_id++; }

/**
init dstore vfs.

@return CDE_SUCC for success otherwise CDE_FAIL.
*/
bool CdeBootstrapInitVfs() {
  // init vfs
  if (!VfsInterface::ModuleInitialize()) {
    return CDE_FAIL;
  }

  CDE_ASSERT(g_guc.tenantConfig->storageConfig.type ==
             DSTORE::StorageType::TENANT_ISOLATION);

  // concat datadir with dstore and create datadir
  std::string dstoreDataHome =
      std::string(g_guc.dataDir) + "/" + DSTORE::BASE_DIR;
  int flags =
#ifdef _WIN32
      0
#else
      S_IRWXU | S_IRGRP | S_IXGRP
#endif
      ;
  if (access(dstoreDataHome.c_str(), F_OK) == 0 ||
      my_mkdir(g_guc.dataDir, flags, MYF(MY_WME)) ||
      my_mkdir(dstoreDataHome.c_str(), flags, MYF(MY_WME))) {
    CDE_LOG_ERROR(
        "dstore root directory (%s) create fail, maybe already exists.",
        dstoreDataHome.c_str());
    return CDE_FAIL;
  }

  if (g_guc.walDirectory != nullptr) {
    std::string dstoreWalDir =
        std::string(g_guc.walDirectory) + "/" + DSTORE::BASE_DIR;
    if (access(g_guc.walDirectory, F_OK) == 0 ||
        my_mkdir(g_guc.walDirectory, flags, MYF(MY_WME)) ||
        my_mkdir(dstoreWalDir.c_str(), flags, MYF(MY_WME))) {
      CDE_LOG_ERROR("Wal directory (%s) create fail, maybe already exists.",
                    dstoreWalDir.c_str());
      return CDE_FAIL;
    }
  }

  g_vfs_lib_handle = VfsInterface::SetupTenantIsoland(
      g_guc.tenantConfig, dstoreDataHome.c_str(), g_guc.walDirectory);
  CDE_ASSERT(g_vfs_lib_handle != nullptr);

  DSTORE::RetStatus status = VfsInterface::CreateTenantDefaultVfs(
      g_guc.tenantConfig, g_guc.walDirectory);
  if (status == DSTORE_SUCC) {
    return CDE_SUCC;
  } else {
    return CDE_FAIL;
  }
}

/**
dstore shutdown handler.

@param[in]  optInitialize  ture if initialize other false.
*/
void CdeShutdownDstoreInstance(bool optInitialize) {
  if (optInitialize) {
    CDE_LOG_SYSTEM("start -- shutdown dstore instance for bootstrap.");
  } else {
    CDE_LOG_SYSTEM("start -- shutdown dstore instance.");
  }

  /* Stop on-going local backup. */
  CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::STOP_FULL_LB, nullptr, false,
                              false, nullptr);
  CDE::ProcessWalArchive(CDE::LocalBackupCmd::STOP_WAL_ARCHIVE, nullptr);

  if (optInitialize) {
    StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
    // now we put create user pdb while shutdown instance after bootstrap
    // cause we should handle --init-file while bootstrap
    if (!g_useDefaultTemplatePDB) {
      CdeCreateUserPdb();
    }
    g_store_instance->RemoveVisibleThread(
        ThreadContextInterface::GetCurrentThreadContext());
    DSTORE::ThreadContextInterface::DestroyCurrentThreadContext();
    g_store_instance->BootstrapDestroy();
    g_store_instance->BootstrapResDestroy();
  } else {
    StoragePdbInterface::PassShutdownSignal(DSTORE::g_defaultPdbId);
    g_store_instance->ShutdownInstance();
  }

  DSTORE::StorageInstanceInterface::DestoryInstance();
  g_store_instance = nullptr;

  StorageLogInterface::StopLogAdapterInstance();

  FreeGucMemberPointers();

  CDE_LOG_SYSTEM("end -- shutdown dstore instance.");
}

StorageInstanceInterface *CdeGetDstoreInstance() { return g_store_instance; }

using std::filesystem::path;

/** set dstore profile,asp,slowquery log dir into the g_dstoreLogPath,
prevent polluting the MySQL data directory
@param[in] dstoreLogHomePath  dstore log home path
 */
static void CdeConfigLogDir(const std::string &dstoreLogHomePath) {
  path profileLogDir = path(dstoreLogHomePath) / PROFILE_LOG_DEFAULT_DIRECTORY;
  path aspLogDir = path(dstoreLogHomePath) / ASP_LOG_DEFAULT_DIRECTORY;
  SetPLogDirectory(profileLogDir.c_str());
  SetAspLogDirectory(aspLogDir.c_str());
}

bool InitializeInternalThread(SQLThrdInitCtx *context [[maybe_unused]]) {
  if (context->type ==
      DSTORE::InternalThreadType::THREAD_INDEX_PARALLEL_BUILD) {
    CdeConstructDstoreThrd();
    SingleStmtTrxStart(READ_COMMITTED);
  }
  return true;
}
void ReleaseInternalThread(SQLThrdInitCtx *context [[maybe_unused]]) {
  if (context->type ==
      DSTORE::InternalThreadType::THREAD_INDEX_PARALLEL_BUILD) {
    DSTORE::RetStatus ret = TransactionInterface::CommitTrxCommand();
    CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
    CdeDestroyDstoreThrd();
  }
}

static GetUserTypeFuncCb CdeGetUserTypeFuncOpset() {
  GetUserTypeFuncCb cb;
  cb.typeCb.funcByOidCb = static_cast<GetCmpFuncByOidCb>(CdeGetFuncCache);
  cb.typeCb.funcByTypeCb = static_cast<GetCmpFuncByTypeCb>(CdeGetFuncCache);
  cb.typeCb.typeInfoCb = static_cast<GetTypeInfoCb>(CdeGetTypeCache);
  cb.typeCb.typeLengthCb = static_cast<ParseTypeLengthCb>(CdeParseDatatypeLen);
  cb.typeCb.typeCopyCb = static_cast<CopyTypeDatumCb>(CdeCopyDatumCb);
  cb.always_use_cb = true;
  return cb;
}

static DSTORE::PdbReplicaFuncCb CdeGetRplWalFuncOpset() {
  DSTORE::PdbReplicaFuncCb cb;
  cb.flushedLsnNotifyFuncCb =
      static_cast<DSTORE::WalFlushedLsnNotifyFuncCb>(CdeWalFlushedLsnNotify);
  cb.getStandbyMinFlushedLsnFuncCb =
      static_cast<DSTORE::GetStandbyMinFlushedLsnFuncCb>(
          CdeGetStandbyMinFlushedLsn);
  cb.replayedLsnNotifyFuncCb = nullptr;
  cb.waitStandbyFlushFuncCb =
      static_cast<DSTORE::WalWaitStandbyFlushFuncCb>(CdeWalWaitStandbyFlush);
  return cb;
}

#ifndef NDEBUG
static void DstoreDebugSyncCb(const char *syncName, void *thd) {
  if (!thd) {
    DEBUG_SYNC_C_WITH_LEN(syncName, strlen(syncName));
  } else {
    DEBUG_SYNC_WITH_LEN((THD *)thd, syncName, strlen(syncName));
  }
}
#else
static inline void DstoreDebugSyncCb(const char *, void *) {}
#endif /* NDEBUG */

/* create user pdb while initialize and use it in startup stage */
void CdeCreateUserPdb() {
  StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
  DSTORE::PdbInfo pdbInfo;
  errno_t err = 0;
  err = memset_s(pdbInfo.pdbName, DSTORE::MAX_FILE_NAME_LEN, 0,
                 DSTORE::MAX_FILE_NAME_LEN);
  CDE_ASSERT(err == EOK);
  err = memcpy_s(pdbInfo.pdbName, DSTORE::MAX_FILE_NAME_LEN,
                 DSTORE_DIRECTORY_NAME, strlen(DSTORE_DIRECTORY_NAME));
  CDE_ASSERT(err == EOK);
  pdbInfo.pdbRoleMode = DSTORE::PdbRoleMode::PDB_PRIMARY;
  pdbInfo.templateId = DSTORE::PDB_TEMPLATE1_ID;
  err = memcpy_s(pdbInfo.pdbUuid, DSTORE::FORMATTED_UUID_ARR_LEN,
                 DSTORE::PDB_DEFAULT_UUID, DSTORE::FORMATTED_UUID_ARR_LEN);
  CDE_ASSERT(err == EOK);
  DSTORE::RetStatus ret = g_store_instance->AllocPdb(&pdbInfo);
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
  ret = g_store_instance->CreatePDB(&pdbInfo);
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);

  ret = g_store_instance->ClosePDB(pdbInfo.pdbName, true);
  CDE_ASSERT(ret == DSTORE::DSTORE_SUCC);
}

/**
dstore startup handler.

@param[in]  optInitialize  ture if initialize other false.
@param[in]  dstoreLogPath  dstore log path
@param[in]  dstoreGucPath  dstore guc path

@return CDE_ERROR for success otherwise CDE_ERROR.
*/
int CdeStartupDstoreInstance(bool optInitialize) {
  CdeConfigLogDir(g_dstoreLogPath);
  int res = InitGlobalGucConfig();
  if (res != CDE_OK) {
    CDE_LOG_ERROR("Global Guc configuration init failed, startup fail");
    return CDE_ERROR;
  }

  if (optInitialize) {
    if (CDE_FAIL == CdeBootstrapInitVfs()) {
      return CDE_ERROR;
    }
    g_guc.recoveryWorkerNum = 1;
  } else {
    if (access(g_guc.dataDir, F_OK) != 0) {
      CDE_LOG_ERROR("Check dstore datadir no exists, startup fail");
      return CDE_ERROR;
    }
  }

  if (optInitialize || g_useDefaultTemplatePDB) {
    DSTORE::SetDefaultPdbId(DSTORE::PDB_TEMPLATE1_ID);
  } else {
    DSTORE::SetDefaultPdbId(DSTORE::FIRST_USER_PDB_ID);
    std::string walFilesBaseDir = g_guc.walDirectory == nullptr
                                      ? g_dstoredata_rootpath
                                      : std::string(g_guc.walDirectory);
    std::string walFilesDir =
        walFilesBaseDir + "/" + DSTORE::BASE_DIR + "/PDB_" +
        std::to_string(DSTORE::FIRST_USER_PDB_ID) + "/wal";

    if (access(walFilesDir.c_str(), F_OK | W_OK | X_OK) != 0) {
      CDE_LOG_ERROR("dstore wal files dir doesn't exist, startup fail");
      return CDE_ERROR;
    }
  }

  g_store_instance =
      StorageInstanceInterface::Create(DSTORE::StorageInstanceType::SINGLE);
  CDE_ASSERT(g_store_instance != nullptr);
  g_store_instance->InitWorkingVersionNum((uint32_t *)&WorkingGrandVersionNum);

  DSTORE::ThreadContextInterface *thrd = ThreadContextInterface::Create();
  CDE_ASSERT(thrd != nullptr);
  thrd->SetXactPdbId(DSTORE::g_defaultPdbId);
  thrd->InitializeBasic();
  thrd->SetDebugSyncCb(DstoreDebugSyncCb);

  if (CDE::NeedRestore()) {
    if (dstore_restore_meta_path != nullptr &&
        dstore_restore_meta_path[0] != '\0') {
      local_backup_config config;
      config.meta_base_path = dstore_restore_meta_path;
      if (CDE::PrepareRecovery(CDE::LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN,
                               &config) != DSTORE::DSTORE_SUCC) {
        CDE_LOG_ERROR("Failed to set recovery start plsn for restore");
        return CDE_ERROR;
      }
    } else {
      CDE_LOG_ERROR(
          "dstore_restore_meta_path is not configured. "
          "Cannot set recovery start plsn for restore");
      return CDE_ERROR;
    }
  }
  if (CDE::CanDoWalArchive()) {
    if (dstore_local_backup_meta_path != nullptr &&
        dstore_local_backup_meta_path[0] != '\0') {
      local_backup_config config;
      config.meta_base_path = dstore_local_backup_meta_path;
      if (CDE::PrepareRecovery(CDE::LocalBackupCmd::SET_WAL_ARCHIVE_PLSN,
                               &config) != DSTORE::DSTORE_SUCC) {
        CDE_LOG_ERROR("Failed to set plsn for wal archive");
      }
    } else {
      CDE_LOG_ERROR(
          "dstore_local_backup_meta_path is not configured. "
          "Cannot set plsn for wal archive");
    }
  }

  g_store_instance->RegisterDstoreInternalThreadCallback(
      InitializeInternalThread, ReleaseInternalThread);

  DSTORE::DDWriteExtraWalCallback ddWriteExtraCommitWalCb =
      reinterpret_cast<DSTORE::DDWriteExtraWalCallback>(
          CdeDdWriteExtraCommitWalCallback);
  /* DDL info will always be written. */
  uint32_t controlFlag = DRAW_DDL_INFO;
  if (rds_dstore_support_binlog_check) {
    controlFlag |= DRAW_GTID_INFO;
  }
  g_store_instance->RegisterDDWriteExtraWalCallback(
      DSTORE::g_defaultPdbId, ddWriteExtraCommitWalCb, controlFlag);

  cache_hash_manager::get_ins()->init();
  GetUserTypeFuncCb cb = CdeGetUserTypeFuncOpset();
  g_indexFuncCb.commonCb = ExprHandleHeapTupleToIndex;
  g_store_instance->RegisterIndexCallbacks(&g_indexFuncCb);
  if (optInitialize) {
    g_store_instance->Bootstrap(&g_guc, &cb);
    // todo: should we keep the context even this thread will exit after boot
    thrd->InitStorageContext(DSTORE::g_defaultPdbId);
    g_store_instance->AddVisibleThread(thrd, DSTORE::g_defaultPdbId);

    DSTORE::CreateTemplateTablespace(DSTORE::g_defaultPdbId);
    DSTORE::CreateUndoMapSegment(DSTORE::g_defaultPdbId);
  } else {
    DSTORE::PdbReplicaFuncCb rpl_cb = CdeGetRplWalFuncOpset();
    g_store_instance->StartupInstance(&g_guc, &cb, &rpl_cb);
    thrd->InitStorageContext(DSTORE::g_defaultPdbId);
  }
  thrd->InitTransactionRuntime(DSTORE::g_defaultPdbId, nullptr, nullptr);
  TableSpaceMgrInit();

  main_thread_id = my_thread_self();

  /* dstore_exit_after_restore is used by local backup. Do restore work for
  backup finish. If this backup is used, it will cost less time to restore. */
  if (!optInitialize && CDE::NeedRestore() && dstore_exit_after_restore) {
    StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
    CDE_LOG_SYSTEM(
        "dstore_exit_after_restore is set. Exit after restore finish");
    flush_error_log_messages();
    exit(0);
  }

  return CDE_OK;
}

/** flag for checking threadlocal key is valid*/
static std::atomic<bool> g_isKeyValid{false};
/** the global thread-local key*/
static pthread_key_t g_cdeThreadlocalKey;

/**
 Will be called when thread exits.
 used for automatically cleaning up threadlocal resources
 when a threadpool worker thread exits.
 */
static void CdeThreadlocalDestroyCallback(void *) {
  if (g_isKeyValid) {
    CdeDestroyDstoreThrd();
  }
}

/** Create a key for access threadlocal variables.
 */
void CdeThreadlocalCreateKey() {
  if (!g_isKeyValid) {
    pthread_key_create(&g_cdeThreadlocalKey, CdeThreadlocalDestroyCallback);
    g_isKeyValid = true;
  }
}

/** Associate the key with a value.
NOTE: The value is useless, just ensure that a callback
function is called when the thread exits.
 */
void CdeThreadlocalSetSpecific(void *val) {
  if (g_isKeyValid) {
    pthread_setspecific(g_cdeThreadlocalKey, val);
  }
}

/** Delete threadlocal key.
 */
void CdeThreadlocalDeleteKey() {
  if (g_isKeyValid) {
    pthread_key_delete(g_cdeThreadlocalKey);
    g_isKeyValid = false;
  }
}

void CdeRegisterWriteExtraCommitWalCallback(
    DSTORE::WriteExtraWalCallback callback) {
  g_store_instance->RegisterWriteExtraWalCallback(callback);
}

void CdeRegisterDbugPushdownCallback(DSTORE::DbugPushDownCallback callback) {
  g_store_instance->RegisterDbugPushdownCallback(callback);
}

void CdeRegisterUndoExtraCallback(DSTORE::UndoExtraCallback callback) {
  g_store_instance->RegisterUndoExtraCallback(callback);
}

void CdeConstructSession(cde_session_t *&cdeSessionRef) {
  CdeConstructDstoreThrd();

  if (cdeSessionRef == nullptr) {
    cdeSessionRef = new cde_session_t();
  }

  // assign the current thread context, maybe used in binlog group commit.
  cdeSessionRef->dstore_thrd =
      ThreadContextInterface::GetCurrentThreadContext();

  if (!cdeSessionRef->dstore_session) {
    THD *thd = ::thd_get_current_thd();
    cdeSessionRef->dstore_session = DSTORE::CreateStorageSession(
        my_thread_self(), thd == nullptr ? 2048 : ThdTmpBufferSize(thd));
  }

  CDE_ASSERT(cdeSessionRef && cdeSessionRef->dstore_thrd &&
             cdeSessionRef->dstore_session);
  cdeSessionRef->dstore_thrd->AttachSessionToThread(
      cdeSessionRef->dstore_session);
}

int CdeDestorySession(cde_session_t *&cdeSessionRef) {
  CDE_ASSERT(g_store_instance != nullptr);

  if (cdeSessionRef->dstore_session) {
    DSTORE::CleanUpSession(cdeSessionRef->dstore_session);
    cdeSessionRef->dstore_session = nullptr;
  }

  delete cdeSessionRef;
  cdeSessionRef = nullptr;

  CdeDestroyDstoreThrd();
  return 0;
}

void CdeConstructDstoreThrd() {
  if (ThreadContextInterface::GetCurrentThreadContext() != nullptr) {
    return;
  }

  DSTORE::ThreadContextInterface *thrdCtx =
      DSTORE::ThreadContextInterface::Create();
  CDE_ASSERT(thrdCtx != nullptr);
  auto thrd = dynamic_cast<DSTORE::ThreadContext *>(thrdCtx);
  thrd->SetThreadMemLevel(DSTORE::ThreadMemoryLevel::THREADMEM_HIGH_PRIORITY);
  DSTORE::RetStatus ret = thrd->InitializeBasic();
  CDE_ASSERT(ret == DSTORE::RetStatus::DSTORE_SUCC);

  ret = thrd->InitStorageContext(DSTORE::g_defaultPdbId);
  CDE_ASSERT(ret == DSTORE::RetStatus::DSTORE_SUCC);
  thrd->SetNeedCommBuffer(true);
  uint32_t *interruptHoldoffCount = new (std::nothrow) uint32_t(0);
  CDE_ASSERT(interruptHoldoffCount);
  {
    std::lock_guard<std::mutex> lock(g_threadInterruptCountMutex);
    g_threadInterruptCountMap.emplace(
        thrdCtx, std::unique_ptr<uint32_t>(interruptHoldoffCount));
  }
  g_store_instance->AddVisibleThread(thrd, DSTORE::g_defaultPdbId, "cde_worker",
                                     interruptHoldoffCount);
  thrd->InitTransactionRuntime(DSTORE::g_defaultPdbId, nullptr, nullptr);
  thrd->SetExtraCallback(ThreadTransactionCallback);
  thrd->SetDebugSyncCb(DstoreDebugSyncCb);

  /* all threads used as threadpool workers should release resources
  when worker threads exit. */
  CdeThreadlocalSetSpecific(ThreadContextInterface::GetCurrentThreadContext());
  cde_perf_counters::getInstance()->_alive_dstore_sessions.increment();
}
/*
 * @brief used to check whether the thrd is valid
 * @param thrd: the thread context to be checked
 *
 * @return true if thrd is valid, otherwise false
 */
bool CdeCheckThrdValid(DSTORE::ThreadContextInterface *thrd) {
  std::lock_guard<std::mutex> lock(g_threadInterruptCountMutex);
  if (g_threadInterruptCountMap.count(thrd) > 0) {
    return true;
  }
  // thrd has been destroyed when thread exit
  return false;
}

void CdeDestroyDstoreThrd() {
  DSTORE::ThreadContextInterface *thrd =
      DSTORE::ThreadContextInterface::GetCurrentThreadContext();
  if (thrd == nullptr) {
    return;
  }
  // remove session
  thrd->DetachSessionFromThread();
  {
    std::lock_guard<std::mutex> lock(g_threadInterruptCountMutex);
    g_threadInterruptCountMap.erase(thrd);
  }
  /* The main thread can't call destory thrd in this context to
  avoid unavailability of thrd and its resources during shutdown. */
  if (likely(my_thread_equal(main_thread_id, my_thread_self()) == 0)) {
    g_store_instance->UnregisterThread();
  }
}

int CdeModuleInit() {
  CdeDebugInit();
  DictSysInit();
  return CDE_OK;
}

int CdeModuleExit() {
  DictSysDestroy();
  CdeDebugExit();
  return CDE_OK;
}
} /* namespace CDE */
