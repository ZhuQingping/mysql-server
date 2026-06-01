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

#include "sql/local_backup/full_local_backup.h"
#include <string>

#include "mysql/components/services/log_builtins.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/status_var.h"
#include "sql/auto_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/events.h"
#include "sql/handler.h"
#include "sql/local_backup/local_backup_binlog.h"
#include "sql/local_backup/local_backup_obs_handler.h"
#include "sql/local_backup/local_backup_utils.h"
#include "sql/mysqld.h"
#include "sql/sql_backup_lock.h"
#include "sql/sql_class.h"
#include "sql/sql_show.h"

static char rds_full_local_backup_path[FN_REFLEN_SE];
char *rds_full_local_backup_path_ptr = rds_full_local_backup_path;
ulong rds_full_local_backup_lock_timeout = 300;
bool rds_full_local_backup_lock_optimize = true;

static std::mutex full_lb_mutex;
static bool full_lb_running = false;
static my_thread_handle full_lb_th;
static my_thread_attr_t full_lb_thd_attr;

constexpr const char *ongoing_backup_dir_prefix = "full_backup_";
constexpr const char *empty_dir = ".empty_dir";

static PSI_thread_key key_thread_full_local_backup;
static PSI_thread_info full_local_backup_threads[] = {
    {&key_thread_full_local_backup, "full_local_backup", "full_lb",
     PSI_FLAG_SINGLETON, 0, PSI_DOCUMENT_ME}};

/** No full backup is requested since mysqld process start. */
constexpr const char *full_lb_status_none = "None";
constexpr const char *full_lb_status_sucess = "Success";
constexpr const char *full_lb_status_fail = "Failed";
static const char *full_lb_last_status = full_lb_status_none;

static time_t full_lb_last_succ_time_point = 0;
static std::uint64_t full_lb_last_succ_lsn = 0;

int show_full_lb_running_status(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_BOOL;
  var->value = buff;
  *(pointer_cast<bool *>(buff)) = full_lb_running;
  return 0;
}

int show_full_lb_last_result_status(THD *, SHOW_VAR *var, char *) {
  var->type = SHOW_CHAR;
  var->value = const_cast<char *>(full_lb_last_status);
  return 0;
}

int show_full_lb_last_success_time_status(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *(pointer_cast<long long *>(buff)) = full_lb_last_succ_time_point;
  return 0;
}

int show_full_lb_last_success_lsn_status(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *(pointer_cast<long long *>(buff)) = full_lb_last_succ_lsn;
  return 0;
}

static SHOW_VAR full_local_backup_status_vars[] = {
    {"Rds_local_backup_full_backup_is_running",
     (char *)&show_full_lb_running_status, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Rds_local_backup_last_full_backup_status",
     (char *)&show_full_lb_last_result_status, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Rds_local_backup_last_success_full_backup_time",
     (char *)&show_full_lb_last_success_time_status, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Rds_local_backup_last_success_full_backup_lsn",
     (char *)&show_full_lb_last_success_lsn_status, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_FUNC, SHOW_SCOPE_GLOBAL}};

void init_full_local_backup() {
  const char *category = "local_backup";
  int count = static_cast<int>(array_elements(full_local_backup_threads));
  mysql_thread_register(category, full_local_backup_threads, count);

  add_status_vars(full_local_backup_status_vars);
}

