/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2. You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 *
 * local_backup.cc
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup.cc
 *
 * ---------------------------------------------------------------------------------------
 */
#include "local_backup.h"
#include "common/cde_def.h"
#include "local_backup_file_mgr.h"
#include "sql/local_backup/local_backup_file_utils.h"
#ifndef IS_DSTORE_BACKUP_TOOL
#include "handler/ha_cde.h"
#endif
#include "sql/current_thd.h"
#include "sql/local_backup/full_local_backup.h"

/** Note: These are dstore handler Parameters. Need follow the code format
of ha_cde*/
bool dstore_open_restore_mode = false;
bool dstore_can_do_wal_archive = false;
char *dstore_local_backup_meta_path = nullptr;
/* Need access restore meta file by the path when restoring on target node */
char *dstore_restore_meta_path = nullptr;
bool dstore_exit_after_restore = false;
/* Size interval of local backup progress update. Default value is 200MB. */
uint64_t dstore_local_backup_progress_size_interval = 209715200;
bool dstore_meta_json_crc = true;
/*
  Sync interval for full local backup, after this value times write, do a sync
  operation.
*/
uint32_t dstore_local_full_backup_sync_interval = 50;

namespace CDE {

#ifndef IS_DSTORE_BACKUP_TOOL

static const std::string FULL_LB_WAL_DIR_NAME = "wal";

static DSTORE::RetStatus ReadLatestLocalBackupPoint(LocalBackupCmd cmd,
                                                    local_backup_config *config,
                                                    local_backup_point *point) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    if (LocalBackupFileMgr::CreateInstance()) {
      CDE_LOG_ERROR("create instance fail");
      return DSTORE::RetStatus::DSTORE_FAIL;
    }
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->ReadLatestLocalBackupPoint(cmd, config->meta_base_path, point)) {
    CDE_LOG_ERROR("read latest local backup point fail for cmd %u",
                  (uint32_t)cmd);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }

  return DSTORE::RetStatus::DSTORE_SUCC;
}

/**
  Update status and write status to meta json file for full local backup.

  @param[in]  fileMgr Local backup file manager.
  @param[in]  backup Information passed from dstore local backup module to
  callback function.
  @param[in]  path The meta json file path.
*/
static void UpdateFullLocalBackupStatus(LocalBackupFileMgr *fileMgr,
                                        DSTORE::LocalBackup *backup,
                                        std::string &path) {
  if (DSTORE::BackupStatusType::FULL_BACKUP_STATUS == backup->statusType) {
    fileMgr->UpdateStatus(backup->type, backup->status);
    if (fileMgr->WriteMetaJson(path)) {
      CDE_LOG_ERROR("write full local backup meta json with status fail");
    }
  }
}

/**
  Update progress, and write progress to meta json file for full local backup if
  needed.

  @param[in]  fileMgr Local backup file manager.
  @param[in]  backup Information passed from dstore local backup module to
  callback function.
  @param[in]  path The meta json file path.
  @param[in]  forceWrite Whether force to write meta json file.
*/
static void UpdateFullLocalBackupProgress(LocalBackupFileMgr *fileMgr,
                                          DSTORE::LocalBackup *backup,
                                          std::string &path, bool forceWrite) {
  if (backup->status.runningStatus !=
      DSTORE::BackupRunningStatus::IN_PROGRESS) {
    fileMgr->UpdateStatus(backup->type, backup->status);
  }
  if (DSTORE::BackupObjType::BACKUP_WAL == backup->type) {
    DSTORE::WalFileBackup *walBackup =
        static_cast<DSTORE::WalFileBackup *>(backup);
    fileMgr->UpdateWalArchivedSize(walBackup->walArchivedSize);
  } else if (DSTORE::BackupObjType::BACKUP_TABLESPACE == backup->type) {
    DSTORE::TableSpaceBackup *tableSpaceBackup =
        static_cast<DSTORE::TableSpaceBackup *>(backup);
    FullLocalBackupProgress progress;
    fileMgr->GetFullLocalBackupProgress(progress);
    uint64_t lastArchivedSize = progress.progress.archivedSize;
    /* Update backup meta json file. */
    if (tableSpaceBackup->progress.archivedSize - lastArchivedSize >
            dstore_local_backup_progress_size_interval ||
        forceWrite) {
      fileMgr->UpdateProgress(tableSpaceBackup->progress);
      if (fileMgr->WriteMetaJson(path)) {
        CDE_LOG_ERROR("write full local backup meta json with progress fail");
      }
    }
  }
}

