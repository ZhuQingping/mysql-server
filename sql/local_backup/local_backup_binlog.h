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

#ifndef SQL_LOCAL_BACKUP_BINLOG_INCLUDED
#define SQL_LOCAL_BACKUP_BINLOG_INCLUDED

#include <string>

#include "my_inttypes.h"
#include "sql/sql_class.h"

extern ulong rds_full_local_backup_lock_binlog_timeout;
extern bool rds_full_local_backup_fetch_gtid;

/**
 * The binlog consisitent point logic is SQL layer
 */
class LocalBackup_binlog_mgr {
 public:
  LocalBackup_binlog_mgr() : m_binlogPrepared(false), m_binlogFilePos(0) {}
  ~LocalBackup_binlog_mgr() {}

  /**
    Get the Consistent Binlog Point logic.
    1) After the ControlFile is backuped during a full backup, this
    function would be called.
    It would acquire the binlog backup lock and get the Binlog point.

    @return false if success
    @return true if fail
  */
  bool get_consistent_binlog_point(const char *backup_data_path);

  /**
    After the WAL copy is done, this function would be called.
    It would release the binlog backup lock.

    @return false if success
    @return true if fail
  */
  bool release_resource();

  bool is_binlog_open();

  std::string get_binlog_file_name() { return m_binlogFileName; }
  uint64_t get_binlog_file_pos() { return m_binlogFilePos; }
  std::string get_gtid_set() { return m_gtidSet; }

  static void release_binlog_lock(THD *thd);

 private:
  bool try_acquire_binlog_lock(THD *thd);
  bool copy_last_binlog_and_index(const char *backup_data_path);
  bool upload_last_binlog_and_index_to_obs(const std::string &binlog_filename,
                                           const std::string &index_filename,
                                           const std::string &backup_dir);

  bool m_binlogPrepared = false;
  std::string m_binlogFileName;
  uint64_t m_binlogFilePos;
  std::string m_gtidSet;
};

#endif  // SQL_LOCAL_BACKUP_BINLOG_INCLUDED
