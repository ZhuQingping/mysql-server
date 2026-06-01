/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include <assert.h>

#include "sql/rpl_wal_mgr.h"
#include "sql/rpl_wal_sender.h"

#include "my_systime.h"
#include "mysql/components/services/log_builtins.h"  // LogErr
#include "mysql/plugin.h"  // MYSQL_STORAGE_ENGINE_PLUGIN
#include "mysql/psi/mysql_mutex.h"
#include "scope_guard.h"    // Scope_guard
#include "sql/item_func.h"  // user_var_entry
#include "sql/log.h"
#include "sql/mysqld.h"  // stage_rpl_wal_wait_flush
#include "sql/protocol_classic.h"
#include "sql/rpl_handler.h"  // RUN_HOOK
#include "sql/rpl_source.h"   // opt_sporadic_binlog_dump_fail
#include "sql/sql_class.h"    // THD
#include "sql/system_variables.h"
#include "sql_string.h"
#include "unsafe_string_append.h"  // qs_append

using namespace std::chrono_literals;

Wal_sender::Wal_sender(THD *thd, uint64_t start_lsn, uint32_t flag,
                       uint64_t wal_stream_id)
    : m_thd(thd),
      m_packet(*thd->get_protocol_classic()->get_output_packet()),
      m_diag_area(false),
      m_flag(flag),
      m_start_lsn(start_lsn),
      m_last_lsn(0),
      m_transmit_started(false),
      m_observe_transmission(false),
      m_errmsg(nullptr),
      m_errno(0),
      m_read_len(0),
      m_cde_ha(nullptr),
      m_master_wal_stream_id(0),
      m_standby_wal_stream_id(wal_stream_id) {}

bool Wal_sender::init() {
  DBUG_TRACE;
  THD *thd = m_thd;

  thd->push_diagnostics_area(&m_diag_area);
  init_heartbeat_period();

  sql_print_information("Wal sender heartbeat period is %lu.",
                        m_heartbeat_period.count());

  const size_t max_header_size = Rpl_wal_msg_utils::get_max_header_size();
  // Initialize the buffer only once.
  m_packet.mem_realloc(MAX_READ_WAL_SIZE + max_header_size +
                       RESERVE_HEADER_SIZE);
  DBUG_PRINT("info", ("Initial rpl_wal packet->alloced_length: %zu.",
                      m_packet.alloced_length()));

  LogErr(INFORMATION_LEVEL, ER_WAL_RPL_WALLOG_STARTING_DUMP, thd->thread_id(),
         thd->server_id, m_start_lsn);

  if (RUN_HOOK(wallog_transmit, wallog_transmit_start,
               (thd, m_flag, m_start_lsn, &m_observe_transmission))) {
    set_unknown_error("Failed to run hook 'wallog_transmit_start'");
    return true;
  }
  m_transmit_started = true;
  thd->variables.max_allowed_packet = MAX_RPL_WAL_ALLOWED_PACKET;
  st_plugin_int *cde_plugin = nullptr;
  {
    LEX_CSTRING engine_name = {STRING_WITH_LEN(DSTORE_ENGINE_NAME)};
    MUTEX_LOCK(plugin_lock, &LOCK_plugin);
    cde_plugin = plugin_find_by_type(engine_name, MYSQL_STORAGE_ENGINE_PLUGIN);
    if (cde_plugin == nullptr) {
      set_unknown_error("Failed to get cde plugin.");
      return true;
    }
  }
  m_cde_ha = (handlerton *)cde_plugin->data;
  DBUG_EXECUTE_IF("wal_sender_cde_plugin_data_null", m_cde_ha = nullptr;);

  if (m_cde_ha) {
    m_master_wal_stream_id = m_cde_ha->get_wal_stream_id(m_thd);
    sql_print_information(
        "Walsender master wal stream id is %lu, "
        "standby wal stream id is %lu.",
        m_master_wal_stream_id, m_standby_wal_stream_id);
  } else {
    set_unknown_error("Failed to get cde plugin data.");
    return true;
  }
  return false;
}

void Wal_sender::cleanup() {
  DBUG_TRACE;

  THD *thd = m_thd;

  if (m_transmit_started) {
    (void)RUN_HOOK(wallog_transmit, wallog_transmit_stop, (thd, m_flag));
  }
  thd->variables.max_allowed_packet =
      global_system_variables.max_allowed_packet;
  thd->pop_diagnostics_area();

  if (has_error()) {
    my_message(m_errno, m_errmsg, MYF(0));
  } else {
    my_eof(thd);
  }
}

