/* Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

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

#include "sql/parallel_query/pq_optimizer.h"

#include "my_base.h"
#include "sql/filesort.h"
#include "sql/item_strfunc.h"
#include "sql/item_sum.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/opt_trace.h"
#include "sql/parallel_query/pq_clone.h"
#include "sql/parallel_query/sql_parallel.h"
#include "sql/range_optimizer/path_helpers.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_optimizer_internal.h"
#include "sql/sql_resolver.h"
#include "sql/sql_tmp_table.h"

bool JOIN::alloc_indirection_slices1() {
  const int num_slices = REF_SLICE_WIN_1 + m_windows.elements;

  assert(ref_items1 == nullptr);

  ref_items1 = (Ref_item_array *)(*THR_MALLOC)
                   ->Alloc(sizeof(Ref_item_array) * num_slices);
  if (ref_items1 == nullptr) return true;

  tmp_fields1 =
      (*THR_MALLOC)
          ->ArrayAlloc<mem_root_deque<Item *>>(num_slices, *THR_MALLOC);
  if (tmp_fields1 == nullptr) return true;

  for (int i = 0; i < num_slices; i++) {
    ref_items1[i].reset();
    tmp_fields1[i].empty();
    tmp_fields1[i].empty();
  }
  ref_items = ref_items1;
  tmp_fields = tmp_fields1;

  return false;
}

bool JOIN::alloc_qep1(uint n) {
  static_assert(MAX_TABLES <= INT_MAX8, "plan_idx needs to be wide enough.");
  assert(tables == n);

  qep_tab1 = new (thd->pq_mem_root) QEP_TAB[n + 1];
  if (!qep_tab1) return true; /* purecov: inspected */

  QEP_shared *qs = new (thd->pq_mem_root) QEP_shared[n + 1];
  if (!qs) return true;

  for (uint i = 0; i < n; i++) {
    qep_tab1[i].pos = i;
    qep_tab1[i].set_qs(&qs[i]);
    qep_tab1[i].set_join(this);
    qep_tab1[i].set_idx(i);
  }
  qep_tab = qep_tab1;

  return false;
}

/**
  restore the optimized group/order list, using saved group/order list
  and optimized_flags.

  @param[in]  ptr              saved group/order list
  @param[in]  src_arg          origin of order list
  @param[in]  optimized_flags  saved optimized_flags
  @param[out] group_order_list restored group/order list
*/
static void pq_restore_optimized_group_order(
    Group_list_ptrs *ptr, Explain_sort_clause src_arg,
    const std::vector<bool> &optimized_flags,
    ORDER_with_src &group_order_list) {
  ORDER *optimized_order = nullptr;
  if (ptr != nullptr && optimized_flags.size() != 0) {
    SQL_I_List<ORDER> orig_list{};
    int idx = 0;
    for (auto order : *ptr) {
      if (!optimized_flags[idx]) orig_list.link_in_list(order, &order->next);
      idx++;
    }
    optimized_order = orig_list.first;
  }
  group_order_list.clean();
  if (optimized_order) {
    group_order_list = ORDER_with_src(optimized_order, src_arg);
  }
}

bool JOIN::restore_optimized_vars() {
  // After the temporary table is created in the make_tmp_tables_info function,
  // the ref_items corresponding to join->fields are switched to the field
  // objects of the temporary table. This may trigger a recalculation of
  // m_accum_properties, potentially causing the loss of the PROP_AGGREGATION
  // attribute. Prior to creating the temporary table in PQ, ref_items for
  // join->fields are reset to base_ref_items. Therefore, it is necessary to
  // call Item::update_used_tables to update the m_accum_properties field
  // accordingly.
  for (Item *item : *fields) item->update_used_tables();
  // restore the make_tmp_tables_info's parameter through
  // saved_optimized_variables
  grouped = saved_optimized_vars.pq_grouped;
  group_optimized_away = saved_optimized_vars.pq_group_optimized_away;
  implicit_grouping = saved_optimized_vars.pq_implicit_grouping;
  need_tmp_before_win = saved_optimized_vars.pq_need_tmp_before_win;
  simple_group = saved_optimized_vars.pq_simple_group;
  simple_order = saved_optimized_vars.pq_simple_order;
  streaming_aggregation = saved_optimized_vars.pq_streaming_aggregation;
  m_ordered_index_usage = static_cast<ORDERED_INDEX_USAGE>(
      saved_optimized_vars.pq_m_ordered_index_usage);
  skip_sort_order = saved_optimized_vars.pq_skip_sort_order;
  select_distinct = saved_optimized_vars.pq_select_distinct;

  pq_restore_optimized_group_order(saved_join_group_list, ESC_GROUP_BY,
                                   saved_optimized_vars.optimized_group_flags,
                                   group_list);
  pq_restore_optimized_group_order(saved_join_order, ESC_ORDER_BY,
                                   saved_optimized_vars.optimized_order_flags,
                                   order);
  if (group_list.order) {
    uint old_group_parts = send_group_parts;
    calc_group_buffer(this, group_list.order);
    send_group_parts = tmp_table_param.group_parts; /* Save org parts */
    if (send_group_parts != old_group_parts)  // error: leader and worker have
                                              // different group fields
      return true;
  }

  having_cond = saved_optimized_vars.pq_saved_having_cond;
  // Note that, mysteriously, the worker uses Query_block->having_cond() while
  // the leader uses pq_saved_having_cond.
  if (query_block->parallel_exec) {
    if (push_down_having()) {
      // If HAVING has been pushed to worker, leader should reset having_cond to
      // null; otherwise, worker's having_cond should reset to null.
      if (thd->is_pq_leader()) {
        having_cond = nullptr;
      } else {
        having_cond = query_block->having_cond();
        if (having_cond) having_cond->update_used_tables();
      }
    } else {
      if (thd->is_pq_worker()) having_cond = nullptr;
    }
  }

  return false;
}

void JOIN::save_optimized_vars() {
  // saved optimized variables
  saved_optimized_vars.pq_grouped = grouped;
  saved_optimized_vars.pq_group_optimized_away = group_optimized_away;
  saved_optimized_vars.pq_implicit_grouping = implicit_grouping;
  saved_optimized_vars.pq_need_tmp_before_win = need_tmp_before_win;
  saved_optimized_vars.pq_simple_group = simple_group;
  saved_optimized_vars.pq_simple_order = simple_order;
  saved_optimized_vars.pq_streaming_aggregation = streaming_aggregation;
  saved_optimized_vars.pq_skip_sort_order = skip_sort_order;
  saved_optimized_vars.pq_m_ordered_index_usage = m_ordered_index_usage;
  saved_optimized_vars.pq_select_distinct = select_distinct;

  // record the mapping: JOIN::group_list -> Query_block->group_list
  saved_join_order = query_block->saved_order_list_ptrs;
  if (!saved_join_group_list) {
    saved_join_group_list = query_block->saved_group_list_ptrs;
  }

  record_optimized_group_order(saved_join_group_list, group_list,
                               saved_optimized_vars.optimized_group_flags);
  record_optimized_group_order(saved_join_order, order,
                               saved_optimized_vars.optimized_order_flags);
  saved_optimized_vars.pq_saved_having_cond = having_cond;
}

void JOIN::set_push_down_having() {
  assert(!pq_pushdown_having);
  // If HAVING contains aggregation, it cannot be pushed to worker
  if (!having_cond || having_cond->has_aggregation() ||
      having_cond->has_grouping_func()) {
    pq_pushdown_having = false;
    return;
  }
  // If select_list contains aggregation and query does not has group by
  // clause, it cannot be pushed to worker. Because in AggregateIterator::Read
  // function, if there's no GROUP BY, it will output a row even if there are
  // no input rows. If it has a having_cond, having_cond will filter results
  // from AggregateIterator::Read. For pq, if having_cond has pushed down to
  // worker in this scenario, leader has not having_cond and the query result
  // would not meet the MySQL expectation because the result from
  // AggregateIterator::Read is not filtered by having_cond. For details, see
  // mysql-test/suite/parallel_query/t/BUG2023110202800.test. So in this
  // scenario, having_cond in leader should not be set to nullptr and saved.
  bool has_agg = false;
  for (Item *item : *fields) {
    if (item->type() == Item::SUM_FUNC_ITEM && !item->const_item() &&
        down_cast<Item_sum *>(item)->aggr_query_block == query_block) {
      has_agg = true;
      break;
    }
  }
  if (has_agg && !grouped && !group_optimized_away) {
    pq_pushdown_having = false;
    return;
  }
  pq_pushdown_having = true;
}

