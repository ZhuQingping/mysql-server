#ifndef SQL_CHUNK_FILES_WRAPPER_H
#define SQL_CHUNK_FILES_WRAPPER_H

/* Copyright (c) 2023, Huawei and/or its affiliates. All rights reserved.

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

/**
  @file sql/chunk_files_wrapper.h

  Classes containing a common interface to ChunkFiles. See class comment for
  more details.
*/

#include <mutex>
#include "sql/current_thd.h"
#include "sql/iterators/hash_join_chunk.h"
#include "sql/vfd/vfd_manager.h"

struct ChunkPair {
  ChunkPair(VfdManager *vfd_manager)
      : probe_chunk(vfd_manager), build_chunk(vfd_manager) {}
  HashJoinChunk probe_chunk;
  HashJoinChunk build_chunk;
};

/// ChunkFilesWrapper is an abstraction layer that provides an uniform access to
/// HashJoinChunk. A quick recap of terminology:
///
/// VfdFile: A file on disk that is opened/closed as needed. Used to store any
///          data.
/// HashJoinChunk: A wrapper over VfdFile. It is used to store rows, as opposed
///                to arbitrary data. Contains for instance row count. Also
///                known as ChunkFile.
/// ChunkPair: A pair of HashJoinChunk; one for the probe input and one for the
///            build input.
///
/// With parallel-aware hash join and spill to disk, reading and writing to
/// HashJoinChunks needs mutex protection since they are accessing the same set
/// of HashJoinChunks. This abstraction layer (ChunkFilesWrapper) gives a
/// uniform access to HashJoinChunks regardless of whether mutex protection is
/// needed or not.
///
/// Note that the API is very tailored towards hash join. If this class is to be
/// used by others than hash join in the future, the API should probably be
/// redesigned to become more generic.
///
/// --- m_needs_mutex_protection == false ---
/// If m_needs_mutex_protection is false, this class is to be used in single
/// thread execution, or in parallel-oblivious hash join where each worker has
/// it own set of HashJoinChunks.
///
/// --- m_needs_mutex_protection == true ---
/// If m_needs_mutex_protection is true, this class is to be used in
/// parallel-aware hash join where all workers share the same set of
/// HashJoinChunks.
///
/// All chunk-local operations are guarded by a chunk-specific mutex.
///
/// Global operations (which affect the set of all chunks) are guarded by a
/// global mutex m_global_mutex, except for three: CurrentChunkIndex, Clear()
/// and Size().
///
/// The value of CurrentChunkIndex (m_current_chunk) is only modified from
/// barrier completion functions, and the barrier guarantees that only one
/// thread will call the function while the other threads are sleeping.
///
/// Clear() is only called from a barrier completion function, so no protection
/// needed for the same reason as above.
///
/// Size() is used in tight loops in hash join, and adding mutex
/// protection to this slows down performance significantly.
///
/// So is it safe to not protect Size() with m_global_mutex? With the current
/// usage pattern in hash join; yes. The reasoning:
///
/// - Size() returns the size of m_chunk_files_on_disk
/// - We have two functions that modify m_chunk_files_on_disk: Clear() and
///   AddChunkPair().
/// - Clear() is only called from within a barrier completion function. These
///   need no extra protection, as the barrier guarantees that only one thread
///   will call the function while the other threads are sleeping.
/// - AddChunkPair() is only called from HashJoinIterator::InitializeChunkFiles,
///   which is called from HashJoinIterator::BuildHashTable(). The code in
///   BuildHashTable() looks like this:
///
///     if (parallel_aware_hash_join) {
///       lock PQHashJoinSharedContext::HashTableMutex
///       if (ChunkFiles->size() == 0) {
///         InitializeChunkFiles()
///       }
///     }
///
///   Here, AddChunkPair() is guarded by HashTableMutex. Why does this make it
///   safe?
///   - Only one worker will call AddChunkPair() due to this mutex.
///   - All usage of Size() happens after BuildHashTable(). Note that Size() is
///     called two times before AddChunkPair, but both calls are under the same
///     mutex protection (HashTableMutex).
///   - At the end of BuildHashTable(), we have a synchronization barrier,
///     ensuring that all workers are at the end of BuildHashTable before
///     continuing.
///
/// So shortly summarized; HashJoinIterator guarantees that all usage of Size()
/// happens after any call to AddChunkPair.
///
/// == A note on locking
/// Execution of parallel-aware hash join includes multiple mutexes. The most
/// important ones are:
///
/// - PQHashJoinSharedContext::HashTableMutex
/// - ChunkFilesWrapper::m_global_mutex
/// - ChunkFilesWrapper::m_mutexes
/// - Barrier::m_mutex
/// - VfdManager::m_lru_mutex
///
/// To keep a well-defined order of locking, locks must be taken in the order
/// given above.
class ChunkFilesWrapper {
 public:
  explicit ChunkFilesWrapper(MEM_ROOT *mem_root, bool needs_mutex_protection,
                             int num_workers)
      : m_chunk_files_on_disk(mem_root),
        m_mutexes(static_cast<size_t>(std::pow(num_workers, 2))),
        m_needs_mutex_protection(needs_mutex_protection) {
    // We initialize the mutex vector with num_workers^2 mutexes. We get a
    // collision probability of around 40% (c.f. the Birthday Paradox). Some
    // performance testing suggests that having a lower collision probability
    // than this has not much performance gain. One would think that the ideal
    // would be to have one mutex per VfdFile, but this is overkill as the
    // collision probability only depends on the number of workers and the
    // number of mutexes available. And since the maximum number of workers is
    // 64 and sizeof(std::mutex) is 40 bytes, it will give a maximum memory
    // consumption of approx. 160 kilobytes (which is not too bad).
  }

