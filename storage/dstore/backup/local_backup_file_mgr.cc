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
 * local_backup_mgr.cc
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup_file_mgr.cc
 *
 * ---------------------------------------------------------------------------------------
 */

#include "local_backup_file_mgr.h"

#include <sys/stat.h>
#include <thread>

#include "common/cde_def.h"
#include "sql/local_backup/full_local_backup.h"
#include "sql/local_backup/local_backup_file_utils.h"

namespace CDE {

LocalBackupFileMgr *LocalBackupFileMgr::m_fileMgr = nullptr;
std::mutex LocalBackupFileMgr::m_instance_mtx;

static const int LB_INVALID_FID = -1;
static const ssize_t LB_ERROR_IO = -1;
static const std::string FULL_LB_PARENT_META_FILE = "fullLocalBackupParentMeta";
static const std::string WAL_ARCHIVE_PARENT_META_FILE = "walArchiveParentMeta";
static const std::string WAL_ARCHIVE_CHILD_META_FILE_PREFIX =
    "WalArchiveChildMeta_";
static const std::string RESTORE_META_FILE_PREFIX = "RestoreMeta_";
static const std::string RESTORE_META_FILE = "RestoreMeta";
static const std::string FULL_LB_META_JSON = "backup_meta.json";
static const std::string FULL_LB_BINLOG_META = "fullLocalBackupBinlogMeta";

static const uint32_t FULL_LB_SUFFIX_SIZE = 20;
static const uint16_t WAL_ARCHIVE_INTERVAL = 1000;
static const uint64_t WAL_ARCHIVE_CHILD_META_FILE_MAXSIZE =
    1024 * 1024 * 32; /*32MB*/
static const uint64_t PARENT_LB_META_BLOCKSIZE =
    64; /* 64 local_backup_point or
         LocalBackupPoint objects */
static const ha_checksum CRC_SEED = 0xDEADCAFE;

LocalBackupFile::LocalBackupFile(const LocalBackupFileType type,
                                 const std::string &path,
                                 const std::string *fileName)
    : m_type(type), m_fd(LB_INVALID_FID) {
  m_fileName = path + "/";
  switch (type) {
    case LocalBackupFileType::DATA_LB: {
      m_fileName += *fileName;
      break;
    }
    case LocalBackupFileType::FULL_PARENT_META_LB: {
      m_fileName += FULL_LB_PARENT_META_FILE;
      break;
    }
    case LocalBackupFileType::ARCHIVE_PARENT_META_LB: {
      m_fileName += WAL_ARCHIVE_PARENT_META_FILE;
      break;
    }
    case LocalBackupFileType::ARCHIVE_CHILD_META_LB: {
      m_fileName += WAL_ARCHIVE_CHILD_META_FILE_PREFIX;
      /* fileName like "1970Y00M00D00H00M00S" */
      m_fileName += *fileName;
      break;
    }
    case LocalBackupFileType::FULL_META_JSON_LB: {
      m_fileName += FULL_LB_META_JSON;
      break;
    }
    case LocalBackupFileType::FULL_BINLOG_META_LB: {
      m_fileName += FULL_LB_BINLOG_META;
      break;
    }
    case LocalBackupFileType::RESTORE_META_LB: {
      m_fileName += RESTORE_META_FILE_PREFIX;
      /* fileName like "1970Y00M00D00H00M00S" */
      m_fileName += *fileName;
      break;
    }
    default: {
    }
  }
}

bool LocalBackupFile::OpenFile(bool create, bool readOnly) {
  if (create) {
    if (LocalBackupFileType::DATA_LB != m_type) {
      m_fd =
          LBopenOther(m_fileName.c_str(), O_CREAT | O_EXCL | O_DSYNC | O_RDWR,
                      S_IRUSR | S_IWUSR | S_IRGRP);
    } else {
      m_fd = LBopenOther(m_fileName.c_str(), O_CREAT | O_EXCL | O_RDWR,
                         S_IRUSR | S_IWUSR | S_IRGRP);
    }
  } else {
    if (readOnly) {
      m_fd = LBOpen(m_fileName.c_str(), O_RDONLY);
    } else {
      if (LocalBackupFileType::DATA_LB != m_type) {
        m_fd = LBOpen(m_fileName.c_str(), O_DSYNC | O_RDWR);
      } else {
        m_fd = LBOpen(m_fileName.c_str(), O_RDWR);
      }
    }
  }
  if (m_fd < 0) {
    CDE_LOG_ERROR(
        "open file %s fail when create is %d and read only is %d, errno is %d",
        m_fileName.c_str(), create, readOnly, errno);
    return true;
  }
  if (create && (LocalBackupFileType::ARCHIVE_CHILD_META_LB == m_type)) {
    WalArchiveMetaHead metaHead;
    InitWalArchiveChildMetaHead(metaHead);
    if (WriteWalArchiveChildMetaHeader(metaHead)) {
      CloseFile();
      return true;
    }
  }
  return false;
}

bool LocalBackupFile::CloseFile() {
  if (m_fd < 0) {
    return true;
  }
  if (0 != LBClose(m_fd)) {
    CDE_LOG_ERROR("close file %s fail and errno is %d", m_fileName.c_str(),
                  errno);
    return true;
  }
  m_fd = LB_INVALID_FID;
  return false;
}

bool LocalBackupFile::TruncateFile(uint64_t size) {
  if (m_fd < 0) {
    return true;
  }
  if (0 != LBFtruncate(m_fd, size)) {
    CDE_LOG_ERROR("truncate file %s fail and errno is %d", m_fileName.c_str(),
                  errno);
    return true;
  }
  return false;
}

bool LocalBackupFile::GetFileSize(uint64_t &size) {
  struct stat st;
  if (0 != LBFstat(m_fd, &st)) {
    CDE_LOG_ERROR("get file %s size fail and errno is %d", m_fileName.c_str(),
                  errno);
    return true;
  }
  size = st.st_size;
  return false;
}

bool LocalBackupFile::ReadSimple(void *buffer, uint64_t length,
                                 uint64_t offset) {
  ssize_t res = LBPread(m_fd, buffer, length, offset);
  if (LB_ERROR_IO == res) {
    CDE_LOG_ERROR("pread file %s fail and errno is %d", m_fileName.c_str(),
                  errno);
    return true;
  }
  if ((ssize_t)length != res) {
    CDE_LOG_ERROR("pread file %s size is %lu and expect size is %lu",
                  m_fileName.c_str(), (uint64_t)res, length);
    return true;
  }

  return false;
}

bool LocalBackupFile::WriteSimple(void *buffer, uint64_t dataLength,
                                  uint64_t offset) {
  uint64_t writeBegin = offset;
  if (LB_INVALID_OFFSET == writeBegin) {
    if (GetFileSize(writeBegin)) {
      return true;
    }
  }
  ssize_t res = LBPwrite(m_fd, buffer, dataLength, writeBegin);
  if (LB_ERROR_IO == res) {
    CDE_LOG_ERROR("pwrite file %s fail and errno is %d", m_fileName.c_str(),
                  errno);
    return true;
  }
  if ((ssize_t)dataLength != res) {
    CDE_LOG_ERROR("pwrite file %s size is %lu and expect size is %lu",
                  m_fileName.c_str(), (uint64_t)res, dataLength);
    return true;
  }
  return false;
}

bool LocalBackupFile::Flush() {
  int res = LBFsync(m_fd);
  if (0 != res) {
    CDE_LOG_ERROR("flush file %s fail and errno is %d", m_fileName.c_str(),
                  errno);
    return true;
  }
  return false;
}

bool LocalBackupFile::FileExists() {
  struct stat s;
  if (0 != LBStat(m_fileName.c_str(), &s)) {
    return false;
  }
  return true;
}

void LocalBackupFile::AddWriteFlag(uint32_t flag) { (void)flag; }

std::string *LocalBackupFile::GetFileNamePtr() { return &m_fileName; }

bool LocalBackupFile::WriteWalArchiveChildMetaHeader(WalArchiveMetaHead &head) {
  bool ret = WriteSimple((void *)&head, LB_FILE_HEAD_ATOMIC_SIZE, 0);
  Flush();
  return ret;
}

bool LocalBackupFile::ReadPointItemFromRestoreMeta(local_backup_point *point) {
  return ReadSimple((void *)point, sizeof(local_backup_point), 0);
}

bool LocalBackupFile::ReadParentMeta(char *buffer, uint64_t maxBufSize,
                                     uint64_t fileSize, uint64_t position,
                                     uint64_t &realReadSize) {
  realReadSize = (((fileSize - position) > maxBufSize) ? maxBufSize
                                                       : (fileSize - position));
  return ReadSimple(buffer, realReadSize, position);
}

bool LocalBackupFile::ReadParentLastItem(LocalBackupPoint &item) {
  if (LocalBackupFileType::FULL_PARENT_META_LB != m_type &&
      LocalBackupFileType::ARCHIVE_PARENT_META_LB != m_type) {
    CDE_LOG_ERROR("can not read parent last item for type %u",
                  (uint32_t)m_type);
    return true;
  }

  uint64_t fileSize;
  if (GetFileSize(fileSize)) {
    return true;
  }

  uint64_t readBegin = 0;
  uint64_t readLength = 0;
  if (LocalBackupFileType::ARCHIVE_PARENT_META_LB == m_type) {
    if (fileSize < sizeof(LocalBackupPoint)) {
      CDE_LOG_ERROR("file size %lu is small for file %s", fileSize,
                    m_fileName.c_str());
      return true;
    }
    readBegin = fileSize - sizeof(LocalBackupPoint);
    readLength = sizeof(LocalBackupPoint);
  } else {
    if (fileSize < sizeof(local_backup_point)) {
      return true;
    }
    readBegin = fileSize - sizeof(local_backup_point);
    readLength = sizeof(local_backup_point);
  }

  return ReadSimple(static_cast<void *>(&item), readLength, readBegin);
}

void LocalBackupFile::InitWalArchiveChildMetaHead(
    WalArchiveMetaHead &metaHead) {
  metaHead.m_lastValidPoint.m_time = 0;
  metaHead.m_lastValidPoint.m_lsn = LB_INVALID_LSN;
  metaHead.m_lastValidOffset = 0;
  metaHead.m_lsnDisjoined = LB_INVALID_LSN;
  metaHead.m_validLength = sizeof(WalArchiveMetaHead);
  metaHead.m_itemNum = 0;
  metaHead.m_intervalOffsetNum = 0;
}

bool LocalBackupFile::ReadWalArchiveMetaHead(WalArchiveMetaHead &head) {
  return ReadSimple((void *)&head, sizeof(WalArchiveMetaHead), 0);
}

bool LocalBackupFile::WriteParentMeta(void *item, uint32_t length) {
  bool ret = WriteSimple(item, length, LB_INVALID_OFFSET);
  Flush();
  return ret;
}

bool LocalBackupFile::WriteChildMetaItem(void *item, uint32_t size,
                                         LocalBackupPoint &point) {
  uint64_t writeBegin = 0;
  WalArchiveMetaHead head;
  if (LocalBackupFileType::ARCHIVE_CHILD_META_LB == m_type) {
    if (!ReadWalArchiveMetaHead(head)) {
      /* Only the file length of the valid content is recognized,
         not the physical length of the file. The last physical content
         may be invalid because of last time crash or other error. */
      writeBegin = head.m_validLength;
    } else {
      CDE_LOG_ERROR("read wal archive meta head fail for file %s",
                    m_fileName.c_str());
      return true;
    }
  } else {
    CDE_LOG_ERROR("can not write child meta item for file type %u",
                  (uint32_t)m_type);
    return true;
  }

  if (nullptr != item) {
    /* write meta item to valid end of child item file  */
    if (WriteSimple(item, size, writeBegin)) {
      CDE_LOG_ERROR("write child meta item fail when write simple for file %s",
                    m_fileName.c_str());
      return true;
    }
  }

  if (nullptr == item) {
    head.m_lsnDisjoined = point.m_lsn;
  } else {
    head.m_lastValidOffset = head.m_validLength;
    head.m_lastValidPoint = point;
    head.m_validLength += size;
    head.m_itemNum++;
    if (0 == head.m_itemNum % WAL_ARCHIVE_INTERVAL) {
      head.m_intervalOffsetNum++;
    }
  }
  if (WriteWalArchiveChildMetaHeader(head)) {
    CDE_LOG_ERROR(
        "write child meta item fail when write wal archive child meta header "
        "for file %s",
        m_fileName.c_str());
    return true;
  }

  return false;
}

bool LocalBackupFile::WriteDataBuffer(uint8_t *buffer, uint32_t dataLength,
                                      uint64_t offset, uint64_t *writeTimes) {
  bool ret = WriteSimple(buffer, dataLength, offset);

#ifndef IS_DSTORE_BACKUP_TOOL
  if (current_lb_use_obs) {
    return ret;
  }
#endif

  /*
    If WriteSimple failed due to file not open, try open the file and write
    again.
  */
  DBUG_EXECUTE_IF("lb_write_simple_not_open_fail", {
    ret = true;
    m_fd = LB_INVALID_FID;
  });
  if (ret && (errno == EBADF || m_fd < 0)) {
    OpenFile(false, false);
    ret = WriteSimple(buffer, dataLength, offset);
  }

  if (writeTimes == nullptr) {
    return ret;
  }

  (*writeTimes)++;
  if (dstore_local_full_backup_sync_interval > 0) {
    if (*writeTimes % dstore_local_full_backup_sync_interval == 0) {
      Flush();
    }
  }
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_local_backup_sleep_interval > 0) {
    if (*writeTimes % rds_local_backup_sleep_interval == 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(rds_local_backup_sleep_time_ms));
    }
  }