void JOIN::pq_restore() {
  // Restore TABLE::do_parallel_scan modified in make_pq_leader_plan
  // to its default value.
  for (uint i = const_tables; i < primary_tables; i++) {
    qep_tab[i].table()->file->do_parallel_scan = false;
  }

  auto query_blocks_to_clone = list_query_blocks_to_clone(query_block);
  for (auto query_block : query_blocks_to_clone) {
    JOIN *join = query_block->join;
    if (!join) continue;
    // Although the current JOIN::group_list/order does not require
    // pq_restore processing and this does not affect functionality,
    // considering that future new features may necessitate pq_restore,
    // we have added pq_restore processing for JOIN::group_list/order.
    for (auto &clause : {join->group_list, join->order}) {
      for (auto order = clause.order; order; order = order->next) {
        (*order->item)->walk(&Item::pq_restore, enum_walk::PREFIX, nullptr);
      }
    }
    // Restore Semijoin_mat_exec::m_pq_shared_info modified in
    // Semijoin_mat_exec::pq_clone to its default value.
    for (uint i = 0; i < join->tables; i++) {
      QEP_TAB *tab = &join->qep_tab[i];
      if (tab->sj_mat_exec()) {
        tab->sj_mat_exec()->m_pq_shared_info = nullptr;
      }
    }
  }

  // Restore table->file modified in SetupPQTab() to default value
  key_range ref_key{};
  for (auto idx : {idx_cut_tab, idx_div_tab}) {
    if (idx < 0) continue;
    TABLE *table = qep_tab[idx].table();
    if (!table) continue;
    table->file->ha_set_reverse_scan(false);
    table->file->pq_ref = false;
    table->file->pq_ref_key = ref_key;
  }

  // Restore idx_div_tab/idx_cut_tab to default value.
  idx_div_tab = -1;
  idx_cut_tab = -1;
}

bool JOIN::setup_tmp_table_info(JOIN *orig) {
  ref_items[REF_SLICE_ACTIVE] = query_block->base_ref_items;
  tmp_table_param.pq_copy(orig->saved_tmp_table_param);
  saved_tmp_table_param = new (thd->mem_root) Temp_table_param();
  if (!saved_tmp_table_param) return true;
  saved_tmp_table_param->pq_copy(orig->saved_tmp_table_param);

  select_distinct = orig->select_distinct;
  if (restore_optimized_vars() || alloc_func_list()) return true;
  return false;
}

bool JOIN::make_leader_rewritten_tab() {
  if (alloc_qep1(tables) ||        /** alloc qep_tab1 */
      alloc_indirection_slices1()) /** alloc item_ref_slice */
    return true;

  assert(idx_div_tab >= 0);
  QEP_TAB *tab = &qep_tab[0];
  tab->pq_div_tab = true;
  POSITION *position = new (thd->pq_mem_root) POSITION;
  if (!position || position->pq_copy(thd, &qep_tab0[idx_div_tab])) return true;

  tab->set_position(position);
  if (tab->filesort) {
    ::destroy(tab->filesort);
    tab->filesort = nullptr;
  }
  tab->set_type(JT_ALL);

  /** create Table_ref object for explain */
  Table_ref *tbl = new (thd->pq_mem_root) Table_ref();
  if (!tbl) return true;

  char buff[64] = {0};
  TABLE *div_table = qep_tab0[idx_div_tab].table();
  assert(div_table);

  tbl->query_block = query_block;
  tbl->table_name = (char *)thd->memdup(div_table->s->table_name.str,
                                        div_table->s->table_name.length);
  tbl->table_name_length = div_table->s->table_name.length;
  tbl->db = (char *)thd->memdup(div_table->s->db.str, div_table->s->db.length);
  tbl->db_length = div_table->s->db.length;
  snprintf(buff, 64, "<gather%u>", query_block->select_number);
  tbl->alias = (char *)thd->memdup(buff, 64);
  tbl->set_tableno(0);
  if (!tbl->table_name || !tbl->db || !tbl->alias) return true;

  tab->table_ref = tbl;
  query_block->m_table_list.link_in_list(tbl, &tbl->next_local);
  Index_lookup *ref = new (thd->pq_mem_root) Index_lookup();
  if (!ref) return true;

  tab->set_ref(ref);
  tab->set_condition(nullptr);
  tab->set_split_table(div_table);
  tab->op_type = QEP_TAB::OT_RECV_MSG_TMP_TABLE;

  /** set base ref_items */
  ref_items[REF_SLICE_ACTIVE] = query_block->base_ref_items;
  // replace parallel scan table with a tmp table
  tmp_tables = primary_tables = const_tables = 0;
  need_tmp_pq_leader = true;

  return false;
}

static const char *PQ_UNSUITABLE_INFO[] = {
    "Internal conversion failed.",                         // INFO_NONE
    "Internal check failed.",                              // INTER_UNSUITE
    "Error happened during save properties or pq clone.",  // INTER_ERROR
    "Query with IN/ANY/ALL/SOME clause may transform to subquery which "
    "pq does not support.",  // SUBQUERY_TRANS
    "PQ only supported SELECT/INSERT_SELECT/REPLACE_SELECT command.",  // UNSUPPORT_COMMAND
    "Query is store procedure or trigger or prepare statement or "
    "attachable transaction or serializable with locking reads.",  // PREPARE_TRIGER_PROCEDURE
    "Parallel hash_join_spill_to_disk is off and the expected number of "
    "re-fill hash table exceeds its upper limit.",  // HASH_JOIN_SPILL
    "Table list has system table/transactional tmp table/system tmp table "
    "or tables have same alias.",  // SYS_OR_TMP_TABLE
    "Query has named view or a derived table which is outer-correlated. PQ "
    "leader could not mateialize it.",  // VIEW_OR_DERIVED_TABLE
    "Query has table function or explicit locking clause.",  // TABLE_FUNCTION_OR_LOCK
    "Query cached the result set(such as with SQL_BUFFER_RESULT or insert "
    "select into the same table) or with rollup or more than one value "
    "item of table value constructor or has windows function.",  // ROLLUP_WINDS_BUFFER
    "parallel insert_select is off or binlog isn't row based.",  // SWITCH_INSERT_SELECT
    "Query has unsupported scenarios including generated column, "
    "unsupported data type, unsupported function and so on.",  // UNSUPPORT_FUNCTION_DATATYPE
    "Item has unsupported ref type",  // UNSUPPORT_REF_TYPE
    "Item has a merged {view or derived table}'s column which wraps a "
    "subquery, and is referenced by another subquery",  // REF_WITH_SUBQUERY
    "Item has aggregate in top query which referenced by a subquery "
    "throught an alias",  // OUTREF_WITH_SUMITEM
    "Query has AGGREGATE(DISTINCT) and has blob in aggregation.",  // AGGR_DISTINCT
    "Query has aggregate which references outer column and is computed in "
    "the subquery.",                         // SUMITEM_OUTREF
    "Query has unsupported subquery-type.",  // UNSUPPORTED_SUBQUERY_TYPE
    "Query's cost less than parallel_cost_threshold or query has lateral "
    "derived tables or number of fields are more than MAX_FIELDS or rollup "
    "state does not support.",         // COST_OR_LATERAL
    "PQ message queue is saturated.",  // QUEUE_SATURATED
    "PQ does not support for group-by with indexing scan when there are "
    "few records in each group.",                                // SCAN_RECORDS
    "Query has zero result or primary tables deemed constant.",  // ZERO_RESULT
    "Query have ORDER BY<correlated subquery> and it doesn't cached.",  // ORDER_BY_SUBQUERY
    "No table meets the conditions for splitting to parallel scanning. "
    "Possible table's fetched row less than parallel_rows_threshold or "
    "query inclues unsupported scan-type or engine/multiple partitions to scan "
    "with partition table/a semi-join inner table/inner_table in BKA or "
    "outer join.",  // NO_DIVIDED_TABLE
    "Query does not comply with the ONLY_FULL_GROUP_BY rule which is "
    "required by PQ.",  // ONLY_FULL_GROUP_BY
    "Failed to execute the query using parallel query. The query was "
    "restarted without parallel query.",  // RETRY_WITHOUT_PQ
    "Trx is rolling back",                // ROLLBACK
    "Subquery will run single-threaded inside the parent query block's worker.",  // SUB_MTIP_WORKER
    "PQ memory size exceed parallel_memory_limit.",  // MEMORY_LIMIT
    "The number of remaining idle threads for PQ (parallel_max_threads - "
    "number of used threads) is less than parallel_default_dop.",  // IDLE_THREAD
    "Query does not meet the requirements of PQ support features switch, "
    "Check switch in pq_support_features_switch variable.",  // DISABLED_IN_PQ_SUPPORT_FEATURES
    "Query has enabled offset pushdown, so PQ cannot be enabled "
    "simultaneously.",  // OFFSET_PUSHDOWN_PRIO
    "The cost of the parallel query execution plan is higher than"
    " that of the serial query execution plan.",  // PQ_COST_HIGHER
    "Count(distinct) does not support PQ when JOIN::need_tmp_before_win is "
    "true.",  // COUNT_DISTINCT_NOT_SUPPORT_NEED_TEMP
    "Disable PQ execution when the query has a LIMIT clause but no ORDER BY"
    " clause.",                                          // LIMIT_NO_ORDERBY
    "No support INTERSECT and EXCEPT table operators.",  // UNSUPPORTED_INTERSECT_AND_EXCEPT
    "There are fields with the same name in the temporary table.",  // TMP_TABLE_FIELD_NAME_SAME
    "Query block implicitly generate aggregate function after prepare stage.",  // IMPLICIT_AGG_AFTER_PREPARE
    "The HAVING expression within the subquery references an alias from the "
    "outer query.",  // HAVING_WITH_OUTER_REF_IN_SUBQUERY
    "Disable PQ if access table is not InnoDB",  // ONLY_SUPPORT_INNODB
    "Query has NO_PQ hint or pq_master_enable is off or PQ is not supported in "
    "some scenarios.",                                                 // NO_PQ
    "Force_parallel_execute is off or parallel_default_dop is zero.",  // ZERO_DOP
    "Use hypergraph optimizer, PQ not support yet"  // HYPERGRAPH_OPTIMIZER
};

