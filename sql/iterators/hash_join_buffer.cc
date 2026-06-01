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

#include "sql/iterators/hash_join_buffer.h"

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <ankerl/unordered_dense.h>

#include "my_alloc.h"
#include "my_compiler.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "securec.h"
#include "sql/item_cmpfunc.h"
#include "sql/parallel_query/pq_hash_join_shared_context.h"
#include "sql/psi_memory_key.h"
#include "sql/sql_class.h"
#include "template_utils.h"

using pack_rows::TableCollection;

namespace hash_join_buffer {

LinkedImmutableString
HashJoinRowBuffer::StoreLinkedImmutableStringFromTableBuffers(
    LinkedImmutableString next_ptr, bool *full) {
  size_t row_size_upper_bound = m_row_size_upper_bound;
  if (m_tables.has_blob_column()) {
    // The row size upper bound can have changed.
    row_size_upper_bound = ComputeRowSizeUpperBound(m_tables);
  }

  const size_t required_value_bytes =
      LinkedImmutableString::RequiredBytesForEncode(row_size_upper_bound);

  std::pair<char *, char *> block = m_mem_root->Peek();
  if (static_cast<size_t>(block.second - block.first) < required_value_bytes) {
    // No room in this block; ask for a new one and try again.
    m_mem_root->ForceNewBlock(required_value_bytes);
    block = m_mem_root->Peek();
  }
  bool committed = false;
  char *start_of_value, *dptr;
  LinkedImmutableString ret{nullptr};
  if (static_cast<size_t>(block.second - block.first) >= required_value_bytes) {
    dptr = start_of_value = block.first;
  } else {
    dptr = start_of_value =
        pointer_cast<char *>(m_overflow_mem_root.Alloc(required_value_bytes));
    if (dptr == nullptr) {
      return LinkedImmutableString{nullptr};
    }
    committed = true;
    *full = true;
  }

  ret = LinkedImmutableString::EncodeHeader(next_ptr, &dptr);

  const char *const start_of_row [[maybe_unused]] = dptr;
  dptr = pointer_cast<char *>(
      StoreFromTableBuffersRaw(m_tables, pointer_cast<uchar *>(dptr)));
  assert(dptr <= start_of_row + row_size_upper_bound);

  const size_t actual_length = dptr - start_of_value;
  assert(actual_length <= required_value_bytes);
  if (!committed) {
    m_mem_root->RawCommit(actual_length);
  }
  return ret;
}

// A convenience form of LoadIntoTableBuffers() that also verifies the end
// pointer for us.
void LoadBufferRowIntoTableBuffers(const TableCollection &tables,
                                   BufferRow row) {
  const uchar *data = pointer_cast<const uchar *>(row.data());
  const uchar *end [[maybe_unused]] = LoadIntoTableBuffers(tables, data);
  assert(end == data + row.size());
}

void LoadImmutableStringIntoTableBuffers(const TableCollection &tables,
                                         LinkedImmutableString row) {
  LoadIntoTableBuffers(tables, pointer_cast<const uchar *>(row.Decode().data));
}

HashJoinRowBuffer::HashJoinRowBuffer(
    TableCollection tables, std::vector<HashJoinCondition> join_conditions,
    size_t max_mem_available, HashJoin::PQHashJoinSharedContext *pq_hash_join)
    : m_join_conditions(std::move(join_conditions)),
      m_tables(std::move(tables)),
      m_mem_root(nullptr),
      m_overflow_mem_root(key_memory_hash_join, 256),
      m_hash_map(nullptr),
      m_max_mem_available(
          std::max<size_t>(max_mem_available, 16384 /* 16 kB */)),
      m_pq_hash_join(pq_hash_join) {}

// Define the destructor here instead of in the header, so that the header can
// forward declare types of member variables (m_hash_map in particular).

HashJoinRowBuffer::~HashJoinRowBuffer() {
  if (m_owned_mem_root) delete m_mem_root;
}

bool HashJoinRowBuffer::Init(bool is_ptrc) {
  // Reset the unique_ptr, so that the hash map destructors are called
  // before clearing the MEM_ROOT.
  if (!m_mem_root) {
    if (!(m_mem_root = new MEM_ROOT(key_memory_hash_join, 16384 /* 16 kB */))) {
      my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 16384);
      return true;
    }
    m_owned_mem_root = true;
  }

