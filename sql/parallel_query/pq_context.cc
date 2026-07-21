/* Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "sql/item_sum.h"
#include "sql/parallel_query/pq_clone.h"
#include "sql/parallel_query/pq_context.h"
#include "sql/parallel_query/pq_optimizer.h"
#include "sql/parallel_query/pq_resource_stat.h"
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_optimizer.h"

void PQ_thd_context::initialize_mem_root() {
  mem_root = new MEM_ROOT();
  init_sql_alloc(key_memory_pq_mem_root, mem_root,
                 global_system_variables.query_alloc_block_size);
  mem_root->allocCBFunc = add_pq_memory;
  mem_root->freeCBFunc = sub_pq_memory;
}

void PQ_thd_context::destroy_mem_root() {
  if (mem_root != nullptr) {
    mem_root->Clear();
    delete mem_root;
    mem_root = nullptr;
  }
}

void PQ_thd_context::cleanup_statement() {
  if (threads_running > 0) {
    release_pq_running_threads(threads_running);
    threads_running = 0;
  }
  if (mem_root != nullptr) mem_root->Clear();
  dop = 0;
  no_pq = false;
  clear_error_after_workers_join();
  has_pq = false;
  current_found_rows = 0;
  gathers.clear();
#ifndef NDEBUG
  skip_fetch_ctx = false;
#endif  // NDEBUG
}

bool PQ_thd_context::suite_for_parallel_query(
    const THD &thd, PQUnsuiteInfo *pq_info) const {
  if (dop == 0) {
    *pq_info = PQUnsuiteInfo::ZERO_DOP;
    return false;
  }
  if (no_pq) {
    *pq_info = PQUnsuiteInfo::NO_PQ;
    return false;
  }
  if (!(thd.lex->sql_command == SQLCOM_SELECT ||
        thd.lex->sql_command == SQLCOM_INSERT_SELECT ||
        thd.lex->sql_command == SQLCOM_REPLACE_SELECT)) {
    *pq_info = PQUnsuiteInfo::UNSUPPORT_COMMAND;
    return false;
  }
  if (thd.lex->in_execute_ps || thd.in_sp_trigger ||
      thd.is_attachable_transaction_active() ||
      thd.tx_isolation == ISO_SERIALIZABLE) {
    *pq_info = PQUnsuiteInfo::PREPARE_TRIGER_PROCEDURE;
    return false;
  }
  if (get_instance_storage_engine_type() != DB_TYPE_INNODB) {
    *pq_info = PQUnsuiteInfo::ONLY_SUPPORT_INNODB;
    return false;
  }
  if (thd.lex->using_hypergraph_optimizer()) {
    *pq_info = PQUnsuiteInfo::HYPERGRAPH_OPTIMIZER;
    return false;
  }
  return true;
}

bool PQ_thd_context::merge_status(THD &owner, THD &other) {
  owner.status_var.pq_merge_status(other.status_var);
  if (is_worker())
    current_found_rows += other.current_found_rows;
  else
    current_found_rows = other.pq_context().current_found_rows;
  return false;
}

void PQ_thd_context::copy_from(const THD &source) {
  has_pq = source.pq_context().has_pq;
  dop = source.pq_context().dop;
}

Query_block *PQ_query_block_context::last_clone(
    const Query_block &owner) const {
  assert(m_pq_last_clone == nullptr ||
         m_pq_last_clone->pq_context().m_pq_is_clone_of == &owner);
  return m_pq_last_clone;
}

Query_block *PQ_query_block_context::clone_of(const Query_block &owner) const {
  assert(m_pq_is_clone_of == nullptr ||
         m_pq_is_clone_of->pq_context().m_pq_last_clone == &owner);
  return m_pq_is_clone_of;
}

void PQ_query_block_context::link_clone(Query_block &owner,
                                         Query_block &clone) {
  assert(m_pq_last_clone == nullptr &&
         clone.pq_context().m_pq_is_clone_of == nullptr);
  m_pq_last_clone = &clone;
  clone.pq_context().m_pq_is_clone_of = &owner;
}

void PQ_query_block_context::unlink_clone(Query_block &clone) {
  assert(m_pq_is_clone_of != nullptr &&
         m_pq_is_clone_of->pq_context().m_pq_last_clone == &clone);
  m_pq_is_clone_of->pq_context().m_pq_last_clone = nullptr;
  m_pq_is_clone_of = nullptr;
}

bool PQ_query_block_context::is_clone(const Query_block &owner) const {
  return clone_of(owner) != nullptr;
}

void PQ_query_block_context::backup(Query_block &owner) {
  auto query_blocks = list_query_blocks_to_clone(&owner);
  for (auto *orig : query_blocks) {
    for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
         tbl_list = tbl_list->next_leaf) {
      tbl_list->table->pq_saved_const_table = tbl_list->table->const_table;
    }
  }
}

void PQ_query_block_context::restore(Query_block &owner) {
  auto restore_saved_list_ptrs = [](Mem_root_array<ORDER *> *saved_list_ptrs) {
    if (saved_list_ptrs == nullptr) return;
    for (auto *order : *saved_list_ptrs) {
      (*order->item)->walk(&Item::pq_restore, enum_walk::PREFIX, nullptr);
    }
  };

  auto query_blocks = list_query_blocks_to_clone(&owner);
  for (auto *orig : query_blocks) {
    orig->pq_context().m_pq_last_clone = nullptr;
    for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
         tbl_list = tbl_list->next_leaf) {
      tbl_list->table->const_table = tbl_list->table->pq_saved_const_table;
    }
    orig->walk(&Item::pq_restore, enum_walk::PREFIX, nullptr);
    if (orig->pq_context().saved_where_cond != nullptr) {
      orig->pq_context().saved_where_cond->walk(&Item::pq_restore,
                                                 enum_walk::PREFIX, nullptr);
    }
    if (orig->pq_context().saved_having_cond != nullptr) {
      orig->pq_context().saved_having_cond->walk(&Item::pq_restore,
                                                  enum_walk::PREFIX, nullptr);
    }
    restore_saved_list_ptrs(orig->pq_context().saved_group_list_ptrs);
    restore_saved_list_ptrs(orig->pq_context().saved_order_list_ptrs);
  }
}

void PQ_query_block_context::check_after_prepare(Query_block &owner,
                                                  THD &thd) {
  if (!m_suite_for_pq) return;

  MEM_ROOT *mem_root = thd.pq_context().mem_root;
  thd.pq_context().mem_root = thd.mem_root;
  const ulong saved_privilege = thd.want_privilege;

  if (owner.group_list.elements) {
    if (owner.save_order_properties(&thd, &owner.group_list,
                                    &saved_group_list_ptrs)) {
      m_suite_for_pq = false;
      goto end;
    }
    check_map_group_to_base = new (thd.pq_context().mem_root)
        mem_root_unordered_map<uint, uint>(thd.pq_context().mem_root);
    if (check_map_group_to_base == nullptr ||
        owner.record_map_order(owner.group_list, *check_map_group_to_base)) {
      m_suite_for_pq = false;
      goto end;
    }
  }

  if (owner.order_list.elements) {
    if (owner.save_order_properties(&thd, &owner.order_list,
                                    &saved_order_list_ptrs)) {
      m_suite_for_pq = false;
      goto end;
    }
    check_map_order_to_base = new (thd.pq_context().mem_root)
        mem_root_unordered_map<uint, uint>(thd.pq_context().mem_root);
    if (check_map_order_to_base == nullptr ||
        owner.record_map_order(owner.order_list, *check_map_order_to_base)) {
      m_suite_for_pq = false;
      goto end;
    }
  }

  thd.want_privilege = 0;
  pq_try_clone_item = true;
  if (owner.where_cond() != nullptr &&
      !(saved_where_cond = owner.where_cond()->pq_clone(&thd, &owner))) {
    m_suite_for_pq = false;
    goto end;
  }
  if (owner.having_cond() != nullptr &&
      !(saved_having_cond = owner.having_cond()->pq_clone(&thd, &owner))) {
    m_suite_for_pq = false;
    goto end;
  }

end:
  if (!m_suite_for_pq) pq_unsuite_info = PQUnsuiteInfo::INTER_ERROR;
  pq_try_clone_item = false;
  thd.want_privilege = saved_privilege;
  thd.pq_context().mem_root = mem_root;
}

bool PQ_query_block_context::suite_for_parallel_query(Query_block &owner,
                                                       THD &thd) {
  if (!thd.suite_for_parallel_query(&pq_unsuite_info)) return false;

  if (owner.active_options() & OPTION_BUFFER_RESULT) {
    pq_unsuite_info = PQUnsuiteInfo::ROLLUP_WINDS_BUFFER;
    return false;
  }
  if (owner.olap == ROLLUP_TYPE ||
      (owner.row_value_list != nullptr && owner.row_value_list->size() > 1) ||
      saved_windows_elements != 0) {
    pq_unsuite_info = PQUnsuiteInfo::ROLLUP_WINDS_BUFFER;
    return false;
  }
  if (owner.master_query_expression()->subquery_suite_for_parallel_query() ==
      PQSubqueryExecution::kImpossible) {
    pq_unsuite_info = PQUnsuiteInfo::UNSUPPORTED_SUBQUERY_TYPE;
    return false;
  }
  if (owner.wrapped_in_intersect_except()) {
    pq_unsuite_info = PQUnsuiteInfo::UNSUPPORTED_INTERSECT_AND_EXCEPT;
    thd.pq_context().no_pq = true;
    return false;
  }
  return true;
}

static bool check_tmp_table_field_name(TABLE *table) {
  const uint field_count = table->visible_field_count();
  Field **fields = table->visible_field_ptr();
  for (uint i = 1; i < field_count; ++i) {
    const char *field_name_i = fields[i]->field_name;
    for (uint j = 0; j < i; ++j) {
      if (!strcmp(field_name_i, fields[j]->field_name)) return true;
    }
  }
  return false;
}

bool PQ_query_block_context::check_table_list(Query_block &owner) {
  /*
    As long as one table doesn't pass the check, the query should not do PQ.
    For the choice of the divided/cut table, there are further checks in
    TABLE::suite_for_pq_division().
  */
  if (!owner.join->qep_tab) return false;
  for (uint i = 0; i < owner.join->tables; ++i) {
    Table_ref *tbl_list = owner.join->qep_tab[i].table_ref;
    if (tbl_list == nullptr || tbl_list->table == nullptr) continue;

    if (tbl_list->db != nullptr && tbl_list->db_length != 0 &&
        (is_sys_db(tbl_list->db) || is_infoschema_db(tbl_list->db) ||
         is_mysql_db(tbl_list->db) || is_perfschema_db(tbl_list->db))) {
      pq_unsuite_info = PQUnsuiteInfo::SYS_OR_TMP_TABLE;
      return true;
    }
    if (tbl_list->is_view_or_derived()) {
      if (tbl_list->derived_query_expression()->uncacheable &
          UNCACHEABLE_DEPENDENT) {
        pq_unsuite_info = PQUnsuiteInfo::VIEW_OR_DERIVED_TABLE;
        return true;
      }
      continue;
    }
    if (tbl_list->is_table_function()) {
      pq_unsuite_info = PQUnsuiteInfo::TABLE_FUNCTION_OR_LOCK;
      return true;
    }
    if (tbl_list->lock_descriptor().type > TL_READ_DEFAULT ||
        owner.parent_lex->locking_clause) {
      pq_unsuite_info = PQUnsuiteInfo::TABLE_FUNCTION_OR_LOCK;
      return true;
    }
    auto *tbl_share = tbl_list->table->s;
    if (tbl_share->tmp_table == TRANSACTIONAL_TMP_TABLE ||
        tbl_share->tmp_table == SYSTEM_TMP_TABLE) {
      pq_unsuite_info = PQUnsuiteInfo::SYS_OR_TMP_TABLE;
      return true;
    }
    if (tbl_list->query_block != &owner) {
      /*
        This is defensive programming. tbl_list->query_block should always be
        equal to the owner, but with subquery-to-derived it sometimes is not
        (this is a bug in Community MySQL). This in turn causes problems in
        pq_can_resolve_Item_field_in(). So we block this case.
      */
      pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
      return true;
    }
    if (tbl_share->tmp_table == INTERNAL_TMP_TABLE &&
        check_tmp_table_field_name(tbl_list->table)) {
      pq_unsuite_info = PQUnsuiteInfo::TMP_TABLE_FIELD_NAME_SAME;
      return true;
    }
  }
  return false;
}