/**
 * Add the reason why pq does not take effect to OPTIMIZER TRACE
 *
  @param trace          trace context
  @param select_number  the number of current select
  @param reason         the reason why pq does not take effect
*/
static void add_reason_to_trace(Opt_trace_context *const trace,
                                uint select_number, const char *reason) {
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object trace_exec(trace, "not_apply_pq_plan");
  trace_exec.add_select_number(select_number);
  trace_exec.add("chosen", false);
  trace_exec.add_alnum("reason", reason);
}

/**
 * determine the query is suit for parallel query based on RBO
 */

bool JOIN::suite_for_parallel_query() {
  Opt_trace_context *const trace = &thd->opt_trace;

  // for quickly determine whether is suitable for parallel query
  if (!query_block->m_suite_for_pq) {
    if (thd->retry_without_pq) {
      query_block->pq_unsuite_info = PQUnsuiteInfo::RETRY_WITHOUT_PQ;
    }
    assert(query_block->pq_unsuite_info < PQUnsuiteInfo::PQ_MAX_UNSUITE);
    goto print_info;
  }

  if (query_block->group_list.elements > 0) {
    uint saved_group_list_elements =
        query_block->saved_group_list_ptrs
            ? query_block->saved_group_list_ptrs->size()
            : 0;
    // for subquery change to derived table scenario, query block may implicitly
    // generate aggregate function after prepare stage, currently pq record
    // optimized group order and restore optimized group order logic can not
    // support this scenario.
    if (saved_group_list_elements < query_block->group_list.elements) {
      query_block->pq_unsuite_info = PQUnsuiteInfo::IMPLICIT_AGG_AFTER_PREPARE;
      goto print_info;
    }
  }

  if (query_expression() &&
      query_expression()->subquery_suite_for_parallel_query() ==
          PQSubqueryExecution::kMultipleByWorkers) {
    // However suitable for PQ, this will run single-threaded, inside the
    // parent query block's worker. We return false to signal that. But still,
    // Query_block::m_suite_for_pq is true in this case.
    query_block->pq_unsuite_info = PQUnsuiteInfo::SUB_MTIP_WORKER;
    goto print_info;
  }

  for (const auto &item : *fields) {
    if (item->skip_create_tmp_table) {
      // When "skip_create_tmp_table == true", it means that the item has been
      // skipped in "create_tmp_table" (i.e., it will not be part of the
      // temporary table). This creates an issue for parallel query, because:
      //
      // 1) skip_create_tmp_table is set to true when the Item is considered
      //    "const". As of writing, create_tmp_table looks something like this:
      //
      //      if (item->const_item()) { skip_create_tmp_table = true; }
      //
      // 2) For fields/columns, the "constness" depends on, among other things,
      //    whether the field points to a constant table "TABLE::const_table"
      // 3) When we clone tables for workers, we always set TABLE::const_table
      //    to false (see pq_dup_select). Why this is done, is a bit unclear.
      // 4) This creates a problem, because the leader and workers now have
      //    different value for TABLE::const_table. The effect of this is that
      //    the worker may have extra columns in temporary tables because
      //    "item->const_item()" now returns false.
      //
      // To avoid this issue, we disable parallel query when an item is skipped
      // in temporary tables because it points to a constant table.
      bool depends_on_const_table = false;
      WalkItem(item, enum_walk::PREFIX,
               [&depends_on_const_table](Item *inner_item) {
                 if (inner_item->type() == Item::FIELD_ITEM) {
                   const auto item_field =
                       static_cast<const Item_field *>(inner_item);
                   if (item_field->table_ref != nullptr &&
                       item_field->table_ref->table != nullptr &&
                       item_field->table_ref->table->const_table) {
                     depends_on_const_table = true;
                   }
                 }
                 return false;
               });

      if (depends_on_const_table) {
        query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
        goto print_info;
      }
    }
  }

  // max PQ memory size limit
  if (get_pq_memory_total() >= parallel_memory_limit) {
    atomic_add<uint>(parallel_memory_refused, 1);
    query_block->pq_unsuite_info = PQUnsuiteInfo::MEMORY_LIMIT;
    goto print_info;
  }

  if (!choose_parallel_tables(true) || !check_pq_select_fields()) {
    if (query_block->pq_unsuite_info == PQUnsuiteInfo::INFO_NONE)
      query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
    goto print_info;
  }

  {
    bool has_threads = check_pq_running_threads(
        thd->pq_dop, thd->variables.parallel_queue_timeout);
    if (!has_threads ||
        DBUG_EVALUATE_IF("no_available_idle_threads", true, false)) {
      atomic_add<uint>(parallel_threads_refused, 1);
      query_block->pq_unsuite_info = PQUnsuiteInfo::IDLE_THREAD;
      goto print_info;
    }
  }

  if (!check_pq_support_features_switch()) {
    query_block->pq_unsuite_info =
        PQUnsuiteInfo::DISABLED_IN_PQ_SUPPORT_FEATURES;
    goto print_info;
  }

  if (!is_suit_for_pq_based_cost()) {
    query_block->pq_unsuite_info = PQUnsuiteInfo::PQ_COST_HIGHER;
    goto print_info;
  }

  query_block->parallel_exec = true;
  thd->has_pq = true;  // pass the RBO & CBO

  m_root_iterator =
      CreateIteratorFromAccessPath(thd, m_root_access_path, this, false);

  return true;

print_info:
  add_reason_to_trace(
      trace, query_block->select_number,
      PQ_UNSUITABLE_INFO[static_cast<int>(query_block->pq_unsuite_info)]);
  return false;
}

/**
  Verification of each pq_support_features_switch.
*/
static bool check_simple_agg(JOIN *join, bool &feature_switch_state) {
  feature_switch_state = join->thd->pq_support_features_switch_flag(
      PQ_SUPPORT_FEATURES_SWITCH_SIMPLE_AGG);

  //  The restrictions are as follows:
  //  1. SELECT command on one table and having aggragate functions
  //  2. no group by clause
  //  3. no join
  //  4. no count distinct, which is controlled by
  //     PQ_SUPPORT_FEATURES_SWITCH_COUNT_DISTINCT.
  if (!join->query_block->is_implicitly_grouped() || join->primary_tables > 1 ||
      join->has_count_distinct)
    return false;

  return true;
}

static bool check_count_distinct(JOIN *join, bool &feature_switch_state) {
  feature_switch_state = join->thd->pq_support_features_switch_flag(
      PQ_SUPPORT_FEATURES_SWITCH_COUNT_DISTINCT);

  return join->has_count_distinct;
}

/*
Note: When adding new features, pay attention to whether they overlap with other
scenarios.
*/
static std::vector<std::function<bool(JOIN *, bool &)>> check_features = {
    check_simple_agg, check_count_distinct};

/**
   Check query is suit for supporting features in
   pq_support_features_switch.
 */
bool JOIN::check_pq_support_features_switch() {
  bool feature_switch_state = false;
  for (const auto &check_func : check_features) {
    // check_func checks whether the feature scenario is satisfied.
    // feature_switch_state: whether the feature switch is on.
    if (check_func(this, feature_switch_state) && !feature_switch_state)
      return false;
  }

  return true;
}

static const enum_field_types NO_PQ_SUPPORTED_FIELD_TYPES[] = {
    MYSQL_TYPE_TINY_BLOB, MYSQL_TYPE_MEDIUM_BLOB, MYSQL_TYPE_BLOB,
    MYSQL_TYPE_LONG_BLOB, MYSQL_TYPE_JSON,        MYSQL_TYPE_GEOMETRY};

