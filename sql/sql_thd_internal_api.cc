/* Copyright (c) 2015, 2024, Oracle and/or its affiliates.

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

#include "sql/sql_thd_internal_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "my_config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(HAVE_SYS_RESOURCE_H)
#include <sys/resource.h>
#endif

#include "m_string.h"
#include "mysql/components/services/bits/psi_stage_bits.h"
#include "pfs_thread_provider.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_socket.h"
#include "mysql/thread_type.h"
#include "sql/binlog.h"       // mysql_bin_log
#include "sql/current_thd.h"  // current_thd
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/protocol_classic.h"
#include "sql/query_options.h"
#include "sql/rpl_filter.h"  // binlog_filter
#include "sql/sql_class.h"   // THD
#include "sql/sql_lex.h"
#include "sql/sql_parse.h"  // sqlcom_can_generate_row_events
#include "sql/system_variables.h"
#include "sql/transaction_info.h"
#include "violite.h"

struct mysql_cond_t;
struct mysql_mutex_t;

namespace {

/**
  Read CPU quota from cgroup v2 cpu.max.
  @retval true   quota detected in @c cpus
  @retval false  unlimited/invalid/unavailable
*/
bool read_cgroup_v2_cpu_quota(double *cpus) {
  FILE *f = std::fopen("/sys/fs/cgroup/cpu.max", "r");
  if (f == nullptr) return false;

  char quota[32] = {0};
  long long period = 0;
  const int n = std::fscanf(f, "%31s %lld", quota, &period);
  std::fclose(f);

  if (n != 2 || period <= 0) return false;
  if (std::strcmp(quota, "max") == 0) return false;

  char *end = nullptr;
  const long long q = std::strtoll(quota, &end, 10);
  if (end == quota || *end != '\0' || q <= 0) return false;

  *cpus = static_cast<double>(q) / static_cast<double>(period);
  return *cpus > 0.0;
}

/**
  Read CPU quota from common cgroup v1 locations.
  @retval true   quota detected in @c cpus
  @retval false  unlimited/invalid/unavailable
*/
bool read_cgroup_v1_cpu_quota(double *cpus) {
  const char *quota_paths[] = {"/sys/fs/cgroup/cpu/cpu.cfs_quota_us",
                               "/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us"};
  const char *period_paths[] = {"/sys/fs/cgroup/cpu/cpu.cfs_period_us",
                                "/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us"};

  for (size_t i = 0; i < sizeof(quota_paths) / sizeof(quota_paths[0]); ++i) {
    FILE *fq = std::fopen(quota_paths[i], "r");
    FILE *fp = std::fopen(period_paths[i], "r");
    if (fq == nullptr || fp == nullptr) {
      if (fq != nullptr) std::fclose(fq);
      if (fp != nullptr) std::fclose(fp);
      continue;
    }

    long long quota = -1;
    long long period = 0;
    const int nq = std::fscanf(fq, "%lld", &quota);
    const int np = std::fscanf(fp, "%lld", &period);
    std::fclose(fq);
    std::fclose(fp);

    if (nq != 1 || np != 1 || period <= 0 || quota <= 0) continue;

    *cpus = static_cast<double>(quota) / static_cast<double>(period);
    if (*cpus > 0.0) return true;
  }

  return false;
}

/**
  Compute effective CPU capacity for the current process.
  It honors both CPU affinity and cgroup CPU quota (if set), returning
  the smaller capacity to avoid underestimating usage in containers.
*/
double effective_cpu_capacity() {
  double affinity_cpus = 1.0;
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  if (sched_getaffinity(0, sizeof(cpu_set), &cpu_set) == 0) {
    const int n = CPU_COUNT(&cpu_set);
    if (n > 0) affinity_cpus = static_cast<double>(n);
  } else {
    const auto hw = std::thread::hardware_concurrency();
    if (hw > 0) affinity_cpus = static_cast<double>(hw);
  }

  double quota_cpus = 0.0;
  if (read_cgroup_v2_cpu_quota(&quota_cpus) ||
      read_cgroup_v1_cpu_quota(&quota_cpus)) {
    return std::max(0.001, std::min(affinity_cpus, quota_cpus));
  }

  return std::max(0.001, affinity_cpus);
}

}  // namespace

