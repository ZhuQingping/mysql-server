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
 * local_backup_mgr.h
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup_file_mgr.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef LOCAL_BACKUP_FILE_MGR_H
#define LOCAL_BACKUP_FILE_MGR_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "common/dstore_common_utils.h"
#include "full_backup_meta_json.h"
#include "local_backup.h"
#include "packutils.h"
#include "sql/handler.h"

#include "local_backup_binlog_file_mgr.h"

#include "sql/local_backup/local_backup_obs_handler.h"
#include "sql/local_backup/local_backup_obs_utils.h"
#include "sql/local_backup/local_backup_utils.h"

namespace CDE {

const uint32_t LB_FILE_HEAD_ATOMIC_SIZE = 512;
const uint64_t LB_INVALID_LSN = 0xFFFFFFFFFFFFFFFF;
const uint16_t LB_INVALID_IDX = 0xFFFF;
const uint64_t LB_INVALID_OFFSET = 0xFFFFFFFFFFFFFFFF;

struct LocalBackupPoint {
  DSTORE::Timestamp m_time;
  uint64_t m_lsn;
};

#pragma pack(push, 2)
struct WalArchiveMetaHead {
  LocalBackupPoint m_lastValidPoint;
  uint64_t m_lastValidOffset;
  /** The latest time's wal archive which have found is disjointed and
   *  not connected last valid archive lsn
   */
  uint64_t m_lsnDisjoined;
  uint64_t m_validLength;
  uint32_t m_itemNum;
  uint16_t m_intervalOffsetNum;
  char m_reserve[LB_FILE_HEAD_ATOMIC_SIZE - sizeof(LocalBackupPoint) - 30];
};
#pragma pack(pop)

enum class LocalBackupFileType {
  DATA_LB,
  FULL_PARENT_META_LB,
  ARCHIVE_PARENT_META_LB,
  ARCHIVE_CHILD_META_LB,
  FULL_META_JSON_LB,
  RESTORE_META_LB,
  FULL_BINLOG_META_LB,
  INVALID_LB,
};

class LocalBackupFile {
 public:
  LocalBackupFile(const LocalBackupFileType type, const std::string &path,
                  const std::string *fileName);
  virtual ~LocalBackupFile() {}

  /**
  Open the file

  @param[in]  create whether create the file by filename and then open
  @param[in]  readOnly whether open the file with readonly mode

  @return false if success
  @return true if fail
  */
  virtual bool OpenFile(bool create, bool readOnly);

  /**
  Close the file have opened

  @return false if success
  @return true if fail
  */
  virtual bool CloseFile();

  /**
  Truncate the file with new size.

  @param[in]  size The new file size of the file

  @return false if success
  @return true if fail
  */
  virtual bool TruncateFile(uint64_t size);

  /**
  Write

  @param[in]  size The file size

  @return false if success
  @return true if fail
  */
  virtual bool GetFileSize(uint64_t &size);

  /**
  Write the content to file by specify length and position

  @param[in]  buffer The content need writted
  @param[in]  dataLength the length of data content
  @param[in]  offset The begin file offset for writing

  @return false if success
  @return true if fail
  */
  virtual bool WriteSimple(void *buffer, uint64_t dataLength, uint64_t offset);

  /**
  Flush the file

  @return false if success
  @return true if fail
  */
  virtual bool Flush();

  /**
  Check whether the file exists

  @return false if have not file
  @return true if have file
  */
  virtual bool FileExists();

  virtual void AddWriteFlag(uint32_t flag);

  /**
  Get file name ptr

  @return false if success
  @return true if fail
  */
  std::string *GetFileNamePtr();

  /**
  Read the local backup point from restore meta file when restoring the database
  node

  @param[out] point The local backup point including time and lsn point used to
  set start recovery lsn

  @return false if success
  @return true if fail
  */
  bool ReadPointItemFromRestoreMeta(local_backup_point *point);