/* callback used on Dstore for backup tablespace, wal and control file */
static DSTORE::RetStatus OutputLocalBackupStream(void *targetConfig,
                                                 DSTORE::LocalBackup *backup) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    CDE_LOG_ERROR(
        "the local backup file mgr is not initialized when output local backup "
        "stream");
    return DSTORE::RetStatus::DSTORE_FAIL;
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  local_backup_config *config =
      static_cast<local_backup_config *>(targetConfig);
  DSTORE::BackupObjType backupObjType = backup->type;

  if (backupObjType == DSTORE::BACKUP_RESTORE_POINT) {
    DSTORE::WalRestorePoint *info =
        static_cast<DSTORE::WalRestorePoint *>(backup);

#ifndef NDEBUG
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
#endif

    CDE_LOG_SYSTEM("Local backup restore current flushed plsn %lu",
                   info->flushedPlsn);
    /* Save flushed plsn to restore meta file. */
    if (fileMgr->UpdateRecoveryStartPointInRestoreMeta(config->meta_base_path,
                                                       info->flushedPlsn)) {
      CDE_LOG_ERROR("failed to write recovery point to restore meta file");
      return DSTORE::RetStatus::DSTORE_FAIL;
    }

#ifndef NDEBUG
    DBUG_EXECUTE_IF("local_backup_restore_suicide", {
      static uint64_t suicide_point = 0;
      if (suicide_point == 0) {
        local_backup_point backupPoint;
        if (!fileMgr->ReadLatestLocalBackupPoint(
                LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN,
                config->meta_base_path, &backupPoint)) {
          /* Suicide for test if restore more than an half */
          suicide_point =
              backupPoint.start_recovery_lsn +
              (backupPoint.lsn_point - backupPoint.start_recovery_lsn) / 2;
        }
      }
      if (suicide_point != 0 && info->flushedPlsn >= suicide_point) {
        DBUG_SUICIDE();
      }
    });
    if (!myThreadInited) {
      my_thread_end();
    }
#endif

    return DSTORE::RetStatus::DSTORE_SUCC;
  }

  /* Update full local backup status. */
  if (DSTORE::BackupStatusType::NOT_STATUS != backup->statusType) {
    UpdateFullLocalBackupStatus(fileMgr, backup, config->root_path);
    return DSTORE::RetStatus::DSTORE_SUCC;
  }

  /* write data file: including tablespace, wal and control file. Need check
     whether is in full local backup or wal archive mode for BACKUP_WAL data
     type */
  uint16_t idx = backup->idx;
  bool isFullBackup =
      (DSTORE::BackupObjType::BACKUP_WAL != backup->type) ? true : false;
  if (DSTORE::BackupObjType::BACKUP_WAL == backup->type) {
    DSTORE::WalFileBackup *walBackup =
        static_cast<DSTORE::WalFileBackup *>(backup);
    /* set Let lsnPoint LB_INVALID_LSN as full local backup's components */
    if (LB_INVALID_LSN == walBackup->lsnPoint) {
      isFullBackup = true;
    }
  }

  // TODO: support using OBS for log archive
  if (current_lb_use_obs && !isFullBackup) {
    CDE_LOG_ERROR("Log archive to OBS not supported by now");
    return DSTORE::RetStatus::DSTORE_FAIL;
  }

  /* Wal archive disjoin */
  if (!isFullBackup && DSTORE::BackupObjType::BACKUP_WAL == backup->type &&
      (DSTORE::LB_SET_WAL_ARCHIVE_DISJOIN & backup->flag)) {
    DSTORE::WalFileBackup *walBackup =
        static_cast<DSTORE::WalFileBackup *>(backup);

    CDE_LOG_ERROR("wal archive disjoin, lsnPoint %lu", walBackup->lsnPoint);
    if (DBUG_EVALUATE_IF("write_wal_archive_disjoin_meta_fail", true, false) ||
        fileMgr->WriteWalArchiveMeta(walBackup->timePoint, walBackup->lsnPoint,
                                     config->meta_base_path, true)) {
      CDE_LOG_ERROR("write wal archive meta for disjoin fail");
      return DSTORE::RetStatus::DSTORE_FAIL;
    }

#ifndef NDEBUG
    if (DBUG_EVALUATE_IF("wal_archive_disjoin_extra_inject", true, false)) {
      /* Extra test for wal archive disjoin. Error will be printed in log
      message and checked in MTR test case. */
      std::vector<std::shared_ptr<local_backup_point>> timePointList;
      MetaFileCursor parentCursor;
      parentCursor.beginOffset = 0;
      fileMgr->ShowTimePointFromFullBackupMetaOffset(
          timePointList, config->meta_base_path, 0, parentCursor);
      MetaFileCursor childCursor;
      childCursor.beginOffset = 0;
      fileMgr->ShowTimePointFromWalArchiveMetaOffset(
          timePointList, config->meta_base_path, 0, parentCursor, childCursor);
    }
#endif
    return DSTORE::RetStatus::DSTORE_SUCC;
  }

  if (LB_INVALID_IDX == idx) {
    bool isCreated = false;
    std::string outFileName = backup->fileName;
    std::string dataDir(config->data_base_path);
    bool createDir = false;
    if (DSTORE::BackupObjType::BACKUP_WAL == backup->type && isFullBackup) {
      dataDir.append("/").append(FULL_LB_WAL_DIR_NAME);
      createDir = true;
    }
    if (!current_lb_use_obs ||
        DSTORE::BackupObjType::BACKUP_CONTROL_FILE == backup->type) {
      idx = fileMgr->AddLocalBackupFile(LocalBackupFileType::DATA_LB,
                                        isFullBackup, dataDir, &outFileName,
                                        isCreated, createDir);
    } else {
      // Not use new object for each file when using OBS.
      idx = fileMgr->GetCurrentObsObjIdx(backup->type);
      if (LB_INVALID_IDX == idx) {
        // Use name of first file as group name
        std::string groupName("group_");
        groupName.append(outFileName);
        idx = fileMgr->AddLocalBackupFile(
            LocalBackupFileType::DATA_LB, isFullBackup, dataDir, &groupName,
            isCreated, createDir, WITH_SEPERATED_META);
        fileMgr->ClearCurrentObsObj(backup->type);
        fileMgr->SetCurrentObsObjIdx(backup->type, idx);
      }
      fileMgr->AddFileToCurrentObsObj(backup->type, outFileName);
    }
    if (LB_INVALID_IDX == idx) {
      CDE_LOG_ERROR(
          "add local backup file fail for data LB and isFullBackup %u",
          (uint32_t)isFullBackup);
      return DSTORE::RetStatus::DSTORE_FAIL;
    }
    backup->idx = idx;
  }
  LocalBackupFile *backupFile = fileMgr->GetLocalBackupFile(
      LocalBackupFileType::DATA_LB, isFullBackup, idx);
  uint64_t *writeTimes =
      isFullBackup ? fileMgr->GetWriteTimes(backupObjType) : nullptr;
  if (backupFile->WriteDataBuffer(backup->buffer, backup->length,
                                  backup->offset, writeTimes)) {
    CDE_LOG_ERROR(
        "backup file %s write data buffer fail, length %d and offset %lu",
        backupFile->GetFileNamePtr()->c_str(), backup->length, backup->offset);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }

  if (current_lb_use_obs &&
      (backup->type == DSTORE::BackupObjType::BACKUP_TABLESPACE ||
       backup->type == DSTORE::BackupObjType::BACKUP_WAL)) {
    fileMgr->UpdateCurrentObsObjSize(backup->type, backup->length);
  }

