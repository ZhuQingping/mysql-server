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

#include <dirent.h>
#include <gtest/gtest.h>
#include <fstream>
#include "securec.h"

#include "sql/field.h"
#include "sql/table.h"

/* There are 'MAX_FILE_SIZE' in my_base.h and
dstore/interface/tablespace/dstore_tablespace_struct.h */
#undef MAX_FILE_SIZE

#include "local_backup/dstore_local_backup_interface.h"
#include "local_backup/dstore_local_backup_wal.h"
#include "pdb/dstore_pdb_interface.h"
#include "transaction/dstore_transaction_interface.h"
#include "transaction/dstore_transaction_types.h"

#include "backup/local_backup.h"
#include "backup/local_backup_file_mgr.h"
#include "backup/packutils.h"
#include "boot/cde_instance.h"
#include "ddl/cde_dd_table.h"
#include "ddl/cde_ddl.h"
#include "dict/cde_dict.h"
#include "dml/cde_dml.h"
#include "dml/cde_heap.h"
#include "sql/local_backup/local_backup_obs_download.h"

#include "utils/ut_cde_test.h"
#include "utils/ut_common.h"
#include "utils/ut_mysql_mock.h"

using namespace DSTORE;
using namespace LocalBackupInterface;

#define DEREF_TAG (0x1111000000000000ULL)

namespace CDE {

extern StorageGUC g_guc;
extern StorageInstanceInterface *g_instance;

std::atomic<uint64_t> g_archiveLsn;

RetStatus MockLocalBackupCallback(void *targetConfig, LocalBackup *backup) {
  if (backup->statusType != BackupStatusType::NOT_STATUS) {
    EXPECT_NE(backup->status.runningStatus, BackupRunningStatus::IN_PROGRESS);
    if (backup->status.runningStatus == BackupRunningStatus::ABORTED) {
      EXPECT_FALSE(backup->status.errMsg.empty());
    } else {
      EXPECT_TRUE(backup->status.errMsg.empty());
    }
    return DSTORE_SUCC;
  }
  EXPECT_EQ(backup->status.runningStatus, BackupRunningStatus::IN_PROGRESS);

  local_backup_config *config = (local_backup_config *)targetConfig;
  std::stringstream ss;
  ss << config->data_base_path << "/";

  switch (backup->type) {
    case BACKUP_TABLESPACE: {
      std::string fileName(backup->fileName);
      size_t pos = fileName.find_last_of('/');
      if (pos == std::string::npos) {
        ss << fileName;
      } else {
        ss << fileName.substr(pos + 1);
      }
      break;
    }
    case BACKUP_WAL:
      ss << "wal/";
      mkdir(ss.str().c_str(), 0755);
      ss << backup->fileName;
      break;
    case BACKUP_CONTROL_FILE:
      ss << backup->fileName;
      break;
    default:
      EXPECT_TRUE(false);
  }

  std::ofstream archFile(ss.str(), std::ios::binary | std::ios::app);
  EXPECT_FALSE(!archFile);

  archFile.write(reinterpret_cast<const char *>(backup->buffer),
                 backup->length);
  EXPECT_FALSE(!archFile);
  archFile.close();
  return DSTORE_SUCC;
}

RetStatus MockArchiveCallback(void *targetConfig, LocalBackup *backup) {
  EXPECT_EQ(backup->type, BACKUP_WAL);

  WalFileBackup *walBackup = (WalFileBackup *)backup;
  EXPECT_TRUE(!walBackup->setPoint || walBackup->timePoint != 0);
  EXPECT_TRUE(g_archiveLsn.load() < walBackup->lsnPoint);
  g_archiveLsn.store(walBackup->lsnPoint);

  local_backup_config *config = (local_backup_config *)targetConfig;
  std::stringstream ss;
  ss << config->data_base_path << "/" << walBackup->fileName;

  std::ofstream archFile(ss.str(), std::ios::binary | std::ios::app);
  EXPECT_FALSE(!archFile);

  archFile.write(reinterpret_cast<const char *>(walBackup->buffer),
                 walBackup->length);
  EXPECT_FALSE(!archFile);
  archFile.close();
  return DSTORE_SUCC;
}

class test_cde_local_backup : public CDETEST {
 protected:
  void SetUp() override {
    CDETEST::SetUp();
    CDETEST::Bootstrap();

    g_guc.bgDiskWriterSlaveNum = 1;
    g_guc.checkpointTimeout = 1;
    g_guc.walArchiveInterval = 1;
    /* For StoragePdb::ClosePdb(). selfNodeId 0 will cause it fail. */
    g_guc.selfNodeId = 1;
    /* local backup progress parameter. */
    dstore_local_backup_progress_size_interval = 209715200;
    CDETEST::Start();

    std::stringstream ss;
    ss << ut_cde_cfg::get_data_path() << "/dstore_full_backup";
    m_fullBackupDir = ss.str();
    ss.str("");
    ss << ut_cde_cfg::get_data_path() << "/dstore_full_backup_meta";
    m_fullBackupMetaDir = ss.str();
    ss.str("");
    ss << ut_cde_cfg::get_data_path() << "/dstore_incre_backup";
    m_increBackupDir = ss.str();
    mkdir(m_fullBackupDir.c_str(), 0755);
    mkdir(m_increBackupDir.c_str(), 0755);
    mkdir(m_fullBackupMetaDir.c_str(), 0755);
    dstore_local_backup_meta_path = m_fullBackupMetaDir.data();
    g_archiveLsn.store(0);
  }

  void TearDown() override {
    SetWalArchiveLsn(g_defaultPdbId, UINT64_MAX);

#ifndef NDEBUG
    UtDestroyLocalBackupInstance();
#endif /* NDEBUG */

    CDETEST::Stop();
    CDETEST::TearDown();
  }

  std::string m_fullBackupDir;
  std::string m_fullBackupMetaDir;
  std::string m_increBackupDir;

