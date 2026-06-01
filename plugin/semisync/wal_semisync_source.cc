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

#include "plugin/semisync/wal_semisync_source.h"
#include "sql/mysqld.h"  // max_connections
#include "sql/rpl_wal_common.h"

constexpr uint32_t TIME_SEC_TO_MS = 1000;
constexpr uint32_t TIME_SEC_TO_US = 1000000;
constexpr uint32_t TIME_SEC_TO_NS = 1000000000;

/* System and status variables for the master component. */
bool rpl_semi_sync_source_enabled_wal;
unsigned long rpl_semi_sync_source_timeout_wal;
unsigned long rpl_semi_sync_source_trace_level_wal;
char rpl_semi_sync_source_status_wal = 0;
unsigned long rpl_semi_sync_source_off_times_wal = 0;
unsigned long rpl_semi_sync_source_wait_timeouts_wal = 0;
unsigned long long rpl_semi_sync_source_net_wait_num_wal = 0;
unsigned long rpl_semi_sync_source_clients_wal = 0;
unsigned long long rpl_semi_sync_source_trx_wait_time_wal = 0;
bool rpl_semi_sync_source_wait_no_replica_wal = true;
unsigned int rpl_semi_sync_source_wait_for_replica_count_wal = 1;
unsigned long rpl_semi_sync_source_yes_transactions_wal = 0;
unsigned long rpl_semi_sync_source_no_transactions_wal = 0;
unsigned long rpl_semi_sync_source_wait_sessions_wal = 0;
unsigned long rpl_semi_sync_source_avg_net_wait_time_wal = 0;
unsigned long rpl_semi_sync_source_avg_trx_wait_time_wal = 0;
unsigned long long rpl_semi_sync_source_net_wait_time_wal = 0;
unsigned long long rpl_semi_sync_source_trx_wait_num_wal = 0;
unsigned long rpl_semi_sync_source_timefunc_fails_wal = 0;
unsigned long rpl_semi_sync_source_wait_pos_backtraverse_wal = 0;

static int get_wait_time(const struct timespec &start_ts);

static uint64_t timespec_to_usec(const struct timespec *ts) {
  return (uint64_t)ts->tv_sec * TIME_SEC_TO_US + ts->tv_nsec / TIME_SEC_TO_MS;
}

int Wal_repl_semi_sync_master::wait_standby_flush(uint64_t target_lsn) {
  const char *cdekWho = "Wal_repl_semi_sync_master::wait_standby_flush";
  function_enter(cdekWho);
  wal_commit_trx(target_lsn);
  return function_exit(cdekWho, 0);
}

void Wal_repl_semi_sync_master::get_standby_flushed_lsn(
    uint64_t *standby_flushed_lsn) {
  lock();
  if (is_on() && received_standby_lsn_inited) {
    *standby_flushed_lsn = received_standby_lsn;
  } else {
    *standby_flushed_lsn = UINT64_MAX;
  }
  unlock();
}

int Wal_repl_semi_sync_master::report_reply_wal_packet(uint32_t server_id,
                                                       const uchar *packet,
                                                       ulong packet_len
                                                       [[maybe_unused]]) {
  const char *kWho = "Wal_repl_semi_sync_master::report_reply_wal_packet";
  int result = -1;

  function_enter(kWho);
  if (unlikely(packet_len != sizeof(Rpl_wal_ack_msg))) {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_INVALID_REPLY_PKT_LEN, packet_len);
    return function_exit(kWho, result);
  }
  Rpl_wal_ack_msg msg{};
  msg.deserialize(packet);
  if (unlikely(msg.common_header.type != RplWalMsgType::RPL_WAL_ACK_MSG) ||
      DBUG_EVALUATE_IF("reply_header_type_error", 1, 0)) {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_INVALID_REPLY_PKT_TYPE);
    return function_exit(kWho, result);
  }
  uint64_t m_flushed_lsn = msg.flushed_lsn;
  if (trace_level_ & kTraceDetail) {
    LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SERVER_REPLY, kWho, m_flushed_lsn,
           server_id);
  }

  handle_wal_ack(server_id, m_flushed_lsn);

  return function_exit(kWho, result);
}

Wal_repl_semi_sync_master::Wal_repl_semi_sync_master() {}