#ifndef NDEBUG
  // LCOV_EXCL_START
  if (backup->type == DSTORE::BackupObjType::BACKUP_TABLESPACE) {
    bool myThreadInited = my_thread_is_inited();
    if (!myThreadInited) {
      my_thread_init();  // For using DBUG_ to test
    }
    while (DBUG_EVALUATE_IF("lb_pause_after_first_dstore_tbs_data_arrive", true,
                            false)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    DSTORE::TableSpaceBackup *tableSpaceBackup =
        static_cast<DSTORE::TableSpaceBackup *>(backup);
    const uint64_t lastTbsDataFileSize = 8ULL * 1024 * 1024;  // 8M
    if (tableSpaceBackup->progress.archivedSize >
        tableSpaceBackup->progress.totalSize - lastTbsDataFileSize) {
      while (DBUG_EVALUATE_IF("lb_pause_before_last_dstore_tbs_data_arrive",
                              true, false)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    if (!myThreadInited) {
      my_thread_end();
    }
  }
  // LCOV_EXCL_STOP
#endif

  /* Update full local backup progress. */
  UpdateFullLocalBackupProgress(fileMgr, backup, config->root_path, false);

  if (DSTORE::LB_ONE_FILE_WRITE_FINISH & backup->flag) {
    if (!current_lb_use_obs ||
        backup->type == DSTORE::BackupObjType::BACKUP_CONTROL_FILE) {
      backupFile->Flush();
      backupFile->CloseFile();
    } else {
      // Not close each file for OBS. Merge multi files to one object.
      const uint64_t objMaxSize = 120ULL * 1024 * 1024 * 1024;  // 120G
      if (fileMgr->GetCurrentObsObjSize(backup->type) > objMaxSize) {
        /* Going to exceed size for one sinlge obs object. Start to use a new
        group object for next file. */
        backupFile->Flush();
        backupFile->CloseFile();

        if (fileMgr->WriteCurrentObsObjMeta(backup->type)) {
          CDE_LOG_ERROR("backup file group obs object %s write meta fail",
                        backupFile->GetFileNamePtr()->c_str());
          return DSTORE::RetStatus::DSTORE_FAIL;
        }
        fileMgr->ClearCurrentObsObj(backup->type);
      }
    }
    if (DSTORE::BackupObjType::BACKUP_TABLESPACE == backupObjType) {
      UpdateFullLocalBackupProgress(fileMgr, backup, config->root_path, true);
    }
    if (backupObjType == DSTORE::BACKUP_CONTROL_FILE) {
      /* Acquire BACKUP MDL before BINLOG MDL.
      For CDE unit test there's no THD environment, current_thd is null. */
      if (current_thd != nullptr && prepare_for_full_backup(current_thd)) {
        backup->status.errMsg = "failed to get backup MDL.";
        CDE_LOG_ERROR("prepare for backup fail after backup control file");
        return DSTORE::RetStatus::DSTORE_FAIL;
      }

      /* Add Binlog Posistion logic when one ControlFile is done:
       * a) add backup binlog lock
       * b) rotate current binlog
       * c) fetch active binlog file and GTID info;
       * d) copy the active binlog and index file */
      if (rds_full_local_backup_fetch_gtid &&
          fileMgr->GetBinlogFileMgr()->PrepareConsistentBinlogPoint(
              config->root_path.c_str())) {
        CDE_LOG_ERROR("LocalBackup get binlog point fail");
        return DSTORE::RetStatus::DSTORE_FAIL;
      }
    }
  }

  /* Clean last round job should be done in the start process,
   * write current round meta should be after the buffer write */
  if (DSTORE::BackupObjType::BACKUP_WAL == backupObjType) {
    DSTORE::WalFileBackup *walBackup =
        static_cast<DSTORE::WalFileBackup *>(backup);
    if (walBackup->setPoint) {
      bool isDisjoin = (DSTORE::LB_SET_WAL_ARCHIVE_DISJOIN & backup->flag);
      /* Must Wal archive and finish one round archive, need write meta info */
      if (fileMgr->WriteWalArchiveMeta(walBackup->timePoint,
                                       walBackup->lsnPoint,
                                       config->meta_base_path, isDisjoin)) {
        CDE_LOG_ERROR(
            "write wal archive meta fail, time point %lu, lsn point %lu, "
            "disjoin %u",
            walBackup->timePoint, walBackup->lsnPoint, isDisjoin);
        return DSTORE::RetStatus::DSTORE_FAIL;
      }
      fileMgr->RemoveLocalBackupFile(LocalBackupFileType::DATA_LB, false);
    }
  }

  return DSTORE::RetStatus::DSTORE_SUCC;
}

/* After finish full local backup in dstore and innodb, sql will write local
 * backup point to meta file */
DSTORE::RetStatus WriteFullBackupMetaInfo(local_backup_point &backupPoint,
                                          std::string &path) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->WriteFullLocalBackupMeta(backupPoint, path)) {
    CDE_LOG_ERROR(
        "write full local backup meta fail, time point %lu, lsn point %lu, "
        "start recovery lsn %lu",
        backupPoint.time_point, backupPoint.lsn_point,
        backupPoint.start_recovery_lsn);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }

  return DSTORE::RetStatus::DSTORE_SUCC;
}