static const Item_sum::Sumfunctype NO_PQ_SUPPORTED_AGG_FUNC_TYPES[] = {
    Item_sum::GROUP_CONCAT_FUNC, Item_sum::JSON_AGG_FUNC,
    Item_sum::UDF_SUM_FUNC,      Item_sum::STD_FUNC,
    Item_sum::VARIANCE_FUNC,     Item_sum::SUM_DISTINCT_FUNC,
    Item_sum::AVG_DISTINCT_FUNC, Item_sum::SUM_BIT_FUNC};

static const Item_func::Functype NO_PQ_SUPPORTED_FUNC_TYPES[] = {
    Item_func::MATCH_FUNC,        Item_func::NOT_ALL_FUNC,
    Item_func::FUNC_SP,           Item_func::JSON_FUNC,
    Item_func::SUSERVAR_FUNC,     Item_func::UDF_FUNC,
    Item_func::XML_FUNC,          Item_func::SP_EQUALS_FUNC,
    Item_func::SP_DISJOINT_FUNC,  Item_func::SP_INTERSECTS_FUNC,
    Item_func::SP_TOUCHES_FUNC,   Item_func::SP_CROSSES_FUNC,
    Item_func::SP_WITHIN_FUNC,    Item_func::SP_CONTAINS_FUNC,
    Item_func::SP_COVEREDBY_FUNC, Item_func::SP_COVERS_FUNC,
    Item_func::SP_OVERLAPS_FUNC,  Item_func::SP_STARTPOINT,
    Item_func::SP_ENDPOINT,       Item_func::SP_EXTERIORRING,
    Item_func::SP_POINTN,         Item_func::SP_GEOMETRYN,
    Item_func::SP_INTERIORRINGN,  Item_func::JSON_CONTAINS};

static const char *NO_PQ_SUPPORTED_FUNC_ARGS[] = {
    "rand",
    "json_value",
    "json_valid",
    "json_length",
    "json_type",
    "json_contains_path",
    "json_unquote",
    "st_distance",
    "get_lock",
    "is_free_lock",
    "is_used_lock",
    "release_lock",
    "sleep",
    "xml_str",
    "json_func",
    "match",
    "weight_string",  // Data truncation (MySQL BUG)
    "des_decrypt",    // Data truncation
    "ST_LONGFROMGEOHASH",
    "ST_LATFROMGEOHASH",
    "json_depth",
    "json_quote",
    "json_schema_valid",
    "json_schema_validation_report",
    "json_storage_free",
    "sha",
    "sha2",
    "md5",
    "ps_thread_id",
    "gtid_subtract"};

static const char *NO_PQ_SUPPORTED_FUNC_NO_ARGS[] = {
    "user",      "current_user",      "current_role",
    "row_count", "release_all_locks", "ps_current_thread_id"};

static bool pq_not_support_scantype(QEP_TAB *tab) {
  auto scan_type = tab->type();
  /*
    Only support full table scan OR index scan OR range scan OR
    ref search on non-unique index.
  */
  if (scan_type != JT_ALL && scan_type != JT_INDEX_SCAN &&
      scan_type != JT_REF &&
      (scan_type != JT_RANGE || !tab->range_scan() ||
       tab->range_scan()->type != AccessPath::INDEX_RANGE_SCAN)) {
    return true;
  }
  /*
    Temptable engine's index is not well suited with PQ. See
    temptable::Handler::pq_leader_scan_init for more details. Note that when
    we come here, the temporary table hasn't been instantiated yet, so
    it might later, at instantion time, be converted to an InnoDB intrinsic
    table, if too big. Which would be dividable with index, but it will be too
    late to change the decision.
  */
  if (scan_type != JT_ALL &&  // using an index
      tab->table()->file->ht->db_type == DB_TYPE_TEMPTABLE)
    return true;

  return false;
}

bool pq_check_not_support_tab(QEP_TAB *tab, bool check_cut_table) {
  if (pq_not_support_scantype(tab) ||  // not support join type
      tab->using_dynamic_range || !tab->table()->suite_for_pq_division() ||
      (UseBKA(tab) && !QueryMixesOuterBKAAndBNL(
                          tab->join()))) {  // not support inner table in BKA
    return true;
  }

  if (check_cut_table) return false;

  // not support inner table in outer join
  if (tab->table_ref->is_inner_table_of_outer_join()) return true;

  POSITION *pos = tab->position();
  if (pos && pos->table) {
    if (pos->table->emb_sj_nest) {
      // dividing a semi-join inner table would produce duplicates
      return true;
    }
  } else {
    // If there is no POSITION, it is an internal temporary table used for
    // GROUP BY or ORDER BY.
    return true;
  }

  return false;
}

/*
 * return true when type is a not_supported_field; return false otherwise.
 */
static bool pq_not_support_datatype(enum_field_types type) {
  for (const enum_field_types &field_type : NO_PQ_SUPPORTED_FIELD_TYPES) {
    if (type == field_type) return true;
  }
  return false;
}

/**
 * check PQ supported function type.
 */
static bool pq_not_support_functype(Item_func::Functype type) {
  for (const Item_func::Functype &func_type : NO_PQ_SUPPORTED_FUNC_TYPES) {
    if (type == func_type) return true;
  }
  return false;
}

/**
 * check PQ supported function.
 */
static bool pq_not_support_func(Item_func *func, PQUnsuiteInfo *pq_info) {
  if (pq_not_support_functype(func->functype())) {
    *pq_info = PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
    return true;
  }
  for (const char *funcname : NO_PQ_SUPPORTED_FUNC_ARGS) {
    if (!strcmp(func->func_name(), funcname) && func->arg_count != 0) {
      *pq_info = PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
      return true;
    }
  }

  for (const char *funcname : NO_PQ_SUPPORTED_FUNC_NO_ARGS) {
    if (!strcmp(func->func_name(), funcname)) {
      *pq_info = PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
      return true;
    }
  }
  return false;
}

/**
 * check PQ support aggregation function.
 */
static bool pq_not_support_aggr_functype(Item_sum::Sumfunctype type) {
  for (const Item_sum::Sumfunctype &sum_func_type :
       NO_PQ_SUPPORTED_AGG_FUNC_TYPES) {
    if (sum_func_type == type) return true;
  }
  return false;
}

static const Item_ref::Ref_Type not_supported_type[] = {Item_ref::OUTER_REF};

/*
 * check PQ supported ref function
 */
bool pq_not_support_ref(Item_ref *ref) {
  Item_ref::Ref_Type type = ref->ref_type();
  for (auto &ref_type : not_supported_type) {
    if (type == ref_type) return true;
  }
  return false;
}

/**
 * check item is supported by Parallel Query or not
 *
 * @retval:
 *     true : supported
 *     false : not supported
 */
