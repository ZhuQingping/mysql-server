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
 * full_backup_meta_json.cc
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/full_backup_meta_json.cc
 *
 * ---------------------------------------------------------------------------------------
 */
#include "full_backup_meta_json.h"
#include "local_backup.h"  // LocalBackupBinlogPoint
#include "local_backup_file_mgr.h"
#include "sql/local_backup/full_local_backup.h"

namespace CDE {

static const std::string RUNNING_STATUS_STR[] = {"Not start", "In progress",
                                                 "Completed", "Aborted"};
static const std::string META_JSON_KEY[] = {"timePoint",
                                            "lsnPoint",
                                            "startRecoveryLsn",
                                            "gtid",
                                            "backupEndBinlogFile",
                                            "backupEndPosition",
                                            "nonDstoreRunningStatus",
                                            "nonDstoreErrorMessage",
                                            "tablespaceRunningStatus",
                                            "tablespaceErrorMessage",
                                            "walRunningStatus",
                                            "walErrorMessage",
                                            "controlFileRunningStatus",
                                            "controlFileErrorMessage",
                                            "tablespaceArchivedSize",
                                            "tablespaceTotalSize",
                                            "walArchivedSize",
                                            "srcFileSize",
                                            "fileSize",
                                            "archivedNumber",
                                            "totalNumber",
                                            "backupStartTime",
                                            "backupEndTime",
                                            "spendSeconds",
                                            "archivingFileId",
                                            "archivedFileSize",
                                            "archiveFileList",
                                            "alreadyArchivedFileList"};

void DumpMetaJson(rapidjson::Value &obj,
                  rapidjson::Document::AllocatorType &allocator,
                  std::string *errMsg, FullLocalBackupProgress &progress,
                  local_backup_point *backupPoint,
                  LocalBackupBinlogPoint &binlogPoint) {
  auto &tablespaceStatus = progress.tablespaceStatus;
  auto &walStatus = progress.walStatus;
  auto &controlFileStatus = progress.controlFileStatus;
  auto &prog = progress.progress;
  auto &file = prog.file;

  std::vector<std::pair<MetaJsonKey, MetaJsonValue>> fields;

  /* Backup point info */
  if (backupPoint == nullptr) {
    /* In progress. */
    fields.emplace_back(MetaJsonKey::Time_Point, MetaJsonValue::UInt(0));
    fields.emplace_back(MetaJsonKey::Lsn_Point, MetaJsonValue::UInt(0));
    fields.emplace_back(MetaJsonKey::Start_Recovery_Lsn,
                        MetaJsonValue::UInt(0));
  } else if (backupPoint->time_point > 0 && errMsg != nullptr &&
             errMsg->empty()) {
    /* Completed. */
    fields.emplace_back(MetaJsonKey::Time_Point,
                        MetaJsonValue::UInt64(backupPoint->time_point));
    fields.emplace_back(MetaJsonKey::Lsn_Point,
                        MetaJsonValue::UInt64(backupPoint->lsn_point));
    fields.emplace_back(MetaJsonKey::Start_Recovery_Lsn,
                        MetaJsonValue::UInt64(backupPoint->start_recovery_lsn));
  } else {
    /* Aborted. */
    fields.emplace_back(MetaJsonKey::Time_Point, MetaJsonValue::Int(-1));
    fields.emplace_back(MetaJsonKey::Lsn_Point, MetaJsonValue::Int(-1));
    fields.emplace_back(MetaJsonKey::Start_Recovery_Lsn,
                        MetaJsonValue::Int(-1));
  }

  /* Binlog Info: GTID, Binlog File and position */
  std::string gtid, binlogFile;
  uint64_t binlogPos = 0;
  if (backupPoint != nullptr && backupPoint->time_point > 0) {
    gtid = binlogPoint.gtidSet;
    binlogFile = binlogPoint.binlogFile;
    binlogPos = binlogPoint.binlogFilePos;
  }
  fields.emplace_back(MetaJsonKey::Gtid, MetaJsonValue::String(gtid.c_str()));
  fields.emplace_back(MetaJsonKey::Backup_End_Binlog_File,
                      MetaJsonValue::String(binlogFile.c_str()));
  fields.emplace_back(MetaJsonKey::Backup_End_Position,
                      MetaJsonValue::UInt64(binlogPos));

  /* Non-dstore part status and error message */
  if (errMsg == nullptr || errMsg->empty()) {
    if (backupPoint != nullptr && backupPoint->time_point > 0) {
      fields.emplace_back(
          MetaJsonKey::Non_Dstore_Running_Status,
          MetaJsonValue::String(
              RUNNING_STATUS_STR[static_cast<size_t>(
                                     RunningStatusIndex::Completed)]
                  .c_str()));
    } else {
      fields.emplace_back(
          MetaJsonKey::Non_Dstore_Running_Status,
          MetaJsonValue::String(
              RUNNING_STATUS_STR[static_cast<size_t>(
                                     RunningStatusIndex::In_Progress)]
                  .c_str()));
    }
  } else {
    fields.emplace_back(
        MetaJsonKey::Non_Dstore_Running_Status,
        MetaJsonValue::String(
            RUNNING_STATUS_STR[static_cast<size_t>(RunningStatusIndex::Aborted)]
                .c_str()));
    fields.emplace_back(MetaJsonKey::Non_Dstore_Error_Message,
                        MetaJsonValue::String(errMsg->c_str()));
  }

  /* Progress info */
  auto addStatusFields = [&](MetaJsonKey statusKey, MetaJsonKey errKey,
                             const auto &status) {
    fields.emplace_back(
        statusKey,
        MetaJsonValue::String(
            RUNNING_STATUS_STR[static_cast<size_t>(status.runningStatus)]
                .c_str()));
    if (!status.errMsg.empty()) {
      fields.emplace_back(errKey, MetaJsonValue::String(status.errMsg.c_str()));
    }
  };

  if (prog.totalSize > 0) {
    /* Status fields */
    addStatusFields(MetaJsonKey::Tablespace_Running_Status,
                    MetaJsonKey::Tablespace_Error_Message, tablespaceStatus);
    addStatusFields(MetaJsonKey::Wal_Running_Status,
                    MetaJsonKey::Wal_Error_Message, walStatus);
    addStatusFields(MetaJsonKey::Control_File_Running_Status,
                    MetaJsonKey::Control_File_Error_Message, controlFileStatus);

    fields.emplace_back(MetaJsonKey::Tablespace_Archived_Size,
                        MetaJsonValue::UInt64(prog.archivedSize));
    fields.emplace_back(MetaJsonKey::Tablespace_Total_Size,
                        MetaJsonValue::UInt64(prog.totalSize));
    fields.emplace_back(MetaJsonKey::Wal_Archived_Size,
                        MetaJsonValue::UInt64(progress.walArchivedSize));
    fields.emplace_back(
        MetaJsonKey::Src_File_Size,
        MetaJsonValue::UInt64(prog.totalSize + progress.walArchivedSize));
    fields.emplace_back(MetaJsonKey::File_Size, MetaJsonValue::String(""));
    fields.emplace_back(MetaJsonKey::Archived_Number,
                        MetaJsonValue::UInt(file.archivedNum));
    fields.emplace_back(MetaJsonKey::Total_Number,
                        MetaJsonValue::UInt(prog.needArchive.size()));
    fields.emplace_back(MetaJsonKey::Backup_Start_Time,
                        MetaJsonValue::TimePoint(prog.startTime));
    if (backupPoint != nullptr && backupPoint->time_point > 0) {
      fields.emplace_back(
          MetaJsonKey::Backup_End_Time,
          MetaJsonValue::TimePoint(std::chrono::system_clock::now()));
    }
    fields.emplace_back(MetaJsonKey::Spend_Seconds,
                        MetaJsonValue::Duration(prog.spendSecs));
    fields.emplace_back(MetaJsonKey::Archiving_File_Id,
                        MetaJsonValue::UInt(file.archivingFile));
    fields.emplace_back(MetaJsonKey::Archived_File_Size,
                        MetaJsonValue::UInt64(file.archivedFileSize));

    /* Array fields */
    fields.emplace_back(MetaJsonKey::Archive_File_List,
                        MetaJsonValue::FileIdArray(prog.needArchive));
    fields.emplace_back(MetaJsonKey::Already_Archived_File_List,
                        MetaJsonValue::FileIdArray(prog.alreadyArchived));
  }

  /* Add members to obj. */
  for (auto &[key, field] : fields) {
    const auto index = static_cast<size_t>(key);
    obj.AddMember(rapidjson::StringRef(META_JSON_KEY[index].c_str()),
                  field.ToJson(allocator), allocator);
  }
}

void showFullBackupStatus(char *buff, size_t buffLength) {
  if (!LocalBackupFileMgr::IsInitialized()) {
    LocalBackupFileMgr::CreateInstance();
  }

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  FullLocalBackupProgress progress;
  fileMgr->GetFullLocalBackupProgress(progress);

  std::chrono::duration<double> spendSecs(0.0);
  if (progress.progress.totalSize > 0) {
    spendSecs =
        std::chrono::high_resolution_clock::now() - progress.progress.startTime;
  }

  /* Combine status error message. */
  std::stringstream errMsg;
  bool hasPreviousError = false;

  auto appendError = [&](const std::string &error) {
    if (error.empty()) {
      return;
    }
    if (hasPreviousError) {
      errMsg << "\n";
    }
    errMsg << error;
    hasPreviousError = true;
  };

  appendError(progress.tablespaceStatus.errMsg);
  appendError(progress.walStatus.errMsg);
  appendError(progress.controlFileStatus.errMsg);

  std::stringstream ss;
  if (!errMsg.str().empty()) {
    ss << "error_message: " << errMsg.str() << "\n";
  }
  ss << "current_backup_spend_time: " << spendSecs.count() << " seconds"
     << "\ncurrent_backup_archived_size: " << progress.progress.archivedSize
     << "\ncurrent_backup_total_size: " << progress.progress.totalSize;
#ifndef IS_DSTORE_BACKUP_TOOL
  if (current_lb_use_obs) {
    ss << "\ncurrent_backup_obs_objects: " << get_current_lb_object_count();
  }
#endif
  std::string status = ss.str();
  size_t length =
      buffLength > status.length() ? status.length() : buffLength - 1;

  strncpy_s(buff, buffLength, status.c_str(), length);
}

}  // namespace CDE