void Wal_sender::run() {
  DBUG_TRACE;
  bool init_res = init();
  if (init_res) {
    sql_print_warning("Failed to init wal sender.");
    cleanup();
    return;
  }

  uint64_t master_flushed_lsn = m_cde_ha->get_wal_flushed_lsn(m_thd);
  sql_print_information("Walsender init flushed lsn: %lu.", master_flushed_lsn);
  mysql_wal.init_wal_flushed_lsn(master_flushed_lsn);

  if (check_param_valid(master_flushed_lsn)) {
    sql_print_warning("Check wal sender param is failed.");
    cleanup();
    return;
  }

  uint64_t start_lsn = m_start_lsn;
  uint64_t run_num = 0;
  uint64_t wait_times = 0;
  while (!has_error() && !m_thd->killed) {
    if (flush_net()) {
      break;
    }
    // read wal file
    int read_res = read_wal(start_lsn);
    if (read_res != 0) {
      // read wal error.
      break;
    }

    if (m_read_len == 0) {
      // thd maybe killed, or wal file is recycled.
      run_num++;
      continue;
    }
    if (m_read_len < rds_physical_rpl_sender_packet_size &&
        wait_times < rds_physical_rpl_sender_wait_times) {
      wait_times++;
      std::this_thread::sleep_for(
          std::chrono::microseconds(rds_physical_rpl_sender_sleep_us));
      continue;
    }
    wait_times = 0;

    if (before_send_hook(start_lsn)) {
      break;
    }
    if (send_packet()) {
      break;
    }
    if (after_send_hook()) {
      break;
    }
    start_lsn += m_read_len;
    run_num++;
  }
  sql_print_information("Walsender cleanup, run_num is %lu.", run_num);
  cleanup();
}

void Wal_sender::init_heartbeat_period() {
  /* Protects m_thd->user_vars. */
  mysql_mutex_lock(&m_thd->LOCK_thd_data);

  // Get user_var.
  const auto &uv = get_user_var_from_alternatives(
      m_thd, "source_heartbeat_period", "master_heartbeat_period");
  // Get value of user_var.
  bool null_value;
  m_heartbeat_period =
      std::chrono::nanoseconds(uv ? uv->val_int(&null_value) : 0);
  mysql_mutex_unlock(&m_thd->LOCK_thd_data);
}

int Wal_sender::read_wal(uint64_t start_lsn) {
  m_read_len = 0;
  const size_t RPL_WAL_HEADER_SIZE = sizeof(Rpl_wal_data_msg);
  uint64_t current_flushed_lsn;
  uint64_t wal_stream_id = m_cde_ha->get_wal_stream_id(m_thd);
  while (!has_error() && !m_thd->killed) {
    if (reset_packet()) {
      return 1;
    }
    current_flushed_lsn = mysql_wal.get_wal_flushed_lsn();
    if (start_lsn >= current_flushed_lsn) {
      // need wait wal flush.
      bool wait_res = sleep_and_wait_wal(current_flushed_lsn);
      if (wait_res) {
        set_unknown_error("wait for new wal failed.");
        return 1;
      }
      continue;
    }
    uint64_t max_read_wal_size = current_flushed_lsn - start_lsn;
    DBUG_EXECUTE_IF("wal_sender_too_large_wal",
                    max_read_wal_size = MAX_READ_WAL_SIZE + 1;);
    if (max_read_wal_size > MAX_READ_WAL_SIZE) {
      max_read_wal_size = MAX_READ_WAL_SIZE;
    }
    size_t needed_buffer_size = max_read_wal_size + RPL_WAL_HEADER_SIZE;
    if (needed_buffer_size > m_packet.alloced_length()) {
      char errmsg_buf[MYSQL_ERRMSG_SIZE];
      snprintf(errmsg_buf, MYSQL_ERRMSG_SIZE,
               "read wal needed buffer size error, need size: %lu",
               needed_buffer_size);
      set_unknown_error(errmsg_buf);
      return 1;
    }
    assert(
        (m_observe_transmission && m_packet.length() == RESERVE_HEADER_SIZE) ||
        (m_observe_transmission == false));

    uint32_t header_size =
        m_observe_transmission ? RESERVE_HEADER_SIZE : NO_RESERVE_HEADER_SIZE;
    uchar *header = pointer_cast<uchar *>(m_packet.ptr() + header_size);
    uchar *wal_buffer_ptr = header + RPL_WAL_HEADER_SIZE;

    assert(wal_buffer_ptr != nullptr);
    int read_res =
        m_cde_ha->read_wal(m_thd, wal_stream_id, start_lsn, wal_buffer_ptr,
                           max_read_wal_size, &m_read_len);

    DBUG_EXECUTE_IF("wal_sender_read_wal_data_failed", read_res = -1;);
    if (read_res < 0 || m_read_len == 0) {
      set_unknown_error("Failed to read wal file.");
      return 1;
    }
    assert(m_read_len != 0 && m_read_len <= max_read_wal_size);

    fill_rpl_wal_header(header, start_lsn);
    m_packet.length(m_read_len + RPL_WAL_HEADER_SIZE + header_size);
    break;
  }
  return 0;
}

