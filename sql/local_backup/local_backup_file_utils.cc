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

#ifndef IS_DSTORE_BACKUP_TOOL
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include "my_securec.h"
#endif

#include "local_backup_file_utils.h"
#include "my_dir.h"
#include "my_sys.h"
#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"

/*
  The interface type that local backup uses to do the file operations.
    0 => sys calls - open/close/pread/pwrite...
    1 => mysys calls - my_open/my_close/my_pread/my_pwrite...
*/
uint32_t rds_local_backup_file_interface_type =
    static_cast<uint32_t>(CDE::LBFileInterfaceType::MYSYS_CALL_TYPE);
/*
  Sleep interval for full local backup, after this value times write, do a
  sleep.
*/
uint32_t rds_local_backup_sleep_interval = 0;
/* Sleep time in milliseconds for full local backup. */
uint32_t rds_local_backup_sleep_time_ms = 10;

bool rds_dstore_lb_use_io_thread = false;
uint32_t rds_dstore_lb_global_io_retry_number = 0;
uint32_t rds_dstore_lb_local_io_retry_times = 0;
int32_t rds_dstore_lb_local_io_hang_timeout_ms = 100000;
int32_t rds_dstore_lb_local_io_thread_quit_timeout_ms = 1000;
uint32_t rds_dstore_lb_io_buffer_size = 64 * 1024;

namespace CDE {

static const ssize_t LB_FILE_ERROR = -1;
static const int LB_IO_ERROR = -1;
static const uint32_t LB_BUFFER_SIZE = 8192 * 64;

#ifndef IS_DSTORE_BACKUP_TOOL
enum LBIoOpType {
  LBOpenOp = 0,
  LBCloseOp,
  LBFtruncateOp,
  LBFstatOp,
  LBStatOp,
  LBPreadOp,
  LBPwriteOp,
  LBFsyncOp,
  LBMkdirOp,
  LBMAXOP = 100,
};

struct LBIoParameter {
  explicit LBIoParameter(LBIoOpType type)
      : m_opType(type),
        m_path(nullptr),
        m_file(nullptr),
        m_fd(LB_IO_ERROR),
        m_mode(LB_IO_ERROR),
        m_flag(LB_IO_ERROR),
        m_buf(nullptr),
        m_length(0),
        m_offset(0) {}
  ~LBIoParameter() {
    m_buf = nullptr;
    if (nullptr != m_file) {
      delete[] m_file;
      m_file = nullptr;
    }
    if (nullptr != m_path) {
      delete[] m_path;
      m_path = nullptr;
    }
  }

  void init(LBIoOpType type) {
    m_opType = type;
    m_path = nullptr;
    m_file = nullptr;
    m_fd = LB_IO_ERROR;
    m_mode = LB_IO_ERROR;
    m_flag = LB_IO_ERROR;
    m_buf = nullptr;
    m_length = 0;
    m_offset = 0;
  }

  LBIoOpType m_opType;
  char *m_path;
  char *m_file;
  int m_fd;
  mode_t m_mode;
  int m_flag;
  uint8_t *m_buf;
  uint64_t m_length;
  uint64_t m_offset;
};

struct LBIoResult {
  LBIoResult()
      : use_ctrl(false),
        ctrl_ret(LB_IO_ERROR),
        wr_ret(LB_FILE_ERROR),
        io_errno(0),
        stat_buf(nullptr),
        buf(nullptr) {}
  ~LBIoResult() {
    if (nullptr != stat_buf) {
      delete stat_buf;
      stat_buf = nullptr;
    }
    if (nullptr != buf) {
      delete[] buf;
      buf = nullptr;
    }
  }

  void reset() {
    use_ctrl = false;
    ctrl_ret = LB_IO_ERROR;
    wr_ret = LB_FILE_ERROR;
    io_errno = 0;
    stat_buf = nullptr;
    buf = nullptr;
  }

