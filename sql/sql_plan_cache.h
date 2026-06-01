#ifndef PLAN_CACHE_INCLUDE_H
#define PLAN_CACHE_INCLUDE_H

/* Copyright (c) 2022, Huawei and/or its affiliates. All rights reserved.

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
#include <set>
#include "m_ctype.h"
#include "my_base.h"
#include "my_inttypes.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/mysqld.h"
#include "sql/opt_explain_format.h"
#include "sql/opt_hints.h"
#include "sql/sql_list.h"
#include "sql_const.h"
class JOIN;
class Query_expression;
class Temp_table_param;
class THD;
struct LEX;
class Query_block;
class Item;
class Item_cache;
class ORDER_with_src;
class QEP_TAB;

namespace plan_cache {

extern std::atomic<ulong> cached_plan_count;
extern std::atomic<ulong> cached_plan_invalidations;

struct Exec_context {
  /// If one of these flags is changed by the user, plan may change, so the
  /// cached plan should not be used.
  static const ulonglong interested_optimizer_switch_flags =
      OPTIMIZER_SWITCH_INDEX_MERGE | OPTIMIZER_SWITCH_INDEX_MERGE_UNION |
      OPTIMIZER_SWITCH_INDEX_MERGE_SORT_UNION |
      OPTIMIZER_SWITCH_INDEX_MERGE_INTERSECT |
      OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN |
      OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN | OPTIMIZER_SWITCH_MRR |
      OPTIMIZER_SWITCH_MRR_COST_BASED | OPTIMIZER_SWITCH_USE_INVISIBLE_INDEXES |
      OPTIMIZER_SKIP_SCAN | OPTIMIZER_SWITCH_PREFER_ORDERING_INDEX |
      OPTIMIZER_SWITCH_HYPERGRAPH_OPTIMIZER;

  Key_map quick_keys_map{0};
  /// If the AccessPath is an index range scan, we store a copy of all its
  /// properties (members) here. The original struct type is anonymous, that's
  /// why the declaration is a bit complex.
  std::remove_reference<decltype(AccessPath().index_range_scan())>::type
      index_range_scan_props;
  uint quick_used_key_parts{0};
  Temp_table_param *tmp_table_param{nullptr};
  ulonglong optimizer_switch{0};
  const CHARSET_INFO *character_set_client;
  bool fill(THD *thd, JOIN *join, QEP_TAB *cached_qep_tab);
  uint pushed_idx_cond_keyno{MAX_KEY};
  Item *pushed_idx_cond{nullptr};
  bool key_read{false};
  double current_query_cost{0};
  ORDER_with_src *distinct_group_list{nullptr};
  ORDER_with_src *order_list{nullptr};
  Item *where_cond{nullptr};
  Item *having_cond{nullptr};
  ha_rows table_records{0};  //#records in table
  ulonglong table_version;
  Key_map covering_keys{0};
  /// It's used to help recover m_condition for QEP_TAB. In Taurus, if ndp
  /// condition is pushed down, it will cleanup m_condition of QEP_TAB.
  Item *tab_condition{nullptr};
  int quick_type{-1};  // -1 to mean "no range access"

  Explain_format_flags explain_flags;
  bool calc_found_rows;
  ha_rows m_select_limit;
  bool grouped;
  bool streaming_aggregation;
  bool select_distinct;
  double best_read;
  /// Owns memory for this cached plan
  Query_arena *arena{nullptr};

  using allocator_for_map = Mem_root_allocator<std::pair<Item *const, Item *>>;
  std::map<Item *, Item *, std::less<Item *>, allocator_for_map>
      transient_items;
  using allocator_for_vector = Mem_root_allocator<std::pair<Item **, Item *>>;
  std::vector<std::pair<Item **, Item *>, allocator_for_vector> new_change_list;

  bool is_environment_changed(THD *thd);
  bool is_table_stats_changed_sharply(ha_rows new_rows,
                                      double allow_change_ratio) {
    // If allow_change_ratio is not 0, when the number of rows in the table
    // is changed more than this fraction, cached plan will be invalidated.
    if (allow_change_ratio &&
        (std::abs(longlong(new_rows - table_records)) /
         (float)(table_records + 1)) > allow_change_ratio) {
      return true;
    }
    return false;
  }

  // PLAN_CACHE_PORT: in the original impl, this struct has no user-defined
  // ctor. But in the new impl: I add a std::map and a vector; as long as they
  // were on the heap it was simple, I could just use move-assignment to fill
  // these members after default empty construction. But when I changed to make
  // them allocate in a mem_root (to not use the heap), by giving them a
  // Mem_root_allocator, I needed to pass the MEM_ROOT, so default ctor was
  // not good anymore.
  Exec_context(MEM_ROOT *root)
      : transient_items(allocator_for_map(root)),
        new_change_list(allocator_for_vector(root)) {}
  ~Exec_context();
};
bool cache_plan(JOIN *orig_join);
bool exec_cached_plan(Query_block *sl);
bool is_ready(Query_block *sl);
void set_uncacheable(Query_block *sl);
void collect_item_params(Item *item, std::set<Item *> &params);
void set_ready(Query_block *sl);
bool is_clone_for_plan_cache(const Query_block *sl);
void invalidate_cached_plan(Query_block *select);
bool emulates_hint(const THD *thd, opt_hints_enum hint);
void restore_param_snapshot_clones(LEX *lex);
void cmp_item_params_after_reduce_cond(THD *thd,
                                       const std::set<Item *> &old_params,
                                       Item *condition);
void destroy_cached_plan(JOIN *join);
}  // namespace plan_cache
// PLAN_CACHE_PORT this is what replaces Item's pq_clone() functions.
Item *clone_if_transient(THD *thd, Item *i);
#endif  // PLAN_CACHE_INCLUDE_H
