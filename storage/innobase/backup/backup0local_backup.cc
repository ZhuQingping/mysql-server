/*****************************************************************************

Copyright (c) 2025, Huawei Technologies Co. and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file backup/backup0local_backup.cc
 Implementation for local backup system for InnoDB data, redo and undo file

 *******************************************************/

#include "backup0local_backup.h"

#include "fil0fil.h"
#include "ibuf0ibuf.h"
#include "log0chkp.h"
#include "log0sys.h"
#include "log0write.h"
#include "sql/events.h"
#include "sql/local_backup/full_local_backup.h"
#include "sql/local_backup/local_backup_utils.h"
#include "trx0purge.h"

bool innodb_lb_use_io_thread = false;
uint32_t innodb_lb_global_io_retry_number = 0;
uint32_t innodb_lb_local_io_retry_times = 0;
int32_t innodb_lb_local_io_hang_timeout_ms = 3600000;      // 3600s
int32_t innodb_lb_local_io_thread_quit_timeout_ms = 1000;  // 1s

static LB_Io_Thrd_Context_Mgr g_lb_io_thrd_context_mgr;
static thread_local LB_Io_Thrd_Context_Pair g_lb_io_thrd_context_pair = {
    UINT64_MAX, nullptr};

void do_lb_io_operation(LB_Io_Parameter *io_parameter, LB_Io_Result *result) {
  uint32_t times = 0;
  dberr_t ret = DB_ERROR;

  ut_ad(nullptr != io_parameter);
  ut_ad(nullptr != result);

  do {
    ret =
        io_parameter->m_callback->backup_cbk(&io_parameter->m_backup_data_desc);

    if (ret == DB_SUCCESS || errno != EIO ||
        (g_lb_io_thrd_context_mgr.m_lb_io_error_retry_number >
         innodb_lb_global_io_retry_number)) {
      break;
    }
    times++;
  } while (times < innodb_lb_local_io_retry_times);
  if (times > 0) {
    g_lb_io_thrd_context_mgr.m_lb_io_error_retry_number++;
  }
  result->ret = ret;
  if (ret != DB_SUCCESS) {
    result->io_errno = (int)errno;
  }
}

void LB_Io_Thrd_Context::io_main() {
  LB_Io_Parameter *io_parameter = nullptr;
  m_running.store(true);
  while (m_running.load()) {
    wait_io_parameter();
    if (!m_running.load()) {
      break;
    }

    m_mutex.lock();
    if (m_io_para.empty()) {
      m_mutex.unlock();
      continue;
    }
    io_parameter = m_io_para.front();
    m_io_para.pop();
    m_mutex.unlock();

    LB_Io_Result *result = &m_result;
    if (nullptr == result) {
      io_parameter->init();
      io_parameter = nullptr;
      push_io_result_and_notify(nullptr);
      continue;
    }

    do_lb_io_operation(io_parameter, result);
    push_io_result_and_notify(result);
  }
  m_have_leave.store(true);
}

bool LB_Io_Thrd_Context::init() {
  const uint32_t wait_time_ms = 2;

  m_parameter.init();
  m_result.reset();
  m_io_thread =
      new (std::nothrow) std::thread(&LB_Io_Thrd_Context::io_main, this);
  if (nullptr == m_io_thread) {
    ib::error() << "Alloc lb innodb io thread fail.";
    return true;
  }
  while (!m_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_time_ms));
  }
  return false;
}

void LB_Io_Thrd_Context::push_io_parameter_and_notify(
    LB_Io_Parameter *parameter) {
  m_mutex.lock();
  if (nullptr != parameter) {
    m_io_para.push(parameter);
  }
  m_has_param_notify = true;
  m_mutex.unlock();
  m_cv.notify_all();
}

void LB_Io_Thrd_Context::push_io_result_and_notify(LB_Io_Result *result) {
  {
    std::unique_lock<std::mutex> lock(m_result_mutex);
    m_io_result.push(result);
  }
  m_result_cv.notify_all();
}

void LB_Io_Thrd_Context::wait_io_parameter() {
  const uint64_t base_timeout_ms = 1;   /*1ms*/
  const uint64_t max_timeout_ms = 1000; /*1s*/
  uint64_t loop = 0;
  std::unique_lock<std::mutex> lock(m_mutex);
  while (!m_has_param_notify) {
    loop++;
    const uint64_t timeout_ms =
        std::min(base_timeout_ms * loop, max_timeout_ms);
    (void)m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    if (!m_running.load()) {
      ib::info() << "One lb innodb io thread need exit";
      break;
    }
  }
  m_has_param_notify = false;
}

