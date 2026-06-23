/* Copyright (c) 2021, Huawei and/or its affiliates. All rights reserved.

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

#include "sql/parallel_query/pq_hash_join_shared_context.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace HashJoin {

bool PQHashJoinSharedContext::MemoryFull(
    int64_t previous_memory_allocated_for_thread,
    int64_t current_memory_allocated_for_thread) {
  const int64_t delta = current_memory_allocated_for_thread -
                        previous_memory_allocated_for_thread;

  const int64_t old_value = m_bytes_allocated.fetch_add(delta);
  const int64_t new_value = old_value + delta;
  assert(old_value >= previous_memory_allocated_for_thread);
  return new_value > static_cast<int64_t>(m_max_mem_available);
}

}  // namespace HashJoin