  bool use_ctrl;
  int ctrl_ret;
  ssize_t wr_ret;
  int io_errno;
  struct stat *stat_buf;
  uint8_t *buf;
};

static void DeleteLbIoParameter(LBIoParameter *ioParameter) {
  ioParameter->m_buf = nullptr;
  if (nullptr != ioParameter->m_file) {
    delete[] ioParameter->m_file;
    ioParameter->m_file = nullptr;
  }
  if (nullptr != ioParameter->m_path) {
    delete[] ioParameter->m_path;
    ioParameter->m_path = nullptr;
  }
  ioParameter->init(LBIoOpType::LBMAXOP);
}

static void DeleteLbIoResult(LBIoResult *result) {
  if (nullptr != result->stat_buf) {
    delete result->stat_buf;
    result->stat_buf = nullptr;
  }
  if (nullptr != result->buf) {
    delete[] result->buf;
    result->buf = nullptr;
  }
}

struct LBIoThrdContext {
  LBIoThrdContext()
      : m_ioThread(nullptr),
        m_hasParamNotify(false),
        m_mutex(),
        m_cv(),
        m_result_mutex(),
        m_result_cv(),
        m_ioPara(),
        m_ioResult(),
        m_buffer(nullptr),
        m_buffer_size(std::max(LB_BUFFER_SIZE, rds_dstore_lb_io_buffer_size)),
        m_parameter(LBIoOpType::LBMAXOP),
        m_result() {}

  ~LBIoThrdContext() { reset(); }

  void reset() {
    while (!m_ioPara.empty()) {
      LBIoParameter *parameter = m_ioPara.front();
      m_ioPara.pop();
      DeleteLbIoParameter(parameter);
      parameter = nullptr;
    }
    while (!m_ioResult.empty()) {
      LBIoResult *result = m_ioResult.front();
      m_ioResult.pop();
      DeleteLbIoResult(result);
      result = nullptr;
    }
    if (nullptr != m_buffer) {
      delete[] m_buffer;
      m_buffer = nullptr;
    }
    DeleteLbIoParameter(&m_parameter);
    m_parameter.init(LBIoOpType::LBMAXOP);
    DeleteLbIoResult(&m_result);
    m_result.reset();
    m_haveLeave.store(false);
    m_running.store(false);
    m_hasParamNotify = false;
  }
  bool Init();
  void IoMain();
  void PushIoParameterAndNotify(LBIoParameter *parameter);
  void PushIoResultAndNotify(LBIoResult *result);
  void WaitIoParameter();
  LBIoResult *WaitIoResult();
  bool TryKillIoThread();

  std::thread *m_ioThread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_haveLeave{false};
  bool m_hasParamNotify;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::mutex m_result_mutex;
  std::condition_variable m_result_cv;
  std::queue<LBIoParameter *> m_ioPara;
  std::queue<LBIoResult *> m_ioResult;
  uint8_t *m_buffer;
  uint32_t m_buffer_size;
  LBIoParameter m_parameter;
  LBIoResult m_result;
};

struct LBIoThrdContextPair {
  uint64_t key;
  LBIoThrdContext *lbIoContext;
};

struct LBIoThrdContextMgr {
  LBIoThrdContextMgr()
      : m_lbIoThrdInc(0),
        m_lbIoContextMap(),
        m_rigidIoContextMap(),
        m_mutex() {}
  ~LBIoThrdContextMgr() {}

