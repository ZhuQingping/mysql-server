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
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <numeric>

#include "securec.h"

#include "framework/dstore_instance_interface.h"
#include "framework/dstore_session_interface.h"
#include "framework/dstore_config_interface.h"
#include "framework/dstore_thread_interface.h"
#include "framework/dstore_vfs_interface.h"
#include "page/dstore_page.h"
#include "pdb/dstore_pdb_interface.h"
#include "transaction/dstore_transaction_interface.h"

#include "framework/dstore_instance.h"
#include "framework/dstore_pdb.h"
#include "log/dstore_log_interface.h"
#include "systable/systable_type.h"
#include "tablespace/dstore_tablespace_interface.h"

// clang-format off
// todo: remove it
#include "common/cde_alloc.h"
#include "common/cde_def.h"
#include "common/cde_session.h"
#include "common/cde_typecache.h"
#include "common/cde_compare_utils.h"
#include "boot/cde_instance.h"
#include "dict/cde_dict.h"
#include "ddl/cde_ddl.h"
#include "ddl/cde_ddl_log.h"

#include "ut_cde_test.h"
// clang-format on

using DSTORE::CleanUpSession;
using DSTORE::CopyTypeDatumCb;
using DSTORE::CreateStorageSession;
using DSTORE::CreateTemplateTablespace;
using DSTORE::CreateUndoMapSegment;
using DSTORE::DSTORE_SUCC;
using DSTORE::g_defaultPdbId;
using DSTORE::GetCmpFuncByOidCb;
using DSTORE::GetCmpFuncByTypeCb;
using DSTORE::GetTypeInfoCb;
using DSTORE::GetUserTypeFuncCb;
using DSTORE::ParseTypeLengthCb;
using DSTORE::PDB_TEMPLATE1_ID;
using DSTORE::PdbId;
using DSTORE::RetStatus;
using DSTORE::SetDefaultPdbId;
using DSTORE::SQLThrdInitCtx;
using DSTORE::StorageGUC;
using DSTORE::StorageInstanceInterface;
using DSTORE::StorageSession;
using DSTORE::TenantConfig;
using DSTORE::ThreadContextInterface;

