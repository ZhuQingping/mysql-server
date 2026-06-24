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
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"

AccessPath *CopyRangeScanAccessPath(THD *, AccessPath *, TABLE *) {
  return nullptr;
}

ORDER *pq_dup_order(THD *, Query_block *, ORDER *) { return nullptr; }

bool pq_dup_tabs(JOIN *, JOIN *, bool) { return true; }

bool pq_dup_tabs_skeleton_preflight(JOIN *worker_join, JOIN *leader_join) {
  pq_global_stats.worker_qep_tab_skeleton_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (worker_join == nullptr || leader_join == nullptr ||
      worker_join->qep_tab != nullptr || leader_join->qep_tab == nullptr ||
      worker_join->tables == 0 || worker_join->tables != leader_join->tables) {
    pq_global_stats.worker_qep_tab_skeleton_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  QEP_shared *const qs =
      new (worker_join->thd->mem_root) QEP_shared[worker_join->tables + 1];
  QEP_TAB *const qep_tab =
      new (worker_join->thd->mem_root) QEP_TAB[worker_join->tables + 1];
  if (qs == nullptr || qep_tab == nullptr) {
    pq_global_stats.worker_qep_tab_skeleton_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  worker_join->qep_tab = qep_tab;
  for (uint i = 0; i < worker_join->tables; ++i) {
    qep_tab[i].set_qs(&qs[i]);
    qep_tab[i].set_join(worker_join);
    qep_tab[i].set_idx(i);
  }

  for (uint i = 0; i < worker_join->tables; ++i) {
    QEP_TAB *const tab = &worker_join->qep_tab[i];
    if (tab->join() != worker_join || tab->idx() != static_cast<plan_idx>(i) ||
        tab->table() != nullptr || tab->table_ref != nullptr ||
        tab->condition() != nullptr || tab->range_scan() != nullptr) {
      pq_global_stats.worker_qep_tab_skeleton_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
  }

  pq_global_stats.worker_qep_tab_skeleton_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

namespace {

bool pq_clone_shell_supported(THD *thd, JOIN *join) {
  return thd != nullptr && join != nullptr && join->query_block != nullptr &&
         join->query_block->table_count() == 1;
}

Query_block *pq_dup_query_shell(THD *thd, Query_block *leader_query_block) {
  LEX *const lex = thd->lex;
  if (lex == nullptr || lex->query_block != nullptr || lex->unit != nullptr ||
      leader_query_block->pq_last_clone() != nullptr) {
    return nullptr;
  }

  lex->thd = thd;
  Query_block *const worker_query_block = lex->new_query(nullptr);
  if (worker_query_block == nullptr) return nullptr;

  leader_query_block->pq_link_clone(worker_query_block);
  lex->set_current_query_block(worker_query_block);
  lex->unit = worker_query_block->master_query_expression();
  lex->query_block = worker_query_block;

  worker_query_block->select_number = leader_query_block->select_number;
  worker_query_block->add_active_options(leader_query_block->active_options());
  worker_query_block->parallel_exec = leader_query_block->parallel_exec;
  worker_query_block->uncacheable = leader_query_block->uncacheable;
  worker_query_block->master_query_expression()->uncacheable =
      leader_query_block->master_query_expression()->uncacheable;

  return worker_query_block;
}

}  // namespace

JOIN *pq_make_join(THD *thd, JOIN *join) {
  if (!pq_clone_shell_supported(thd, join)) return nullptr;

  Query_block *const worker_query_block =
      pq_dup_query_shell(thd, join->query_block);
  if (worker_query_block == nullptr) return nullptr;

  JOIN *const worker_join = new (thd->mem_root) JOIN(thd, worker_query_block);
  if (worker_join == nullptr || worker_join->pq_copy_from(join)) {
    worker_query_block->pq_restore();
    if (worker_join != nullptr) worker_join->destroy();
    return nullptr;
  }

  return worker_join;
}

void Query_block::pq_backup() {}

void Query_block::pq_restore() {
  if (pq_is_clone()) pq_unlink_clone();
}

bool JOIN::pq_copy_from(JOIN *orig) {
  pq_global_stats.worker_join_shape_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (orig == nullptr || query_block == nullptr || query_expression() == nullptr ||
      orig->query_expression() == nullptr) {
    pq_global_stats.worker_join_shape_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  if (alloc_indirection_slices()) {
    pq_global_stats.worker_join_shape_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  query_block->join = this;
  ref_items[REF_SLICE_ACTIVE] = query_block->base_ref_items;
  where_cond = query_block->where_cond();
  tables_list = query_block->leaf_tables;

  tables = orig->tables;
  const_tables = orig->const_tables;
  primary_tables = orig->primary_tables;
  explain_flags = orig->explain_flags;
  set_plan_state(orig->plan_state);
  zero_result_cause = orig->zero_result_cause;
  calc_found_rows = orig->calc_found_rows;
  m_select_limit = orig->m_select_limit;
  query_expression()->select_limit_cnt =
      orig->query_expression()->select_limit_cnt;
  query_expression()->offset_limit_cnt =
      query_block->parallel_exec ? 0 : orig->query_expression()->offset_limit_cnt;
  found_const_table_map = orig->found_const_table_map;
  best_rowcount = orig->best_rowcount;

  pq_eligible = orig->pq_eligible;
  pq_unsuitable_reason = orig->pq_unsuitable_reason;
  pq_plan_rewritten = orig->pq_plan_rewritten;
  need_tmp_pq_leader = orig->need_tmp_pq_leader;
  pq_dop = orig->pq_dop;

  pq_global_stats.worker_join_shape_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool JOIN::setup_tmp_table_info(JOIN *) { return true; }

bool JOIN::restore_optimized_vars() { return true; }

void JOIN::pq_restore() {
  if (query_block != nullptr) query_block->pq_restore();
}

bool pq_clone_contract_preflight(THD *thd, JOIN *join) {
  pq_global_stats.clone_preflight_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (!pq_clone_shell_supported(thd, join)) {
    pq_global_stats.clone_preflight_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  /*
    M11-A6 proves only that a single-table statement can create the local
    JOIN shell needed by the commercial clone path. The shell is not an
    executable worker plan: Item subclass clone/refix/restore, QEP_TAB copy,
    table clone, Gather_operator, and handler/InnoDB ownership remain closed.
  */
  pq_global_stats.clone_preflight_success.fetch_add(
      1, std::memory_order_relaxed);
  return true;
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

  JOIN *const clone_shell = pq_make_join(thd, join);
  if (clone_shell == nullptr) {
    pq_global_stats.clone_probe_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    pq_global_stats.clone_probe_fallback.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  clone_shell->pq_restore();
  clone_shell->destroy();

  /*
    The shell construction contract is proven, but activation still remains
    fail-closed until executable clone ownership, restore, and cleanup are
    complete. Do not increment clone_probe_success here.
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