  bool WaitArchiveLsn(uint64_t lsn, int waitSec = 5) {
    for (int i = 0; i < waitSec * 10; i++) {
      if (g_archiveLsn.load() >= lsn) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

  void CreateOneTable(const char *name) {
    TABLE *form = new ut_mysql_table(123456, name);
    form->s->primary_key = MAX_KEY;  // no primary key
    HA_CREATE_INFO *create_info = new HA_CREATE_INFO();
    dd::Table *table_def = ut_mysql_dd_init();

    TransactionInterface::StartTrxCommand();
    TransactionInterface::SetSnapShot();
    int ret = CdeCreateTable(nullptr, name, form, create_info, table_def);
    EXPECT_EQ(ret, 0);
    TransactionInterface::CommitTrxCommand();

    delete form;
    ut_mysql_dd_release(table_def);
    delete create_info;
  }

  void InsertRow(const char *tableName, cde_dict_t *d_heap,
                 session_rel_info *relInfo, uint64_t count = 1) {
    ut_mysql_table *table = new ut_mysql_table(123456, tableName);

    TransactionInterface::StartTrxCommand();
    TransactionInterface::SetSnapShot();

    cde_session_t session;
    session.trxinfo.check_foreigns = false;
    dml_ins_ctx ins_ctx(session.get_trxinfo(), d_heap, relInfo,
                        table->s->fields, 0, nullptr);
    ins_ctx.init();
    for (uint32_t i = 0; i < count; i++) {
      EXPECT_EQ(CdeDmlWriteRow(table, table->record[0], &ins_ctx), 0);
    }
    TransactionInterface::CommitTrxCommand();

    delete table;
  }

  uint64_t GetRowCount(dstore_handler_t *dstore_handler,
                       DSTORE::StorageRelationData *rd_storage_releation) {
    dstore_handler->dstore_heap_scan =
        HeapInterface::CreateHeapScanHandler(rd_storage_releation);

    TransactionInterface::StartTrxCommand();
    TransactionInterface::SetSnapShot();

    DSTORE::SnapshotData dstore_snapshot;
    CDESetSnapshotByTrans(dstore_snapshot);
    HeapInterface::BeginScan(dstore_handler->dstore_heap_scan,
                             &dstore_snapshot);

    uint64_t rows = 0;
    DSTORE::HeapTuple *tuple =
        HeapInterface::SeqScan(dstore_handler->dstore_heap_scan);
    while (tuple != nullptr) {
      rows++;
      tuple = HeapInterface::SeqScan(dstore_handler->dstore_heap_scan);
    }

    HeapInterface::EndScan(dstore_handler->dstore_heap_scan);
    HeapInterface::DestroyHeapScanHandler(dstore_handler->dstore_heap_scan);
    dstore_handler->dstore_heap_scan = nullptr;

    TransactionInterface::CommitTrxCommand();
    return rows;
  }

  cde_dict_t *buildCdeDictTable(Oid oid, const char *name, PageId segmentId) {
    cde_dict_t *dict_table = new (std::nothrow) cde_dict_t();
    if (dict_table == nullptr) {
      return nullptr;
    }
    ut_mysql_table *form = new (std::nothrow) ut_mysql_table(123456, name);
    if (form == nullptr) {
      delete dict_table;
      return nullptr;
    }
    if (form->s == nullptr || form->field == nullptr) {
      delete form;
      delete dict_table;
      return nullptr;
    }
    dict_table->m_dictCols = new DictCol[2];
    dict_table->m_dictCols[0].m_isVisible = true;
    dict_table->m_dictCols[0].m_isVirtual = false;
    dict_table->m_dictCols[0].m_phyPos = 0;
    dict_table->m_dictCols[1].m_isVisible = true;
    dict_table->m_dictCols[1].m_isVirtual = false;
    dict_table->m_dictCols[1].m_phyPos = 1;

    CdeCreateTableInfo tableInfo(CDE_DEFAULT_TABLE_SPACE_ID);
    StorageRelation heap_rel = tableInfo.CreateHeapStorRel(
        oid, segmentId, INVALID_PAGE_ID, form, g_defaultFillfactor);
    EXPECT_NE(heap_rel, nullptr);
    dict_table->dstore_relation = heap_rel;
    dict_table->name.assign(name);
    for (uint i = 0; i < form->s->fields; i++) {
      Field *mysql_field = form->field[i];
      std::string col_name(mysql_field->field_name);
      dict_table->col_names.push_back(col_name);
    }
    dict_table->m_id =
        CdeFormDictTableId(heap_rel->rel->reltablespace, heap_rel->relOid);
    dict_table->m_totalColCount = form->s->fields;
    delete form;
    return dict_table;
  }

  void CopyFile(std::string src, std::string dest) {
    std::ifstream srcFile(src, std::ios::binary);
    EXPECT_FALSE(!srcFile);
    std::ofstream destFile(dest, std::ios::binary | std::ios::trunc);
    EXPECT_FALSE(!destFile);

    destFile << srcFile.rdbuf();
    EXPECT_FALSE(!srcFile);
    EXPECT_FALSE(!destFile);
  }

  void CopyFilesInDir(std::string srcDir, std::string destDir) {
    DIR *dir = opendir(srcDir.c_str());
    EXPECT_NE(dir, nullptr);

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }

      std::stringstream ssSrc;
      ssSrc << srcDir << "/" << entry->d_name;

      struct stat statbuf;
      EXPECT_EQ(stat(ssSrc.str().c_str(), &statbuf), 0);
      if (!S_ISREG(statbuf.st_mode)) {
        continue;
      }

      std::stringstream ssDest;
      ssDest << destDir << "/" << entry->d_name;
      CopyFile(ssSrc.str(), ssDest.str());
    }

    closedir(dir);
  }

  void RestartWithBackupRestore(uint64_t recoveryStartLsn,
                                bool useArchive = false,
                                bool useDstore = true) {
    char pdbPath[1024];
    StoragePdbInterface::GetPdbPath(g_defaultPdbId, pdbPath);

#ifndef NDEBUG
    UtDestroyLocalBackupInstance();
#endif /* NDEBUG */
    CDETEST::Stop();

    std::stringstream ss;
    ss << pdbPath << "_orig";
    EXPECT_EQ(rename(pdbPath, ss.str().c_str()), 0);
    EXPECT_EQ(rename(m_fullBackupDir.c_str(), pdbPath), 0);

    if (useArchive) {
      std::stringstream ssWalDir;
      ssWalDir << pdbPath << "/wal";
      CopyFilesInDir(m_increBackupDir, ssWalDir.str());
    }

    if (useDstore) {
      SetWalRecoveryStartLsn(g_defaultPdbId, recoveryStartLsn);
    } else {
      DIR *dir = opendir(m_fullBackupMetaDir.c_str());
      EXPECT_NE(dir, nullptr);
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "RestoreMeta_", strlen("RestoreMeta_")) ==
            0) {
          std::stringstream ssSrc;
          ssSrc << m_fullBackupMetaDir << "/" << entry->d_name;
          std::stringstream ssDest;
          ssDest << m_fullBackupMetaDir << "/"
                 << "RestoreMeta_RestoreMeta";
          CopyFile(ssSrc.str(), ssDest.str());
          break;
        }
      }
      closedir(dir);

      local_backup_config config;
      config.data_base_path = m_fullBackupDir;
      config.meta_base_path = m_fullBackupMetaDir;
      RetStatus ret = CDE::PrepareRecovery(
          CDE::LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN, &config);
      EXPECT_EQ(ret, DSTORE_SUCC);
    }
    CDETEST::Start();
    SetWalRecoveryStartLsn(g_defaultPdbId, UINT64_MAX);
  }
};

