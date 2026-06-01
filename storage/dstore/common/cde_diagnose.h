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

#ifndef __CDE_DIAGNOSE_H__
#define __CDE_DIAGNOSE_H__

#include <stdio.h>

#include "backup/full_backup_meta_json.h"
#include "diagnose/dstore_bg_page_writer_diagnose.h"
#include "diagnose/dstore_buf_mgr_diagnose.h"
#include "diagnose/dstore_checkpointer_diagnose.h"
#include "diagnose/dstore_csn_mgr_diagnose.h"
#include "diagnose/dstore_diagnose.h"
#include "diagnose/dstore_double_write_diagnose.h"
#include "diagnose/dstore_lock_mgr_diagnose.h"
#include "diagnose/dstore_lwlock_diagnose.h"
#include "diagnose/dstore_memory_diagnose.h"
#include "diagnose/dstore_wal_diagnose.h"
#include "framework/dstore_instance_interface.h"
#include "include/mysql/status_var.h"
namespace CDE {

struct StatusExportVars {
  char localBackupFullBackupStatus[SHOW_VAR_FUNC_BUFF_SIZE];
  uint64_t dstoreDirtyPageCnt;
  uint64_t dstorePageFlushCnt;
  uint64_t dstorePagesGetCnt;
  uint64_t dstorePagesHitCnt;
  uint64_t dstoreCrBufferGetCnt;
  uint64_t dstoreCrBufferHitCnt;

  /* Walstream plsn info */
  uint64_t minWalStandbyFlushedPlsn;
  uint64_t binlogMaxSyncPlsn;
  uint64_t pageWriteRecoveryPlsn;
  uint64_t recoveryPlsnForTaurus;
  uint64_t maxFlushFinishPlsn;
  uint64_t maxAppendedPlsn;
  uint64_t maxWrittenToFilePlsn;
  uint64_t archivePlsn;
  uint64_t backedUpPlsn;

  /* walstream checkpoint info */
  uint64_t dstoreCkptTime;
  uint64_t dstoreDiskCkpt;

  uint32_t lwlockBlockedCount;

  /* Double write info */
  bool isDoubleWriteInitNormal;
};
extern struct StatusExportVars g_statusExport;

struct StatusExportModuleData {
  DSTORE::BufferPoolGlobalStatusInfo bufferPoolInfo;
  DSTORE::WalGlobalPlsnInfo walPlsnInfo;
  DSTORE::WalGlobalCkpInfo walCkptInfo;
};

void CollectDstoreStatus(FILE *fp);

void UpdateStatusExport(const StatusExportModuleData &exportData);
void DstoreExportStatus();
void DstoreShowInfo(const char *func_name, const char *func_arg,
                    std::string &res);
}  // namespace CDE

#endif  // __CDE_DIAGNOSE_H__