static bool check_pq_support_fieldtype(Item *arg, JOIN *join) {
  if (!arg) return false;
  if (arg->type() == Item::REF_ITEM) {
    Item_ref *item_ref = down_cast<Item_ref *>(arg);
    if (pq_not_support_ref(item_ref)) {
      join->query_block->pq_unsuite_info = PQUnsuiteInfo::UNSUPPORT_REF_TYPE;
      return false;
    }
    if (item_ref->used_tables() & OUTER_REF_TABLE_BIT) {
      // Case of an aggregate, in the top query, which is referencing a column
      // of the top query, and which is referenced by a subquery through
      // an alias: SELECT MIN(a) AS m ... WHERE (SELECT m ... )
      if (item_ref->real_item()->type() == Item::SUM_FUNC_ITEM) {
        join->query_block->pq_unsuite_info = PQUnsuiteInfo::OUTREF_WITH_SUMITEM;
        return false;
      }
      // Case of a merged {view or derived table}'s column which wraps a
      // subquery, and is referenced by another subquery. Example:
      // CREATE VIEW v AS SELECT (subq1) AS c FROM t1;
      // SELECT 1 FROM v WHERE (SELECT v.c FROM t2);
      // After merging of 'v', we have, roughly,
      // SELECT 1 FROM t1 WHERE (SELECT (subq1) FROM t2);
      // So we have a correlated subquery inside a correlated subquery. This
      // is not supported by PQ, and subquery_suite_for_parallel_query() tries
      // to enforce that by checking, for subquery-on-t2, if it has any inner
      // Query_expression. But here it fails, because during merging of 'v'
      // into the root SELECT, the Query_expression of subq1 is made a child of
      // the _root_ SELECT, not of subquery-on-t2. And this relationship
      // cannot be changed, as one view's column can be referenced from
      // multiple places but still has one single underlying Item expression
      // which itself has one single underlying Query_expression - consider
      // what would happen for SELECT v.c FROM v WHERE (SELECT v.c FROM t2)
      // where both v.c are two Item_view_ref pointing to the same
      // Item_subselect - who should be the parent unit of subq1's unit? There
      // is no good answer, and MySQL's simply chooses the recipient of
      // merging, here the root SELECT. That is why we block this situation.
      // This N-references/1-subquery-expression issue is also the cause of
      // numerous non-PQ problems (grep for 'view_ref_with_subquery' and
      // 'is_direct_view_ref').
      if (item_ref->ref_type() == Item_ref::VIEW_REF &&
          item_ref->has_subquery()) {
        join->query_block->pq_unsuite_info = PQUnsuiteInfo::REF_WITH_SUBQUERY;
        return false;
      }
    }
  }

  Item *const item = arg->real_item();
  if (!item) return false;
  if (pq_not_support_datatype(item->data_type())) {
    join->query_block->pq_unsuite_info =
        PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
    return false;
  }

  auto item_type = item->type();
  if (item_type == Item::FIELD_ITEM) {
    Field *field = static_cast<Item_field *>(item)->field;
    assert(field);
    // not supported for generated column
    if (field && (field->is_gcol() || pq_not_support_datatype(field->type()))) {
      join->query_block->pq_unsuite_info =
          PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
      return false;
    }
  } else if (item_type == Item::FUNC_ITEM) {
    Item_func *func = static_cast<Item_func *>(item);
    assert(func);

    // check func type
    if (pq_not_support_func(func, &join->query_block->pq_unsuite_info))
      return false;

    // the case of Item_in_optimizer
    if (!strcmp(func->func_name(), "<in_optimizer>")) {
      return false;
    }
    // the case of Item_func_make_set
    if (!strcmp(func->func_name(), "make_set")) {
      Item *arg_item = down_cast<Item_func_make_set *>(func)->item;
      if (arg_item && !check_pq_support_fieldtype(arg_item, join)) return false;
    }

    if (func->functype() == Item_func::TRIG_COND_FUNC) {
      auto *trig_cond = down_cast<Item_func_trig_cond *>(func);
      // For case of IN->EXISTS subquery transformation, there is correlative
      // subquery. Since it lack PQ infrastructure for correlative subquery, so
      // we should skip PQ for this case.
      if (trig_cond->get_trig_type() ==
              Item_func_trig_cond::OUTER_FIELD_IS_NOT_NULL &&
          trig_cond->get_trig_var() != nullptr)
        return false;
    }

    // check func args type
    for (uint i = 0; i < func->arg_count; i++) {
      // c1: args contain unsupported fields
      Item *arg_item = func->arguments()[i];
      if (!arg_item || !check_pq_support_fieldtype(arg_item, join))
        return false;
    }

    // the case of Item_equal
    if (func->functype() == Item_func::MULT_EQUAL_FUNC) {
      Item_equal *item_equal = down_cast<Item_equal *>(item);
      assert(item_equal);

      // check const_item
      Item *const_item = item_equal->const_arg();
      if (const_item && (const_item->type() == Item::SUM_FUNC_ITEM ||     // c1
                         !check_pq_support_fieldtype(const_item, join)))  // c2
        return false;

      // check fields
      Item *field_item;
      auto it = item_equal->get_fields().begin();
      while (it != item_equal->get_fields().end()) {
        field_item = &*it++;  // Field to generate equality for.
        if (!check_pq_support_fieldtype(field_item, join)) return false;
      }
    }

  } else if (item_type == Item::COND_ITEM) {
    Item_cond *cond = static_cast<Item_cond *>(item);
    assert(cond);

    if (pq_not_support_functype(cond->functype())) {
      join->query_block->pq_unsuite_info =
          PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
      return false;
    }
    Item *arg_item;
    List_iterator_fast<Item> it(*cond->argument_list());
    while ((arg_item = it++)) {
      if (arg_item->type() == Item::SUM_FUNC_ITEM ||    // c1
          !check_pq_support_fieldtype(arg_item, join))  // c2
        return false;
    }
  } else if (item_type == Item::SUM_FUNC_ITEM) {
    // select hex(min(a)), hex(max(b)), min(a) is const
    if (item->const_item()) return false;
    Item_sum *sum = static_cast<Item_sum *>(item);
    if (!sum) return false;
    join->has_count_distinct |=
        (sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC);
    if (pq_not_support_aggr_functype(sum->sum_func())) {
      join->query_block->pq_unsuite_info =
          PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
      return false;
    }

    for (uint i = 0; i < sum->arg_count; i++) {
      if (!check_pq_support_fieldtype(sum->get_arg(i), join)) return false;
    }

    if (sum->has_with_distinct() && sum->has_blob_in_aggr()) {
      join->query_block->pq_unsuite_info = PQUnsuiteInfo::AGGR_DISTINCT;
      return false;
    }

    if (item->used_tables() & OUTER_REF_TABLE_BIT) {
      /*
        Subquery with aggregate, aggregate which references outer column and
        is computed in the subquery:
        SELECT ... FROM t1 WHERE (SELECT MIN(t1.a) ... )
        This is not redundant with the test for item->has_aggregation()
        further down because the aggregation is made in the subquery (because
        the subquery in in the top Q's WHERE).
      */
      join->query_block->pq_unsuite_info = PQUnsuiteInfo::SUMITEM_OUTREF;
      return false;
    }
  } else if (item_type == Item::REF_ITEM) {
    // As 'item' is the result of real_item(), this branch is probably dead.
    Item_ref *item_ref = down_cast<Item_ref *>(item);
    if (pq_not_support_ref(item_ref)) {
      join->query_block->pq_unsuite_info = PQUnsuiteInfo::UNSUPPORT_REF_TYPE;
      return false;
    }

    if (!check_pq_support_fieldtype(item_ref->ref_pointer()[0], join))
      return false;

    if (item_ref->ref_type() == Item_ref::VIEW_REF) {
      auto view_table =
          down_cast<Item_view_ref *>(item_ref)->get_first_inner_table();
      if (view_table) {
        bool found_view_in_list = false;
        for (uint i = 0; i < join->primary_tables; i++) {
          if (view_table == join->qep_tab[i].table_ref) {
            found_view_in_list = true;
          }
        }
        // When cloning Item_view_ref, we cannot find "first_inner_table"
        // and thus should disable it.
        if (found_view_in_list == false) {
          return false;
        }
      }
    }
  } else if (item_type == Item::CACHE_ITEM) {
    Item_cache *item_cache = dynamic_cast<Item_cache *>(item);
    assert(item_cache);

    Item *example_item = item_cache->get_example();
    if (!example_item || example_item->type() == Item::SUM_FUNC_ITEM ||  // c1
        !check_pq_support_fieldtype(example_item, join))                 // c2
      return false;
  } else if (item_type == Item::ROW_ITEM) {
    // check each item in Item_row
    Item_row *row_item = down_cast<Item_row *>(item);
    for (uint i = 0; i < row_item->cols(); i++) {
      Item *n_item = row_item->element_index(i);
      if (!n_item || n_item->type() == Item::SUM_FUNC_ITEM ||  // c1
          !check_pq_support_fieldtype(n_item, join))           // c2
        return false;
    }
  } else if (item_type == Item::SUBSELECT_ITEM) {
    // Subquery with aggregate, aggregate which references outer column and is
    // computed in the top query:
    // SELECT (SELECT .. ORDER BY MIN(t1.a)) FROM t1;
    if (item->has_aggregation()) {
      join->query_block->pq_unsuite_info = PQUnsuiteInfo::SUMITEM_OUTREF;
      return false;
    }
    auto *sub_item = down_cast<Item_subselect *>(item);
    if (sub_item->unit && sub_item->unit->subquery_suite_for_parallel_query() !=
                              PQSubqueryExecution::kImpossible) {
      return true;
    }
    join->query_block->pq_unsuite_info =
        PQUnsuiteInfo::UNSUPPORTED_SUBQUERY_TYPE;
    return false;
  } else if (item_type == Item::VALUES_COLUMN_ITEM) {
    return false;  // clone function not implemented.
  }
  return true;
}

/*
 * generate item's result_field
 *
 * @retval:
 *    true: all generated fields are suite for parallel query
 *    false: otherwise
 */