int Wal_repl_semi_sync_master::init_object() {
  const char *kWho = "Wal_repl_semi_sync_master::init_object";
  int result;

  if (init_done) {
    LogErr(WARNING_LEVEL, ER_WAL_RPL_FUNCTION_CALLED_TWICE, kWho);
    return 1;
  }
  init_done = true;

  set_wait_timeout(rpl_semi_sync_source_timeout_wal);
  set_trace_level(rpl_semi_sync_source_trace_level_wal);

  /* Mutex initialization can only be done after MY_INIT(). */
  mysql_mutex_init(key_ss_mutex_LOCK_wallog_, &LOCK_wallog_,
                   MY_MUTEX_INIT_FAST);

  if (rpl_semi_sync_source_enabled_wal)
    result = enable_master();
  else
    result = disable_master();

  return result;
}

int Wal_repl_semi_sync_master::enable_master() {
  int result = 0;
  lock();

  if (!get_master_enabled()) {
    if (lsnWaitSlot == nullptr) {
      lsnWaitSlot = (LsnWaitSlot *)my_malloc(
          key_ss_memory_LsnWaitSlot, sizeof(LsnWaitSlot) * WAIT_SLOTS_ARRAY_LEN,
          MYF(0));
    }

    if (lsnWaitSlot != nullptr) {
      for (int i = 0; i < WAIT_SLOTS_ARRAY_LEN; i++)
        mysql_cond_init(key_ss_cond_COND_wallog_send_, &lsnWaitSlot[i].cond);
      max_trx_lsn_inited = false;
      received_standby_lsn_inited = false;
      notify_start_lsn_inited = false;
      set_master_enabled(true);
      /**
        state_ will be set off when users don't want to wait(
        rpl_semi_sync_source_wait_no_replica_wal == 0) if there is no enough
        active semisync clients.
      */
      state_ = (rpl_semi_sync_source_wait_no_replica_wal != 0 ||
                (rpl_semi_sync_source_clients_wal >=
                 rpl_semi_sync_source_wait_for_replica_count_wal));
      LogErr(INFORMATION_LEVEL, ER_WAL_RPL_ENABLED_ON_SOURCE);
    } else {
      LogErr(ERROR_LEVEL, ER_WAL_RPL_SOURCE_OOM);
      result = -1;
    }
  }

  unlock();
  return result;
}

int Wal_repl_semi_sync_master::disable_master() {
  lock();

  if (get_master_enabled()) {
    /**
      Switch off the semi-sync first so that waiting transaction will be
      waken up.
     */
    switch_off();

    if (lsnWaitSlot) {
      for (int i = 0; i < WAIT_SLOTS_ARRAY_LEN; i++)
        mysql_cond_destroy(&lsnWaitSlot[i].cond);
      my_free(lsnWaitSlot);
      lsnWaitSlot = nullptr;
    }
    received_standby_lsn_inited = false;
    notify_start_lsn_inited = false;
    max_trx_lsn_inited = false;
    set_master_enabled(false);
    LogErr(INFORMATION_LEVEL, ER_WAL_RPL_DISABLED_ON_SOURCE);
  }

  unlock();
  return 0;
}

Wal_repl_semi_sync_master::~Wal_repl_semi_sync_master() {
  if (init_done) {
    mysql_mutex_destroy(&LOCK_wallog_);
  }
  if (lsnWaitSlot) {
    for (int i = 0; i < WAIT_SLOTS_ARRAY_LEN; i++)
      mysql_cond_destroy(&lsnWaitSlot[i].cond);
    my_free(lsnWaitSlot);
    lsnWaitSlot = nullptr;
  }
}

void Wal_repl_semi_sync_master::lock() { mysql_mutex_lock(&LOCK_wallog_); }

void Wal_repl_semi_sync_master::unlock() { mysql_mutex_unlock(&LOCK_wallog_); }

void Wal_repl_semi_sync_master::add_slave() {
  lock();
  rpl_semi_sync_source_clients_wal++;
  unlock();
}

void Wal_repl_semi_sync_master::remove_slave() {
  lock();
  rpl_semi_sync_source_clients_wal--;
  /* Only switch off if semi-sync is enabled and is on */
  if (get_master_enabled() && is_on()) {
    /**
      If user has chosen not to wait if no enough semi-sync slave available
      and after a slave exists, turn off semi-semi master immediately if active
      slaves are less then required slave numbers.
    */
    if ((rpl_semi_sync_source_clients_wal ==
         rpl_semi_sync_source_wait_for_replica_count_wal - 1) &&
        (!rpl_semi_sync_source_wait_no_replica_wal ||
         connection_events_loop_aborted())) {
      /**
        If there is a forced shutdown and there are transactions in a waiting
        state on the master.
      */
      if (connection_events_loop_aborted()) {
        if (max_trx_lsn_inited && received_standby_lsn_inited &&
            received_standby_lsn < max_trx_lsn) {
          LogErr(WARNING_LEVEL, ER_WAL_RPL_FORCED_SHUTDOWN);
        }
      }
      switch_off();
    }
  }
  unlock();
}

