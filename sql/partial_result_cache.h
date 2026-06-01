#ifndef PARTIAL_RESULT_CACHE_H
#define PARTIAL_RESULT_CACHE_H

/* Copyright (c) 2021, 2025, Huawei and/or its affiliates.

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

#include "my_alloc.h"
#include "my_base.h"
#include "sql/item.h"
#include "sql/item_subselect.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/iterators/row_iterator.h"
#include "sql/mem_root_array.h"
#include "sql/sql_class.h"

struct AccessPath;

using pack_rows::TableCollection;

namespace ptrc {

using namespace hash_join_buffer;

using Key_pair = std::pair<std::pair<const char *, size_t> &,
                           std::pair<const char *, size_t> &>;

struct Instrumentation {
  /**
    Number of rescans where we've found the scan parameter values to be
    cached.
  */
  uint64_t cache_hits{0};
  /**
    Number of rescans where we've not found the scan parameter values to be
    cached.
  */
  uint64_t cache_misses{0};
  /// Number of cache entries removed due to the need to free memory
  uint64_t cache_evictions{0};
  /**
    Number of times we've had to bypass the cache when filling it due to
    not being able to free enough space to store the current scan's tuples.
  */
  uint64_t cache_overflows{0};
  /// peak memory usage in bytes
  uint64_t mem_used{0};
};

struct LRU_node {
  void set_data(const char *k, size_t ks, const char *r, size_t rs) {
    // Here, we replace our two Key-s by new objects. Key has const members,
    // so it has no assignment operator. That's why placement-new is used.
    // As we overwrite the object, verify that no destructor needs to be
    // called first.
    static_assert(
        std::is_trivially_destructible<ImmutableStringWithLength>::value,
        "destructor of ImmutableStringWithLength should be called");
    static_assert(std::is_trivially_destructible<LinkedImmutableString>::value,
                  "destructor of LinkedImmutableString should be called");
    new (&key_data) ImmutableStringWithLength(k, /*is_pad_to_max_length=*/true);
    new (&row_data) LinkedImmutableString(r);
    key_size = ks;
    row_size = rs;
  }
  LRU_node *prev{nullptr}, *next{nullptr};
  ImmutableStringWithLength key_data{nullptr, /*is_pad_to_max_length=*/true};
  size_t key_size = 0;
  LinkedImmutableString row_data{nullptr};
  size_t row_size = 0;
};

struct LRU_list_type {
  LRU_node *head{nullptr};
  LRU_node *tail{nullptr};
  std::list<Key_pair *> free_list;
};

struct PTRC_context {
  PTRC_context() : item_addr(new Item_uint(0)), exec_flags(new Item_uint(0)) {}
  /// Allocations in constructor may fail.
  bool is_valid() const { return item_addr && exec_flags; }
  /// It's used to help save address of LRU node. In order to save to create
  /// this item when create LRU_node, here we use a unified item_addr to help
  /// save address of LRU_node.
  Item_uint *item_addr;
  /// This set of flags is used to help record some information during ptrc
  /// execution.
  /// @see NO_MATCHED_ROW and WAS_NULL
  Item_uint *exec_flags;
};

struct PathParameters {
  ~PathParameters();
  /// Columns related with hash keys
  TableCollection *key_tables{nullptr};
  /// Columns related with result records
  TableCollection *res_tables{nullptr};
  mem_root_deque<Item *> *res_items{nullptr};
  TABLE *res_tmp_table{nullptr};  ///< @See PtrcIterator::m_res_tmp_table
  /// @See PtrcIterator::pseudo_context_table
  TABLE *pseudo_context_table{nullptr};
  /// Pointer to Item_in_subselect::was_null if current subquery is
  /// Item_in_subselect instance. Otherwise, nullptr.
  bool *was_null{nullptr};
  /// True means PtrcIterator works for EXISTS subquery.
  bool is_exists_subquery{false};
  void set_was_null(Item_in_subselect *sub);
};

/**
  Keeps track of PTRC's MEM_ROOT and its related objects.

  Access paths must have trivial destructors, which are not called. But PTRC's
  access path contains a PathParameters which has a non-trivial destructor
  (deep inside, it has a Prealloced_array which can allocate memory from the
  heap). This last object is recorded in an array here, so we can destroy it.
*/
struct Memory_objects {
  Memory_objects() : root(), saved_root(&root), params(&root) {}
  ~Memory_objects() {
    for (auto *p : params) destroy(p);
  }
  MEM_ROOT root;  ///< Where PTRC allocates most of its memory
  /// A 'backup' pointer for when we temporarily replace THD's MEM_ROOT with
  /// ours
  MEM_ROOT *saved_root;
  /// Allocates its cells in 'root'. Declared after 'root', so that it is
  /// destroyed before 'root'.
  Mem_root_array<PathParameters *> params;
};

