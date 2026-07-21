/* Copyright (c) 2018, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/iterators/hash_join_iterator.h"

#include <assert.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "field_types.h"
#include "my_alloc.h"
#include "my_bit.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "my_xxhash.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/iterators/row_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/parallel_query/bloom_filter.h"
#include "sql/parallel_query/chunk_files_wrapper.h"
#include "sql/parallel_query/pq_hash_join_shared_context.h"
#include "sql/pfs_batch_mode.h"
#include "sql/query_result.h"
#include "sql/sql_class.h"
#include "sql/sql_list.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/vfd/vfd_manager.h"

using hash_join_buffer::LoadBufferRowIntoTableBuffers;
using hash_join_buffer::LoadImmutableStringIntoTableBuffers;

// An arbitrary hash value for the empty string, to avoid the hash function
// from doing arithmetic on nullptr, which is undefined behavior.
static constexpr size_t kZeroKeyLengthHash = 2669509769;

/// @Returns the VfdManager from the PQ leader's THD, even if this is called
/// from a PQ worker. Note that the memory is still managed by the PQ leader's
/// THD, so do not call delete on the returned object!
static VfdManager *GetVfdManager(const THD *thd) {
  return thd->is_pq_worker() ? thd->pq_context().leader->m_vfd_manager.get()
                             : thd->m_vfd_manager.get();
}

HashJoinIterator::HashJoinIterator(
    THD *thd, unique_ptr_destroy_only<RowIterator> build_input,
    const Prealloced_array<TABLE *, 4> &build_input_tables,
    double estimated_build_rows,
    unique_ptr_destroy_only<RowIterator> probe_input,
    const Prealloced_array<TABLE *, 4> &probe_input_tables, bool store_rowids,
    table_map tables_to_get_rowid_for, size_t max_memory_available,
    const std::vector<HashJoinCondition> &join_conditions,
    bool allow_spill_to_disk, JoinType join_type,
    const Mem_root_array<Item *> &extra_conditions, bool probe_input_batch_mode,
    uint64_t *hash_table_generation, bool use_bloom_filter,
    HashJoin::PQHashJoinSharedContext *pq_hash_join, AccessPath *access_path)
    : RowIterator(thd),
      m_state(State::READING_ROW_FROM_PROBE_ITERATOR),
      m_hash_table_generation(hash_table_generation),
      m_build_input(std::move(build_input)),
      m_probe_input(std::move(probe_input)),
      m_probe_input_tables(probe_input_tables, store_rowids,
                           tables_to_get_rowid_for,
                           /*tables_to_store_contents_of_null_rows_for=*/0),
      m_build_input_tables(build_input_tables, store_rowids,
                           tables_to_get_rowid_for,
                           /*tables_to_store_contents_of_null_rows_for=*/0),
      m_tables_to_get_rowid_for(tables_to_get_rowid_for),
      m_row_buffer(m_build_input_tables, join_conditions, max_memory_available,
                   pq_hash_join),
      m_join_conditions(PSI_NOT_INSTRUMENTED, join_conditions.data(),
                        join_conditions.data() + join_conditions.size()),
      m_estimated_build_rows(estimated_build_rows),
      m_probe_input_batch_mode(probe_input_batch_mode),
      m_allow_spill_to_disk(allow_spill_to_disk),
      m_join_type(join_type),
      m_probe_row_saving_write_file(GetVfdManager(thd)),
      m_probe_row_saving_read_file(GetVfdManager(thd)),
      m_use_bloom_filter(use_bloom_filter),
      m_pq_hash_join(pq_hash_join),
      m_access_path(access_path) {
  if (m_pq_hash_join != nullptr) {
    m_pq_hash_join->IncrementNumberOfConstructedWorkers();

    // Attach to the ChunkFiles given by PQHashJoinSharedContext (initialized by
    // the leader) since we want to share ChunkFiles across all workers.
    m_chunk_files = m_pq_hash_join->ChunkFilesOnDisk();
  } else {
    m_chunk_files = std::make_shared<ChunkFilesWrapper>(
        thd->mem_root, /*needs_mutex_protection=*/false, thd->pq_context().dop);
  }

  assert(m_build_input != nullptr);
  assert(m_probe_input != nullptr);

  // If there are multiple extra conditions, merge them into a single AND-ed
  // condition, so evaluation of the item is a bit easier.
  if (extra_conditions.size() == 1) {
    m_extra_condition = extra_conditions[0];
  } else if (extra_conditions.size() > 1) {
    List<Item> items;
    for (Item *cond : extra_conditions) {
      items.push_back(cond);
    }
    m_extra_condition = new Item_cond_and(items);
    m_extra_condition->quick_fix_field();
    m_extra_condition->update_used_tables();
    m_extra_condition->apply_is_true();
  }

  if (pq_hash_join != nullptr) {
    // Unsupported.
    assert(!m_use_bloom_filter);
  }
}

bool HashJoinIterator::InitRowBuffer() {
  m_reader.Reset();

  if (m_row_buffer.Init()) {
    assert(thd()->is_error());  // my_error should have been called.
    return true;
  }

  return false;
}

// Mark that blobs should be copied for each table that contains at least one
// geometry column.
static void MarkCopyBlobsIfTableContainsGeometry(
    const pack_rows::TableCollection &table_collection) {
  for (const pack_rows::Table &table : table_collection.tables()) {
    for (const pack_rows::Column &col : table.columns) {
      if (col.field_type == MYSQL_TYPE_GEOMETRY) {
        table.table->copy_blobs = true;
        break;
      }
    }
  }
}

bool HashJoinIterator::InitProbeIterator() {
  assert(m_state == State::READING_ROW_FROM_PROBE_ITERATOR);

  if (m_probe_input->Init()) {
    return true;
  }

  if (m_probe_input_batch_mode) {
    m_probe_input->StartPSIBatchMode();
  }
  return false;
}

