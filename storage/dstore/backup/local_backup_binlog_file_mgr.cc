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
 * local_backup_binlog_file_mgr.cc
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup_binlog_file_mgr.cc
 *
 * ---------------------------------------------------------------------------------------
 */

#include "local_backup_binlog_file_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include "local_backup_file_mgr.h"

#include "common/cde_def.h"

namespace CDE {

static const uint32_t LB_VERSION = 1;

bool LocalBackupBinlogFileMgr::ReadBinlogMetaHeader(
    LocalBackupFile *binlogMetaFile, PackHeaderCrc &header,
    uint64_t offset = 0) {
  uchar buf[PACKHEADERCRC_BYTE_SIZE + 1];
  if (binlogMetaFile->ReadSimple((void *)buf, PACKHEADERCRC_BYTE_SIZE,
                                 offset)) {
    return true;
  }

  PackBuffer packBuffer((uchar *)buf, PACKHEADERCRC_BYTE_SIZE);
  return packBuffer.read_header(&header);
}

bool LocalBackupBinlogFileMgr::WriteBinlogMetaHeader(
    LocalBackupFile *binlogMetaFile, PackHeaderCrc &fileHeader) {
  uchar buf[PACKHEADERCRC_BYTE_SIZE + 1];
  PackBuffer packBuffer(buf, PACKHEADERCRC_BYTE_SIZE);
  packBuffer.write_header(fileHeader);
  packBuffer.complete_record();

  return binlogMetaFile->WriteSimple((void *)buf, PACKHEADERCRC_BYTE_SIZE, 0);
}

bool LocalBackupBinlogFileMgr::OpenBinlogMeta(std::string &metaPath,
                                              bool createFlag, bool &isCreated,
                                              LocalBackupFile **metaFile) {
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  LocalBackupFile *binlogMetaFile = fileMgr->GetLocalBackupFile(
      LocalBackupFileType::FULL_BINLOG_META_LB, true, 0);

  if (nullptr == binlogMetaFile) {
#ifndef IS_DSTORE_BACKUP_TOOL
    std::unique_ptr<LocalBackupFile> file(
        current_lb_use_obs
            ? new LocalBackupObsObject(LocalBackupFileType::FULL_BINLOG_META_LB,
                                       metaPath, nullptr)
            : new LocalBackupFile(LocalBackupFileType::FULL_BINLOG_META_LB,
                                  metaPath, nullptr));
#else
    std::unique_ptr<LocalBackupFile> file(new LocalBackupFile(
        LocalBackupFileType::FULL_BINLOG_META_LB, metaPath, nullptr));

#endif
    if (!createFlag && !file->FileExists()) {
      *metaFile = nullptr;
      return false;
    }

    isCreated = false;
    if (DBUG_EVALUATE_IF("full_lb_binlog_open_meta_failed", true, false) ||
        LB_INVALID_IDX == fileMgr->AddLocalBackupFile(
                              LocalBackupFileType::FULL_BINLOG_META_LB, true,
                              metaPath, nullptr, isCreated)) {
      CDE_LOG_ERROR("add local backup binlog meta file fail");
      return true;
    }
    binlogMetaFile = fileMgr->GetLocalBackupFile(
        LocalBackupFileType::FULL_BINLOG_META_LB, true, 0);
  }
  *metaFile = binlogMetaFile;
  return false;
}

bool LocalBackupBinlogFileMgr::ReadBinlogMeta(
    std::string &metaPath, std::vector<LocalBackupBinlogPoint> &binlogPoints) {
  LocalBackupFile *binlogMetaFile = nullptr;

  bool isCreated;
  if (OpenBinlogMeta(metaPath, false, isCreated, &binlogMetaFile)) {
    return true;
  }

  /* file not exists */
  if (isCreated || binlogMetaFile == nullptr) {
    return false;
  }

  PackHeaderCrc fileHeader;
  if (ReadBinlogMetaHeader(binlogMetaFile, fileHeader)) {
    CDE_LOG_ERROR("read local backup binlog meta file header fail");
    return true;
  }

  uint64_t offset = PACKHEADERCRC_BYTE_SIZE;
  while (offset < fileHeader.getSize()) {
    PackHeaderCrc metaHeader;
    if (ReadBinlogMetaHeader(binlogMetaFile, metaHeader, offset)) {
      CDE_LOG_ERROR(
          "read local backup binlog meta header fail, at position %lu", offset);
      return true;
    }

    if (metaHeader.getVersion() == 0) {
      CDE_LOG_ERROR("local backup binlog meta item invalid version");
      return true;
    }

    uint64_t length = metaHeader.getSize();
    char *buf = new (std::nothrow) char[length + 1];
    if (nullptr == buf) {
      CDE_LOG_ERROR("local backup failed to alloc memory, size is %lu", length);
      return true;
    }
    offset += PACKHEADERCRC_BYTE_SIZE;

    if (binlogMetaFile->ReadSimple(buf, length, offset)) {
      // LCOV_EXCL_START
      delete[] buf;
      // LCOV_EXCL_STOP
      CDE_LOG_ERROR(
          "read full local backup meta from %s fail, length is %lu, offset is "
          "%lu",
          binlogMetaFile->GetFileNamePtr()->c_str(), length, offset);
      return true;
    }

    LocalBackupBinlogPoint point;

    PackBuffer packBuffer((uchar *)buf, length);

    /* For Version 1 */
    point.binlogFilePos = packBuffer.read_bytes_8();
    uint32_t filenameSize = packBuffer.read_bytes_4();
    point.binlogFile.resize(filenameSize);
    packBuffer.read_string(point.binlogFile.data(), filenameSize);

    uint32_t gtidSize = packBuffer.read_bytes_4();
    point.gtidSet.resize(gtidSize);
    packBuffer.read_string(point.gtidSet.data(), gtidSize);
    point.lsnPoint.start_recovery_lsn = packBuffer.read_bytes_8();
    point.lsnPoint.time_point = packBuffer.read_bytes_8();
    point.lsnPoint.lsn_point = packBuffer.read_bytes_8();

    /* For Later Version */

    delete[] buf;

    binlogPoints.push_back(std::move(point));
    offset += length;
  }
  return false;
}

#ifndef IS_DSTORE_BACKUP_TOOL

bool LocalBackupBinlogFileMgr::WriteBinlogMeta(
    LocalBackupBinlogPoint &binlogPoint, std::string &path) {
  LocalBackupFile *binlogMetaFile = nullptr;
  uint32_t fileLastPos = PACKHEADERCRC_BYTE_SIZE;

  bool isCreated;
  if (OpenBinlogMeta(path, true /* createFlag */, isCreated, &binlogMetaFile)) {
    return true;
  }

  PackHeaderCrc fileHeader;
  fileHeader.setVersion(LB_VERSION);
  fileHeader.setSize(fileLastPos);

  uint64_t metaFileSize = 0;
  if (binlogMetaFile->GetFileSize(metaFileSize)) {
    CDE_LOG_ERROR("get local backup binlog meta file size fail");
    return true;
  }

  /* File exist */
  if (DBUG_EVALUATE_IF("full_lb_binlog_open_old_meta_failed", true, false) ||
      (!isCreated && metaFileSize > 0 &&
       ReadBinlogMetaHeader(binlogMetaFile, fileHeader))) {
    CDE_LOG_ERROR("read local backup binlog meta file header fail");
    return true;
  }
  fileLastPos = fileHeader.getSize();

  bool ret = false;

  // clang-format off
  /** binlogMeta format :
   *  Meta  					:=  [ File Header ] [ File Body ]
   *
   *  File Header 		:= [ PackHeaderCrc ]
   *
   *  File Body 			:=  [ MetaItem ] .. [ Meta Item ]
   *
   *  Meta Item	  		:=  [ MetaItemHeader ] [ MetaItemBody ]
   *
   *  MetaItemHeader 	:=  [ PackHeaderCrc ]
   *  MetaItemBody 		:=  [ Binlog File Size | 8 Bytes ]
   *            					[ Binlog FileName length | 4 Bytes ]
   *            					[ Binlog FileName string | length Bytes ]
   *            					[ GTID Length | 4 Bytes ]
   *            					[ GTID string | length ]
   *            					[ recoveryLsn | 8 Byes ]
   *            					[ timePoint | 8 Bytes ]
   *            					[ lsn_point | 8 Bytes ] */
  uint32_t length = PACKHEADERCRC_BYTE_SIZE +
                    sizeof(uint64_t) +              /* Binlog File Pos */
                    sizeof(uint32_t) +              /* Binlog FileName length*/
                    binlogPoint.binlogFile.size() + /* Binlog FileName */
                    sizeof(uint32_t) +              /* GTID SET length */
                    binlogPoint.gtidSet.size() +    /* GTID SET String */
                    sizeof(uint64_t) * 3;
  // clang-format on

  uchar *msg_buffer = new (std::nothrow) uchar[length + 1];
  DBUG_EXECUTE_IF("full_lb_write_binlog_meta_alloc_failed", {
    delete[] msg_buffer;
    msg_buffer = nullptr;
  });
  if (nullptr == msg_buffer) {
    CDE_LOG_ERROR(
        "LocalBackup failed to alloc memory for binLog meta buf, size %u",
        length);
    return true;
  }

  PackBuffer packBuffer(msg_buffer, length);
  PackHeaderCrc metaHeader(LB_VERSION, length - PACKHEADERCRC_BYTE_SIZE);
  packBuffer.write_header(metaHeader);
  packBuffer.write_bytes_8(binlogPoint.binlogFilePos);
  packBuffer.write_bytes_4(binlogPoint.binlogFile.size());
  if (binlogPoint.binlogFile.size() > 0) {
    packBuffer.write_string(binlogPoint.binlogFile.data(),
                            binlogPoint.binlogFile.size());
  }
  packBuffer.write_bytes_4(binlogPoint.gtidSet.size());
  if (binlogPoint.gtidSet.size() > 0) {
    packBuffer.write_string(binlogPoint.gtidSet.data(),
                            binlogPoint.gtidSet.size());
  }
  packBuffer.write_bytes_8(binlogPoint.lsnPoint.start_recovery_lsn);
  packBuffer.write_bytes_8(binlogPoint.lsnPoint.time_point);
  packBuffer.write_bytes_8(binlogPoint.lsnPoint.lsn_point);

  packBuffer.complete_record();

  fileHeader.setSize(fileLastPos + length);
  if ((ret = binlogMetaFile->WriteSimple((void *)msg_buffer, length,
                                         fileLastPos))) {
    CDE_LOG_ERROR(
        "LocalBackup failed to write binlog meta file with length %u at "
        "position %u, file is %s",
        length, fileLastPos, binlogMetaFile->GetFileNamePtr()->c_str());
  } else if ((ret = WriteBinlogMetaHeader(binlogMetaFile, fileHeader))) {
    CDE_LOG_ERROR(
        "LocalBackup failed to write binlog meta file header, file is %s",
        binlogMetaFile->GetFileNamePtr()->c_str());
  } else {
    CDE_LOG_SYSTEM("LocalBackup write binlog meta %s",
                   binlogPoint.toString().c_str());
  }
  binlogMetaFile->Flush();
  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();
  fileMgr->RemoveLocalBackupFile(LocalBackupFileType::FULL_BINLOG_META_LB,
                                 true);
  delete[] msg_buffer;

  return ret;
}

bool LocalBackupBinlogFileMgr::PrepareConsistentBinlogPoint(
    const char *backupDataPath) {
  return m_binlogMgr.get_consistent_binlog_point(backupDataPath);
}

void LocalBackupBinlogFileMgr::GetBinlogPoint(LocalBackupBinlogPoint &point) {
  point.binlogFile = m_binlogMgr.get_binlog_file_name();
  point.binlogFilePos = m_binlogMgr.get_binlog_file_pos();
  point.gtidSet = m_binlogMgr.get_gtid_set();
}

bool LocalBackupBinlogFileMgr::FinishConsistentBinlogPoint(
    const char *backupMetaPath, local_backup_point &backupPoint) {
  m_binlogMgr.release_resource();

  if (!m_binlogMgr.is_binlog_open()) {
    return false;
  }

  std::string metaPath(backupMetaPath);
  LocalBackupBinlogPoint point;
  GetBinlogPoint(point);
  point.lsnPoint = backupPoint;
  return WriteBinlogMeta(point, metaPath);
}
#endif

}  // namespace CDE
