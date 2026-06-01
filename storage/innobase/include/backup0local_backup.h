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

/** @file include/backup0local_backup.h
 Interface for local backup system for InnoDB data, redo and undo file

 *******************************************************/

#ifndef BACKUP_LOCAL_BACKUP_INCLUDE
#define BACKUP_LOCAL_BACKUP_INCLUDE

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "db0err.h"
#include "sql/handler.h"

/* Parameters for innodb io thread. */
extern bool innodb_lb_use_io_thread;
extern uint32_t innodb_lb_global_io_retry_number;
extern uint32_t innodb_lb_local_io_retry_times;
extern int32_t innodb_lb_local_io_hang_timeout_ms;
extern int32_t innodb_lb_local_io_thread_quit_timeout_ms;

/** Information of data to backup.
Copy file only for InnoDB in Taurus on dstore. */
struct Backup_Data_Desc {
  std::string file_path;

  Backup_Data_Desc() = default;
  Backup_Data_Desc(const char *file) : file_path(file) {}
  Backup_Data_Desc(const std::string &file) : file_path(file) {}
};

class Local_Backup_Sys;

/** Base class for backup callback */
class Backup_Callback {
 public:
  Backup_Callback() {}
  virtual ~Backup_Callback() {}

  virtual local_backup_config get_config() const = 0;

  /** Process backup data.
  @param[in]  data_desc   information of data to backup
  @param[in]  backup_sys  Local_Backup_Sys instance
  @return false on success */
  virtual bool backup_cbk(std::vector<Backup_Data_Desc> &data_desc,
                          Local_Backup_Sys *backup_sys) = 0;

  /** Process backup data for one single file.
  @param[in]  data_desc  information of data to backup
  @return error code */
  virtual dberr_t backup_cbk(Backup_Data_Desc *data_desc) = 0;
};

class Local_Backup_Callback : public Backup_Callback {
 public:
  explicit Local_Backup_Callback(local_backup_config *config)
      : m_config(config) {}
  virtual ~Local_Backup_Callback() { m_config = nullptr; }

  virtual local_backup_config get_config() const override { return *m_config; }

  /** Process backup data. Copy file to destination due to 'm_config'.
  @param[in]  data_desc   information of data to backup
  @param[in]  backup_sys  Local_Backup_Sys instance
  @return false on success */
  virtual bool backup_cbk(std::vector<Backup_Data_Desc> &data_desc,
                          Local_Backup_Sys *backup_sys) override;

  /** Process backup data for one single file.
  @param[in]  data_desc  information of data to backup
  @return error code */
  virtual dberr_t backup_cbk(Backup_Data_Desc *data_desc) override;

 protected:
  local_backup_config *m_config;
};

class Local_Backup_Obs_Callback : public Local_Backup_Callback {
 public:
  explicit Local_Backup_Obs_Callback(local_backup_config *config)
      : Local_Backup_Callback(config) {}
  virtual ~Local_Backup_Obs_Callback() {}

  /** Process backup data. Upload file to OBS objects.
  @param[in]  data_desc   information of data to backup
  @param[in]  backup_sys  Local_Backup_Sys instance
  @return false on success */
  virtual bool backup_cbk(std::vector<Backup_Data_Desc> &data_desc,
                          Local_Backup_Sys *backup_sys) override;

  /** Process backup data for one single file.
  @param[in]  data_desc  information of data to backup
  @return error code */
  virtual dberr_t backup_cbk(Backup_Data_Desc *data_desc) override {
    // TODO: support async io thread for OBS
    return DB_ERROR;
  }
};

/** Local backup system */
class Local_Backup_Sys {
 public:
  static Local_Backup_Sys *get_instance() { return &instance; }

  /** Resume InnoDB write and set state when destructor is called. */
  class Backup_End_Guard {
   public:
    explicit Backup_End_Guard(Local_Backup_Sys *backup_sys)
        : m_backup_sys(backup_sys) {}

    ~Backup_End_Guard() {
      m_backup_sys->resume_write();

      std::unique_lock<std::mutex> lock(m_backup_sys->m_state_mutex);
      m_backup_sys->m_running = false;
      m_backup_sys->m_wait_end.notify_all();
    }

   private:
    Local_Backup_Sys *m_backup_sys;
  };

