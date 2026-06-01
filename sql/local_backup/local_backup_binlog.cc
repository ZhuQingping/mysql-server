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

#include "local_backup_binlog.h"
#include "my_securec.h"      // memcpy_s
#include "sql/debug_sync.h"  // DEBUG_SYNC

#include <string>

#include "mysql/components/services/log_builtins.h"
#include "mysql/psi/mysql_file.h"
#include "sql/binlog.h"
#include "sql/local_backup/full_local_backup.h"
#include "sql/local_backup/local_backup_obs_handler.h"
#include "sql/local_backup/local_backup_utils.h"
#include "sql/mysqld.h"
#include "sql/sql_backup_lock.h"

#define LB_LOG_MSG(severity, message)                          \
  do {                                                         \
    std::stringstream ss;                                      \
    ss << message;                                             \
    LogErr(severity, ER_LOCAL_BACKUP_ERROR, ss.str().c_str()); \
  } while (0)

ulong rds_full_local_backup_lock_binlog_timeout = 300;
bool rds_full_local_backup_fetch_gtid = true;

/**
  Get the Binlog position

  @param[out] log_flle_name     the current active binlog file name
  @param[out] log_flle_pos      the current active binlog file pos
  @param[out] gtid_set_str      the current executed GTID set

  @return return false on success
*/
bool get_binlog_position(std::string &log_file_name, uint64_t &log_file_pos,
                         std::string &gtid_set_str) {
  char *gtid_set_buffer = nullptr;

  global_sid_lock->wrlock();
  const Gtid_set *gtid_set = gtid_state->get_executed_gtids();

  int gtid_set_size = gtid_set->to_string(&gtid_set_buffer);
  DBUG_EXECUTE_IF("full_lb_gtid_parse_failed", { gtid_set_size = -1; });
  if (gtid_set_size < 0) {
    global_sid_lock->unlock();
    my_free(gtid_set_buffer);
    return true;
  }
  global_sid_lock->unlock();
  gtid_set_str = gtid_set_buffer;
  my_free(gtid_set_buffer);

  LOG_INFO li;
  mysql_bin_log.get_current_log(&li, false /* need_lock_log */);
  log_file_name = std::string(li.log_file_name);
  log_file_pos = li.pos;

  return false;
}

/**
  Copy source file to destination file with a specified file size

  @param src_file_name      the source file name.
  @param dst_file_name      the target file name.
  @param file_size          the file size to copy.

  @retval 0    ok
  @retval -1    error
*/
static bool copy_file_fixed(const char *src_file_name,
                            const char *dst_file_name, my_off_t file_size) {
  bool ret = false;
  File src_file, dst_file;
  int bytes_read, bytes_written;
  uchar io_buf[IO_SIZE + 1];
  uint64_t copy_times = (file_size + IO_SIZE - 1) / IO_SIZE;
  uint64_t offset = 0;
  uint64_t to_read;

  src_file = mysql_file_open(key_file_binlog, src_file_name, O_RDONLY, MYF(0));
  DBUG_EXECUTE_IF("full_lb_open_old_binlog_fail", { src_file = -1; });
  if (src_file < 0) {
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to open source file "
                                << src_file_name << ", errno: " << my_errno());
    return true;
  }

  dst_file = mysql_file_open(key_file_binlog, dst_file_name, O_CREAT | O_WRONLY,
                             MYF(MY_WME));
  DBUG_EXECUTE_IF("full_lb_open_new_binlog_fail", { dst_file = -1; });
  if (dst_file < 0) {
    mysql_file_close(src_file, MYF(MY_WME));
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to open target file "
                                << dst_file_name << ", errno: " << my_errno());
    return true;
  }

  for (uint64_t i = 0; i < copy_times; i++) {
    to_read = std::min((uint64_t)file_size - offset, (uint64_t)IO_SIZE);

    /**
     * Here use MY_NABP flag :
     * a) this function would return MY_FILE_ERROR(-1)
     * if not all bytes read/written;
     * b) return 0 if succeed and all the bytes are read/written
     */
    bytes_read =
        mysql_file_read(src_file, io_buf, to_read, MYF(MY_WME | MY_NABP));
    DBUG_EXECUTE_IF("full_lb_binlog_read_fail", { bytes_read = -1; });
    if (bytes_read < 0) {
      LB_LOG_MSG(ERROR_LEVEL, "full backup failed to read source file "
                                  << src_file_name
                                  << ", errno: " << my_errno());
      ret = true;
      break;
    }

    bytes_written =
        mysql_file_write(dst_file, io_buf, to_read, MYF(MY_WME | MY_NABP));
    DBUG_EXECUTE_IF("full_lb_binlog_write_fail", { bytes_written = -1; });
    if (bytes_written < 0) {
      LB_LOG_MSG(ERROR_LEVEL, "full backup failed to write target file "
                                  << dst_file_name
                                  << ", errno: " << my_errno());
      ret = true;
      break;
    }
  }

  if (DBUG_EVALUATE_IF("full_lb_binlog_sync_fail", true, false) ||
      mysql_file_sync(dst_file, MYF(MY_WME)) ||
      mysql_file_close(dst_file, MYF(MY_WME)) ||
      mysql_file_close(src_file, MYF(MY_WME))) {
    ret = true;
  }

  return ret;
}