bool HashJoinIterator::Init() {
  // If we are entirely in-memory and the JOIN we are part of hasn't been
  // asked to clear its hash tables since last time, we can reuse the table
  // without having to rebuild it. This is useful if we are on the right side
  // of a nested loop join, ie., we might be scanned multiple times.
  //
  // Note that this only ever happens in the hypergraph optimizer; see comments
  // in CreateIteratorFromAccessPath().
  if (m_row_buffer.Initialized() &&
      (m_hash_join_type == HashJoinType::IN_MEMORY ||
       (m_hash_join_type == HashJoinType::SPILL_TO_DISK &&
        m_chunk_files->Size() == 0)) &&
      m_hash_table_generation != nullptr &&
      *m_hash_table_generation == m_last_hash_table_generation) {
    m_probe_row_match_flag = false;
    m_hash_join_type = HashJoinType::IN_MEMORY;

    // Reset m_current_chunk to -1 to clear previous state.
    if (m_pq_hash_join != nullptr) {
      m_pq_hash_join->ExecutionBarrier().ArriveAndWait(
          [this]() { m_chunk_files->ResetCurrentChunk(); });
    } else {
      m_chunk_files->ResetCurrentChunk();
    }

    if (m_join_type == JoinType::ANTI && m_join_conditions.empty() &&
        m_extra_condition == nullptr && !m_row_buffer.empty()) {
      // See below.
      m_state = State::END_OF_ROWS;
      return false;
    } else {
      m_state = State::READING_ROW_FROM_PROBE_ITERATOR;
      m_probe_input->EndPSIBatchModeIfStarted();
      return InitProbeIterator();
    }
  }

  // Prepare to read the build input into the hash map.
  PrepareForRequestRowId(m_build_input_tables.tables(),
                         m_tables_to_get_rowid_for);
  if (m_build_input->Init()) {
    assert(thd()->is_error() ||
           thd()->killed);  // my_error should have been called.
    return true;
  }

  // We always start out by doing everything in memory.
  m_hash_join_type = HashJoinType::IN_MEMORY;
  m_write_to_probe_row_saving = false;

  m_build_iterator_has_more_rows = true;
  m_probe_input->EndPSIBatchModeIfStarted();
  m_probe_row_match_flag = false;

  // Set up the buffer that is used when
  // a) moving a row between the tables' record buffers, and,
  // b) when constructing a join key from join conditions.
  size_t upper_row_size = 0;
  if (!m_build_input_tables.has_blob_column()) {
    upper_row_size = ComputeRowSizeUpperBound(m_build_input_tables);
  }

  if (!m_probe_input_tables.has_blob_column()) {
    upper_row_size = std::max(upper_row_size,
                              ComputeRowSizeUpperBound(m_probe_input_tables));
  }

  if (m_temporary_row_and_join_key_buffer.reserve(upper_row_size)) {
    my_error(ER_OUTOFMEMORY, MYF(0), upper_row_size);
    return true;  // oom
  }

  // If any of the tables contains a geometry column, we must ensure that
  // the geometry data is copied to the row buffer (see
  // Field_geom::store_internal) instead of only setting the pointer to the
  // data. This is needed if the hash join spills to disk; when we read a row
  // back from chunk file, row data is stored in a temporary buffer. If not told
  // otherwise, Field_geom::store_internal will only store the pointer to the
  // data, and not the data itself. The data this field points to will then
  // become invalid when the temporary buffer is used for something else.
  MarkCopyBlobsIfTableContainsGeometry(m_probe_input_tables);
  MarkCopyBlobsIfTableContainsGeometry(m_build_input_tables);

  // Close any leftover files from previous iterations.
  if (m_pq_hash_join != nullptr) {
    // We need a barrier here to avoid clearing ChunkFiles that was initialized
    // by a previous worker. This can happen if one worker gets to the point
    // where ChunkFiles are initialized before the next worker is launched.
    m_pq_hash_join->ExecutionBarrier().ArriveAndWait(
        [this]() { m_chunk_files->Clear(); });
  } else {
    m_chunk_files->Clear();
  }

  PrepareForRequestRowId(m_probe_input_tables.tables(),
                         m_tables_to_get_rowid_for);

  // If Init() is called multiple times (e.g., if hash join is inside an
  // dependent subquery), we must clear the NULL row flag, as it may have been
  // set by the previous executing of this hash join.
  m_build_input->SetNullRowFlag(/*is_null_row=*/false);

  if (m_pq_hash_join != nullptr) {
    // When storing/restoring rows from the hash table, TABLE::is_created() is
    // used to determine whether we should store/restore row IDs. With parallel
    // query, each worker has its own TABLE object for the same physical table,
    // causing different workers to potentially see different properties from
    // the same table. To illustrate why this can be a problem, consider the
    // following execution tree:
    //
    //                    Weedout
    //                       |
    //              parallel-aware hash join, inner
    //      (build) |                             | (probe)
    //          Hash join, inner                  t3
    //  (build) |             | (probe)
    //          t1        Materialize
    //                        |
    //                       t2
    //
    // t3 is the divided table, and t1 is the cut table. There is a weedout on
    // the top, which forces us to store/restore row IDs (weedout needs them to
    // deduplicate rows). Let us assume that we have two workers; w1 and w2. w1
    // and w2 calls Init() on the parallel-aware hash join, which will trigger a
    // full execution of the build input. Let us further assume that w1 reads 10
    // rows from t1, and w2 reads 0 rows from t1. If the build input is empty
    // for an inner or semi hash join, hash join will skip calling Init() on the
    // probe input as the result from the hash join will be empty regardless of
    // the contents of the probe input. In the illustrated case, not calling
    // Init() will cause the materialized table not to be instantiated. We are
    // now in the problematic case where w1 and w2 will see different result of
    // TABLE::is_created() on t2, causing a mix of rows with and without row IDs
    // in the shared hash table of the parallel-aware hash join. This is
    // problematic when restoring rows; a worker may read a row with row ID when
    // TABLE::is_created() returns false (or vice versa), causing invalid reads
    // when restoring the row. To overcome this issue, ensure that all temporary
    // tables are created so all workers see the same result from
    // TABLE::is_created().
    bool error_on_init = false;
    auto func = [&error_on_init](AccessPath *path, const JOIN *) {
      if (path->type == AccessPath::MATERIALIZE) {
        error_on_init = path->iterator->Init();
      }
      return error_on_init;
    };

    WalkAccessPathsProxy(m_access_path->hash_join().inner,
                         /*cross_query_blocks=*/false, func,
                         /*walk_from_top=*/true);

    if (error_on_init) {
      // If any of the iterators returned error on Init(), assert that it has
      // been reported to the user.
      assert(thd()->is_error());
      return true;
    }
  }

  // Build the hash table
  if (BuildHashTable()) {
    assert(thd()->is_error() ||
           thd()->killed);  // my_error should have been called.
    return true;
  }
  if (m_hash_table_generation != nullptr) {
    m_last_hash_table_generation = *m_hash_table_generation;
  }

  if (m_state == State::END_OF_ROWS) {
    // BuildHashTable() decided that the join is done (the build input is
    // empty, and we are in an inner-/semijoin. Anti-/outer join must output
    // NULL-complemented rows from the probe input).
    return false;
  }

  if (m_join_type == JoinType::ANTI && m_join_conditions.empty() &&
      m_extra_condition == nullptr && !m_row_buffer.empty()) {
    // For degenerate antijoins, we know we will never output anything
    // if there's anything in the hash table, so we can end right away.
    // (We also don't need to read more than one row, but
    // CreateHashJoinAccessPath() has already added a LIMIT 1 for us
    // in this case.)
    m_state = State::END_OF_ROWS;
    return false;
  }

  return InitProbeIterator();
}