static bool get_schema_file_meta(std::vector<BakFileItem> &files,
                                 const char *src, const char *dst) {
  std::stringstream ss_err;
  MY_DIR *src_dir = my_dir(src, MYF(MY_DONT_SORT | MY_WANT_STAT));
  if (src_dir == nullptr) {
    ss_err << "failed to open soure directory " << src << ", errno: " << errno;
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
    return true;
  }

  bool error = false;
  for (uint i = 0; i < src_dir->number_off_files; i++) {
    if (src_dir->dir_entry[i].name[0] == '.') {
      continue;
    }

    std::string sub_src(src);
    sub_src.append("/").append(src_dir->dir_entry[i].name);
    std::string sub_dst(dst);
    sub_dst.append("/").append(src_dir->dir_entry[i].name);

    if (MY_S_ISDIR(src_dir->dir_entry[i].mystat->st_mode) ||
        DBUG_EVALUATE_IF("get_schema_file_meta_cp_sub_dir_fail", true, false)) {
      error = get_schema_file_meta(files, sub_src.c_str(), sub_dst.c_str());
      if (error) {
        ss_err << "failed to get schema files for sub directory " << sub_src;
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
        break;
      }
    } else {
      std::string ibd_suffix(".ibd");
      if (sub_dst.size() > ibd_suffix.size() &&
          sub_dst.compare(sub_dst.size() - ibd_suffix.size(),
                          ibd_suffix.length(), ibd_suffix) == 0) {
        // .ibd files already backup by InnoDB engine
        continue;
      }
      off_t file_size = get_file_size_for_backup(sub_src);
      DBUG_EXECUTE_IF("get_schema_file_meta_get_file_size_fail",
                      { file_size = -1; });
      if (file_size == -1) {
        error = true;
        ss_err << "get file size fail for " << sub_src;
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
        break;
      }
      files.emplace_back(sub_src, sub_dst, file_size);
    }
  }

  /* Backup empty directory. */
  if (src_dir->number_off_files == 2) {
    std::string empty_src(src);
    empty_src.append("/").append(empty_dir);
    std::string empty_dst(dst);
    empty_dst.append("/").append(empty_dir);
    files.emplace_back(empty_src, empty_dst, 0);
  }

  my_dirend(src_dir);
  return error;
}

static bool merge_schema_dirs_to_obs_obj(std::vector<dd::String_type> &schemas,
                                         std::string &dst_dir) {
  bool error = false;
  std::vector<BakFileItem> merge_schema_files;

  for (auto schema : schemas) {
    std::string src_dir(mysql_real_data_home);
    src_dir.append("/").append(schema);
    MY_STAT stat;
    std::stringstream ss_err;
    if (mysql_file_stat(key_file_misc, src_dir.c_str(), &stat, MYF(0)) ==
        nullptr) {
      /* Such as information_schema. */
      ss_err << "schema directory " << src_dir << " not exist";
      LogErr(INFORMATION_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
      continue;
    }

    std::string dest_dir(schema);
    error = get_schema_file_meta(merge_schema_files, src_dir.c_str(),
                                 dest_dir.c_str());
    if (error) {
      ss_err << "failed to get schema file informations for directory "
             << src_dir;
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
      return true;
    }
  }

  lb_object_handler *handler = fetch_lb_object_handler();
  // LCOV_EXCL_START
  if (nullptr == handler) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to fetch lb object handler for local backup with obs mode");
    return true;
  }
  // LCOV_EXCL_STOP
  std::string obj_name(dst_dir);
  obj_name.append("/");
  obj_name.append(full_lb_schema_obs_obj_name);
  handler->make_object_name(obj_name, OBS_OBJ_TYPE_MERGED_FILES, 0, 0);
  increase_current_lb_object_count();

  if (gen_merged_file_obj_meta(merge_schema_files, handler) ||
      DBUG_EVALUATE_IF(
          "merge_schema_dirs_to_obs_obj_gen_merged_file_obj_meta_fail", true,
          false)) {
    come_back_lb_object_handler(handler);
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to generate schema files meta data for OBS object");
    return true;
  }

  // Write each file into object
  for (auto file : merge_schema_files) {
    if (append_one_file_to_obj(file.file_name_orig, file.file_size, handler) ||
        DBUG_EVALUATE_IF(
            "merge_schema_dirs_to_obs_obj_append_one_file_to_obj_fail", true,
            false)) {
      come_back_lb_object_handler(handler);
      std::stringstream ss_err;
      ss_err << "failed to append schema file " << file.file_name_orig
             << " to OBS object ";
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
      return true;
    }
  }
  error = handler->append_object();
  if (error) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to append schema file to OBS object");
  }
  come_back_lb_object_handler(handler);
  return error;
}