bool LocalBackup_binlog_mgr::try_acquire_binlog_lock(THD *thd) {
  if (!rds_binlog_lock_enable) {
    LB_LOG_MSG(ERROR_LEVEL, "binlog lock is not enabled");
    return true;
  }

  /*
    Local Backup should use the member function acquire() of backup_binlog_lock,
    which would add MDL_SHARED instead of MDL_INTENTION_EXCLUSIVE.
  */
  if (DBUG_EVALUATE_IF("full_lb_binlog_lock_already", true, false) ||
      thd->backup_binlog_lock.is_acquired()) {
    LocalBackup_binlog_mgr::release_binlog_lock(thd);
    LB_LOG_MSG(
        ERROR_LEVEL,
        "full backup failed, the backup binlog lock was acquired unexpected");
    return true;
  }

  /* change the THD's timeout variable temporary */
  const ulong old_timeout = thd->variables.lock_wait_timeout;
  thd->variables.lock_wait_timeout = rds_full_local_backup_lock_binlog_timeout;
  if (DBUG_EVALUATE_IF("full_lb_binlog_lock_failed", true, false) ||
      thd->backup_binlog_lock.acquire(thd)) {
    thd->variables.lock_wait_timeout = old_timeout;
    LB_LOG_MSG(ERROR_LEVEL,
               "full backup failed to acquire the backup binlog lock");
    return true;
  }

  thd->variables.lock_wait_timeout = old_timeout;
  LB_LOG_MSG(INFORMATION_LEVEL, "full backup acquired the backup binlog lock");
  m_binlogPrepared = true;
  return false;
}

bool LocalBackup_binlog_mgr::copy_last_binlog_and_index(
    const char *backup_data_path) {
  bool ret = false;

  size_t pos = m_binlogFileName.find_last_of(FN_LIBCHAR);

  std::ostringstream oss;
  oss << backup_data_path << FN_LIBCHAR;

  std::string backup_binlog_path(oss.str());
  std::string backup_binlog_file(backup_binlog_path);
  backup_binlog_file.append(m_binlogFileName.substr(pos + 1));
  if (!current_lb_use_obs &&
      !(ret = copy_file_fixed(m_binlogFileName.c_str(),
                              backup_binlog_file.c_str(), m_binlogFilePos))) {
    LB_LOG_MSG(SYSTEM_LEVEL, "full backup copy binlog file done, file size is "
                                 << m_binlogFilePos << ", from "
                                 << m_binlogFileName << " to "
                                 << backup_binlog_file);
  }

  char *tmp_log_bin_index = mysql_bin_log.get_index_fname();
  DBUG_EXECUTE_IF("full_lb_get_binlog_index_fail",
                  { tmp_log_bin_index = nullptr; });
  if (nullptr == tmp_log_bin_index) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "full backup found binlog index filename is null");
    return true;
  }
  char *index_pos = strrchr(tmp_log_bin_index, FN_LIBCHAR);

  std::string binlog_index_filename(backup_binlog_path);
  binlog_index_filename.append(index_pos + 1);

  if (current_lb_use_obs) {
    return upload_last_binlog_and_index_to_obs(m_binlogFileName.substr(pos + 1),
                                               std::string(index_pos + 1),
                                               backup_binlog_path);
  }

  File binlog_index_file =
      mysql_file_open(key_file_binlog_index, binlog_index_filename.c_str(),
                      O_CREAT | O_WRONLY, MYF(MY_WME));
  DBUG_EXECUTE_IF("full_lb_open_binlog_index_fail",
                  { binlog_index_file = -1; });
  if (binlog_index_file < 0) {
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to open target index file "
                                << binlog_index_filename
                                << ", errno: " << my_errno());
    return true;
  }

  /* only write current log file into .index in the backup directory */
  uchar line[FN_REFLEN];
  my_memcpy(line, FN_REFLEN, m_binlogFileName.data(), m_binlogFileName.size());
  line[m_binlogFileName.size()] = '\n';

  if (DBUG_EVALUATE_IF("full_lb_write_binlog_index_fail", true, false) ||
      mysql_file_write(binlog_index_file, line, m_binlogFileName.size() + 1,
                       MYF(MY_WME | MY_NABP))) {
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to write index file "
                                << binlog_index_filename
                                << ", errno: " << my_errno());
    ret = true;
  }
  if (binlog_index_file > 0) {
    mysql_file_close(binlog_index_file, MYF(MY_WME));
  }

  m_binlogFileName = m_binlogFileName.substr(pos + 1);
  return ret;
}

bool LocalBackup_binlog_mgr::is_binlog_open() {
  return (opt_bin_log && mysql_bin_log.is_open());
}

