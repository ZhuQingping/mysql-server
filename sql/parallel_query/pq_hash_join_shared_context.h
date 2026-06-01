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
#include <memory>
#include <mutex>
#include <unordered_set>

#include "include/my_alloc.h"
#include "sql/current_thd.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/parallel_query/barrier.h"
#include "sql/parallel_query/chunk_files_wrapper.h"

namespace HashJoin {

/// A class that contains data/structures that are shared across multiple
/// workers for one single hash join node. It contains a barrier for
/// synchronizing state between workers as well as locks that is used when
/// reading/writing to hash maps. Note that the hash maps are located in each
/// worker, and each worker will insert data into its own hash map. But during
/// the probe phase, each worker will read from all the other workers' hash map.
/// The same will also happen whenever a worker calls size() or empty() on the
/// hash map; each worker will look at all the other workers' hash map.
class PQHashJoinSharedContext {
 public:
  explicit PQHashJoinSharedContext(int num_workers, size_t max_mem_available)
      : m_barrier(num_workers),
        m_num_workers(num_workers),
        m_max_mem_available(max_mem_available),
        m_destroy_barrier(num_workers),
        m_chunk_files(std::make_shared<ChunkFilesWrapper>(
            current_thd->mem_root,
            /*needs_mutex_protection=*/true, num_workers)) {}
  std::mutex &HashTableMutex() { return m_hash_table_mutex; }

  /// @returns true if we have exceeded the memory budget.
  bool MemoryFull(int64_t previous_memory_allocated_for_thread,
                  int64_t current_memory_allocated_for_thread);

  int NumWorkers() const { return m_num_workers; }

  int NumberOfConstructedWorkers() const {
    return m_number_of_constructed_workers;
  }

  void IncrementNumberOfConstructedWorkers() {
    ++m_number_of_constructed_workers;
  }

  /// Return the barrier used to synchronize workers during query execution.
  Barrier &ExecutionBarrier() { return m_barrier; }

  /// Return the barrier used to synchronize workers during the destruction of
  /// HashJoinIterator. We do not for instance want to destroy the MEM_ROOT that
  /// contains the row data while another thread is reading these rows.
  Barrier &DestructionBarrier() { return m_destroy_barrier; }

  std::shared_ptr<ChunkFilesWrapper> ChunkFilesOnDisk() {
    return m_chunk_files;
  }

  bool HasMoreDataInBuildInput() const {
    return m_has_more_data_in_build_input;
  }

  void SetHasMoreDataInBuildInput(bool has_more_data) {
    m_has_more_data_in_build_input = has_more_data;
  }

  void RegisterHashMap(hash_join_buffer::HashJoinRowBuffer::HashMap *hash_map) {
    const std::lock_guard<std::mutex> lock(HashTableMutex());
    m_hash_maps.push_back(hash_map);
  }

  const std::vector<const hash_join_buffer::HashJoinRowBuffer::HashMap *>
      &HashMaps() const {
    return m_hash_maps;
  }

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

  std::atomic<int64_t> m_bytes_allocated{
      0};  ///< Total of bytes allocated by all workers
  const size_t m_max_mem_available;  ///< Upper limit on this total

  Barrier m_destroy_barrier;

  /// The number of constructed HashJoinIterators/workers. This is to decrement
  /// the number of participants in the synchronization barriers with N, where N
  /// is the different between "dop" (the parallel degree) and
  /// m_number_of_constructed_workers. If we do not decrement by N, constructed
  /// workers will wait forever for these non-existing workers.
  int m_number_of_constructed_workers{0};

  std::shared_ptr<ChunkFilesWrapper> m_chunk_files;

  /// Whether any of the workers has more data in its build input. Used to
  /// determine whether we can stop the hash join execution.
  std::atomic<bool> m_has_more_data_in_build_input{false};

  /// A mutex used to protect the below variable "m_hash_maps" as well as the
  /// list of chunk files "m_chunk_files". This mutex must be locked whenever
  /// the user wants to modify m_hash_maps or m_chunk_files.
  std::mutex m_hash_table_mutex;

  /// A list that contains the hash map for each worker. It is used by workers
  /// to access other workers hash map.
  std::vector<const hash_join_buffer::HashJoinRowBuffer::HashMap *> m_hash_maps;

  /// The aggregated size() of all hash maps in the above vector m_hash_maps.
  std::atomic<size_t> m_global_hash_map_count;
};

}  // namespace HashJoin

#endif
