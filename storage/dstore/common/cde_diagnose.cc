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

#include <stdint.h>
#include <mutex>

#include "common/cde_def.h"
#include "common/cde_diagnose.h"
#include "mysql/status_var.h"

namespace CDE {

StatusExportVars g_statusExport;
/** Mutex to prevent access dstore status info concurrently. */
static std::mutex g_collectDstoreInfoMtx;
static std::mutex g_updateStatusExportMtx;

static void GetWalStreamInfo(FILE *fp) {
  DSTORE::WalDiagnose wal;
  DSTORE::WalStreamStateInfo *items{nullptr};
  uint32_t streamCount = 0;
  char *errMsg{nullptr};

  DSTORE::RetStatus ret = wal.GetWalStreamInfoLocally(
      DSTORE::g_defaultPdbId, &items, &streamCount, &errMsg);
  fprintf(fp, "WalStream info\n");
  if (ret == DSTORE::DSTORE_SUCC) {
    for (size_t i = 0; i < streamCount; i++) {
      auto item = &items[i];
      fprintf(fp,
              "pdbId:%u, walId:%lu, usage:%s, state:%s, "
              "walFileCount:%u, headFileName:%s, tailFileName:%s, "
              "flushedPlsn:%lu, replayedPlsn:%lu, maxAppendedPlsn:%lu, "
              "maxWrittenToFilePlsn:%lu, redoStartPlsn:%lu, "
              "redoFinishedPlsn:%lu, singleFlushPlsnBeforePromote:%lu, "
              "singleReplayPlsnAfterDemote:%lu, RoleMode:%s \n",
              item->pdbId, item->walId, item->usage, item->state,
              item->walFileCount, item->headFileName, item->tailFileName,
              item->maxFlushFinishPlsn, item->redoDonePlsn,
              item->maxAppendedPlsn, item->maxWrittenToFilePlsn,
              item->redoStartPlsn, item->redoFinishedPlsn,
              item->singleFlushPlsnBeforePromote,
              item->singleReplayPlsnAfterDemote,
              item->isSingleStandby ? "Standby" : "Primary");
    }
    DSTORE::DestroyObject((void **)&items);
  }
}

static void GetBpPageWriterInfo(FILE *fp) {
  DSTORE::BgPageWriterDiagnose bpPage(DSTORE::g_defaultPdbId);
  char *info = bpPage.GetBgPageWriterSummaryInfo();
  if (info != nullptr) {
    fprintf(fp, "%s\n\n", info);
    DSTORE::DestroyObject((void **)&info);
  }
}

static void GetCsnInfo(FILE *fp) {
  DSTORE::CsnMgrDiagnose csnMgr;
  char *info = csnMgr.DumpCsnMgr();
  if (info != nullptr) {
    fprintf(fp, "%s\n", info);
    DSTORE::DestroyObject((void **)&info);
  }
}

static void GetMemInfo(FILE *fp) {
  DSTORE::MemoryDiagnose memDiag;
  char *info = memDiag.PrintMemoryInfo();
  if (info != nullptr) {
    fprintf(fp, "%s\n", info);
    DSTORE::DestroyObject((void **)&info);
  }
}

static void GetBufMgrInfo(FILE *fp) {
  DSTORE::BufMgrDiagnose buf;
  char *info = buf.PrintBufMgrStatistics();
  if (info != nullptr) {
    fprintf(fp, "%s\n", info);
    DSTORE::DestroyObject((void **)&info);
  }
}

/**
Get DStore lwlock info

@param[in]      fp  File handler for store data
*/
static void GetLwlockInfo(FILE *fp) {
  fprintf(fp, "All threads lwlock info\n");
  DSTORE::LWLockDiagnose lwLock;
  char *info = lwLock.GetLWLockStatus();
  if (info != nullptr) {
    fprintf(fp, "%s\n", info);
    DSTORE::DestroyObject((void **)&info);
  }
}

/**
Collect DStore diagnose info and output to the file.

@param[in]      fp  File handler for store data
*/
void CollectDstoreStatus(FILE *fp) {
  if (!fp) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_collectDstoreInfoMtx);

  GetWalStreamInfo(fp);

  GetBpPageWriterInfo(fp);