bool WriteFullBackupRestoreMeta(local_backup_point &backupPoint,
                                std::string &metaPath,
                                std::string &restoreMetaPath) {
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  local_backup_point expectPoint;
  if (fileMgr->CreateRestoreMeta(backupPoint.time_point, metaPath, expectPoint,
                                 restoreMetaPath, true)) {
    CDE_LOG_ERROR("create full backup restore meta fail with time point %lu",
                  (uint64_t)(backupPoint.time_point));
    return true;
  }
  CDE_ASSERT(expectPoint.start_recovery_lsn == backupPoint.start_recovery_lsn);
  CDE_ASSERT(expectPoint.time_point == backupPoint.time_point);
  CDE_ASSERT(expectPoint.lsn_point == backupPoint.lsn_point);
  return false;
}

/* After finish full local backup in dstore and innodb, sql will write
 * non-dstore part status and error message, backup point info and binlog info
 * to full local backup meta json file */
DSTORE::RetStatus WriteFullBackupMetaJson(std::string *errMsg,
                                          local_backup_point *backupPoint,
                                          std::string &path) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  DSTORE::RetStatus ret = DSTORE::RetStatus::DSTORE_SUCC;
  if (fileMgr->WriteFullLocalBackupMetaJson(errMsg, backupPoint, path)) {
    CDE_LOG_ERROR("write full local backup meta json fail");
    ret = DSTORE::RetStatus::DSTORE_FAIL;
  }

  /*
    After meta json file is written, should clear info except error message in
    full local backup progress. The uncleared error message can be displayed in
    the 'SHOW STATUS' result to clarify the reason of last time full local
    backup failure.
  */
  fileMgr->ClearFullLocalBackupProgressWithoutErrMsg();

  return ret;
}

