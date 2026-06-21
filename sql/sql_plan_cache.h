#ifndef SQL_PLAN_CACHE_INCLUDED
#define SQL_PLAN_CACHE_INCLUDED

/* Copyright (c) 2000, 2026, Oracle and/or its affiliates.

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

#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "m_ctype.h"
#include "my_base.h"
#include "my_inttypes.h"
#include "sql/mem_root_allocator.h"
#include "sql/sql_const.h"

class Item;
class JOIN;
struct MEM_ROOT;
class Query_block;
class Query_arena;
class THD;

namespace plan_cache {

extern std::atomic<ulong> cached_plan_count;
extern std::atomic<ulong> cached_plan_invalidations;

struct Exec_context {
  static const ulonglong interested_optimizer_switch_flags =
      OPTIMIZER_SWITCH_INDEX_MERGE | OPTIMIZER_SWITCH_INDEX_MERGE_UNION |
      OPTIMIZER_SWITCH_INDEX_MERGE_SORT_UNION |
      OPTIMIZER_SWITCH_INDEX_MERGE_INTERSECT |
      OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN |
      OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN | OPTIMIZER_SWITCH_MRR |
      OPTIMIZER_SWITCH_MRR_COST_BASED | OPTIMIZER_SWITCH_USE_INVISIBLE_INDEXES |
      OPTIMIZER_SKIP_SCAN | OPTIMIZER_SWITCH_PREFER_ORDERING_INDEX |
      OPTIMIZER_SWITCH_HYPERGRAPH_OPTIMIZER;

  using allocator_for_map = Mem_root_allocator<std::pair<Item *const, Item *>>;
  using allocator_for_vector = Mem_root_allocator<std::pair<Item **, Item *>>;

  explicit Exec_context(MEM_ROOT *root)
      : transient_items(allocator_for_map(root)),
        new_change_list(allocator_for_vector(root)) {}
  Exec_context(const Exec_context &) = delete;
  Exec_context &operator=(const Exec_context &) = delete;
  ~Exec_context();

  /// Owns the item list and MEM_ROOT used by the cached plan.
  Query_arena *arena{nullptr};
  /// True once this context has been included in Cached_plan_count.
  bool counted{false};
  /// True if this cached plan has the minimal QEP snapshot needed for a hit.
  bool supports_cached_hit{false};
  ulonglong optimizer_switch{0};
  const CHARSET_INFO *character_set_client{nullptr};
  ha_rows table_records{0};
  ulonglong table_version{0};

  std::map<Item *, Item *, std::less<Item *>, allocator_for_map>
      transient_items;
  std::vector<std::pair<Item **, Item *>, allocator_for_vector>
      new_change_list;

  bool is_environment_changed(THD *thd) const;
  bool is_table_stats_changed_sharply(ha_rows new_rows,
                                      double allow_change_ratio) const {
    const ha_rows changed_rows = new_rows >= table_records
                                     ? new_rows - table_records
                                     : table_records - new_rows;
    if (allow_change_ratio &&
        (static_cast<double>(changed_rows) /
         (static_cast<double>(table_records) + 1.0)) > allow_change_ratio) {
      return true;
    }
    return false;
  }
};

bool cache_plan(JOIN *join);
bool exec_cached_plan(Query_block *query_block);
bool is_ready(Query_block *query_block);
void set_uncacheable(Query_block *query_block);
void set_ready(Query_block *query_block);
void invalidate_cached_plan(Query_block *query_block);
void destroy_cached_plan(JOIN *join);
void detach_cached_plan(JOIN *cached_plan);
void cleanup_cached_plan_items(JOIN *cached_plan);
void collect_item_params(Item *item, std::set<Item *> &params);
void cmp_item_params_after_reduce_cond(THD *thd,
                                       const std::set<Item *> &old_params,
                                       Item *condition);

}  // namespace plan_cache

Item *clone_if_transient(THD *thd, Item *item);

#endif /* SQL_PLAN_CACHE_INCLUDED */