bool pq_create_result_fields(THD *thd, Temp_table_param *param,
                             mem_root_deque<Item *> *fields,
                             bool save_sum_fields, ulonglong select_options,
                             PQUnsuiteInfo *pq_info) {
  const bool not_all_columns = !(select_options & TMP_TABLE_ALL_COLUMNS);
  long hidden_field_count = param->hidden_field_count;
  Field *from_field = nullptr;
  Field **tmp_from_field = &from_field;
  Field **default_field = &from_field;

  TABLE_SHARE s;
  TABLE table;
  table.s = &s;

  uint copy_func_count = param->func_count;
  if (param->precomputed_group_by) copy_func_count += param->sum_func_count;

  Func_ptr_array *copy_func = new (thd->mem_root) Func_ptr_array(thd->mem_root);
  if (!copy_func) return false;

  copy_func->reserve(copy_func_count);
  for (uint i = 0; i < fields->size(); i++) {
    Item *item = (*fields)[i];
    Field *new_field = NULL;
    Item::Type type = item->type();
    const bool is_sum_func =
        type == Item::SUM_FUNC_ITEM && !item->m_is_window_function;

    if (not_all_columns) {
      if (item->has_aggregation() && type != Item::SUM_FUNC_ITEM) {
        if (item->used_tables() & OUTER_REF_TABLE_BIT)
          item->update_used_tables();
        if (type == Item::SUBSELECT_ITEM ||
            (item->used_tables() & ~OUTER_REF_TABLE_BIT)) {
          param->using_outer_summary_function = 1;
          goto update_hidden;
        }
      }
      if (item->m_is_window_function) {
        if (!param->m_window || param->m_window_frame_buffer) {
          goto update_hidden;
        }
        if (param->m_window != down_cast<Item_sum *>(item)->window()) {
          goto update_hidden;
        }
      } else if (item->has_wf()) {
        if (param->m_window == nullptr || !param->m_window->is_last())
          goto update_hidden;
      }
      if (item->const_item() && (int)hidden_field_count <= 0)
        continue;  // We don't have to store this
    }

    if (is_sum_func && !save_sum_fields) {
      /* Can't calc group yet */
    } else {
      new_field =
          (param->schema_table)
              ? create_tmp_field_for_schema(item, &table)
              : create_tmp_field(thd, &table, item, type, copy_func,
                                 tmp_from_field, default_field, false,  //(1)
                                 false,
                                 item->marker == Item::MARKER_BIT ||
                                     param->bit_fields_as_long,  //(2)
                                 false);

      if (!new_field) {
        assert(thd->is_fatal_error());
        return false;
      }

      if (pq_not_support_datatype(new_field->type())) {
        *pq_info = PQUnsuiteInfo::UNSUPPORT_FUNCTION_DATATYPE;
        return false;
      }
    }

  update_hidden:
    if (!--hidden_field_count) {
      param->hidden_field_count = 0;
    }
  }  // end of while ((item=li++)).

  return true;
}

/**
 * check whether the select result fields is suitable for parallel query
 *
 * @return:
 *    true, suitable
 *    false.
 */
bool JOIN::check_pq_select_fields() {
  DBUG_TRACE;

  MEM_ROOT *saved_root = thd->mem_root;
  MEM_ROOT *savd_thr_root = *THR_MALLOC;
  mem_root_deque<Item *> *tmp_all_fields = nullptr;
  Temp_table_param *tmp_param = nullptr;
  std::vector<Field *> saved_result_field;
  bool suite_for_pq = false;

  MEM_ROOT *pq_check_root = ::new MEM_ROOT();
  if (!pq_check_root) goto err;
  init_sql_alloc(key_memory_thd_main_mem_root, pq_check_root,
                 global_system_variables.query_alloc_block_size);
  assert(thd->mem_root == *THR_MALLOC);
  thd->mem_root = pq_check_root;
  THR_MALLOC = &thd->mem_root;

  tmp_all_fields = (last_slice_before_pq == REF_SLICE_SAVED_BASE)
                       ? fields
                       : &tmp_fields0[last_slice_before_pq];
  tmp_param = new (pq_check_root) Temp_table_param();
  if (!tmp_param) goto err;
  tmp_param->pq_copy(&tmp_table_param);
  tmp_param->m_window_frame_buffer = false;
  tmp_param->hidden_field_count = CountHiddenFields(*tmp_all_fields);

  suite_for_pq = pq_create_result_fields(thd, tmp_param, tmp_all_fields, true,
                                         query_block->active_options(),
                                         &query_block->pq_unsuite_info);
err:
  // free the memory
  pq_check_root->Clear();
  if (pq_check_root) ::delete pq_check_root;
  thd->mem_root = saved_root;
  *THR_MALLOC = savd_thr_root;
  return suite_for_pq;
}

bool JOIN::pq_copy_from(JOIN *orig) {
  // HUAWEI_ASSERT_MATCH_CANON sql_class_JOIN

  if (alloc_indirection_slices()) return true;

  query_block->join = this;
  where_cond = query_block->where_cond();
  tables_list = query_block->leaf_tables;
  having_for_explain = orig->having_for_explain;
  tables = orig->tables;
  explain_flags = orig->explain_flags;
  set_plan_state(orig->plan_state);
  zero_result_cause = orig->zero_result_cause;
  idx_div_tab = orig->idx_div_tab;
  idx_cut_tab = orig->idx_cut_tab;
  calc_found_rows = orig->calc_found_rows;
  m_select_limit = orig->m_select_limit;
  query_expression()->select_limit_cnt =
      orig->query_expression()->select_limit_cnt;
  query_expression()->offset_limit_cnt =
      orig->query_expression()->offset_limit_cnt;
  if (query_block->parallel_exec) {
    // If there are workers, they should not apply offset, as it depends on
    // how many rows are returned by other workers. But if this is a cloned
    // correlated subquery, the worker is the single executor and should apply
    query_expression()->offset_limit_cnt = 0;
  }
  pq_stable_sort = orig->pq_stable_sort;
  saved_optimized_vars = orig->saved_optimized_vars;
  send_group_parts = orig->send_group_parts;
  saved_join_order = query_block->saved_order_list_ptrs;
  found_const_table_map = orig->found_const_table_map;
  has_count_distinct = orig->has_count_distinct;

  if (orig->saved_join_group_list == orig->query_block->saved_group_list_ptrs) {
    saved_join_group_list = query_block->saved_group_list_ptrs;
  } else {
    SQL_I_List<ORDER> cloned_list;
    for (auto group : *orig->saved_join_group_list) {
      ORDER *new_group = pq_dup_order(thd, query_block, group);
      if (!new_group ||
          find_order_in_list(
              thd, query_block->base_ref_items, query_block->get_table_list(),
              new_group, query_block->get_fields_list(), true, false, true)) {
        return true;
      }
      cloned_list.link_in_list(new_group, &new_group->next);
    }

    if (query_block->save_order_properties(thd, &cloned_list,
                                           &saved_join_group_list))
      return true;
  }

  // See comment in TABLE::pq_copy about partial result cache:
  best_rowcount = orig->best_rowcount;
  pq_pushdown_having = orig->pq_pushdown_having;

  // Hypergraph optimizer doesn't create QEP_TABs, and PQ clones QEP_TABs.
  assert(!thd->lex->using_hypergraph_optimizer());
  assert(temp_tables.empty() && filesorts_to_cleanup.empty() &&
         !m_root_access_path_no_in2exists &&
         !hash_table_generation);  // only used by hypergraph optimizer
  // JOIN::semijoin_deduplication_fields needs no copying, it's set only by an
  // iterator. The same for JOIN::hash_table_generation, it's a counter used
  // only by an iterator and set when creating an access path. As we create
  // access paths and iterators for each worker there is no problem.
  return false;
}

static inline bool check_div_table_rows_fetched(const QEP_TAB *qep_tab,
                                                ulong parallel_rows_threshold) {
  /*
    qep_tab->position()->rows_fetched is # of rows which will be read by the
    access method, minus those which will not pass the constant condition;
    that's how calculate_scan_cost() works. At this point, we need to obtain
    the actual number of rows scanned by the table.
  */
  double rows_fetched = qep_tab->position()->rows_fetched;
  if (qep_tab->type() == JT_ALL || qep_tab->type() == JT_INDEX_SCAN) {
    TABLE *const table = qep_tab->table();
    rows_fetched = static_cast<double>(table->file->stats.records);
  } else if (qep_tab->type() == JT_RANGE || qep_tab->type() == JT_INDEX_MERGE) {
    rows_fetched = rows2double(qep_tab->range_scan()->num_output_rows());
  }
  return (rows_fetched > static_cast<double>(parallel_rows_threshold));
}

/**
 * choose a table that do parallel query, currently only do parallel scan on
 * first no-const primary table. If the choice is successful, backs up some
 * members of this JOIN to be ready for PQ.
 *
 *
 * @return:
 *    true, found a parallel scan table
 *    false, can't find a parallel scan table
 */