// Construct a join key from a list of join conditions, where the join key from
// each join condition is concatenated together in the output buffer
// "join_key_buffer". The function returns true if a SQL NULL value is found.
static bool ConstructJoinKey(
    THD *thd, const Prealloced_array<HashJoinCondition, 4> &join_conditions,
    table_map tables_bitmap, String *join_key_buffer) {
  join_key_buffer->length(0);
  for (const HashJoinCondition &hash_join_condition : join_conditions) {
    if (hash_join_condition.join_condition()->append_join_key_for_hash_join(
            thd, tables_bitmap, hash_join_condition, join_conditions.size() > 1,
            join_key_buffer)) {
      // The join condition returned SQL NULL.
      return true;
    }
    if (thd->is_error()) return true;
  }
  return false;
}

// Write a single row to a HashJoinChunk. The row must lie in the record buffer
// (record[0]) for each involved table. The row is put into one of the chunks in
// the input vector "chunks"; which chunk to use is decided by the hash value of
// the join attribute.
static bool WriteRowToChunk(
    THD *thd, ChunkFilesWrapper *chunks, bool write_to_build_chunk,
    const pack_rows::TableCollection &tables,
    const Prealloced_array<HashJoinCondition, 4> &join_conditions,
    const uint32 xxhash_seed, bool row_has_match,
    bool store_row_with_null_in_join_key, String *join_key_and_row_buffer) {
  assert(!thd->is_error());
  bool null_in_join_key = ConstructJoinKey(
      thd, join_conditions, tables.tables_bitmap(), join_key_and_row_buffer);
  if (thd->is_error()) return true;

  if (null_in_join_key && !store_row_with_null_in_join_key) {
    // NULL values will never match in a inner join or a semijoin. The optimizer
    // will often set up a NULL filter for inner joins, but not in all cases. So
    // we must handle this gracefully instead of asserting.
    return false;
  }

  const uint64_t join_key_hash =
      join_key_and_row_buffer->length() == 0
          ? kZeroKeyLengthHash
          : MY_XXH64(join_key_and_row_buffer->ptr(),
                     join_key_and_row_buffer->length(), xxhash_seed);

  assert((chunks->Size() & (chunks->Size() - 1)) == 0);
  // Since we know that the number of chunks will be a power of two, do a
  // bitwise AND instead of (join_key_hash % chunks->size()).
  const size_t chunk_index = join_key_hash & (chunks->Size() - 1);
  return chunks->WriteToChunkIndex(join_key_and_row_buffer, chunk_index,
                                   write_to_build_chunk, row_has_match, tables);
}

// Write all the remaining rows from the given iterator out to chunk files
// on disk. If the function returns true, an unrecoverable error occurred
// (IO error etc.).
static bool WriteRowsToChunks(
    THD *thd, RowIterator *iterator, const pack_rows::TableCollection &tables,
    const Prealloced_array<HashJoinCondition, 4> &join_conditions,
    const uint32 xxhash_seed, ChunkFilesWrapper *chunks,
    bool write_to_build_chunk, bool write_rows_with_null_in_join_key,
    table_map tables_to_get_rowid_for, String *join_key_buffer) {
  for (;;) {  // Termination condition within loop.
    int res = iterator->Read();
    if (res == 1) {
      assert(thd->is_error() ||
             thd->killed);  // my_error should have been called.
      return true;
    }

    if (res == -1) {
      return false;  // EOF; success.
    }

    assert(res == 0);
    RequestRowId(tables.tables(), tables_to_get_rowid_for);
    if (WriteRowToChunk(thd, chunks, write_to_build_chunk, tables,
                        join_conditions, xxhash_seed, /*row_has_match=*/false,
                        write_rows_with_null_in_join_key, join_key_buffer)) {
      assert(thd->is_error());  // my_error should have been called.
      return true;
    }
  }
}

// Initialize all HashJoinChunks for both inputs. When estimating how many
// chunks we need, we first assume that the estimated row count from the planner
// is correct. Furthermore, we assume that the current row buffer is
// representative of the overall row density, so that if we divide the
// (estimated) number of remaining rows by the number of rows read so far and
// use that as our chunk count, we will get on-disk chunks that each will fit
// into RAM when we read them back later. As a safeguard, we subtract a small
// percentage (reduction factor), since we'd rather get one or two extra chunks
// instead of having to re-read the probe input multiple times. We limit the
// number of chunks per input, so we don't risk hitting the server's limit for
// number of open files.
static bool InitializeChunkFiles(size_t estimated_rows_produced_by_join,
                                 size_t rows_in_hash_table,
                                 size_t max_chunk_files,
                                 bool include_match_flag_for_probe,
                                 ChunkFilesWrapper *chunk_files,
                                 VfdManager *vfd_manager) {
  constexpr double kReductionFactor = 0.9;
  const double reduced_rows_in_hash_table =
      std::max<double>(1, rows_in_hash_table * kReductionFactor);

  // Avoid underflow, since the hash table may contain more rows than the
  // estimate from the planner.
  const size_t remaining_rows =
      std::max(rows_in_hash_table, estimated_rows_produced_by_join) -
      rows_in_hash_table;

  const size_t chunks_needed = std::max<size_t>(
      1, std::ceil(remaining_rows / reduced_rows_in_hash_table));
  const size_t num_chunks = std::min(max_chunk_files, chunks_needed);

  // Ensure that the number of chunks is always a power of two. This allows
  // us to do some optimizations when calculating which chunk a row should
  // be placed in.
  const size_t num_chunks_pow_2 = my_round_up_to_next_power(num_chunks);
  assert(chunk_files != nullptr && chunk_files->Size() == 0);
  for (size_t i = 0; i < num_chunks_pow_2; ++i) {
    ChunkPair chunk_pair(vfd_manager);
    chunk_pair.build_chunk.Init(/*uses_match_flags=*/false);
    chunk_pair.probe_chunk.Init(include_match_flag_for_probe);

    chunk_files->AddChunkPair(std::move(chunk_pair));
  }
  return false;
}

void HashJoinIterator::CreateBloomFilter() {
  if (!m_use_bloom_filter) {
    return;
  }

  // Have an upper limit on the Bloom filter size to avoid it from exploding
  // with large inputs.
  const size_t MAX_NUM_BITS = 41943040;                    // ~5 megabytes
  const double FALSE_POSITIVE_PROBABILITY_TARGET = 0.005;  // 0.5%

  // Note that we should use the number of distinct elements/keys in the hash
  // map, and not the total number of elements. However, we do not currently
  // have any way of finding the number of distinct keys efficiently, so we use
  // the total number of elements for now.
  const size_t elements_in_hash_table = m_row_buffer.size();

  size_t num_bits = std::min(
      MAX_NUM_BITS, GetNumBitsInFilter(elements_in_hash_table,
                                       FALSE_POSITIVE_PROBABILITY_TARGET));
  size_t num_hash_functions =
      std::min(MAX_NUM_HASH_FUNCTIONS,
               GetNumHashFunctions(elements_in_hash_table,
                                   FALSE_POSITIVE_PROBABILITY_TARGET));

  // Clear any previous state that the Bloom filter might have.
  m_bloom_filter.Init(num_bits, num_hash_functions);

  const double estimated_false_positive_probability =
      GetFalsePositiveProbability(num_bits, elements_in_hash_table,
                                  num_hash_functions);

  // If the Bloom filter is estimated to filter out less than 50% of the rows,
  // do not build the Bloom filter as we expect it to have very little (or
  // possibly negative) effect during the probe phase.
  if (estimated_false_positive_probability >= 0.5) {
    m_bloom_filter.Init(/*num_bits=*/0, /*num_hash_functions=*/0);
  } else {
    auto reader = m_row_buffer.all();
    while (reader.Valid()) {
      const auto &key = reader.GetKey().Decode();
      m_bloom_filter.AddValue(key.data(), key.length());
      reader.Next();
    }
  }
}