static bool full_lb_copy_dir(const char *src, const char *dst) {
  std::stringstream ss_err;
  MY_DIR *src_dir = my_dir(src, MYF(MY_DONT_SORT | MY_WANT_STAT));
  DBUG_EXECUTE_IF("full_lb_cp_one_dir_open_fail", {
    my_dirend(src_dir);
    src_dir = nullptr;
  });
  if (src_dir == nullptr) {
    ss_err << "failed to open soure directory " << src << ", errno: " << errno;
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
    return true;
  }

  MY_STAT stat;
  if (DBUG_EVALUATE_IF("full_lb_cp_one_dir_stat_fail", true, false) ||
      mysql_file_stat(key_file_misc, dst, &stat, MYF(0)) == nullptr) {
    if (DBUG_EVALUATE_IF("full_lb_cp_one_dir_mkdir_fail", true, false) ||
        my_mkdir(dst, my_umask_dir, MYF(0)) != 0) {
      ss_err << "failed to create destination directory " << dst
             << ", errno: " << errno;
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
      my_dirend(src_dir);
      return true;
    }
  }

  bool error = false;
  for (uint i = 0; i < src_dir->number_off_files; i++) {
    if (src_dir->dir_entry[i].name[0] == '.') {
      continue;
    }

    std::string sub_src(src);
    sub_src.append("/").append(src_dir->dir_entry[i].name);
    std::string sub_dst(dst);
    sub_dst.append("/").append(src_dir->dir_entry[i].name);

    if (DBUG_EVALUATE_IF("full_lb_cp_one_dir_cp_sub_dir_fail", true, false) ||
        MY_S_ISDIR(src_dir->dir_entry[i].mystat->st_mode)) {
      error = full_lb_copy_dir(sub_src.c_str(), sub_dst.c_str());
      if (error) {
        ss_err << "failed to copy sub directory " << sub_src;
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
        break;
      }
    } else {
      if (mysql_file_stat(key_file_misc, sub_dst.c_str(), &stat, MYF(0)) ==
          nullptr) {
        if (DBUG_EVALUATE_IF("full_lb_cp_one_dir_copy_fail", true, false) ||
            my_copy(sub_src.c_str(), sub_dst.c_str(),
                    MYF(MY_HOLD_ORIGINAL_MODES)) != 0) {
          error = true;
          ss_err << "failed to copy file " << sub_src << ", errno: " << errno;
          LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
          break;
        }
      }
    }
  }

  my_dirend(src_dir);
  return error;
}

static bool copy_schema_dirs(THD *thd, std::string &dst_dir) {
  std::vector<dd::String_type> schemas;
  bool error =
      thd->dd_client()->fetch_global_component_names<dd::Schema>(&schemas);
  DBUG_EXECUTE_IF("full_lb_fetch_schema_fail", error = true;);
  if (error) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to fetch schemas from DD");
    return true;
  }

  // Merge schema files into one object
  if (current_lb_use_obs) {
    return merge_schema_dirs_to_obs_obj(schemas, dst_dir);
  }

  for (auto schema : schemas) {
    std::string src_dir(mysql_real_data_home);
    src_dir.append("/").append(schema);
    MY_STAT stat;
    std::stringstream ss_err;
    if (mysql_file_stat(key_file_misc, src_dir.c_str(), &stat, MYF(0)) ==
        nullptr) {
      /* Such as information_schema. */
      ss_err << "schema directory " << src_dir << " not exist";
      LogErr(INFORMATION_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
      continue;
    }

    std::string dest_dir(dst_dir);
    dest_dir.append("/").append(schema);
    error = full_lb_copy_dir(src_dir.c_str(), dest_dir.c_str());
    DBUG_EXECUTE_IF("full_lb_cp_schema_dir_fail", error = true;);
    if (error) {
      ss_err << "failed to copy schema directory " << src_dir;
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err.str().c_str());
      break;
    }
  }
  return error;
}

