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

#include "my_dbug.h"
#include "my_systime.h"
#include "sql/log.h"
#include "sql/rpl_wal_mgr.h"

MYSQL_WAL mysql_wal;

MYSQL_WAL::MYSQL_WAL() : m_inited(false) {}

void MYSQL_WAL::init_pthread_objects() {
  assert(m_inited == false);
  m_inited = true;
  m_wal_flushed_lsn_inited.store(false);
  m_wal_flushed_lsn.store(0);
  mysql_mutex_init(m_key_LOCK_wal_flushed_lsn, &m_lock_wal_flushed_lsn,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(m_key_wal_update_cond, &m_wal_update_cond);
}

void MYSQL_WAL::cleanup() {
  DBUG_TRACE;
  if (m_inited) {
    m_inited = false;
    mysql_mutex_destroy(&m_lock_wal_flushed_lsn);
    mysql_cond_destroy(&m_wal_update_cond);
  }
}

void MYSQL_WAL::set_psi_keys(PSI_mutex_key key_LOCK_wal_flushed_lsn,
                             PSI_cond_key key_wal_update_cond) {
  m_key_LOCK_wal_flushed_lsn = key_LOCK_wal_flushed_lsn;
  m_key_wal_update_cond = key_wal_update_cond;
}

void MYSQL_WAL::wait_for_update_wal() {
  DBUG_TRACE;
  mysql_mutex_assert_owner(&m_lock_wal_flushed_lsn);
  mysql_cond_wait(&m_wal_update_cond, &m_lock_wal_flushed_lsn);
}

int MYSQL_WAL::wait_for_update_wal(const std::chrono::nanoseconds &timeout) {
  DBUG_TRACE;
  struct timespec ts;
  set_timespec_nsec(&ts, timeout.count());
  mysql_mutex_assert_owner(&m_lock_wal_flushed_lsn);
  int res =
      mysql_cond_timedwait(&m_wal_update_cond, &m_lock_wal_flushed_lsn, &ts);
  return res;
}

void MYSQL_WAL::wait_for_update_wal(uint64_t lsn) {
  DBUG_TRACE;
  // TODO: add time out logic later
  lock_wal_flush_lsn();
  mysql_mutex_assert_owner(&m_lock_wal_flushed_lsn);
  while (get_wal_flushed_lsn() < lsn) {
    mysql_cond_wait(&m_wal_update_cond, &m_lock_wal_flushed_lsn);
  }
  unlock_wal_flush_lsn();
}

void MYSQL_WAL::update_wal_flushed_lsn(uint64_t lsn) {
  lock_wal_flush_lsn();
  if (lsn > m_wal_flushed_lsn.load()) {
    m_wal_flushed_lsn.store(lsn);
  }
  signal_update();
  unlock_wal_flush_lsn();
}

void MYSQL_WAL::signal_update() {
  DBUG_TRACE;
  DBUG_EXECUTE_IF("simulate_delay_in_wal_signal_update",
                  std::this_thread::sleep_for(std::chrono::milliseconds(120)););
  mysql_cond_broadcast(&m_wal_update_cond);
  return;
}

void MYSQL_WAL::init_wal_flushed_lsn(uint64_t lsn) {
  if (m_wal_flushed_lsn_inited.load()) {
    return;
  }
  lock_wal_flush_lsn();

  if (m_wal_flushed_lsn_inited.load()) {
    unlock_wal_flush_lsn();
    return;
  }

  m_wal_flushed_lsn.store(lsn);
  m_wal_flushed_lsn_inited.store(true);
  sql_print_information("MYSQL_WAL init flushed lsn: %lu.",
                        (uint64_t)m_wal_flushed_lsn);
  unlock_wal_flush_lsn();
  return;
}

uint64_t MYSQL_WAL::get_wal_flushed_lsn() {
  if (m_wal_flushed_lsn_inited.load()) {
    return m_wal_flushed_lsn.load();
  }
  return 0;
}