TEST_F(test_cde_local_backup, basic_backup_archive) {
  local_backup_config backupConfig;
  local_backup_config archiveConfig;
  FullLocalBackupPoint backupPoint;

  backupConfig.data_base_path = m_fullBackupDir;
  archiveConfig.data_base_path = m_increBackupDir;

  RetStatus ret = StartFullLocalBackup(g_defaultPdbId, MockLocalBackupCallback,
                                       &backupConfig, backupPoint, 0);
  EXPECT_EQ(ret, DSTORE_SUCC);

  SetWalArchiveLsn(g_defaultPdbId, 0);
  ret = StartWalArchive(g_defaultPdbId, MockArchiveCallback, &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = StopFullLocalBackup(g_defaultPdbId);
  EXPECT_EQ(ret, DSTORE_SUCC);

  EXPECT_TRUE(WaitArchiveLsn(backupPoint.archivePlsnPoint));
  ret = StopWalArchive(g_defaultPdbId);
  EXPECT_EQ(ret, DSTORE_SUCC);
}

TEST_F(test_cde_local_backup, full_backup_restore) {
  local_backup_config backupConfig;
  local_backup_config archiveConfig;
  FullLocalBackupPoint backupPoint;

  backupConfig.data_base_path = m_fullBackupDir;
  archiveConfig.data_base_path = m_increBackupDir;

  const char *tableName = "t1";
  CreateOneTable(tableName);

  cde_dict_t *d_heap = DictSysGetTable(tableName);
  EXPECT_NE(d_heap, nullptr);

  /* Keep table information before restart since we don't have DD in InnoDB */
  uint64_t heapOid = d_heap->m_id;
  PageId segmentId;
  segmentId.m_fileId = d_heap->get_dstore_relation()->rel->relfileid;
  segmentId.m_blockId = d_heap->get_dstore_relation()->rel->relblknum;

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  cde_dict_t *d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  InsertRow(tableName, d_heap_new, relInfo);

  RetStatus ret = StartFullLocalBackup(g_defaultPdbId, MockLocalBackupCallback,
                                       &backupConfig, backupPoint, 0);
  EXPECT_EQ(ret, DSTORE_SUCC);

  SetWalArchiveLsn(g_defaultPdbId, 0);
  ret = StartWalArchive(g_defaultPdbId, MockArchiveCallback, &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = StopFullLocalBackup(g_defaultPdbId);
  EXPECT_EQ(ret, DSTORE_SUCC);

  EXPECT_TRUE(WaitArchiveLsn(backupPoint.archivePlsnPoint));

  InsertRow(tableName, d_heap_new, relInfo);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation), 2UL);

  EXPECT_TRUE(WaitArchiveLsn(backupPoint.archivePlsnPoint + 1));
  ret = StopWalArchive(g_defaultPdbId);
  EXPECT_EQ(ret, DSTORE_SUCC);

  RestartWithBackupRestore(backupPoint.startRecoveryPlsn);

  /* For error branch coverage. */
  local_backup_config config;
  ret = CDE::PrepareRecovery(CDE::LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN,
                             &config);
  EXPECT_EQ(ret, DSTORE_FAIL);
  config.data_base_path = m_fullBackupDir;
  config.meta_base_path = m_fullBackupMetaDir;
  ret = CDE::PrepareRecovery(CDE::LocalBackupCmd::START_FULL_LB, &config);
  EXPECT_EQ(ret, DSTORE_FAIL);

  delete d_heap_new;
  d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem2 = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfoNew(new (mem2) session_rel_info(0));
  relInfoNew->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  /* Should get only one row. */
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation), 1UL);

  InsertRow(tableName, d_heap_new, relInfoNew, 2);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation), 3UL);

  delete dstore_handler;
  delete d_heap_new;
  std::free(mem);
  mem = nullptr;
  std::free(mem2);
  mem2 = nullptr;
}

TEST_F(test_cde_local_backup, full_backup_archive_restore) {
  local_backup_config backupConfig;
  local_backup_config archiveConfig;
  FullLocalBackupPoint backupPoint;

  backupConfig.data_base_path = m_fullBackupDir;
  archiveConfig.data_base_path = m_increBackupDir;

  const char *tableName = "t1";
  CreateOneTable(tableName);

  cde_dict_t *d_heap = DictSysGetTable(tableName);
  EXPECT_NE(d_heap, nullptr);

  /* Keep table information before restart since we don't have DD in InnoDB */
  uint64_t heapOid = d_heap->m_id;
  PageId segmentId;
  segmentId.m_fileId = d_heap->get_dstore_relation()->rel->relfileid;
  segmentId.m_blockId = d_heap->get_dstore_relation()->rel->relblknum;

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  cde_dict_t *d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  InsertRow(tableName, d_heap_new, relInfo);

  RetStatus ret = StartFullLocalBackup(g_defaultPdbId, MockLocalBackupCallback,
                                       &backupConfig, backupPoint, 0);
  EXPECT_EQ(ret, DSTORE_SUCC);

  SetWalArchiveLsn(g_defaultPdbId, 0);
  ret = StartWalArchive(g_defaultPdbId, MockArchiveCallback, &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = StopFullLocalBackup(g_defaultPdbId);
  EXPECT_EQ(ret, DSTORE_SUCC);

  EXPECT_TRUE(WaitArchiveLsn(backupPoint.archivePlsnPoint));

  InsertRow(tableName, d_heap_new, relInfo);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation), 2UL);

  /* Wait next checkpoint */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  EXPECT_TRUE(WaitArchiveLsn(backupPoint.archivePlsnPoint + 1));
  ret = StopWalArchive(g_defaultPdbId);
  EXPECT_EQ(ret, DSTORE_SUCC);

  RestartWithBackupRestore(backupPoint.startRecoveryPlsn, true);

  delete d_heap_new;
  d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem2 = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfoNew(new (mem2) session_rel_info(0));
  relInfoNew->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  /* Should get two rows. */
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation), 2UL);

  InsertRow(tableName, d_heap_new, relInfoNew, 3);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation), 5UL);

  delete dstore_handler;
  delete d_heap_new;
  std::free(mem);
  mem = nullptr;
  std::free(mem2);
  mem2 = nullptr;
}

TEST_F(test_cde_local_backup, cde_basic_full_backup_restore) {
  local_backup_config backupConfig;
  local_backup_point backupPoint;

  backupConfig.root_path = m_fullBackupDir;
  backupConfig.data_base_path = m_fullBackupDir;
  backupConfig.meta_base_path = m_fullBackupMetaDir;

  const char *tableName = "t1";
  CreateOneTable(tableName);

  cde_dict_t *d_heap = DictSysGetTable(tableName);
  EXPECT_NE(d_heap, nullptr);

  /* Keep table information before restart since we don't have DD in InnoDB */
  uint64_t heapOid = d_heap->m_id;
  PageId segmentId;
  segmentId.m_fileId = d_heap->get_dstore_relation()->rel->relfileid;
  segmentId.m_blockId = d_heap->get_dstore_relation()->rel->relblknum;

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  cde_dict_t *d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  uint64_t rows = 10000;
  InsertRow(tableName, d_heap_new, relInfo, rows);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation), rows);

  RetStatus ret =
      CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB,
                                  &backupConfig, false, false, &backupPoint);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::STOP_FULL_LB, nullptr,
                                    false, false, nullptr);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::WriteFullBackupMetaInfo(backupPoint, backupConfig.meta_base_path);
  EXPECT_EQ(ret, DSTORE_SUCC);

  InsertRow(tableName, d_heap_new, relInfo, 1);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation),
            rows + 1);

  ret = CDE::CreateRestoreMeta(backupPoint.time_point,
                               backupConfig.meta_base_path, backupPoint,
                               backupConfig.meta_base_path, false);
  EXPECT_EQ(ret, DSTORE_SUCC);
  RestartWithBackupRestore(backupPoint.start_recovery_lsn, false, false);

  delete d_heap_new;
  d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem2 = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfoNew(new (mem2) session_rel_info(0));
  relInfoNew->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  /* Should not get rows inserted after backup finish */
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation),
            rows);

  InsertRow(tableName, d_heap_new, relInfoNew, 2);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation),
            rows + 2);

  delete dstore_handler;
  delete d_heap_new;
  std::free(mem);
  mem = nullptr;
  std::free(mem2);
  mem2 = nullptr;
}