  uint64_t m_lbIoThrdInc;
  std::unordered_map<uint64_t, LBIoThrdContext *> m_lbIoContextMap;
  std::unordered_map<uint64_t, LBIoThrdContext *> m_rigidIoContextMap;
  std::atomic<uint32_t> m_lbIoErrorRetryNumber{0};
  std::mutex m_mutex;
};

static LBIoThrdContextMgr g_lbIoThrdContextMgr;
static thread_local LBIoThrdContextPair g_lbIoThrdContextPair = {UINT64_MAX,
                                                                 nullptr};

#endif

int LBOpenInternal(const char *file, int oflag);
int LBOpenInternal(const char *file, int oflag, mode_t mode);
int LBCloseInternal(int fd);
int LBFtruncateInternal(int fd, uint64_t length);
int LBFstatInternal(int fd, struct stat *buf);
int LBStatInternal(const char *file, struct stat *buf);
ssize_t LBPreadInternal(int fd, void *buf, uint64_t length, uint64_t offset);
ssize_t LBPwriteInternal(int fd, void *buf, uint64_t length, uint64_t offset);
int LBFsyncInternal(int fd);
int LBMkdirInternal(const char *path, mode_t mode);

#ifndef IS_DSTORE_BACKUP_TOOL

void DoLbIoOperation(LBIoParameter *ioParameter, LBIoResult *result) {
  uint32_t times = 0;
  int ctrl_res = LB_IO_ERROR;
  ssize_t wr_res = LB_FILE_ERROR;
  bool use_ctrl = true;

  assert(nullptr != ioParameter);
  assert(nullptr != result);

  do {
    switch (ioParameter->m_opType) {
      case LBIoOpType::LBOpenOp: {
        if (LB_IO_ERROR == (int)ioParameter->m_mode) {
          ctrl_res = LBOpenInternal(ioParameter->m_file, ioParameter->m_flag);
        } else {
          ctrl_res = LBOpenInternal(ioParameter->m_file, ioParameter->m_flag,
                                    ioParameter->m_mode);
        }
        break;
      }
      case LBIoOpType::LBCloseOp: {
        ctrl_res = LBCloseInternal(ioParameter->m_fd);
        break;
      }
      case LBIoOpType::LBFtruncateOp: {
        ctrl_res =
            LBFtruncateInternal(ioParameter->m_fd, ioParameter->m_length);
        break;
      }
      case LBIoOpType::LBFstatOp: {
        ctrl_res = LBFstatInternal(ioParameter->m_fd, result->stat_buf);
        break;
      }
      case LBIoOpType::LBStatOp: {
        ctrl_res = LBStatInternal(ioParameter->m_file, result->stat_buf);
        break;
      }
      case LBIoOpType::LBPreadOp: {
        use_ctrl = false;
        wr_res = LBPreadInternal(ioParameter->m_fd, (void *)(result->buf),
                                 ioParameter->m_length, ioParameter->m_offset);
        break;
      }
      case LBIoOpType::LBPwriteOp: {
        use_ctrl = false;
        wr_res =
            LBPwriteInternal(ioParameter->m_fd, (void *)(ioParameter->m_buf),
                             ioParameter->m_length, ioParameter->m_offset);
        break;
      }
      case LBIoOpType::LBFsyncOp: {
        ctrl_res = LBFsyncInternal(ioParameter->m_fd);
        break;
      }
      case LBIoOpType::LBMkdirOp: {
        ctrl_res = LBMkdirInternal(ioParameter->m_path, ioParameter->m_mode);
        break;
      }
      default: {
        return;
      }
    }

    if ((use_ctrl && ctrl_res >= 0) || (!use_ctrl && wr_res >= 0) ||
        (errno != EIO) ||
        (g_lbIoThrdContextMgr.m_lbIoErrorRetryNumber >
         rds_dstore_lb_global_io_retry_number)) {
      break;
    }
    times++;
  } while (times < rds_dstore_lb_local_io_retry_times);
  if (times > 0) {
    g_lbIoThrdContextMgr.m_lbIoErrorRetryNumber++;
  }
  result->use_ctrl = use_ctrl;
  if (use_ctrl) {
    result->ctrl_ret = ctrl_res;
    if (ctrl_res < 0) {
      result->io_errno = (int)errno;
    }
  } else {
    result->wr_ret = wr_res;
    if (wr_res < 0) {
      result->io_errno = (int)errno;
    }
  }
}

void LBIoThrdContext::IoMain() {
  LBIoParameter *ioParameter = nullptr;
  m_running.store(true);
  while (m_running.load()) {
    WaitIoParameter();
    if (!m_running.load()) {
      break;
    }

    m_mutex.lock();
    if (m_ioPara.empty()) {
      m_mutex.unlock();
      continue;
    }
    ioParameter = m_ioPara.front();
    m_ioPara.pop();
    m_mutex.unlock();

    LBIoResult *result = &m_result;
    if (nullptr == result) {
      DeleteLbIoParameter(ioParameter);
      ioParameter = nullptr;
      PushIoResultAndNotify(nullptr);
      continue;
    }
    if ((LBIoOpType::LBFstatOp == ioParameter->m_opType) ||
        (LBIoOpType::LBStatOp == ioParameter->m_opType)) {
      result->stat_buf = new (std::nothrow) struct stat();
      if (nullptr == result->stat_buf) {
        DeleteLbIoParameter(ioParameter);
        ioParameter = nullptr;
        DeleteLbIoResult(result);
        result = nullptr;
        PushIoResultAndNotify(nullptr);
        continue;
      }
    }

    if (LBIoOpType::LBPreadOp == ioParameter->m_opType) {
      result->buf = new (std::nothrow) uint8_t[ioParameter->m_length];
      if (nullptr == result->buf) {
        DeleteLbIoParameter(ioParameter);
        ioParameter = nullptr;
        DeleteLbIoResult(result);
        result = nullptr;
        PushIoResultAndNotify(nullptr);
        continue;
      }
    }

    DoLbIoOperation(ioParameter, result);
    PushIoResultAndNotify(result);
  }
  m_haveLeave.store(true);
}

bool LBIoThrdContext::Init() {
  const uint32_t waitTimeMs = 2;

  m_buffer = new (std::nothrow) uint8_t[m_buffer_size];
  if (nullptr == m_buffer) {
    return true;
  }
  m_parameter.init(LBIoOpType::LBMAXOP);
  m_result.reset();
  m_ioThread = new (std::nothrow) std::thread(&LBIoThrdContext::IoMain, this);
  if (nullptr == m_ioThread) {
    delete[] m_buffer;
    m_buffer = nullptr;
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, "alloc lb io thread fail.");
    return true;
  }
  while (!m_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(waitTimeMs));
  }
  return false;
}

