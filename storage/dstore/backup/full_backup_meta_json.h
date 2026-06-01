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
 * full_backup_meta_json.h
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/full_backup_meta_json.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef FULL_BACKUP_META_JSON_H
#define FULL_BACKUP_META_JSON_H

#include <chrono>
#include <string>
#include <vector>
#include "sql/handler.h"

#include "local_backup/dstore_local_backup_struct.h"

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>

namespace CDE {

/** Full local backup progress. */
struct FullLocalBackupProgress {
  DSTORE::LocalBackupStatus tablespaceStatus;
  DSTORE::LocalBackupStatus walStatus;
  DSTORE::LocalBackupStatus controlFileStatus;
  DSTORE::LocalBackupProgress progress;
  uint64_t walArchivedSize;

  /** Constructor. */
  FullLocalBackupProgress() { Clear(); }

  /** Clear all info in full local backup progress. */
  void Clear() {
    ClearProgress();
    tablespaceStatus.errMsg.clear();
    walStatus.errMsg.clear();
    controlFileStatus.errMsg.clear();
  }

  /** Clear info except error message in full local backup progress. */
  void ClearProgress() {
    tablespaceStatus.runningStatus = DSTORE::BackupRunningStatus::IN_PROGRESS;
    walStatus.runningStatus = DSTORE::BackupRunningStatus::IN_PROGRESS;
    controlFileStatus.runningStatus = DSTORE::BackupRunningStatus::IN_PROGRESS;
    progress.needArchive.clear();
    progress.alreadyArchived.clear();
    progress.startTime =
        std::chrono::time_point<std::chrono::system_clock,
                                std::chrono::duration<double>>();
    progress.spendSecs = std::chrono::duration<double>(0.0);
    progress.file.archivedNum = 0;
    progress.file.archivingFile = DSTORE::INVALID_VFS_FILE_ID;
    progress.file.archivedFileSize = 0;
    progress.totalSize = 0;
    progress.archivedSize = 0;
    walArchivedSize = 0;
  }

  /**
    Update status info in full local backup progress.

    @param[in]  backupObjType The backup type.
    @param[in]  status The status info.
  */
  void UpdateStatus(DSTORE::BackupObjType &backupObjType,
                    DSTORE::LocalBackupStatus &status) {
    switch (backupObjType) {
      case DSTORE::BackupObjType::BACKUP_TABLESPACE: {
        tablespaceStatus = status;
        break;
      }
      case DSTORE::BackupObjType::BACKUP_WAL: {
        walStatus = status;
        break;
      }
      case DSTORE::BackupObjType::BACKUP_CONTROL_FILE: {
        controlFileStatus = status;
        break;
      }
      default: {
      }
    }
  }
};

/* Running status index, need to match string array RUNNING_STATUS_STR. */
enum class RunningStatusIndex : size_t {
  In_Progress = 1,
  Completed = 2,
  Aborted = 3
};

/* Meta json key, need to match string array META_JSON_KEY. */
enum class MetaJsonKey : size_t {
  Time_Point = 0,
  Lsn_Point,
  Start_Recovery_Lsn,
  Gtid,
  Backup_End_Binlog_File,
  Backup_End_Position,
  Non_Dstore_Running_Status,
  Non_Dstore_Error_Message,
  Tablespace_Running_Status,
  Tablespace_Error_Message,
  Wal_Running_Status,
  Wal_Error_Message,
  Control_File_Running_Status,
  Control_File_Error_Message,
  Tablespace_Archived_Size,
  Tablespace_Total_Size,
  Wal_Archived_Size,
  Src_File_Size,
  File_Size,
  Archived_Number,
  Total_Number,
  Backup_Start_Time,
  Backup_End_Time,
  Spend_Seconds,
  Archiving_File_Id,
  Archived_File_Size,
  Archive_File_List,
  Already_Archived_File_List
};

/** Meta json value. */
struct MetaJsonValue {
  /* Value type. */
  enum class ValueType {
    INT,
    UINT,
    UINT64,
    STRING,
    DOUBLE,
    TIME_POINT,
    DURATION,
    FILE_ID_ARRAY
  };