static bool g_workerStop = false;

TEST_F(test_cde_local_backup, cde_full_backup_parallel_with_insert) {
  local_backup_config backupConfig;
  local_backup_point backupPoint;

  backupConfig.root_path = m_fullBackupDir;
  backupConfig.data_base_path = m_fullBackupDir;
  backupConfig.meta_base_path = m_fullBackupMetaDir;

  const char *tableName = "t1";
  CreateOneTable(tableName);

  cde_dict_t *d_heap = DictSysGetTable(tableName);
  EXPECT_NE(d_heap, nullptr);

  /* Keep table information before restart since we don't have DD in InnoDB */
  uint64_t heapOid = d_heap->m_id;
  PageId segmentId;
  segmentId.m_fileId = d_heap->get_dstore_relation()->rel->relfileid;
  segmentId.m_blockId = d_heap->get_dstore_relation()->rel->relblknum;

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  cde_dict_t *d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  uint64_t rows = 11000;
  InsertRow(tableName, d_heap_new, relInfo, rows);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation), rows);

  /* Start a thread to keep insert. */
  g_workerStop = false;
  std::thread worker([this, tableName, d_heap_new, dstore_handler, relInfo]() {
    g_instance->CreateThreadAndRegister(g_defaultPdbId, false);
    ThreadContextInterface::GetCurrentThreadContext()->InitTransactionRuntime(
        g_defaultPdbId, nullptr, nullptr);
    while (!g_workerStop) {
      InsertRow(tableName, d_heap_new, relInfo, 1);
    }
    ThreadContextInterface::GetCurrentThreadContext()
        ->DestroyTransactionRuntime();
    g_instance->UnregisterThread();
  });

  RetStatus ret =
      CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB,
                                  &backupConfig, false, false, &backupPoint);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::STOP_FULL_LB, nullptr,
                                    false, false, nullptr);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::WriteFullBackupMetaInfo(backupPoint, backupConfig.meta_base_path);
  EXPECT_EQ(ret, DSTORE_SUCC);

  g_workerStop = true;
  worker.join();
  uint64_t finalRows =
      GetRowCount(dstore_handler, relInfo->rd_storage_releation);
  EXPECT_TRUE(finalRows > rows);

  ret = CDE::CreateRestoreMeta(backupPoint.time_point,
                               backupConfig.meta_base_path, backupPoint,
                               backupConfig.meta_base_path, false);
  EXPECT_EQ(ret, DSTORE_SUCC);
  RestartWithBackupRestore(backupPoint.start_recovery_lsn, false, false);

  delete d_heap_new;
  d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem2 = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfoNew(new (mem2) session_rel_info(0));
  relInfoNew->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  /* Should restore more than 'rows' */
  uint64_t restoreRows =
      GetRowCount(dstore_handler, relInfoNew->rd_storage_releation);
  EXPECT_TRUE(restoreRows > rows && restoreRows <= finalRows);

  InsertRow(tableName, d_heap_new, relInfoNew, 2);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation),
            restoreRows + 2);

  delete dstore_handler;  // Will destroy dstore_handler->rel_info
  delete d_heap_new;
  std::free(mem);
  mem = nullptr;
  std::free(mem2);
  mem2 = nullptr;
}

TEST_F(test_cde_local_backup, cde_basic_full_backup_archive_restore) {
  local_backup_config backupConfig;
  local_backup_config archiveConfig;
  local_backup_point backupPoint;

  backupConfig.root_path = m_fullBackupDir;
  backupConfig.data_base_path = m_fullBackupDir;
  backupConfig.meta_base_path = m_fullBackupMetaDir;
  archiveConfig.data_base_path = m_increBackupDir;
  // ArchiveMeta is in the same directory
  archiveConfig.meta_base_path = m_fullBackupMetaDir;

  const char *tableName = "t1";
  CreateOneTable(tableName);

  cde_dict_t *d_heap = DictSysGetTable(tableName);
  EXPECT_NE(d_heap, nullptr);

  /* Keep table information before restart since we don't have DD in InnoDB */
  uint64_t heapOid = d_heap->m_id;
  PageId segmentId;
  segmentId.m_fileId = d_heap->get_dstore_relation()->rel->relfileid;
  segmentId.m_blockId = d_heap->get_dstore_relation()->rel->relblknum;

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  cde_dict_t *d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  uint64_t rows = 1000;
  InsertRow(tableName, d_heap_new, relInfo, rows);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation), rows);

  RetStatus ret =
      CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB,
                                  &backupConfig, true, false, &backupPoint);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::STOP_FULL_LB, nullptr,
                                    false, false, nullptr);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::WriteFullBackupMetaInfo(backupPoint, backupConfig.meta_base_path);
  EXPECT_EQ(ret, DSTORE_SUCC);

  /* Insert data during archive */
  InsertRow(tableName, d_heap_new, relInfo, 1);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation),
            rows + 1);

  ret = CDE::ProcessWalArchive(CDE::LocalBackupCmd::START_WAL_ARCHIVE,
                               &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  /* Wait next checkpoint */
  std::this_thread::sleep_for(std::chrono::seconds(4));

  ret = CDE::ProcessWalArchive(CDE::LocalBackupCmd::STOP_WAL_ARCHIVE,
                               &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  /* Insert data after backup */
  InsertRow(tableName, d_heap_new, relInfo, 1);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation),
            rows + 2);

  RestartWithBackupRestore(backupPoint.start_recovery_lsn,
                           true /* use_archive*/, true /*use_dstore*/);

  delete d_heap_new;
  d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem2 = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfoNew(new (mem2) session_rel_info(0));
  relInfoNew->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  /* Should not get rows inserted after backup finish */
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation),
            rows + 1);

  InsertRow(tableName, d_heap_new, relInfoNew, 1);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation),
            rows + 2);

  delete dstore_handler;
  delete d_heap_new;
  std::free(mem);
  mem = nullptr;
  std::free(mem2);
  mem2 = nullptr;
}