bool prepare_for_full_backup(THD *thd) {
  /* When execute event, DD table 'events' will be updated.
  Stop event scheduler. Will do nothing if event scheduler is disabled. */
  Events::stop();

#ifndef NDEBUG
  while (DBUG_EVALUATE_IF("full_lb_thd_wait_before_get_mdl", true, false)) {
    my_sleep(100000);  // 100ms
  }
#endif

  bool error = (DBUG_EVALUATE_IF("full_lb_acquire_mdl_fail", true, false) ||
                acquire_exclusive_backup_lock(
                    thd, rds_full_local_backup_lock_timeout, false));
  if (error) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, "failed to get backup MDL");
    return true;
  }

#ifndef NDEBUG
  while (DBUG_EVALUATE_IF("full_lb_thd_wait_after_get_mdl", true, false)) {
    my_sleep(100000);  // 100ms
  }
#endif
  return false;
}

struct full_local_backup_thread_arg {
  bool involve_log_arch;
  bool involve_replica;
};

static full_local_backup_thread_arg full_lb_thd_arg;

extern "C" void *full_local_backup_main(void *arg) {
  bool involve_log_arch =
      ((full_local_backup_thread_arg *)arg)->involve_log_arch;
  bool involve_replica = ((full_local_backup_thread_arg *)arg)->involve_replica;

  local_backup_config cde_bak_cfg;
  local_backup_config innodb_bak_cfg;

  std::stringstream ss_err_msg;
  std::stringstream ss_dir;
  ss_dir << rds_full_local_backup_path_ptr << "/" << ongoing_backup_dir_prefix
         << my_micro_time();
  cde_bak_cfg.data_base_path = ss_dir.str();
  innodb_bak_cfg.data_base_path = ss_dir.str();

  my_thread_init();

  Auto_THD auto_thd;
  THD *thd = current_thd;
  bool error = false;
  int err_no = 0;

  local_backup_point bak_point;
  bak_point.time_point = 0;
  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);

  LogErr(SYSTEM_LEVEL, ER_LOCAL_BACKUP_ERROR, "start full backup");

  /* initialize obs. */
  if (rds_local_backup_use_obs) {
    std::string uri_style = rds_local_backup_obs_uri_style == 1 ? "1" : "0";
    std::string scc_decrypt = rds_lb_obs_enable_scc_decrypt ? "true" : "false";
    std::string encryption = rds_lb_obs_encryption ? "1" : "0";
    std::string kms_key_id =
        (rds_lb_obs_kms_key_id == nullptr) ? "" : rds_lb_obs_kms_key_id;
    std::map<std::string, std::string> obs_para = {
        {"obs_url", rds_local_backup_obs_host},
        {"AK", rds_local_backup_obs_ak},
        {"SK", rds_local_backup_obs_sk},
        {"bucket_name", rds_local_backup_obs_bucket},
        {"uri_style", uri_style},
        {"scc_decrypt", scc_decrypt},
        {"encryption", encryption},
        {"kms_key", kms_key_id}};
    if (create_lb_object_handler_instance(true, rds_lb_obs_buffer_size,
                                          obs_para)) {
      error = true;
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
             "Failed to create OBS service handler instance");
      goto exit;
    }
    current_lb_use_obs = true;
    reset_current_lb_object_count();
  }

  if (!current_lb_use_obs &&
      (DBUG_EVALUATE_IF("full_lb_create_dir_fail", true, false) ||
       my_mkdir(ss_dir.str().c_str(), my_umask_dir, MYF(0)) != 0)) {
    error = true;
    ss_err_msg << "failed to mkdir for full backup";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err_msg.str().c_str());
    goto exit;
  }
  cde_bak_cfg.root_path = ss_dir.str();

  if (!rds_full_local_backup_lock_optimize && prepare_for_full_backup(thd)) {
    error = true;
    goto exit;
  }

  error = cde_hton->start_full_local_backup(&cde_bak_cfg, &bak_point,
                                            involve_log_arch, involve_replica);
  if (error) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to do full backup for dstore");
    goto exit;
  }

  error = innodb_hton->start_full_local_backup(&innodb_bak_cfg, nullptr, false,
                                               false);
  if (error) {
    ss_err_msg << "failed to do full backup for innodb";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err_msg.str().c_str());
    goto exit;
  }

  error = copy_schema_dirs(thd, innodb_bak_cfg.data_base_path);
  if (error) {
    ss_err_msg << "failed to copy schema directory";
  }

