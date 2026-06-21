/* Copyright (c) 2026, Oracle and/or its affiliates.

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

#include "sql/parallel_query/pq_clone.h"

#include <vector>

#include "sql/parallel_query/sql_parallel.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"

AccessPath *CopyRangeScanAccessPath(THD *, AccessPath *, TABLE *) {
  return nullptr;
}

ORDER *pq_dup_order(THD *, Query_block *, ORDER *) { return nullptr; }

bool pq_dup_tabs(JOIN *, JOIN *, bool) { return true; }

JOIN *pq_make_join(THD *, JOIN *) { return nullptr; }

void Query_block::pq_backup() {}

void Query_block::pq_restore() {}

bool JOIN::pq_copy_from(JOIN *) { return true; }

bool JOIN::setup_tmp_table_info(JOIN *) { return true; }

bool JOIN::restore_optimized_vars() { return true; }

void JOIN::pq_restore() {}

bool pq_clone_contract_preflight(THD *thd, JOIN *join) {
  pq_global_stats.clone_preflight_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (thd == nullptr || join == nullptr || join->query_block == nullptr ||
      join->query_block->table_count() != 1) {
    pq_global_stats.clone_preflight_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  /*
    M11-A has only the base Item/JOIN/Query_block shells and conservative
    resolver lookup helpers. The commercial clone path still lacks executable
    Item subclass clone/refix/restore coverage and full QEP_TAB/JOIN state
    cleanup, so the preflight remains fail-closed.
  */
  pq_global_stats.clone_preflight_unsupported.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool pq_clone_activation_probe(THD *thd, JOIN *join) {
  pq_global_stats.clone_probe_attempts.fetch_add(1,
                                                 std::memory_order_relaxed);

  if (!pq_clone_contract_preflight(thd, join)) {
    pq_global_stats.clone_probe_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.clone_probe_fallback.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  /*
    A4 does not produce an executable cloned JOIN even if the preflight helper
    is extended later. Keep activation fail-closed until pq_make_join()
    ownership, restore, and cleanup contracts are proven.
  */
  pq_global_stats.clone_probe_unsupported.fetch_add(
      1, std::memory_order_relaxed);
  pq_global_stats.clone_probe_fallback.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

void swap_column_names_of_unit_and_tmp_table(
    const mem_root_deque<Item *> &, const Create_col_name_list &) {}

void reset_avg_property(mem_root_deque<Item *> &) {}

bool pq_replace_base_item(Query_block *) { return true; }

bool check_resolved_order_item(const SQL_I_List<ORDER> &, const Ref_item_array &,
                               const mem_root_unordered_map<uint, uint> &) {
  return true;
}

std::vector<Query_block *> list_query_blocks_to_clone(Query_block *top_sl) {
  std::vector<Query_block *> query_blocks;
  if (top_sl != nullptr) query_blocks.push_back(top_sl);
  return query_blocks;
}