TEST_F(test_cde_local_backup, cde_full_backup_archive_parallel_with_insert) {
  local_backup_config backupConfig;
  local_backup_config archiveConfig;
  local_backup_point backupPoint;

  backupConfig.root_path = m_fullBackupDir;
  backupConfig.data_base_path = m_fullBackupDir;
  backupConfig.meta_base_path = m_fullBackupMetaDir;
  archiveConfig.data_base_path = m_increBackupDir;
  // ArchiveMeta is in the same directory
  archiveConfig.meta_base_path = m_fullBackupMetaDir;

  const char *tableName = "t1";
  CreateOneTable(tableName);

  cde_dict_t *d_heap = DictSysGetTable(tableName);
  EXPECT_NE(d_heap, nullptr);

  /* Keep table information before restart since we don't have DD in InnoDB */
  uint64_t heapOid = d_heap->m_id;
  PageId segmentId;
  segmentId.m_fileId = d_heap->get_dstore_relation()->rel->relfileid;
  segmentId.m_blockId = d_heap->get_dstore_relation()->rel->relblknum;

  dstore_handler_t *dstore_handler = new dstore_handler_t;
  cde_dict_t *d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfo(new (mem) session_rel_info(0));
  relInfo->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  uint64_t rows = 1000;
  InsertRow(tableName, d_heap_new, relInfo, rows);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfo->rd_storage_releation), rows);

  /* Start a thread to keep insert. */
  g_workerStop = false;
  std::thread worker([this, tableName, d_heap_new, dstore_handler, relInfo]() {
    g_instance->CreateThreadAndRegister(g_defaultPdbId, false);
    ThreadContextInterface::GetCurrentThreadContext()->InitTransactionRuntime(
        g_defaultPdbId, nullptr, nullptr);
    while (!g_workerStop) {
      InsertRow(tableName, d_heap_new, relInfo, 1);
    }
    ThreadContextInterface::GetCurrentThreadContext()
        ->DestroyTransactionRuntime();
    g_instance->UnregisterThread();
  });

  RetStatus ret =
      CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB,
                                  &backupConfig, true, false, &backupPoint);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::STOP_FULL_LB, nullptr,
                                    false, false, nullptr);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::WriteFullBackupMetaInfo(backupPoint, backupConfig.meta_base_path);
  EXPECT_EQ(ret, DSTORE_SUCC);

  ret = CDE::ProcessWalArchive(CDE::LocalBackupCmd::START_WAL_ARCHIVE,
                               &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  /* keep running for several Archive rounds, WalArchiveInteval is 1 second. */
  std::this_thread::sleep_for(std::chrono::seconds(6));

  ret = CDE::ProcessWalArchive(CDE::LocalBackupCmd::STOP_WAL_ARCHIVE,
                               &archiveConfig);
  EXPECT_EQ(ret, DSTORE_SUCC);

  g_workerStop = true;
  worker.join();
  uint64_t finalRows =
      GetRowCount(dstore_handler, relInfo->rd_storage_releation);
  EXPECT_TRUE(finalRows > rows);

  ret = CDE::CreateRestoreMeta(backupPoint.time_point,
                               backupConfig.meta_base_path, backupPoint,
                               backupConfig.meta_base_path, false);
  EXPECT_EQ(ret, DSTORE_SUCC);
  RestartWithBackupRestore(backupPoint.start_recovery_lsn,
                           true /* use_archive*/, true /*use_dstore*/);

  delete d_heap_new;
  d_heap_new = buildCdeDictTable(heapOid, tableName, segmentId);
  dstore_handler->table_handler = d_heap_new;
  void *mem2 = std::malloc(sizeof(session_rel_info));
  session_rel_info *relInfoNew(new (mem2) session_rel_info(0));
  relInfoNew->rd_storage_releation =
      dstore_handler->table_handler->get_dstore_relation();

  /* Should restore more than 'rows' */
  uint64_t restoreRows =
      GetRowCount(dstore_handler, relInfoNew->rd_storage_releation);
  EXPECT_TRUE(restoreRows > rows && restoreRows <= finalRows);

  InsertRow(tableName, d_heap_new, relInfoNew, 2);
  EXPECT_EQ(GetRowCount(dstore_handler, relInfoNew->rd_storage_releation),
            restoreRows + 2);

  delete dstore_handler;
  delete d_heap_new;
  std::free(mem);
  mem = nullptr;
  std::free(mem2);
  mem2 = nullptr;
}