  /**
  Read a certain number meta iteams from parent meta file

  @param[in]  buffer The meta iteam buffer reading to
  @param[in]  maxBufSize The max buffer size
  @param[in]  position The begin file position in which read file
  @param[out] realReadSize The size of content readed

  @return false if success
  @return true if fail
  */
  bool ReadParentMeta(char *buffer, uint64_t maxBufSize, uint64_t fileSize,
                      uint64_t position, uint64_t &realReadSize);

  /**
  Read last meta item from parent meta file

  @param[out]  item The last meta item in the parent meta file

  @return false if success
  @return true if fail
  */
  bool ReadParentLastItem(LocalBackupPoint &item);

  /**
  Read the file head from wal archive meta file

  @param[out]  head The file head from wal archive child meta file
  @param[in]

  @return false if success
  @return true if fail
  */
  bool ReadWalArchiveMetaHead(WalArchiveMetaHead &head);

  /**
  Write meta items to parent meta file

  @param[in]  item The meta item buffer will be written to parent meta file
  @param[in]  length The length of meta item buffer

  @return false if success
  @return true if fail
  */
  bool WriteParentMeta(void *item, uint32_t length);

  /**
  Write meta item to child meta file and update file head

  @param[in]  item The item buffer need to written
  @param[in]  size The size of item buffer
  @param[in]  point The last valid point need to be written in head

  @return false if success
  @return true if fail
  */
  bool WriteChildMetaItem(void *item, uint32_t size, LocalBackupPoint &point);

  /**
  Write data content to one data file

  @param[in]  buffer The data buffer need to written to data file
  @param[in]  dataLength The length of data buffer
  @param[in]  offset The begin offset for these file written
  @param[in, out]  writeTimes The write times for this type of backup file

  @return false if success
  @return true if fail
  */
  bool WriteDataBuffer(uint8_t *buffer, uint32_t dataLength, uint64_t offset,
                       uint64_t *writeTimes);

  /**
  Read all content of file at thes time

  @param[out]  length The length of file content have readed

  @return the buffer including child meta data
  */
  char *ReadChildMeta(uint64_t &length);

  /**
  Check whether the file content is full if it is the child meta file

  @param[in]  length
  @param[out] contentFull Whether the file content is full

  @return false if content is full
  @return true if content is not full
  */
  bool IsContentFull(uint32_t length, bool &contentFull);

  /**
  Check whether the child meta file is disjoined in the view of wal archive lsn

  @param[out] disjoined Whether the file is disjoined

  @return false if success
  @return true if fail
  */
  bool IsDisjoined(bool &disjoined);

  /**
  Read the file content to buffer

  @param[in]  buffer The buffer used to read
  @param[in]  length The expect length of content reading
  @param[in]  offset The begin position of reading

  @return false if success
  @return true if fail
  */
  virtual bool ReadSimple(void *buffer, uint64_t length, uint64_t offset);

 private:
  /**
  Init the file head for wal archive child meta file

  @param[out]  metaHead The wal archive meta file head
  */
  void InitWalArchiveChildMetaHead(WalArchiveMetaHead &metaHead);

  /**
  Write the file head to awl archive child meta file

  @param[in]  head The wal archive meta file head have will be written

  @return false if success
  @return true if fail
  */
  bool WriteWalArchiveChildMetaHeader(WalArchiveMetaHead &head);

 protected:
  /* Include absolute path */
  std::string m_fileName;

