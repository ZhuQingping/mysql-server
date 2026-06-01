#ifndef MYSQL_PQ_RESOURCE_STAT_H
#define MYSQL_PQ_RESOURCE_STAT_H

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

#include <iostream>
#include <memory>
#include "my_alloc.h"
#include "my_compiler.h"
#include "mysql/components/services/bits/mysql_cond_bits.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "mysql/components/services/bits/psi_bits.h"
/**
 *wraps some basic variables and functions for PQ
 */

#define TIME_THOUSAND 1000
#define TIME_MILLION 1000000
#define TIME_BILLION 1000000000
#define PQ_MEMORY_USED_BUCKET 0xf

extern ulonglong parallel_memory_limit;
extern ulong parallel_max_threads;
extern uint parallel_threads_running;
extern uint parallel_threads_refused;
extern uint parallel_memory_refused;
extern uint pq_memory_used[16];
extern uint pq_memory_total_used;
extern uint pq_stmt_executed;

extern mysql_mutex_t LOCK_pq_threads_running;
extern mysql_cond_t COND_pq_threads_running;

/** range search */
enum PQ_RANGE_TYPE {
  PQ_QUICK_SELECT_NONE = 1,
  PQ_RANGE_SELECT = 2,
  PQ_RANGE_SELECT_DESC = 4,
  PQ_SKIP_SCAN_SELECT = 8,
  PQ_GROUP_MIN_MAX_SELECT = 16,
  PQ_INDEX_MERGE_SELECT = 32,
  PQ_ROR_INTERSECT_SELECT = 64,
  PQ_ROR_UNION_SELECT = 128,
  PQ_QUICK_SELECT_INVALID = 256
};

template <typename T>
T atomic_add(T &value, T n);

template <typename T>
T atomic_sub(T &value, T n);

uint get_pq_memory_total();

void add_pq_memory(PSI_memory_key key, size_t length,
                   unsigned int id /*MY_ATTRIBUTE((unused))*/);

void sub_pq_memory(PSI_memory_key key, size_t length,
                   unsigned int id /*MY_ATTRIBUTE((unused))*/);

void release_pq_running_threads(uint dop);

#endif  // MYSQL_PQ_RESOURCE_STAT_H