TEST_F(test_cde_local_backup, cde_local_backup_misc) {
  local_backup_config backupConfig;
  local_backup_point backupPoint;
  std::string path(m_fullBackupDir);
  path.append("notExist");
  backupConfig.root_path = path;
  backupConfig.data_base_path = path;
  backupConfig.meta_base_path = path;
  std::string name("notExist");

  RetStatus ret = CDE::ProcessFullLocalBackup(
      CDE::LocalBackupCmd::START_FULL_LB, nullptr, false, false, &backupPoint);
  EXPECT_EQ(ret, DSTORE_FAIL);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB,
                                    &backupConfig, false, false, nullptr);
  EXPECT_EQ(ret, DSTORE_FAIL);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB, nullptr,
                                    false, false, nullptr);
  EXPECT_EQ(ret, DSTORE_FAIL);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_WAL_ARCHIVE,
                                    &backupConfig, false, false, &backupPoint);
  EXPECT_EQ(ret, DSTORE_FAIL);

  ret = CDE::ProcessFullLocalBackup(CDE::LocalBackupCmd::START_FULL_LB,
                                    &backupConfig, true, true, &backupPoint);
  EXPECT_EQ(ret, DSTORE_FAIL);

  LocalBackupFile dataFile(LocalBackupFileType::DATA_LB, path, &name);
  EXPECT_TRUE(dataFile.CloseFile());

  uint64_t fileSize = 0;
  EXPECT_TRUE(dataFile.GetFileSize(fileSize));

  char buf[1024];
  uint64_t len = 1024;
  EXPECT_TRUE(dataFile.WriteSimple(buf, len, 0xFFFFFFFFFFFFFFFF));
  EXPECT_TRUE(dataFile.WriteSimple(buf, len, 0));
  EXPECT_TRUE(dataFile.Flush());

  LocalBackupPoint point;
  EXPECT_TRUE(dataFile.ReadParentLastItem(point));

  LocalBackupFile fullParentFile(LocalBackupFileType::FULL_PARENT_META_LB, path,
                                 &name);
  EXPECT_TRUE(fullParentFile.ReadParentLastItem(point));

  EXPECT_EQ(dataFile.ReadChildMeta(len), nullptr);
  EXPECT_TRUE(dataFile.WriteChildMetaItem(buf, 1024, point));

  LocalBackupFile archChildFile(LocalBackupFileType::ARCHIVE_CHILD_META_LB,
                                path, &name);
  EXPECT_TRUE(dataFile.WriteChildMetaItem(buf, 1024, point));

  bool res = false;
  EXPECT_TRUE(dataFile.IsContentFull(1024, res));
  EXPECT_TRUE(dataFile.IsDisjoined(res));
  EXPECT_TRUE(archChildFile.IsDisjoined(res));

  EXPECT_FALSE(LocalBackupFileMgr::CreateInstance());
  EXPECT_FALSE(LocalBackupFileMgr::CreateInstance());  // Cover create again

  LocalBackupFileMgr *fileMgr = LocalBackupFileMgr::GetInstance();

  bool create = false;
  uint16_t idx = fileMgr->AddLocalBackupFile(LocalBackupFileType::DATA_LB, true,
                                             path, &name, create, false);
  EXPECT_EQ(idx, LB_INVALID_IDX);

  fileMgr->RemoveLocalBackupFile(LocalBackupFileType::INVALID_LB, true);

  LocalBackupFile *f =
      fileMgr->GetLocalBackupFile(LocalBackupFileType::INVALID_LB, true, 1);
  EXPECT_EQ(f, nullptr);
  f = fileMgr->GetLocalBackupFile(LocalBackupFileType::DATA_LB, true, 1);
  EXPECT_EQ(f, nullptr);
  f = fileMgr->GetLocalBackupFile(LocalBackupFileType::DATA_LB, false, 1);
  EXPECT_EQ(f, nullptr);
  EXPECT_TRUE(fileMgr->WriteWalArchiveMeta(0, 0, path, false));

  local_backup_point lbPoint;
  EXPECT_TRUE(fileMgr->ReadLatestLocalBackupPoint(
      LocalBackupCmd::SET_WAL_ARCHIVE_PLSN, path, &lbPoint));
  EXPECT_TRUE(fileMgr->ReadLatestLocalBackupPoint(
      LocalBackupCmd::SET_WAL_STARTRECOVERY_PLSN, path, &lbPoint));
  EXPECT_TRUE(fileMgr->ReadLatestLocalBackupPoint(LocalBackupCmd::START_FULL_LB,
                                                  path, &lbPoint));

  std::vector<std::string> fileList;
  EXPECT_TRUE(
      fileMgr->ShowRestoreFileList(0, path, lbPoint, path, fileList, false));

  std::vector<std::shared_ptr<local_backup_point>> pointList;
  MetaFileCursor cursor;
  EXPECT_TRUE(fileMgr->ShowTimePointFromFullBackupMetaOffset(pointList, path, 0,
                                                             cursor));
  EXPECT_TRUE(fileMgr->ShowTimePointFromWalArchiveMetaOffset(pointList, path, 0,
                                                             cursor, cursor));

  LocalBackupFileMgr::DestroyInstance();
  LocalBackupFileMgr::DestroyInstance();  // To cover destroy again

  EXPECT_EQ(WriteFullBackupMetaInfo(lbPoint, path), DSTORE_FAIL);
  LocalBackupFileMgr::DestroyInstance();

  SetWalArchiveLsn(g_defaultPdbId, UINT64_MAX);
  DSTORE::WalFullLocalBackup::SetBackedUpPlsn(g_defaultPdbId, UINT64_MAX);
  EXPECT_EQ(ProcessWalArchive(LocalBackupCmd::START_WAL_ARCHIVE, &backupConfig),
            DSTORE_FAIL);
  EXPECT_EQ(ProcessWalArchive(LocalBackupCmd::START_FULL_LB, &backupConfig),
            DSTORE_FAIL);
  LocalBackupFileMgr::DestroyInstance();

  EXPECT_EQ(ShowRestoreFileList(0, path, lbPoint, path, fileList, false),
            DSTORE_FAIL);
}

TEST_F(test_cde_local_backup, cde_pack_utils) {
  uint32_t version = 1;
  uint16_t val1 = 100;
  uint64_t val2 = INT64_MAX;
  std::string strVal("Hello");

  uint32_t len = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint32_t) +
                 strVal.size() + PACKHEADERCRC_BYTE_SIZE;
  byte buf[128];

  PackBuffer packBuffer(buf, len);
  EXPECT_EQ(packBuffer.get_size(), len);
  PackHeaderCrc header1(version, len - PACKHEADERCRC_BYTE_SIZE);
  packBuffer.write_header(header1);
  packBuffer.write_bytes_2(val1);
  packBuffer.write_bytes_8(val2);
  packBuffer.write_bytes_4(strVal.size());
  packBuffer.write_string(strVal.data(), strVal.size());
  packBuffer.complete_record();

  PackBuffer packBuffer2(buf, len);
  PackHeaderCrc header2;
  EXPECT_FALSE(packBuffer2.read_header(&header2, true));
  ASSERT_EQ(header2.getVersion(), header1.getVersion());
  ASSERT_EQ(header2.getSize(), header1.getSize());
  EXPECT_EQ(packBuffer2.read_bytes_2(), val1);
  EXPECT_EQ(packBuffer2.read_bytes_8(), val2);

  uint32_t strLen = packBuffer2.read_bytes_4();
  EXPECT_EQ(strLen, strVal.size());

  std::string strVal2;
  strVal2.resize(strLen);
  EXPECT_FALSE(packBuffer2.read_string(strVal2.data(), strLen));
  EXPECT_TRUE(strVal == strVal2);

  /* illegal cases */
  byte buf1[10];
  PackBuffer packBuffer3(buf1, 10);
  EXPECT_TRUE(packBuffer3.write_string((char *)buf, 0));
  EXPECT_TRUE(packBuffer3.read_string((char *)buf, 0));

  PackBuffer packBuffer4(buf, len);
  PackHeaderCrc header4;
  buf[0] = 0;
  buf[1] = 0;
  EXPECT_TRUE(packBuffer4.read_header(&header4, true));
}

std::string GenRandomStr(size_t length) {
  const std::string chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device rd;
  std::mt19937 generator(rd());
  std::uniform_int_distribution<> dist(0, chars.size() - 1);

  std::ostringstream oss;
  for (size_t i = 0; i < length; ++i) {
    oss << chars[dist(generator)];
  }
  return oss.str();
}

bool GetObsAkSk(std::string &obs_ak, std::string &obs_sk) {
  char *char_obs_ak = getenv("OBS_AK");
  if (char_obs_ak == nullptr || char_obs_ak[0] == '\0') {
    return true;
  }
  obs_ak = char_obs_ak;

  char *char_obs_sk = getenv("OBS_SK");
  if (char_obs_sk == nullptr || char_obs_sk[0] == '\0') {
    return true;
  }
  obs_sk = char_obs_sk;

  return false;
}