void LBIoThrdContext::PushIoParameterAndNotify(LBIoParameter *parameter) {
  m_mutex.lock();
  if (nullptr != parameter) {
    m_ioPara.push(parameter);
  }
  m_hasParamNotify = true;
  m_mutex.unlock();
  m_cv.notify_all();
}

void LBIoThrdContext::PushIoResultAndNotify(LBIoResult *result) {
  {
    std::unique_lock<std::mutex> lock(m_result_mutex);
    m_ioResult.push(result);
  }
  m_result_cv.notify_all();
}

void LBIoThrdContext::WaitIoParameter() {
  const uint64_t baseTimeoutMs = 1;   /*1ms*/
  const uint64_t maxTimeoutMs = 1000; /*1s*/
  uint64_t loop = 0;
  std::unique_lock<std::mutex> lock(m_mutex);
  while (!m_hasParamNotify) {
    loop++;
    const uint64_t timeoutMs = std::min(baseTimeoutMs * loop, maxTimeoutMs);
    (void)m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs));
    if (!m_running.load()) {
      LogErr(INFORMATION_LEVEL, ER_LOCAL_BACKUP_ERROR,
             "One Lb io thread need exit.");
      break;
    }
  }
  m_hasParamNotify = false;
}

LBIoResult *LBIoThrdContext::WaitIoResult() {
  const uint64_t baseTimeoutMs = 1;   /*1ms*/
  const uint64_t maxTimeoutMs = 1000; /*1s*/
  uint64_t loop = 0;
  std::unique_lock<std::mutex> lock(m_result_mutex);
  auto startTime = std::chrono::high_resolution_clock::now();
  while (m_ioResult.empty()) {
    loop++;
    uint64_t timeoutMs = std::min(baseTimeoutMs * loop, maxTimeoutMs);
    (void)m_result_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs));
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime);
    if (duration.count() > (int64_t)rds_dstore_lb_local_io_hang_timeout_ms) {
      std::stringstream ss;
      ss << "Wait io thread doing io work timeout " << duration.count()
         << " ms for local backup.";
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
      break;
    }
  }
  LBIoResult *result = nullptr;
  if (!m_ioResult.empty()) {
    result = m_ioResult.front();
    m_ioResult.pop();
  }
  return result;
}