  GetBufMgrInfo(fp);

  GetLwlockInfo(fp);

  GetCsnInfo(fp);

  GetMemInfo(fp);

  lock.unlock();

  fflush(fp);
}

void UpdateStatusExport(const StatusExportModuleData &exportData) {
  std::unique_lock<std::mutex> lock(g_updateStatusExportMtx);

  g_statusExport.dstorePagesGetCnt = exportData.bufferPoolInfo.numberPagesGet;
  g_statusExport.dstorePagesHitCnt = exportData.bufferPoolInfo.numberPagesHit;
  g_statusExport.dstoreCrBufferGetCnt =
      exportData.bufferPoolInfo.numberCrBuffersGet;
  g_statusExport.dstoreCrBufferHitCnt =
      exportData.bufferPoolInfo.numberCrBuffersHit;
  g_statusExport.dstoreDirtyPageCnt =
      exportData.bufferPoolInfo.modifiedDatabasePages;
  g_statusExport.dstorePageFlushCnt =
      exportData.bufferPoolInfo.numberPagesWriten;

  g_statusExport.minWalStandbyFlushedPlsn =
      exportData.walPlsnInfo.minWalStandbyFlushedPlsn;
  g_statusExport.binlogMaxSyncPlsn = exportData.walPlsnInfo.binlogMaxSyncPlsn;
  g_statusExport.pageWriteRecoveryPlsn =
      exportData.walPlsnInfo.pageWriteRecoveryPlsn;
  g_statusExport.recoveryPlsnForTaurus =
      exportData.walPlsnInfo.recoveryPlsnForTaurus;
  g_statusExport.maxFlushFinishPlsn = exportData.walPlsnInfo.maxFlushFinishPlsn;
  g_statusExport.maxAppendedPlsn = exportData.walPlsnInfo.maxAppendedPlsn;
  g_statusExport.maxWrittenToFilePlsn =
      exportData.walPlsnInfo.maxWrittenToFilePlsn;
  g_statusExport.archivePlsn = exportData.walPlsnInfo.archivePlsn;
  g_statusExport.backedUpPlsn = exportData.walPlsnInfo.backedUpPlsn;

  g_statusExport.dstoreCkptTime = exportData.walCkptInfo.ckpTimestamp;
  g_statusExport.dstoreDiskCkpt = exportData.walCkptInfo.diskRecoveryPlsn;

  showFullBackupStatus(g_statusExport.localBackupFullBackupStatus,
                       SHOW_VAR_FUNC_BUFF_SIZE);

  lock.unlock();
}

void DstoreExportStatus() {
  StatusExportModuleData exportData;

  DSTORE::RetStatus ret = DSTORE::BufMgrDiagnose::GetBufferPoolGlobalStatusInfo(
      &exportData.bufferPoolInfo);
  if (ret != DSTORE::DSTORE_SUCC) {
    return;
  }

  ret = DSTORE::WalDiagnose::GetWalPlsnInfo(exportData.walPlsnInfo);
  if (ret != DSTORE::DSTORE_SUCC) {
    return;
  }

  ret = DSTORE::CheckpointerDiagnose::GetWalCkpInfo(exportData.walCkptInfo);
  if (ret != DSTORE::DSTORE_SUCC) {
    return;
  }

  /* Get the result of double write initialization. */
  ret = DSTORE::DoubleWriteDiagnose::GetDoubleWriteInfo(
      g_statusExport.isDoubleWriteInitNormal);
  DBUG_EXECUTE_IF("get_double_write_info_fail", { ret = DSTORE::DSTORE_FAIL; });
  if (ret != DSTORE::DSTORE_SUCC) {
    CDE_LOG_ERROR("Get double write info failed.");
    g_statusExport.isDoubleWriteInitNormal = false;
    return;
  }

  UpdateStatusExport(exportData);

  g_statusExport.lwlockBlockedCount =
      DSTORE::LWLockDiagnose::GetBlockedThreadCnt();
}

void DstoreShowInfo(const char *func_name, const char *func_arg,
                    std::string &res) {
  DSTORE::DstoreDiagnose::ShowDstoreInfo(func_name, func_arg, res);
}
} /* namespace CDE */