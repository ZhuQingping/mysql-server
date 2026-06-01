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
 * local_backup.h
 *
 *
 *
 * IDENTIFICATION
 *        storage/dstore/backup/local_backup.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef LOCAL_BACKUP_H
#define LOCAL_BACKUP_H

#include <string>
#include <vector>
#include "sql/handler.h"

#include "framework/dstore_instance_interface.h"
#include "local_backup/dstore_local_backup_interface.h"

extern bool dstore_open_restore_mode;
extern bool dstore_can_do_wal_archive;
extern char *dstore_local_backup_meta_path;
extern char *dstore_restore_meta_path;
extern bool dstore_exit_after_restore;
extern uint64_t dstore_local_backup_progress_size_interval;
extern bool dstore_meta_json_crc;
extern uint32_t dstore_local_full_backup_sync_interval;

namespace CDE {

enum class LocalBackupCmd {
  START_FULL_LB,
  STOP_FULL_LB,
  START_WAL_ARCHIVE,
  STOP_WAL_ARCHIVE,
  SET_WAL_STARTRECOVERY_PLSN,
  SET_WAL_ARCHIVE_PLSN
};

/**
Process cmd START_FULL_LB and STOP_FULL_LB

@param[in]   cmd The command of full local backup
@param[in]   config The path config including data and meta for output file
stream to the path
@param[in]   involveWalArchive Whether consider using lsn point to catch wal
recycle in the view of archive lsn
@param[in]   involveStandby Whether consider using lsn point to catch wal
recycle in the view of standby lsn
@param[out]  backupPoint The point information to be these fully local backed
up.

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus ProcessFullLocalBackup(LocalBackupCmd cmd,
                                         local_backup_config *config,
                                         bool involveWalArchive,
                                         bool involveStandby,
                                         local_backup_point *backupPoint);

/**
Process wal archive command for START_WAL_ARCHIVE and STOP_WAL_ARCHIVE

@param[in]  cmd The wal archive command
@param[in]  config The config path for data and meta file

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus ProcessWalArchive(LocalBackupCmd cmd,
                                    local_backup_config *config);

/**
Check whether open the restore mode
@return true if open the resotre mode
@return false if not open the restore mode
*/
bool NeedRestore();

/**
Check whether can do wal archive
@return true if can do wal archive
@return false if can not do wal archive
*/
bool CanDoWalArchive();

/**
process SET_WAL_STARTRECOVERY_PLSN for restore and SET_WAL_ARCHIVE_PLSN for wal
archive before dstore crash recovery

@param[in]  cmd The command of SET_WAL_STARTRECOVERY_PLSN and
SET_WAL_ARCHIVE_PLSN
@param[in]  config The path of meta file which is used to search lsn point

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus PrepareRecovery(LocalBackupCmd cmd,
                                  local_backup_config *config);

/**
Finish full local backup binlog point logic, which would release the lock.
finish including dstore and innodb.

@param[in]  backupPoint  The full local backup point info including time and lsn

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus FinishFullBackupBinlog(local_backup_point &backupPoint);

/**
Write full local backup point info to meta file when full local backup
finish including dstore and innodb.

@param[in]  backupPoint  The full local backup point info including time and lsn
@param[in]  path The path of meta file

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus WriteFullBackupMetaInfo(local_backup_point &backupPoint,
                                          std::string &path);

/**
write full backup point to full local backup restore meta file

@param[in]  backupPoint        full local backup point of restore meta
@param[in]  metaPath          the path of full local backup meta
@param[in]  restoreMetaPath  the path of restore meta

@return false on succeed.
@return true if error.
*/
bool WriteFullBackupRestoreMeta(local_backup_point &backupPoint,
                                std::string &metaPath,
                                std::string &restoreMetaPath);

/**
Write non-dstore part status and error message, backup point info and binlog
info to full local backup meta json file

@param[in]  errMsg The non-dstore part error message
@param[in]  backupPoint The local backup point
@param[in]  path The path of meta json file

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus WriteFullBackupMetaJson(std::string *errMsg,
                                          local_backup_point *backupPoint,
                                          std::string &path);

/**
Create restore meta info wrting to new restore meta file based on time point by
search meta file above full local backup or wal archive. The restore meta file
is used to restore database to time point

@param[in]  timePoint The time corresponding to one round of full backup or wal
archive
@param[in]  metaPath The meta path
@param[out] expectPoint Get the local backup point during creating restore meta
@param[in]  restoreMetaPath The path of restore meta file created
@param[in]  restoreMetaNameChanged Whether the name of restore meta file is
changed

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus CreateRestoreMeta(DSTORE::Timestamp timePoint,
                                    std::string &metaPath,
                                    local_backup_point &expectPoint,
                                    std::string &restoreMetaPath,
                                    bool restoreMetaNameChanged);

/**
Show the full data dir name and wal archived file names based on time point.
It is used to get data file or dir list to copy when restoring

@param[in]  timePoint The time point on restore
@param[in]  metaPath meta path
@param[out] point The local backup point for restore
@param[out] fullDir The full data dir from one full local backup
@param[out] walFileList The wal file list from last full local backup lsn point
to the lsn point that restored in the current round.
@param[in]  restoreMetaNameChanged Whether the name of restore meta file is
changed

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus ShowRestoreFileList(DSTORE::Timestamp timePoint,
                                      std::string &metaPath,
                                      local_backup_point &point,
                                      std::string &fullDir,
                                      std::vector<std::string> &walFileList,
                                      bool restoreMetaNameChanged);

struct MetaFileCursor {
  /** [in] The Meta file name searched */
  std::string fileName;
  /** [in] The begin offset during searching meta file */
  uint64_t beginOffset;
  /** [out] The file length have been searched, and will be beginOffset for next
   * search */
  uint64_t endFileLength;
};