  const int64_t pre_allocated_memory =
      static_cast<int64_t>(m_mem_root->allocated_size());

  if (m_hash_map != nullptr) {
    // Reset the unique_ptr, so that the hash map destructors are called before
    // clearing the MEM_ROOT.
    m_hash_map.reset(nullptr);
    m_mem_root->Clear();
    // Limit is being applied only after the first row.
    m_mem_root->set_max_capacity(0);
    m_overflow_mem_root.ClearForReuse();

    // Now that the destructors are finished and the MEM_ROOT is cleared,
    // we can allocate a new hash map.
    // Note: Mem_root_allocator::allocate will throw std::bad_alloc if mem_root
    // is full. But hash_map_type doesn't throw std::bad_alloc. So we can't
    // cache std::bad_alloc here.
  }

  if (m_all_last_rows != nullptr) m_all_last_rows.reset(nullptr);
  if (is_ptrc) {
    m_all_last_rows.reset(new HashMap());
    if (m_all_last_rows == nullptr) {
      my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), sizeof(*m_all_last_rows));
      return true;
    }
  }

  // NOTE: Will be ignored and re-calculated if there are any blobs in the
  // table.
  m_row_size_upper_bound = ComputeRowSizeUpperBound(m_tables);

  // Now that the destructors are finished and the MEM_ROOT is cleared,
  // we can allocate a new hash map.
  m_hash_map.reset(new HashMap());
  if (m_hash_map == nullptr) {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), sizeof(*m_hash_map));
    return true;
  }

  if (HasSharedHashTable()) {
    const int64_t post_allocated_memory =
        static_cast<int64_t>(m_mem_root->allocated_size());
    m_pq_hash_join->RegisterHashMap(m_hash_map.get());
    // We call MemoryFull to _decrease_ the memory consumption recorded in
    // m_pq_hash_join::m_bytes_allocated. MemoryFull will usually return "false"
    // here, but there may be situations where it returns true. Consider the
    // following scenario:
    //
    // 1) First of all, the MEM_ROOTs used by hash join has a block size of 16kB
    //    to avoid many small allocations. This block size may very well be
    //    larger than the smallest possible value for join_buffer_size, which is
    //    128 bytes.
    // 2) When we call this function (HashJoinRowBuffer::Init), we will
    //    construct a hash table on the MEM_ROOT
    //    (HashJoinRowBuffer::m_mem_root). This will allocate some data, and
    //    since the MEM_ROOT block size is 16kB it will allocate at least 16kB.
    // 3) If we have set join_buffer_size to 128 bytes, the below call to
    //    MemoryFull will always return "true" because we will always have
    //    allocated at least 16kB.
    m_pq_hash_join->MemoryFull(pre_allocated_memory, post_allocated_memory);
  }

  m_last_row_stored = LinkedImmutableString{nullptr};

  // Mark that the next insert into the hash table will be the first insert.
  m_first = true;
  m_last_row_length_stored = 0;
  m_last_key_stored = ImmutableStringWithLength{nullptr};
  m_last_key_length_stored = 0;
  return false;
}

