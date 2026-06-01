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

#ifndef DEFINED_RPL_WAL_MGR
#define DEFINED_RPL_WAL_MGR

#include <atomic>
#include <chrono>

#include "mysql/components/services/bits/mysql_cond_bits.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "mysql/components/services/bits/psi_cond_bits.h"
#include "mysql/components/services/bits/psi_file_bits.h"
#include "mysql/components/services/bits/psi_mutex_bits.h"
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"

class MYSQL_WAL {
 public:
  explicit MYSQL_WAL();
  ~MYSQL_WAL() = default;

  /**
    m_lock_wal_flushed_lsn and m_wal_update_cond is inited by
    init_pthread_objects()
  */
  void init_pthread_objects();
  void cleanup();
  void set_psi_keys(PSI_mutex_key key_LOCK_wal_flushed_lsn,
                    PSI_cond_key key_wal_update_cond);

  /**
    Master has no new wal to read, waiting for the new wal to flush and
    does not send a heartbeat message.
  */
  void wait_for_update_wal();

  /**
    Master has no new wal to read, waiting for the new wal to flush,
    if it's awakened by timeout, a heartbeat message will be sent.

    @param[in] timeout  The timeout period for waking up.

    @retval 0 Succeed
    @retval 1 Fail
  */
  int wait_for_update_wal(const std::chrono::nanoseconds &timeout);

  /**
    Replica waits to flush the received wal data,
    it's woken up after flushing.

    @param[in] lsn  LSN that needs to be flushed.
  */
  void wait_for_update_wal(uint64_t lsn);

  /**
    When wal data is flushed to disk, update m_wal_flushed_lsn.

    @param[in] lsn  Max lsn that has been flushed on disk.
  */
  void update_wal_flushed_lsn(uint64_t lsn);

  /**
    Init m_wal_flushed_lsn.

    @param[in] lsn  Max lsn that has been flushed on disk.
  */
  void init_wal_flushed_lsn(uint64_t lsn);

  uint64_t get_wal_flushed_lsn();

  inline bool is_inited() { return m_inited; }
  inline bool is_wal_flushed_lsn_inited() {
    return m_wal_flushed_lsn_inited.load();
  }
  inline mysql_mutex_t *get_wal_flush_lsn_lock() {
    return &m_lock_wal_flushed_lsn;
  }
  inline mysql_cond_t *get_wal_update_cond() { return &m_wal_update_cond; }
  void lock_wal_flush_lsn() { mysql_mutex_lock(&m_lock_wal_flushed_lsn); }
  void unlock_wal_flush_lsn() { mysql_mutex_unlock(&m_lock_wal_flushed_lsn); }

 private:
  void signal_update();

  PSI_mutex_key m_key_LOCK_wal_flushed_lsn;
  PSI_cond_key m_key_wal_update_cond;
  mysql_mutex_t m_lock_wal_flushed_lsn;
  mysql_cond_t m_wal_update_cond;
  /* max flushed lsn on disk. */
  std::atomic<uint64_t> m_wal_flushed_lsn;
  std::atomic<bool> m_wal_flushed_lsn_inited;

  bool m_inited;
};

extern MYSQL_WAL mysql_wal;
#endif