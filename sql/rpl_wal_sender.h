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

#ifndef DEFINED_RPL_WAL_SENDER
#define DEFINED_RPL_WAL_SENDER

#include <chrono>

#include "mysql_com.h"
#include "mysqld_error.h"  // ER_*
#include "sql/handler.h"   // handlerton
#include "sql/rpl_wal_common.h"
#include "sql/sql_error.h"  // Diagnostics_area

/**
  @class Wal_sender

  The major logic of dump wal thread is implemented in this class. It sends
  required wal to clients according to their requests.
*/
class Wal_sender {
 public:
  Wal_sender(THD *thd, uint64_t start_lsn, uint32_t flag,
             uint64_t wal_stream_id);
  ~Wal_sender() = default;

  /**
    It checks the dump wal request and sends wal to the client until it finish
    all wal or encounters an error.
  */
  void run();

 private:
  /**
    It initializes the context, checks if the dump wal request is valid and
    if wal status is correct.

    @retval false Succeed
    @retval true Fail
  */
  bool init();

  /**
    Cleanup resourses.
  */
  void cleanup();

  /**
    It initializes the heartbeat period.
  */
  void init_heartbeat_period();

  /**
    Reset the thread transmit packet buffer for wal sending.

    This function reserves the bytes for wal transmission, and
    should be called before storing the wal data to the packet buffer.

    @param[in] flags  The flag used in wallog_reserve_header hook.
  */
  int reset_packet(ushort flags = 0);
  int send_packet();
  int flush_net();

  /**
    It sends a heartbeat to the client.
    @param[in] master_flushed_lsn  The flushed lsn on master.

    @return It returns 0 if succeeds, otherwise 1 is returned.
  */
  int send_heartbeat(uint64_t master_flushed_lsn);
  int before_send_hook(uint64_t log_pos);
  int after_send_hook();

  /**
    It reads wal data from wal file.

    @param[in] start_lsn  Start lsn of the wal file.

    @retval 0 Succeed
    @retval 1 Fail
  */
  int read_wal(uint64_t start_lsn);

  /**
    Fill the Rpl_wal_data_msg header before send packet.

    @param[in] header  Rpl_wal_data_msg to be sent.
    @param[in] start_lsn  Start lsn of the wal file.
  */
  void fill_rpl_wal_header(uchar *header, uint64_t start_lsn);

  /**
    If there is no new wal to read, sleep and wait for wal flush to wake up.
    If sleep times out, a heartbeat message will be sent.

    @param[in] master_flushed_lsn  The flushed lsn on master.

    @retval false Succeed
    @retval true Fail
  */
  bool sleep_and_wait_wal(uint64_t master_flushed_lsn);

  /**
    Checks whether thread should continue awaiting new wal data.

    @param master_flush_lsn The flushed lsn on master.

    @retval true  Stop waiting, some wal are flushed to disk or thread killed.
    @retval false Still need to waiting.
  */
  bool stop_waiting_for_update_wal(uint64_t master_flush_lsn) const;

  /**
    Sleep and wait for wal flush to wake up. If sleep times out,
    a heartbeat message will be sent.

    @param[in] master_flushed_lsn  The flushed lsn on master.
  */
  void wait_without_heartbeat(uint64_t master_flush_lsn);

  /**
    Sleep and wait for wal flush to wake up. Even if the timeout period is
    exceeded, the heartbeat will not be sent.

    @param[in] master_flushed_lsn  The flushed lsn on master.

    @retval false Succeed
    @retval true  Fail
  */
  bool wait_with_heartbeat(uint64_t master_flush_lsn);

  /**
    Verify that each parameter is legal, such as whether the wal data is
    recycled and whether the wal stream id of the master and slave are the same.

    @param[in] master_flushed_lsn  The flushed lsn on master.

    @retval false Succeed
    @retval true  Fail
  */
  bool check_param_valid(uint64_t master_flushed_lsn);

  inline bool has_error() { return m_errno != 0; }

  inline void set_error(int errorno, const char *errmsg) {
    snprintf(m_errmsg_buf, sizeof(m_errmsg_buf), "%.*s", MYSQL_ERRMSG_SIZE - 1,
             errmsg);
    m_errmsg = m_errmsg_buf;
    m_errno = errorno;
  }

  inline void set_unknown_error(const char *errmsg) {
    set_error(ER_UNKNOWN_ERROR, errmsg);
  }

  inline void set_last_lsn(uint64_t last_lsn) { m_last_lsn = last_lsn; }

  THD *m_thd;
  String &m_packet;
  Diagnostics_area m_diag_area;
  uint32_t m_flag;

  /* Requested start wal file position. */
  uint64_t m_start_lsn;

  /* Last sent lsn. */
  uint64_t m_last_lsn;

  /**
    It is true if wallog_transmit_start hook is called. If the hook is not
    called it will be false.
  */
  bool m_transmit_started;

  /**
    It is true if any plugin requires to observe the transmission for each
    wal data. And HOOKs(wallog_reserve_header, wallog_before_send_event and
    wallog_after_send_event) are called when transmitting each wal data.
    Otherwise, it is false and HOOKs are not called.
  */
  bool m_observe_transmission;

  char m_errmsg_buf[MYSQL_ERRMSG_SIZE];
  const char *m_errmsg;
  int m_errno;

  /**
    The size of the wal data read this time.
  */
  uint64_t m_read_len;

  /**
    Heartbeat interval.
  */
  std::chrono::nanoseconds m_heartbeat_period;

  /**
    The maximum value for MAX_ALLOWED_PACKET.  This is also the
    maxmium size of wal data, and dump wal threads always use this
    value for MAX_RPL_WAL_ALLOWED_PACKET.
  */
  constexpr static size_t MAX_RPL_WAL_ALLOWED_PACKET = 1024 * 1024 * 1024;

  /** Same as the max walsender buffer size in dstore. */
  constexpr static uint32_t MAX_READ_WAL_SIZE = 1024 * 1024;

  /** Reserve header: '\0', magic num and sync flag. */
  constexpr static uint32_t RESERVE_HEADER_SIZE = 3;
  constexpr static uint32_t NO_RESERVE_HEADER_SIZE = 1;
  handlerton *m_cde_ha;

  /**
    Master wal stream id, should never change.
  */
  uint64_t m_master_wal_stream_id;

  /**
    Standby wal stream id, should never change.
  */
  uint64_t m_standby_wal_stream_id;
};

#endif  // DEFINED_RPL_WAL_SENDER