StoreRowResult HashJoinRowBuffer::StoreRow(THD *thd,
                                           bool reject_duplicate_keys) {
  const size_t pre_memory_allocated = m_mem_root->allocated_size();
  bool full = false;

  // Make the key from the join conditions.
  m_buffer.length(0);
  for (const HashJoinCondition &hash_join_condition : m_join_conditions) {
    bool null_in_join_condition =
        hash_join_condition.join_condition()->append_join_key_for_hash_join(
            thd, m_tables.tables_bitmap(), hash_join_condition,
            m_join_conditions.size() > 1, &m_buffer);

    if (thd->is_error()) {
      // An error was raised while evaluating the join condition.
      return StoreRowResult::FATAL_ERROR;
    }

    if (null_in_join_condition) {
      // One of the components of the join key had a NULL value, and
      // that component was part of an equality predicate (=), *not* a
      // NULL-safe equality predicate, so it can never match a row in
      // the other table. There's no need to store the row in the hash
      // table. Skip it.
      return StoreRowResult::ROW_STORED;
    }
  }

  // Store the key in the MEM_ROOT. Note that we will only commit the memory
  // usage for it if the key was a new one (see the call to emplace() below)..
  const size_t required_key_bytes =
      ImmutableStringWithLength::RequiredBytesForEncode(m_buffer.length());
  ImmutableStringWithLength key;

  std::pair<char *, char *> block = m_mem_root->Peek();
  if (static_cast<size_t>(block.second - block.first) < required_key_bytes) {
    // No room in this block; ask for a new one and try again.
    m_mem_root->ForceNewBlock(required_key_bytes);
    block = m_mem_root->Peek();
  }
  size_t bytes_to_commit = 0;
  if (static_cast<size_t>(block.second - block.first) >= required_key_bytes) {
    char *ptr = block.first;
    key = ImmutableStringWithLength::Encode(m_buffer.ptr(), m_buffer.length(),
                                            &ptr);
    assert(ptr < block.second);
    bytes_to_commit = ptr - block.first;
  } else {
    char *ptr =
        pointer_cast<char *>(m_overflow_mem_root.Alloc(required_key_bytes));
    if (ptr == nullptr) {
      return StoreRowResult::FATAL_ERROR;
    }
    key = ImmutableStringWithLength::Encode(m_buffer.ptr(), m_buffer.length(),
                                            &ptr);
    // Keep bytes_to_commit == 0; the value is already committed.
  }

  std::pair<HashMap::iterator, bool> key_it_and_inserted;
  try {
    key_it_and_inserted =
        m_hash_map->emplace(key, LinkedImmutableString{nullptr});
  } catch (const std::overflow_error &) {
    // This can only happen if the hash function is extremely bad
    // (should never happen in practice).
    return StoreRowResult::FATAL_ERROR;
  }
  LinkedImmutableString next_ptr{nullptr};
  if (key_it_and_inserted.second) {
    // Here we only count memory usage for normal scenario. When parallel query
    // with shared hash tables, each worker will store their data on their own
    // hash table, so We will calculate the parallel memory usage separately.
    if (!HasSharedHashTable()) {
      // We inserted an element, so the hash table may have grown.
      // Update the capacity available for the MEM_ROOT; our total may
      // have gone slightly over already, and if so, we will signal
      // that and immediately start spilling to disk.
      const size_t bytes_used =
          m_hash_map->bucket_count() * sizeof(HashMap::bucket_type) +
          m_hash_map->values().capacity() *
              sizeof(HashMap::value_container_type::value_type);
      if (bytes_used >= m_max_mem_available) {
        // 0 means no limit, so set the minimum possible limit.
        m_mem_root->set_max_capacity(1);
        full = true;
      } else {
        m_mem_root->set_max_capacity(m_max_mem_available - bytes_used);
      }
    }

    // We need to keep this key.
    m_mem_root->RawCommit(bytes_to_commit);
  } else {
    if (reject_duplicate_keys) {
      return StoreRowResult::ROW_STORED;
    }
    // We already have another element with the same key, so our insert
    // failed, Put the new value in the hash bucket, but keep track of
    // what the old one was; it will be our “next” pointer.
    next_ptr = key_it_and_inserted.first->second;
  }

  // Save the contents of all columns marked for reading.
  m_last_row_stored = key_it_and_inserted.first->second =
      StoreLinkedImmutableStringFromTableBuffers(next_ptr, &full);

  if (m_last_row_stored == nullptr) {
    return StoreRowResult::FATAL_ERROR;
  }

  if (HasSharedHashTable()) {
    m_pq_hash_join->IncrementGlobalHashMapCount();
  }

  const size_t post_memory_allocated = m_mem_root->allocated_size();

  // Why don't we always call MemoryFull() in case of parallel-aware hash join?
  // Most of the time, the allocation size will not change since the MEM_ROOT
  // usually will allocate a lot more memory than requested to avoid many small
  // allocations. And since the function MemoryFull has an atomic, we do not
  // want to call this function more than necessary.
  //
  // Note that we always call MemoryFull() after the first insert to the hash
  // table. The reason is:
  // - When we call HashJoinRowBuffer::Init(), we will clear the MEM_ROOT and
  //   allocate a new hash table on said MEM_ROOT.
  // - Since the MEM_ROOT block size is ~16kB, this will allocate 16kB on this
  //   MEM_ROOT.
  // - This means that we will have to have more than 16kB of data inserted to
  //   the hash table before the condition "pre_memory_allocated !=
  //   post_memory_allocated" happens. More specifically, the condition will be
  //   satisified only when a new block of data is allocated on the MEM_ROOT.
  // - If we now have a low join_buffer_size (it can be as low as 128 bytes), it
  //   will take quite some inserts before we realize that our memory budget has
  //   been exceeded even before we start inserting data into the hash table.
  if (HasSharedHashTable() &&
      (m_first || pre_memory_allocated != post_memory_allocated)) {
    m_first = false;
    if (m_pq_hash_join->MemoryFull(pre_memory_allocated,
                                   post_memory_allocated)) {
      return StoreRowResult::BUFFER_FULL;
    }
    return StoreRowResult::ROW_STORED;
  } else {
    return full ? StoreRowResult::BUFFER_FULL : StoreRowResult::ROW_STORED;
  }
}