void Wal_sender::fill_rpl_wal_header(uchar *header, uint64_t start_lsn) {
  assert(m_read_len != 0 && !has_error());
  Rpl_wal_data_msg *msg_header = pointer_cast<Rpl_wal_data_msg *>(header);
  Rpl_wal_msg_utils::init_common_msg_header(
      &(msg_header->common_header), RplWalMsgType::RPL_WAL_DATA_MSG,
      m_thd->server_id, 0, rpl_wal_default_version);
  msg_header->start_lsn = start_lsn;
  msg_header->wal_len = m_read_len;
  msg_header->wal_stream_id = m_master_wal_stream_id;
}

int Wal_sender::before_send_hook(uint64_t log_pos) {
  if (m_observe_transmission &&
      RUN_HOOK(wallog_transmit, wallog_before_send_event,
               (m_thd, m_flag, &m_packet, log_pos))) {
    set_unknown_error("Failed to run hook Wal_sender::before_send_hook.");
    return 1;
  }
  return 0;
}

int Wal_sender::after_send_hook() {
  if (m_observe_transmission &&
      RUN_HOOK(wallog_transmit, wallog_after_send_event,
               (m_thd, m_flag, &m_packet))) {
    set_unknown_error("Failed to run hook Wal_sender::after_send_hook.");
    return 1;
  }

  /*
    semisync wallog_after_send_event hook doesn't return and error
    when net error happens.
  */
  if (m_thd->get_protocol_classic()->get_net()->last_errno != 0) {
    set_unknown_error("Found net error in Wal_sender::after_send_hook.");
    return 1;
  }
  return 0;
}

int Wal_sender::reset_packet(ushort flags) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("rpl wal m_packet->alloced_length: %zu",
                      m_packet.alloced_length()));
  assert(m_packet.alloced_length() ==
         ALIGN_SIZE(MAX_READ_WAL_SIZE +
                    Rpl_wal_msg_utils::get_max_header_size() +
                    RESERVE_HEADER_SIZE));

  m_packet.length(0);
  qs_append('\0', &m_packet);

  /* reserve and set default header */
  if (m_observe_transmission && RUN_HOOK(wallog_transmit, wallog_reserve_header,
                                         (m_thd, flags, &m_packet))) {
    set_unknown_error("Failed to run hook Wal_sender::reset_packet.");
    return 1;
  }
  return 0;
}

int Wal_sender::send_packet() {
  DBUG_TRACE;
  // We should always use the same buffer to guarantee that the reallocation
  // logic is not broken.
  if (DBUG_EVALUATE_IF("simulate_wal_send_error", true,
                       my_net_write(m_thd->get_protocol_classic()->get_net(),
                                    pointer_cast<const uchar *>(m_packet.ptr()),
                                    m_packet.length()))) {
    set_unknown_error("Failed on my_net_write()");
    return 1;
  }

  return 0;
}

inline int Wal_sender::flush_net() {
  if (DBUG_EVALUATE_IF("simulate_flush_error", 1,
                       m_thd->get_protocol()->flush())) {
    set_unknown_error("failed on flush_net()");
    return 1;
  }
  return 0;
}

int Wal_sender::send_heartbeat(uint64_t master_flushed_lsn) {
  DBUG_TRACE;
  if (reset_packet()) {
    return 1;
  }
  assert((m_observe_transmission && m_packet.length() == RESERVE_HEADER_SIZE) ||
         (m_observe_transmission == false));
  const size_t needed_buffer_size = sizeof(Rpl_wal_heartbeat_msg);
  uint32_t header_size =
      m_observe_transmission ? RESERVE_HEADER_SIZE : NO_RESERVE_HEADER_SIZE;
  uchar *header = pointer_cast<uchar *>(m_packet.ptr() + header_size);
  Rpl_wal_heartbeat_msg *msg_header =
      pointer_cast<Rpl_wal_heartbeat_msg *>(header);
  Rpl_wal_msg_utils::init_common_msg_header(
      &(msg_header->common_header), RplWalMsgType::RPL_WAL_HEARTBEAT_MSG,
      m_thd->server_id, 0, rpl_wal_default_version);
  Rpl_wal_msg_utils::fill_heartbeat_msg(msg_header, master_flushed_lsn);

  m_packet.length(needed_buffer_size + header_size);
  if (send_packet() || flush_net()) {
    return 1;
  }
  return 0;
}