static void pq_restore_optimized_group_order(
    Mem_root_array<ORDER *> *ptr, Explain_sort_clause src_arg,
    const std::vector<bool> &optimized_flags,
    ORDER_with_src &group_order_list) {
  ORDER *optimized_order = nullptr;
  if (ptr != nullptr && !optimized_flags.empty()) {
    SQL_I_List<ORDER> orig_list{};
    int idx = 0;
    for (auto order : *ptr) {
      if (!optimized_flags[idx]) orig_list.link_in_list(order, &order->next);
      ++idx;
    }
    optimized_order = orig_list.first;
  }
  group_order_list.clean();
  if (optimized_order != nullptr) {
    group_order_list = ORDER_with_src(optimized_order, src_arg);
  }
}

bool PQ_join_context::restore_optimized_vars(JOIN &join) {
  for (Item *item : *join.fields) item->update_used_tables();

  join.grouped = saved_optimized_vars.pq_grouped;
  join.group_optimized_away = saved_optimized_vars.pq_group_optimized_away;
  join.implicit_grouping = saved_optimized_vars.pq_implicit_grouping;
  join.need_tmp_before_win = saved_optimized_vars.pq_need_tmp_before_win;
  join.simple_group = saved_optimized_vars.pq_simple_group;
  join.simple_order = saved_optimized_vars.pq_simple_order;
  join.streaming_aggregation = saved_optimized_vars.pq_streaming_aggregation;
  join.m_ordered_index_usage = static_cast<JOIN::ORDERED_INDEX_USAGE>(
      saved_optimized_vars.pq_m_ordered_index_usage);
  join.skip_sort_order = saved_optimized_vars.pq_skip_sort_order;
  join.select_distinct = saved_optimized_vars.pq_select_distinct;

  pq_restore_optimized_group_order(saved_join_group_list, ESC_GROUP_BY,
                                   saved_optimized_vars.optimized_group_flags,
                                   join.group_list);
  pq_restore_optimized_group_order(saved_join_order, ESC_ORDER_BY,
                                   saved_optimized_vars.optimized_order_flags,
                                   join.order);
  if (join.group_list.order != nullptr) {
    const uint old_group_parts = join.send_group_parts;
    calc_group_buffer(&join, join.group_list.order);
    join.send_group_parts = join.tmp_table_param.group_parts;
    if (join.send_group_parts != old_group_parts) return true;
  }

  join.having_cond = saved_optimized_vars.pq_saved_having_cond;
  if (join.query_block->pq_context().parallel_exec) {
    if (pq_pushdown_having) {
      if (join.thd->is_pq_leader()) {
        join.having_cond = nullptr;
      } else {
        join.having_cond = join.query_block->having_cond();
        if (join.having_cond != nullptr) join.having_cond->update_used_tables();
      }
    } else if (join.thd->is_pq_worker()) {
      join.having_cond = nullptr;
    }
  }
  return false;
}

