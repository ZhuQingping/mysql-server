#ifndef SQL_ITERATORS_HASH_JOIN_BUFFER_H_
#define SQL_ITERATORS_HASH_JOIN_BUFFER_H_

/* Copyright (c) 2019, 2024, Oracle and/or its affiliates.

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

/// @file
///
/// This file contains the HashJoinRowBuffer class and related
/// functions/classes.
///
/// A HashJoinBuffer is a row buffer that can hold a certain amount of rows.
/// The rows are stored in a hash table, which allows for constant-time lookup.
/// The HashJoinBuffer maintains its own internal MEM_ROOT, where all of the
/// data is allocated.
///
/// The HashJoinBuffer contains an operand with rows from one or more tables,
/// keyed on the value we join on. Consider the following trivial example:
///
///   SELECT t1.data FROM t1 JOIN t2 ON (t1.key = t2.key);
///
/// Let us say that the table "t2" is stored in a HashJoinBuffer. In this case,
/// the hash table key will be the value found in "t2.key", since that is the
/// join condition that belongs to t2. If we have multiple equalities, they
/// will be concatenated together in order to form the hash table key. The hash
/// table key is a std::string_view.
///
/// In order to store a row, we use the function StoreFromTableBuffers. See the
/// comments attached to the function for more details.
///
/// The amount of memory a HashJoinBuffer instance can use is limited by the
/// system variable "join_buffer_size". However, note that we check whether we
/// have exceeded the memory limit _after_ we have inserted data into the row
/// buffer. As such, we will probably use a little bit more memory than
/// specified by join_buffer_size.
///
/// The primary use case for these classes is, as the name implies,
/// for implementing hash join.

#include <assert.h>
#include <stddef.h>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "my_alloc.h"
#include "sql/immutable_string.h"
#include "sql/item_cmpfunc.h"
#include "sql/pack_rows.h"
#include "sql_string.h"

class HashJoinCondition;
class THD;

namespace HashJoin {
class PQHashJoinSharedContext;
}

namespace hash_join_buffer {

/// The key type for the hash structure in HashJoinRowBuffer.
///
/// A key consists of the value from one or more columns, taken from the join
/// condition(s) in the query.  E.g., if the join condition is
/// (t1.col1 = t2.col1 AND t1.col2 = t2.col2), the key is (col1, col2), with the
/// two key parts concatenated together.
///
/// What the data actually contains depends on the comparison context for the
/// join condition. For instance, if the join condition is between a string
/// column and an integer column, the comparison will be done in a string
/// context, and thus the integers will be converted to strings before storing.
/// So the data we store in the key are in some cases converted, so that we can
/// hash and compare them byte-by-byte (i.e. decimals), while other types are
/// already comparable byte-by-byte (i.e. integers), and thus stored as-is.
///
/// Note that the key data can come from items as well as fields if the join
/// condition is an expression. E.g. if the join condition is
/// UPPER(t1.col1) = UPPER(t2.col1), the join key data will come from an Item
/// instead of a Field.
///
/// The Key class never takes ownership of the data. As such, the user must
/// ensure that the data has the proper lifetime. When storing rows in the row
/// buffer, the data must have the same lifetime as the row buffer itself.
/// When using the Key class for lookups in the row buffer, the same lifetime is
/// not needed; the key object is only needed when the lookup is done.
using Key = std::string_view;

// A row in the hash join buffer is the same as the Key class.
using BufferRow = Key;

// A convenience form of LoadIntoTableBuffers() that also verifies the end
// pointer for us.
void LoadBufferRowIntoTableBuffers(const pack_rows::TableCollection &tables,
                                   BufferRow row);

// A convenience form of the above that also decodes the LinkedImmutableString
// for us.
void LoadImmutableStringIntoTableBuffers(
    const pack_rows::TableCollection &tables, LinkedImmutableString row);

enum class StoreRowResult { ROW_STORED, BUFFER_FULL, FATAL_ERROR };

class HashMapReader;

class KeyEquals {
 public:
  // This is a marker from C++17 that signals to the container that
  // operator() can be called with arguments of which one of the types
  // differs from the container's key type (ImmutableStringWithLength),
  // and thus enables map.find(Key). The type itself does not matter.
  using is_transparent = void;

  bool operator()(const Key &str1,
                  const ImmutableStringWithLength &other) const {
    return str1 == other.Decode();
  }

  bool operator()(const ImmutableStringWithLength &str1,
                  const ImmutableStringWithLength &str2) const {
    return str1 == str2;
  }
};

class KeyHasher {
 public:
  // This is a marker from C++17 that signals to the container that
  // operator() can be called with an argument that differs from the
  // container's key type (ImmutableStringWithLength), and thus enables
  // map.find(Key). The type itself does not matter.
  using is_transparent = void;

  // This is a marker telling ankerl::unordered_dense that the hash function has
  // good quality.
  using is_avalanching = void;

  uint64_t operator()(Key key) const {
    return ankerl::unordered_dense::hash<Key>()(key);
  }

  uint64_t operator()(ImmutableStringWithLength key) const {
    return operator()(key.Decode());
  }
};

class HashJoinRowBuffer {
 public:
  // Construct the buffer. Note that Init() must be called before the buffer can
  // be used.
  HashJoinRowBuffer(pack_rows::TableCollection tables,
                    std::vector<HashJoinCondition> join_conditions,
                    size_t max_mem_available_bytes,
                    HashJoin::PQHashJoinSharedContext *pq_hash_join);

  /// Constructs a HashJoinRowBuffer with specified memory constraints.
  /// This constructor initializes a hash join row buffer with a maximum memory
  /// limit and allocates memory using the provided MEM_ROOT. Currently, this
  /// constructor is primarily used in PTRC scenarios.

  /// @param max_mem_available_bytes The maximum amount of memory (in bytes)
  /// that the buffer is allowed to use.
  /// @param mem_root The memory root (MEM_ROOT*) used for all allocations.
  HashJoinRowBuffer(size_t max_mem_available_bytes,
                    MEM_ROOT *mem_root = nullptr)
      : m_mem_root(mem_root),
        m_hash_map(nullptr),
        m_max_mem_available(max_mem_available_bytes) {}

  ~HashJoinRowBuffer();

  // Initialize the HashJoinRowBuffer so it is ready to store rows. This
  // function can be called multiple times; subsequent calls will only clear the
  // buffer for existing rows. If no MEM_ROOT was provided at construction
  // time, one will be allocated here.
  bool Init(bool is_ptrc = false);

  /// Store the row that is currently lying in the tables record buffers.
  /// The hash map key is extracted from the join conditions that the row buffer
  /// holds.
  ///
  /// @param thd the thread handler
  /// @param reject_duplicate_keys If true, reject rows with duplicate keys.
  ///        If a row is rejected, the function will still return ROW_STORED.
  ///
  /// @retval ROW_STORED the row was stored.
  /// @retval BUFFER_FULL the row was stored, and the buffer is full.
  /// @retval FATAL_ERROR an unrecoverable error occurred (most likely,
  ///         malloc failed). It is the caller's responsibility to call
  ///         my_error().
  StoreRowResult StoreRow(THD *thd, bool reject_duplicate_keys);
  /// This is intended for "Partial Table Result Cache PTRC".
  /// Store the row consisted of specified key and value.
  /// Optionally, use specified positions to store key and value.
  /// To ensure the consistency of the data return order in ptrc, when inserting
  /// row into the row chain of the same key in the hash table, the insertion is
  /// performed at the tail rather than at the head.
  ///
  /// @param key_data hash map key data
  /// @param key_length length of key data
  /// @param key_store_pos position to store key data, or nullptr if none
  /// @param val_data hash map value data
  /// @param val_size length of value data
  /// @param val_store_pos position to store value data, or nullptr if none
  ///
  /// @retval ROW_STORED the row was stored.
  /// @retval BUFFER_FULL the row was stored, and the buffer is full.
  /// @retval FATAL_ERROR an unrecoverable error occured (most likely,
  ///  malloc failed). It is the callers responsibility to call my_error().
  StoreRowResult StoreRow(char *key_data, size_t key_size, char *key_store_pos,
                          char *val_data, size_t val_size, char *val_store_pos);

  size_t size() const;

  bool empty() const;

  HashMapReader all() const;
  HashMapReader pq_find(const Key &key) const;

  std::optional<LinkedImmutableString> find(Key key) const;

  std::optional<LinkedImmutableString> first_row() const;

  LinkedImmutableString LastRowStored() const {
    assert(Initialized());
    return m_last_row_stored;
  }
  size_t LastRowLengthStored() const { return m_last_row_length_stored; }
  bool Initialized() const { return m_hash_map != nullptr; }

  // A wrapper class around ankerl::unordered_dense::segmented_map, so that it
  // can be forward-declared in the header file. This is done to limit the
  // number of files that include directly or indirectly headers from the
  // third-party library.
  class HashMap
      : public ankerl::unordered_dense::segmented_map<ImmutableStringWithLength,
                                                      LinkedImmutableString,
                                                      KeyHasher, KeyEquals> {
   public:
    using ankerl::unordered_dense::segmented_map<
        ImmutableStringWithLength, LinkedImmutableString, KeyHasher,
        KeyEquals>::const_iterator;
    // Inherit the constructors from the base class.
    using ankerl::unordered_dense::segmented_map<
        ImmutableStringWithLength, LinkedImmutableString, KeyHasher,
        KeyEquals>::segmented_map;
  };

  using hash_map_iterator = HashMap::const_iterator;

  bool contains(const Key &key) const { return find(key).has_value(); }

  void erase(Key key);

  ImmutableStringWithLength LastKeyStored() const {
    assert(Initialized());
    return m_last_key_stored;
  }
  size_t LastKeyLengthStored() const { return m_last_key_length_stored; }

 private:
  const std::vector<HashJoinCondition> m_join_conditions;

  // A row can consist of parts from different tables. This structure tells us
  // which tables that are involved.
  const pack_rows::TableCollection m_tables;

  // The MEM_ROOT on which all of the hash table data is allocated. Note that
  // if we have parallel query with multiple shared hash tables, the data each
  // worker inserts into their local hash table is also allocated on this
  // MEM_ROOT.
  MEM_ROOT *m_mem_root;

  // A MEM_ROOT used only for storing the final row (possibly both key and
  // value). The code assumes fairly deeply that inserting a row never fails, so
  // when m_mem_root goes full (we set a capacity on it to ensure that the last
  // allocated block does not get too big), we allocate the very last row on
  // this MEM_ROOT and the signal fullness so that we can start spilling to
  // disk.
  MEM_ROOT m_overflow_mem_root;

  // The hash table where the rows are stored. Note that if we have parallel
  // query with shared hash tables, each worker will store their data on their
  // own hash table (which is this variable).
  std::unique_ptr<HashMap> m_hash_map;

  // A buffer we can use when we are constructing a join key from a join
  // condition. In order to avoid reallocating memory, the buffer never shrinks.
  String m_buffer;
  size_t m_row_size_upper_bound;

  // The maximum size of the buffer, given in bytes.
  const size_t m_max_mem_available;

  // The last row that was stored in the hash table, or nullptr if the hash
  // table is empty. We may have to put this row back into the tables' record
  // buffers if we have a child iterator that expects the record buffers to
  // contain the last row returned by the storage engine (the probe phase of
  // hash join may put any row in the hash table in the tables' record buffer).
  // See HashJoinIterator::BuildHashTable() for an example of this.
  LinkedImmutableString m_last_row_stored{nullptr};
  size_t m_last_row_length_stored{0};
  ImmutableStringWithLength m_last_key_stored{nullptr};
  size_t m_last_key_length_stored{0};

  // True means the MEM_ROOT used is allocated by this object. Which should
  // thus free it at the end of lifetime. Otherwise, the MEM_ROOT is borrowed
  // from the caller of the constructor.
  bool m_owned_mem_root{false};
  // Whether the next insert to the hash table is the first insert.
  bool m_first{true};

  HashJoin::PQHashJoinSharedContext *m_pq_hash_join{nullptr};
  bool HasSharedHashTable() const { return m_pq_hash_join != nullptr; }

  // Store the tail nodes of the singly linked lists of rows corresponding to
  // all keys in m_hash_map, which in theory are completely consistent with the
  // keys of m_hash_map.
  // This member variable ensures that in the PTRC scenario the algorithmic
  // complexity of StoreRow is O(1). The reason for not modifying HashMap to add
  // a value field here is concern about affecting non‑PTRC scenarios.
  std::unique_ptr<HashMap> m_all_last_rows{nullptr};

  // Fetch the relevant fields from each table, and pack them into m_mem_root
  // as a LinkedImmutableString where the “next” pointer points to “next_ptr”.
  // If that does not work (capacity reached), pack into m_overflow_mem_root
  // instead and set “full” to true. If _that_ does not work (fatally out
  // of memory), returns nullptr. Otherwise, returns a pointer to the newly
  // packed string.
  LinkedImmutableString StoreLinkedImmutableStringFromTableBuffers(
      LinkedImmutableString next_ptr, bool *full);
};

/// A class for reading values from one or more hash maps. The main use case for
/// this is for parallel-aware hash join, where each worker must examine the
/// data found in the other workers hash maps.
///
/// The interface is somewhat similar to STL iterators in the sense that after a
/// HashMapReader has been constructed, it is positioned at the first value (if
/// any). Note that Value() should only be called if Valid() returns true.
class HashMapReader {
 public:
  HashMapReader() : m_current_row(nullptr) {}

  /// @returns true if the reader is pointing to a valid value. That is, Value()
  ///          can safely be called.
  bool Valid() const {
    // when reached the end of hash table entries. m_current_row is nullptr
    return m_idx < m_iterators.size() && m_current_row != nullptr;
  }

  /// Go to the next value.
  void Next() {
    if (!Valid()) {
      // If we already are at the end, do not try to proceed further.
      assert(false);
      return;
    }

    // Step 1: traverse the linked list
    m_current_row = m_current_row.Decode().next;
    if (m_current_row != nullptr) {
      return;
    }

    // Step 2: traverse current hash map
    ++m_current;
    if (m_current != m_iterators[m_idx].second) {
      m_current_key = m_current->first;
      m_current_row = m_current->second;
      return;
    }

    // Step 3: traverse between hash maps
    while (++m_idx < m_iterators.size()) {
      m_current = m_iterators[m_idx].first;
      if (m_current != m_iterators[m_idx].second) {
        m_current_key = m_current->first;
        m_current_row = m_current->second;
        break;
      }
    }
  }

  /// @returns the value we currently are positioned at. The returned value is
  ///          only valid if "Valid()" returns true.
  const LinkedImmutableString &Value() const {
    assert(Valid());
    return m_current_row;
  }

  const ImmutableStringWithLength &GetKey() const {
    assert(Valid());
    return m_current_key;
  }

  /// Reset the contents of the reader to a clean state. Must be called before
  /// the memory of the hash map(s) this reader points to is released.
  void Reset() {
    m_iterators.clear();
    m_current_row = LinkedImmutableString{nullptr};
    {
      // Due to a bug in LLVM, we have to introduce a non-nested alias in order
      // to call the destructor (https://bugs.llvm.org//show_bug.cgi?id=12350).
      using iterator = hash_join_buffer::HashJoinRowBuffer::hash_map_iterator;
      m_current.~iterator();
    }
  }

  /// @returns a reader for reading all values in all the given hash maps.
  static HashMapReader all(
      const std::vector<const HashJoinRowBuffer::HashMap *> &hash_maps) {
    std::vector<std::pair<HashJoinRowBuffer::hash_map_iterator,
                          HashJoinRowBuffer::hash_map_iterator>>
        ranges;
    for (const auto &map : hash_maps) {
      ranges.emplace_back(map->begin(), map->end());
    }

    return HashMapReader(ranges);
  }

  /// @returns a reader for reading all values matching the given key in all the
  ///          given hash maps.
  static HashMapReader find(
      const std::vector<const HashJoinRowBuffer::HashMap *> &hash_maps,
      const Key &key) {
    std::vector<std::pair<HashJoinRowBuffer::hash_map_iterator,
                          HashJoinRowBuffer::hash_map_iterator>>
        ranges;
    for (const auto &map : hash_maps) {
      auto iter = map->find(key);
      if (iter == map->end()) {
        ranges.push_back({iter, iter});
      } else {
        auto next_iter = iter + 1;
        ranges.push_back({iter, next_iter});
      }
    }

    return HashMapReader(ranges);
  }

 private:
  explicit HashMapReader(
      const std::vector<std::pair<HashJoinRowBuffer::hash_map_iterator,
                                  HashJoinRowBuffer::hash_map_iterator>>
          &iterators)
      : m_current_row(nullptr), m_iterators(iterators) {
    if (iterators.empty()) {
      return;
    }

    // Position "m_current" to be the first non-end, if any. Regardless of
    // whether m_iterators[m_idx].first->second!= nullptr holds true or not.
    while (m_idx < m_iterators.size() &&
           m_iterators[m_idx].first == m_iterators[m_idx].second) {
      ++m_idx;
    }

    if (m_idx == m_iterators.size()) {
      m_current_key = ImmutableStringWithLength{nullptr};
      m_current_row = LinkedImmutableString{nullptr};
    } else {
      m_current = m_iterators[m_idx].first;
      m_current_key = m_current->first;
      m_current_row = m_current->second;
    }
  }

  /// The value we currently are positioned at.
  ImmutableStringWithLength m_current_key;
  LinkedImmutableString m_current_row;
  HashJoinRowBuffer::hash_map_iterator m_current;

  /// Which of the iterator pairs in "m_iterators" we currently are working on.
  size_t m_idx{0};

  /// A list of ranges, expressed as iterator pairs. Note that a pair can
  /// consist of {end(), end()}, meaning that the range contains no values.
  std::vector<std::pair<HashJoinRowBuffer::hash_map_iterator,
                        HashJoinRowBuffer::hash_map_iterator>>
      m_iterators;
};

}  // namespace hash_join_buffer

#endif  // SQL_ITERATORS_HASH_JOIN_BUFFER_H_