bool LBIoThrdContext::TryKillIoThread() {
  assert(nullptr != m_ioThread);

  const uint64_t maxSleepMs = 1000;
  const uint64_t baseSleepMs = 1;
  uint32_t loop = 0;
  m_running.store(false);
  PushIoParameterAndNotify(nullptr);
  auto startTime = std::chrono::high_resolution_clock::now();
  while (!m_haveLeave.load()) {
    loop++;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::min(loop * baseSleepMs, maxSleepMs)));
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime);
    if (duration.count() >
        (int64_t)rds_dstore_lb_local_io_thread_quit_timeout_ms) {
      std::stringstream ss;
      ss << "Try kill one LB io thread time out during time "
         << duration.count() << " ms.";
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
      break;
    }
    m_running.store(false);
    PushIoParameterAndNotify(nullptr);
  }
  if (m_haveLeave.load()) {
    m_ioThread->join();
    delete m_ioThread;
    m_ioThread = nullptr;
    delete[] m_buffer;
    m_buffer = nullptr;
    DeleteLbIoParameter(&m_parameter);
    m_parameter.init(LBIoOpType::LBMAXOP);
    DeleteLbIoResult(&m_result);
    m_result.reset();
    return false;
  }
  return true;
}

static bool TryCreateLocalIoThread() {
  if (rds_dstore_lb_use_io_thread) {
    if (nullptr != g_lbIoThrdContextPair.lbIoContext) {
      std::unique_lock<std::mutex> lock(g_lbIoThrdContextMgr.m_mutex);
      auto iter =
          g_lbIoThrdContextMgr.m_lbIoContextMap.find(g_lbIoThrdContextPair.key);
      /* lb io context have removed from last local backup task */
      if (iter == g_lbIoThrdContextMgr.m_lbIoContextMap.end()) {
        g_lbIoThrdContextPair.lbIoContext = nullptr;
        LogErr(INFORMATION_LEVEL, ER_LOCAL_BACKUP_ERROR,
               "Need Replace invalid lb io thrd context use new object.");
      }
    }
    if (nullptr == g_lbIoThrdContextPair.lbIoContext) {
      g_lbIoThrdContextPair.lbIoContext = new (std::nothrow) LBIoThrdContext();
      if (nullptr == g_lbIoThrdContextPair.lbIoContext) {
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
               "Alloc io thread context fail.");
        return true;
      }
      if (g_lbIoThrdContextPair.lbIoContext->Init()) {
        delete g_lbIoThrdContextPair.lbIoContext;
        g_lbIoThrdContextPair.lbIoContext = nullptr;
        LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, "Lb io context init fail.");
        return true;
      }
      g_lbIoThrdContextPair.key =
          __sync_fetch_and_add(&(g_lbIoThrdContextMgr.m_lbIoThrdInc), 1);
      std::unique_lock<std::mutex> lock(g_lbIoThrdContextMgr.m_mutex);
      g_lbIoThrdContextMgr.m_lbIoContextMap.emplace(std::make_pair(
          g_lbIoThrdContextPair.key, g_lbIoThrdContextPair.lbIoContext));
    }
  }
  return false;
}

#endif

int LBOpenInternal(const char *file, int oflag) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return open(file, oflag);
  }
  return my_open(file, oflag, MYF(0));
}

int LBOpenInternal(const char *file, int oflag, mode_t mode) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return open(file, oflag, mode);
  }
  return my_open(file, oflag, MYF(0));
}