void PQ_join_context::save_optimized_vars(JOIN &join) {
  saved_optimized_vars.pq_grouped = join.grouped;
  saved_optimized_vars.pq_group_optimized_away = join.group_optimized_away;
  saved_optimized_vars.pq_implicit_grouping = join.implicit_grouping;
  saved_optimized_vars.pq_need_tmp_before_win = join.need_tmp_before_win;
  saved_optimized_vars.pq_simple_group = join.simple_group;
  saved_optimized_vars.pq_simple_order = join.simple_order;
  saved_optimized_vars.pq_streaming_aggregation = join.streaming_aggregation;
  saved_optimized_vars.pq_skip_sort_order = join.skip_sort_order;
  saved_optimized_vars.pq_m_ordered_index_usage = join.m_ordered_index_usage;
  saved_optimized_vars.pq_select_distinct = join.select_distinct;

  saved_join_order = join.query_block->pq_context().saved_order_list_ptrs;
  if (saved_join_group_list == nullptr) {
    saved_join_group_list = join.query_block->pq_context().saved_group_list_ptrs;
  }
  record_optimized_group_order(saved_join_group_list, join.group_list,
                               saved_optimized_vars.optimized_group_flags);
  record_optimized_group_order(saved_join_order, join.order,
                               saved_optimized_vars.optimized_order_flags);
  saved_optimized_vars.pq_saved_having_cond = join.having_cond;
}