bool JOIN::choose_parallel_tables(bool do_mark) {
  if (thd->no_pq || (!query_block->m_suite_for_pq)) return false;

  bool is_correlated_subquery =
      (query_expression() &&
       query_expression()->subquery_suite_for_parallel_query() ==
           PQSubqueryExecution::kMultipleByWorkers);
  // For a correlated subquery, its execution is going to be split into multiple
  // worker threads and does not use PQ. So some checks for suiting for PQ don't
  // need be done for correlated subquery, such as query's cost, rows cost.
  if ((!is_correlated_subquery &&
       best_read < thd->variables.parallel_cost_threshold) ||
      (has_lateral) || (query_block->fields.size() > MAX_FIELDS) ||
      (rollup_state != RollupState::NONE) ||
      query_block->pq_check_table_list()) {
    if (query_block->pq_unsuite_info == PQUnsuiteInfo::INFO_NONE)
      query_block->pq_unsuite_info = PQUnsuiteInfo::COST_OR_LATERAL;
    return false;
  }

  if (qep_tab) {
    // check whether support table type.
    for (uint i = 0; i < tables; i++) {
      TABLE *table = qep_tab[i].table();
      if (table && table->file->ht->db_type == DB_TYPE_DSTORE) {
        query_block->pq_unsuite_info = PQUnsuiteInfo::ONLY_SUPPORT_INNODB;
        return false;
      }
    }

    // check whether tables have same ident.
    for (uint i = 0; i < tables; i++) {
      Table_ref *tbl = qep_tab[i].table_ref;
      if (!tbl) continue;
      for (uint j = 0; j < i; j++) {
        Table_ref *prev_tbl = qep_tab[j].table_ref;
        if (prev_tbl &&
            !my_strcasecmp(table_alias_charset, prev_tbl->alias, tbl->alias) &&
            !strcmp(prev_tbl->db, tbl->db)) {
          query_block->pq_unsuite_info = PQUnsuiteInfo::SYS_OR_TMP_TABLE;
          return false;
        }
      }
    }
    /*
     * If the selectivity value of group field is low (that is, each group has
     * only a few number of records), performing "group-by" in parallel
     * execution might be inefficient, espcially with "limit" cluase. We utilize
     * some statistical information to decide whether to execute "group by" in
     * PQ. Briefly, assuming that there are N records and K groups in the query,
     * and I) each worker scans N/dop records, II) each worker produces
     * min{N/dop, K} partial results,
     *
     * Thus, the total number of records scanned is N/dop + min{N/dop, K} * dop
     * in parallel execution. Only when the following conditions is satified:
     * N/dop + min{N/dop, K} * dop < #limit * N / K, we perform parallel "group
     * by", where #limit is the limit value.
     *
     * Currently, we only consider the case that "group by" is performed in one
     * primary table and is implemented by indexing scan. For the case of
     * "group-by" implememted using "tmp table", we will consider it later.
     *
     */

    if (do_mark && m_ordered_index_usage == ORDERED_INDEX_GROUP_BY &&
        primary_tables == (const_tables + 1)) {
      QEP_TAB *_tab = &qep_tab[primary_tables - 1];
      TABLE *table = _tab->table();
      uint64 table_records = table->file->stats.records;  //#records in table

      uint keyno = MAX_KEY;
      if (_tab->type() == JT_RANGE) {
        keyno = used_index(_tab->range_scan());
      } else if (_tab->type() == JT_INDEX_SCAN) {
        keyno = _tab->index();
      } else if (_tab->type() == JT_REF) {
        keyno = _tab->ref().key;
      }

      if (keyno != MAX_KEY) {
        KEY *key = &table->key_info[keyno];
        uint key_parts = key->user_defined_key_parts;
        if ((1 <= send_group_parts && send_group_parts <= key_parts) &&
            key->rec_per_key[send_group_parts - 1] > 0) {
          /** estimate #groups in the query */
          uint64 estimate_groups =
              table_records / key->rec_per_key[send_group_parts - 1];
          if (estimate_groups > 0) {
            /** estimate scanned #records of original execution plan */
            uint64 orig_scan_records =
                key->rec_per_key[send_group_parts - 1] *
                (query_expression()->select_limit_cnt != HA_POS_ERROR
                     ? query_expression()->select_limit_cnt
                     : estimate_groups);
            orig_scan_records = std::min(orig_scan_records, table_records);

            /** see the RBO computation method in second paragraph in last
             *  comment */
            uint64 pq_scan_records =
                table_records / thd->pq_dop +
                std::min(table_records / thd->pq_dop, estimate_groups) *
                    thd->pq_dop;
            if (pq_scan_records > orig_scan_records) {
#ifndef NDEBUG
              sql_print_information(
                  "Parallel query: not support for group-by with indexing scan "
                  "as there are few records in each group");
#endif  // NDEBUG
              query_block->pq_unsuite_info = PQUnsuiteInfo::SCAN_RECORDS;
              return false;
            }
          }
        }
      }
    }

  }  // end of if(qep_tab)

  // check whether contains blob, text, json and geometry field
  for (auto iter = query_block->fields.begin();
       iter != query_block->fields.end(); ++iter) {
    if (!check_pq_support_fieldtype(*iter, this)) {
      if (query_block->pq_unsuite_info == PQUnsuiteInfo::INFO_NONE)
        query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
      return false;
    }
  }

  // Check WHERE and HAVING condition.
  for (auto cond : {query_block->where_cond(), query_block->having_cond()}) {
    if (cond && !check_pq_support_fieldtype(cond, this)) {
      if (query_block->pq_unsuite_info == PQUnsuiteInfo::INFO_NONE)
        query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
      return false;
    }
  }

  // For example: 'SELECT SUM(a) AS s, (SELECT 1 HAVING s) FROM t' with
  // 'subquery_to_derived=on'. The query 'SELECT SUM(a) AS s FROM t' is
  // materialized as derived_1_3, and the correlated subquery
  // '(SELECT 1 HAVING s)' references the derived table field derived_1_3.s.
  // Worker reuses HAVING Item from leader plan, but ref slice switch causes
  // Item_ref to point to <receive_data>.s. Worker fails to evaluate filter
  // correctly, leading to wrong results. Therefore, PQ needs to be disabled in
  // this scenario.
  if (query_block->having_cond() &&
      (query_block->having_cond()->used_tables() & OUTER_REF_TABLE_BIT)) {
    query_block->pq_unsuite_info =
        PQUnsuiteInfo::HAVING_WITH_OUTER_REF_IN_SUBQUERY;
    return false;
  }

  for (uint i = const_tables; i < tables; i++) {
    Item *cond = qep_tab[i].condition();
    if (cond && !check_pq_support_fieldtype(cond, this)) {
      if (query_block->pq_unsuite_info == PQUnsuiteInfo::INFO_NONE)
        query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
      return false;
    }
  }

  /*
     A correlated subquery is not supported if located inside a join condition
     (which includes anti-join and left-join). Whereas inner-join and
     semi-join conditions are ok. Here is why. Consider:
       select * from t1, t2 where t2.a=(select t1.a from t3);
     In chronological order of PQ steps:
     1) saved_where_cond is made, contains a reference to the Item_subselect
     (see Item_singlerow_subselect::pq_clone(), with pq_try_clone_item==true).
     2) pq_dup_select() duplicates the Query_block of the top query then the
     Query_block defining the subquery.
     3) pq_dup_select() duplicates the WHERE clause of the top query (which is
     found in saved_where_cond), which
     calls Item_singlerow_subselect::pq_clone() with pq_try_clone_item==false,
     which makes a real clone and establishes bidirectional links between it
     and the just-made Query_block clone of the subquery (e.g.
     Item_singlerow_subselect::unit == Query_block::master_unit()).
     4) pq_select_prepare() is called, fixes fields of the subquery,
     especially the outer reference t1.a. Item_field::fix_outer_field() uses
     the link between Query_block and Item_singlerow_subselect to achieve
     resolution.
     Now let's consider:
       select * from t1 left join t2 on t2.a=(select t1.a from t3);
     The subquery item is in t2->join_cond(), not in WHERE. So it's not in
     saved_where_cond: it's not cloned. So the logic above fails. However, the
     subquery item is in QEP_TAB::condition() too. We could think this would
     solve the problem as pq_dup_tabs() then clones it. But no, because
     pq_dup_tabs() runs _after_ pq_select_prepare(), too late.

     Another example:
     SELECT 1 FROM t1 WHERE NOT EXISTS
        (SELECT 2 FROM t2 WHERE (SELECT 3 FROM t3 WHERE t1.c >= 1));
     which becomes t1 ANTI-JOIN t2 ON (SELECT 3 etc).
     Again the most inner subquery is in t2's join condition.

     Semi-joins like:
     SELECT 1 FROM t1 WHERE EXISTS
        (SELECT 2 FROM t2 WHERE (SELECT 3 FROM t3 WHERE t1.c >= 1));
     have no problem, because the semi-join condition is put into WHERE (see
     convert_subquery_to_semijoin()), so in saved_where_cond. For the same
     reason, inner-join is ok (they are converted to comma-joins with WHERE).

     So we walk all join conditions, if such condition contains a correlated
     subquery then we cannot do PQ. We think that supporting the cases above
     is not worth the effort.
  */
  auto is_correlated_subq = [](Item *item) {
    return item->type() == Item::SUBSELECT_ITEM &&
           down_cast<Item_subselect *>(item)->is_uncacheable();
  };

  // Trick inspired by WalkItem template.
  if (query_block->m_current_table_nest &&
      walk_join_condition(
          query_block->m_current_table_nest,
          &Item::walk_helper_thunk<decltype(is_correlated_subq)>,
          enum_walk::POSTFIX, reinterpret_cast<uchar *>(&is_correlated_subq))) {
    query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
    return false;
  }

  // Search for a table which could be divided.
  int div_tab = -1;

  if (is_correlated_subquery) {
    /*
      Exception: for a correlated subquery, we don't divide any table (each
      worker executes the subquery on its own) so the lack of a divided table
      is not a problem; the common example is a subquery selecting from a
      single table which is one of those cases blocked by
      pq_not_support_scantype(): a materialized derived table accessed through
      an auto-generated index, or a base table accessed through its primary
      key with EQ_REF.
    */
    assert(!do_mark);
    goto ok;
  }

  // If we come here, this is not a correlated subquery. Therefore, its
  // execution is going to be split into multiple worker threads, which makes no
  // sense if this execution is known to be instant:
  if (tables_list == nullptr || zero_result_cause ||
      (primary_tables == const_tables)) {
    query_block->pq_unsuite_info = PQUnsuiteInfo::ZERO_RESULT;
    return false;
  }

  if (!need_tmp_before_win && !order.empty()) {
    // This is to block a case where PQ is slower than non-PQ. If there is
    // "ORDER BY <correlated subquery>", and no buffering before ORDER BY
    // (need_tmp_before_win==false), then the worker will evaluate the
    // subquery for its own filesort, and then evaluate it again when sending
    // the row to the leader's MQ. If the subquery is expensive, this
    // double-evaluation can make PQ very slow. On the contrary, if there is
    // buffering, the worker sends an Item_field to MQ, which is cheap.
    // If the subquery is not correlated, it is actually executed only once
    // and its value is cached for all next times.
    for (auto *o = order.order; o; o = o->next) {
      auto i = *o->item;
      if (i->has_subquery() && WalkItem(i, enum_walk::PREFIX, [](Item *item) {
            if (item->type() == Item::SUBSELECT_ITEM) {
              const auto item_subq = static_cast<const Item_subselect *>(item);
              if (item_subq->is_uncacheable()) return true;
            }
            return false;
          })) {
        query_block->pq_unsuite_info = PQUnsuiteInfo::ORDER_BY_SUBQUERY;
        return false;
      }
    }
  }
  if (need_tmp_before_win && has_count_distinct) {
    query_block->pq_unsuite_info =
        PQUnsuiteInfo::COUNT_DISTINCT_NOT_SUPPORT_NEED_TEMP;
    return false;
  }

  // PQ hint
  for (uint i = const_tables; i < primary_tables; i++) {
    bool pq_table_on =
        hint_table_state(thd, qep_tab[i].table_ref, PQ_HINT_ENUM, 0);
    if (pq_table_on) {
      if (pq_check_not_support_tab(&qep_tab[i], false)) {
        if (do_mark) {
          std::stringstream ss;
          ss << "Table " << qep_tab[i].table_ref->alias
             << " does not support to be chosen as parallel table";
          push_warning(thd, Sql_condition::SL_WARNING, ER_PARALLEL_QUERY_ERROR,
                       ss.str().c_str());
        }
        continue;
      }
      div_tab = i;
      if (do_mark) qep_tab[i].pq_div_tab = true;
      break;
    }
  }

  // No PQ hint, Find out divided table
  if (div_tab < 0) {
    // When parallel_rows_threshold > 0, find table whose fetched
    // rows > parallel_rows_threshold as divided table.
    // Otherwise, find the largest table as divided table.
    if (thd->variables.parallel_rows_threshold > 0) {
      for (uint i = const_tables; i < primary_tables; i++) {
        if (check_div_table_rows_fetched(
                &qep_tab[i], thd->variables.parallel_rows_threshold) &&
            !pq_check_not_support_tab(&qep_tab[i], false)) {
          div_tab = i;
          if (do_mark) qep_tab[i].pq_div_tab = true;
          break;
        }
      }
    } else {
      std::vector<std::pair<uint, ha_rows>> rows_in_tables;
      for (uint i = const_tables; i < primary_tables; i++) {
        rows_in_tables.emplace_back(i, qep_tab[i].position()->rows_fetched);
      }
      std::sort(rows_in_tables.begin(), rows_in_tables.end(),
                [](const std::pair<uint, ha_rows> &a,
                   const std::pair<uint, ha_rows> &b) {
                  return a.second > b.second;
                });
      for (auto pair : rows_in_tables) {
        if (pq_check_not_support_tab(&qep_tab[pair.first], false)) continue;
        div_tab = pair.first;
        if (do_mark) qep_tab[pair.first].pq_div_tab = true;
        break;
      }
    }
  }

  if (div_tab >= 0 && do_mark) {
    // Find out cut table after accesspath tree built
    MarkCutTable(m_root_access_path, this);
  }

  if (div_tab < 0) {
    query_block->pq_unsuite_info = PQUnsuiteInfo::NO_DIVIDED_TABLE;
    return false;
  }

ok:
  // If do_mark, "save_XX" objects have already been createdy
  if (!do_mark) {
    // save temp table param for later PQ scan
    saved_tmp_table_param = new (thd->pq_mem_root) Temp_table_param();
    if (!saved_tmp_table_param ||
        DBUG_EVALUATE_IF("have_error_in_saving", true, false)) {
      query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_ERROR;
      return false;
    }
    set_push_down_having();
    saved_tmp_table_param->pq_copy(&tmp_table_param);
    // saved optimized variables to saved_optimized_vars.
    save_optimized_vars();
  }
  return true;
}