bool LocalBackup_binlog_mgr::get_consistent_binlog_point(
    const char *backup_data_path) {
  bool ret = false;
  THD *thd = current_thd;

  if (DBUG_EVALUATE_IF("full_lb_binlog_closed", true, false) ||
      !is_binlog_open()) {
    LB_LOG_MSG(WARNING_LEVEL, "full backup found binlog is not enabled");
    return false;
  }

  /* ControlFile callback would be called twice */
  if (m_binlogPrepared) {
    return false;
  }
  assert(nullptr != backup_data_path);

  /* First acquire backup binlog lock. */
  if ((ret = try_acquire_binlog_lock(thd))) {
    return true;
  }

#ifndef NDEBUG
  DBUG_EXECUTE_IF("full_lb_wait_binlog_lock", {
    const char act[] =
        "now signal signal.full_lb_binlog_lock_acquired"
        " wait_for signal.full_lb_test_end";
    assert(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
  });
#endif

  /* try to rotate binlog. */
  if (DBUG_EVALUATE_IF("full_lb_binlog_rotate_failed", true, false) ||
      mysql_bin_log.rotate(true /* force_rotate */)) {
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to rotate binlog");
  } else if ((ret = get_binlog_position(m_binlogFileName, m_binlogFilePos,
                                        m_gtidSet))) {
    /* Fetch the binlog point */
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to get binlog position");
  } else if ((ret = copy_last_binlog_and_index(backup_data_path))) {
    /* Copy the active binlog file and index file */
    LB_LOG_MSG(ERROR_LEVEL, "full backup failed to copy binlog file");
  }

  /* release lock if failed */
  if (ret) {
    release_resource();
  }

  return ret;
}

void LocalBackup_binlog_mgr::release_binlog_lock(THD *thd) {
  if (nullptr != thd && thd->backup_binlog_lock.is_acquired()) {
    thd->backup_binlog_lock.release(thd);
    LB_LOG_MSG(INFORMATION_LEVEL, "full backup release binlog lock");
  }
}

bool LocalBackup_binlog_mgr::release_resource() {
  THD *thd = current_thd;
  if (rds_binlog_lock_enable && m_binlogPrepared) {
    LocalBackup_binlog_mgr::release_binlog_lock(thd);
  }
  m_binlogPrepared = false;
  return false;
}

bool LocalBackup_binlog_mgr::upload_last_binlog_and_index_to_obs(
    const std::string &binlog_filename, const std::string &index_filename,
    const std::string &backup_dir) {
  std::vector<BakFileItem> merge_files;
  merge_files.emplace_back(m_binlogFileName, binlog_filename, m_binlogFilePos);
  merge_files.emplace_back(index_filename, index_filename,
                           m_binlogFileName.size() + 1);

  lb_object_handler *handler = fetch_lb_object_handler();
  // LCOV_EXCL_START
  if (nullptr == handler) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to fetch lb object handler for local backup with obs mode");
    return true;
  }
  // LCOV_EXCL_STOP
  std::string obj_name(backup_dir);
  obj_name.append(full_lb_binlog_obs_obj_name);
  handler->make_object_name(obj_name, OBS_OBJ_TYPE_MERGED_FILES, 0, 0);
  increase_current_lb_object_count();

  bool error = gen_merged_file_obj_meta(merge_files, handler);
#ifndef NDEBUG
  bool my_thread_inited = my_thread_is_inited();
  if (!my_thread_inited) {
    my_thread_init();  // For using DBUG_ to test
  }
#endif
  DBUG_EXECUTE_IF(
      "upload_last_binlog_and_index_to_obs_gen_merged_file_obj_meta_fail",
      { error = true; });
  if (error) {
    come_back_lb_object_handler(handler);
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to generate binlog files meta data for OBS object");
#ifndef NDEBUG
    if (!my_thread_inited) {
      my_thread_end();
    }
#endif
    return true;
  }

  error = append_one_file_to_obj(m_binlogFileName, m_binlogFilePos, handler);
  if (!error) {
    error = handler->append_object();
  }
  DBUG_EXECUTE_IF(
      "upload_last_binlog_and_index_to_obs_append_one_file_to_obj_fail",
      { error = true; });
  if (error) {
    come_back_lb_object_handler(handler);
    std::stringstream ss_err;
    ss_err << "failed to append binlog file " << m_binlogFileName
           << " to OBS object ";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
#ifndef NDEBUG
    if (!my_thread_inited) {
      my_thread_end();
    }
#endif
    return true;
  }

  /* only write current log file into .index in the backup directory */
  char line[FN_REFLEN];
  my_memcpy(line, FN_REFLEN, m_binlogFileName.data(), m_binlogFileName.size());
  line[m_binlogFileName.size()] = '\n';

  error = append_data_to_obj(line, m_binlogFileName.size() + 1, handler);
  DBUG_EXECUTE_IF("upload_last_binlog_and_index_to_obs_append_data_to_obj_fail",
                  { error = true; });
  if (error) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to append binlog index to OBS object");
  }
  come_back_lb_object_handler(handler);
#ifndef NDEBUG
  if (!my_thread_inited) {
    my_thread_end();
  }
#endif
  size_t pos = m_binlogFileName.find_last_of(FN_LIBCHAR);
  m_binlogFileName = m_binlogFileName.substr(pos + 1);
  return error;
}