#endif

  return ret;
}

char *LocalBackupFile::ReadChildMeta(uint64_t &length) {
  if (GetFileSize(length)) {
    CDE_LOG_ERROR("read file size fail for read child meta %s",
                  m_fileName.c_str());
    return nullptr;
  }

  char *metaBuf = new (std::nothrow) char[length];
  if (nullptr == metaBuf) {
    CDE_LOG_ERROR("new char fail for length %lu", length);
    return nullptr;
  }
  if (ReadSimple((void *)metaBuf, length, 0)) {
    delete[] metaBuf;
    CDE_LOG_ERROR("read simple fail when read child meta %s",
                  m_fileName.c_str());
  }
  return metaBuf;
}

bool LocalBackupFile::IsContentFull(uint32_t length, bool &contentFull) {
  contentFull = false;
  uint64_t fileSize;
  if (GetFileSize(fileSize)) {
    CDE_LOG_ERROR("check if file %s content full when get file size",
                  m_fileName.c_str());
    return true;
  }

  uint64_t expectSize = fileSize + length;
  if (LocalBackupFileType::ARCHIVE_CHILD_META_LB == m_type) {
    if (expectSize >= WAL_ARCHIVE_CHILD_META_FILE_MAXSIZE) {
      contentFull = true;
    }
  }

  return false;
}

bool LocalBackupFile::IsDisjoined(bool &disjoined) {
  disjoined = false;
  if (LocalBackupFileType::ARCHIVE_CHILD_META_LB != m_type) {
    return true;
  }
  WalArchiveMetaHead metaHead;
  if (ReadWalArchiveMetaHead(metaHead)) {
    CDE_LOG_ERROR(
        "check if disjoined for file %s when read wal archive meta head",
        m_fileName.c_str());
    return true;
  }
  if (LB_INVALID_LSN != metaHead.m_lsnDisjoined) {
    disjoined = true;
  }

  return false;
}

LocalBackupFileMgr *LocalBackupFileMgr::GetInstance() { return m_fileMgr; }

bool LocalBackupFileMgr::IsInitialized() { return (nullptr != m_fileMgr); }

bool LocalBackupFileMgr::CreateInstance() {
  std::unique_lock<std::mutex> lock(m_instance_mtx);
  if (nullptr == m_fileMgr) {
    m_fileMgr = new (std::nothrow) LocalBackupFileMgr();
  }
  m_fileMgr->ClearFullLocalBackupProgress();
  return (nullptr == m_fileMgr);
}

void LocalBackupFileMgr::DestroyInstance() {
  std::unique_lock<std::mutex> lock(m_instance_mtx);
  if (nullptr != m_fileMgr) {
    delete m_fileMgr;
  }
  m_fileMgr = nullptr;
}

