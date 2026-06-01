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
 * local_backup_binlog_file_mgr.h
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup_binlog_file_mgr.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef LOCAL_BACKUP_BINLOG_FILE_MGR_H
#define LOCAL_BACKUP_BINLOG_FILE_MGR_H

#include <cstdint>
#include <string>
#include <vector>
#include "common/dstore_common_utils.h"
#include "local_backup.h"  // LocalBackupBinlogPoint
#include "packutils.h"

#ifndef IS_DSTORE_BACKUP_TOOL
#include "sql/local_backup/local_backup_binlog.h"
#endif

namespace CDE {

class LocalBackupFile;

class LocalBackupBinlogFileMgr {
 public:
  LocalBackupBinlogFileMgr() {}
  ~LocalBackupBinlogFileMgr() {}

#ifndef IS_DSTORE_BACKUP_TOOL
  /**
  Prepare the Consistent Binlog Point logic.
  After the ControlFile is backuped during a full backup, this
  function would be called.
  It would acquire the binlog backup lock and get the Binlog point.

  @param[in]  backupDataPath      the backup data path

  @return false if success
  @return true if fail
  */
  bool PrepareConsistentBinlogPoint(const char *backupDataPath);

  /**
  Finish the Consistent Binlog Point logic.
  After the WAL copy is done, this funciton would be called again.
  It would release the binlog backup lock.

  @param[in]  backupMetaPpath      the backup meta path
  @param[in]  backupPoint          the lsn point to be saved

  @return false if success
  @return true if fail
  */
  bool FinishConsistentBinlogPoint(const char *backupMetaPath,
                                   local_backup_point &backupPoint);

  void GetBinlogPoint(LocalBackupBinlogPoint &point);
#endif

  /**
    Read full local backup binlog meta info from meta file

    @param[in]  metaPath        The path of binlog meta file
    @param[out]  binlogPoints   The vector of Binlg Points

    @return false if success
    @return true  if fail
    */
  bool ReadBinlogMeta(std::string &metaPath,
                      std::vector<LocalBackupBinlogPoint> &binlogPoints);

 private:
  bool ReadBinlogMetaHeader(LocalBackupFile *binlogMetaFile,
                            PackHeaderCrc &fileHeader, uint64_t offset);

  bool WriteBinlogMetaHeader(LocalBackupFile *binlogMetaFile,
                             PackHeaderCrc &fileHeader);

  bool OpenBinlogMeta(std::string &metaPath, bool createFlag, bool &isCreated,
                      LocalBackupFile **metaFile);

  /* the backup monitor tool should not use the SQL layer logic */
#ifndef IS_DSTORE_BACKUP_TOOL
  /**
  Write the Full Backup Binlog Meta file

  @param[in]  binlogPoint   The Binlog point to be saved
  @param[in]  path          The meta file path

  @return false if success
  @return true if fail
  */
  bool WriteBinlogMeta(LocalBackupBinlogPoint &binlogPoint, std::string &path);

  /**
   * The SQL layer binlog logic manager
   */
  LocalBackup_binlog_mgr m_binlogMgr;
#endif
};

}  // namespace CDE

#endif