THD *create_internal_thd() {
  /* For internal threads, use enabled_plugins = false. */
  THD *thd = new THD(false);
  thd->system_thread = SYSTEM_THREAD_BACKGROUND;
  // Skip grants and set the system_user flag in THD.
  thd->security_context()->skip_grants();
  thd->thread_stack = reinterpret_cast<char *>(&thd);
  thd->store_globals();

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *psi;
  psi = PSI_THREAD_CALL(get_thread)();
  if (psi != nullptr) {
    /*
      Associate this THD to the background thread instrumentation,
      so that system variables and status variables
      are visible for the background thread.
    */
    PSI_THREAD_CALL(set_thread_THD)(psi, thd);
    thd->set_psi(psi);
  }
#endif /* HAVE_PSI_THREAD_INTERFACE */

  return thd;
}

void destroy_internal_thd(THD *thd) {
  assert(thd->system_thread == SYSTEM_THREAD_BACKGROUND);

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *psi;
  psi = PSI_THREAD_CALL(get_thread)();
  if (psi != nullptr) {
    /*
      Dissociate this THD from the background thread instrumentation.
    */
    PSI_THREAD_CALL(set_thread_THD)(psi, nullptr);
    thd->set_psi(nullptr);
  }
#endif /* HAVE_PSI_THREAD_INTERFACE */

  thd->release_resources();
  delete thd;
}

void thd_init(THD *thd, char *stack_start) {
  DBUG_TRACE;
  // TODO: Purge threads currently terminate too late for them to be added.
  // Note that P_S interprets all threads with thread_id != 0 as
  // foreground threads. And THDs need thread_id != 0 to be added
  // to the global THD list.
  if (thd->system_thread != SYSTEM_THREAD_BACKGROUND) {
    thd->set_new_thread_id();
    Global_THD_manager *thd_manager = Global_THD_manager::get_instance();
    thd_manager->add_thd(thd);
  }

  if (!thd->system_thread) {
    DBUG_PRINT("info",
               ("init new connection. thd: %p fd: %d", thd,
                mysql_socket_getfd(
                    thd->get_protocol_classic()->get_vio()->mysql_socket)));
  }
  thd_set_thread_stack(thd, stack_start);

  thd->store_globals();
}

void thd_init(THD *thd, char *stack_start, bool bound [[maybe_unused]],
              PSI_thread_key psi_key [[maybe_unused]],
              unsigned int psi_seqnum [[maybe_unused]]) {
  DBUG_TRACE;

  thd_init(thd, stack_start);

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *psi;
  psi = PSI_THREAD_CALL(new_thread)(psi_key, psi_seqnum, thd, thd->thread_id());
  if (bound) {
    PSI_THREAD_CALL(set_thread_os_id)(psi);
  }
  PSI_THREAD_CALL(set_thread_THD)(psi, thd);
  thd->set_psi(psi);
#endif /* HAVE_PSI_THREAD_INTERFACE */
}

THD *create_thd(bool enable_plugins, bool background_thread, bool bound,
                PSI_thread_key psi_key, unsigned int psi_seqnum) {
  THD *thd = new THD(enable_plugins);
  if (background_thread) {
    thd->system_thread = SYSTEM_THREAD_BACKGROUND;
    // Skip grants and set the system_user flag in THD.
    thd->security_context()->skip_grants();
  }
  (void)thd_init(thd, reinterpret_cast<char *>(&thd), bound, psi_key,
                 psi_seqnum);
  return thd;
}

void destroy_thd(THD *thd, bool clear_pfs_events [[maybe_unused]]) {
  thd->release_resources();
#ifdef HAVE_PSI_THREAD_INTERFACE
  if (clear_pfs_events) PSI_THREAD_CALL(delete_thread)(thd->get_psi());
  thd->set_psi(nullptr);
#endif /* HAVE_PSI_THREAD_INTERFACE */

  // TODO: Purge threads currently terminate too late for them to be added.
  if (thd->system_thread != SYSTEM_THREAD_BACKGROUND) {
    Global_THD_manager *thd_manager = Global_THD_manager::get_instance();
    thd_manager->remove_thd(thd);
  }
  delete thd;
}

