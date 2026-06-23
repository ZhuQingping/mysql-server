/* Copyright (c) 2020, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "pq_resource_stat.h"

#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"

/**
 * wraps some basic variables and functions for PQ
 */

ulonglong parallel_memory_limit = 0;
ulong parallel_max_threads = 0;
uint parallel_threads_running = 0;
uint parallel_threads_refused = 0;
uint parallel_memory_refused = 0;
uint pq_memory_used[16] = {0};
uint pq_memory_total_used = 0;
uint pq_stmt_executed = 0;

mysql_mutex_t LOCK_pq_threads_running;
mysql_cond_t COND_pq_threads_running;

template <typename T>
T atomic_add(T &value, T n) {
  return __sync_fetch_and_add(&value, n);
}

template <typename T>
T atomic_sub(T &value, T n) {
  return __sync_fetch_and_sub(&value, n);
}

template unsigned int atomic_add<unsigned int>(unsigned int &, unsigned int);

uint get_pq_memory_total() {
  uint sum_memory = 0;
  for (int i = 0; i < PQ_MEMORY_USED_BUCKET; i++)
    sum_memory += atomic_add<uint>(pq_memory_used[i], 0);
  return sum_memory;
}

void add_pq_memory(PSI_memory_key key, size_t length,
                   unsigned int id /*MY_ATTRIBUTE((unused))*/) {
  (void)key;
  if (id < PQ_MEMORY_USED_BUCKET) atomic_add<uint>(pq_memory_used[id], length);
}

void sub_pq_memory(PSI_memory_key key, size_t length,
                   unsigned int id /*MY_ATTRIBUTE((unused))*/) {
  (void)key;
  if (id < PQ_MEMORY_USED_BUCKET) atomic_sub<uint>(pq_memory_used[id], length);
}

void release_pq_running_threads(uint dop) {
  mysql_mutex_lock(&LOCK_pq_threads_running);
  if (parallel_threads_running >= dop)
    parallel_threads_running -= dop;
  else
    parallel_threads_running = 0;
  mysql_cond_broadcast(&COND_pq_threads_running);
  mysql_mutex_unlock(&LOCK_pq_threads_running);
}