uint16_t LocalBackupFileMgr::AddLocalBackupFile(
    LocalBackupFileType type, bool isFullLB, std::string &path,
    std::string *fileName, bool &create, bool createPath, uint32_t writeFlag) {
  create = false;
#ifndef IS_DSTORE_BACKUP_TOOL
  LocalBackupFile *file =
      current_lb_use_obs
          ? new (std::nothrow) LocalBackupObsObject(type, path, fileName)
          : new (std::nothrow) LocalBackupFile(type, path, fileName);
  if (current_lb_use_obs && type == LocalBackupFileType::DATA_LB) {
    increase_current_lb_object_count();
  }
#else
  LocalBackupFile *file =
      new (std::nothrow) LocalBackupFile(type, path, fileName);
#endif
  file->AddWriteFlag(writeFlag);
  DBUG_EXECUTE_IF("full_lb_file_mgr_add_full_parent_meta_fail", {
    if (type == LocalBackupFileType::FULL_PARENT_META_LB) {
      delete file;
      file = nullptr;
    }
  });
  if (nullptr == file) {
    CDE_LOG_ERROR("new LocalBackupFile object fail when add local backup file");
    return LB_INVALID_IDX;
  }

  if (file->FileExists()) {
    if (file->OpenFile(false, false)) {
      CDE_LOG_ERROR("file open fail when add local backup file");
      delete file;
      file = nullptr;
      return LB_INVALID_IDX;
    }
  } else {
    if (createPath) {
      struct stat info;
      if (LBStat(path.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0) {
        if (LBMkdir(path.c_str(), 0750) != 0) {
          CDE_LOG_ERROR("Create dir %s fail when add local backup file %s: %d",
                        path.c_str(), fileName->c_str(), errno);
          delete file;
          file = nullptr;
          return LB_INVALID_IDX;
        }
      }
    }
    if (file->OpenFile(true, false)) {
      CDE_LOG_ERROR("create new file fail when add local backup file %s",
                    file->GetFileNamePtr()->c_str());
      delete file;
      file = nullptr;
      return LB_INVALID_IDX;
    }
    create = true;
  }

  uint16_t idx = 0;
  switch (type) {
    case LocalBackupFileType::DATA_LB: {
      if (isFullLB) {
        m_fullLock.lock();
        m_fullDataFileList.push_back(file);
        idx = m_fullDataFileList.size() - 1;
        m_fullLock.unlock();
      } else {
        m_walArchiveFileList.push_back(file);
        idx = m_walArchiveFileList.size() - 1;
      }
      break;
    }
    case LocalBackupFileType::ARCHIVE_CHILD_META_LB: {
      if (nullptr != m_walArchiveChildMeta) {
        m_walArchiveChildMeta->CloseFile();
        delete m_walArchiveChildMeta;
      }
      m_walArchiveChildMeta = file;
      break;
    }
    case LocalBackupFileType::ARCHIVE_PARENT_META_LB: {
      if (nullptr != m_walArchiveParentMeta) {
        m_walArchiveParentMeta->CloseFile();
        delete m_walArchiveParentMeta;
      }
      m_walArchiveParentMeta = file;
      break;
    }
    case LocalBackupFileType::FULL_PARENT_META_LB: {
      if (nullptr != m_fullParentMeta) {
        m_fullParentMeta->CloseFile();
        delete m_fullParentMeta;
      }
      m_fullParentMeta = file;
      break;
    }
    case LocalBackupFileType::FULL_META_JSON_LB: {
      if (nullptr != m_fullLbMetaJson) {
        m_fullLbMetaJson->CloseFile();
        delete m_fullLbMetaJson;
      }
      m_fullLbMetaJson = file;
      break;
    }
    case LocalBackupFileType::FULL_BINLOG_META_LB: {
      if (nullptr != m_fullLbBinlogMeta) {
        m_fullLbBinlogMeta->CloseFile();
        delete m_fullLbBinlogMeta;
      }
      m_fullLbBinlogMeta = file;
      break;
    }
    default: {
      CDE_LOG_ERROR("error type %u when add local backup file", (uint32_t)type);
      file->CloseFile();
      delete file;
      file = nullptr;
      return LB_INVALID_IDX;
    }
  }

  file = nullptr;
  return idx;
}

void LocalBackupFileMgr::RemoveLocalBackupFile(LocalBackupFileType type,
                                               bool isFull) {
  switch (type) {
    case LocalBackupFileType::DATA_LB: {
      std::mutex *currentLock = nullptr;
      std::vector<LocalBackupFile *> *filelist = nullptr;
      if (isFull) {
        currentLock = &m_fullLock;
        filelist = &m_fullDataFileList;
      } else {
        filelist = &m_walArchiveFileList;
      }
      if (nullptr != currentLock) {
        currentLock->lock();
      }
      for (uint32_t loop = 0; loop < filelist->size(); loop++) {
        LocalBackupFile *file = (*filelist)[loop];
        file->CloseFile();
        delete file;
        (*filelist)[loop] = nullptr;
      }
      filelist->clear();
      if (isFull) {
        ClearCurrentObsObj(DSTORE::BackupObjType::BACKUP_TABLESPACE);
        ClearCurrentObsObj(DSTORE::BackupObjType::BACKUP_WAL);
      }
      if (nullptr != currentLock) {
        currentLock->unlock();
      }
      break;
    }
    case LocalBackupFileType::FULL_PARENT_META_LB: {
      if (m_fullParentMeta != nullptr) {
        m_fullParentMeta->CloseFile();
        delete m_fullParentMeta;
        m_fullParentMeta = nullptr;
      }
      break;
    }
    case LocalBackupFileType::ARCHIVE_PARENT_META_LB: {
      if (nullptr != m_walArchiveParentMeta) {
        m_walArchiveParentMeta->CloseFile();
        delete m_walArchiveParentMeta;
        m_walArchiveParentMeta = nullptr;
      }
      break;
    }
    case LocalBackupFileType::ARCHIVE_CHILD_META_LB: {
      if (nullptr != m_walArchiveChildMeta) {
        m_walArchiveChildMeta->CloseFile();
        delete m_walArchiveChildMeta;
        m_walArchiveChildMeta = nullptr;
      }
      break;
    }
    case LocalBackupFileType::FULL_META_JSON_LB: {
      if (m_fullLbMetaJson != nullptr) {
        m_fullLbMetaJson->CloseFile();
        delete m_fullLbMetaJson;
        m_fullLbMetaJson = nullptr;
      }
      break;
    }
    case LocalBackupFileType::FULL_BINLOG_META_LB: {
      if (m_fullLbBinlogMeta != nullptr) {
        m_fullLbBinlogMeta->CloseFile();
        delete m_fullLbBinlogMeta;
        m_fullLbBinlogMeta = nullptr;
      }
      break;
    }
    default: {
      CDE_LOG_ERROR("type error %u when remove local backup file",
                    (uint32_t)type);
    }
  }
  DestoryGlobalLbIoContext();
}

void LocalBackupFileMgr::DestoryGlobalLbIoContext() {
  return DestoryLbIoContextSet();
}

void LocalBackupFileMgr::TryClearRigidIoContext() {
  return TryClearRigidIoContextSet();
}

LocalBackupFile *LocalBackupFileMgr::GetLocalBackupFile(
    LocalBackupFileType type, bool isFullLB, const uint16_t index) {
  switch (type) {
    case LocalBackupFileType::DATA_LB: {
      if (isFullLB) {
        if (index >= m_fullDataFileList.size()) {
          return nullptr;
        }
        return m_fullDataFileList[index];
      } else {
        if (index >= m_walArchiveFileList.size()) {
          return nullptr;
        }
        return m_walArchiveFileList[index];
      }
    }
    case LocalBackupFileType::FULL_PARENT_META_LB: {
      return m_fullParentMeta;
    }
    case LocalBackupFileType::ARCHIVE_PARENT_META_LB: {
      return m_walArchiveParentMeta;
    }
    case LocalBackupFileType::ARCHIVE_CHILD_META_LB: {
      return m_walArchiveChildMeta;
    }
    case LocalBackupFileType::FULL_META_JSON_LB: {
      return m_fullLbMetaJson;
    }
    case LocalBackupFileType::FULL_BINLOG_META_LB: {
      return m_fullLbBinlogMeta;
    }
    default: {
      return nullptr;
    }
  }
  return nullptr;
}

void *LocalBackupFileMgr::CreateWalArchiveChildMetaBuf(LocalBackupPoint &point,
                                                       uint32_t &length) {
  uint32_t walArchiveFileNum = m_walArchiveFileList.size();
  if (0 == walArchiveFileNum) {
    length = 0;
    return nullptr;
  }
  /*  meta iteam:
         LocalBackupPoint
         file number
         file1 name length
         file1 name */
  length = sizeof(LocalBackupPoint);
  length += sizeof(uint32_t);
  for (uint32_t loop = 0; loop < walArchiveFileNum; loop++) {
    length += sizeof(uint16_t);
    length += m_walArchiveFileList[loop]->GetFileNamePtr()->length();
  }

  char *buffer = new (std::nothrow) char[length];
  if (nullptr == buffer) {
    CDE_LOG_ERROR(
        "new char fail when create wal archive child meta buf and length %u",
        length);
    return nullptr;
  }

  char *ptr = buffer;
  ((LocalBackupPoint *)ptr)->m_time = point.m_time;
  ((LocalBackupPoint *)ptr)->m_lsn = point.m_lsn;
  ptr += sizeof(LocalBackupPoint);
  *((uint32_t *)ptr) = walArchiveFileNum;
  ptr += sizeof(uint32_t);
  for (uint32_t loop = 0; loop < walArchiveFileNum; loop++) {
    uint32_t nameLength =
        m_walArchiveFileList[loop]->GetFileNamePtr()->length();
    *(uint16_t *)ptr = nameLength;
    ptr += sizeof(uint16_t);
    if (memcpy_s(ptr, length - (ptr - buffer),
                 m_walArchiveFileList[loop]->GetFileNamePtr()->c_str(),
                 nameLength) != 0) {
      delete[] buffer;
      buffer = nullptr;
      CDE_LOG_ERROR(
          "failed to copy file name to wal archive child meta buffer");
      return nullptr;
    }
    ptr += nameLength;
  }
  return buffer;
}

bool LocalBackupFileMgr::WriteWalArchiveMeta(DSTORE::Timestamp timePoint,
                                             uint64_t lsnPoint,
                                             std::string &path,
                                             bool isDisjoin) {
  bool childIsCreated = false;
  bool newChildMeta = false;

  uint32_t length = 0;
  LocalBackupPoint point;
  point.m_time = timePoint;
  point.m_lsn = lsnPoint;

  LocalBackupFile *backupChildFile =
      GetLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB, false, 0);
  if (nullptr != backupChildFile) {
    bool contentFull = false;
    bool disjoined = false;
    if (backupChildFile->IsContentFull(length, contentFull)) {
      CDE_LOG_ERROR(
          "check file content full fail when write wal archive meta for file "
          "%s",
          backupChildFile->GetFileNamePtr()->c_str());
      return true;
    }
    if (!contentFull && backupChildFile->IsDisjoined(disjoined)) {
      CDE_LOG_ERROR(
          "check the file %s if disjoined fail when write wal archive meta "
          "fail",
          backupChildFile->GetFileNamePtr()->c_str());
      return true;
    }
    if (contentFull || disjoined) {
      /* detach the old child meta file, and need create and attach new child
       * meta file */
      RemoveLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB, false);
      backupChildFile = nullptr;
      newChildMeta = true;
    }
  }

  LocalBackupFile *backupParentFile = nullptr;
  if (nullptr == backupChildFile) {
    bool parentIsCreated = false;
    std::string childMetaFile;
    /* get Archive parent meta file object */
    backupParentFile = GetLocalBackupFile(
        LocalBackupFileType::ARCHIVE_PARENT_META_LB, false, 0);

    if (nullptr == backupParentFile) {
      if (LB_INVALID_IDX ==
          AddLocalBackupFile(LocalBackupFileType::ARCHIVE_PARENT_META_LB, false,
                             path, nullptr, parentIsCreated)) {
        CDE_LOG_ERROR("add local backup file fail when write wal archive meta");
        return true;
      }
      backupParentFile = GetLocalBackupFile(
          LocalBackupFileType::ARCHIVE_PARENT_META_LB, false, 0);
    }

    /* construct Archive child meta file name from time */
    if (!parentIsCreated && !newChildMeta) {
      LocalBackupPoint item;
      if (backupParentFile->ReadParentLastItem(item)) {
        CDE_LOG_ERROR(
            "read parent last item fail for file %s when write wal archive "
            "meta",
            backupParentFile->GetFileNamePtr()->c_str());
        return true;
      }
      ConcatenateTimeSuffix(childMetaFile, item.m_time);
    } else {
      ConcatenateTimeSuffix(childMetaFile, timePoint);
    }
    if (LB_INVALID_IDX ==
        AddLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB, false,
                           path, &childMetaFile, childIsCreated)) {
      return true;
    }
    backupChildFile = GetLocalBackupFile(
        LocalBackupFileType::ARCHIVE_CHILD_META_LB, false, 0);
    if (!childIsCreated) {
      newChildMeta = false;
      bool contentFull = false;
      bool disjoined = false;
      if (backupChildFile->IsContentFull(length, contentFull)) {
        CDE_LOG_ERROR(
            "check if file %s content full fail when write wal archive meta",
            backupChildFile->GetFileNamePtr()->c_str());
        return true;
      }
      if (!contentFull && backupChildFile->IsDisjoined(disjoined)) {
        CDE_LOG_ERROR(
            "check file %s if disjoined when write wal archive meta and child "
            "is not create",
            backupChildFile->GetFileNamePtr()->c_str());
        return true;
      }
      if (contentFull || disjoined) {
        CDE_LOG_INFO("the file %s contentFull %u and disjoined %u",
                     backupChildFile->GetFileNamePtr()->c_str(), contentFull,
                     disjoined);
        RemoveLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB,
                              false);
        backupChildFile = nullptr;
        newChildMeta = true;
      }

      if (newChildMeta) {
        RemoveLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB,
                              false);
        childMetaFile.clear();
        ConcatenateTimeSuffix(childMetaFile, timePoint);
        if (LB_INVALID_IDX ==
            AddLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB,
                               false, path, &childMetaFile, childIsCreated)) {
          CDE_LOG_ERROR(
              "add local backup file %s fail when write wal archive meta",
              childMetaFile.c_str());
          return true;
        }
        backupChildFile = GetLocalBackupFile(
            LocalBackupFileType::ARCHIVE_CHILD_META_LB, false, 0);
      }
    }
  }

  char *meta_buffer = nullptr;
  if (!isDisjoin) {
    /* first wal file is not added to the FileList yet */
    if (0 == m_walArchiveFileList.size()) {
      return false;
    }
    meta_buffer = (char *)CreateWalArchiveChildMetaBuf(point, length);
    if (nullptr == meta_buffer) {
      CDE_LOG_ERROR(
          "create wal archive child meta buf fail when write wal archive meta");
      return true;
    }
  }
  if (backupChildFile->WriteChildMetaItem(meta_buffer, length, point)) {
    if (nullptr != meta_buffer) {
      delete[] meta_buffer;
    }
    CDE_LOG_ERROR("write chile meta iteam fail for file %s",
                  backupChildFile->GetFileNamePtr()->c_str());
    return true;
  }

  if (nullptr != meta_buffer) {
    delete[] meta_buffer;
  }

  if (childIsCreated) {
    LocalBackupPoint item;
    item.m_time = timePoint;
    item.m_lsn = lsnPoint;
    if (backupParentFile->WriteParentMeta(static_cast<void *>(&item),
                                          sizeof(LocalBackupPoint))) {
      CDE_LOG_ERROR(
          "write parent meta fail for file %s when write wal archive meta",
          backupParentFile->GetFileNamePtr()->c_str());
      return true;
    }
  }

  return false;
}