 private:
  Local_Backup_Sys();
  ~Local_Backup_Sys();

 public:
  /** Backup InnoDB data. Will fetch list of InnoDB files and use callback to
  process each file. Write operations will be paused during backup.
  @param[in]  callback  callback to process files for backup
  @return false on success */
  bool do_backup(Backup_Callback *callback);

  /** Stop backup and wait do_backup() exit. Ongoing backup will be interrupted.
  @return false if backup is complete */
  bool stop_backup();

  bool should_stop();

 private:
  static Local_Backup_Sys instance;

  std::mutex m_state_mutex;
  bool m_running;
  bool m_should_stop;
  bool m_complete;
  std::condition_variable m_wait_end;

  void pause_write();
  void resume_write();
};

/* Parameter for innodb io thread. */
struct LB_Io_Parameter {
  LB_Io_Parameter() : m_config(), m_callback(nullptr), m_backup_data_desc() {}
  ~LB_Io_Parameter() {}

  void init() {
    m_config.data_base_path.clear();
    m_config.meta_base_path.clear();
    m_config.root_path.clear();
    m_callback.reset();
    m_backup_data_desc.file_path.clear();
  }

  local_backup_config m_config;
  std::unique_ptr<Backup_Callback> m_callback;
  Backup_Data_Desc m_backup_data_desc;
};

/* Result for innodb io thread. */
struct LB_Io_Result {
  LB_Io_Result() : ret(DB_ERROR), io_errno(0) {}
  ~LB_Io_Result() {}

  void reset() {
    ret = DB_ERROR;
    io_errno = 0;
  }

  dberr_t ret;
  int io_errno;
};

/* Innodb io thread context. */
struct LB_Io_Thrd_Context {
  LB_Io_Thrd_Context()
      : m_io_thread(nullptr),
        m_has_param_notify(false),
        m_mutex(),
        m_cv(),
        m_result_mutex(),
        m_result_cv(),
        m_io_para(),
        m_io_result(),
        m_parameter(),
        m_result() {}
  ~LB_Io_Thrd_Context() { reset(); }

  void reset() {
    while (!m_io_para.empty()) {
      LB_Io_Parameter *parameter = m_io_para.front();
      m_io_para.pop();
      parameter->init();
      parameter = nullptr;
    }
    while (!m_io_result.empty()) {
      LB_Io_Result *result = m_io_result.front();
      m_io_result.pop();
      result->reset();
      result = nullptr;
    }
    m_parameter.init();
    m_result.reset();
    m_have_leave.store(false);
    m_running.store(false);
    m_has_param_notify = false;
  }

  bool init();
  void io_main();
  void push_io_parameter_and_notify(LB_Io_Parameter *parameter);
  void push_io_result_and_notify(LB_Io_Result *result);
  void wait_io_parameter();
  LB_Io_Result *wait_io_result();
  bool try_kill_io_thread();

  std::thread *m_io_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_have_leave{false};
  bool m_has_param_notify;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::mutex m_result_mutex;
  std::condition_variable m_result_cv;
  std::queue<LB_Io_Parameter *> m_io_para;
  std::queue<LB_Io_Result *> m_io_result;
  LB_Io_Parameter m_parameter;
  LB_Io_Result m_result;
};

/* Innodb io thread context pair. */
struct LB_Io_Thrd_Context_Pair {
  uint64_t key;
  LB_Io_Thrd_Context *lb_io_context;
};

/* Innodb io thread context manager. */
struct LB_Io_Thrd_Context_Mgr {
  LB_Io_Thrd_Context_Mgr()
      : m_lb_io_thrd_inc(0),
        m_lb_io_context_map(),
        m_rigid_io_context_map(),
        m_mutex() {}
  ~LB_Io_Thrd_Context_Mgr() {}

  uint64_t m_lb_io_thrd_inc;
  std::unordered_map<uint64_t, LB_Io_Thrd_Context *> m_lb_io_context_map;
  std::unordered_map<uint64_t, LB_Io_Thrd_Context *> m_rigid_io_context_map;
  std::atomic<uint32_t> m_lb_io_error_retry_number{0};
  std::mutex m_mutex;
};

/** Local backup system global */
extern Local_Backup_Sys *local_backup_sys;
#endif /* BACKUP_LOCAL_BACKUP_INCLUDE */