void Wal_repl_semi_sync_master::report_reply_wal(uint64_t standby_flushed_lsn) {
  const char *kWho = "Wal_repl_semi_sync_master::report_reply_wal";
  function_enter(kWho);

  mysql_mutex_assert_owner(&LOCK_wallog_);
  if (!get_master_enabled()) {
    function_exit(kWho, 0);
    return;
  }
  if (!is_on()) try_switch_on(standby_flushed_lsn);

  if (!received_standby_lsn_inited ||
      standby_flushed_lsn > received_standby_lsn) {
    received_standby_lsn = standby_flushed_lsn;
    received_standby_lsn_inited = true;
    if (trace_level_ & kTraceDetail)
      LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SOURCE_GOT_REPLY_AT_POS, kWho,
             received_standby_lsn);
  }

  if (rpl_semi_sync_source_wait_sessions_wal > 0 &&
      received_standby_lsn >= notify_start_lsn) {
    if (trace_level_ & kTraceDetail)
      LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SOURCE_SIGNAL_ALL_WAITING_THREADS,
             kWho, notify_start_lsn, received_standby_lsn);
    notify_wait_trx(notify_start_lsn, received_standby_lsn);
    notify_start_lsn = received_standby_lsn;
  }

  function_exit(kWho, 0);
}

void Wal_repl_semi_sync_master::notify_wait_trx(uint64_t start_lsn,
                                                uint64_t end_lsn) {
  int start_slot = compute_wait_slot_no(start_lsn);
  int end_slot = compute_wait_slot_no(end_lsn);

  int total_slots =
      (end_slot - start_slot + 1 + WAIT_SLOTS_ARRAY_LEN) % WAIT_SLOTS_ARRAY_LEN;

  for (int i = 0; i < total_slots; ++i) {
    int current_slot = (start_slot + i) % WAIT_SLOTS_ARRAY_LEN;
    mysql_cond_broadcast(&lsnWaitSlot[current_slot].cond);
  }
}

int Wal_repl_semi_sync_master::compute_wait_slot_no(uint64_t lsn) {
  return ((lsn - 1) / WAIT_SLOTS_BLOCK_SIZE) & (WAIT_SLOTS_ARRAY_LEN - 1);
}