  /// Return the current ChunkFile index.
  int CurrentChunkIndex() { return m_current_chunk; }

  /// Add the given ChunkPair to the list of ChunkFiles. Size() will by one.
  void AddChunkPair(ChunkPair &&chunk_pair) {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(m_global_mutex)
                    : std::unique_lock<std::mutex>();

    m_chunk_files_on_disk.push_back(std::move(chunk_pair));
  }

  /// Number of ChunkFiles.
  size_t Size() { return m_chunk_files_on_disk.size(); }

  /// Whether there are more rows in the current build chunk.
  bool HasMoreDataInBuildChunk() {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(
                          m_mutexes[CurrentChunkIndex() % m_mutexes.size()])
                    : std::unique_lock<std::mutex>();

    return m_build_chunk_current_row <
           m_chunk_files_on_disk[m_current_chunk].build_chunk.num_rows();
  }

  /// Rewind the current build chunk to the beginning so that the next read will
  /// start at the beginning.
  bool RewindCurrentBuildChunk() {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(
                          m_mutexes[CurrentChunkIndex() % m_mutexes.size()])
                    : std::unique_lock<std::mutex>();

    m_build_chunk_current_row = 0;
    return m_chunk_files_on_disk[m_current_chunk].build_chunk.Rewind();
  }

  /// Rewind the current probe chunk to the beginning.
  bool RewindCurrentProbeChunk() {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(
                          m_mutexes[CurrentChunkIndex() % m_mutexes.size()])
                    : std::unique_lock<std::mutex>();

    m_probe_chunk_current_row = 0;
    return m_chunk_files_on_disk[m_current_chunk].probe_chunk.Rewind();
  }

  /// Remove all ChunkFiles and reset to a clean state.
  void Clear() {
    m_chunk_files_on_disk.clear();

    m_build_chunk_current_row = 0;
    m_probe_chunk_current_row = 0;
    m_current_chunk = -1;
  }

  /// Reset m_current_chunk to -1
  void ResetCurrentChunk() { m_current_chunk = -1; }