bool LocalBackupFileMgr::WriteFullLocalBackupMeta(
    local_backup_point &backupPoint, std::string &path) {
  LocalBackupFile *backupParentFile =
      GetLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB, true, 0);
  if (nullptr == backupParentFile) {
    bool isCreated;
    if (LB_INVALID_IDX ==
        AddLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB, true, path,
                           nullptr, isCreated)) {
      CDE_LOG_ERROR(
          "add local backup file fail when write full local backup meta");
      return true;
    }
    backupParentFile =
        GetLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB, true, 0);
  }

  backupParentFile->WriteParentMeta(&backupPoint, sizeof(local_backup_point));

  RemoveLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB, true);

  return false;
}

bool LocalBackupFileMgr::WriteFullLocalBackupMetaJson(
    std::string *errMsg, local_backup_point *backupPoint, std::string &path) {
  bool error = false;

  if (!path.empty() && WriteMetaJson(path, errMsg, backupPoint)) {
    CDE_LOG_ERROR("create full local backup meta json file fail");
    error = true;
  }

  RemoveLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true);

  return error;
}

void LocalBackupFileMgr::ConcatenateTimeSuffix(std::string &FilePrefix,
                                               DSTORE::Timestamp timePoint) {
  const uint32_t yearStrLen = 5;
  const int32_t baseYear = 1900;
  const int32_t tenDigits = 10;
  const uint32_t tenDigitChars = 2;

  struct tm da;
  localtime_r(&timePoint, &da);
  char buff[FULL_LB_SUFFIX_SIZE + 1];
  char *str = buff;
  size_t len = FULL_LB_SUFFIX_SIZE + 1;

  /* like 2025Y */
  sprintf_s(str, len, "%dY", da.tm_year + baseYear);
  str += yearStrLen;
  /* tm_mon is from 0; like 03M */
  int32_t mon = da.tm_mon + 1;
  if (mon < tenDigits) {
    sprintf_s(str, len - (str - buff), "%d", 0);
    str++;
  }
  sprintf_s(str, len - (str - buff), "%dM", mon);
  if (mon < tenDigits) {
    str += tenDigitChars;
  } else {
    str += (tenDigitChars + 1);
  }
  /* like 12D */
  int32_t day = da.tm_mday;
  if (day < tenDigits) {
    sprintf_s(str, len - (str - buff), "%d", 0);
    str++;
  }
  sprintf_s(str, len - (str - buff), "%dD", day);
  if (day < tenDigits) {
    str += tenDigitChars;
  } else {
    str += (tenDigitChars + 1);
  }
  /* like 08H */
  int32_t hour = da.tm_hour;
  if (hour < tenDigits) {
    sprintf_s(str, len - (str - buff), "%d", 0);
    str++;
  }
  sprintf_s(str, len - (str - buff), "%dH", hour);
  if (hour < tenDigits) {
    str += tenDigitChars;
  } else {
    str += (tenDigitChars + 1);
  }
  /* like 36M */
  int32_t min = da.tm_min;
  if (min < tenDigits) {
    sprintf_s(str, len - (str - buff), "%d", 0);
    str++;
  }
  sprintf_s(str, len - (str - buff), "%dM", min);
  if (min < tenDigits) {
    str += tenDigitChars;
  } else {
    str += (tenDigitChars + 1);
  }
  /* like 09S */
  int32_t sec = da.tm_sec;
  if (sec < tenDigits) {
    sprintf_s(str, len - (str - buff), "%d", 0);
    str++;
  }
  sprintf_s(str, len - (str - buff), "%dS", sec);
  if (sec < tenDigits) {
    str += tenDigitChars;
  } else {
    str += (tenDigitChars + 1);
  }

  str[0] = '\0';
  FilePrefix.append(buff);
}

bool LocalBackupFileMgr::ReadLatestLocalBackupPoint(LocalBackupCmd cmd,
                                                    std::string &path,
                                                    local_backup_point *point) {
  if (LocalBackupCmd::SET_WAL_ARCHIVE_PLSN == cmd) {
    LocalBackupFile *childFile = GetLocalBackupFile(
        LocalBackupFileType::ARCHIVE_CHILD_META_LB, false, 0);
    if (nullptr == childFile) {
      LocalBackupFile *parentFile = GetLocalBackupFile(
          LocalBackupFileType::ARCHIVE_PARENT_META_LB, false, 0);
      if (nullptr == parentFile) {
        LocalBackupFile walParentMetaFile(
            LocalBackupFileType::ARCHIVE_PARENT_META_LB, path,
            &WAL_ARCHIVE_PARENT_META_FILE);
        if (!walParentMetaFile.FileExists()) {
          CDE_LOG_ERROR(
              "file %s is not exists when read last local backup point",
              walParentMetaFile.GetFileNamePtr()->c_str());
          return true;
        }

        bool isCreated = false;
        if (LB_INVALID_IDX ==
            AddLocalBackupFile(LocalBackupFileType::ARCHIVE_PARENT_META_LB,
                               false, path, nullptr, isCreated)) {
          CDE_LOG_ERROR(
              "add local backup file which type %u fail when read last local "
              "backup point",
              (uint32_t)LocalBackupFileType::ARCHIVE_PARENT_META_LB);
          return true;
        }
        parentFile = GetLocalBackupFile(
            LocalBackupFileType::ARCHIVE_PARENT_META_LB, false, 0);
      }
      LocalBackupPoint item;
      std::string childMetaFile;
      if (parentFile->ReadParentLastItem(item)) {
        CDE_LOG_ERROR(
            "read parent file %s last item when read last local backup point",
            parentFile->GetFileNamePtr()->c_str());
        return true;
      }
      ConcatenateTimeSuffix(childMetaFile, item.m_time);
      bool isCreated = false;
      if (LB_INVALID_IDX ==
          AddLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB, false,
                             path, &childMetaFile, isCreated)) {
        CDE_LOG_ERROR(
            "add local backup file which type %u fail when read last local "
            "backup point",
            (uint32_t)LocalBackupFileType::ARCHIVE_CHILD_META_LB);
        return true;
      }
      childFile = GetLocalBackupFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB,
                                     false, 0);
    }

    WalArchiveMetaHead head;
    if (childFile->ReadWalArchiveMetaHead(head)) {
      CDE_LOG_ERROR(
          "read wal archive meta file %s head fail when read last local backup "
          "point",
          childFile->GetFileNamePtr()->c_str());
      return true;
    }

    point->lsn_point = head.m_lastValidPoint.m_lsn;
    point->time_point = head.m_lastValidPoint.m_time;
    /** If the wal archive have disjoined, need to check whether have new full
     * local backup and catch up */
    if (LB_INVALID_LSN != head.m_lsnDisjoined) {
      LocalBackupFile *fullParentFile =
          GetLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB, true, 0);
      if (nullptr == fullParentFile) {
        bool isCreated = false;
        if (LB_INVALID_IDX ==
            AddLocalBackupFile(LocalBackupFileType::FULL_PARENT_META_LB, true,
                               path, nullptr, isCreated)) {
          CDE_LOG_ERROR(
              "add local backup file which type %u fail when read last local "
              "backup point",
              (uint32_t)LocalBackupFileType::FULL_PARENT_META_LB);
          return true;
        }
        fullParentFile = GetLocalBackupFile(
            LocalBackupFileType::FULL_PARENT_META_LB, true, 0);
      }
      LocalBackupPoint fullParentItem;
      if (fullParentFile->ReadParentLastItem(fullParentItem)) {
        CDE_LOG_ERROR(
            "read parent file %s last item fail when read last local backup "
            "point",
            fullParentFile->GetFileNamePtr()->c_str());
        return true;
      }
      if (fullParentItem.m_lsn > point->lsn_point) {
        /* new wal archive need start from the lsn point belong to new full
         * local backup */
        point->lsn_point = fullParentItem.m_lsn;
        point->time_point = fullParentItem.m_time;
      } else {
        /* need do new full local backup */
        CDE_LOG_ERROR(
            "can not catch last full local backup and need do new full local "
            "backup, "
            "full parent item lsn %lu and lsn point %lu",
            fullParentItem.m_lsn, point->lsn_point);
        return true;
      }
    }
  } else if (LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN == cmd) {
    LocalBackupFile restoreMetaFile(LocalBackupFileType::RESTORE_META_LB, path,
                                    &RESTORE_META_FILE);
    if (restoreMetaFile.OpenFile(false, true)) {
      CDE_LOG_ERROR("open file %s fail when read last local backup point",
                    restoreMetaFile.GetFileNamePtr()->c_str());
      return true;
    }
    if (restoreMetaFile.ReadPointItemFromRestoreMeta(point)) {
      restoreMetaFile.CloseFile();
      CDE_LOG_ERROR(
          "read point item from restore meta file %s fail when read last local "
          "backup point",
          restoreMetaFile.GetFileNamePtr()->c_str());
      return true;
    }
    restoreMetaFile.CloseFile();
  } else {
    CDE_LOG_ERROR("error cmd %u when read last local backup point",
                  (uint32_t)cmd);
    return true;
  }

  return false;
}