exit:
  /* Finish the binlog backup logic, release the backup binlog lock */
  if (rds_full_local_backup_fetch_gtid &&
      cde_hton->finish_full_backup_binlog(bak_point)) {
    error = true;
    ss_err_msg << "failed to finish backup binlog logic";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err_msg.str().c_str());
  }
  release_backup_lock(thd);

  if (!error) {
    error = cde_hton->write_full_backup_meta_info(bak_point,
                                                  cde_bak_cfg.meta_base_path);
    if (error) {
      ss_err_msg << "failed to write full backup meta info";
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err_msg.str().c_str());
    } else if (!current_lb_use_obs) {
      char tm_buf[21];  // Format: 2025Y04M16D02H18M48S
      struct tm bak_tm;
      localtime_r(&bak_point.time_point, &bak_tm);

      /* Same format as LocalBackupFileMgr::ConcatenateTimeSuffix(). */
      strftime(tm_buf, sizeof(tm_buf), "%YY%mM%dD%HH%MM%SS", &bak_tm);
      std::string dst_dir_name(rds_full_local_backup_path_ptr);
      dst_dir_name.append("/").append(tm_buf);
      if (my_rename(ss_dir.str().c_str(), dst_dir_name.c_str(), MYF(0)) != 0) {
        error = true;
        ss_err_msg << "failed to rename full backup direcory";
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err_msg.str().c_str());
      } else {
        cde_bak_cfg.root_path = dst_dir_name;
      }
    }

    /* If using OBS, generates restore meta file and upload to OBS after dstore
    backup success. Then no need to do this outside kernel by using another
    tool. */
    if (!error && current_lb_use_obs &&
        DBUG_EVALUATE_IF("full_lb_obs_skip_write_restore_meta", false, true)) {
      if (DBUG_EVALUATE_IF("full_lb_obs_write_restore_meta_fail", true,
                           false) ||
          cde_hton->write_full_backup_restore_meta(
              bak_point, cde_bak_cfg.meta_base_path, cde_bak_cfg.root_path)) {
        error = true;
        ss_err_msg << "failed to write full backup restore meta to OBS";
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss_err_msg.str().c_str());
      }
      increase_current_lb_object_count();
    }
  }

  /* Resume event scheduler. Will do nothing if event scheduler is disabled. */
  if (Events::opt_event_scheduler == Events::EVENTS_ON &&
      Events::start(&err_no)) {
    std::stringstream ss;
    ss << "failed to resume event scheduler(error " << err_no << ")";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
  }

  /*
    Write non-dstore part status and error message, backup point info and
    binlog info to full local backup meta json file.
  */
  std::string err_msg = ss_err_msg.str();
  if (cde_hton->write_full_backup_meta_json(&err_msg, &bak_point,
                                            cde_bak_cfg.root_path)) {
    error = true;
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to write full backup meta json");
  }
  if (current_lb_use_obs) {
    increase_current_lb_object_count();
    std::stringstream ss;
    ss << "current backup uses " << get_current_lb_object_count()
       << " OBS objects in total";
    LogErr(SYSTEM_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
  }

  if (!error) {
    full_lb_last_status = full_lb_status_sucess;
    full_lb_last_succ_time_point = bak_point.time_point;
    full_lb_last_succ_lsn = bak_point.lsn_point;
    LogErr(SYSTEM_LEVEL, ER_LOCAL_BACKUP_ERROR, "full backup complete");
  } else {
    full_lb_last_status = full_lb_status_fail;
  }

#ifndef NDEBUG
  while (DBUG_EVALUATE_IF("full_lb_thd_wait_before_exit", true, false)) {
    my_sleep(100000);  // 100ms
  }
#endif
  full_lb_running = false;
  /* Destroy obs. */
  destroy_lb_object_handler_instance();
  current_lb_use_obs = false;
  my_thread_end();
  my_thread_exit(nullptr);
  return nullptr;
}