bool HashJoinIterator::BuildHashTable() {
  if (thd()->killed) {  // Aborted by user.
    thd()->send_kill_message();
    return 1;
  }

  if (m_pq_hash_join != nullptr) {
    // Make sure to reset "has_more_data" inside a barrier completion function.
    // If we were to do it without the barrier/completion function, the
    // following could happen:
    // - Worker 1 starts executing, calls Init() -> BuildHashTable(), and sets
    //   "has_more_data" to false.
    // - Worker 1 runs BuildHashTable() and runs out of memory. To signal this
    //   to other workers it sets "has_more_data" to true.
    // - Worker 2 starts executing, calls Init() -> BuildHashTable(), and sets
    //   "has_more_data" to false. We have now overwritten the value set by
    //   worker 1!
    m_pq_hash_join->ExecutionBarrier().ArriveAndWait([this]() {
      m_pq_hash_join->SetHasMoreDataInBuildInput(/*has_more_data=*/false);
      m_pq_hash_join->ClearHashMaps();
    });
  }

  auto switch_parallel_query_state = create_scope_guard([this]() {
    if (m_pq_hash_join != nullptr) {
      // If this is a parallel-aware hash join, wait for other threads to finish
      // the build phase before proceeding to the probe phase.
      m_pq_hash_join->ExecutionBarrier().ArriveAndWait(
          /*completion_function=*/nullptr);

      // Have all workers agreed that the build input is fully consumed?
      if (!m_pq_hash_join->HasMoreDataInBuildInput()) {
        m_build_iterator_has_more_rows = false;
        // As we managed to read to the end of the build iterator, this is the
        // last time we will read from the probe iterator. Thus, we can disable
        // probe row saving again (it was enabled if the hash table ran out of
        // memory _and_ we were not allowed to spill to disk).
        m_write_to_probe_row_saving = false;

        // All workers are done with building the hash table.
        // If the hash table is empty, the result of inner joins and semijoins
        // will also be empty. However, if the build input was empty, the output
        // of antijoins will be all the rows from the probe input.
        if (m_row_buffer.empty() && m_join_type != JoinType::ANTI &&
            m_join_type != JoinType::OUTER) {
          m_state = State::END_OF_ROWS;
        }
      } else if (m_join_type != JoinType::INNER) {
        // Enable probe row saving, so that unmatched probe rows are written
        // to the probe row saving file. After the next refill of the hash
        // table, we will read rows from the probe row saving file, ensuring
        // that we only read unmatched probe rows.
        InitWritingToProbeRowSavingFile();
      }
    }
  });

  if (!m_build_iterator_has_more_rows) {
    m_state = State::END_OF_ROWS;
    return false;
  }

  // Restore the last row that was inserted into the row buffer. This is
  // necessary if the build input is a nested loop with a filter on the inner
  // side, like this:
  //
  //        +---Hash join---+
  //        |               |
  //  Nested loop          t1
  //  |         |
  //  t3    Filter: (t3.i < t2.i)
  //               |
  //              t2
  //
  // If the hash join is not allowed to spill to disk, we may need to re-fill
  // the hash table multiple times. If the nested loop happens to be in the
  // state "reading inner rows" when a re-fill is triggered, the filter will
  // look at the data in t3's record buffer in order to evaluate the filter. The
  // row in t3's record buffer may be any of the rows that was stored in the
  // hash table, and not the last row returned from t3. To ensure that the
  // filter is looking at the correct data, restore the last row that was
  // inserted into the hash table.
  if (m_row_buffer.Initialized() && m_row_buffer.LastRowStored() != nullptr) {
    pack_rows::LoadIntoTableBuffers(
        m_build_input_tables, reinterpret_cast<const uchar *>(
                                  m_row_buffer.LastRowStored().Decode().data));
  }

  if (InitRowBuffer()) {
    return true;
  }

  const bool reject_duplicate_keys = RejectDuplicateKeys();

  // After the hash table has been built, create a Bloom filter. This is delayed
  // after the hash table has been built so that we know exactly how many rows
  // there will be in the Bloom filter. This gives us more accurate estimates
  // when calculating the Bloom filter size and the number of hash functions.
  //
  // Since there are multiple return points in this function, we use a scope
  // guard to ensure that the Bloom filter is created in all scenarios.
  auto create_bloom_filter_guard = create_scope_guard([this]() {
    if (thd()->is_error() || thd()->killed) {
      return;
    }
    CreateBloomFilter();
  });

  PFSBatchMode batch_mode(m_build_input.get());
  for (;;) {  // Termination condition within loop.
    int res = m_build_input->Read();
    if (res == 1) {
      assert(thd()->is_error() ||
             thd()->killed);  // my_error should have been called.
      return true;
    }

    if (res == -1) {
      if (m_pq_hash_join == nullptr) {
        // As we managed to read to the end of the build iterator, this is the
        // last time we will read from the probe iterator. Thus, we can disable
        // probe row saving again (it was enabled if the hash table ran out of
        // memory _and_ we were not allowed to spill to disk). If this is a
        // parallel hash join, other workers may add rows to the shared hash
        // table, so it may not be the end.
        m_write_to_probe_row_saving = false;

        // Note that for parallel-aware hash join, we cannot abort the join if
        // the build input is empty. All workers are sharing the same hash
        // table, which means that data from the probe input may match rows that
        // other workers have inserted into the hash table.
        m_build_iterator_has_more_rows = false;
      }

      SetReadingProbeRowState();

      // If the build input was empty, the result of inner joins and semijoins
      // will also be empty. However, if the build input was empty, the output
      // of antijoins will be all the rows from the probe input. Note that we
      // cannot determine this for parallel hash join at this point, since other
      // workers may not have started reading from the build input. So setting
      // the state to EOF is delayed until all workers are done with the build
      // phase, which is synchronized in the parallel hash join scope guard.
      if (m_row_buffer.empty() && m_join_type != JoinType::ANTI &&
          m_join_type != JoinType::OUTER && m_pq_hash_join == nullptr) {
        m_state = State::END_OF_ROWS;
      }

      return false;
    }
    assert(res == 0);
    RequestRowId(m_build_input_tables.tables(), m_tables_to_get_rowid_for);

    const hash_join_buffer::StoreRowResult store_row_result =
        m_row_buffer.StoreRow(thd(), reject_duplicate_keys);
    switch (store_row_result) {
      case hash_join_buffer::StoreRowResult::ROW_STORED:
        break;
      case hash_join_buffer::StoreRowResult::BUFFER_FULL: {
        // The row buffer is full, so start spilling to disk (if allowed). Note
        // that the row buffer checks for OOM _after_ the row was inserted, so
        // we should always manage to insert at least one row.
        assert(!m_row_buffer.empty());

        if (m_pq_hash_join != nullptr) {
          // Notify other workers that there are more data to be read from the
          // build input.
          m_pq_hash_join->SetHasMoreDataInBuildInput(/*has_more_data=*/true);
        }

        // If we are not allowed to spill to disk, just go on to reading from
        // the probe iterator.
        if (!m_allow_spill_to_disk) {
          if (m_pq_hash_join == nullptr && m_join_type != JoinType::INNER) {
            // Enable probe row saving, so that unmatched probe rows are written
            // to the probe row saving file. After the next refill of the hash
            // table, we will read rows from the probe row saving file, ensuring
            // that we only read unmatched probe rows.
            InitWritingToProbeRowSavingFile();
          }
          SetReadingProbeRowState();
          return false;
        }

        // Disable the Bloom filter now that we switch to an on-disk hash join.
        // The reason is that the Bloom filter is created from the rows in the
        // hash table. Now we will have rows that goes to disk instead of the
        // hash table. With such "incomplete" Bloom filter we will filter out
        // rows from the probe input that instead should have been written out
        // to disk for later processing.
        m_use_bloom_filter = false;

        if (m_pq_hash_join != nullptr) {
          // The first worker that gets to this point will have the
          // responsibility of initializing the necessary ChunkFiles. Note that
          // we multiply the estimated number of build rows with the number of
          // workers, since the number of build rows is per worker. Since the
          // ChunkFiles are shared across all workers, all rows from all workers
          // will be placed in the same set of ChunkFiles. Therefore, we must
          // size the number of chunk files based on the total number of rows
          // from all workers.
          // A next worker coming here, after acquiring the mutex, will notice
          // that there are chunk files and properly skip initialization.

          const std::lock_guard<std::mutex> lock(
              m_pq_hash_join->HashTableMutex());
          if (m_chunk_files->Size() == 0) {
            if (InitializeChunkFiles(
                    static_cast<size_t>(m_estimated_build_rows *
                                        thd()->pq_context().dop),
                    m_row_buffer.size(), kMaxChunks,
                    /*include_match_flag_for_probe=*/m_join_type ==
                        JoinType::OUTER,
                    m_chunk_files.get(), GetVfdManager(thd()))) {
              assert(thd()->is_error());  // my_error should have been called.
              return true;
            }
          }
        } else if (InitializeChunkFiles(
                       m_estimated_build_rows, m_row_buffer.size(), kMaxChunks,

                       /*include_match_flag_for_probe=*/m_join_type ==
                           JoinType::OUTER,
                       m_chunk_files.get(), GetVfdManager(thd()))) {
          assert(thd()->is_error());  // my_error should have been called.
          return true;
        }

        // Write out the remaining rows from the build input out to chunk files.
        // The probe input will be written out to chunk files later; we will do
        // it _after_ we have checked the probe input for matches against the
        // rows that are already written to the hash table. An alternative
        // approach would be to write out the remaining rows from the build
        // _and_ the rows that already are in the hash table. In that case, we
        // could also write out the entire probe input to disk here as well. But
        // we don't want to waste the rows that we already have stored in
        // memory.
        //
        // We never write out rows with NULL in condition for the build/right
        // input, as these rows will never match in a join condition.
        if (WriteRowsToChunks(thd(), m_build_input.get(), m_build_input_tables,
                              m_join_conditions, kChunkPartitioningHashSeed,
                              m_chunk_files.get(),
                              true /* write_to_build_chunks */,
                              false /* write_rows_with_null_in_join_key */,
                              m_tables_to_get_rowid_for,
                              &m_temporary_row_and_join_key_buffer)) {
          assert(thd()->is_error() ||
                 thd()->killed);  // my_error should have been called.
          return true;
        }

        // The build chunks are now positioned at the end. We will rewind them
        // in ReadNextHashJoinChunk() right before we start reading from them.
        SetReadingProbeRowState();
        return false;
      }
      case hash_join_buffer::StoreRowResult::FATAL_ERROR:
        // An unrecoverable error. Most likely, malloc failed, so report OOM.
        // Note that we cannot say for sure how much memory we tried to allocate
        // when failing, so just report 'join_buffer_size' as the amount of
        // memory we tried to allocate.
        my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR),
                 thd()->variables.join_buff_size);
        return true;
    }
  }
}