int LBCloseInternal(int fd) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return close(fd);
  }
  return my_close(fd, MYF(0));
}

int LBFtruncateInternal(int fd, uint64_t length) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return ftruncate(fd, length);
  }
  return my_chsize(fd, length, 0, MYF(0));
}

int LBFstatInternal(int fd, struct stat *buf) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return fstat(fd, buf);
  }
  return my_fstat(fd, buf);
}

int LBStatInternal(const char *file, struct stat *buf) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return stat(file, buf);
  }
  return my_stat(file, buf, MYF(0)) == nullptr ? -1 : 0;
}

ssize_t LBPreadInternal(int fd, void *buf, uint64_t length, uint64_t offset) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return pread(fd, buf, length, offset);
  }
  size_t res = my_pread(fd, (uchar *)buf, length, offset, MYF(0));
  return unlikely(res == MY_FILE_ERROR) ? LB_FILE_ERROR : (ssize_t)res;
}

ssize_t LBPwriteInternal(int fd, void *buf, uint64_t length, uint64_t offset) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return pwrite(fd, buf, length, offset);
  }
  size_t res = my_pwrite(fd, (uchar *)buf, length, offset, MYF(0));
  return unlikely(res == MY_FILE_ERROR) ? LB_FILE_ERROR : (ssize_t)res;
}

int LBFsyncInternal(int fd) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return fsync(fd);
  }
  return my_sync(fd, MYF(0));
}

int LBMkdirInternal(const char *path, __mode_t mode) {
  if (rds_local_backup_file_interface_type ==
      static_cast<uint32_t>(LBFileInterfaceType::SYS_CALL_TYPE)) {
    return mkdir(path, mode);
  }
  return my_mkdir(path, mode, MYF(0));
}

#ifndef IS_DSTORE_BACKUP_TOOL

int WaitCtrlIoResult(struct stat *buf) {
  LBIoResult *result = g_lbIoThrdContextPair.lbIoContext->WaitIoResult();
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  if (nullptr == result) {
    DeleteLbIoParameter(parameter);
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "Wait Lb ctrl io result is empty.");
    return LB_IO_ERROR;
  }
  if (nullptr != buf) {
    memcpy_s((char *)buf, sizeof(struct stat), (char *)(result->stat_buf),
             sizeof(struct stat));
  }

  int ret = result->ctrl_ret;
  errno = result->io_errno;
  DeleteLbIoParameter(parameter);
  DeleteLbIoResult(result);
  result = nullptr;
  return ret;
}

ssize_t WaitWriteIoResult() {
  LBIoResult *result = g_lbIoThrdContextPair.lbIoContext->WaitIoResult();
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  if (nullptr == result) {
    DeleteLbIoParameter(parameter);
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "Wait Lb write io result is empty.");
    return LB_FILE_ERROR;
  }

  ssize_t ret = result->wr_ret;
  errno = result->io_errno;
  DeleteLbIoParameter(parameter);
  DeleteLbIoResult(result);
  result = nullptr;
  return ret;
}

ssize_t WaitReadIoResult(void *buf, uint64_t length) {
  LBIoResult *result = g_lbIoThrdContextPair.lbIoContext->WaitIoResult();
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  if (nullptr == result) {
    // LCOV_EXCL_START
    DeleteLbIoParameter(parameter);
    // LCOV_EXCL_STOP
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
           "Wait Lb read io result is empty.");
    return LB_FILE_ERROR;
  }
  ssize_t ret = result->wr_ret;
  if (ret == (ssize_t)length) {
    memcpy_s((char *)buf, length, (char *)result->buf, length);
  }
  errno = result->io_errno;
  DeleteLbIoParameter(parameter);
  DeleteLbIoResult(result);
  result = nullptr;
  return ret;
}