/* To sync StopFullLocalBackup() for LocalBackupCmd::START_FULL_LB and
LocalBackupCmd::STOP_FULL_LB. */
std::mutex lb_stop_mutex;

/* process cmd of start or stop full local backup */
DSTORE::RetStatus ProcessFullLocalBackup(LocalBackupCmd cmd,
                                         local_backup_config *config,
                                         bool involveWalArchive,
                                         bool involveStandby,
                                         local_backup_point *backupPoint) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();

  DSTORE::PdbId pdbId = GetCurrentPdbId();
  if (LocalBackupCmd::START_FULL_LB == cmd) {
    if ((nullptr == config) || (nullptr == backupPoint)) {
      CDE_LOG_ERROR(
          "process full local backup fail for cmd %u and config or backup "
          "point is null",
          (uint32_t)cmd);
      return DSTORE::DSTORE_FAIL;
    }
    uint16_t flag = DSTORE::NO_OUTSIDE_ADVANCE_WAL_RECYCLE;
    if (involveWalArchive) {
      flag |= DSTORE::ARCHIVER_ADVANCE_WAL_RECYCLE;
    }
    if (involveStandby) {
      flag |= DSTORE::REPLICATION_ADVANCE_WAL_RECYCLE;
    }
    fileMgr->lock();
    fileMgr->ClearFullLocalBackupProgress();
    fileMgr->ClearWriteTimes();
    fileMgr->TryClearRigidIoContext();
    /* TODO check and handle the unfinised jobs */
    DSTORE::FullLocalBackupPoint dstorePoint;
    DSTORE::RetStatus error1 = LocalBackupInterface::StartFullLocalBackup(
        pdbId, OutputLocalBackupStream, static_cast<void *>(config),
        dstorePoint, flag);
    if (DSTORE::DSTORE_SUCC != error1) {
      CDE_LOG_ERROR("start full local backup fail, pdb id %u, flag %u",
                    (uint32_t)pdbId, (uint32_t)flag);
    }

    lb_stop_mutex.lock();
    DSTORE::RetStatus error2 = LocalBackupInterface::StopFullLocalBackup(pdbId);
    lb_stop_mutex.unlock();
    if (DSTORE::DSTORE_SUCC != error2) {
      CDE_LOG_ERROR("stop full local backup fail for pdb id %u from cmd %u",
                    (uint32_t)pdbId, (uint32_t)cmd);
    }

    bool obsError = false;
    if (current_lb_use_obs && DSTORE::DSTORE_SUCC == error1 &&
        DSTORE::DSTORE_SUCC == error2) {
      if (fileMgr->WriteCurrentObsObjMeta(
              DSTORE::BackupObjType::BACKUP_TABLESPACE) ||
          fileMgr->WriteCurrentObsObjMeta(DSTORE::BackupObjType::BACKUP_WAL)) {
        obsError = true;
        CDE_LOG_ERROR("backup file group obs object write meta fail");
      }
    }

    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::DATA_LB, true);
    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB,
                                   true);
    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB,
                                   true);
    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::FULL_BINLOG_META_LB,
                                   true);
    if ((DSTORE::DSTORE_SUCC != error1) || (DSTORE::DSTORE_SUCC != error2) ||
        obsError) {
      fileMgr->unlock();
      return DSTORE::DSTORE_FAIL;
    }
    backupPoint->start_recovery_lsn = dstorePoint.startRecoveryPlsn;
    backupPoint->time_point = dstorePoint.timePoint;
    backupPoint->lsn_point = dstorePoint.archivePlsnPoint;
    fileMgr->unlock();
  } else if (LocalBackupCmd::STOP_FULL_LB == cmd) {
    std::lock_guard<std::mutex> lock(lb_stop_mutex);
    if (DSTORE::DSTORE_SUCC !=
        LocalBackupInterface::StopFullLocalBackup(pdbId)) {
      CDE_LOG_ERROR("stop full local backup fail for pdb id %u",
                    (uint32_t)pdbId);
      return DSTORE::DSTORE_FAIL;
    }
    /* Local backup files in fileMgr will be removed when backup thread exit. */
  } else {
    CDE_LOG_ERROR("can not call process full local backup for cmd %u",
                  (uint32_t)cmd);
    return DSTORE::DSTORE_FAIL;
  }

  return DSTORE::DSTORE_SUCC;
}