bool HashJoinIterator::ReadNextHashJoinChunk() {
  bool rewind_error = false;
  if (m_pq_hash_join != nullptr) {
    // Parallel-aware hash join: The last worker to arrive at this barrier will
    // decide whether to move to the next ChunkPair. If we move, it will also
    // ensure that the new build chunk is positioned at the beginning.
    m_pq_hash_join->ExecutionBarrier().ArriveAndWait([this, &rewind_error]() {
      if (m_chunk_files->PossiblyMoveToNextChunk()) {
        rewind_error = m_chunk_files->RewindCurrentBuildChunk();
      }

      m_pq_hash_join->ClearHashMaps();
    });
  } else {
    if (m_chunk_files->PossiblyMoveToNextChunk()) {
      rewind_error = m_chunk_files->RewindCurrentBuildChunk();
    }
  }

  if (rewind_error) {
    assert(thd()->is_error());  // my_error should have been called.
    return true;
  }

  if (m_last_seen_chunk_index != m_chunk_files->CurrentChunkIndex()) {
    // Since we are moving to a new set of chunk files, ensure that we read from
    // the chunk file and not from the probe row saving file.
    m_read_from_probe_row_saving = false;
    m_last_seen_chunk_index = m_chunk_files->CurrentChunkIndex();
  }

  if (m_chunk_files->CurrentChunkIndex() ==
      static_cast<int>(m_chunk_files->Size())) {
    // We have moved past the last chunk, so we are done.
    m_state = State::END_OF_ROWS;
    return false;
  }

  if (InitRowBuffer()) {
    return true;
  }

  const bool reject_duplicate_keys = RejectDuplicateKeys();

  for (;;) {
    // Read the next row from the chunk file, and put it in the in-memory row
    // buffer. If the buffer goes full, do the probe phase against the rows we
    // managed to put in the buffer and continue reading where we left in the
    // next iteration.
    ChunkFilesWrapper::LoadRowResult res = m_chunk_files->LoadNextRow(
        &m_temporary_row_and_join_key_buffer,
        /*matched=*/nullptr,
        /*is_build_chunk=*/true, m_build_input_tables);

    if (res == ChunkFilesWrapper::LoadRowResult::END_OF_ROWS) {
      break;
    }

    if (res == ChunkFilesWrapper::LoadRowResult::ERROR) {
      assert(thd()->is_error());  // my_error should have been called.
      return true;
    }

    assert(res == ChunkFilesWrapper::LoadRowResult::ROW_READY);

    hash_join_buffer::StoreRowResult store_row_result =
        m_row_buffer.StoreRow(thd(), reject_duplicate_keys);

    if (store_row_result == hash_join_buffer::StoreRowResult::BUFFER_FULL) {
      // The row buffer checks for OOM _after_ the row was inserted, so we
      // should always manage to insert at least one row.
      assert(!m_row_buffer.empty());
      break;
    } else if (store_row_result ==
               hash_join_buffer::StoreRowResult::FATAL_ERROR) {
      // An unrecoverable error. Most likely, malloc failed, so report OOM.
      // Note that we cannot say for sure how much memory we tried to allocate
      // when failing, so just report 'join_buffer_size' as the amount of
      // memory we tried to allocate.
      my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR),
               thd()->variables.join_buff_size);
      return true;
    }

    assert(store_row_result == hash_join_buffer::StoreRowResult::ROW_STORED);
  }

  // Prepare to do a lookup in the hash table for all rows from the probe
  // chunk.
  rewind_error = false;
  if (m_pq_hash_join != nullptr) {
    m_pq_hash_join->ExecutionBarrier().ArriveAndWait([this, &rewind_error]() {
      rewind_error = m_chunk_files->RewindCurrentProbeChunk();
    });
  } else {
    rewind_error = m_chunk_files->RewindCurrentProbeChunk();
  }

  if (rewind_error) {
    assert(thd()->is_error());  // my_error should have been called.
    return true;
  }
  SetReadingProbeRowState();

  if (m_chunk_files->HasMoreDataInBuildChunk() &&
      m_join_type != JoinType::INNER) {
    // The build chunk did not fit into memory, causing us to refill the hash
    // table once the probe input is consumed. If we don't take any special
    // action, we can end up outputting the same probe row twice if the probe
    // phase finds a match in both iterations through the hash table.
    // By enabling probe row saving, unmatched probe rows are written to a probe
    // row saving file. After the next hash table refill, we load the probe rows
    // from the probe row saving file instead of from the build chunk, and thus
    // ensuring that we only see unmatched probe rows. Note that we have not
    // started reading probe rows yet, but we are about to do so.
    InitWritingToProbeRowSavingFile();
  } else {
    m_write_to_probe_row_saving = false;
  }

  return false;
}