  ValueType type;
  union {
    int32_t intVal;
    uint32_t uintVal;
    uint64_t uint64Val;
    double doubleVal;
    const char *strVal;
  };
  std::unique_ptr<std::chrono::time_point<std::chrono::system_clock,
                                          std::chrono::duration<double>>>
      timePointVal;
  std::unique_ptr<std::chrono::duration<double>> durationVal;
  std::vector<DSTORE::FileId> arrayVal;

  /* Int value. */
  static MetaJsonValue Int(int32_t val) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::INT;
    metaJsonValue.intVal = val;
    return metaJsonValue;
  }

  /* Uint value. */
  static MetaJsonValue UInt(uint32_t val) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::UINT;
    metaJsonValue.uintVal = val;
    return metaJsonValue;
  }

  /* Uint64 value. */
  static MetaJsonValue UInt64(uint64_t val) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::UINT64;
    metaJsonValue.uint64Val = val;
    return metaJsonValue;
  }

  /* Double value. */
  static MetaJsonValue Double(double val) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::DOUBLE;
    metaJsonValue.doubleVal = val;
    return metaJsonValue;
  }

  /* String value. */
  static MetaJsonValue String(const char *val) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::STRING;
    metaJsonValue.strVal = val;
    return metaJsonValue;
  }

  /* Time point value. */
  static MetaJsonValue TimePoint(
      const std::chrono::time_point<std::chrono::system_clock,
                                    std::chrono::duration<double>> &timePoint) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::TIME_POINT;
    metaJsonValue.timePointVal = std::make_unique<std::chrono::time_point<
        std::chrono::system_clock, std::chrono::duration<double>>>(timePoint);
    return metaJsonValue;
  }

  /* Duration value. */
  static MetaJsonValue Duration(
      const std::chrono::duration<double> &durationValue) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::DURATION;
    metaJsonValue.durationVal =
        std::make_unique<std::chrono::duration<double>>(durationValue);
    return metaJsonValue;
  }

  /* File id array value. */
  static MetaJsonValue FileIdArray(const std::vector<DSTORE::FileId> &ids) {
    MetaJsonValue metaJsonValue;
    metaJsonValue.type = ValueType::FILE_ID_ARRAY;
    metaJsonValue.arrayVal = ids;
    return metaJsonValue;
  }

  /* Convert to rapidjson:Value */
  rapidjson::Value ToJson(rapidjson::Document::AllocatorType &allocator) const {
    switch (type) {
      case ValueType::INT:
        return rapidjson::Value(intVal);
      case ValueType::UINT:
        return rapidjson::Value(uintVal);
      case ValueType::UINT64:
        return rapidjson::Value(uint64Val);
      case ValueType::DOUBLE:
        return rapidjson::Value(doubleVal);
      case ValueType::STRING:
        return rapidjson::Value(strVal, allocator);
      case ValueType::TIME_POINT:
        return rapidjson::Value(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                timePointVal->time_since_epoch())
                .count()));
      case ValueType::DURATION:
        return rapidjson::Value(static_cast<double>(durationVal->count()));
      case ValueType::FILE_ID_ARRAY: {
        rapidjson::Value arr(rapidjson::kArrayType);
        for (const auto &id : arrayVal) {
          arr.PushBack(rapidjson::Value().SetUint(id), allocator);
        }
        return arr;
      }
      default:
        return rapidjson::Value();
    }
  }
};

struct LocalBackupBinlogPoint;
/**
  Dump meta json info.

  @param[out]  obj Single record object of rapidjson
  @param[out]  allocator Allocator of rapidjson
  @param[in]   errMsg The non-dstore part error message
  @param[in]   progress The full local backup progress
  @param[in]   backupPoint The full local backup point
  @param[in]   binlogPoint The full local backup binlog point
*/
void DumpMetaJson(rapidjson::Value &obj,
                  rapidjson::Document::AllocatorType &allocator,
                  std::string *errMsg, FullLocalBackupProgress &progress,
                  local_backup_point *backupPoint,
                  LocalBackupBinlogPoint &binlogPoint);

/** Show full local backup status. */
void showFullBackupStatus(char *buff, size_t buffLength);

}  // namespace CDE

#endif