/* To sync StopWalArchive() for LocalBackupCmd::START_WAL_ARCHIVE and
LocalBackupCmd::STOP_WAL_ARCHIVE. */
std::mutex lb_archive_stop_mutex;

/* process cmd of start or stop wal archive */
DSTORE::RetStatus ProcessWalArchive(LocalBackupCmd cmd,
                                    local_backup_config *config) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  DSTORE::PdbId pdbId = GetCurrentPdbId();
  if (LocalBackupCmd::START_WAL_ARCHIVE == cmd) {
    fileMgr->lock();
    /* TODO: the StartWalChive no need paremeter startPlsn because the
       START_FULL_LB or SET_WAL_ARCHIVE_PLSN have set the wal archive plsn */
    if (DSTORE::DSTORE_SUCC !=
        LocalBackupInterface::StartWalArchive(pdbId, OutputLocalBackupStream,
                                              static_cast<void *>(config))) {
      std::lock_guard<std::mutex> lock(lb_archive_stop_mutex);
      if (DSTORE::DSTORE_SUCC != LocalBackupInterface::StopWalArchive(pdbId)) {
        CDE_LOG_ERROR("stop wal archive fail from cmd %u", (uint32_t)cmd);
      }
      fileMgr->unlock();
      CDE_LOG_ERROR("start wal archive fail for pdb id %u", (uint32_t)pdbId);
      return DSTORE::DSTORE_FAIL;
    }
    fileMgr->unlock();
  } else if (LocalBackupCmd::STOP_WAL_ARCHIVE == cmd) {
    std::lock_guard<std::mutex> lock(lb_archive_stop_mutex);
    if (DSTORE::DSTORE_SUCC != LocalBackupInterface::StopWalArchive(pdbId)) {
      CDE_LOG_ERROR("stop wal archive fail for pdb id %u", (uint32_t)pdbId);
      return DSTORE::DSTORE_FAIL;
    }

    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::DATA_LB, false);
    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB,
                                   false);
    fileMgr->RemoveLocalBackupFile(LocalBackupFileType::ARCHIVE_PARENT_META_LB,
                                   false);
  } else {
    CDE_LOG_ERROR("can not call process wal archive for cmd %u", (uint32_t)cmd);
    return DSTORE::DSTORE_FAIL;
  }
  return DSTORE::DSTORE_SUCC;
}