void destroy_thd(THD *thd) { return destroy_thd(thd, true); }

void thd_set_thread_stack(THD *thd, const char *stack_start) {
  thd->thread_stack = stack_start;
}

extern "C" void thd_enter_cond(void *opaque_thd, mysql_cond_t *cond,
                               mysql_mutex_t *mutex,
                               const PSI_stage_info *stage,
                               PSI_stage_info *old_stage,
                               const char *src_function, const char *src_file,
                               int src_line) {
  THD *thd = static_cast<THD *>(opaque_thd);
  if (!thd) thd = current_thd;

  return thd->enter_cond(cond, mutex, stage, old_stage, src_function, src_file,
                         src_line);
}

extern "C" void thd_exit_cond(void *opaque_thd, const PSI_stage_info *stage,
                              const char *src_function, const char *src_file,
                              int src_line) {
  THD *thd = static_cast<THD *>(opaque_thd);
  if (!thd) thd = current_thd;

  thd->exit_cond(stage, src_function, src_file, src_line);
}

extern "C" void thd_enter_stage(void *opaque_thd,
                                const PSI_stage_info *new_stage,
                                PSI_stage_info *old_stage,
                                const char *src_function, const char *src_file,
                                int src_line) {
  THD *thd = static_cast<THD *>(opaque_thd);
  if (!thd) thd = current_thd;

  thd->enter_stage(new_stage, old_stage, src_function, src_file, src_line);
}

extern "C" void thd_set_waiting_for_disk_space(void *opaque_thd,
                                               const bool waiting) {
  THD *thd = static_cast<THD *>(opaque_thd);
  if (!thd) thd = current_thd;

  thd->set_waiting_for_disk_space(waiting);
}

void thd_increment_bytes_sent(size_t length) {
  THD *thd = current_thd;
  if (likely(thd != nullptr)) { /* current_thd==NULL when close_connection()
                                calls net_send_error() */
    thd->status_var.bytes_sent += length;
  }
}

void thd_increment_bytes_received(size_t length) {
  THD *thd = current_thd;
  if (likely(thd != nullptr)) thd->status_var.bytes_received += length;
}

partition_info *thd_get_work_part_info(THD *thd) { return thd->work_part_info; }

enum_tx_isolation thd_get_trx_isolation(const THD *thd) {
  return thd->tx_isolation;
}

const CHARSET_INFO *thd_charset(THD *thd) { return (thd->charset()); }

LEX_CSTRING thd_query_unsafe(THD *thd) {
  assert(current_thd == thd);
  return thd->query();
}

size_t thd_query_safe(THD *thd, char *buf, size_t buflen) {
  mysql_mutex_lock(&thd->LOCK_thd_query);
  LEX_CSTRING query_string = thd->query();
  size_t len = std::min(buflen - 1, query_string.length);
  if (len > 0) strncpy(buf, query_string.str, len);
  buf[len] = '\0';
  mysql_mutex_unlock(&thd->LOCK_thd_query);
  return len;
}

int thd_slave_thread(const THD *thd) { return (thd->slave_thread); }

int thd_non_transactional_update(const THD *thd) {
  return thd->get_transaction()->has_modified_non_trans_table(
      Transaction_ctx::SESSION);
}

int thd_binlog_format(const THD *thd) {
  if (mysql_bin_log.is_open() && (thd->variables.option_bits & OPTION_BIN_LOG))
    return (int)thd->variables.binlog_format;
  else
    return BINLOG_FORMAT_UNSPEC;
}

bool thd_binlog_filter_ok(const THD *thd) {
  return binlog_filter->db_ok(thd->db().str);
}

bool thd_sqlcom_can_generate_row_events(const THD *thd) {
  return sqlcom_can_generate_row_events(thd->lex->sql_command);
}