LB_Io_Result *LB_Io_Thrd_Context::wait_io_result() {
  const uint64_t base_timeout_ms = 1;   /*1ms*/
  const uint64_t max_timeout_ms = 1000; /*1s*/
  uint64_t loop = 0;
  std::unique_lock<std::mutex> lock(m_result_mutex);
  auto start_time = std::chrono::high_resolution_clock::now();
  while (m_io_result.empty()) {
    loop++;
    uint64_t timeout_ms = std::min(base_timeout_ms * loop, max_timeout_ms);
    (void)m_result_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    if (duration.count() > (int64_t)innodb_lb_local_io_hang_timeout_ms) {
      ib::error() << "Wait innodb io thread doing io work timeout "
                  << duration.count() << " ms for local backup.";
      break;
    }
  }
  LB_Io_Result *result = nullptr;
  if (!m_io_result.empty()) {
    result = m_io_result.front();
    m_io_result.pop();
  }
  return result;
}

bool LB_Io_Thrd_Context::try_kill_io_thread() {
  ut_ad(nullptr != m_io_thread);

  const uint64_t max_sleep_ms = 1000;
  const uint64_t base_sleep_ms = 1;
  uint32_t loop = 0;
  m_running.store(false);
  push_io_parameter_and_notify(nullptr);
  auto start_time = std::chrono::high_resolution_clock::now();
  while (!m_have_leave.load()) {
    loop++;
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min(loop * base_sleep_ms, max_sleep_ms)));
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    if (duration.count() > (int64_t)innodb_lb_local_io_thread_quit_timeout_ms) {
      ib::error() << "Try kill one LB io thread time out during time "
                  << duration.count() << " ms.";
      break;
    }
    m_running.store(false);
    push_io_parameter_and_notify(nullptr);
  }
  if (m_have_leave.load()) {
    m_io_thread->join();
    delete m_io_thread;
    m_io_thread = nullptr;
    m_parameter.init();
    m_result.reset();
    return false;
  }
  return true;
}

static bool try_create_local_io_thread() {
  if (innodb_lb_use_io_thread) {
    if (nullptr != g_lb_io_thrd_context_pair.lb_io_context) {
      std::unique_lock<std::mutex> lock(g_lb_io_thrd_context_mgr.m_mutex);
      auto iter = g_lb_io_thrd_context_mgr.m_lb_io_context_map.find(
          g_lb_io_thrd_context_pair.key);
      /* lb io context have removed from last local backup task */
      if (iter == g_lb_io_thrd_context_mgr.m_lb_io_context_map.end()) {
        g_lb_io_thrd_context_pair.lb_io_context = nullptr;
        ib::error()
            << "Need Replace invalid lb innodb io thrd context use new object";
      }
    }
    if (nullptr == g_lb_io_thrd_context_pair.lb_io_context) {
      g_lb_io_thrd_context_pair.lb_io_context =
          new (std::nothrow) LB_Io_Thrd_Context();
      if (nullptr == g_lb_io_thrd_context_pair.lb_io_context) {
        ib::error() << "Alloc innodb io thread context fail.";
        return true;
      }
      if (g_lb_io_thrd_context_pair.lb_io_context->init()) {
        delete g_lb_io_thrd_context_pair.lb_io_context;
        g_lb_io_thrd_context_pair.lb_io_context = nullptr;
        ib::error() << "Lb innodb io context init fail.";
        return true;
      }
      g_lb_io_thrd_context_pair.key =
          __sync_fetch_and_add(&(g_lb_io_thrd_context_mgr.m_lb_io_thrd_inc), 1);
      std::unique_lock<std::mutex> lock(g_lb_io_thrd_context_mgr.m_mutex);
      g_lb_io_thrd_context_mgr.m_lb_io_context_map.emplace(
          std::make_pair(g_lb_io_thrd_context_pair.key,
                         g_lb_io_thrd_context_pair.lb_io_context));
    }
  }
  return false;
}

dberr_t wait_lb_io_result() {
  LB_Io_Result *result =
      g_lb_io_thrd_context_pair.lb_io_context->wait_io_result();
  if (nullptr == result) {
    ib::error() << "Wait lb innodb io result is empty.";
    return DB_ERROR;
  }

  dberr_t ret = result->ret;
  errno = result->io_errno;
  result->reset();
  result = nullptr;
  return ret;
}