HashMapReader HashJoinRowBuffer::pq_find(const Key &key) const {
  if (HasSharedHashTable()) {
    return HashMapReader::find(m_pq_hash_join->HashMaps(), key);
  }

  return HashMapReader::find({m_hash_map.get()}, key);
}

HashMapReader HashJoinRowBuffer::all() const {
  if (HasSharedHashTable()) {
    return HashMapReader::all(m_pq_hash_join->HashMaps());
  }

  return HashMapReader::all({m_hash_map.get()});
}

StoreRowResult HashJoinRowBuffer::StoreRow(char *key_data, size_t key_size,
                                           char *key_store_pos, char *val_data,
                                           size_t val_size,
                                           char *val_store_pos) {
  ImmutableStringWithLength key{nullptr};
  bool first_value = true;
  if (key_store_pos == nullptr) {
    DBUG_EXECUTE_IF(
        "ptrc_sim_allocate_failure_during_cache",
        if (size() > 8) { return StoreRowResult::FATAL_ERROR; });
    // Allocate the join key on the same MEM_ROOT that the hash map is
    // allocated on, so it has the same lifetime as the rest of the contents in
    // the hash map (until Clear() is called on the HashJoinBuffer).  Note that
    // this is not true for parallel query, where the hash map is allocated on
    // the MEM_ROOT in PQHashJoinSharedContext.
    if (key_size > 0) {
      const size_t required_key_bytes =
          ImmutableStringWithLength::RequiredBytesForEncode(key_size);
      key_store_pos = m_mem_root->ArrayAlloc<char>(required_key_bytes);
      if (key_store_pos == nullptr) {
        return StoreRowResult::FATAL_ERROR;
      }
    }
  }
  if (key_size > 0) {
    key = ImmutableStringWithLength::Encode(key_data, key_size, &key_store_pos,
                                            true);
  }

  std::pair<HashMap::iterator, bool> key_it_and_inserted;
  try {
    key_it_and_inserted =
        m_hash_map->emplace(key, LinkedImmutableString{nullptr});
  } catch (const std::overflow_error &) {
    // This can only happen if the hash function is extremely bad
    // (should never happen in practice).
    return StoreRowResult::FATAL_ERROR;
  }
  LinkedImmutableString next_ptr{nullptr};
  if (key_it_and_inserted.second) {
    // Only store the key if the insertion is successful (i.e., there is no
    // identical key in the map).
    m_last_key_stored = key;
    m_last_key_length_stored = key_size;
  } else {
    // We already have another element with the same key, so our insert
    // failed.
    first_value = false;
  }

  LinkedImmutableString value{nullptr};
  const size_t required_value_bytes =
      LinkedImmutableString::RequiredBytesForEncode(val_size);
  if (val_store_pos == nullptr) {
    if (val_size > 0) {
      val_store_pos = m_mem_root->ArrayAlloc<char>(required_value_bytes);
      if (val_store_pos == nullptr) {
        return StoreRowResult::FATAL_ERROR;
      }
    }
    DBUG_EXECUTE_IF(
        "allocate_failure_when_storerow",
        if (size() > 8) { return StoreRowResult::BUFFER_FULL; });
  }
  if (val_size > 0) {
#ifndef NDEBUG
    char *start_of_value = val_store_pos;
#endif /* NDEBUG */
    value = LinkedImmutableString::EncodeFixedHeader(next_ptr, &val_store_pos);

    memcpy_s(val_store_pos, val_size, val_data, val_size);
    val_store_pos += val_size;

#ifndef NDEBUG
    const size_t actual_length = val_store_pos - start_of_value;
    assert(actual_length == required_value_bytes);
#endif /* NDEBUG */
  }

  const auto it = m_all_last_rows->find(
      Key(pointer_cast<const char *>(key_data), key_size));
  if (it != m_all_last_rows->end() && !first_value) {
    LinkedImmutableString tail_row = it->second;
    assert(tail_row != nullptr);
    assert(tail_row.DecodeFixed().next == nullptr);
    char *tail_ptr = const_cast<char *>(tail_row.GetDataPointer());
    // Set the next pointer of the current tail node to point to the new node
    // (i.e., value).
    LinkedImmutableString old_value [[maybe_unused]] =
        LinkedImmutableString::EncodeFixedHeader(value, &tail_ptr);
    assert(old_value.GetDataPointer() == tail_row.GetDataPointer());
  } else {
    // In theory, entering this branch should only occur when
    // 'it == m_all_last_rows->end() && first_value', that is, when adding the
    // first row for a key. If, in extreme cases, this branch is entered,
    // modifying the head node will not affect functionality, but it will impact
    // the cache hit rate of ptrc.
    assert(it == m_all_last_rows->end() && first_value);
    key_it_and_inserted.first->second = value;
  }

  std::pair<HashMap::iterator, bool> last_rows_iter_and_inserted;
  try {
    last_rows_iter_and_inserted = m_all_last_rows->emplace(key, value);
  } catch (const std::overflow_error &) {
    // m_all_last_rows can only potentially trigger a memory exception when
    // adding a new node. Therefore, at this point the head node is directly set
    // to null to avoid inconsistency between m_all_last_rows and m_hash_map.
    key_it_and_inserted.first->second = LinkedImmutableString{nullptr};
    return StoreRowResult::FATAL_ERROR;
  }
  if (!last_rows_iter_and_inserted.second) {
    last_rows_iter_and_inserted.first->second = value;
  }

  m_last_row_stored = value;
  m_last_row_length_stored = val_size;

  if (m_mem_root->allocated_size() > m_max_mem_available) {
    return StoreRowResult::BUFFER_FULL;
  }

  return StoreRowResult::ROW_STORED;
}