enum durability_properties thd_get_durability_property(const THD *thd) {
  enum durability_properties ret = HA_REGULAR_DURABILITY;

  if (thd != nullptr) ret = thd->durability_property;

  return ret;
}

void thd_get_autoinc(const THD *thd, ulong *off, ulong *inc) {
  *off = thd->variables.auto_increment_offset;
  *inc = thd->variables.auto_increment_increment;
}

size_t thd_get_tmp_table_size(const THD *thd) {
  // We are intentionally narrowing the unsigned long long int (type of
  // thd->variables.tmp_table_size) to size_t here. Issue with the former is
  // that it represents more memory than one can address, in particular this is
  // the case with 32-bit builds because unsigned long long int is guaranteed
  // to be _at least_ 64 bits wide. That is much larger than the available
  // address space.
  //
  // Given that tmp_table_size sysvar is about limiting the consumed (virtual)
  // memory, size_t is the type which actually only makes sense to use here as
  // it represents exactly the theoretical maximum sized object
  if (thd->variables.tmp_table_size < std::numeric_limits<size_t>::max()) {
    return thd->variables.tmp_table_size;
  } else {
    return std::numeric_limits<size_t>::max();
  }
}

bool thd_is_strict_mode(const THD *thd) { return thd->is_strict_mode(); }

bool thd_is_error(const THD *thd) { return thd->is_error(); }

bool is_mysql_datadir_path(const char *path) {
  if (path == nullptr || strlen(path) >= FN_REFLEN) return false;

  char mysql_data_dir[FN_REFLEN], path_dir[FN_REFLEN];
  convert_dirname(path_dir, path, NullS);
  convert_dirname(mysql_data_dir, mysql_unpacked_real_data_home, NullS);
  size_t mysql_data_home_len = dirname_length(mysql_data_dir);
  size_t path_len = dirname_length(path_dir);

  if (path_len < mysql_data_home_len) return true;

  if (!lower_case_file_system)
    return memcmp(mysql_data_dir, path_dir, mysql_data_home_len);

  return files_charset_info->coll->strnncoll(
      files_charset_info, reinterpret_cast<uchar *>(path_dir), path_len,
      reinterpret_cast<uchar *>(mysql_data_dir), mysql_data_home_len, true);
}

int mysql_tmpfile_path(const char *path, const char *prefix) {
  assert(path != nullptr);
  assert((strlen(path) + strlen(prefix)) <= FN_REFLEN);

  char filename[FN_REFLEN];
  int mode = O_CREAT | O_EXCL | O_RDWR;
#ifdef _WIN32
  mode |= O_TRUNC | O_SEQUENTIAL;
#endif
  File fd = mysql_file_create_temp(PSI_NOT_INSTRUMENTED, filename, path, prefix,
                                   mode, UNLINK_FILE, MYF(MY_WME));
  return fd;
}

bool thd_is_bootstrap_thread(THD *thd) {
  assert(thd);
  return (thd->is_bootstrap_system_thread() &&
          !thd->is_init_file_system_thread());
}

bool thd_is_dd_update_stmt(const THD *thd) {
  assert(thd != nullptr);

  /*
    OPTION_DD_UPDATE_CONTEXT flag is set when thread switches context to
    update data dictionary tables for the
      * DDL statements.
      * Administration statements as ANALYZE TABLE.
      * Event threads for next activation time of a event and to update status.
      * SDI import.
      ...
    So verifying OPTION_DD_UPDATE_CONTEXT flag value to check if thread is
    updating the data dictionary tables.
  */
  return (thd->variables.option_bits & OPTION_DD_UPDATE_CONTEXT);
}

my_thread_id thd_thread_id(const THD *thd) { return (thd->thread_id()); }