bool create_and_push_lb_parameter(Backup_Callback *callback,
                                  Backup_Data_Desc backup_data_desc) {
  LB_Io_Parameter *parameter =
      &(g_lb_io_thrd_context_pair.lb_io_context->m_parameter);
  parameter->init();
  parameter->m_config = callback->get_config();
  parameter->m_callback =
      std::make_unique<Local_Backup_Callback>(&parameter->m_config);
  parameter->m_backup_data_desc = backup_data_desc;
  g_lb_io_thrd_context_pair.lb_io_context->push_io_parameter_and_notify(
      parameter);
  return false;
}

void destroy_lb_io_context() {
  std::unique_lock<std::mutex> lock(g_lb_io_thrd_context_mgr.m_mutex);
  auto iter = g_lb_io_thrd_context_mgr.m_lb_io_context_map.begin();
  while (iter != g_lb_io_thrd_context_mgr.m_lb_io_context_map.end()) {
    LB_Io_Thrd_Context *context = iter->second;
    ut_ad(nullptr != context);
    if (context->try_kill_io_thread()) {
      g_lb_io_thrd_context_mgr.m_rigid_io_context_map.emplace(
          std::make_pair(iter->first, iter->second));
    } else {
      delete context;
      context = nullptr;
    }
    iter = g_lb_io_thrd_context_mgr.m_lb_io_context_map.erase(iter);
  }
  g_lb_io_thrd_context_mgr.m_lb_io_error_retry_number.store(0);
}

void try_clear_rigid_io_context() {
  std::unique_lock<std::mutex> lock(g_lb_io_thrd_context_mgr.m_mutex);
  auto iter = g_lb_io_thrd_context_mgr.m_rigid_io_context_map.begin();
  while (iter != g_lb_io_thrd_context_mgr.m_rigid_io_context_map.end()) {
    LB_Io_Thrd_Context *context = iter->second;
    if (context->try_kill_io_thread()) {
      ib::error() << "Try kill one innodb io thread fail during clear rigid io "
                     "context.";
      iter++;
    } else {
      delete context;
      context = nullptr;
      iter = g_lb_io_thrd_context_mgr.m_rigid_io_context_map.erase(iter);
    }
  }
}

/** Local backup system global */
Local_Backup_Sys *local_backup_sys = nullptr;

Local_Backup_Sys Local_Backup_Sys::instance;

Local_Backup_Sys::Local_Backup_Sys()
    : m_running(false), m_should_stop(false), m_complete(false) {}

Local_Backup_Sys::~Local_Backup_Sys() {}

bool Local_Backup_Sys::do_backup(Backup_Callback *callback) {
  /* It is a must to disable change buffer on Taurus on dstore for local backup.
  Only DD tables use InnoDB for Taurus on dstore. DD tables don't use change
  buffer. See ibuf_should_try(). Disable change buffer to avoid potential ibuf
  merge when read secondary index page during backup. In case other InnoDB
  tables introduced in the future for Taurus on dstore. (Shouldn't happen). */
  if (innodb_change_buffering != IBUF_USE_NONE) {
    ib::error() << "Cannot start local backup: change buffer is not disabled";
    return true;
  }

  try_clear_rigid_io_context();

  {
    std::unique_lock<std::mutex> lock(m_state_mutex);
    if (m_running) {
      ib::error() << "Cannot start local backup: local backup is running";
      return true;
    }
    m_running = true;
    m_should_stop = false;
    m_complete = false;
  }

  Backup_End_Guard end_guard(this);
  std::vector<Backup_Data_Desc> backup_files;

  /* Do checkpoint may need flush log in dict_persist_to_dd_table_buffer(). So
  create checkpoint first, then pause log write. */
  log_request_checkpoint(*log_sys, true);

  /* Will resume write when destory Backup_End_Guard. */
  pause_write();

  /* Get all InnoDB tablespaces. Including undo. */
  dberr_t err = Fil_iterator::for_each_file([&](fil_node_t *file) {
    if (should_stop()) {
      return DB_INTERRUPTED;
    }
    backup_files.emplace_back(file->name);
    return DB_SUCCESS;
  });
  if (err != DB_SUCCESS) {
    ut_ad(err == DB_INTERRUPTED && should_stop());
    ib::error() << "Local backup interrupted when traverse tablespace files";
    return true;
  }

  /* Get redo log files. */
  ut::vector<Log_file_id> redo_file_ids;
  err = log_list_existing_files(log_sys->m_files_ctx, redo_file_ids);
  if (err != DB_SUCCESS) {
    ib::error() << "Local backup list redo files error: " << err;
    return true;
  }
  for (Log_file_id id : redo_file_ids) {
    if (should_stop()) {
      ib::error() << "Local backup interrupted when traverse redo files";
      return true;
    }
    std::string file_path = log_file_path(log_sys->m_files_ctx, id);
    backup_files.emplace_back(file_path);
  }

  if (callback->backup_cbk(backup_files, this)) {
    return true;
  }

  destroy_lb_io_context();

  m_complete = true;
  return false;
}