 private:
  LocalBackupFileType m_type;
  int m_fd;
};

/* The Object is single model, write buffer and put one new object directly */
const uint32_t SINGLE_OBJECT_MODLE = 1;
/* The Single object is existence, and need read to buffer */
const uint32_t SINGLE_OBJECT_EXISTENCE = 2;
const uint32_t MULTI_OBJECT_EXISTENCE = 4;
/* If this file is belong to one file group which putted to one object */
const uint32_t NO_NORMAL_FREE_HANDLE = 8;
/* File group with seperated meta object */
const uint32_t WITH_SEPERATED_META = 16;

class LocalBackupObsObject : public LocalBackupFile {
 public:
  LocalBackupObsObject(const LocalBackupFileType type, const std::string &path,
                       const std::string *fileName)
      : LocalBackupFile(type, path, fileName),
        m_handler(nullptr),
        m_writeFlag(0),
        m_readOnly(false) {
    /* Set flags for file */
    switch (type) {
      case LocalBackupFileType::ARCHIVE_CHILD_META_LB:
      case LocalBackupFileType::ARCHIVE_PARENT_META_LB:
      case LocalBackupFileType::FULL_PARENT_META_LB:
      case LocalBackupFileType::RESTORE_META_LB:
      case LocalBackupFileType::FULL_BINLOG_META_LB:
        AddWriteFlag(SINGLE_OBJECT_MODLE);
        break;
      case LocalBackupFileType::FULL_META_JSON_LB:
        AddWriteFlag(SINGLE_OBJECT_MODLE);
        AddWriteFlag(NO_NORMAL_FREE_HANDLE);  // Will write more than once
        break;
      default:
        break;
    }
  }

  ~LocalBackupObsObject() { FreeLbObsObjectHandle(); }

  bool OpenFile(bool create, bool readOnly) override;
  bool CloseFile() override;
  bool TruncateFile(uint64_t size) override;
  bool GetFileSize(uint64_t &size) override;
  bool WriteSimple(void *buffer, uint64_t dataLength, uint64_t offset) override;
  bool Flush() override;
  bool FileExists() override;
  bool ReadSimple(void *buffer, uint64_t length, uint64_t offset) override;
  void AddWriteFlag(uint32_t flag) override { m_writeFlag |= flag; }
  void DelWriteFlag(uint32_t flag) { m_writeFlag &= (~flag); }

 private:
  bool GetLbObsObjectHandle(std::string &name);
  void FreeLbObsObjectHandle();

  lb_object_handler *m_handler;
  uint32_t m_writeFlag;
  bool m_readOnly;
};

class LocalBackupFileMgr {
 public:
  static LocalBackupFileMgr *GetInstance();
  static bool IsInitialized();
  static bool CreateInstance();
  static void DestroyInstance();

  /**
  Read latest local backup point
    SET_WAL_STARTRECOVERY_PLSN
      Obtain the value from the restore meta file.
    SET_WAL_ARCHIVE_PLSN
      First see if there is an incremental succession of sites, if there is,
  obtain; This site needs to be larger than the last valid site of the delta
  archive of the disjoint;

  @param[in]  cmd command need to read latest local backup point
  @param[in]  path meta file path
  @param[out] point local backup point

  @return false if success
  @return true if fail
  */
  bool ReadLatestLocalBackupPoint(LocalBackupCmd cmd, std::string &path,
                                  local_backup_point *point);

  /**
  Write time point and lsn point to wal archive meta file

  @param[in]  timePoint time point
  @param[in]  lsnPoint  lsn point
  @param[in]  path      meta file path
  @param[in]  isDisjoin whether have disjoin lsn point

  @return false if success
  @return true if fail
  */
  bool WriteWalArchiveMeta(DSTORE::Timestamp timePoint, uint64_t lsnPoint,
                           std::string &path, bool isDisjoin);

  /**
  Create and add LocalBackupFile object

  @param[in]  type The file type
  @param[in]  isFullLB Whether the file is used for full local backup
  @param[in]  path the file path
  @param[in]  fileName The file name
  @param[out] create Whether the file is created during adding local backup
  @param[in]  createPath Whether create the directory of file if not exist

  @return false if success
  @return true if fail
  */
  uint16_t AddLocalBackupFile(LocalBackupFileType type, bool isFullLB,
                              std::string &path, std::string *fileName,
                              bool &create, bool createPath = false,
                              uint32_t writeFlag = 0);

  /**
  Get the LocalBackupFile object

  @param[in]  type The file type
  @param[in]  isFullLB Whether the object is belong to full local backup
  @param[in]  index The index of file list

  @return false if success
  @return true if fail
  */
  LocalBackupFile *GetLocalBackupFile(LocalBackupFileType type, bool isFullLB,
                                      const uint16_t index);