std::chrono::seconds thd_auto_stats_recalc_interval() {
  std::chrono::seconds fast_interval{10};
  std::chrono::seconds slow_interval{30};
  constexpr std::chrono::seconds kRefresh_interval{1};
  constexpr double kCpu_load_threshold_pct = 75.0;
  DBUG_EXECUTE_IF("thd_auto_stats_interval_test_short", {
    fast_interval = std::chrono::seconds{1};
    slow_interval = std::chrono::seconds{3};
  };);

  // This helper is currently used by a single background stats thread.
  // Static state keeps the implementation lightweight without extra locks.
  // The state stores:
  // - last refresh timestamp (to cap sampling frequency),
  // - CPU usage sampling points (for delta-based CPU utilization),
  // - current interval mode.
  struct State {
    bool use_slow_interval{false};
    std::chrono::seconds cached_interval{10};
    std::chrono::steady_clock::time_point last_refresh{};
    std::chrono::steady_clock::time_point last_cpu_sample{};
    double last_cpu_total_us{0.0};
    bool has_cpu_sample{false};
#ifndef NDEBUG
    bool owner_thread_initialized{false};
    my_thread_t owner_thread{};
#endif
  };
  static State state;

#ifndef NDEBUG
  // Debug safety check: this function is expected to be called by a single
  // thread. If another thread enters, fail fast in debug builds.
  if (!state.owner_thread_initialized) {
    state.owner_thread = my_thread_self();
    state.owner_thread_initialized = true;
  } else {
    assert(my_thread_equal(state.owner_thread, my_thread_self()));
  }
#endif

  const auto now = std::chrono::steady_clock::now();
  // Sampling is intentionally rate-limited to once per second. Between two
  // refresh points we return the last computed interval.
  if (state.last_refresh != std::chrono::steady_clock::time_point{} &&
      now - state.last_refresh < kRefresh_interval) {
    return state.cached_interval;
  }
  state.last_refresh = now;

  rusage usage;
  // On sampling failure, choose a conservative interval to avoid creating
  // extra work under uncertain conditions.
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    state.use_slow_interval = false;
    state.cached_interval = fast_interval;
    return state.cached_interval;
  }

  const double cpu_total_us =
      static_cast<double>(usage.ru_utime.tv_sec) * 1000000.0 +
      static_cast<double>(usage.ru_utime.tv_usec) +
      static_cast<double>(usage.ru_stime.tv_sec) * 1000000.0 +
      static_cast<double>(usage.ru_stime.tv_usec);

  if (!state.has_cpu_sample) {
    // First sample initializes the baseline and keeps conservative behavior
    // until we have enough data to compute a delta.
    state.last_cpu_total_us = cpu_total_us;
    state.last_cpu_sample = now;
    state.has_cpu_sample = true;
    state.use_slow_interval = false;
    state.cached_interval = fast_interval;
    return state.cached_interval;
  }

  const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              now - state.last_cpu_sample)
                              .count();
  if (elapsed_us <= 0) {
    return state.cached_interval;
  }

  const double cpu_delta_us =
      std::max(0.0, cpu_total_us - state.last_cpu_total_us);
  state.last_cpu_total_us = cpu_total_us;
  state.last_cpu_sample = now;

  // Normalize by effective CPU capacity (affinity and cgroup quota aware).
  const double cpu_capacity = effective_cpu_capacity();

  double cpu_usage_pct =
      cpu_delta_us * 100.0 / static_cast<double>(elapsed_us) / cpu_capacity;
  cpu_usage_pct = std::clamp(cpu_usage_pct, 0.0, 100.0);
  DBUG_EXECUTE_IF("thd_auto_stats_interval_force_high_cpu", cpu_usage_pct = 100.0;);
  DBUG_EXECUTE_IF("thd_auto_stats_interval_force_low_cpu", cpu_usage_pct = 0.0;);

  // Single-threshold mode:
  // high load (>= threshold) -> slow interval, otherwise fast interval.
  state.use_slow_interval = (cpu_usage_pct >= kCpu_load_threshold_pct);

  // Low CPU load -> faster stats refresh cadence.
  // High CPU load -> slower cadence to reduce background CPU pressure.
  state.cached_interval =
      state.use_slow_interval ? slow_interval : fast_interval;
  return state.cached_interval;
}