void PQ_join_context::set_push_down_having(JOIN &join) {
  assert(!pq_pushdown_having);
  if (join.having_cond == nullptr || join.having_cond->has_aggregation() ||
      join.having_cond->has_grouping_func()) {
    pq_pushdown_having = false;
    return;
  }

  bool has_agg = false;
  for (Item *item : *join.fields) {
    if (item->type() == Item::SUM_FUNC_ITEM && !item->const_item() &&
        down_cast<Item_sum *>(item)->aggr_query_block == join.query_block) {
      has_agg = true;
      break;
    }
  }
  pq_pushdown_having = !(has_agg && !join.grouped && !join.group_optimized_away);
}

void PQ_join_context::restore_plan(JOIN &join) {
  for (uint i = join.const_tables; i < join.primary_tables; ++i) {
    join.qep_tab[i].table()->file->do_parallel_scan = false;
  }

  auto query_blocks_to_clone = list_query_blocks_to_clone(join.query_block);
  for (auto *query_block : query_blocks_to_clone) {
    JOIN *cloned_join = query_block->join;
    if (cloned_join == nullptr) continue;
    for (auto &clause : {cloned_join->group_list, cloned_join->order}) {
      for (auto order = clause.order; order != nullptr; order = order->next) {
        (*order->item)->walk(&Item::pq_restore, enum_walk::PREFIX, nullptr);
      }
    }
    for (uint i = 0; i < cloned_join->tables; ++i) {
      QEP_TAB *tab = &cloned_join->qep_tab[i];
      if (tab->sj_mat_exec() != nullptr) {
        tab->sj_mat_exec()->m_pq_shared_info = nullptr;
      }
    }
  }

  key_range ref_key{};
  for (const int idx : {idx_cut_tab, idx_div_tab}) {
    if (idx < 0) continue;
    TABLE *table = join.qep_tab[idx].table();
    if (table == nullptr) continue;
    table->file->ha_set_reverse_scan(false);
    table->file->pq_ref = false;
    table->file->pq_ref_key = ref_key;
  }
  idx_div_tab = -1;
  idx_cut_tab = -1;
}