  /**
  Write full local backup meta backup point to full local backup meta file

  @param[in]  backupPoint The local backup point
  @param[in]  path The path of meta file

  @return false if success
  @return true if fail
  */
  bool WriteFullLocalBackupMeta(local_backup_point &backupPoint,
                                std::string &path);

  /**
  Write non-dstore part status and error message, backup point info and binlog
  info to full local backup meta json file

  @param[in]  errMsg The non-dstore part error message
  @param[in]  backupPoint The local backup point
  @param[in]  path The path of meta json file

  @return false if success
  @return true if fail
  */
  bool WriteFullLocalBackupMetaJson(std::string *errMsg,
                                    local_backup_point *backupPoint,
                                    std::string &path);

  /**
  Remove Local backup file

  @param[in]  type The local backup file type
  @param[in]  isFull Whether the file is belong to full local backup
  */
  void RemoveLocalBackupFile(LocalBackupFileType type, bool isFull);

  /**
  Destory Lb Io context from normal set when context can leave and not hang
  */
  void DestoryGlobalLbIoContext();

  /**
  Try clear io context when it can leave and not hang from rigid io context set
  */
  void TryClearRigidIoContext();

  /**
  Create new restore meta file based on time point
  The restore meta including local backup point,
  full data dir name and wal file name list

  @param[in]  timePoint The time point need restore to
  @param[in]  path The meta path
  @param[out] expectPoint The local backup point expected based on one time
  @param[in]  restoreMetaPath The path of restore meta file created
  @param[in]  restoreMetaNameChanged Whether the name of restore meta file is
  changed

  @return false if success
  @return true if fail
  */
  bool CreateRestoreMeta(DSTORE::Timestamp timePoint, std::string path,
                         local_backup_point &expectPoint,
                         std::string &restoreMetaPath,
                         bool restoreMetaNameChanged);

  /**
  Show the local backup point, full data dir and wal file list from one restore
  meta file based on one time

  @param[in]  timePoint The time point to which restore
  @param[in]  metaPath The path of restore meta
  @param[out] point The local backup point
  @param[out] fullDir The full data dir
  @param[out] walFileList The wal file list
  @param[in]  restoreMetaNameChanged Whether the name of restore meta file is
  changed

  @return false if success
  @return true if fail
  */
  bool ShowRestoreFileList(DSTORE::Timestamp timePoint, std::string &metaPath,
                           local_backup_point &point, std::string &fullDir,
                           std::vector<std::string> &walFileList,
                           bool restoreMetaNameChanged);

  /**
  Show the backup points from one offset in full local backup meta.

  @param[out]  timePointList  The list of time and lsn point for full local
  backup
  @param[in]   metaPath The path of meta file
  @param[in]   maxExpectCount The max count of continue timePoint for full local
  backup during these search
  @param[in, out]  parentMetaCursor The search cursor for full local backup meta
                      note: the fileName is nullptr as parameter.

  @return false if success
  @return true if fail
  */
  bool ShowTimePointFromFullBackupMetaOffset(
      std::vector<std::shared_ptr<local_backup_point>> &timePointList,
      const std::string &metaPath, uint32_t maxExpectCount,
      MetaFileCursor &parentMetaCursor);

  /**
  Show the backup points from one offset in wal archive meta.

  @param[out]      timePointList  The list of time and lsn point for wal archive
  @param[in]       metaPath The path of meta file
  @param[in]       maxExpectCount The max count of continue timePoint for wal
  archive during these search
  @param[in, out]  parentMetaCursor The search parent meta cursor for wal
  archive meta note: the fileName is nullptr as parameter
  @param[in, out]  childMetaCursor The search child meta cursor for wal archive
  meta

  @return false if success
  @return true if fail
  */
  bool ShowTimePointFromWalArchiveMetaOffset(
      std::vector<std::shared_ptr<local_backup_point>> &timePointList,
      std::string &metaPath, uint32_t maxExpectCount,
      MetaFileCursor &parentMetaCursor, MetaFileCursor &childMetaCursor);