/// Partial result cache running status.
enum class Ptrc_status {
  NONE,              ///< Initialized status
  LOOKUP,            ///< Attempt to perform a cache lookup
  FETCH_NEXT_TUPLE,  ///< Get another tuple from the cache
  FILLING_CACHE,     ///< Read inner node to fill cache
  BYPASS_MODE,       ///< Bypass mode. Just read from our
                     ///< inner node without caching anything
  END_OF_SCAN,       ///< Ready for rescan
};

class PtrcIterator : public RowIterator {
 public:
  PtrcIterator(THD *thd, TableCollection *key_tables,
               TableCollection *res_tables,
               unique_ptr_destroy_only<RowIterator> source,
               mem_root_deque<Item *> *res_items, TABLE *res_tmp_tabl,
               bool *was_null, bool need_fill_res_tables);

  ~PtrcIterator() override;

  bool Init() override;

  int Read() override;

  int End() override { return m_source_iterator->End(); }

  // TODO mysql 8.0.41 ready removed RowIterator::End()
  // int End() override { return m_source_iterator->End(); }

  void StartPSIBatchMode() override { m_source_iterator->StartPSIBatchMode(); }

  void EndPSIBatchModeIfStarted() override {
    m_source_iterator->EndPSIBatchModeIfStarted();
  }
  bool InitRowBuffer();
  void SetNullRowFlag(bool is_null_row) override {
    m_source_iterator->SetNullRowFlag(is_null_row);
  }
  /// Since we cached the row, and may return it another time, keep it locked
  void UnlockRow() override {}
  bool FillTmpTable();
  void ChangeToUseTmpField();
  void RestoreCacheItems();
  void DisablePtrc() { change_status(Ptrc_status::BYPASS_MODE); }
  bool IsDisabled() const { return status == Ptrc_status::BYPASS_MODE; }

 public:
  // Columns related with hash keys
  TableCollection *m_key_tables{nullptr};
  // Columns related with result records
  TableCollection *m_res_tables{nullptr};
  Instrumentation stats;

 private:
  // Current session
  THD *m_thd;
  // Ptrc execution status
  Ptrc_status status{Ptrc_status::NONE};
  LRU_list_type LRU_list;
  // Used to mark LRU node created for current PTRC cache record
  LRU_node *current_LRU_node{nullptr};
  // Helper to store PTRC context information. LRU node address will be
  // appended to the end of the cached record. This is used for quick update
  // on LRU list. And a bit will also be appended to signal that there was a
  // match or none.
  TABLE *pseudo_context_table{nullptr};

  // The iterator we are reading records from.
  unique_ptr_destroy_only<RowIterator> m_source_iterator;
  // Hash table to cache result set.
  HashJoinRowBuffer *m_row_buffer{nullptr};

  LinkedImmutableString m_current_row{nullptr};
  // Used to record select_list which are needed to be evaluated and cached.
  mem_root_deque<Item *> *m_res_items{nullptr};
  // This virtual table is used for creating Fields from select_list. If PTRC
  // is for correlated subquery, such a tmp table is used to help to store
  // Item list into cached result set.
  TABLE *m_res_tmp_table{nullptr};
  // Keep old select_list because we need a switch when using a tmp table to
  // help to cache result set. When PTRC needs get result set from
  // m_source_iterator, saved_res_items should be used. When READ or cache
  // result set from PTRC, tmp table fields will be used.
  mem_root_deque<Item *> *saved_res_items{nullptr};
  // Item_field list created by temporary table fields, used to replace
  // original select_list to return values.
  mem_root_deque<Item *> *cached_res_fields{nullptr};
  size_t num_hidden_fields = 0;
  // String buffer used to cache intermediate result of one record.
  String key_buff, res_buff;
  PTRC_context *ptrc_context{nullptr};
  // Reference comments on PathParameters::was_null.
  bool *m_was_null{nullptr};
  // True means res_tables should be filled with result set of
  // m_source_iterator::Read. EXISTS subqueries won't need such a process
  // because they only care whether one row is returned but not calculate
  // anything in select list.
  bool m_need_fill_res_tables{true};

 private:
  bool insert_into_LRU_list();
  void update_LRU_list();
  Key_pair *LRU_free_mem(THD *thd, const Key &key, size_t res_size);
  void change_status(Ptrc_status s) { status = s; }
};

AccessPath *CreateAccessPath(THD *thd, Item_subselect *sub,
                             AccessPath *inner_path,
                             qep_tab_map ptrc_key_tables,
                             qep_tab_map ptrc_res_tables, JOIN *ptrc_join);

void SetCostOnPtrcPath(THD *thd, JOIN *join, qep_tab_map outer_tables,
                       AccessPath *path);

inline bool is_ptrc_enabled(const THD *thd) {
  return thd->optimizer_switch_flag(OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE);
}
void print(const AccessPath *path, String *str, THD *thd);
inline double get_min_cost_threshold(const THD *thd) {
  return thd->variables.partial_result_cache_cost_threshold;
}
void cleanup(THD *thd);
}  // namespace ptrc
#endif /* PARTIAL_RESULT_CACHE */