int Wal_repl_semi_sync_master::wal_commit_trx(uint64_t target_lsn) {
  const char *kWho = "Wal_repl_semi_sync_master::wal_commit_trx";
  function_enter(kWho);

  lock();
  if (!get_master_enabled()) {
    unlock();
    return function_exit(kWho, 0);
  }

  if (!max_trx_lsn_inited) {
    max_trx_lsn = target_lsn;
    max_trx_lsn_inited = true;
  } else if (target_lsn > max_trx_lsn) {
    max_trx_lsn = target_lsn;
  }

  if (!received_standby_lsn_inited) {
    unlock();
    return function_exit(kWho, 0);
  }

  if (trace_level_ & kTraceDetail)
    LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SOURCE_TRX_WAIT_POS, kWho, target_lsn,
           (int)is_on());

  struct timespec start_ts;
  struct timespec abstime;
  int wait_result;
  int slot_index = 0;

  while (is_on()) {
    if (received_standby_lsn_inited && received_standby_lsn >= target_lsn) {
      if (trace_level_ & kTraceDetail)
        LogErr(INFORMATION_LEVEL, ER_WAL_RPL_WALLOG_REPLY_IS_AHEAD, kWho,
               received_standby_lsn);
      break;
    }

    if (!notify_start_lsn_inited) {
      notify_start_lsn = target_lsn;
      notify_start_lsn_inited = true;
    } else if (target_lsn < notify_start_lsn ||
               DBUG_EVALUATE_IF("test_wait_pos_backtraverse_wal", 1, 0)) {
      notify_start_lsn = target_lsn;
      rpl_semi_sync_source_wait_pos_backtraverse_wal++;
      if (trace_level_ & kTraceDetail)
        LogErr(INFORMATION_LEVEL, ER_WAL_RPL_MOVE_BACK_WAIT_POS, kWho,
               notify_start_lsn);
    }

    set_timespec(&start_ts, 0);
    /* Calculate the waiting period. */
    abstime.tv_sec = start_ts.tv_sec + wait_timeout / TIME_SEC_TO_MS;
    abstime.tv_nsec =
        start_ts.tv_nsec + (wait_timeout % TIME_SEC_TO_MS) * TIME_SEC_TO_US;
    if (abstime.tv_nsec >= TIME_SEC_TO_NS) {
      abstime.tv_sec++;
      abstime.tv_nsec -= TIME_SEC_TO_NS;
    }

    if (connection_events_loop_aborted() &&
        (rpl_semi_sync_source_clients_wal ==
         rpl_semi_sync_source_wait_for_replica_count_wal - 1) &&
        is_on()) {
      LogErr(WARNING_LEVEL, ER_WAL_RPL_FORCED_SHUTDOWN);
      switch_off();
      break;
    }

    if (trace_level_ & kTraceDetail)
      LogErr(INFORMATION_LEVEL, ER_WAL_RPL_WAIT_TIME_FOR_WALLOG_SENT, kWho,
             wait_timeout, notify_start_lsn);

    slot_index = compute_wait_slot_no(target_lsn);
    rpl_semi_sync_source_wait_sessions_wal++;
    wait_result = mysql_cond_timedwait(&lsnWaitSlot[slot_index].cond,
                                       &LOCK_wallog_, &abstime);
    if (rpl_semi_sync_source_wait_sessions_wal > 0)
      rpl_semi_sync_source_wait_sessions_wal--;

    if (wait_result != 0) {
      /* This is a real wait timeout. */
      LogErr(WARNING_LEVEL, ER_WAL_RPL_WAIT_FOR_WALLOG_TIMEDOUT, target_lsn,
             received_standby_lsn);
      /* switch semi-sync off */
      switch_off();
    } else {
      int wait_time;
      wait_time = get_wait_time(start_ts);
      if (wait_time < 0 || DBUG_EVALUATE_IF("get_wait_time_error", 1, 0)) {
        LogErr(INFORMATION_LEVEL,
               ER_WAL_RPL_WAIT_TIME_ASSESSMENT_FOR_COMMIT_TRX_FAILED,
               target_lsn);
        rpl_semi_sync_source_timefunc_fails_wal++;
      } else {
        rpl_semi_sync_source_trx_wait_time_wal += wait_time;
        rpl_semi_sync_source_trx_wait_num_wal++;
      }
    }
  }
  if (is_on())
    rpl_semi_sync_source_yes_transactions_wal++;
  else
    rpl_semi_sync_source_no_transactions_wal++;

  unlock();
  return function_exit(kWho, 0);
}

void Wal_repl_semi_sync_master::set_wait_no_replica(const void *val) {
  lock();
  char set_switch = *static_cast<const char *>(val);
  if (set_switch == 0) {
    if ((rpl_semi_sync_source_clients_wal == 0) && (is_on())) switch_off();
  } else {
    if (!is_on() && get_master_enabled()) force_switch_on();
  }
  unlock();
}

void Wal_repl_semi_sync_master::force_switch_on() { state_ = true; }

int Wal_repl_semi_sync_master::switch_off() {
  const char *kWho = "Wal_repl_semi_sync_master::switch_off";
  function_enter(kWho);

  state_ = false;
  rpl_semi_sync_source_off_times_wal++;
  notify_start_lsn_inited = false;
  received_standby_lsn_inited = false;
  LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SWITCHED_OFF);
  notify_wait_trx(notify_start_lsn, max_trx_lsn);

  return function_exit(kWho, 0);
}

int Wal_repl_semi_sync_master::try_switch_on(uint64_t standby_flushed_lsn) {
  const char *kWho = "Wal_repl_semi_sync_master::try_switch_on";
  bool semi_sync_on = false;
  function_enter(kWho);

  if (!max_trx_lsn_inited || standby_flushed_lsn >= max_trx_lsn) {
    semi_sync_on = true;
  }

  if (semi_sync_on) {
    /* Switch semi-sync replication on. */
    state_ = true;
    LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SWITCHED_ON, standby_flushed_lsn);
  }

  return function_exit(kWho, 0);
}

void Wal_repl_semi_sync_master::set_export_stats() {
  lock();

  rpl_semi_sync_source_status_wal = state_;
  rpl_semi_sync_source_avg_trx_wait_time_wal =
      ((rpl_semi_sync_source_trx_wait_num_wal)
           ? (unsigned long)((double)rpl_semi_sync_source_trx_wait_time_wal /
                             ((double)rpl_semi_sync_source_trx_wait_num_wal))
           : 0);
  rpl_semi_sync_source_avg_net_wait_time_wal =
      ((rpl_semi_sync_source_net_wait_num_wal)
           ? (unsigned long)((double)rpl_semi_sync_source_net_wait_time_wal /
                             ((double)rpl_semi_sync_source_net_wait_num_wal))
           : 0);

  unlock();
}