bool HashJoinIterator::ReadRowFromProbeIterator() {
  assert(m_chunk_files->CurrentChunkIndex() == -1);

  int result = m_probe_input->Read();
  if (result == 1) {
    assert(thd()->is_error() ||
           thd()->killed);  // my_error should have been called.
    return true;
  }

  if (result == 0) {
    RequestRowId(m_probe_input_tables.tables(), m_tables_to_get_rowid_for);

    // A row from the probe iterator is ready.
    LookupProbeRowInHashTable();
    if (thd()->is_error()) return true;
    return false;
  }

  assert(result == -1);
  m_probe_input->EndPSIBatchModeIfStarted();

  // The probe iterator is out of rows. We may be in three different situations
  // here (ordered from most common to less common):
  // 1. The build input is also empty, and the join is done. The iterator state
  //    will go into "LOADING_NEXT_CHUNK_PAIR", and we will see that there are
  //    no chunk files when trying to load the next pair of chunk files.
  // 2. We have degraded into an on-disk hash join, and we will now start
  //    reading from chunk files on disk.
  // 3. The build input is not empty, and we have not degraded into an on-disk
  //    hash join (i.e. we were not allowed due to a LIMIT in the query),
  //    re-populate the hash table with the remaining rows from the build input.
  if (m_allow_spill_to_disk) {
    m_hash_join_type = HashJoinType::SPILL_TO_DISK;
    m_state = State::LOADING_NEXT_CHUNK_PAIR;
    return false;
  }

  m_hash_join_type = HashJoinType::IN_MEMORY_WITH_HASH_TABLE_REFILL;
  if (m_write_to_probe_row_saving) {
    // If probe row saving is enabled, it means that the probe row saving write
    // file contains all the rows from the probe input that should be
    // read/processed again. We must swap the probe row saving writing and probe
    // row saving reading file _before_ calling BuildHashTable, since
    // BuildHashTable may initialize (and thus clear) the probe row saving write
    // file, losing any rows written to said file.
    if (InitReadingFromProbeRowSavingFile()) {
      assert(thd()->is_error());  // my_error should have been called.
      return true;
    }
  }

  if (BuildHashTable()) {
    assert(thd()->is_error() ||
           thd()->killed);  // my_error should have been called.
    return true;
  }

  switch (m_state) {
    case State::END_OF_ROWS:
      // BuildHashTable() decided that the join is done (the build input is
      // empty, and we are in an inner-/semijoin. Anti-/outer join must output
      // NULL-complemented rows from the probe input).
      return false;
    case State::READING_ROW_FROM_PROBE_ITERATOR:
      // Start reading from the beginning of the probe iterator.
      return InitProbeIterator();
    case State::READING_ROW_FROM_PROBE_ROW_SAVING_FILE:
      // The probe row saving read file is already initialized for reading
      // further up in this function.
      return false;
    default:
      assert(false);
      return true;
  }
}

bool HashJoinIterator::ReadRowFromProbeChunkFile() {
  assert(on_disk_hash_join() && m_chunk_files->CurrentChunkIndex() != -1);

  // Read one row from the current HashJoinChunk, and put
  // that row into the record buffer of the probe input table.
  ChunkFilesWrapper::LoadRowResult res = m_chunk_files->LoadNextRow(
      &m_temporary_row_and_join_key_buffer, &m_probe_row_match_flag,
      /*is_build_chunk=*/false, m_probe_input_tables);

  if (res == ChunkFilesWrapper::LoadRowResult::END_OF_ROWS) {
    // No more rows in the current probe chunk, so load the next chunk of
    // build rows into the hash table.
    if (m_write_to_probe_row_saving) {
      // If probe row saving is enabled, the build chunk did not fit in memory.
      // This causes us to refill the hash table with the rows from the build
      // chunk that did not fit, and thus read the probe chunk multiple times.
      // This can be problematic for semijoin; we do not want to output a probe
      // row that has a match in both parts of the hash table. To mitigate
      // this, we write probe rows that does not have a match in the hash table
      // to a probe row saving file (m_probe_row_saving_write_file), and read
      // from said file instead of from the probe input the next time.
      if (InitReadingFromProbeRowSavingFile()) {
        assert(thd()->is_error());  // my_error should have been called.
        return true;
      }
    } else {
      m_read_from_probe_row_saving = false;
    }

    m_state = State::LOADING_NEXT_CHUNK_PAIR;
    return false;
  } else if (res == ChunkFilesWrapper::LoadRowResult::ERROR) {
    assert(thd()->is_error());  // my_error should have been called.
    return true;
  }

  assert(res == ChunkFilesWrapper::LoadRowResult::ROW_READY);

  // A row from the chunk file is ready.
  LookupProbeRowInHashTable();
  return false;
}