bool LocalBackupFileMgr::SearchPointFromParentMeta(
    DSTORE::Timestamp timePoint, std::string &path, LocalBackupFileType type,
    local_backup_point *expectFullLBPoint,
    std::vector<LocalBackupPoint> *expectArchivePoint) {
  if (LocalBackupFileType::FULL_PARENT_META_LB != type &&
      LocalBackupFileType::ARCHIVE_PARENT_META_LB != type) {
    return true;
  }

  /* Parent file locates in path 'dstore_local_backup_meta_path' */
#ifndef IS_DSTORE_BACKUP_TOOL
  LocalBackupFileUniquePtr parentFile(
      current_lb_use_obs
          ? new (std::nothrow) LocalBackupObsObject(type, path, nullptr)
          : new (std::nothrow) LocalBackupFile(type, path, nullptr));
#else
  LocalBackupFileUniquePtr parentFile(new (std::nothrow)
                                          LocalBackupFile(type, path, nullptr));
#endif
  if (nullptr == parentFile) {
    CDE_LOG_ERROR(
        "new LocalBackupFile object fail when search point from parent meta");
    return true;
  }
  if (parentFile->OpenFile(false, true)) {
    CDE_LOG_ERROR("file %s open fail when search point from parent meta",
                  parentFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t fileSize = 0;
  if (parentFile->GetFileSize(fileSize)) {
    CDE_LOG_ERROR("get file %s size fail when search point from parent meta",
                  parentFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t maxBufSize = 0;
  uint64_t position = 0;
  uint32_t itemSize = 0;
  if (LocalBackupFileType::FULL_PARENT_META_LB == type) {
    if (fileSize < sizeof(local_backup_point)) {
      CDE_LOG_ERROR("file %s size %lu small than local_backup_point",
                    parentFile->GetFileNamePtr()->c_str(), fileSize);
      return true;
    }
    maxBufSize = sizeof(local_backup_point) * PARENT_LB_META_BLOCKSIZE;
    expectFullLBPoint->start_recovery_lsn = LB_INVALID_LSN;
    itemSize = sizeof(local_backup_point);
  } else {
    if (fileSize < sizeof(LocalBackupPoint)) {
      CDE_LOG_ERROR("file %s size %lu small than LocalBackupPoint",
                    parentFile->GetFileNamePtr()->c_str(), fileSize);
      return true;
    }
    maxBufSize = sizeof(LocalBackupPoint) * PARENT_LB_META_BLOCKSIZE;
    itemSize = sizeof(LocalBackupPoint);
  }

  if (fileSize > maxBufSize) {
    position = fileSize - maxBufSize;
  }

  uint64_t realReadSize = 0;
  uint64_t totalSizeRead = 0;
  char *buffer = new (std::nothrow) char[maxBufSize];
  /* hit point range for wal archive item from parent file */
  bool haveMeet = false;

  while (totalSizeRead < fileSize) {
    if (parentFile->ReadParentMeta(buffer, maxBufSize, fileSize, position,
                                   realReadSize)) {
      delete[] buffer;
      CDE_LOG_ERROR(
          "read parent file %s meta fail when search point from parent meta",
          parentFile->GetFileNamePtr()->c_str());
      return true;
    }

    totalSizeRead += realReadSize;
    uint32_t scanNum = realReadSize / itemSize;
    if (LocalBackupFileType::FULL_PARENT_META_LB == type) {
      local_backup_point *point = (local_backup_point *)(buffer);
      uint32_t loop = 0;
      for (loop = scanNum; loop > 0; loop--) {
        if (point[loop - 1].time_point <= timePoint) {
          expectFullLBPoint->lsn_point = point[loop - 1].lsn_point;
          expectFullLBPoint->start_recovery_lsn =
              point[loop - 1].start_recovery_lsn;
          expectFullLBPoint->time_point = point[loop - 1].time_point;
          break;
        }
      }
      if (0 == loop) {
        position -= realReadSize;
      }
      if (0 != loop) {
        break;
      }
    } else {
      LocalBackupPoint *point = (LocalBackupPoint *)(buffer);
      uint32_t loop = 0;

      for (loop = scanNum; loop > 0; loop--) {
        if (point[loop - 1].m_time <= timePoint) {
          if (point[loop - 1].m_lsn < expectFullLBPoint->lsn_point) {
            if (haveMeet) {
              break;
            }
            haveMeet = true;
          }
          LocalBackupPoint localBackupPoint;
          localBackupPoint.m_lsn = point[loop - 1].m_lsn;
          localBackupPoint.m_time = point[loop - 1].m_time;
          expectArchivePoint->push_back(localBackupPoint);
        }
      }
      if (0 == loop) {
        position -= realReadSize;
      }
      if (0 != loop) {
        break;
      }
    }
  }
  delete[] buffer;
  if (LocalBackupFileType::FULL_PARENT_META_LB == type) {
    if (LB_INVALID_LSN == expectFullLBPoint->start_recovery_lsn) {
      CDE_LOG_ERROR(
          "the start recovery lsn invalid when search point from parent meta");
      return true;
    }
  } else {
    if (0 == expectArchivePoint->size()) {
      CDE_LOG_INFO(
          "expect archive point list empty when search point from parent meta");
    }
  }
  return false;
}

bool LocalBackupFileMgr::CreateRestoreMeta(DSTORE::Timestamp timePoint,
                                           std::string path,
                                           local_backup_point &expectPoint,
                                           std::string &restoreMetaPath,
                                           bool restoreMetaNameChanged) {
  local_backup_point expectFullLBPoint;
  std::vector<LocalBackupPoint> expectArchivePoint;

  /* Step 1: search full local backup meta near timePoint */
  if (SearchPointFromParentMeta(timePoint, path,
                                LocalBackupFileType::FULL_PARENT_META_LB,
                                &expectFullLBPoint, nullptr)) {
    CDE_LOG_ERROR(
        "search point from full parent meta fail when create restore meta");
    return true;
  }

  /* Step 2: search wal archive Parent meta near timePoint based on full LB
   * point */
  if (expectFullLBPoint.time_point != timePoint &&
      SearchPointFromParentMeta(timePoint, path,
                                LocalBackupFileType::ARCHIVE_PARENT_META_LB,
                                &expectFullLBPoint, &expectArchivePoint)) {
    CDE_LOG_WARN(
        "search point from archive parent meta fail when create restore meta");
  }

  expectPoint.start_recovery_lsn = expectFullLBPoint.start_recovery_lsn;
  expectPoint.time_point = expectFullLBPoint.time_point;
  expectPoint.lsn_point = expectFullLBPoint.lsn_point;

  /* Step 3: search wal archive child meta files from LocalBackupPoint list */
  std::vector<LocalBackupFileUniquePtr> childFileList;
  childFileList.reserve(expectArchivePoint.size());
  for (uint32_t loop = 0; loop < expectArchivePoint.size(); loop++) {
    LocalBackupPoint &point = expectArchivePoint[loop];
    std::string childMetaFileName;
    ConcatenateTimeSuffix(childMetaFileName, point.m_time);
    LocalBackupFileUniquePtr file(new (std::nothrow) LocalBackupFile(
        LocalBackupFileType::ARCHIVE_CHILD_META_LB, path, &childMetaFileName));
    if (nullptr == file) {
      CDE_LOG_ERROR("new LocalBackupFile object fail when create restore meta");
      return true;
    }
    if (file->OpenFile(false, true)) {
      CDE_LOG_ERROR("open file %s fail when create restore meta",
                    file->GetFileNamePtr()->c_str());
      return true;
    }
    bool disjoined = false;
    if (file->IsDisjoined(disjoined)) {
      CDE_LOG_ERROR("file %s check if disjoined fail when create restore meta",
                    file->GetFileNamePtr()->c_str());
      return true;
    }

    /*
      It's OK for scenario below:
        Wal archive A1: disjoin
        Full backup B1: full backup after disjoin
        Wal archive A2: disjoin
        Full backup B2: full backup after disjoin
        Wal archive A3

      Restore to time point of wal archive A2, then first and last point in
      'expectArchivePoint' are both disjoined.
      LSN of wal archive A1 is less than LSN of Full backup B1 and will be
      filtered out in wal file list in subsequent code.
    */
    if (disjoined && (loop > 0 && loop != expectArchivePoint.size() - 1)) {
      CDE_LOG_ERROR("The no %u file disjoined when create restore meta", loop);
      return true;
    }
    childFileList.push_back(std::move(file));
  }

  std::vector<std::string> archiveWalFileList;
  uint32_t filenameListLength = 0;
  for (uint32_t loop = childFileList.size(); loop > 0; loop--) {
    LocalBackupFile *file = childFileList[loop - 1].get();
    uint64_t length = 0;
    char *buf = file->ReadChildMeta(length);
    if (nullptr == buf) {
      CDE_LOG_ERROR("read child file %s meta fail when create restore meta",
                    file->GetFileNamePtr()->c_str());
      return true;
    }
    uint64_t pos = sizeof(WalArchiveMetaHead);
    char *ptr = buf;
    ptr += pos;
    while (pos < length) {
      LocalBackupPoint *archivePoint = (LocalBackupPoint *)(ptr);
      ptr += sizeof(LocalBackupPoint);
      pos += sizeof(LocalBackupPoint);
      if (archivePoint->m_time > timePoint) {
        break;
      }
      uint32_t fileNumber = *((uint32_t *)(ptr));
      ptr += sizeof(uint32_t);
      pos += sizeof(uint32_t);
      for (uint32_t i = 0; i < fileNumber; i++) {
        uint16_t nameLength = *(uint16_t *)(ptr);
        ptr += sizeof(uint16_t);
        pos += sizeof(uint16_t);
        std::string walFileName;
        walFileName.append(ptr, nameLength);
        /* Only add distinct wal file to the archiveWalFileList. */
        if (archivePoint->m_lsn >= expectFullLBPoint.lsn_point &&
            std::find(archiveWalFileList.begin(), archiveWalFileList.end(),
                      walFileName) == archiveWalFileList.end()) {
          archiveWalFileList.push_back(walFileName);
          filenameListLength += (sizeof(uint16_t) + walFileName.size());
        }
        ptr += nameLength;
        pos += nameLength;
      }
      if (expectPoint.lsn_point < archivePoint->m_lsn) {
        expectPoint.time_point = archivePoint->m_time;
        expectPoint.lsn_point = archivePoint->m_lsn;
      }
    }
    delete[] buf;
    if (pos < length) {
      break;
    }
  }

  uint32_t metaContentLength = sizeof(local_backup_point) + sizeof(time_t) +
                               sizeof(uint32_t) + filenameListLength;
  char *restoreBuf = new (std::nothrow) char[metaContentLength];
  if (nullptr == restoreBuf) {
    CDE_LOG_ERROR("new buf length %u fail when create restore meta",
                  metaContentLength);
    return true;
  }
  char *restorePtr = restoreBuf;
  ((local_backup_point *)restorePtr)->start_recovery_lsn =
      expectPoint.start_recovery_lsn;
  ((local_backup_point *)restorePtr)->time_point = expectPoint.time_point;
  ((local_backup_point *)restorePtr)->lsn_point = expectPoint.lsn_point;
  restorePtr += sizeof(local_backup_point);
  *((time_t *)restorePtr) = expectFullLBPoint.time_point;
  restorePtr += sizeof(time_t);
  *((uint32_t *)restorePtr) = archiveWalFileList.size();
  restorePtr += sizeof(uint32_t);
  if (0 != archiveWalFileList.size()) {
    for (uint32_t loop = 0; loop < archiveWalFileList.size(); loop++) {
      *((uint16_t *)restorePtr) = (uint16_t)archiveWalFileList[loop].size();
      restorePtr += sizeof(uint16_t);
      if (memcpy_s(restorePtr, metaContentLength - (restorePtr - restoreBuf),
                   archiveWalFileList[loop].c_str(),
                   archiveWalFileList[loop].size()) != 0) {
        delete[] restoreBuf;
        restoreBuf = nullptr;
        CDE_LOG_ERROR("failed to copy wal file name to restore meta buffer");
        return true;
      }
      restorePtr += archiveWalFileList[loop].size();
    }
  }

  std::string restoreFileName;
  if (!restoreMetaNameChanged) {
    ConcatenateTimeSuffix(restoreFileName, expectPoint.time_point);
  } else {
    restoreFileName = RESTORE_META_FILE;
  }
#ifndef IS_DSTORE_BACKUP_TOOL
  LocalBackupFileUniquePtr restoreMetaFile(
      current_lb_use_obs
          ? new (std::nothrow)
                LocalBackupObsObject(LocalBackupFileType::RESTORE_META_LB,
                                     restoreMetaPath, &restoreFileName)
          : new (std::nothrow)
                LocalBackupFile(LocalBackupFileType::RESTORE_META_LB,
                                restoreMetaPath, &restoreFileName));
#else
  LocalBackupFileUniquePtr restoreMetaFile(new (std::nothrow) LocalBackupFile(
      LocalBackupFileType::RESTORE_META_LB, restoreMetaPath, &restoreFileName));
#endif
  if (nullptr == restoreMetaFile) {
    delete[] restoreBuf;
    restoreBuf = nullptr;
    CDE_LOG_ERROR("new LocalBackupFile object fail when create restore meta");
    return true;
  }
  if (restoreMetaFile->OpenFile(true, false)) {
    CDE_LOG_ERROR("file %s open fail when create restore meta",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    delete[] restoreBuf;
    restoreBuf = nullptr;
    return true;
  }
  if (restoreMetaFile->WriteSimple((void *)restoreBuf, metaContentLength, 0)) {
    CDE_LOG_ERROR("write simple file %s fail when create restore meta",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    delete[] restoreBuf;
    restoreBuf = nullptr;
    return true;
  }
  restoreMetaFile->Flush();

  delete[] restoreBuf;
  restoreBuf = nullptr;

  return false;
}

/* The timePoint is from local_backup_point */
bool LocalBackupFileMgr::ShowRestoreFileList(
    DSTORE::Timestamp timePoint, std::string &metaPath,
    local_backup_point &point, std::string &fullDir,
    std::vector<std::string> &walFileList, bool restoreMetaNameChanged) {
  std::string restoreFileName;
  if (!restoreMetaNameChanged) {
    ConcatenateTimeSuffix(restoreFileName, timePoint);
  } else {
    restoreFileName = RESTORE_META_FILE;
  }
  LocalBackupFileUniquePtr restoreMetaFile(new (std::nothrow) LocalBackupFile(
      LocalBackupFileType::RESTORE_META_LB, metaPath, &restoreFileName));
  if (nullptr == restoreMetaFile) {
    CDE_LOG_ERROR("new LocalBackupFile fail when show restore file list");
    return true;
  }
  if (!restoreMetaFile->FileExists()) {
    CDE_LOG_ERROR("restore meta file %s not exists when show restore file list",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    return true;
  }
  if (restoreMetaFile->OpenFile(false, true)) {
    CDE_LOG_ERROR("restore meta file %s open fail when show restore file list",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t length = 0;
  char *metaBuf = restoreMetaFile->ReadChildMeta(length);
  if (nullptr == metaBuf) {
    CDE_LOG_ERROR("read child meta file %s fail when show restore file list",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    return true;
  }
  char *metaPtr = metaBuf;
  point.time_point = ((local_backup_point *)metaPtr)->time_point;
  if (point.time_point != timePoint) {  // TODO:need check valid timePoint not
                                        // copy from user directly
    delete[] metaBuf;
    CDE_LOG_ERROR("expect timepoint %lu is not same with meta timepoint %lu",
                  timePoint, point.time_point);
    return true;
  }
  point.lsn_point = ((local_backup_point *)metaPtr)->lsn_point;
  point.start_recovery_lsn =
      ((local_backup_point *)metaPtr)->start_recovery_lsn;
  metaPtr += sizeof(local_backup_point);
  time_t fullLBTimePoint = *((time_t *)metaPtr);
  metaPtr += sizeof(time_t);
  fullDir.clear();
  ConcatenateTimeSuffix(fullDir, fullLBTimePoint);
  uint32_t number = *((uint32_t *)metaPtr);
  metaPtr += sizeof(uint32_t);

  std::string fileName;
  for (uint32_t loop = 0; loop < number; loop++) {
    uint16_t nameLength = *((uint16_t *)metaPtr);
    metaPtr += sizeof(uint16_t);
    fileName.clear();
    fileName.append(metaPtr, nameLength);
    walFileList.push_back(fileName);
    metaPtr += nameLength;
  }

  delete[] metaBuf;

  return false;
}

bool LocalBackupFileMgr::ShowTimePointFromFullBackupMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    const std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &parentMetaCursor) {
  LocalBackupFileUniquePtr parentFile(new (std::nothrow) LocalBackupFile(
      LocalBackupFileType::FULL_PARENT_META_LB, metaPath, nullptr));
  if (nullptr == parentFile) {
    CDE_LOG_ERROR(
        "new LocalBackupFile object fail when show time point from full backup "
        "meta offset");
    return true;
  }
  if (parentFile->OpenFile(false, true)) {
    CDE_LOG_ERROR(
        "open parent file %s fail when show time point from full backup meta "
        "offset",
        parentFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t fileSize = 0;
  if (parentFile->GetFileSize(fileSize)) {
    CDE_LOG_ERROR("get parent file %s size fail",
                  parentFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t maxBufSize = 0;
  uint64_t position = parentMetaCursor.beginOffset;
  uint32_t itemSize = 0;
  parentMetaCursor.fileName = *parentFile->GetFileNamePtr();
  parentMetaCursor.endFileLength = LB_INVALID_OFFSET;

  if (fileSize < sizeof(local_backup_point) || fileSize <= position) {
    CDE_LOG_ERROR(
        "file size is %lu and position is %lu when show time point from full "
        "backup meta offset",
        fileSize, position);
    return true;
  }
  if (position % sizeof(local_backup_point) != 0) {
    CDE_LOG_ERROR(
        "position %lu is not alignment with size of local_backup_point when "
        "show time point "
        "from full backup meta offset",
        position);
    return true;
  }

  maxBufSize = sizeof(local_backup_point) * PARENT_LB_META_BLOCKSIZE;
  itemSize = sizeof(local_backup_point);

  uint64_t realReadSize = 0;
  char *buffer = new (std::nothrow) char[maxBufSize];

  while (position < fileSize) {
    if (parentFile->ReadParentMeta(buffer, maxBufSize, fileSize, position,
                                   realReadSize)) {
      delete[] buffer;
      CDE_LOG_ERROR(
          "parent file %s read parent meta fail when show time point from full "
          "backup meta offset",
          parentFile->GetFileNamePtr()->c_str());
      return true;
    }

    uint32_t scanNum = realReadSize / itemSize;

    local_backup_point *point = (local_backup_point *)(buffer);
    uint32_t loop = 0;
    for (loop = 0; loop < scanNum; loop++) {
      std::shared_ptr<local_backup_point> timePoint(new (std::nothrow)
                                                        local_backup_point());
      DBUG_EXECUTE_IF("wal_archive_disjoin_extra_inject", timePoint.reset(););
      if (timePoint == nullptr) {
        CDE_LOG_ERROR(
            "failed to alloc time point in "
            "ShowTimePointFromFullBackupMetaOffset");
        delete[] buffer;
        return true;
      }
      timePoint->lsn_point = point[loop].lsn_point;
      timePoint->start_recovery_lsn = point[loop].start_recovery_lsn;
      timePoint->time_point = point[loop].time_point;
      timePointList.push_back(std::move(timePoint));

      if (maxExpectCount != 0 && timePointList.size() >= maxExpectCount) {
        break;
      }
    }
    if (scanNum == loop) {
      position += realReadSize;
    }
    if (scanNum != loop) {
      position += sizeof(local_backup_point) * (loop + 1);
      break;
    }
  }

  delete[] buffer;

  if (position > fileSize) {
    CDE_LOG_ERROR(
        "the position %lu is large than file size %lu when show time point "
        "from full backup meta offset",
        position, fileSize);
    return true;
  }
  parentMetaCursor.endFileLength = position;

  return false;
}

bool LocalBackupFileMgr::ShowTimePointFromWalArchiveChildMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &childMetaCursor, DSTORE::Timestamp timeStamp,
    bool lastParentPoint) {
  std::string childFileName;
  ConcatenateTimeSuffix(childFileName, timeStamp);
  LocalBackupFileUniquePtr childFile(new (std::nothrow) LocalBackupFile(
      LocalBackupFileType::ARCHIVE_CHILD_META_LB, metaPath, &childFileName));
  if (nullptr == childFile) {
    CDE_LOG_ERROR(
        "new LocalBackupFile object fail when show time point from wal archive "
        "child meta offset");
    return true;
  }
  if (childFile->OpenFile(false, true)) {
    CDE_LOG_ERROR(
        "child file %s fail when show time point from wal archive child meta "
        "offset",
        childFile->GetFileNamePtr()->c_str());
    return true;
  }
  bool disjoined = false;
  if (childFile->IsDisjoined(disjoined)) {
    CDE_LOG_WARN(
        "check child file %s if disjoined fail when show time point from wal "
        "archive child meta offset",
        childFile->GetFileNamePtr()->c_str());
  }
  if (disjoined && !lastParentPoint) {
    CDE_LOG_WARN(
        "the child file is disjoined and not last parent point when show time "
        "point from wal archive child meta offset");
  }

  childMetaCursor.fileName = *childFile->GetFileNamePtr();

  uint64_t length = 0;
  char *buf = childFile->ReadChildMeta(length);
  if (nullptr == buf) {
    CDE_LOG_ERROR(
        "child file %s read child meta fail when show time point from wal "
        "archive child meta offset",
        childFile->GetFileNamePtr()->c_str());
    return true;
  }

  WalArchiveMetaHead metaHead;
  if (childFile->ReadWalArchiveMetaHead(metaHead)) {
    delete[] buf;
    CDE_LOG_ERROR(
        "child file %s read wal archive meta head fail when show time point "
        "from wal archive child meta offset",
        childFile->GetFileNamePtr()->c_str());
    return true;
  }
  length = metaHead.m_validLength;

  uint64_t pos = sizeof(WalArchiveMetaHead);
  char *ptr = buf;
  ptr += pos;
  while (pos < length) {
    LocalBackupPoint *archivePoint = (LocalBackupPoint *)(ptr);
    ptr += sizeof(LocalBackupPoint);
    pos += sizeof(LocalBackupPoint);

    if (pos > childMetaCursor.beginOffset) {
      std::shared_ptr<local_backup_arch_point> timePoint(
          new (std::nothrow) local_backup_arch_point());
      DBUG_EXECUTE_IF("wal_archive_disjoin_extra_inject", timePoint.reset(););
      if (timePoint == nullptr) {
        CDE_LOG_ERROR(
            "failed to alloc time point in "
            "ShowTimePointFromWalArchiveChildMetaOffset");
        delete[] buf;
        return true;
      }
      timePoint->lsn_point = archivePoint->m_lsn;
      timePoint->time_point = archivePoint->m_time;
      timePoint->start_recovery_lsn = LB_INVALID_LSN;
      timePoint->is_disjoin = false;
      timePointList.push_back(std::move(timePoint));
    }

    uint32_t fileNumber = *((uint32_t *)(ptr));
    ptr += sizeof(uint32_t);
    pos += sizeof(uint32_t);
    for (uint32_t i = 0; i < fileNumber; i++) {
      uint16_t nameLength = *(uint16_t *)(ptr);
      ptr += sizeof(uint16_t) + nameLength;
      pos += sizeof(uint16_t) + nameLength;
    }

    if (disjoined && pos == length && timePointList.size() > 0) {
      /* Mark last time point of this child meta disjoin. */
      ((local_backup_arch_point *)(timePointList[timePointList.size() - 1]
                                       .get()))
          ->is_disjoin = true;
    }

    if (maxExpectCount != 0 && timePointList.size() >= maxExpectCount) {
      break;
    }
  }
  delete[] buf;

  if (pos > length) {
    CDE_LOG_ERROR(
        "the pos is %lu and length is %lu when show time point from wal "
        "archive child meta offset",
        pos, length);
    return true;
  }
  childMetaCursor.endFileLength = pos;

  return false;
}

bool LocalBackupFileMgr::ShowTimePointFromWalArchiveMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &parentMetaCursor, MetaFileCursor &childMetaCursor) {
  LocalBackupFileUniquePtr parentFile(new (std::nothrow) LocalBackupFile(
      LocalBackupFileType::ARCHIVE_PARENT_META_LB, metaPath, nullptr));
  if (nullptr == parentFile) {
    CDE_LOG_ERROR(
        "new LocalBackupFile object fail when show time point from wal archive "
        "meta offset");
    return true;
  }
  if (parentFile->OpenFile(false, true)) {
    CDE_LOG_ERROR(
        "parent file %s open fail when show time point from wal archive meta "
        "offset",
        parentFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t fileSize = 0;
  if (parentFile->GetFileSize(fileSize)) {
    CDE_LOG_ERROR(
        "parent file %s get file size fail when show time point from wal "
        "archive meta offset",
        parentFile->GetFileNamePtr()->c_str());
    return true;
  }
  uint64_t maxBufSize = 0;
  uint64_t position = parentMetaCursor.beginOffset;
  uint32_t itemSize = 0;
  parentMetaCursor.fileName = *parentFile->GetFileNamePtr();
  parentMetaCursor.endFileLength = LB_INVALID_OFFSET;
  childMetaCursor.endFileLength = LB_INVALID_OFFSET;

  if (fileSize < sizeof(LocalBackupPoint) || fileSize <= position) {
    CDE_LOG_ERROR(
        "file size %lu and position %lu when show time point from wal archive "
        "meta offset",
        fileSize, position);
    return true;
  }
  if (position % sizeof(LocalBackupPoint) != 0) {
    CDE_LOG_ERROR(
        "position %lu is not alignment with size of LocalBackupPoint when show "
        "time point from wal archive meta offset",
        position);
    return true;
  }

  maxBufSize = sizeof(LocalBackupPoint) * PARENT_LB_META_BLOCKSIZE;
  itemSize = sizeof(LocalBackupPoint);

  uint64_t realReadSize = 0;
  char *buffer = new (std::nothrow) char[maxBufSize];

  while (position < fileSize) {
    if (parentFile->ReadParentMeta(buffer, maxBufSize, fileSize, position,
                                   realReadSize)) {
      delete[] buffer;
      CDE_LOG_ERROR(
          "parent file %s read parent meta fail when show time point from wal "
          "archive meta offset",
          parentFile->GetFileNamePtr()->c_str());
      return true;
    }

    uint32_t scanNum = realReadSize / itemSize;

    LocalBackupPoint *point = (LocalBackupPoint *)(buffer);
    uint32_t loop = 0;
    for (loop = 0; loop < scanNum; loop++) {
      bool firstParentPoint =
          (loop == 0 && position == parentMetaCursor.beginOffset);
      bool lastParentPoint =
          (loop == scanNum - 1 && position + realReadSize == fileSize);

      if (!firstParentPoint) {
        childMetaCursor.beginOffset = 0;
      }

      if (ShowTimePointFromWalArchiveChildMetaOffset(
              timePointList, metaPath, maxExpectCount, childMetaCursor,
              point[loop].m_time, lastParentPoint)) {
        delete[] buffer;
        CDE_LOG_ERROR(
            "show time point from wal archive child meta offset fail");
        return true;
      }

      if (maxExpectCount != 0 && timePointList.size() >= maxExpectCount) {
        break;
      }
    }

    if (scanNum == loop) {
      position += realReadSize;
    }
    if (scanNum != loop) {
      position += sizeof(LocalBackupPoint) * (loop + 1);
      break;
    }
  }

  delete[] buffer;

  if (position > fileSize) {
    CDE_LOG_ERROR(
        "the position is %lu and file size %lu when show time point from wal "
        "archive meta offset",
        position, fileSize);
    return true;
  }
  parentMetaCursor.endFileLength = position - sizeof(LocalBackupPoint);

  return false;
}

bool LocalBackupFileMgr::WriteMetaJson(std::string &path, std::string *errMsg,
                                       local_backup_point *backupPoint) {
  LocalBackupFile *metaJsonFile =
      GetLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true, 0);
  if (nullptr == metaJsonFile) {
    bool isCreated;
    if (LB_INVALID_IDX ==
        AddLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true, path,
                           nullptr, isCreated)) {
      CDE_LOG_ERROR(
          "add local backup file fail when create full local backup meta json");
      return true;
    }
    metaJsonFile =
        GetLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true, 0);
  }

  uint64_t fileSize = 0;
  if (metaJsonFile->GetFileSize(fileSize)) {
    CDE_LOG_ERROR("get file %s size fail",
                  metaJsonFile->GetFileNamePtr()->c_str());
    return true;
  }

  rapidjson::Document document(rapidjson::kObjectType);
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  LocalBackupBinlogPoint binlogPoint;
#ifndef IS_DSTORE_BACKUP_TOOL
  m_binlogFileMgr.GetBinlogPoint(binlogPoint);
#endif
  {
    std::unique_lock<std::mutex> lock(m_progressLock);
    DumpMetaJson(document, allocator, errMsg, m_progress, backupPoint,
                 binlogPoint);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> prettyWriter(buffer);
  document.Accept(prettyWriter);

  char *buf = const_cast<char *>(buffer.GetString());
  uint64_t dataLength = buffer.GetSize();

  /* Add CRC32 checksum to meta json file. */
  bool writeError = false;
  uint64_t crcLength = 0;
  if (dstore_meta_json_crc) {
    ha_checksum crc =
        my_checksum(CRC_SEED, pointer_cast<const uchar *>(buf), dataLength);
    crcLength = sizeof(ha_checksum);
    std::unique_ptr<char[]> combinedData(new char[crcLength + dataLength]);
    if (memcpy_s(combinedData.get(), crcLength, &crc, crcLength) != 0 ||
        memcpy_s(combinedData.get() + crcLength, dataLength, buf, dataLength) !=
            0 ||
        DBUG_EVALUATE_IF("meta_json_data_copy_fail", true, false)) {
      CDE_LOG_ERROR("failed to copy backup meta json data to buffer");
      return true;
    }
    writeError = metaJsonFile->WriteSimple((void *)combinedData.get(),
                                           crcLength + dataLength, 0);
  } else {
    writeError = metaJsonFile->WriteSimple((void *)buf, dataLength, 0);
  }

  if (writeError) {
    CDE_LOG_ERROR(
        "write simple file %s fail when write full local backup meta json data",
        metaJsonFile->GetFileNamePtr()->c_str());
    return true;
  }

  /*
    If the new meta json content size is less than before, should truncate the
    file to eliminate residual data.
  */
  uint64_t jsonLength = crcLength + dataLength;
  if (jsonLength < fileSize) {
    metaJsonFile->TruncateFile(jsonLength);
  }
  metaJsonFile->Flush();

  return false;
}

bool LocalBackupFileMgr::VerifyMetaJsonCrc(std::string &path,
                                           ha_checksum &storageCrc,
                                           ha_checksum &calcCrc) {
  LocalBackupFile *metaJsonFile =
      GetLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true, 0);
  if (nullptr == metaJsonFile) {
    LocalBackupFile file(LocalBackupFileType::FULL_META_JSON_LB, path, nullptr);
    if (!file.FileExists()) {
      CDE_LOG_ERROR("full local backup meta json file does not exist.");
      return true;
    }
    bool isCreated;
    if (LB_INVALID_IDX ==
        AddLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true, path,
                           nullptr, isCreated)) {
      CDE_LOG_ERROR(
          "add local backup file fail when create full local backup meta json");
      return true;
    }
    metaJsonFile =
        GetLocalBackupFile(LocalBackupFileType::FULL_META_JSON_LB, true, 0);
  }

  uint64_t length = 0;
  char *buf = metaJsonFile->ReadChildMeta(length);
  DBUG_EXECUTE_IF("read_meta_json_fail", {
    /* mtr case could cause the memory leak and delete nullptr is supported */
    delete[] buf;
    buf = nullptr;
  });

  if (nullptr == buf || DBUG_EVALUATE_IF("read_meta_json_fail", true, false)) {
    CDE_LOG_ERROR("read child meta file %s fail when verify meta json crc",
                  metaJsonFile->GetFileNamePtr()->c_str());
    return true;
  }

  char *ptr = buf;
  storageCrc = *((ha_checksum *)ptr);
  ptr += sizeof(ha_checksum);

  uint64_t dataLength = length - sizeof(ha_checksum);
  calcCrc = my_checksum(CRC_SEED, pointer_cast<const uchar *>(ptr), dataLength);

  delete[] buf;

  if (storageCrc != calcCrc) {
    std::ostringstream oss;
    oss << "meta json file checksum doesn't match"
        << ". storage checksum= " << storageCrc
        << ", calculate checksum= " << calcCrc;
    CDE_LOG_ERROR("%s", oss.str().c_str());
    return true;
  }

  return false;
}

void LocalBackupFileMgr::UpdateStatus(DSTORE::BackupObjType &backupObjType,
                                      DSTORE::LocalBackupStatus &status) {
  std::unique_lock<std::mutex> lock(m_progressLock);
  m_progress.UpdateStatus(backupObjType, status);
}

void LocalBackupFileMgr::UpdateProgress(DSTORE::LocalBackupProgress &prog) {
  std::unique_lock<std::mutex> lock(m_progressLock);
  m_progress.progress = prog;
}

void LocalBackupFileMgr::UpdateWalArchivedSize(uint64_t archivedSize) {
  std::unique_lock<std::mutex> lock(m_progressLock);
  m_progress.walArchivedSize = archivedSize;
}

void LocalBackupFileMgr::GetFullLocalBackupProgress(
    FullLocalBackupProgress &progress) {
  std::unique_lock<std::mutex> lock(m_progressLock);
  progress = m_progress;
}

void LocalBackupFileMgr::ClearFullLocalBackupProgress() { m_progress.Clear(); }

void LocalBackupFileMgr::ClearFullLocalBackupProgressWithoutErrMsg() {
  m_progress.ClearProgress();
}

void LocalBackupFileMgr::lock() { m_outLock.lock(); }

void LocalBackupFileMgr::unlock() { m_outLock.unlock(); }

bool LocalBackupFileMgr::UpdateRecoveryStartPointInRestoreMeta(
    const std::string &restoreMetaPath, uint64_t startPoint) {
  LocalBackupFileUniquePtr restoreMetaFile(
      new (std::nothrow) LocalBackupFile(LocalBackupFileType::RESTORE_META_LB,
                                         restoreMetaPath, &RESTORE_META_FILE));
  if (restoreMetaFile == nullptr ||
      DBUG_EVALUATE_IF("restore_update_meta_new_fail", true, false)) {
    CDE_LOG_ERROR(
        "create restore meta file object fail when update restore start point");
    return true;
  }

  if (restoreMetaFile->OpenFile(false, false) ||
      DBUG_EVALUATE_IF("restore_update_meta_open_fail", true, false)) {
    CDE_LOG_ERROR("open file %s fail when update restore start point",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    return true;
  }

  local_backup_point point;
  if (restoreMetaFile->ReadPointItemFromRestoreMeta(&point) ||
      DBUG_EVALUATE_IF("restore_update_meta_read_fail", true, false)) {
    CDE_LOG_ERROR("read point from %s fail when update restore start point",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    return true;
  }

  if (point.start_recovery_lsn >= startPoint) {
    CDE_LOG_INFO("start point %lu in %s is not less than %lu. Ignore it",
                 point.start_recovery_lsn,
                 restoreMetaFile->GetFileNamePtr()->c_str(), startPoint);
    return false;
  }

  point.start_recovery_lsn = startPoint;
  if (restoreMetaFile->WriteSimple((void *)&point, sizeof(point), 0) ||
      DBUG_EVALUATE_IF("restore_update_meta_write_fail", true, false)) {
    CDE_LOG_ERROR("write restore start point to restore meta file %s fail",
                  restoreMetaFile->GetFileNamePtr()->c_str());
    return true;
  }
  restoreMetaFile->Flush();
  return false;
}

uint64_t *LocalBackupFileMgr::GetWriteTimes(
    DSTORE::BackupObjType backupObjType) {
  switch (backupObjType) {
    case DSTORE::BackupObjType::BACKUP_WAL: {
      return &m_walFileWriteTimes;
    }
    case DSTORE::BackupObjType::BACKUP_CONTROL_FILE: {
      return &m_controlFileWriteTimes;
    }
    case DSTORE::BackupObjType::BACKUP_TABLESPACE:
    default: {
      return &m_tablespaceFileWriteTimes;
    }
  }
}

void LocalBackupFileMgr::ClearWriteTimes() {
  m_controlFileWriteTimes = 0;
  m_walFileWriteTimes = 0;
  m_controlFileWriteTimes = 0;
}

#ifndef IS_DSTORE_BACKUP_TOOL
bool LocalBackupFileMgr::WriteCurrentObsObjMeta(DSTORE::BackupObjType type) {
  LocalBackupFile *backupFile = GetLocalBackupFile(
      LocalBackupFileType::DATA_LB, true, m_currentObsObjIdx[type]);
  // LCOV_EXCL_START
  if (nullptr == backupFile) {
    CDE_LOG_ERROR(
        "failed to get local backup file for local backup with obs mode");
    return true;
  }
  // LCOV_EXCL_STOP
  std::string name(backupFile->GetFileNamePtr()->c_str());
  lb_object_handler *handler = fetch_lb_object_handler();
  // LCOV_EXCL_START
  if (nullptr == handler) {
    CDE_LOG_ERROR(
        "failed to fetch lb object handler for local backup with obs mode");
    return true;
  }
  // LCOV_EXCL_STOP
  handler->make_object_name(name, OBS_OBJ_TYPE_MERGED_FILES_META, 0, 0);
  increase_current_lb_object_count();

  if (gen_merged_file_obj_meta(m_currentObsObj[type].files, handler)) {
    come_back_lb_object_handler(handler);
    CDE_LOG_ERROR("write file group meta object to OBS fail for %s",
                  name.c_str());
    return true;
  }
  come_back_lb_object_handler(handler);
  return false;
}
#endif
}  // namespace CDE