TEST_F(test_cde_local_backup, cde_obs_handler) {
  std::string obs_ak, obs_sk;
  if (GetObsAkSk(obs_ak, obs_sk)) {
    GTEST_SKIP() << "OBS AK/SK env are required.";
  }

  std::map<std::string, std::string> obs_para = {
      {"obs_url", "obs.cn-southwest-244.ulanqab.huawei.com"},
      {"AK", obs_ak},
      {"SK", obs_sk},
      {"bucket_name", "taurus-dstore-bak"}};
  EXPECT_FALSE(create_lb_object_handler_instance(true, 1024 * 1024, obs_para));

  lb_object_handler *handler = fetch_lb_object_handler();
  EXPECT_NE(handler, nullptr);
  std::string obj_prefix("taurus/dstore/bak/test/");
  obj_prefix.append(GenRandomStr(10));
  obj_prefix.append("/");
  std::string obj_name(obj_prefix);
  obj_name.append("test_single_obj");
  std::string append_obj_name(obj_prefix);
  append_obj_name.append("test_append_obj");

  /* Remove objects last run left */
  std::vector<std::string> object_list;
  EXPECT_FALSE(handler->list_object_list(obj_prefix, object_list));
  for (uint i = 0; i < object_list.size(); i++) {
    EXPECT_FALSE(handler->remove_object(&object_list[i]));
  }
  object_list.clear();

  handler->make_object_name(obj_name, 0, 0, 0);

  size_t data_len = 100;
  std::unique_ptr<char[]> data1(new char[data_len]);
  EXPECT_EQ(memset_s(data1.get(), data_len, 'a', data_len), 0);
  EXPECT_EQ(memset_s(handler->m_buffer, handler->m_buffer_len, 'a', data_len),
            0);
  handler->m_buffer_offset = data_len;
  EXPECT_FALSE(handler->put_object());

  /* List object */
  lb_object_handler *handler2 = fetch_lb_object_handler();
  EXPECT_NE(handler2, nullptr);
  EXPECT_FALSE(handler2->list_object_list(obj_prefix, object_list));
  EXPECT_EQ(object_list.size(), 1);

  std::string obj_name_no_suffix;
  EXPECT_FALSE(
      lb_object_handler::get_prefix_name(object_list[0], obj_name_no_suffix));
  EXPECT_TRUE(obj_name_no_suffix == obj_name);
  come_back_lb_object_handler(handler2);

  /* Fetch object */
  handler2 = fetch_lb_object_handler();
  EXPECT_NE(handler2, nullptr);
  handler2->make_object_name(obj_name_no_suffix, 0, 0, 0);
  EXPECT_FALSE(handler2->get_full_object());
  EXPECT_EQ(handler2->m_buffer_offset, data_len);
  EXPECT_EQ(memcmp(data1.get(), handler2->m_buffer, data_len), 0);
  come_back_lb_object_handler(handler2);

  come_back_lb_object_handler(handler);

  /* Append object */
  handler = fetch_lb_object_handler();
  EXPECT_NE(handler, nullptr);
  handler->make_object_name(append_obj_name, 0, 0, 0);
  EXPECT_EQ(memset_s(handler->m_buffer, handler->m_buffer_len, 'a', data_len),
            0);
  handler->m_buffer_offset = data_len;
  EXPECT_FALSE(handler->append_object());
  std::unique_ptr<char[]> data2(new char[data_len]);
  EXPECT_EQ(memset_s(data2.get(), data_len, 'b', data_len), 0);
  EXPECT_EQ(memset_s(handler->m_buffer, handler->m_buffer_len, 'b', data_len),
            0);
  handler->m_buffer_offset = data_len;
  EXPECT_FALSE(handler->append_object());

  handler2 = fetch_lb_object_handler();
  EXPECT_NE(handler2, nullptr);
  handler2->make_object_name(append_obj_name, 0, 0, 0);
  uint64_t append_obj_len = 0;
  EXPECT_FALSE(handler2->get_object_length(append_obj_len));
  EXPECT_EQ(append_obj_len, data_len * 2);

  uint64_t real_len = 0;
  EXPECT_FALSE(handler2->get_object_stream(0, data_len, real_len));
  EXPECT_EQ(data_len, real_len);
  EXPECT_EQ(handler2->m_buffer_offset, data_len);
  EXPECT_EQ(memcmp(data1.get(), handler2->m_buffer, data_len), 0);
  EXPECT_FALSE(handler2->get_object_stream(data_len, data_len, real_len));
  EXPECT_EQ(data_len, real_len);
  EXPECT_EQ(handler2->m_buffer_offset, data_len);
  EXPECT_EQ(memcmp(data2.get(), handler2->m_buffer, data_len), 0);
  come_back_lb_object_handler(handler2);

  /* Remove objects */
  std::string append_obj_full_name = handler->get_name();
  EXPECT_FALSE(handler->remove_object(&append_obj_full_name));
  EXPECT_FALSE(handler->remove_object(&object_list[0]));
  object_list.clear();
  EXPECT_FALSE(handler->list_object_list(obj_prefix, object_list));
  EXPECT_EQ(object_list.size(), 0);
  come_back_lb_object_handler(handler);

  destroy_lb_object_handler_instance();
}

TEST_F(test_cde_local_backup, cde_obs_handler_max_append) {
  std::string obs_ak, obs_sk;
  if (GetObsAkSk(obs_ak, obs_sk)) {
    GTEST_SKIP() << "OBS AK/SK env are required.";
  }

  std::map<std::string, std::string> obs_para = {
      {"obs_url", "obs.cn-southwest-244.ulanqab.huawei.com"},
      {"AK", obs_ak},
      {"SK", obs_sk},
      {"bucket_name", "taurus-dstore-bak"}};
  EXPECT_FALSE(create_lb_object_handler_instance(true, 1024, obs_para));

  lb_object_handler *handler = fetch_lb_object_handler();
  EXPECT_NE(handler, nullptr);
  std::string obj_prefix("taurus/dstore/bak/test/");
  obj_prefix.append(GenRandomStr(10));
  obj_prefix.append("/");
  std::string obj_name(obj_prefix);
  obj_name.append("test_max_append_obj");

  /* Remove objects last run left */
  std::vector<std::string> object_list;
  EXPECT_FALSE(handler->list_object_list(obj_name, object_list));
  for (uint i = 0; i < object_list.size(); i++) {
    EXPECT_FALSE(handler->remove_object(&object_list[i]));
  }
  object_list.clear();

  handler->make_object_name(obj_name, 0, 0, 0);
  EXPECT_EQ(memset_s(handler->m_buffer, handler->m_buffer_len, 'a',
                     handler->m_buffer_len),
            0);

  uint max_append = 8000;
  for (uint i = 0; i < max_append; i++) {
    handler->m_buffer_offset = handler->m_buffer_len;
    EXPECT_FALSE(handler->append_object());
  }
  handler->m_buffer_offset = handler->m_buffer_len;
  // Should add a new object
  EXPECT_FALSE(handler->append_object());
  uint32_t type;
  uint32_t version;
  uint32_t id;
  EXPECT_FALSE(handler->get_number(handler->get_name(), &type, &version, &id));
  EXPECT_EQ(type, 0);
  EXPECT_EQ(version, 0);
  EXPECT_EQ(id, 1);

  object_list.clear();
  EXPECT_FALSE(handler->list_object_list(obj_name, object_list));
  EXPECT_EQ(object_list.size(), 2);

  for (size_t i = 0; i < object_list.size(); i++) {
    EXPECT_FALSE(handler->get_number(object_list[i], &type, &version, &id));
    EXPECT_EQ(type, 0);
    EXPECT_EQ(version, 0);
    uint64_t len = 0;

    lb_object_handler *h = fetch_lb_object_handler();
    EXPECT_NE(h, nullptr);
    h->make_object_name(obj_name, type, version, id);
    if (id == 0) {
      EXPECT_FALSE(h->get_object_length(len));
      EXPECT_EQ(len, max_append * handler->m_buffer_len);
    } else {
      EXPECT_EQ(id, 1);
      EXPECT_FALSE(h->get_object_length(len));
      EXPECT_EQ(len, handler->m_buffer_len);
    }
    come_back_lb_object_handler(h);

    EXPECT_FALSE(handler->remove_object(&object_list[i]));
  }

  come_back_lb_object_handler(handler);
  destroy_lb_object_handler_instance();
}