void check_pq_suite_for_insert_select(THD *thd, Query_block *query_block) {
  // INSERT SELECT statements place row locks on the rows of selected-from table
  // and parallel query (PQ) does not have a row locking logic. Such locks are
  // however only necessary for replication when the binlog is statement-based.
  // For INSERT INTO SELECT/ REPLACE INTO SELECT statements to be
  // run with PQ, it requires that in
  // store_lock (ha_innodb.cc) S locks were not placed on the selected-from
  // table:
  // For that to have happened, it entails:
  // a) parallel insert_select is on (switch for this optimization)
  // b) Binlog is row based - need S locks for replication otherwise
  if ((thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
       thd->lex->sql_command == SQLCOM_REPLACE_SELECT) &&
      query_block->m_suite_for_pq &&
      (!thd->pq_support_features_switch_flag(
           PQ_SUPPORT_FEATURES_SWITCH_INSERT_SELECT) ||  // a)
       !thd->is_current_stmt_binlog_format_row())) {     // b)
    query_block->m_suite_for_pq = false;
    query_block->pq_unsuite_info = PQUnsuiteInfo::SWITCH_INSERT_SELECT;
  }
}

void disable_pq_if_limit_without_orderby(THD *thd, Query_block *query_block,
                                         bool no_order_by,
                                         bool implicit_grouping) {
  /*
    If the query block has a LIMIT clause but no ORDER BY clause, disable
    parallel query. Additionally, consider the following scenarios where
    restrictions are not required.
    1. The LIMIT clause generated during subquery transformation.
    2. The query block is implicitly grouped, resulting in only
    one row.
  */
  if (query_block->m_suite_for_pq &&
      !thd->variables.parallel_limit_no_order_by && query_block->has_limit() &&
      no_order_by && !query_block->m_internal_limit &&
      !(implicit_grouping || query_block->m_was_implicitly_grouped)) {
    query_block->m_suite_for_pq = false;
    query_block->pq_unsuite_info = PQUnsuiteInfo::LIMIT_NO_ORDERBY;
  }
}

void pq_save_join_group_list(THD *thd, Query_block *query_block,
                             ORDER *old_group_list, bool select_distinct,
                             Group_list_ptrs **join_group_list) {
  if (query_block->m_suite_for_pq && query_block->is_distinct() &&
      old_group_list && !select_distinct) {
    // The group_list.order belongs to the JOIN object, so we save it in
    // saved_join_group_list to differenciate between the group_list we save
    // from the JOIN and the one we save from the query_block, that is saved in
    // saved_group_list_ptrs
    SQL_I_List<ORDER> tmp_list;
    if (old_group_list->convert_to_list(tmp_list, thd) ||
        query_block->save_order_properties(thd, &tmp_list, join_group_list)) {
      query_block->m_suite_for_pq = false;
      query_block->pq_unsuite_info = PQUnsuiteInfo::INTER_ERROR;
    }
  }
}