bool Local_Backup_Sys::stop_backup() {
  std::unique_lock<std::mutex> lock(m_state_mutex);

  if (m_running) {
    m_should_stop = true;
    while (m_running) {
      m_wait_end.wait(lock);
    }
  }

  if (!m_complete) {
    ib::info() << "Local backup stopped but not complete";
  }
  return false;
}

void Local_Backup_Sys::pause_write() {
  /* Stop purge */
  if (trx_purge_state() != PURGE_STATE_DISABLED) {
    trx_purge_stop();
  }

  /* Stop log writer */
  log_pause_write_for_local_backup();

  /* Stop checkpoint */
  log_checkpointer_mutex_enter(*log_sys);
  log_limits_mutex_enter(*log_sys);
  log_sys->periodical_checkpoints_enabled = false;
  log_limits_mutex_exit(*log_sys);
  log_checkpointer_mutex_exit(*log_sys);
}

void Local_Backup_Sys::resume_write() {
  /* Resume checkpoint */
  log_checkpointer_mutex_enter(*log_sys);
  log_limits_mutex_enter(*log_sys);
  log_sys->periodical_checkpoints_enabled = true;
  log_limits_mutex_exit(*log_sys);
  log_checkpointer_mutex_exit(*log_sys);

  /* Resume log writer */
  log_resume_write_for_local_backup();

  /* Resume purge */
  if (trx_purge_state() != PURGE_STATE_DISABLED) {
    trx_purge_run();
  }
}

bool Local_Backup_Sys::should_stop() {
  std::unique_lock<std::mutex> lock(m_state_mutex);
  return m_should_stop;
}

bool Local_Backup_Callback::backup_cbk(std::vector<Backup_Data_Desc> &data_desc,
                                       Local_Backup_Sys *backup_sys) {
  for (auto file : data_desc) {
    if (backup_sys->should_stop()) {
      ib::error() << "Local backup interrupted when copy files";
      return true;
    }

    dberr_t err = DB_SUCCESS;
    if (innodb_lb_use_io_thread && !try_create_local_io_thread()) {
      if (create_and_push_lb_parameter(this, file)) {
        err = DB_ERROR;
      }
      err = wait_lb_io_result();
    } else {
      err = backup_cbk(&file);
    }
    if (err != DB_SUCCESS) {
      ib::error() << "Local backup file " << file.file_path
                  << " error: " << err;
      return true;
    }
  }
  return false;
}