bool HashJoinIterator::ReadRowFromProbeRowSavingFile() {
  // Read one row from the probe row saving file, and put that row into the
  // record buffer of the probe input table.
  if (m_probe_row_saving_read_file_current_row >=
      m_probe_row_saving_read_file.num_rows()) {
    // We are done reading all the rows from the probe row saving file. If probe
    // row saving is still enabled, we have a new set of rows in the probe row
    // saving write file.
    if (m_write_to_probe_row_saving) {
      if (InitReadingFromProbeRowSavingFile()) {
        assert(thd()->is_error());  // my_error should have been called.
        return true;
      }
    } else {
      m_read_from_probe_row_saving = false;
    }

    // If we are executing an on-disk hash join, go and load the next pair of
    // chunk files. If we are doing everything in memory with multiple hash
    // table refills, go and refill the hash table.
    if (m_hash_join_type == HashJoinType::SPILL_TO_DISK) {
      m_state = State::LOADING_NEXT_CHUNK_PAIR;
      return false;
    }
    assert(m_hash_join_type == HashJoinType::IN_MEMORY_WITH_HASH_TABLE_REFILL);

    // No more rows in the probe row saving file.
    if (BuildHashTable()) {
      assert(thd()->is_error() ||
             thd()->killed);  // my_error should have been called.
      return true;
    }

    if (m_state == State::END_OF_ROWS) {
      // BuildHashTable() decided that the join is done (the build input is
      // empty).
      return false;
    }

    SetReadingProbeRowState();
    return false;
  } else if (m_probe_row_saving_read_file.LoadRowFromChunk(
                 &m_temporary_row_and_join_key_buffer, &m_probe_row_match_flag,
                 m_probe_input_tables)) {
    assert(thd()->is_error());  // my_error should have been called.
    return true;
  }

  m_probe_row_saving_read_file_current_row++;

  // A row from the chunk file is ready.
  LookupProbeRowInHashTable();
  return false;
}

void HashJoinIterator::LookupProbeRowInHashTable() {
  if (m_join_conditions.empty()) {
    // Skip the call to equal_range in case we don't have any join conditions.
    // This can save up to 20% in case of multi-table joins.
    m_reader = m_row_buffer.all();
    m_state = State::READING_FIRST_ROW_FROM_HASH_TABLE;
    return;
  }

  // Extract the join key from the probe input, and use that key as the lookup
  // key in the hash table.
  bool null_in_join_key = ConstructJoinKey(
      thd(), m_join_conditions, m_probe_input_tables.tables_bitmap(),
      &m_temporary_row_and_join_key_buffer);

  if (null_in_join_key || (m_use_bloom_filter &&
                           !m_bloom_filter.PossiblyExists(
                               m_temporary_row_and_join_key_buffer.ptr(),
                               m_temporary_row_and_join_key_buffer.length()))) {
    if (m_join_type == JoinType::ANTI || m_join_type == JoinType::OUTER) {
      // SQL NULL was found, or the Bloom filter told us that the value
      // definitely does not exist, so we will never find a matching row in the
      // hash table. Let us indicate that, so that a null-complemented row is
      // returned.
      m_reader.Reset();
      m_state = State::READING_FIRST_ROW_FROM_HASH_TABLE;
    } else {
      SetReadingProbeRowState();
    }
    return;
  }

  hash_join_buffer::Key key{m_temporary_row_and_join_key_buffer.ptr(),
                            m_temporary_row_and_join_key_buffer.length()};

  // No need to use equal_range now since there's no multimap
  // anymore
  m_reader = m_row_buffer.pq_find(key);
  m_state = State::READING_FIRST_ROW_FROM_HASH_TABLE;
}

int HashJoinIterator::ReadJoinedRow() {
  if (!m_reader.Valid()) {
    // Signal that we have reached the end of hash table entries. Let the caller
    // determine which state we end up in.
    return -1;
  }

  // A row is ready in the hash table, so put the data from the hash table row
  // into the record buffers of the build input tables.
  pack_rows::LoadIntoTableBuffers(
      m_build_input_tables,
      reinterpret_cast<const uchar *>(m_reader.Value().Decode().data));
  return 0;
}

bool HashJoinIterator::WriteProbeRowToDiskIfApplicable() {
  // If we are spilling to disk, we need to match the row against rows from
  // the build input that are written out to chunk files. So we need to write
  // the probe row to chunk files as well. Semijoin/antijoin has an exception to
  // this; if the probe input already got a match in the hash table, we do not
  // need to write it out to disk. Outer joins should always write the row out
  // to disk, since the probe/left input should return NULL-complemented rows
  // even if the join condition contains SQL NULL.
  if (m_state == State::READING_FIRST_ROW_FROM_HASH_TABLE) {
    const bool found_match = m_reader.Valid();

    if ((m_join_type == JoinType::INNER || m_join_type == JoinType::OUTER) ||
        !found_match) {
      if (on_disk_hash_join() && m_chunk_files->CurrentChunkIndex() == -1) {
        // For inner joins and semijoins, we can skip probe rows that have a
        // NULL in the join key, unless the join condition uses NULL-safe equal
        // (<=>), because we know that it won't have any match in the build
        // table. For left outer join and antijoin, however, rows in the
        // outer/probe table which have no match in the inner/build table, will
        // be part of the join result, so we can't skip rows with NULLs for
        // those join types. Hence, store_row_with_null_in_join_key must be true
        // for left outer join and antijoin.
        const bool store_row_with_null_in_join_key =
            m_join_type == JoinType::OUTER || m_join_type == JoinType::ANTI;
        if (WriteRowToChunk(thd(), m_chunk_files.get(),
                            false /* write_to_build_chunk */,
                            m_probe_input_tables, m_join_conditions,
                            kChunkPartitioningHashSeed, found_match,
                            store_row_with_null_in_join_key,
                            &m_temporary_row_and_join_key_buffer)) {
          return true;
        }
      }

      if (m_write_to_probe_row_saving &&
          m_probe_row_saving_write_file.WriteRowToChunk(
              &m_temporary_row_and_join_key_buffer,
              found_match || m_probe_row_match_flag, m_probe_input_tables)) {
        return true;
      }
    }
  }

  return false;
}

bool HashJoinIterator::JoinedRowPassesExtraConditions() const {
  if (m_extra_condition != nullptr) {
    return m_extra_condition->val_int() != 0;
  }

  return true;
}