TEST_F(test_cde_local_backup, cde_obs_download) {
  std::string obs_ak, obs_sk;
  if (GetObsAkSk(obs_ak, obs_sk)) {
    GTEST_SKIP() << "OBS AK/SK env are required.";
  }

  std::map<std::string, std::string> obs_para = {
      {"obs_url", "obs.cn-southwest-244.ulanqab.huawei.com"},
      {"AK", obs_ak},
      {"SK", obs_sk},
      {"bucket_name", "taurus-dstore-bak"}};
  EXPECT_FALSE(create_lb_object_handler_instance(true, 1024 * 1024, obs_para));
  lb_object_handler *handler = fetch_lb_object_handler();
  EXPECT_NE(handler, nullptr);
  std::string obj_prefix("taurus/dstore/bak/test/download/");
  obj_prefix.append(GenRandomStr(10));
  obj_prefix.append("/");

  /* Upload one file to obs. */
  std::string obj_name(obj_prefix);
  obj_name.append("test_single_obj");
  handler->make_object_name(obj_name, 0, 0, 0);
  size_t data_len = 100;
  std::unique_ptr<char[]> data1(new char[data_len]);
  EXPECT_EQ(memset_s(data1.get(), data_len, 'a', data_len), 0);
  EXPECT_EQ(memset_s(handler->m_buffer, handler->m_buffer_len, 'a', data_len),
            0);
  handler->m_buffer_offset = data_len;
  EXPECT_FALSE(handler->put_object());
  come_back_lb_object_handler(handler);
  destroy_lb_object_handler_instance();

  /* Download from obs. */
  std::string prefix_key(obj_prefix);
  std::string config_name(m_fullBackupDir + "/obs_download.config");
  std::string trace_log_name(m_fullBackupDir + "/download_trace.json");
  uint32_t worker_num = 4;

  std::ofstream config_file(config_name, std::ios::binary | std::ios::app);
  EXPECT_FALSE(!config_file);
  std::ostringstream oss;
  oss << "{\"obs_mode\":true,\"buffer_len\":16777216,\"obs_url\":\"obs.cn-"
         "southwest-244.ulanqab.huawei.com\",\"ak\":\""
      << obs_ak << "\",\"sk\":\"" << obs_sk
      << "\",\"bucket_name\":\"taurus-dstore-bak\",\"base_path\":\""
      << m_fullBackupDir << "/obs\"}";
  config_file.write(oss.str().c_str(), oss.str().length());
  EXPECT_FALSE(!config_file);
  config_file.close();

  EXPECT_FALSE(start_lb_obs_objects_download(prefix_key, config_name,
                                             trace_log_name, worker_num));

  /* Check if the file downloaded from obs valid. */
  std::unique_ptr<char[]> backup_read_buf(new char[data_len]);
  std::string backup_file_name = m_fullBackupDir + "/obs/test_single_obj";
  std::ifstream backup_file(backup_file_name, std::ios::binary | std::ios::in);
  EXPECT_FALSE(!backup_file);
  backup_file.read(backup_read_buf.get(), data_len);
  EXPECT_EQ(memcmp(data1.get(), backup_read_buf.get(), data_len), 0);
  backup_file.close();

  /* Remove objects */
  EXPECT_FALSE(create_lb_object_handler_instance(true, 1024 * 1024, obs_para));
  handler = fetch_lb_object_handler();
  EXPECT_NE(handler, nullptr);
  std::vector<std::string> object_list;
  EXPECT_FALSE(handler->list_object_list(obj_prefix, object_list));
  for (uint i = 0; i < object_list.size(); i++) {
    EXPECT_FALSE(handler->remove_object(&object_list[i]));
  }
  object_list.clear();
  come_back_lb_object_handler(handler);
  destroy_lb_object_handler_instance();
}

TEST_F(test_cde_local_backup, cde_obs_handler_list_truncated) {
  std::string obs_ak, obs_sk;
  if (GetObsAkSk(obs_ak, obs_sk)) {
    GTEST_SKIP() << "OBS AK/SK env are required.";
  }

  std::map<std::string, std::string> obs_para = {
      {"obs_url", "obs.cn-southwest-244.ulanqab.huawei.com"},
      {"AK", obs_ak},
      {"SK", obs_sk},
      {"bucket_name", "taurus-dstore-bak"}};
  EXPECT_FALSE(create_lb_object_handler_instance(true, 1024, obs_para));

  lb_object_handler *handler = fetch_lb_object_handler();
  EXPECT_NE(handler, nullptr);
  std::string obj_prefix("taurus/dstore/bak/test/");
  obj_prefix.append(GenRandomStr(10));
  obj_prefix.append("/");
  std::string obj_name(obj_prefix);
  obj_name.append("test_list_truncated_obj");

  EXPECT_EQ(memset_s(handler->m_buffer, handler->m_buffer_len, 'a',
                     handler->m_buffer_len),
            0);

  uint max_put = 1100;
  for (uint i = 0; i < max_put; i++) {
    std::stringstream name_ss;
    name_ss << obj_name << i;
    std::string name = name_ss.str();
    handler->make_object_name(name, 0, 0, 0);
    handler->m_buffer_offset = handler->m_buffer_len;
    EXPECT_FALSE(handler->put_object());
  }

  std::vector<std::string> object_list;
  EXPECT_FALSE(handler->list_object_list(obj_name, object_list));
  EXPECT_EQ(object_list.size(), max_put);

  for (size_t i = 0; i < object_list.size(); i++) {
    uint32_t type;
    uint32_t version;
    uint32_t id;
    EXPECT_FALSE(handler->get_number(object_list[i], &type, &version, &id));
    EXPECT_EQ(type, 0);
    EXPECT_EQ(version, 0);
    EXPECT_EQ(id, 0);
    uint64_t len = 0;

    lb_object_handler *h = fetch_lb_object_handler();
    EXPECT_NE(h, nullptr);
    std::string name;
    EXPECT_FALSE(h->get_prefix_name(object_list[i], name));
    h->make_object_name(name, type, version, id);
    EXPECT_FALSE(h->get_object_length(len));
    EXPECT_EQ(len, handler->m_buffer_len);
    come_back_lb_object_handler(h);

    EXPECT_FALSE(handler->remove_object(&object_list[i]));
  }

  come_back_lb_object_handler(handler);
  destroy_lb_object_handler_instance();
}

}  // namespace CDE

GTEST_API_ int main(int argc, char **argv) {
  printf("Running main() from %s\n", __FILE__);

  MY_INIT("local_backup-test");
  CDE::CDETEST::InitOnce();
  testing::InitGoogleTest(&argc, argv);
  int exit_code = RUN_ALL_TESTS();
  CDE::CDETEST::Destroy();

  return exit_code;
}