  /**
  Write meta json file based on progress, backup point and binlog info.

  @param[in]  path The created meta json file path
  @param[in]  errMsg The non-dstore part error message
  @param[in]  backupPoint The full local backup point

  @return false if success
  @return true if fail
  */
  bool WriteMetaJson(std::string &path, std::string *errMsg = nullptr,
                     local_backup_point *backupPoint = nullptr);

  /**
  Read meta json file, and verify if the crc is matched.

  @param[in]   path The meta json file path
  @param[out]  storageCrc The crc value stored in meta json file head.
  @param[out]  calcCrc The crc value calculated by the meta json file data.

  @return false if success
  @return true if fail
  */
  bool VerifyMetaJsonCrc(std::string &path, ha_checksum &storageCrc,
                         ha_checksum &calcCrc);

  /**
  Update status info in full local backup progress.

  @param[in]  backupObjType The backup type.
  @param[in]  status The status info.
  */
  void UpdateStatus(DSTORE::BackupObjType &backupObjType,
                    DSTORE::LocalBackupStatus &status);

  /**
  Update progress info in full local backup progress.

  @param[in]  prog The progress info.
  */
  void UpdateProgress(DSTORE::LocalBackupProgress &prog);

  /**
  Update wal archived size in full local backup progress.

  @param[in]  archivedSize The wal archived size.
  */
  void UpdateWalArchivedSize(uint64_t archivedSize);

  /**
  Get full local backup progress.

  @param[out]  progress The full local backup progress.
  */
  void GetFullLocalBackupProgress(FullLocalBackupProgress &progress);

  /** Clear all info in full local backup progress. */
  void ClearFullLocalBackupProgress();

  /** Clear info except error message in full local backup progress. */
  void ClearFullLocalBackupProgressWithoutErrMsg();

  void lock();
  void unlock();

  /**
  Update restore start point to restore meta file.

  @param[in]  restoreMetaPath The path of restore meta file
  @param[in]  startPoint      The restore start point

  @return false if success
  @return true if fail
  */
  bool UpdateRecoveryStartPointInRestoreMeta(const std::string &restoreMetaPath,
                                             uint64_t startPoint);

  LocalBackupBinlogFileMgr *GetBinlogFileMgr() { return &m_binlogFileMgr; }

  /**
  Get write times based on backup object type.

  @param[in]  backupObjType The backup object type.

  @return write times of the specific backup object type.
  */
  uint64_t *GetWriteTimes(DSTORE::BackupObjType backupObjType);

  /** Clear write times of all backup object type. */
  void ClearWriteTimes();

  uint16_t GetCurrentObsObjIdx(DSTORE::BackupObjType type) {
    CDE_ASSERT(type == DSTORE::BackupObjType::BACKUP_TABLESPACE ||
               type == DSTORE::BackupObjType::BACKUP_WAL);
    return m_currentObsObjIdx[type];
  }

  void SetCurrentObsObjIdx(DSTORE::BackupObjType type, uint16_t idx) {
    CDE_ASSERT(type == DSTORE::BackupObjType::BACKUP_TABLESPACE ||
               type == DSTORE::BackupObjType::BACKUP_WAL);
    m_currentObsObjIdx[type] = idx;
  }

  void ClearCurrentObsObj(DSTORE::BackupObjType type) {
    m_currentObsObj[type].clear();
    m_currentObsObjIdx[type] = LB_INVALID_IDX;
  }

  void AddFileToCurrentObsObj(DSTORE::BackupObjType type,
                              const std::string &name) {
    m_currentObsObj[type].files.emplace_back(name, name, 0);
  }

  void UpdateCurrentObsObjSize(DSTORE::BackupObjType type,
                               uint64_t appendSize) {
    m_currentObsObj[type].total_size += appendSize;
    m_currentObsObj[type].files.back().file_size += appendSize;
  }

  uint64_t GetCurrentObsObjSize(DSTORE::BackupObjType type) {
    return m_currentObsObj[type].total_size;
  }

#ifndef IS_DSTORE_BACKUP_TOOL
  bool WriteCurrentObsObjMeta(DSTORE::BackupObjType type);
#endif