bool CreateAndPushLBOpenParameter(const char *file, int oflag) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBOpenOp);
  parameter->m_file = new (std::nothrow) char[strlen(file) + 1];
  if (nullptr == parameter->m_file) {
    std::stringstream ss;
    ss << "Alloc file name for Lb io parameter " << LBIoOpType::LBOpenOp
       << " fail.";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
    parameter = nullptr;
    return true;
  }
  strncpy_s(parameter->m_file, strlen(file) + 1, file, strlen(file));
  parameter->m_flag = oflag;
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBOpenParameter(const char *file, int oflag, mode_t mode) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBOpenOp);
  parameter->m_file = new (std::nothrow) char[strlen(file) + 1];
  if (nullptr == parameter->m_file) {
    std::stringstream ss;
    ss << "Alloc file name for Lb io parameter " << LBIoOpType::LBOpenOp
       << " with mode " << mode << " fail.";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
    parameter = nullptr;
    return true;
  }
  strncpy_s(parameter->m_file, strlen(file) + 1, file, strlen(file));
  parameter->m_flag = oflag;
  parameter->m_mode = mode;
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

int LdCopyFd(int fd) { return fd; }

bool CreateAndPushLBCloseParameter(int fd) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBCloseOp);
  parameter->m_fd = LdCopyFd(fd);
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBFtruncateParameter(int fd, uint64_t length) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBFtruncateOp);
  parameter->m_fd = LdCopyFd(fd);
  parameter->m_length = length;
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBFstatParameter(int fd) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBFstatOp);
  parameter->m_fd = LdCopyFd(fd);
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBStatParameter(const char *file) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBStatOp);
  parameter->m_file = new (std::nothrow) char[strlen(file) + 1];
  if (nullptr == parameter->m_file) {
    std::stringstream ss;
    ss << "Alloc file name for Lb io parameter " << LBIoOpType::LBStatOp
       << " fail.";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
    parameter = nullptr;
  }
  strncpy_s(parameter->m_file, strlen(file) + 1, file, strlen(file));
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBPreadParameter(int fd, uint64_t length, uint64_t offset) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBPreadOp);
  parameter->m_fd = LdCopyFd(fd);
  parameter->m_length = length;
  parameter->m_offset = offset;
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBPwriteParameter(int fd, void *buf, uint64_t length,
                                    uint64_t offset) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBPwriteOp);
  parameter->m_fd = LdCopyFd(fd);
  parameter->m_buf = g_lbIoThrdContextPair.lbIoContext->m_buffer;
  memcpy_s(parameter->m_buf, length, (uint8_t *)buf, length);
  parameter->m_length = length;
  parameter->m_offset = offset;
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBFsyncParameter(int fd) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBFsyncOp);
  parameter->m_fd = LdCopyFd(fd);
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

bool CreateAndPushLBMkdir(const char *path, __mode_t mode) {
  LBIoParameter *parameter = &(g_lbIoThrdContextPair.lbIoContext->m_parameter);
  parameter->init(LBIoOpType::LBMkdirOp);
  parameter->m_path = new (std::nothrow) char[strlen(path) + 1];
  if (nullptr == parameter->m_path) {
    std::stringstream ss;
    ss << "Alloc path name for Lb io parameter " << LBIoOpType::LBMkdirOp
       << " fail.";
    LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR, ss.str().c_str());
    parameter = nullptr;
    return true;
  }
  strncpy_s(parameter->m_path, strlen(path) + 1, path, strlen(path));
  parameter->m_mode = mode;
  g_lbIoThrdContextPair.lbIoContext->PushIoParameterAndNotify(parameter);
  return false;
}

#endif

/**
  LBOpen, LBClose, LBFtruncate, LBFstat, LBStat, LBPread, LBPwrite, LBFsync,
LBMkdir using special io thread and detached from hang stat for local full
backup service thread
   1.  Try create local io context including io thread, if have then continue to
use
   2.  CreateAndPushLBXXX:  create io operation parameter, push to queue and
then notify the io thread
   3.  wait ctrl io or read/write io result during which the hang event will be
processed
   4.  io thread will call LBXXXInternal which is same with old LBOpen when have
io thread
 */