namespace CDE {

#define UT_MAX_PATH_LEN (1024)

// todo:
// dstore相关文件路径可配置。当前所有配置文件要求放在二进制文件的工作目录

StorageGUC g_guc;
StorageInstanceInterface *g_instance = nullptr;

thread_local ThreadContextInterface *dstore_thrd_ctx = nullptr;
thread_local StorageSession *dstore_session = nullptr;

thread_local cde_session_t *g_cde_session = nullptr;

std::string ut_cde_cfg::m_root_fp = "";
std::string ut_cde_cfg::m_start_cfg_fp = "";
std::string ut_cde_cfg::m_data_fp = "";
std::string ut_cde_cfg::m_log_fp = "";
std::string ut_cde_cfg::m_md_fp = "";
std::string ut_cde_cfg::m_wal_fp = "";
std::string ut_cde_cfg::m_dstore_fp = "";
static uint32 WorkingGrandVersionNum = 0;

static int remove_dir(const char *dir_path) {
  const char *cur_dir = ".";
  const char *upper_dir = "..";
  DIR *dir_handle = nullptr;
  dirent *dir_context = nullptr;
  struct stat dir_stat;
  char sub_dir_path[UT_MAX_PATH_LEN];
  if (access(dir_path, F_OK) != 0) {
    return 0;
  }

  if (stat(dir_path, &dir_stat) < 0) {
    assert(0);
  }
  if (S_ISDIR(dir_stat.st_mode)) {
    dir_handle = opendir(dir_path);
    assert(dir_handle);
    while ((dir_context = readdir(dir_handle)) != nullptr) {
      if ((strcmp(dir_context->d_name, cur_dir) == 0) ||
          strcmp(dir_context->d_name, upper_dir) == 0) {
        continue;
      }
      sprintf_s(sub_dir_path, UT_MAX_PATH_LEN, "%s/%s", dir_path,
                dir_context->d_name);
      remove_dir(sub_dir_path);
    }
    closedir(dir_handle);
    rmdir(dir_path);
  } else {
    remove(dir_path);
  }
  return 0;
}

void ut_cde_cfg::reset_root_path(std::string path) {
  m_root_fp = path;
  std::string delim = "/";
  // get binary file name
  char buf[UT_MAX_PATH_LEN] = {0};
  memset_s(buf, UT_MAX_PATH_LEN, 0, UT_MAX_PATH_LEN);
  int ret = readlink("/proc/self/exe", buf, UT_MAX_PATH_LEN);
  ASSERT_GT(ret, 0);
  std::string tmp = std::string(buf);
  size_t last_slash_pos = tmp.find_last_of('/');
  if (last_slash_pos != std::string::npos) {
    tmp = tmp.substr(last_slash_pos + 1);
  }
  m_data_fp = m_root_fp + delim + std::string("cde_ut_") + tmp;
  m_dstore_fp = m_data_fp + delim + DSTORE::BASE_DIR;
  m_log_fp = m_data_fp + delim + std::string("log");
  m_md_fp = m_data_fp + delim + std::string("metadata");
  m_wal_fp = m_data_fp + delim + std::string("dstore_wal");

  // todo: remove following
  remove_dir(m_data_fp.c_str());
  mkdir(m_data_fp.c_str(), 0777);
  mkdir(m_dstore_fp.c_str(), 0777);
  mkdir(m_md_fp.c_str(), 0777);
  mkdir(m_wal_fp.c_str(), 0777);
  if (chdir(m_root_fp.c_str()) != 0) {
    perror("chdir failed");
  }
}

void ut_cde_cfg::init_file_path() {
  std::string delim = "/";
  std::string ul = "..";
  // 获取可执行文件所在路径
  char buf[UT_MAX_PATH_LEN] = {0};
  memset_s(buf, UT_MAX_PATH_LEN, 0, UT_MAX_PATH_LEN);
  int ret = readlink("/proc/self/exe", buf, UT_MAX_PATH_LEN);
  ASSERT_GT(ret, 0);
  std::string tmp = std::string(buf);
  // m_root_fp = tmp.substr(0, tmp.find_last_of("/")) + delim + ul + delim;
  m_root_fp = tmp.substr(0, tmp.find_last_of("/"));
  // m_root_fp = m_root_fp.substr(0, m_root_fp.find_last_of("/"));
  m_start_cfg_fp = m_root_fp + delim + ul + delim +
                   std::string("storage/dstore/config") + delim +
                   std::string("tenant_isoland_start.json");
  reset_root_path(m_root_fp);
}

void ut_cde_cfg::deinit_file_path() {
  // remove dstore data dir
  remove_dir(m_data_fp.c_str());
}

void cdetest_init_dstore_guc() {
  std::cout << "--------------------"
            << "Tenant isoland start config "
            << ut_cde_cfg::get_start_cfg_path() << "--------------------"
            << std::endl;

  g_guc.selfNodeId = 1;
  std::cout << "selfNodeId: " << g_guc.selfNodeId << std::endl;

  g_guc.buffer = 300000;
  std::cout << "buffer: " << g_guc.buffer << std::endl;

  g_guc.bufferLruPartition = 100;
  std::cout << "bufferLruPartition: " << g_guc.bufferLruPartition << std::endl;

  g_guc.checkpointTimeout = 60;
  std::cout << "checkpointTimeout: " << g_guc.checkpointTimeout << std::endl;

  g_guc.defaultIsolationLevel = 0;
  std::cout << "defaultIsolationLevel: " << g_guc.defaultIsolationLevel
            << std::endl;

  g_guc.maintenanceWorkMem = 2097152;
  std::cout << "maintenanceWorkMem: " << g_guc.maintenanceWorkMem << std::endl;

  const char *data_path = ut_cde_cfg::get_data_path();
  g_guc.dataDir = new char[strlen(data_path) + 1];
  memcpy_s(g_guc.dataDir, strlen(data_path) + 1, data_path,
           strlen(data_path) + 1);
  std::cout << "dataDir: "
            << (g_guc.dataDir == nullptr ? "nullptr" : g_guc.dataDir)
            << std::endl;

  g_guc.ncores = 800;
  std::cout << "ncores: " << g_guc.ncores << std::endl;

  g_guc.logMinMessages = 3;
  std::cout << "logMinMessages: " << g_guc.logMinMessages << std::endl;

  g_guc.foldPeriod = 20;
  std::cout << "foldPeriod: " << g_guc.foldPeriod << std::endl;

  g_guc.foldThreshold = 10;
  std::cout << "foldThreshold: " << g_guc.foldThreshold << std::endl;

  g_guc.foldLevel = 5;
  std::cout << "foldLevel: " << g_guc.foldLevel << std::endl;

  g_guc.csnAssignmentIncrement = 1000;
  std::cout << "csnAssignmentIncrement: " << g_guc.csnAssignmentIncrement
            << std::endl;

  g_guc.moduleLoggingConfigure = nullptr;
  std::cout << "moduleLoggingConfigure: "
            << (g_guc.moduleLoggingConfigure == nullptr
                    ? "nullptr"
                    : g_guc.moduleLoggingConfigure)
            << std::endl;

  g_guc.lockHashTableSize = 256;
  std::cout << "lockHashTableSize: " << g_guc.lockHashTableSize << std::endl;

  g_guc.lockTablePartitionNum = 256;
  std::cout << "lockTablePartitionNum: " << g_guc.lockTablePartitionNum
            << std::endl;

  g_guc.enableLazyLock = 0;
  std::cout << "enableLazyLock: "
            << ((g_guc.enableLazyLock == 1) ? "true" : "false") << std::endl;

  g_guc.vfsTenantIsolationConfigPath = nullptr;
  std::cout << "vfsTenantIsolationConfigPath: "
            << (g_guc.vfsTenantIsolationConfigPath == nullptr
                    ? "nullptr"
                    : g_guc.vfsTenantIsolationConfigPath)
            << std::endl;

  g_guc.updateCsnMinInterval = 500000;
  std::cout << "updateCsnMinInterval: " << g_guc.updateCsnMinInterval
            << std::endl;

  g_guc.numObjSpaceMgrWorkers = 1;
  std::cout << "numObjSpaceMgrWorkers: " << g_guc.numObjSpaceMgrWorkers
            << std::endl;

  g_guc.minFreePagePercentageThreshold1 = 0;
  std::cout << "minFreePagePercentageThreshold1: "
            << g_guc.minFreePagePercentageThreshold1 << std::endl;

  g_guc.minFreePagePercentageThreshold2 = 0;
  std::cout << "minFreePagePercentageThreshold2: "
            << g_guc.minFreePagePercentageThreshold2 << std::endl;

  g_guc.probOfExtensionThreshold = 0;
  std::cout << "probOfExtensionThreshold: " << g_guc.probOfExtensionThreshold
            << std::endl;

  g_guc.recoveryWorkerNum = 1;
  std::cout << "recoveryWorkerNum: " << g_guc.recoveryWorkerNum << std::endl;

  g_guc.synchronousCommit = 1;
  std::cout << "synchronousCommit: "
            << ((g_guc.synchronousCommit == 1) ? "true" : "false") << std::endl;

  g_guc.walStreamCount = 10;
  std::cout << "walStreamCount: " << g_guc.walStreamCount << std::endl;

  g_guc.walFileNumber = 1;
  std::cout << "walFileNumber: " << g_guc.walFileNumber << std::endl;

  char *tmp_str_value = (char *)"3221225472";
  g_guc.walFileSize = std::stoll(tmp_str_value);
  std::cout << "walFileSize: " << g_guc.walFileSize << std::endl;

  g_guc.walBuffers = 16384;
  std::cout << "walBuffers: " << g_guc.walBuffers << std::endl;

  tmp_str_value = (char *)"536870912";
  g_guc.walReadBufferSize = std::stoll(tmp_str_value);
  std::cout << "walReadBufferSize: " << g_guc.walReadBufferSize << std::endl;

  tmp_str_value = (char *)"268435456";
  g_guc.walRedoBufferSize = std::stoll(tmp_str_value);
  std::cout << "walRedoBufferSize: " << g_guc.walRedoBufferSize << std::endl;

  g_guc.walwriterCpuBind = 0;
  std::cout << "walwriterCpuBind: " << g_guc.walwriterCpuBind << std::endl;

  g_guc.redoBindCpuAttr = (char *)"nobind";
  std::cout << "redoBindCpuAttr: " << g_guc.redoBindCpuAttr << std::endl;

  g_guc.numaNodeNum = 1;
  std::cout << "numaNodeNum: " << static_cast<int>(g_guc.numaNodeNum)
            << std::endl;

  g_guc.disableBtreePageRecycle = 0;
  std::cout << "disableBtreePageRecycle: "
            << ((g_guc.disableBtreePageRecycle == 1) ? "true" : "false")
            << std::endl;

  g_guc.deadlockTimeInterval = 20000;
  std::cout << "deadlockTimeInterval: " << g_guc.deadlockTimeInterval
            << std::endl;

  g_guc.recycleFsmTimeInterval = 1440;
  std::cout << "recycleFsmTimeInterval: " << g_guc.recycleFsmTimeInterval
            << std::endl;

  g_guc.probOfUpdateFsmTimestamp = 10;
  std::cout << "probOfUpdateFsmTimestamp: " << g_guc.probOfUpdateFsmTimestamp
            << std::endl;

  g_guc.probOfRecycleFsm = 0;
  std::cout << "probOfRecycleFsm: " << g_guc.probOfRecycleFsm << std::endl;

  g_guc.probOfRecycleBtree = 0;
  std::cout << "probOfRecycleBtree: " << g_guc.probOfRecycleBtree << std::endl;

  /* StorageDistributedGUC */
  g_guc.distLockNumBuckets = 1024;
  std::cout << "distLockNumBuckets: " << g_guc.distLockNumBuckets << std::endl;

  g_guc.distLockMaxRingSize = 1024;
  std::cout << "distLockMaxRingSize: " << g_guc.distLockMaxRingSize
            << std::endl;

  g_guc.csnMode = static_cast<DSTORE::CsnMode>(0);
  std::cout << "csnMode: " << static_cast<int>(g_guc.csnMode) << std::endl;

  g_guc.ctrlPlanePort = 11830;
  std::cout << "ctrlPlanePort: " << g_guc.ctrlPlanePort << std::endl;

  g_guc.rdmaGidIndex = 3;
  std::cout << "rdmaGidIndex: " << static_cast<int>(g_guc.rdmaGidIndex)
            << std::endl;

  g_guc.rdmaIbPort = 1;
  std::cout << "rdmaIbPort: " << static_cast<int>(g_guc.rdmaIbPort)
            << std::endl;

  g_guc.pdReadAuthResetPeriod = 30;
  std::cout << "pdReadAuthResetPeriod: " << g_guc.pdReadAuthResetPeriod
            << std::endl;

  g_guc.csnThreadBindCpu = -1;
  std::cout << "csnThreadBindCpu: " << g_guc.csnThreadBindCpu << std::endl;

  g_guc.commConfigStr = nullptr;
  std::cout << "commConfigStr: "
            << (g_guc.commConfigStr == nullptr ? "nullptr"
                                               : g_guc.commConfigStr)
            << std::endl;

  g_guc.commThreadMin = 30;
  std::cout << "commThreadMin: " << g_guc.commThreadMin << std::endl;
  g_guc.commThreadMax = 50;
  std::cout << "commThreadMax: " << g_guc.commThreadMax << std::endl;
  g_guc.clusterId = 1;
  std::cout << "clusterId: " << g_guc.clusterId << std::endl;

  g_guc.memberView = nullptr;
  std::cout << "memberView: "
            << (g_guc.memberView == nullptr ? "nullptr" : g_guc.memberView)
            << std::endl;

  g_guc.commProtocolTypeStr = (char *)"TCP_TYPE";
  std::cout << "commProtocolTypeStr: "
            << (g_guc.commProtocolTypeStr == nullptr
                    ? "nullptr"
                    : g_guc.commProtocolTypeStr)
            << std::endl;

  g_guc.commProtocolType = 0;
  std::cout << "commProtocolType: " << g_guc.commProtocolType << std::endl;

  g_guc.globalClockAdjustWaitTimeUs = 250;
  std::cout << "globalClockAdjustWaitTimeUs: "
            << g_guc.globalClockAdjustWaitTimeUs << std::endl;

  g_guc.globalClockSyncIntervalMs = 5;
  std::cout << "globalClockSyncIntervalMs: " << g_guc.globalClockSyncIntervalMs
            << std::endl;

  g_guc.gclockOverlapWaitTimeOptimization = 1;
  std::cout << "gclockOverlapWaitTimeOptimization: "
            << ((g_guc.gclockOverlapWaitTimeOptimization == 1) ? "true"
                                                               : "false")
            << std::endl;

  g_guc.enableQuickStartUp = 1;
  std::cout << "enableQuickStartUp: "
            << ((g_guc.enableQuickStartUp == 1) ? "true" : "false")
            << std::endl;

  g_guc.defaultHeartbeatTimeoutInterval = 1;
  std::cout << "defaultHeartbeatTimeoutInterval: "
            << g_guc.defaultHeartbeatTimeoutInterval << std::endl;

  g_guc.defaultWalSizeThreshold = 1;
  std::cout << "defaultWalSizeThreshold: " << g_guc.defaultWalSizeThreshold
            << std::endl;

  g_guc.bgPageWriterSleepMilliSecond = 2000;
  g_guc.walThrottlingSize = 1048576;
  g_guc.maxIoCapacityKb = 512000;
  g_guc.bgWalWriterMinBytes = 51200;
  g_guc.walEachWriteLenghthLimit = 524288;

  g_guc.tenantConfig = new TenantConfig;
  assert(g_guc.tenantConfig != nullptr);
  assert(memset_s(g_guc.tenantConfig, sizeof(TenantConfig), 0,
                  sizeof(TenantConfig)) == EOK);
  tmp_str_value = strdup(ut_cde_cfg::get_start_cfg_path());
  assert(strlen(tmp_str_value) > 0);
  /* It must exist. if not, please check your guc config, or retry cmake to
   * install exactly */
  ASSERT_EQ(access(tmp_str_value, F_OK), 0);
  RetStatus ret =
      TenantConfigInterface::GetTenantConfig(tmp_str_value, g_guc.tenantConfig);
  assert(ret == DSTORE_SUCC);
  (void)ret;
  std::cout << "tmp_str_value = " << tmp_str_value << std::endl << std::endl;
  std::cout << "config.storageConfig.clientLibPath = "
            << g_guc.tenantConfig->storageConfig.clientLibPath << std::endl
            << std::endl;
  std::cout << std::endl << std::endl;
}

void cdetest_init_dstore_env() { ut_cde_cfg::init_file_path(); }
void cdetest_deinit_dstore_env() { ut_cde_cfg::deinit_file_path(); }

void cdetest_init_dstore_vfs() {
  bool flag = VfsInterface::ModuleInitialize();
  CDE_ASSERT(flag);
  VfsInterface::SetupTenantIsoland(g_guc.tenantConfig,
                                   ut_cde_cfg::get_dstore_path());
  RetStatus ret_status =
      VfsInterface::CreateTenantDefaultVfs(g_guc.tenantConfig);
  CDE_ASSERT(DSTORE_SUCC == ret_status);
}

void cdetest_init_dstore_log_adpt() {
  StorageLogInterface::InitLogAdapterInstance(
      g_guc.logMinMessages, ut_cde_cfg::get_log_path(), g_guc.foldPeriod,
      g_guc.foldThreshold, g_guc.foldLevel, g_guc.enableLogFileRotate);
}

bool initialize_dstore_internal_thread(SQLThrdInitCtx *context
                                       [[maybe_unused]]) {
  return true;
}
void release_dstore_internal_thread(SQLThrdInitCtx *context [[maybe_unused]]) {}

void cdetest_bootstrap_dstore_inner() {
  g_instance =
      StorageInstanceInterface::Create(DSTORE::StorageInstanceType::SINGLE);
  g_instance->InitWorkingVersionNum((uint32_t *)&WorkingGrandVersionNum);
  SetDefaultPdbId(PDB_TEMPLATE1_ID);
  dstore_thrd_ctx = ThreadContextInterface::Create();
  dstore_thrd_ctx->InitializeBasic();
  g_instance->Bootstrap(&g_guc);
  dstore_thrd_ctx->SetXactPdbId(g_defaultPdbId);
  dstore_thrd_ctx->InitStorageContext(g_defaultPdbId);
  g_instance->AddVisibleThread(dstore_thrd_ctx, DSTORE::g_defaultPdbId);
  CreateTemplateTablespace(DSTORE::g_defaultPdbId);
  CreateUndoMapSegment(DSTORE::g_defaultPdbId);

  dstore_thrd_ctx->InitTransactionRuntime(DSTORE::g_defaultPdbId, nullptr,
                                          nullptr);
  g_instance->RegisterDstoreInternalThreadCallback(
      initialize_dstore_internal_thread, release_dstore_internal_thread);

  StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
  g_instance->UnregisterThread();
  g_instance->BootstrapDestroy();

  StorageInstanceInterface::DestoryInstance();
  g_instance = nullptr;
  StorageLogInterface::StopLogAdapterInstance();
}

void CDETEST::Bootstrap() {
  cdetest_bootstrap_dstore_inner();
  // init_dstore_paras paras = {
  //     .guc_path = ut_cde_cfg::get_guc_cfg_path(),
  //     .log_path = ut_cde_cfg::get_log_path(),
  // };

  // int ret = CdeStartupDstoreInstance(paras);
  // assert(ret==0);

  // ret = CdeModuleInit();
  // assert(ret==0);
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

void CDETEST::Start() {
  if (nullptr != g_instance) return;
  cdetest_init_dstore_log_adpt();
  g_instance =
      StorageInstanceInterface::Create(DSTORE::StorageInstanceType::SINGLE);
  g_instance->InitWorkingVersionNum((uint32_t *)&WorkingGrandVersionNum);
  g_instance->RegisterDstoreInternalThreadCallback(
      initialize_dstore_internal_thread, release_dstore_internal_thread);
  dstore_thrd_ctx = ThreadContextInterface::Create();
  dstore_thrd_ctx->InitializeBasic();
  dstore_session = CreateStorageSession(1ULL);
  dstore_thrd_ctx->AttachSessionToThread(dstore_session);
  GetUserTypeFuncCb cb = CdeGetUserTypeFuncOpset();
  g_instance->StartupInstance(&g_guc, &cb);
  dstore_thrd_ctx->SetXactPdbId(g_defaultPdbId);
  dstore_thrd_ctx->InitStorageContext(g_defaultPdbId);
  dstore_thrd_ctx->InitTransactionRuntime(DSTORE::g_defaultPdbId, nullptr,
                                          nullptr);

  cache_hash_manager::get_ins()->init();
  CdeModuleInit();
  CdeDdlLog::GetInstance()->SetSkip(true);
}

void CDETEST::Stop() {
  CdeModuleExit();
  if (nullptr == g_instance) return;
  StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
  g_instance->ShutdownInstance();
  StorageInstanceInterface::DestoryInstance();
  g_instance = nullptr;
  dstore_thrd_ctx = nullptr;
  CleanUpSession(dstore_session);
  StorageLogInterface::StopLogAdapterInstance();
}

void CDETEST::StartSession() {
  if (dstore_session != nullptr) return;
  assert((g_instance != nullptr) && (dstore_thrd_ctx != nullptr));
  g_instance->CreateThreadAndRegister(g_defaultPdbId, false, "cde_simulator",
                                      true);
  ThreadContextInterface::GetCurrentThreadContext()->InitTransactionRuntime(
      DSTORE::g_defaultPdbId, nullptr, nullptr);
  dstore_session = CreateStorageSession(1ULL);
  ThreadContextInterface *thrd_ctx =
      ThreadContextInterface::GetCurrentThreadContext();
  thrd_ctx->AttachSessionToThread(dstore_session);

  // // todo:
  // g_cde_session = new cde_session_t();
  // (void)CdeConstructSession(g_cde_session);
}

void CDETEST::StopSession() {
  if (dstore_session == nullptr) return;
  assert((g_instance != nullptr) && (dstore_thrd_ctx != nullptr));
  dstore_thrd_ctx->DetachSessionFromThread();
  CleanUpSession(dstore_session);
  dstore_session = nullptr;

  // // todo:
  // int ret = CdeCleanupDstoreSession(g_cde_session);
  // assert(ret == 0);
}

void CDETEST::SetUpTestCase() {}

void CDETEST::TearDownTestCase() {}

void CDETEST::InitOnce() {
  cdetest_init_dstore_env();
  cdetest_init_dstore_guc();

  // // todo:
  // ut_cde_cfg::init_file_path();
  // init_dstore_paras paras = {
  //     .guc_path = ut_cde_cfg::get_guc_cfg_path(),
  //     .log_path = ut_cde_cfg::get_log_path(),
  // };

  // CdeBoostrapDstoreInstance(paras);
}

void CDETEST::Destroy() {
  if (g_guc.tenantConfig) {
    delete g_guc.tenantConfig;
    g_guc.tenantConfig = nullptr;
  }
  cdetest_deinit_dstore_env();
}

void CDETEST::SetUp() {
  system_charset_info = &my_charset_utf8mb3_general_ci;
  cdetest_init_dstore_env();
  SetDefaultPdbId(PDB_TEMPLATE1_ID);
  g_guc.selfNodeId = 0;
  cdetest_init_dstore_log_adpt();
  cdetest_init_dstore_vfs();
}

void CDETEST::TearDown() {
  StorageLogInterface::StopLogAdapterInstance();
  VfsInterface::ModuleInitialize();
  ::OffloadVfsLib(VfsInterface::GetVfsModuleLib());
}
} /* namespace CDE */