bool PQ_join_context::allocate_indirection_slices(JOIN &join) {
  const int num_slices = REF_SLICE_WIN_1 + join.m_windows.elements;
  assert(ref_items1 == nullptr);

  ref_items1 = (Ref_item_array *)(*THR_MALLOC)
                   ->Alloc(sizeof(Ref_item_array) * num_slices);
  if (ref_items1 == nullptr) return true;

  tmp_fields1 = (*THR_MALLOC)
                    ->ArrayAlloc<mem_root_deque<Item *>>(num_slices,
                                                          *THR_MALLOC);
  if (tmp_fields1 == nullptr) return true;

  for (int i = 0; i < num_slices; ++i) {
    ref_items1[i].reset();
    tmp_fields1[i].empty();
    tmp_fields1[i].empty();
  }
  join.ref_items = ref_items1;
  join.tmp_fields = tmp_fields1;
  return false;
}

bool PQ_join_context::allocate_qep(JOIN &join, uint n) {
  static_assert(MAX_TABLES <= INT_MAX8, "plan_idx needs to be wide enough.");
  assert(join.tables == n);

  qep_tab1 = new (join.thd->pq_context().mem_root) QEP_TAB[n + 1];
  if (qep_tab1 == nullptr) return true;

  QEP_shared *qs = new (join.thd->pq_context().mem_root) QEP_shared[n + 1];
  if (qs == nullptr) return true;

  for (uint i = 0; i < n; ++i) {
    qep_tab1[i].pos = i;
    qep_tab1[i].set_qs(&qs[i]);
    qep_tab1[i].set_join(&join);
    qep_tab1[i].set_idx(i);
  }
  join.qep_tab = qep_tab1;
  return false;
}

bool PQ_join_context::setup_tmp_table_info(JOIN &join, JOIN &orig) {
  join.ref_items[REF_SLICE_ACTIVE] = join.query_block->base_ref_items;
  join.tmp_table_param.pq_copy(orig.pq_context().saved_tmp_table_param);
  saved_tmp_table_param = new (join.thd->mem_root) Temp_table_param();
  if (saved_tmp_table_param == nullptr) return true;
  saved_tmp_table_param->pq_copy(orig.pq_context().saved_tmp_table_param);

  join.select_distinct = orig.select_distinct;
  return restore_optimized_vars(join) || join.alloc_func_list();
}
