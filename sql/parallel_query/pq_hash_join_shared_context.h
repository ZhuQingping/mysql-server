#ifndef SQL_PQ_HASH_JOIN_SHARED_CONTEXT_H
#define SQL_PQ_HASH_JOIN_SHARED_CONTEXT_H

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

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "sql/parallel_query/barrier.h"

namespace hash_join_buffer {
class HashJoinRowBuffer;
}

namespace HashJoin {

class PQHashJoinSharedContext {
 public:
  explicit PQHashJoinSharedContext(int num_workers, size_t max_mem_available)
      : m_barrier(num_workers),
        m_num_workers(num_workers),
        m_max_mem_available(max_mem_available),
        m_destroy_barrier(num_workers) {}

  std::mutex &HashTableMutex() { return m_hash_table_mutex; }

  bool MemoryFull(int64_t previous_memory_allocated_for_thread,
                  int64_t current_memory_allocated_for_thread);

  int NumWorkers() const { return m_num_workers; }

  int NumberOfConstructedWorkers() const {
    return m_number_of_constructed_workers;
  }

  void IncrementNumberOfConstructedWorkers() {
    ++m_number_of_constructed_workers;
  }

  Barrier &ExecutionBarrier() { return m_barrier; }

  Barrier &DestructionBarrier() { return m_destroy_barrier; }

  bool HasMoreDataInBuildInput() const {
    return m_has_more_data_in_build_input;
  }

  void SetHasMoreDataInBuildInput(bool has_more_data) {
    m_has_more_data_in_build_input = has_more_data;
  }

  void RegisterHashMap(const void *hash_map) {
    const std::lock_guard<std::mutex> lock(HashTableMutex());
    m_hash_maps.push_back(hash_map);
  }

  const std::vector<const void *> &HashMaps() const { return m_hash_maps; }

  void ClearHashMaps() {
    const std::lock_guard<std::mutex> lock(HashTableMutex());
    m_hash_maps.clear();
    m_global_hash_map_count = 0;
  }

  void IncrementGlobalHashMapCount() { ++m_global_hash_map_count; }

  size_t GlobalHashMapCount() const { return m_global_hash_map_count; }

 private:
  Barrier m_barrier;
  const int m_num_workers;
  std::atomic<int64_t> m_bytes_allocated{0};
  const size_t m_max_mem_available;
  Barrier m_destroy_barrier;
  int m_number_of_constructed_workers{0};
  std::atomic<bool> m_has_more_data_in_build_input{false};
  std::mutex m_hash_table_mutex;
  std::vector<const void *> m_hash_maps;
  std::atomic<size_t> m_global_hash_map_count{0};
};

}  // namespace HashJoin

#endif