int HashJoinIterator::ReadNextJoinedRowFromHashTable() {
  int res;
  bool passes_extra_conditions = false;
  do {
    res = ReadJoinedRow();

    // ReadJoinedRow() can only return 0 (row is ready) or -1 (EOF).
    assert(res == 0 || res == -1);

    // Evaluate any extra conditions that are attached to this iterator before
    // we return a row.
    if (res == 0) {
      passes_extra_conditions = JoinedRowPassesExtraConditions();
      if (thd()->is_error() || thd()->killed) {
        // Evaluation of extra conditions raised an error, so abort the join.
        return 1;
      }

      if (!passes_extra_conditions) {
        // Advance to the next matching row in the hash table. Note that the
        // iterator stays in the state READING_FIRST_ROW_FROM_HASH_TABLE even
        // though we are not actually reading the first row anymore. This is
        // because WriteProbeRowToDiskIfApplicable() needs to know if this is
        // the first row that matches both the join condition and any extra
        // conditions; only unmatched rows will be written to disk.
        m_reader.Next();
      }
    }
  } while (res == 0 && !passes_extra_conditions);

  // The row passed all extra conditions (or we are out of rows in the hash
  // table), so we can now write the row to disk.
  // Inner and outer joins: Write out all rows from the probe input (given that
  //   we have degraded into on-disk hash join).
  // Semijoin and antijoin: Write out rows that do not have any matching row in
  //   the hash table.
  if (WriteProbeRowToDiskIfApplicable()) {
    return 1;
  }

  if (res == -1) {
    // If we did not find a matching row in the hash table, antijoin and outer
    // join should output the last row read from the probe input together with a
    // NULL-complemented row from the build input. However, in case of on-disk
    // antijoin, a row from the probe input can match a row from the build input
    // that has already been written out to disk. So for on-disk antijoin, we
    // cannot output any rows until we have started reading from chunk files.
    //
    // On-disk outer join is a bit more tricky; we can only output a
    // NULL-complemented row if the probe row did not match anything from the
    // build input while doing any of the probe phases. We can have multiple
    // probe phases if e.g. a build chunk file is too big to fit in memory; we
    // would have to read the build chunk in multiple smaller chunks while doing
    // a probe phase for each of these smaller chunks. To keep track of this,
    // each probe row is prefixed with a match flag in the chunk files.
    bool return_null_complemented_row = false;
    if ((on_disk_hash_join() && m_chunk_files->CurrentChunkIndex() == -1) ||
        m_write_to_probe_row_saving) {
      return_null_complemented_row = false;
    } else if (m_join_type == JoinType::ANTI) {
      return_null_complemented_row = true;
    } else if (m_join_type == JoinType::OUTER &&
               m_state == State::READING_FIRST_ROW_FROM_HASH_TABLE &&
               !m_probe_row_match_flag) {
      return_null_complemented_row = true;
    }

    SetReadingProbeRowState();

    if (return_null_complemented_row) {
      m_build_input->SetNullRowFlag(true);
      return 0;
    }
    return -1;
  }

  // We have a matching row ready.
  switch (m_join_type) {
    case JoinType::SEMI:
      // Semijoin should return the first matching row, and then go to the next
      // row from the probe input.
      SetReadingProbeRowState();
      break;
    case JoinType::ANTI:
      // Antijoin should immediately go to the next row from the probe input,
      // without returning the matching row.
      SetReadingProbeRowState();
      return -1;  // Read the next row.
    case JoinType::OUTER:
    case JoinType::INNER:
      // Inner join should return all matching rows from the hash table before
      // moving to the next row from the probe input.
      m_state = State::READING_FROM_HASH_TABLE;
      break;
    case JoinType::FULL_OUTER:
      assert(false);
  }

  m_reader.Next();
  return 0;
}

int HashJoinIterator::Read() {
  for (;;) {
    if (thd()->killed) {  // Aborted by user.
      thd()->send_kill_message();
      return 1;
    }

    switch (m_state) {
      case State::LOADING_NEXT_CHUNK_PAIR:
        if (ReadNextHashJoinChunk()) {
          return 1;
        }
        break;
      case State::READING_ROW_FROM_PROBE_ITERATOR:
        if (ReadRowFromProbeIterator()) {
          return 1;
        }
        break;
      case State::READING_ROW_FROM_PROBE_CHUNK_FILE:
        if (ReadRowFromProbeChunkFile()) {
          return 1;
        }
        break;
      case State::READING_ROW_FROM_PROBE_ROW_SAVING_FILE:
        if (ReadRowFromProbeRowSavingFile()) {
          return 1;
        }
        break;
      case State::READING_FIRST_ROW_FROM_HASH_TABLE:
      case State::READING_FROM_HASH_TABLE: {
        const int res = ReadNextJoinedRowFromHashTable();
        if (res == 0) {
          // A joined row is ready, so send it to the client.
          return 0;
        }

        if (res == -1) {
          // No more matching rows in the hash table, or antijoin found a
          // matching row. Read a new row from the probe input.
          continue;
        }

        // An error occurred, so abort the join.
        assert(res == 1);
        return res;
      }
      case State::END_OF_ROWS:
        return -1;
    }
  }

  // Unreachable.
  assert(false);
  return 1;
}

bool HashJoinIterator::InitWritingToProbeRowSavingFile() {
  m_write_to_probe_row_saving = true;
  m_probe_row_saving_write_file.Init(m_join_type == JoinType::OUTER);

  // Rewind the file, so that the next write starts from the beginning.
  return m_probe_row_saving_write_file.Rewind();
}

bool HashJoinIterator::InitReadingFromProbeRowSavingFile() {
  m_probe_row_saving_read_file = std::move(m_probe_row_saving_write_file);
  m_probe_row_saving_read_file_current_row = 0;
  m_read_from_probe_row_saving = true;
  // No need to call "Rewind" as the move assignment operator will implicitly
  // close both files.
  return false;
}

void HashJoinIterator::SetReadingProbeRowState() {
  switch (m_hash_join_type) {
    case HashJoinType::IN_MEMORY:
      m_state = State::READING_ROW_FROM_PROBE_ITERATOR;
      break;
    case HashJoinType::IN_MEMORY_WITH_HASH_TABLE_REFILL:
      if (m_join_type == JoinType::INNER) {
        // As inner joins does not need probe row match flags, probe row saving
        // will never be activated for inner joins.
        m_state = State::READING_ROW_FROM_PROBE_ITERATOR;
      } else {
        m_state = State::READING_ROW_FROM_PROBE_ROW_SAVING_FILE;
      }
      break;
    case HashJoinType::SPILL_TO_DISK:
      if (m_read_from_probe_row_saving) {
        // Probe row saving may be activated if a build chunk did not fit in
        // memory.
        m_state = State::READING_ROW_FROM_PROBE_ROW_SAVING_FILE;
        return;
      }
      m_state = State::READING_ROW_FROM_PROBE_CHUNK_FILE;
      break;
  }
}