constexpr longlong LB_FLAG_INVOLVE_LOG_ARCH = 1;
constexpr longlong LB_FLAG_INVOLVE_REPLICA = 1 << 1;

bool exec_start_full_lb_cmd(THD *, longlong flag) {
  std::unique_lock<std::mutex> lock(full_lb_mutex);
  if (full_lb_running) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "previous full backup is running");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "previous full backup is running");
    return true;
  }

  if (rds_full_local_backup_path_ptr[0] == '\0') {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "full backup path not configured");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "full backup path not configured");
    return true;
  }

  if (!rds_local_backup_use_obs &&
      access(rds_full_local_backup_path_ptr, F_OK | R_OK | W_OK | X_OK) != 0) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "cannot access full backup directory");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "cannot access full backup directory");
    return true;
  }

  if (full_lb_th.thread != 0) {
    my_thread_join(&full_lb_th, nullptr);
    full_lb_th.thread = 0;
  }

  if (DBUG_EVALUATE_IF("full_lb_binlog_check_fail", true, false) ||
      (opt_bin_log && !rds_binlog_lock_enable)) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "full backup would need rds_binlog_lock_enable for binlog");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "full backup would need rds_binlog_lock_enable for binlog");
    return true;
  }

  my_thread_attr_init(&full_lb_thd_attr);
  full_lb_thd_arg.involve_log_arch = flag & LB_FLAG_INVOLVE_LOG_ARCH;
  full_lb_thd_arg.involve_replica = flag & LB_FLAG_INVOLVE_REPLICA;
  full_lb_running = true;
  if (DBUG_EVALUATE_IF("full_lb_create_thread_fail", true, false) ||
      mysql_thread_create(key_thread_full_local_backup, &full_lb_th,
                          &full_lb_thd_attr, full_local_backup_main,
                          (void *)&full_lb_thd_arg)) {
    full_lb_running = false;
    full_lb_th.thread = 0;
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "failed to create full backup thread");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "failed to create full backup thread");
    return true;
  }

  return false;
}

bool exec_stop_full_lb_cmd(THD *thd) {
  std::unique_lock<std::mutex> lock(full_lb_mutex);
  if (!full_lb_running) {
    return false;
  }

  handlerton *cde_hton = ha_resolve_by_legacy_type(thd, DB_TYPE_DSTORE);
  if (DBUG_EVALUATE_IF("full_lb_stop_dstore_fail", true, false) ||
      cde_hton->stop_full_local_backup()) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "stop dstore full backup failed");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "stop dstore full backup failed");
    return true;
  }

  if (DBUG_EVALUATE_IF("full_lb_stop_innodb_fail", true, false) ||
      innodb_hton->stop_full_local_backup()) {
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "stop innodb full backup failed");
    my_error(ER_DA_LOCAL_BACKUP_ERROR, MYF(0),
             "stop innodb full backup failed");
    return true;
  }

  if (full_lb_th.thread != 0) {
    my_thread_join(&full_lb_th, nullptr);
    full_lb_th.thread = 0;
  }
  full_lb_running = false;
  return false;
}

std::atomic<uint> current_lb_object_count(0);
constexpr uint max_lb_object_num = 1000;

void increase_current_lb_object_count(uint increase_count) {
  uint count = current_lb_object_count.fetch_add(increase_count,
                                                 std::memory_order_relaxed);
  if (count > max_lb_object_num) {
    std::stringstream ss;
    ss << "current backup uses too many OBS objects(" << count << ")";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
  }
}

uint get_current_lb_object_count() {
  return current_lb_object_count.load(std::memory_order_relaxed);
}

void reset_current_lb_object_count(uint count) {
  current_lb_object_count.store(count, std::memory_order_relaxed);
}

bool get_full_lb_running() { return full_lb_running; }