/* Copyright (c) 2025, Huawei and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/local_backup/log_archive.h"

#include <string>
#include "sql/auto_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/events.h"
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_backup_lock.h"
#include "sql/sql_class.h"

#include "mysql/components/services/log_builtins.h"

char rds_log_archive_path[FN_REFLEN_SE];
char *rds_log_archive_path_ptr = rds_log_archive_path;

class Log_archiver {
 public:
  Log_archiver();
  ~Log_archiver() {}

  bool start_wal_archive(THD *thd);

  bool stop_wal_archive(THD *thd);

  bool is_running() { return m_isRunning.load(); }

 private:
  std::mutex m_cmdMutex;
  std::atomic<bool> m_isRunning;
  local_backup_config m_backup_cfg;

  Log_archiver(const Log_archiver &) = delete;
  Log_archiver &operator=(const Log_archiver &) = delete;
};

Log_archiver g_log_archiver;

Log_archiver::Log_archiver() : m_isRunning(false) {}

bool Log_archiver::start_wal_archive(THD *thd) {
  std::unique_lock<std::mutex> lock(m_cmdMutex);
  if (m_isRunning) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, "wal archiver is running");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "wal archiver is already running");
    return false;
  }

  if (rds_log_archive_path_ptr[0] == '\0') {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "log archive path not configured");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "log archive path should be configured");
    return false;
  }

  if (access(rds_log_archive_path_ptr, F_OK | R_OK | W_OK | X_OK) != 0) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "cannot access log archive directory");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "cannot access log archive directory");
    return false;
  }

  m_isRunning.store(true);
  LogErr(SYSTEM_LEVEL, ER_LOCAL_BACKUP_ERROR, "start wal archiver");

  m_backup_cfg.data_base_path = rds_log_archive_path_ptr;

  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);
  if (cde_hton->start_wal_archive(&m_backup_cfg)) {
    m_isRunning.store(false);
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, "failed to start wal archiver");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0), "failed to start wal archiver");
    return false;
  }

  LogErr(SYSTEM_LEVEL, ER_LOCAL_BACKUP_ERROR, "wal archiver is started");
  return true;
}

bool Log_archiver::stop_wal_archive(THD *thd) {
  std::unique_lock<std::mutex> lock(m_cmdMutex);
  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);
  if (cde_hton->stop_wal_archive()) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, "wal archiver stop failed");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0), "wal archiver stop failed");
    return false;
  }

  m_isRunning.store(false);
  return true;
}

bool exec_start_log_archive_cmd(THD *thd) {
  return !g_log_archiver.start_wal_archive(thd);
}

bool exec_stop_log_archive_cmd(THD *thd) {
  return !g_log_archiver.stop_wal_archive(thd);
}