bool Wal_sender::sleep_and_wait_wal(uint64_t master_flush_lsn) {
  // wait until wal_writer wakeup.
  assert(mysql_wal.is_inited());
  bool ret = false;

  PSI_stage_info old_stage;
  mysql_wal.lock_wal_flush_lsn();

  m_thd->ENTER_COND(mysql_wal.get_wal_update_cond(),
                    mysql_wal.get_wal_flush_lsn_lock(),
                    &stage_rpl_wal_wait_flush, &old_stage);

  if (m_heartbeat_period.count() > 0) {
    ret = wait_with_heartbeat(master_flush_lsn);
  } else {
    wait_without_heartbeat(master_flush_lsn);
  }

  mysql_wal.unlock_wal_flush_lsn();
  m_thd->EXIT_COND(&old_stage);

  return ret;
}

bool Wal_sender::stop_waiting_for_update_wal(uint64_t master_flush_lsn) const {
  if (mysql_wal.get_wal_flushed_lsn() > master_flush_lsn || m_thd->killed) {
    return true;
  }
  return false;
}

void Wal_sender::wait_without_heartbeat(uint64_t master_flush_lsn) {
  while (!stop_waiting_for_update_wal(master_flush_lsn)) {
    mysql_wal.wait_for_update_wal();
  }
}

bool Wal_sender::wait_with_heartbeat(uint64_t master_flush_lsn) {
#ifndef NDEBUG
  ulong hb_info_counter = 0;
#endif

  while (!stop_waiting_for_update_wal(master_flush_lsn)) {
    // ignoring timeout on conditional variable
    mysql_wal.wait_for_update_wal(m_heartbeat_period);

    if (stop_waiting_for_update_wal(master_flush_lsn)) {
      return false;
    }
    mysql_wal.unlock_wal_flush_lsn();
    Scope_guard lock([]() { mysql_wal.lock_wal_flush_lsn(); });
#ifndef NDEBUG
    if (hb_info_counter < 3) {
      LogErr(INFORMATION_LEVEL, ER_WAL_RPL_WALLOG_SOURCE_SENDS_HEARTBEAT);
      hb_info_counter++;
      if (hb_info_counter == 3)
        LogErr(INFORMATION_LEVEL,
               ER_WAL_RPL_WALLOG_SKIPPING_REMAINING_HEARTBEAT_INFO);
    }
#endif
    if (send_heartbeat(master_flush_lsn)) {
      return true;
    }
  }

  return false;
}

bool Wal_sender::check_param_valid(uint64_t master_flushed_lsn) {
  DBUG_EXECUTE_IF("wal_sender_stream_id_diff",
                  m_master_wal_stream_id = m_standby_wal_stream_id + 1;);
  char errmsg_buf[MYSQL_ERRMSG_SIZE];

  if (m_master_wal_stream_id != m_standby_wal_stream_id) {
    snprintf(errmsg_buf, MYSQL_ERRMSG_SIZE,
             "The wal stream id of the master and standby are different, "
             "master: %lu, standby: %lu, standby server id: %u",
             m_master_wal_stream_id, m_standby_wal_stream_id, m_thd->server_id);
    set_unknown_error(errmsg_buf);
    return true;
  }
  uint64_t actual_read_size = 0;
  uint32_t header_size =
      m_observe_transmission ? RESERVE_HEADER_SIZE : NO_RESERVE_HEADER_SIZE;
  uchar *header = pointer_cast<uchar *>(m_packet.ptr() + header_size);
  uchar *wal_buffer_ptr = header + sizeof(Rpl_wal_data_msg);
  // Check whether wal data has been recycled, so only 1 byte needs to be read.
  const size_t max_read_len = 1;
  DBUG_EXECUTE_IF("wal_sender_master_flush_lsn_error",
                  m_start_lsn = master_flushed_lsn + 1;);
  if (master_flushed_lsn < m_start_lsn) {
    snprintf(errmsg_buf, MYSQL_ERRMSG_SIZE,
             "The flushed lsn of the master is smaller than standby, "
             "master: %lu, standby: %lu, standby server id: %u.",
             master_flushed_lsn, m_start_lsn, m_thd->server_id);
    set_unknown_error(errmsg_buf);
    return true;
  }
  DBUG_EXECUTE_IF("wal_sender_wal_recycled",
                  master_flushed_lsn = m_start_lsn + 1;);
  if (master_flushed_lsn != m_start_lsn) {
    int read_res =
        m_cde_ha->read_wal(m_thd, m_master_wal_stream_id, m_start_lsn,
                           wal_buffer_ptr, max_read_len, &actual_read_size);
    if (read_res != 0 || actual_read_size == 0) {
      snprintf(errmsg_buf, MYSQL_ERRMSG_SIZE,
               "Wal data has been recycled and cannot be sent, lsn: "
               "%lu.",
               m_start_lsn);
      set_unknown_error(errmsg_buf);
      return true;
    }
  }

  return false;
}