int LBOpen(const char *file, int oflag) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBOpenParameter(file, oflag)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(nullptr);
  }
#endif
  return LBOpenInternal(file, oflag);
}

int LBopenOther(const char *file, int oflag, mode_t mode) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBOpenParameter(file, oflag, mode)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(nullptr);
  }
#endif
  return LBOpenInternal(file, oflag, mode);
}

int LBClose(int fd) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBCloseParameter(fd)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(nullptr);
  }
#endif
  return LBCloseInternal(fd);
}

int LBFtruncate(int fd, uint64_t length) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBFtruncateParameter(fd, length)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(nullptr);
  }
#endif
  return LBFtruncateInternal(fd, length);
}

int LBFstat(int fd, struct stat *buf) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBFstatParameter(fd)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(buf);
  }
#endif
  return LBFstatInternal(fd, buf);
}

int LBStat(const char *file, struct stat *buf) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBStatParameter(file)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(buf);
  }
#endif
  return LBStatInternal(file, buf);
}

ssize_t LBPread(int fd, void *buf, uint64_t length, uint64_t offset) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBPreadParameter(fd, length, offset)) {
      return LB_FILE_ERROR;
    }
    return WaitReadIoResult(buf, length);
  }
#endif
  return LBPreadInternal(fd, buf, length, offset);
}

ssize_t LBPwrite(int fd, void *buf, uint64_t length, uint64_t offset) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBPwriteParameter(fd, buf, length, offset)) {
      return LB_FILE_ERROR;
    }
    return WaitWriteIoResult();
  }
#endif
  return LBPwriteInternal(fd, buf, length, offset);
}

int LBFsync(int fd) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBFsyncParameter(fd)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(nullptr);
  }
#endif
  return LBFsyncInternal(fd);
}

int LBMkdir(const char *path, __mode_t mode) {
#ifndef IS_DSTORE_BACKUP_TOOL
  if (rds_dstore_lb_use_io_thread && !TryCreateLocalIoThread()) {
    if (CreateAndPushLBMkdir(path, mode)) {
      return LB_IO_ERROR;
    }
    return WaitCtrlIoResult(nullptr);
  }
#endif
  return LBMkdirInternal(path, mode);
}

void DestoryLbIoContextSet() {
#ifndef IS_DSTORE_BACKUP_TOOL
  std::unique_lock<std::mutex> lock(g_lbIoThrdContextMgr.m_mutex);
  auto iter = g_lbIoThrdContextMgr.m_lbIoContextMap.begin();
  while (iter != g_lbIoThrdContextMgr.m_lbIoContextMap.end()) {
    LBIoThrdContext *context = iter->second;
    assert(nullptr != context);
    if (context->TryKillIoThread()) {
      g_lbIoThrdContextMgr.m_rigidIoContextMap.emplace(
          std::make_pair(iter->first, iter->second));
    } else {
      delete context;
      context = nullptr;
    }
    iter = g_lbIoThrdContextMgr.m_lbIoContextMap.erase(iter);
  }
  g_lbIoThrdContextMgr.m_lbIoErrorRetryNumber.store(0);
#endif
}

void TryClearRigidIoContextSet() {
#ifndef IS_DSTORE_BACKUP_TOOL
  std::unique_lock<std::mutex> lock(g_lbIoThrdContextMgr.m_mutex);
  auto iter = g_lbIoThrdContextMgr.m_rigidIoContextMap.begin();
  while (iter != g_lbIoThrdContextMgr.m_rigidIoContextMap.end()) {
    LBIoThrdContext *context = iter->second;
    if (context->TryKillIoThread()) {
      LogErr(ERROR_LEVEL, ER_LOCAL_BACKUP_ERROR,
             "Try kill one io thread fail during clear rigid io context.");
      iter++;
    } else {
      delete context;
      context = nullptr;
      iter = g_lbIoThrdContextMgr.m_rigidIoContextMap.erase(iter);
    }
  }
#endif
}

}  // namespace CDE
