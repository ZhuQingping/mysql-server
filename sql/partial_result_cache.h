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
#include "my_table_map.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/iterators/row_iterator.h"
#include "sql/mem_root_array.h"
#include "sql/pack_rows.h"
#include "sql_string.h"

#include <cstdint>
#include <list>
#include <type_traits>
#include <utility>

class Item;
class Item_subselect;
class Item_in_subselect;
class JOIN;
class String;
class THD;
struct AccessPath;
struct TABLE;
template <class T>
class mem_root_deque;

namespace ptrc {

using namespace hash_join_buffer;
using pack_rows::TableCollection;

using Key_pair = std::pair<std::pair<const char *, size_t>,
                           std::pair<const char *, size_t>>;

struct Instrumentation {
  uint64_t cache_hits{0};
  uint64_t cache_misses{0};
  uint64_t cache_evictions{0};
  uint64_t cache_overflows{0};
  uint64_t mem_used{0};
};

struct LRU_node {
  void set_data(const char *key, size_t key_length, const char *row,
                size_t row_length) {
    static_assert(
        std::is_trivially_destructible<ImmutableStringWithLength>::value,
        "ImmutableStringWithLength must be trivially destructible");
    static_assert(std::is_trivially_destructible<LinkedImmutableString>::value,
                  "LinkedImmutableString must be trivially destructible");
    new (&key_data)
        ImmutableStringWithLength(key, /*is_pad_to_max_length=*/true);
    new (&row_data) LinkedImmutableString(row);
    key_size = key_length;
    row_size = row_length;
  }

  LRU_node *prev{nullptr};
  LRU_node *next{nullptr};
  ImmutableStringWithLength key_data{nullptr,
                                     /*is_pad_to_max_length=*/true};
  size_t key_size{0};
  LinkedImmutableString row_data{nullptr};
  size_t row_size{0};
};

struct LRU_list_type {
  LRU_node *head{nullptr};
  LRU_node *tail{nullptr};
  std::list<Key_pair *> free_list;
};

struct PTRC_context;

struct PathParameters {
  ~PathParameters();

  TableCollection *key_tables{nullptr};
  TableCollection *res_tables{nullptr};
  mem_root_deque<Item *> *res_items{nullptr};
  TABLE *res_tmp_table{nullptr};
  TABLE *pseudo_context_table{nullptr};
  bool *was_null{nullptr};
  bool is_exists_subquery{false};

  void set_was_null(Item_in_subselect *sub);
};

enum class Ptrc_status {
  NONE,
  LOOKUP,
  FETCH_NEXT_TUPLE,
  FILLING_CACHE,
  BYPASS_MODE,
  END_OF_SCAN,
};

class PtrcIterator : public RowIterator {
 public:
  PtrcIterator(THD *thd, TableCollection *key_tables,
               TableCollection *res_tables,
               unique_ptr_destroy_only<RowIterator> source,
               mem_root_deque<Item *> *res_items, TABLE *res_tmp_table,
               bool *was_null, bool need_fill_res_tables);

  ~PtrcIterator() override;

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void UnlockRow() override;
  void StartPSIBatchMode() override;
  void EndPSIBatchModeIfStarted() override;

  bool InitRowBuffer();
  bool FillTmpTable();
  void ChangeToUseTmpField();
  void RestoreCacheItems();
  void DisablePtrc() { change_status(Ptrc_status::BYPASS_MODE); }
  bool IsDisabled() const { return status == Ptrc_status::BYPASS_MODE; }

  TableCollection *m_key_tables{nullptr};
  TableCollection *m_res_tables{nullptr};
  Instrumentation stats;

 private:
  bool insert_into_LRU_list();
  void update_LRU_list();
  Key_pair *LRU_free_mem(THD *thd, const Key &key, size_t res_size);
  void change_status(Ptrc_status new_status) { status = new_status; }

  THD *m_thd{nullptr};
  Ptrc_status status{Ptrc_status::NONE};
  LRU_list_type LRU_list;
  LRU_node *current_LRU_node{nullptr};
  TABLE *pseudo_context_table{nullptr};
  unique_ptr_destroy_only<RowIterator> m_source_iterator;
  HashJoinRowBuffer *m_row_buffer{nullptr};
  LinkedImmutableString m_current_row{nullptr};
  mem_root_deque<Item *> *m_res_items{nullptr};
  TABLE *m_res_tmp_table{nullptr};
  mem_root_deque<Item *> *saved_res_items{nullptr};
  mem_root_deque<Item *> *cached_res_fields{nullptr};
  size_t num_hidden_fields{0};
  String key_buff;
  String res_buff;
  PTRC_context *ptrc_context{nullptr};
  bool *m_was_null{nullptr};
  bool m_need_fill_res_tables{true};
};

/**
  Keeps track of PTRC's MEM_ROOT and its related objects.
*/
struct Memory_objects {
  Memory_objects();
  ~Memory_objects();

  MEM_ROOT root;
  MEM_ROOT *saved_root;
  Mem_root_array<PathParameters *> params;
};

AccessPath *CreateAccessPath(THD *thd, Item_subselect *sub,
                             AccessPath *inner_path,
                             qep_tab_map ptrc_key_tables,
                             qep_tab_map ptrc_res_tables, JOIN *ptrc_join);

bool is_ptrc_enabled(const THD *thd);
double get_min_cost_threshold(const THD *thd);
void cleanup(THD *thd);
void print(const AccessPath *path, String *str, THD *thd);

}  // namespace ptrc

#endif /* PARTIAL_RESULT_CACHE_H */