size_t HashJoinRowBuffer::size() const {
  return HasSharedHashTable() ? m_pq_hash_join->GlobalHashMapCount()
                              : m_hash_map->size();
}

std::optional<LinkedImmutableString> HashJoinRowBuffer::find(Key key) const {
  const auto it = m_hash_map->find(key);
  if (it == m_hash_map->end()) return {};
  return it->second;
}

std::optional<LinkedImmutableString> HashJoinRowBuffer::first_row() const {
  if (m_hash_map->empty()) return {};
  return m_hash_map->begin()->second;
}

bool HashJoinRowBuffer::empty() const {
  return HasSharedHashTable() ? size() == 0 : m_hash_map->empty();
}

void HashJoinRowBuffer::erase(Key key) {
  m_hash_map->erase(key);
  if (m_all_last_rows != nullptr) m_all_last_rows->erase(key);
  return;
}

}  // namespace hash_join_buffer

// From protobuf.
std::pair<const char *, uint64_t> VarintParseSlow64(const char *p,
                                                    uint32_t res32) {
  uint64_t res = res32;
  for (std::uint32_t i = 2; i < 10; i++) {
    uint64_t x = static_cast<uint8_t>(p[i]);
    res += (x - 1) << (7 * i);
    if (likely(x < 128)) {
      return {p + i + 1, res};
    }
  }
  return {nullptr, 0};
}