int Wal_repl_semi_sync_master::reserve_sync_header(unsigned char *header,
                                                   unsigned long size) {
  const char *kWho = "Wal_repl_semi_sync_master::reserve_sync_header";
  function_enter(kWho);

  int hlen = 0;

  /* No enough space for the extra header, disable semi-sync master. */
  if (sizeof(kSyncHeader) > size) {
    LogErr(WARNING_LEVEL, ER_WAL_RPL_NO_SPACE_IN_THE_PKT);
    disable_master();
    return 0;
  }

  /**
    Set the magic number and the sync status.  By default, no sync
    is required.
  */
  memcpy(header, kSyncHeader, sizeof(kSyncHeader));
  hlen = sizeof(kSyncHeader);

  return function_exit(kWho, hlen);
}

int Wal_repl_semi_sync_master::set_master_sync_bit(unsigned char *packet,
                                                   uint64_t lsn,
                                                   uint32_t server_id) {
  const char *kWho = "Wal_repl_semi_sync_master::set_master_sync_bit";
  function_enter(kWho);

  lock();
  if (!get_master_enabled()) {
    unlock();
    return function_exit(kWho, 0);
  }
  unlock();

  /* For wal rpl, always set kPacketFlagSync. */
  bool sync = true;
  if (sync) {
    (packet)[2] = kPacketFlagSync;
  }

  if (trace_level_ & kTraceDetail)
    LogErr(INFORMATION_LEVEL, ER_WAL_RPL_SYNC_HEADER_UPDATE_INFO, kWho,
           server_id, lsn, sync, (int)is_on());

  return function_exit(kWho, 0);
}

int Wal_repl_semi_sync_master::read_slave_wal_reply(NET *net,
                                                    const char *event_buf
                                                    [[maybe_unused]]) {
  const char *kWho = "Wal_repl_semi_sync_master::read_slave_wal_reply";
  int result = -1;

  function_enter(kWho);

  assert((unsigned char)event_buf[1] == kPacketMagicNum);

  if ((unsigned char)event_buf[2] != kPacketFlagSync) {
    return function_exit(kWho, 0);
  }

  /**
    We flush to make sure that the current event is sent to the network,
    instead of being buffered in the TCP/IP stack.
  */
  if (net_flush(net)) {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_SOURCE_FAILED_ON_NET_FLUSH);
    goto l_end;
  }

  net_clear(net, false);
  net->pkt_nr++;
  result = 0;
  rpl_semi_sync_source_net_wait_num_wal++;

l_end:
  return function_exit(kWho, result);
}

int Wal_repl_semi_sync_master::reset_master() {
  const char *kWho = "Wal_repl_semi_sync_master::reset_master";
  int result = 0;
  function_enter(kWho);

  lock();
  notify_start_lsn_inited = false;
  received_standby_lsn_inited = false;
  max_trx_lsn_inited = false;

  rpl_semi_sync_source_wait_pos_backtraverse_wal = 0;
  rpl_semi_sync_source_timefunc_fails_wal = 0;
  rpl_semi_sync_source_trx_wait_num_wal = 0;
  rpl_semi_sync_source_net_wait_time_wal = 0;
  rpl_semi_sync_source_wait_sessions_wal = 0;
  rpl_semi_sync_source_yes_transactions_wal = 0;
  rpl_semi_sync_source_no_transactions_wal = 0;
  rpl_semi_sync_source_off_times_wal = 0;
  rpl_semi_sync_source_trx_wait_time_wal = 0;
  rpl_semi_sync_source_net_wait_num_wal = 0;

  unlock();
  return function_exit(kWho, result);
}

/**
  Get the waiting time given the wait's staring time.

  @return >= 0: the waiting time in microsecons(us)
          < 0: error in get time or time back traverse
*/
static int get_wait_time(const struct timespec &start_ts) {
  uint64_t start_usecs, end_usecs;
  struct timespec end_ts;

  /* Starting time in microseconds(us). */
  start_usecs = timespec_to_usec(&start_ts);

  /* Get the wait time interval. */
  set_timespec(&end_ts, 0);

  /* Ending time in microseconds(us). */
  end_usecs = timespec_to_usec(&end_ts);
  if (end_usecs < start_usecs) return -1;

  return (int)(end_usecs - start_usecs);
}