dberr_t Local_Backup_Callback::backup_cbk(Backup_Data_Desc *data_desc) {
  std::stringstream ss_dest;
  ss_dest << m_config->data_base_path;

  size_t path_len = m_config->data_base_path.size();
  ut_ad(path_len > 0);
  ut_ad(path_len < OS_FILE_MAX_PATH);
  if (m_config->data_base_path[path_len - 1] != OS_PATH_SEPARATOR) {
    /* Add a '/' to the end */
    ss_dest << OS_PATH_SEPARATOR;
  }
  ss_dest << data_desc->file_path;
  std::string dest_file_path = ss_dest.str();

  if (DBUG_EVALUATE_IF("innodb_lb_cbk_file_exist_fail", true, false) ||
      os_file_exists(dest_file_path.c_str())) {
    ib::error() << "Backup file already exist: " << dest_file_path;
    return DB_ERROR;
  }

  dberr_t err = os_file_create_subdirs_if_needed(dest_file_path.c_str());
  DBUG_EXECUTE_IF("innodb_lb_cbk_create_sub_dir_fail", err = DB_ERROR;);
  if (err != DB_SUCCESS) {
    ib::error() << "Failed to create directory when backup " << dest_file_path;
    return err;
  }

  os_file_t dest_file =
      ::open(dest_file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, my_umask);
  DBUG_EXECUTE_IF("innodb_lb_cbk_open_dst_fail", {
    ::close(dest_file);
    dest_file = -1;
  });
  if (dest_file == -1) {
    ib::error() << "Failed to open destination file " << dest_file_path
                << " for backup. errno: " << errno;
    return DB_CANNOT_OPEN_FILE;
  }
  os_file_t src_file = ::open(data_desc->file_path.c_str(), O_RDONLY, my_umask);
  DBUG_EXECUTE_IF("innodb_lb_cbk_open_src_fail", {
    ::close(src_file);
    src_file = -1;
  });
  if (src_file == -1) {
    ib::error() << "Failed to open source file " << data_desc->file_path
                << " for backup. errno: " << errno;
    ::close(dest_file);
    return DB_CANNOT_OPEN_FILE;
  }

  off_t file_size = lseek(src_file, 0, SEEK_END);
  DBUG_EXECUTE_IF("innodb_lb_cbk_lseek_fail", file_size = -1;);
  if (file_size == -1) {
    ib::error() << "Failed to get size of " << data_desc->file_path
                << " for backup. errno: " << errno;
    ::close(src_file);
    ::close(dest_file);
    return DB_IO_ERROR;
  }

  ib::info() << "Start to copy file " << data_desc->file_path
             << " for backup. Size: " << file_size;

  /* To support file larger than 4GB. off_t should be 64-bit. */
  static_assert(sizeof(off_t) == 8);

  /* 'size' of os_file_copy_func() is 32-bit. Copy file in a loop to support
  file larger than 4GB. */
  off_t left_size = file_size;
  while (left_size > 0) {
    os_offset_t copy_offset = file_size - left_size;
    uint copy_step = (2 << 28);  // 512M
    if (innodb_lb_use_io_thread) {
      copy_step = (1 << 24);  // 16M
    }
    uint copy_size = left_size <= copy_step ? left_size : copy_step;
    err = os_file_copy_func(src_file, copy_offset, dest_file, copy_offset,
                            copy_size);
    DBUG_EXECUTE_IF("innodb_lb_cbk_copy_fail", err = DB_ERROR;);
    if (err != DB_SUCCESS) {
      ib::error() << "Failed to copy file " << data_desc->file_path << " to "
                  << dest_file_path << " for backup, error: " << err;
      break;
    }
    ::fsync(dest_file);
    left_size -= copy_size;
  }

  ::close(src_file);
  ::close(dest_file);
  if (err != DB_SUCCESS) {
    bool exist = true;
    os_file_delete_if_exists_func(dest_file_path.c_str(), &exist);
  }
  return err;
}

bool Local_Backup_Obs_Callback::backup_cbk(
    std::vector<Backup_Data_Desc> &data_desc, Local_Backup_Sys *backup_sys) {
  std::vector<BakFileItem> merge_files;

  std::stringstream ss_dest;
  ss_dest << m_config->data_base_path;

  size_t path_len = m_config->data_base_path.size();
  ut_ad(path_len > 0);
  ut_ad(path_len < OS_FILE_MAX_PATH);
  if (m_config->data_base_path[path_len - 1] != OS_PATH_SEPARATOR) {
    /* Add a '/' to the end */
    ss_dest << OS_PATH_SEPARATOR;
  }

  /* Merge tablesapce files, undo files and redo files into one obs object. */
  for (auto file : data_desc) {
    if (backup_sys->should_stop()) {
      ib::error() << "Local backup interrupted when get size of files";
      return true;
    }

    off_t file_size = get_file_size_for_backup(file.file_path);
    if (file_size == -1) {
      ib::error() << "Local backup get file size fail for " << file.file_path;
      return true;
    }

    merge_files.emplace_back(file.file_path, file.file_path, file_size);
  }

  lb_object_handler *handler = fetch_lb_object_handler();
  // LCOV_EXCL_START
  if (nullptr == handler) {
    ib::error()
        << "failed to fetch lb object handler for local backup with obs mode";
    return true;
  }
  // LCOV_EXCL_STOP
  std::string merge_obj_name(ss_dest.str());
  merge_obj_name.append(full_lb_dd_obs_obj_name);
  handler->make_object_name(merge_obj_name, OBS_OBJ_TYPE_MERGED_FILES, 0, 0);
  increase_current_lb_object_count();

  if (gen_merged_file_obj_meta(merge_files, handler)) {
    come_back_lb_object_handler(handler);
    ib::error() << "Local backup failed to get merge file meta";
    return true;
  }

  // Write each file into object
  for (auto file : merge_files) {
    if (backup_sys->should_stop()) {
      come_back_lb_object_handler(handler);
      ib::error() << "Local backup interrupted when write file to obs";
      return true;
    }
    if (append_one_file_to_obj(file.file_name_orig, file.file_size, handler)) {
      come_back_lb_object_handler(handler);
      ib::error() << "Failed to append file " << file.file_name_orig
                  << " to OBS object";
      return true;
    }
  }
  bool error = handler->append_object();
  if (error) {
    ib::error() << "failed to append file to OBS object";
  }
  come_back_lb_object_handler(handler);
  return error;
}