 private:
  LocalBackupFileMgr()
      : m_currentObsObj(DSTORE::BackupObjType::BACKUP_WAL + 1),
        m_currentObsObjIdx(DSTORE::BackupObjType::BACKUP_WAL + 1,
                           LB_INVALID_IDX) {}
  ~LocalBackupFileMgr() {}

  /**
  Create buffer including wal archive child meta info based on one point

  @param[in]  point The LocalBackupPoint point come from a certain round wal
  archive
  @param[in]  length The buffer length

  @return buffer
  */
  void *CreateWalArchiveChildMetaBuf(LocalBackupPoint &point, uint32_t &length);

  /**
  concatente time string

  @param[out] FilePrefix The file string
  @param[in]  timePoint The time number

  */
  void ConcatenateTimeSuffix(std::string &FilePrefix,
                             DSTORE::Timestamp timePoint);

  /**
  Search one full local backup point or immediately following some wal archive
  point

  @param[in]  timePoint The time point
  @param[in]  path The meta path
  @param[in]  type The local backup meta file type
  @param[in,out] expectFullLBPoint The full local backup point based on
  timePoint
  @param[out] expectArchivePoint The list of wal archive point based on
  timePoint and one full local backup point

  @return false if success
  @return true if fail
  */
  bool SearchPointFromParentMeta(
      DSTORE::Timestamp timePoint, std::string &path, LocalBackupFileType type,
      local_backup_point *expectFullLBPoint,
      std::vector<LocalBackupPoint> *expectArchivePoint);

  /**
  Show the backup points from one offset in wal archive child meta.

  @param[out]      timePointList  The list of time and lsn point for wal archive
  @param[in]       metaPath The path of meta file
  @param[in]       maxExpectCount The max count of continue timePoint for wal
  archive during these search
  @param[in, out]  childMetaCursor The search child meta cursor for wal archive
  meta note: the fileName is nullptr as parameter
  @param[in]       timeStamp The timestamp of the wal archive child meta file
  @param[in]       lastParentPoint Whether the wal archive child meta file is
  the last file based on the parent meta file

  @return false if success
  @return true if fail
  */
  bool ShowTimePointFromWalArchiveChildMetaOffset(
      std::vector<std::shared_ptr<local_backup_point>> &timePointList,
      std::string &metaPath, uint32_t maxExpectCount,
      MetaFileCursor &childMetaCursor, DSTORE::Timestamp timeStamp,
      bool lastParentPoint);

  static LocalBackupFileMgr *m_fileMgr;
  static std::mutex m_instance_mtx;

  LocalBackupFile *m_fullParentMeta{nullptr};
  LocalBackupFile *m_walArchiveParentMeta{nullptr};
  LocalBackupFile *m_walArchiveChildMeta{nullptr};
  LocalBackupFile *m_fullLbMetaJson{nullptr};
  LocalBackupFile *m_fullLbBinlogMeta{nullptr};
  std::vector<LocalBackupFile *> m_fullDataFileList;
  std::vector<LocalBackupFile *> m_walArchiveFileList;
  FullLocalBackupProgress m_progress;

  /**
   * The Binlog File Mgr which provides binlog point and binlog meta file
   * operations
   */
  LocalBackupBinlogFileMgr m_binlogFileMgr;

  std::mutex m_fullLock;
  std::mutex m_outLock;
  std::mutex m_progressLock;

  /* Write times for tablespace file, wal file and control file. */
  uint64_t m_tablespaceFileWriteTimes{0};
  uint64_t m_walFileWriteTimes{0};
  uint64_t m_controlFileWriteTimes{0};

  std::vector<BakFileGroup> m_currentObsObj;
  std::vector<uint16_t> m_currentObsObjIdx;
};

/* Custom Deleter: closes files and frees memory. */
struct LocalBackupFileDeleter {
  void operator()(LocalBackupFile *p) const {
    if (p) {
      p->CloseFile();
      delete p;
    }
  }
};

using LocalBackupFileUniquePtr =
    std::unique_ptr<LocalBackupFile, LocalBackupFileDeleter>;

}  // namespace CDE

#endif