bool NeedRestore() { return dstore_open_restore_mode; }

bool CanDoWalArchive() { return dstore_can_do_wal_archive; }

local_backup_config g_restoreConfig;

/*  Before running crash recovery on dstore, in walArchive case, need set wal
 * archive lsn from meta file, and in restore case, need set wal recovery start
 * lsn from meta file.
 */
DSTORE::RetStatus PrepareRecovery(LocalBackupCmd cmd,
                                  local_backup_config *config) {
  DSTORE::PdbId pdbId = GetCurrentPdbId();
  local_backup_point backupPoint;
  if (ReadLatestLocalBackupPoint(cmd, config, &backupPoint)) {
    return DSTORE::DSTORE_FAIL;
  }
  if (LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN == cmd) {
    LocalBackupInterface::SetWalRecoveryStartLsn(
        pdbId, backupPoint.start_recovery_lsn);

    g_restoreConfig.meta_base_path = config->meta_base_path;
    LocalBackupInterface::SetRestoreCallback(OutputLocalBackupStream,
                                             &g_restoreConfig, pdbId,
                                             backupPoint.lsn_point);
    CDE_LOG_SYSTEM("Restore use start_recovery_lsn %lu end_recovery_lsn %lu",
                   backupPoint.start_recovery_lsn, backupPoint.lsn_point);
  } else if (LocalBackupCmd::SET_WAL_ARCHIVE_PLSN == cmd) {
    LocalBackupInterface::SetWalArchiveLsn(pdbId, backupPoint.lsn_point);
  } else {
    CDE_LOG_ERROR("can not call local backup prepare recovery for cmd %u",
                  (uint32_t)cmd);
    return DSTORE::DSTORE_FAIL;
  }
  return DSTORE::DSTORE_SUCC;
}

#endif  // IS_DSTORE_BACKUP_TOOL

/* Based on time point, create one restore meta file in which the full local
 * backup's data dir and wal file list from wal archive will be present
 */
DSTORE::RetStatus CreateRestoreMeta(DSTORE::Timestamp timePoint,
                                    std::string &metaPath,
                                    local_backup_point &expectPoint,
                                    std::string &restoreMetaPath,
                                    bool restoreMetaNameChanged) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->CreateRestoreMeta(timePoint, metaPath, expectPoint,
                                 restoreMetaPath, restoreMetaNameChanged)) {
    CDE_LOG_ERROR("create resotre meta fail with time point %lu",
                  (uint64_t)timePoint);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }

  return DSTORE::RetStatus::DSTORE_SUCC;
}

/* From on restore file which created base on time point, the backup point and
 * dir or file items will be showed. These will be collected to on restore data
 * set together with the meta file. The restore data set is used to restore to
 * one time point.
 */