  /// Possibly move to the next ChunkPair. See the implementation for details on
  /// when a move is performed. To determine whether a move actually happened,
  /// callers can either look at the return value or observe the value of
  /// CurrentChunkIndex() before and after calling this function.
  ///
  /// @returns true if we did move to the next ChunkPair (CurrentChunkIndex()
  ///          was incremented by one).
  bool PossiblyMoveToNextChunk() {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(m_global_mutex)
                    : std::unique_lock<std::mutex>();

    // See if we should proceed to the next pair of chunk files. In general,
    // it works like this; if we are at the end of the build chunk, move to the
    // next. If not, keep reading from the same chunk pair. We also move to the
    // next pair of chunk files if the probe chunk file is empty.
    bool move_to_next_chunk = false;
    if (m_current_chunk == -1) {
      // We are before the first chunk, so move to the next.
      move_to_next_chunk = true;
    } else if (m_build_chunk_current_row >=
               m_chunk_files_on_disk[m_current_chunk].build_chunk.num_rows()) {
      // We are done reading all the rows from the build chunk.
      move_to_next_chunk = true;
    } else if (m_chunk_files_on_disk[m_current_chunk].probe_chunk.num_rows() ==
               0) {
      // The probe chunk file is empty.
      move_to_next_chunk = true;
    }

    if (move_to_next_chunk) {
      m_current_chunk++;
    }

    if (m_current_chunk == static_cast<int>(Size())) {
      // We have moved past the last chunk. Notify the caller that they should
      // not try to read from the chunk files anymore.
      move_to_next_chunk = false;
    }

    return move_to_next_chunk;
  }

  enum class LoadRowResult { ROW_READY, END_OF_ROWS, ERROR };

  /// Load the next row in the current ChunkFile, given by "is_build_chunk", to
  /// the tables record buffer.
  LoadRowResult LoadNextRow(String *buffer, bool *matched, bool is_build_chunk,
                            const pack_rows::TableCollection &tables) {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(
                          m_mutexes[CurrentChunkIndex() % m_mutexes.size()])
                    : std::unique_lock<std::mutex>();

    HashJoinChunk *chunk_file;
    ha_rows *counter;
    if (is_build_chunk) {
      chunk_file = &m_chunk_files_on_disk[m_current_chunk].build_chunk;
      counter = &m_build_chunk_current_row;
    } else {
      chunk_file = &m_chunk_files_on_disk[m_current_chunk].probe_chunk;
      counter = &m_probe_chunk_current_row;
    }

    if (*counter >= chunk_file->num_rows()) {
      return LoadRowResult::END_OF_ROWS;
    }

    if (chunk_file->LoadRowFromChunk(buffer, matched, tables)) {
      return LoadRowResult::ERROR;
    }

    ++(*counter);
    return LoadRowResult::ROW_READY;
  }

  /// Write the data stored in "tables" to the ChunkFiles found at the given
  /// index. Which columns to store is given by the read_set of each table.
  /// "buffer" is, as the name implies, a buffer that is used to reduce the
  /// number of allocations needed.
  bool WriteToChunkIndex(String *buffer, size_t chunk_index,
                         bool is_build_chunk, bool row_has_match,
                         const pack_rows::TableCollection &tables) {
    auto lock = m_needs_mutex_protection
                    ? std::unique_lock<std::mutex>(
                          m_mutexes[chunk_index % m_mutexes.size()])
                    : std::unique_lock<std::mutex>();

    ChunkPair &chunk_pair = m_chunk_files_on_disk[chunk_index];
    if (is_build_chunk) {
      return chunk_pair.build_chunk.WriteRowToChunk(buffer, row_has_match,
                                                    tables);
    } else {
      return chunk_pair.probe_chunk.WriteRowToChunk(buffer, row_has_match,
                                                    tables);
    }
  }

 private:
  Mem_root_array<ChunkPair> m_chunk_files_on_disk;

  /// Which row we currently are reading from each of the hash join chunk file.
  ha_rows m_build_chunk_current_row = 0;
  ha_rows m_probe_chunk_current_row = 0;

  /// Which pair of HashJoinChunks, if any, we are currently reading from.
  int m_current_chunk{-1};

  /// A vector of mutexes that is used to protect each individual ChunkPair.
  /// Having multiple mutexes is to reduce mutex contention compared to having a
  /// global mutex.
  std::vector<std::mutex> m_mutexes;

  /// A mutex for protecting global operations.
  std::mutex m_global_mutex;

  const bool m_needs_mutex_protection;
};

#endif