/**
Show the backup points from one offset in full local backup meta.

@param[out]  timePointList  The list of time and lsn point for full local backup
@param[in]   metaPath The path of meta file
@param[in]   maxExpectCount The max count of continue timePoint for full local
backup during these search
@param[in, out]  parentMetaCursor The search cursor for full local backup meta
                    note: the fileName is nullptr as paremeter.
@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus ShowTimePointFromFullBackupMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &parentMetaCursor);

/**
Show the backup points from one offset in wal archive meta.

@param[out]      timePointList  The list of time and lsn point for wal archive
@param[in]       metaPath The path of meta file
@param[in]       maxExpectCount The max count of continue timePoint for wal
archive during these search
@param[in, out]  parentMetaCursor The search parent meta cursor for wal archive
meta note: the fileName is nullptr as paremeter
@param[in, out]  childMetaCursor The search child meta cursor for wal archive
meta
@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus ShowTimePointFromWalArchiveMetaOffset(
    std::vector<std::shared_ptr<local_backup_point>> &timePointList,
    std::string &metaPath, uint32_t maxExpectCount,
    MetaFileCursor &parentMetaCursor, MetaFileCursor &childMetaCursor);

/**
Local Backup Binlog Point is consists of binlog part and backup point.
*/
struct LocalBackupBinlogPoint {
  std::string gtidSet;
  std::string binlogFile;
  uint64_t binlogFilePos;
  local_backup_point lsnPoint;
  LocalBackupBinlogPoint() : binlogFilePos(0) {}
  std::string toString();
};

/**
Read full local backup binlog meta info from meta file

@param[in]  metaPath        The path of binlog meta file
@param[out] binlogPoint     The binlogPoint list

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus ReadFullLocalBackupBinlogMeta(
    std::string &metaPath, std::vector<LocalBackupBinlogPoint> &binlogPoint);

/**
Read full local backup meta json file, and verify if the crc is matched.

@param[in]   path The meta json file path
@param[out]  storageCrc The crc value stored in meta json file head.
@param[out]  calcCrc The crc value calculated by the meta json file data.

@return DSTORE_SUCC if success
@return DSTORE_FAIL if fail
*/
DSTORE::RetStatus VerifyFullBackupMetaJsonCrc(std::string &path,
                                              ha_checksum &storageCrc,
                                              ha_checksum &calcCrc);

}  // namespace CDE

#endif