DSTORE::RetStatus ShowRestoreFileList(DSTORE::Timestamp timePoint,
                                      std::string &metaPath,
                                      local_backup_point &point,
                                      std::string &fullDir,
                                      std::vector<std::string> &walFileList,
                                      bool restoreMetaNameChanged) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->ShowRestoreFileList(timePoint, metaPath, point, fullDir,
                                   walFileList, restoreMetaNameChanged)) {
    CDE_LOG_ERROR("show restore file list fail for time point %lu",
                  (uint64_t)timePoint);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }
  return DSTORE::RetStatus::DSTORE_SUCC;
}

DSTORE::RetStatus ShowTimePointFromFullBackupMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &parentMetaCursor) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->ShowTimePointFromFullBackupMetaOffset(
          timePointList, metaPath, maxExpectCount, parentMetaCursor)) {
    CDE_LOG_ERROR(
        "show time point from full backup meta offset fail and max expect "
        "count %u",
        maxExpectCount);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }
  return DSTORE::RetStatus::DSTORE_SUCC;
}

DSTORE::RetStatus ShowTimePointFromWalArchiveMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &parentMetaCursor, MetaFileCursor &childMetaCursor) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->ShowTimePointFromWalArchiveMetaOffset(
          timePointList, metaPath, maxExpectCount, parentMetaCursor,
          childMetaCursor)) {
    CDE_LOG_ERROR(
        "show time point from wal archive meta offset fail and max expect "
        "count %u",
        maxExpectCount);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }
  return DSTORE::RetStatus::DSTORE_SUCC;
}

/* After finish full local backup in dstore and innodb, sql will write local
 * backup point to meta file */
DSTORE::RetStatus FinishFullBackupBinlog(local_backup_point &backupPoint) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }
  (void)backupPoint;
#ifndef IS_DSTORE_BACKUP_TOOL
  /* Finish the binlog logic:
   * a) release locks of Binlog;
   * b) write binlog meta to files; */
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->GetBinlogFileMgr()->FinishConsistentBinlogPoint(
          dstore_local_backup_meta_path, backupPoint)) {
    CDE_LOG_ERROR(
        "finish full local binlog logic failed, time point %lu, lsn point %lu, "
        "start recovery lsn %lu",
        backupPoint.time_point, backupPoint.lsn_point,
        backupPoint.start_recovery_lsn);
    return DSTORE::RetStatus::DSTORE_FAIL;
  }

#endif

  return DSTORE::RetStatus::DSTORE_SUCC;
}

std::string LocalBackupBinlogPoint::toString() {
  std::ostringstream oss;
  oss << "binlogFile: " << binlogFile << ", Position: " << binlogFilePos
      << ", GTID Set: " << gtidSet << ", lsn Point: " << lsnPoint.lsn_point
      << ", TimePoint: " << lsnPoint.time_point
      << ", Start Recovery lsn: " << lsnPoint.start_recovery_lsn;
  return oss.str();
}

DSTORE::RetStatus ReadFullLocalBackupBinlogMeta(
    std::string &metaPath, std::vector<LocalBackupBinlogPoint> &binlogPoint) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->GetBinlogFileMgr()->ReadBinlogMeta(metaPath, binlogPoint)) {
    CDE_LOG_ERROR("read full local backup meta fail");
    return DSTORE::RetStatus::DSTORE_FAIL;
  }
  return DSTORE::RetStatus::DSTORE_SUCC;
}

/* Read full local backup meta json file, and verify if the crc is matched. */
DSTORE::RetStatus VerifyFullBackupMetaJsonCrc(std::string &path,
                                              ha_checksum &storageCrc,
                                              ha_checksum &calcCrc) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  if (fileMgr->VerifyMetaJsonCrc(path, storageCrc, calcCrc)) {
    CDE_LOG_ERROR("verify full local backup meta json crc fail");
    return DSTORE::RetStatus::DSTORE_FAIL;
  }
  return DSTORE::RetStatus::DSTORE_SUCC;
}

}  // namespace CDE
