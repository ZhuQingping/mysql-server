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
#include "partial_result_cache.h"
#include <new>
#include "my_table_map.h"
#include "scope_guard.h"
#include "securec.h"
#include "sql/create_field.h"
#include "sql/error_handler.h"  // Ignore_error_handler
#include "sql/item_subselect.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/opt_statistics.h"
#include "sql/opt_trace.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_planner.h"
#include "sql/sql_tmp_table.h"
#include "tables_contained_in.h"

/*-------------------------------------------------------------------------
 *
 * partial_result_cache.cc
 *   Routines to handle caching of results from parameterized nodes
 *
 * ResultCache nodes are intended to sit above a parameterized node in the
 * plan tree in order to cache results from them.  The intention here is that
 * a repeat scan with a parameter which has already been seen by the node can
 * fetch tuples from the cache rather than having to re-scan the outer node
 * all over again.  The query planner may choose to make use of one of these
 * when it thinks rescans for previously seen values are likely enough to
 * warrant adding the additional node.
 *
 * The method of cache we use is a hash table.  When the cache fills, we never
 * spill records to disk, instead, we choose to evict the least recently used
 * cache entry from the cache.  We remember the least recently used entry by
 * always pushing new entries and entries we look for onto the tail of a
 * doubly linked list.  This means that older items always bubble to the top
 * of this LRU list.
 *
 * It's possible when we're filling the cache for a given set of parameters
 * that we're unable to free enough memory to store any more tuples.  If this
 * happens then we'll have already evicted all other cache entries.  When
 * caching another tuple would cause us to exceed our memory budget, we must
 * free the entry that we're currently populating and move the state machine
 * into BYPASS_MODE.  This means that we'll not attempt to cache any
 * further tuples for this particular scan.  We don't have the memory for it.
 * The state machine will be reset again on the next rescan.  If the memory
 * requirements to cache the next parameter's tuples are less demanding, then
 * that may allow us to start putting useful entries back into the cache
 * again.
 */
namespace ptrc {

void PathParameters::set_was_null(Item_in_subselect *sub) {
  was_null = &sub->was_null;
}

PathParameters::~PathParameters() {
  destroy(key_tables);
  destroy(res_tables);
  if (pseudo_context_table) {
    close_tmp_table(pseudo_context_table);
    free_tmp_table(pseudo_context_table);
  }
  if (res_tmp_table) {
    close_tmp_table(res_tmp_table);
    free_tmp_table(res_tmp_table);
  }
}

/// RAII class
class PtrcMemRoot {
 public:
  /// Replaces THD's current MEM_ROOT with PTRC's
  explicit PtrcMemRoot(THD *thd) : m_thd(thd) {
    // prevent nesting of two such objects (2nd would cancel 1st)
    assert(&m_thd->ptrc_objects->root == m_thd->ptrc_objects->saved_root);
    do_swap();
  }
  /// Restores the initial state
  ~PtrcMemRoot() { do_swap(); }
  /// Copying the object would create unwanted swap-s, disable:
  PtrcMemRoot(const PtrcMemRoot &other) = delete;
  PtrcMemRoot &operator=(const PtrcMemRoot &other) = delete;
  void do_swap() {
    std::swap(m_thd->mem_root, m_thd->ptrc_objects->saved_root);
  }

 private:
  THD *m_thd;
};

static bool fill_table_collection(const std::set<Field *> *cols,
                                  std::set<pack_rows::Table *> *tables) {
  THD *thd = current_thd;
  // In the list 'cols', two columns may belong to the same TABLE, but we
  // want only one Table for the two. So we use a map for tracking:
  std::map<TABLE *, pack_rows::Table *> table_map;
  for (auto &field : *cols) {
    TABLE *table = field->table;
    if (table->pos_in_table_list != nullptr) {
      /* It can happen that a subquery becomes unused (it will never be
      evaluated) but remains linked into the tree of Query_expressions-s (no
      call to clean_up_after_removal()) (for an example, you may read
      Item_func_isnull::resolve_type(), when the argument is not nullable).
      Therefore the subquery will be optimized, and PTRC will be set up for
      it. If the outer references in the subquery belong to tables which have
      been eliminated from a LEFT JOIN in the outer query, these tables have
      not gone through the optimization phase, have no usable statistics,
      cannot be used in PTRC. */
      JOIN_TAB *join_tab = table->reginfo.join_tab;
      QEP_TAB *qep_tab = table->reginfo.qep_tab;
      const QEP_shared_owner *tab =
          join_tab ? implicit_cast<QEP_shared_owner *>(join_tab)
                   : implicit_cast<QEP_shared_owner *>(qep_tab);
      if (tab == nullptr) return true;
    }
    auto it = table_map.find(table);
    pack_rows::Table *tab = nullptr;
    if (it == table_map.end()) {
      // Initialize an empty list of columns
      tab = new (thd->mem_root) pack_rows::Table(table, false);
      if (!tab) return true;
      tables->emplace(tab);
      table_map.emplace(table, tab);
    } else
      tab = it->second;

    tab->AddColumn(field);
  }

  return false;
}

/**
 * This function is used to construct key columns for PTRC.
 *
 * @param[in] thd session pointer
 * @param[in] sub_sel subquery Item or nullptr for nested loop join
 * @param[in] outer_tables bitmap for outer qep_tabs for current nested loop
 * join
 * @param[in] ptrc_join If for a nested loop join, the owning JOIN, nullptr
 * otherwise.
 * @param[in,out] tc collection of all key tables
 *
 * @returns TableCollection of key tables, nullptr if error
 */
static TableCollection *make_key_cols_tables(const THD *thd,
                                             Item_subselect *sub_sel,
                                             qep_tab_map outer_tables,
                                             const JOIN *ptrc_join,
                                             TableCollection *const tc) {
  // Don't work for both Subquery and NLJ.
  assert(!sub_sel || !ptrc_join);
  /**
   * If Nested loop join is located in one subquery, key fields should include
   * outer-ref fields and outer_tables' readable fields. outer-ref fields are
   * needed because we don't know whether these fields are referenced by such
   * a Nested loop join's inner tables' conditions.
   */
  Item_subselect *sub =
      sub_sel ? sub_sel
              : ptrc_join->query_block->master_query_expression()->item;
  // Collect left_expr
  std::set<pack_rows::Table *> tables;
  if (sub) {
    Dependent_item_params prm;
    prm.nest_level = sub->unit->first_query_block()->nest_level;
    prm.first_select = sub->unit->first_query_block();
    sub->get_item_params(thd, &prm);
    // Subquery must be a correlated one.
    if (sub_sel && prm.parameters.empty()) return nullptr;
    if (!prm.parameters.empty() &&
        fill_table_collection(&prm.parameters, &tables))
      return nullptr;
  }

  if (ptrc_join) {
    // All columns in read_set_internal of outer_tables, are cache keys. TODO:
    // sometimes this is overkill. For example, for:
    // SELECT t1.b FROM t1 JOIN t2 ON t1.a=t2.b;
    // only t1.a should be a cache key of the join, t1.b is not needed.
    for (QEP_TAB *tab : TablesContainedIn(ptrc_join, outer_tables)) {
      TABLE *table = tab->table();
      if (!table || (no_bytes_in_map(&table->read_set_internal) == 0) ||
          bitmap_is_clear_all(&table->read_set_internal))
        continue;
      assert(table);
      // In PTRC we do not need the non-NULL values of the NULL-complemented
      // row.
      tc->AddTable(table, false);
    }
  }

  for (auto &tab : tables) {
    tc->AddTable(tab);
    // TableCollection will create Table object by its own.
    destroy(tab);
  }

  if (tc->has_blob_column()) return nullptr;  // No support for BLOBs
  return tc;
}

static TABLE *make_context_pseudo_table(THD *thd) {
  List<Create_field> field_list;

  Create_field lru_node_field;
  lru_node_field.init_for_tmp_table(MYSQL_TYPE_LONGLONG, 8, 0, false, false, 8,
                                    "LRU_node_ptr");
  if (field_list.push_back(&lru_node_field)) return nullptr;
  Create_field exec_flags;
  exec_flags.init_for_tmp_table(MYSQL_TYPE_TINY, 1, 0, false, false, 1,
                                "Exec_flags");
  if (field_list.push_back(&exec_flags)) return nullptr;

  // This is a virtual table (no storage)
  TABLE *table;
  if (!(table = create_tmp_table_from_fields(thd, field_list))) return nullptr;
  bitmap_set_all(table->read_set);
  return table;
}

/**
 * This function is used to construct cached result columns for PTRC.
 *
 * @param[in] thd session pointer
 * @param[in] sl pointer of Query_block for nested loop join or subquery
 * @param[in] res_items array of Items cached in PTRC
 * @param[out] res_tmp_table point to the intermediate tmp table when
 * converting Items into Item_fields.
 * @param[out] context_table points to the special table (LRU and flags)
 * @param[in] inner_tables bitmap for inner qep_tabs for current nested loop
 * join
 * @param[in] ptrc_join join plan for the nested loop join
 * @param[in,out] tc collection of all key tables
 *
 * @retval TableCollection of result tables
 */
static TableCollection *make_res_cols_tables(
    THD *thd, Query_block *sl, const mem_root_deque<Item *> *res_items,
    TABLE **res_tmp_table, TABLE **context_table, qep_tab_map inner_tables,
    const JOIN *ptrc_join, TableCollection *const tc) {
  if (ptrc_join) {
    for (QEP_TAB *tab : TablesContainedIn(ptrc_join, inner_tables)) {
      TABLE *table = tab->table();
      // For ptrc, cache for ref access might be messed up because cached key
      // might not be updated if ptrc is hit.
      tab->ref().disable_cache = true;
      assert(table);
      // In PTRC we do not need the non-NULL values of the NULL-complemented
      // row.
      tc->AddTable(table, false);
    }
  } else {
    // PTRC for a subquery: prepare a temporary table which will serve for
    // storing to-be-cached values into Field objects then into the hash
    // structure, and in the reverse direction too.
    Temp_table_param tmp_table_param;

    // Only the visible part of the subquery's SELECT list has to be stored in
    // the cache, and thus in the tmp table.
    mem_root_deque<Item *> visible_fields(thd->mem_root);
    for (Item *item : VisibleFields(*res_items)) {
      if (visible_fields.push_back(item)) return nullptr;
    }
    tmp_table_param.skip_create_table = true;
    // SUM() etc are calculated before they get stored in the tmp table:
    tmp_table_param.precomputed_group_by = true;
    count_field_types(sl, &tmp_table_param, visible_fields, false, false);
    TABLE *table;
    if (!(table = create_tmp_table(
              thd, &tmp_table_param, visible_fields, /*group=*/nullptr, false,
              false, thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS,
              HA_POS_ERROR, "")))
      return nullptr;
    *res_tmp_table = table;
    std::set<Field *> res_fields;
    for (size_t i = 0; i < table->s->fields; i++) {
      res_fields.insert(table->field[i]);
    }
    std::set<pack_rows::Table *> tables;
    if (fill_table_collection(&res_fields, &tables)) return nullptr;
    for (auto &tab : tables) {
      tc->AddTable(tab);
      // TableCollection will create Table object by its own.
      destroy(tab);
    }
  }
  if (tc->has_blob_column()) return nullptr;
  *context_table = make_context_pseudo_table(thd);
  if (!*context_table) return nullptr;
  // In PTRC we do not need the non-NULL values of the NULL-complemented row.
  tc->AddTable(*context_table, false);
  return tc;
}

static rec_per_key_t get_record_per_key(const pack_rows::Table *table) {
  TABLE *tab = table->table;
  rec_per_key_t keys_per_group = 0;
  uint idx = UINT_MAX;
  uint max_part_no = 0;

  if (!tab) return 0;
  // No index at all
  if (tab->s->keys <= 0) return 0;

  // Use covering index directly
  if (tab->key_read) {
    for (idx = 0; idx < tab->s->keys; ++idx) {
      if (!tab->key_info[idx].supports_records_per_key()) continue;
      if (tab->covering_keys.is_set(idx)) break;
    }
    max_part_no = tab->key_info[idx].actual_key_parts - 1;
  } else {
    for (idx = 0; idx < tab->s->keys; ++idx) {
      KEY_PART_INFO *cur_part;
      KEY_PART_INFO *end_part;  // Last part for loops.
      bool found = true;
      const KEY *cur_index_info = &tab->key_info[idx];

      if (!cur_index_info->supports_records_per_key()) continue;

      max_part_no = 0;
      end_part = cur_index_info->key_part + cur_index_info->actual_key_parts;
      for (auto col : table->columns) {
        Field *field = col.field;
        cur_part = cur_index_info->key_part;
        for (uint i = 0; cur_part != end_part; i++, cur_part++) {
          if (!field->eq(cur_part->field)) continue;
          // Record the maximum key part index.
          if (i > max_part_no) max_part_no = i;
          break;
        }
        // Current index doesn't include required column
        if (cur_part == end_part) {
          found = false;
          break;
        }
      }
      if (found) break;
    }
  }

  // No index includes all of the columns, fail to use index per key
  // estimation.
  if (idx == UINT_MAX || idx >= tab->s->keys) return 0;

  KEY *key_info = &tab->key_info[idx];
  // Compute the number of keys in a group.
  if (key_info->has_records_per_key(max_part_no))
    // Use index statistics
    keys_per_group = key_info->records_per_key(max_part_no);
  else
    // If there is no statistics try to guess
    keys_per_group = guess_rec_per_key(tab, key_info, max_part_no + 1);

  return keys_per_group;
}

// Estimated rowcount after duplicate removal
static void estimate_num_groups(JOIN *src_join, const TableCollection *tables,
                                double *distinct_rows, double *est_rows,
                                bool is_sub) {
  double rows = 1.0;
  double dist_rows = 1.0;

  for (const pack_rows::Table &table : tables->tables()) {
    Table_ref *tl = table.table->pos_in_table_list;
    // Might be materialized table
    if (!tl) continue;
    JOIN_TAB *join_tab = table.table->reginfo.join_tab;
    QEP_TAB *qep_tab = table.table->reginfo.qep_tab;
    const QEP_shared_owner *tab =
        join_tab ? implicit_cast<QEP_shared_owner *>(join_tab)
                 : implicit_cast<QEP_shared_owner *>(qep_tab);
    assert(tab);

    // Const tables won't change anything
    if (tab && (tab->type() == JT_EQ_REF || tab->type() == JT_CONST ||
                tab->type() == JT_SYSTEM))
      continue;

    POSITION *const pos = tab->position();
    // Case:
    // SELECT c1 as c10
    // FROM t0 as t1
    // WHERE c1 NOT IN
    // (((
    //   SELECT c1 as c11
    //   FROM t0 as t2
    // ) IN (t1.c1 IS NULL)) IN (
    //   SELECT c1 as c12
    //   FROM t0 as t3
    // ));
    //
    // In this query, the IN predicate involving t3 has its left expression
    // involving t1.c1, thus t1.c1 is a PTRC cache key for this subquery. When
    // deciding whether to use PTRC optimization, it needs to access t1's
    // m_position. However, at this time, the outermost query block containing
    // t1 is still in the JOIN::estimate_rowcount processing phase, (this is one
    // of these cases where the subquery is evaluated by the outer query which
    // has not yet finished its own optimization) and m_position has not been
    // assigned a valid value, remaining as an initialized null pointer. The
    // m_position information will only be set in the subsequent
    // JOIN::get_best_combination phase. In such scenarios, we cannot estimate
    // whether using PTRC will be beneficial. Therefore, we set the threshold to
    // 0 to avoid using PTRC as much as possible.
    if (pos == nullptr) {
      *distinct_rows = 1.0;
      *est_rows = 1.0;
      return;
    }
    double rowcount = pos->rows_fetched * pos->filter_effect;
    double rec_per_key = get_record_per_key(&table);
    if (rec_per_key > 0)
      dist_rows *= (rowcount / rec_per_key);
    else
      dist_rows *= rowcount;
    rows *= rowcount;
  }
  *distinct_rows = dist_rows ? dist_rows : 1;

  // Here we didn't consider too much details on how to do a Nested loop join
  // etc.  whether including semijoin etc. Estimated rows might higher than
  // join->best_rowcount. For subquery, we still needs estimated rows of
  // parent query so that we can estimate how much the distinct values ratio
  // of key tables. Here we use join->best_rowcount to estimate whole parent
  // join output records.
  if (is_sub)
    *est_rows = std::max(rows, static_cast<double>(src_join->best_rowcount));
  else
    *est_rows = rows;
  if (*est_rows == 0) *est_rows = 1;
}

static void SetCostOnPtrcPath(THD *thd, Item_subselect *sub, JOIN *join,
                              AccessPath *path) {
  const Cost_model_server &cost_model = *thd->cost_model();
  // Cost for cache lookup once
  double cpu_tuple_cost = cost_model.row_evaluate_cost(1);
  // Cost for cache write
  double cpu_operator_cost = cost_model.tmptable_readwrite_cost(
      Cost_model_server::MEMORY_TMPTABLE, 1, 0);
  double total_cost;

  // For subquery, in order to make cost calculation simpler, we just add some
  // extra cost to write cache.
  if (sub) {
    total_cost =
        path->ptrc().child->cost +
        sub->unit->first_query_block()->join->best_rowcount * cpu_operator_cost;
    path->cost = total_cost;
    return;
  }
  // available cache space
  double hash_mem_bytes = thd->variables.partial_result_cache_max_mem_size;

  /*
   * Set the number of bytes each cache entry should consume in the cache.
   * To provide us with better estimations on how many cache entries we can
   * store at once, we make a call to the executor here to ask it what
   * memory overheads there are for a single cache entry.
   *
   * XXX we also store the cache key, but that's not accounted for here.
   */
  PathParameters *param = path->ptrc().param;
  double est_entry_bytes = ComputeRowSizeUpperBound(*param->key_tables);
  est_entry_bytes += ComputeRowSizeUpperBound(*param->res_tables);

  // estimate on the upper limit of cache entries we can hold at once
  double est_cache_entries = floor(hash_mem_bytes / est_entry_bytes);

  // estimate on the distinct number of parameter values
  double ndistinct, est_rows;
  estimate_num_groups(join, param->key_tables, &ndistinct, &est_rows, sub);

  /*
   * When the number of distinct parameter values is above the amount we can
   * store in the cache, then we'll have to evict some entries from the
   * cache.  This is not free. Here we estimate how often we'll incur the
   * cost of that eviction.
   */
  double evict_ratio = 1.0 - std::min(est_cache_entries, ndistinct) / ndistinct;

  /*
   * In order to estimate how costly a single scan will be, we need to
   * attempt to estimate what the cache hit ratio will be.  To do that we
   * must look at how many scans are estimated in total for this node and
   * how many of those scans we expect to get a cache hit.
   */
  double hit_ratio = 1.0 / ndistinct * std::min(est_cache_entries, ndistinct) -
                     (ndistinct / est_rows);

  // Ensure we don't go negative
  hit_ratio = std::max(hit_ratio, 0.0);

  /*
   * Set the total_cost accounting for the expected cache hit ratio. We also
   * add on a cpu_operator_cost to account for a cache lookup. This will happen
   * regardless of whether it's a cache hit or not.
   */
  total_cost = path->ptrc().child->cost * (1.0 - hit_ratio) + cpu_tuple_cost;

  // Now adjust the total cost to account for cache evictions
  // Charge a cpu_tuple_cost for evicting the actual cache entry
  total_cost += cpu_tuple_cost * evict_ratio;

  // The maximum output rows can't be bigger than best_rowcount
  double calls = std::min(est_rows, (double)join->best_rowcount);
  /*
   * Charge a 10th of cpu_operator_cost to evict every record in that entry.
   * The per-tuple eviction is really just a free, so charging a whole
   * cpu_operator_cost seems a little excessive.
   */
  total_cost += cpu_tuple_cost / 10.0 * evict_ratio * calls;

  /*
   * Now adjust for storing things in the cache, since that's not free either.
   * Everything must go in the cache. We don't proportion this over any ratio,
   * just apply it once for the scan. We charge a cpu_tuple_cost for the
   * creation of the cache entry and also a cpu_operator_cost for each tuple
   * we expect to cache.
   */
  total_cost += cpu_tuple_cost + cpu_operator_cost * (1.0 - hit_ratio) * calls;

  path->set_num_output_rows(path->ptrc().child->num_output_rows());
  path->cost = total_cost;
}

/**
  Estimate keys' distinct value ratio.

  @param[in] join Join object which current PTRC locates
  @param[in] tables Key tables
*/
static double estimate_distinct_val_ratio(JOIN *join,
                                          const TableCollection *tables,
                                          Opt_trace_object *trace_obj,
                                          bool is_sub) {
  double distinct_rows;
  double estimate_rows;
  estimate_num_groups(join, tables, &distinct_rows, &estimate_rows, is_sub);
  trace_obj->add("distinct_rows", distinct_rows);
  trace_obj->add("estimate_rows", estimate_rows);

  return (estimate_rows - distinct_rows) / estimate_rows;
}

static void process_ptrc_hints(Item_subselect *sub, JOIN *join,
                               qep_tab_map inner_tables, bool *force_to_use,
                               bool *force_not_use) {
  table_map force_tab_map = 0;
  table_map ignore_tab_map = 0;
  bool force_subquery_ptrc = false;
  bool ignore_subquery_ptrc = false;
  bool force_join_ptrc = false;
  bool ignore_join_ptrc = false;
  if (sub) {
    join = sub->unit->first_query_block()->join;
    inner_tables = TablesBetween(0, join->tables);
  }
  assert(join);
  if (join->query_block->opt_hints_qb)
    join->query_block->opt_hints_qb->apply_ptrc_hints(
        join, &force_tab_map, &ignore_tab_map, &force_subquery_ptrc,
        &ignore_subquery_ptrc, &force_join_ptrc, &ignore_join_ptrc);
  // Only if any of global hints for subquery, we need do this step. Morover,
  // global hints have no effect on nested loop join.
  if (sub) {
    *force_to_use = force_subquery_ptrc;
    *force_not_use = ignore_subquery_ptrc;
    return;
  } else {
    *force_to_use = force_join_ptrc;
    *force_not_use = ignore_join_ptrc;
  }

  if (*force_to_use || *force_not_use) return;

  // No force tables hint
  if (!force_tab_map && !ignore_tab_map) return;

  table_map inner_table_map = ConvertQepTabMapToTableMap(join, inner_tables);
  // Only all of the inner_tables are forced, PTRC will be enabled.
  if (!(inner_table_map & ~force_tab_map)) *force_to_use = true;
  // Any of the inner table is forced not to use PTRC, optimizer will quit to
  // use PTRC.
  if (ignore_tab_map & inner_table_map) *force_not_use = true;
}

static bool is_ptrc_suitable(THD *thd, Query_block *query_block,
                             AccessPath *inner_path, bool deterministic,
                             Opt_trace_object *ptrc_work) {
  if (thd->lex->sql_command != SQLCOM_SELECT) {
    ptrc_work->add("chosen", false);
    ptrc_work->add_utf8("cause", "Not SELECT query");
    return false;
  }

  if (!deterministic) {
    ptrc_work->add("chosen", false);
    ptrc_work->add_utf8("cause", "Has non-deterministic expression");
    return false;
  }

  // Any const plan or parent is const plan doesn't need ptrc.
  for (auto sl = query_block; sl; sl = sl->outer_query_block()) {
    // Note: For SP, outer_select() might have a nullptr JOIN pointer.
    if (!sl->join || sl->join->plan_is_const()) {
      ptrc_work->add("chosen", false);
      ptrc_work->add_utf8("cause", "Plan is const");
      return false;
    }
  }

  if (inner_path->type == AccessPath::ZERO_ROWS) {
    ptrc_work->add("chosen", false);
    ptrc_work->add_utf8("cause", "Zero rows path");
    return false;
  }

  Query_block *parent = query_block->outer_query_block();
  if (parent) {
    Item_subselect *sub = query_block->master_query_expression()->item;
    if (sub && sub->has_aggregation()) {
      // The subquery contains an aggregate which aggregates in the outer
      // query, like
      // SELECT t1.b, (SELECT SUM(t1.a) FROM t2) FROM t1 GROUP BY t1.b
      // If we do PTRC for the subquery's result, t1.a will be chosen as
      // cache key, which is incorrect (the subquery depends on more than
      // the current value of t1.a).
      ptrc_work->add("chosen", false);
      ptrc_work->add_utf8(
          "cause",
          "Subquery referencing outer-query aggregation is not supported");
      return false;
    }
    // HAVING condition is processed after GROUP BY operation. It means the
    // field value of key tables referenced by PTRC doesn't have old value
    // as cached key value.
    Item *having = parent->join->having_cond;
    if (having) {
      if (having->walk(&Item::find_item_processor, enum_walk::SUBQUERY_POSTFIX,
                       pointer_cast<uchar *>(sub))) {
        ptrc_work->add("chosen", false);
        ptrc_work->add_utf8("cause",
                            "Subquery in HAVING condition is not supported");
        return false;
      }
    }
    Item *pushed_having = parent->join->pushed_having_cond;
    if (pushed_having) {
      if (pushed_having->walk(&Item::find_item_processor,
                              enum_walk::SUBQUERY_POSTFIX,
                              pointer_cast<uchar *>(sub))) {
        ptrc_work->add("chosen", false);
        ptrc_work->add_utf8("cause",
                            "Subquery in HAVING condition is not supported");
        return false;
      }
    }
  }
  return true;
}

/**
  Helper function to write the current plan's prefix to the optimizer trace.
*/
static void trace_tables_info(JOIN *join, table_map tables,
                              Opt_trace_array *trace_array) {
  THD *const thd = join->thd;
  for (QEP_TAB *tab : TablesContainedIn(join, tables)) {
    Table_ref *const tr = tab->table_ref;
    if (!tr) continue;
    StringBuffer<32> str;
    tr->print(thd, &str,
              enum_query_type(QT_TO_SYSTEM_CHARSET | QT_SHOW_SELECT_NUMBER |
                              QT_NO_DEFAULT_DB | QT_DERIVED_TABLE_ONLY_ALIAS));
    trace_array->add_utf8(str.ptr(), str.length());
  }
}

static void reset_record(TableCollection *const tc) {
  for (auto &tbl : tc->tables()) {
    TABLE *table = tbl.table;
    // If table has null row, null_flags are already initialized, and columns'
    // values will not be read from their place in the record. Const table has
    // been filled with data, we don't need do such a reset any more.
    // In the PQ scenario, within the pq_dup_select function, all tables'
    // TABLE::const_table are uniformly set to false. Therefore,
    // TABLE::pq_saved_const_table should be used.
    if (!table->has_null_row() && !table->const_table &&
        !(table->in_use && table->in_use->pq_context().has_pq &&
          table->pq_saved_const_table)) {
      memset_s(table->record[0], table->s->reclength, 0, table->s->reclength);
    }
  }
}

/**
  Create PTRC path.

  @param[in] thd pointer of session
  @param[in] sub pointer of subquery, nullptr if a nested loop join
  @param[in] inner_path pointer of source path for partial result cache
  @param[in] ptrc_key_tables tablemap used to create key_tables
  @param[in] ptrc_res_tables tablemap used to create res_tables
  @param[in] ptrc_join join plan for nested loop join nesting, nullptr for
  subquery.

  @retval partial result cache path. inner_path if any error happens.
*/
AccessPath *CreateAccessPath(THD *thd, Item_subselect *sub,
                             AccessPath *inner_path,
                             qep_tab_map ptrc_key_tables,
                             qep_tab_map ptrc_res_tables, JOIN *ptrc_join) {
  bool force_to_use = false;
  bool force_not_use = false;

  assert(inner_path);

  Opt_trace_context *const trace = &thd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object ptrc_work(trace, "partial_result_cache");
  ptrc_work.add_utf8("caching_result_of",
                     sub ? "subquery" : "nested_loop_join");
  if (ptrc_join) {
    {
      Opt_trace_array plan_prefix(trace, "plan_prefix");
      trace_tables_info(ptrc_join, ptrc_key_tables, &plan_prefix);
    }
    {
      Opt_trace_array plan_suffix(trace, "right_tables");
      trace_tables_info(ptrc_join, ptrc_res_tables, &plan_suffix);
    }
  }

  process_ptrc_hints(sub, ptrc_join, ptrc_res_tables, &force_to_use,
                     &force_not_use);
  // Hints within a statement take precedence over optimizer_switch flags
  if ((!force_to_use && !is_ptrc_enabled(thd)) || force_not_use) {
    ptrc_work.add("chosen", false);
    ptrc_work.add_utf8("cause", "Disabled by optimizer_switch or hint");
    return inner_path;
  }

  bool deterministic = true;
  auto in_subs = dynamic_cast<Item_in_subselect *>(sub);
  if (ptrc_join && ptrc_join->where_cond &&
      (ptrc_join->where_cond->used_tables() & RAND_TABLE_BIT)) {
    // If caching for nested loop join, we don't want to cache if the nested
    // loop join's condition depends on RAND(). This test is a bit too
    // coarse-grained though.
    deterministic = false;
  } else if (sub) {
    // CreateAccessPath() (this function) is not called if the subquery's unit
    // is uncacheable. Alas, when IN-to-EXISTS injects a random expression
    // into the WHERE, unit->uncacheable is not updated. So we need this extra
    // test:
    if (in_subs && (in_subs->left_expr->used_tables() & RAND_TABLE_BIT))
      deterministic = false;
  }

  if (!is_ptrc_suitable(
          thd, sub ? sub->unit->first_query_block() : ptrc_join->query_block,
          inner_path, deterministic, &ptrc_work))
    return inner_path;  // not creating PTRC, let inner_path take our place

  if (!thd->ptrc_objects) {
    thd->ptrc_objects = new (std::nothrow) Memory_objects();
    if (!thd->ptrc_objects) return inner_path;
    auto root = &thd->ptrc_objects->root;
    init_sql_alloc(key_memory_ptrc_mem_root, root, 16 * 1024);
    root->set_max_capacity(thd->variables.partial_result_cache_max_mem_size);
  }

  // All PTRC-related allocations are done on this MEM_ROOT:
  PtrcMemRoot ptrc_mem_root(thd);

  TableCollection *key_tables = new (thd->mem_root) TableCollection();
  if (!key_tables) return inner_path;

  // For a while in this function, key_tables will be a local-only object:
  auto free_key_tables =
      create_scope_guard([key_tables] { destroy(key_tables); });

  if (!make_key_cols_tables(thd, sub, ptrc_key_tables, ptrc_join, key_tables))
    return inner_path;

  // No key tables are found.
  if (key_tables->tables().empty()) return inner_path;

  // Note: Should point to the outer query block's JOIN.
  JOIN *join = sub ? sub->unit->outer_query_block()->join : ptrc_join;
  assert(join);
  // Ignore cost comparison when force hint is used.
  if (!force_to_use) {
    auto ratio = estimate_distinct_val_ratio(join, key_tables, &ptrc_work,
                                             /*is_sub=*/sub != nullptr);
    ptrc_work.add("estimate_distinct_val_ratio", ratio);
    // Hit ratio is too lower to use ptrc.
    if (ratio < ptrc::get_min_cost_threshold(thd)) {
      ptrc_work.add("chosen", false);
      ptrc_work.add_utf8("cause", "cost below threshold");
      return inner_path;
    }
  }

  ptrc_work.add("chosen", true);
  if (force_to_use) ptrc_work.add_utf8("cause", "Hint forces to use");
  AccessPath *path = new (thd->mem_root) AccessPath;
  if (!path) return inner_path;
  path->type = AccessPath::PARTIAL_RESULT_CACHE;
  path->ptrc().child = inner_path;

  PathParameters *param = new (thd->mem_root) PathParameters();
  // AccessPath has a trivial destructor, no need to call it if failure
  if (!param) return inner_path;

  if (thd->ptrc_objects->params.push_back(param)) {
    destroy(param);
    return inner_path;
  }

  path->ptrc().param = param;
  param->key_tables = key_tables;

  // Now key_tables is registered in THD via 'params' above, so
  // ptrc::cleanup() will clean it up in all cases (error or success).
  free_key_tables.release();

  if (sub) {
    param->res_items = sub->unit->first_query_block()->join->fields;
    if (sub->substype() == Item_subselect::IN_SUBS ||
        sub->substype() == Item_subselect::ALL_SUBS ||
        sub->substype() == Item_subselect::ANY_SUBS ||
        sub->substype() == Item_subselect::EXISTS_SUBS) {
      if (((Item_exists_subselect *)sub)->strategy ==
          Subquery_strategy::SUBQ_EXISTS)
        param->is_exists_subquery = true;
    }
  }
  param->res_tables = new (thd->mem_root) TableCollection();
  if (!param->res_tables) return inner_path;
  if (!make_res_cols_tables(thd, sub ? sub->unit->first_query_block() : nullptr,
                            param->res_items, &param->res_tmp_table,
                            &param->pseudo_context_table, ptrc_res_tables,
                            ptrc_join, param->res_tables))
    return inner_path;
  if (in_subs) param->set_was_null(in_subs);
  SetCostOnPtrcPath(thd, sub, join, path);
  // PTRC (StoreFromTableBuffers) will use the whole record as a hash key to
  // do a comparison, including NULL bits of columns out of read_set. These
  // columns have their NULL bit un-initialized at this point, so we have to
  // set these. Moreover, if a result table has no row, we will still hash the
  // columns' values, so we need these values to not be garbage, or
  // Field::pack() could work abnormally.
  reset_record(param->key_tables);
  reset_record(param->res_tables);
  return path;
}

static bool reserve_space(String *buffer, TableCollection *tables) {
  DBUG_EXECUTE_IF("ptrc_initrowlbuffer_reserver_space_fail", {
    my_error(ER_OUTOFMEMORY, MYF(0), 20210404);
    return true;
  });
  if (!tables->has_blob_column()) {
    size_t upper_row_size = ComputeRowSizeUpperBound(*tables);
    if (buffer->reserve(upper_row_size)) {
      my_error(ER_OUTOFMEMORY, MYF(0), upper_row_size);
      return true;
    }
  }
  return false;
}

bool PtrcIterator::InitRowBuffer() {
  m_row_buffer = new (m_thd->mem_root) HashJoinRowBuffer(
      m_thd->variables.partial_result_cache_max_mem_size, m_thd->mem_root);
  DBUG_EXECUTE_IF("ptrc_init_row_buffer_fail", return true;);

  if (m_row_buffer == nullptr) return true;

  // When memory of mem_root is insufficient, m_row_buffer->Init would crash.
  // Setting m_error_for_capacity_exceeded can make allocation success by
  // allocating memory out of current mem_root when it is full.
  m_thd->mem_root->set_error_for_capacity_exceeded(true);

  // When something failed with ptrc, query should continue to execute
  // without ptrc. So we ignore the error from reserve_space and
  // m_row_buffer->Init.
  Dummy_error_handler error_handler;
  m_thd->push_internal_handler(&error_handler);
  bool res = true;

  if (m_row_buffer->Init(true) || error_handler.is_error_handled()) {
    goto final;
  }
  m_current_row = LinkedImmutableString{nullptr};

  if (reserve_space(&key_buff, m_key_tables) ||
      (reserve_space(&res_buff, m_res_tables))) {
    goto final;
  }
  res = false;
final:
  m_thd->pop_internal_handler();
  m_thd->mem_root->set_error_for_capacity_exceeded(false);
  if (res) {
    Opt_trace_context *const trace = &m_thd->opt_trace;
    Opt_trace_object trace_wrapper(trace);
    Opt_trace_object ptrc_work(trace, "partial_result_cache");
    ptrc_work.add("disabled", true);
    ptrc_work.add_utf8("cause",
                       "InitRowBuffer failed because of ER_OUTOFMEMORY");
  }
  return res;
}

PtrcIterator::PtrcIterator(THD *thd, TableCollection *key_tables,
                           TableCollection *res_tables,
                           unique_ptr_destroy_only<RowIterator> source,
                           mem_root_deque<Item *> *res_items,
                           TABLE *res_tmp_table, bool *was_null,
                           bool need_fill_res_tables)
    : RowIterator(thd),
      m_key_tables(std::move(key_tables)),
      m_res_tables(std::move(res_tables)),
      m_thd(thd),
      m_source_iterator(std::move(source)),
      m_res_items(res_items),
      m_res_tmp_table(res_tmp_table),
      m_was_null(was_null),
      m_need_fill_res_tables(need_fill_res_tables) {}

PtrcIterator::~PtrcIterator() {
  destroy(m_row_buffer);
  LRU_node *node = LRU_list.head;
  while (node) {
    LRU_list.head = node->next;
    destroy(node);
    node = LRU_list.head;
  }
  destroy(ptrc_context);
  if (m_res_tmp_table) {
    RestoreCacheItems();
  }
  delete saved_res_items;
  delete cached_res_fields;
}

bool PtrcIterator::Init() {
  if (status == Ptrc_status::BYPASS_MODE) return m_source_iterator->Init();

  bool res = true;
  if (!m_row_buffer || !m_row_buffer->Initialized()) {
    PrepareForRequestRowId(m_res_tables->tables(),
                           m_res_tables->tables_to_get_rowid_for());
    PtrcMemRoot ptrc_mem_root(m_thd);
    if (!ptrc_context) {
      ptrc_context = new (m_thd->mem_root) PTRC_context();
      if (!ptrc_context || !ptrc_context->is_valid()) {
        goto final;
      }
    }
    pseudo_context_table = m_res_tables->tables().back().table;
    if (InitRowBuffer()) {
      goto final;
    }
    if (m_res_tmp_table) {
      saved_res_items = new mem_root_deque<Item *>(m_thd->mem_root);
      DBUG_EXECUTE_IF("fail_alloc_saved_res_items", {
        delete saved_res_items;
        saved_res_items = nullptr;
      });
      if (!saved_res_items) {
        goto final;
      }
      cached_res_fields = new mem_root_deque<Item *>(m_thd->mem_root);
      DBUG_EXECUTE_IF("fail_alloc_cached_res_fields", {
        delete cached_res_fields;
        cached_res_fields = nullptr;
      });
      if (!cached_res_fields) {
        goto final;
      }

      num_hidden_fields = CountHiddenFields(*m_res_items);
      for (size_t i = 0; i < m_res_tmp_table->s->fields; i++) {
        Item_field *field = new Item_field(m_res_tmp_table->field[i]);
        if (!field) {
          goto final;
        }
        if (cached_res_fields->push_back(field) ||
            saved_res_items->push_back((*m_res_items)[i + num_hidden_fields]))
          goto final;
      }
    }
  }
  change_status(Ptrc_status::LOOKUP);
  res = false;
final:
  if (res) change_status(Ptrc_status::BYPASS_MODE);
  return m_source_iterator->Init();
}

bool PtrcIterator::FillTmpTable() {
  for (size_t i = 0; i < m_res_tmp_table->s->fields; i++) {
    type_conversion_status st =
        (*m_res_items)[i + num_hidden_fields]->save_in_field(
            m_res_tmp_table->field[i], false);
    if (st != TYPE_OK && m_res_tmp_table->in_use->is_error()) return true;
  }
  return false;
}

/**
  Makes room in the hash buffer, by deleting one or more matches. the choice
  is based on LRU.
  @param thd Thread handler
  @param key  Key we want to insert
  @param res_size space we need for its matching record (so any free place
  returned by this function must be of this size at least)
  @returns Free place in the hash buffer, to be written to. nullptr if none.
*/
Key_pair *PtrcIterator::LRU_free_mem(THD *thd, const Key &key,
                                     size_t res_size) {
  Key_pair *res = nullptr;
  std::list<Key_pair *> &free_list = LRU_list.free_list;
  size_t required_key_bytes =
      ImmutableStringWithLength::RequiredBytesForEncode(key.size());
  size_t required_row_bytes =
      LinkedImmutableString::RequiredBytesForEncode(res_size);
  ImmutableStringWithLength need_delete_node{nullptr};
  size_t expected_key_size, expected_value_size;
  // Try to get from free_list
  if (!(free_list.empty())) {
    for (std::list<Key_pair *>::iterator it = free_list.begin();
         it != free_list.end(); it++) {
      if ((*it)->first.second >= required_key_bytes &&
          (*it)->second.second >= required_row_bytes) {
        // This free element has enough room for what we need to store, use it
        Key_pair *tmp = *it;
        it = free_list.erase(it);
        return tmp;
      }
    }
  }

  LRU_node *tail, *prev_node;
  // Record the first start LRU_node with a new key
  LRU_node *start_batch;
  // Empty or only one record (which is the record just inserted by Read() and
  // thus must be kept)
  if (!LRU_list.head || LRU_list.tail == LRU_list.head) return nullptr;

  // We will walk from the LRU's tail (oldest records), finding a record to
  // delete. It cannot have the same key as the one we want to insert, and
  // when we remove this record we must also remove all records having its
  // key. This is because we never want to be in a situation where only some
  // subset of the matches of a key are cached, it would make the lookup code
  // return incomplete results.
  // start_batch: the oldest record to remove.
  // tail: the newest record to remove.
  // prev_node: the one before 'tail'.
  start_batch = tail = LRU_list.tail;
  prev_node = tail->prev;
  do {
    if (tail == LRU_list.head) break;  // single record, must be kept
    // Don't remove the ones which have the key we want to insert
    if (tail->key_data.Decode() == key) {
      tail = prev_node;
      prev_node = prev_node->prev;
      continue;
    }
    if (!(tail->key_data == start_batch->key_data)) {
      // we skipped forbidden records, adjust start_batch
      start_batch = tail;
    }
    // At this stage, start_batch==tail.
    if (tail->key_size >= required_key_bytes &&
        tail->row_size >= required_row_bytes) {
      // Enough room at this place
      // this is where the new record will go
      expected_key_size = tail->key_size;
      expected_value_size = tail->row_size;
      prev_node = start_batch->prev;
      tail = start_batch;
      // Saving a copy of start_batch->key_data for comparison in the loop,
      // because *start_batch may become unreadable due to destroy(tail)
      need_delete_node = start_batch->key_data;
      // Must cleanup all the records with the same key. start_batch doesn't
      // move, 'tail' moves towards newest.
      while (tail->key_data == need_delete_node) {
        std::pair<const char *, size_t> *key_pair_key =
            new (thd->mem_root) std::pair<const char *, size_t>(
                tail->key_data.GetDataPointer(), tail->key_size);
        std::pair<const char *, size_t> *key_pair_value =
            new (thd->mem_root) std::pair<const char *, size_t>(
                tail->row_data.GetDataPointer(), tail->row_size);
        DBUG_EXECUTE_IF("key_pair_key_null_pointer_injection",
                        key_pair_key = nullptr;);
        if (!key_pair_key || !key_pair_value) return nullptr;
        Key_pair *key_pair =
            new (thd->mem_root) Key_pair(*key_pair_key, *key_pair_value);
        if (!key_pair) return nullptr;
        if ((tail->key_size == expected_key_size) &&
            (tail->row_size == expected_value_size) && res == nullptr) {
          // this place will be used immediately, will be returned to caller
          // in 'res'
          res = key_pair;
        } else {
          // this place won't be used immediately, put it in free_list for later
          free_list.emplace_back(key_pair);
        }
        // One new LRU node has been inserted before StoreRow, so prev_node
        // should exist
        assert(prev_node);
        prev_node->next = tail->next;
        if (tail->next)
          tail->next->prev = prev_node;
        else
          // Reset the list tail
          LRU_list.tail = prev_node;

        // Free such a node
        destroy(tail);

        tail = prev_node;
        prev_node = prev_node->prev;
        if (tail == LRU_list.head) break;
        stats.cache_evictions++;
      }
      // Found a matched one
      break;
    }
    tail = prev_node;
    prev_node = prev_node->prev;
  } while (true);

  // Remove it from result cache
  if (res && need_delete_node.Decode().data() != nullptr) {
    m_row_buffer->erase(need_delete_node.Decode());
  }
  return res;
}
static bool read_keys_value(String *key_buff, TableCollection *tables) {
  return (StoreFromTableBuffers(*tables, key_buff));
}

static bool read_res_value(String *res_buff, TableCollection *tables) {
  return (StoreFromTableBuffers(*tables, res_buff));
}

void PtrcIterator::ChangeToUseTmpField() {
  std::copy(cached_res_fields->begin(), cached_res_fields->end(),
            m_res_items->begin() + num_hidden_fields);
}

void PtrcIterator::RestoreCacheItems() {
  // If fail to allocate saved_res_items, saved_res_items might be nullptr.
  if (saved_res_items)
    std::copy(saved_res_items->begin(), saved_res_items->end(),
              m_res_items->begin() + num_hidden_fields);
}

/// Insert a newly cached element at the head of the LRU
bool PtrcIterator::insert_into_LRU_list() {
  PTRC_context *context = ptrc_context;
  LRU_node *node = new (m_thd->mem_root) LRU_node;
  if (!node) return true;
  // Insert at the head of LRU_list
  LRU_list_type *LRU = &LRU_list;
  if (!LRU->head)
    LRU->head = LRU->tail = node;
  else {
    // Insert at the head
    node->next = LRU->head;
    LRU->head->prev = node;
    LRU->head = node;
  }
  current_LRU_node = node;
  ulonglong addr = reinterpret_cast<ulonglong>(node);
  static_assert(sizeof(addr) == sizeof(context->item_addr->value) &&
                    sizeof(addr) >= sizeof(node),
                "cannot store pointer into BIGINT");
  context->item_addr->value = addr;
  context->item_addr->save_in_field(pseudo_context_table->field[0], false);
  return false;
}

/// Move the just-matched element to the head of the LRU
void PtrcIterator::update_LRU_list() {
  assert(LRU_list.head);
  LRU_node *node =
      reinterpret_cast<LRU_node *>(pseudo_context_table->field[0]->val_int());
  // Not the first one
  if (LRU_list.head != node) {
    if (node == LRU_list.tail) LRU_list.tail = node->prev;
    node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    node->next = LRU_list.head;
    node->prev = nullptr;
    LRU_list.head->prev = node;
    LRU_list.head = node;
  }
}

static const auto NO_MATCHED_ROW = 1ULL;
static const auto WAS_NULL = 1ULL << 1;

/**
 * State machine if no cache: LOOKUP -> FILLING_CACHE -> END_OF_SCAN
 *
 * State machine if has cache: LOOKUP -> FETCH_NEXT_TUPLE -> FETCH_NEXT_TUPLE
 * -> END_OF_SCAN
 */
int PtrcIterator::Read() {
  // TODO: Only invalidate records for current key
  if (status == Ptrc_status::BYPASS_MODE) {
    int error = m_source_iterator->Read();
    // In FindTablesToGetRowidFor() we promised to parent iterators that we
    // will handle rowids. So even if we are now in disabled state we must
    // fill them before returning.
    // Positon function in RequestRowId for Temptable might fail if error
    // occurs and m_index_cursor is not initialized.
    if (error == 0 && m_res_tables->store_rowids())
      RequestRowId(m_res_tables->tables(),
                   m_res_tables->tables_to_get_rowid_for());
    return error;
  }

  assert(m_row_buffer->Initialized());
  PtrcMemRoot ptrc_mem_root(m_thd);
  int res = 0;

  if (status == Ptrc_status::LOOKUP || status == Ptrc_status::END_OF_SCAN) {
    // Try to check the cache and see whether keys are matched.
    if (read_keys_value(&key_buff, m_key_tables)) {
      goto err;
    }
    change_status(Ptrc_status::LOOKUP);
    // If matched, load cached result
    m_current_row = m_row_buffer
                        ->find(Key(pointer_cast<const char *>(key_buff.ptr()),
                                   key_buff.length()))
                        .value_or(LinkedImmutableString{nullptr});
    if (m_current_row != nullptr) {
      stats.cache_hits++;
      LoadIntoTableBuffers(
          *m_res_tables,
          pointer_cast<const uchar *>(m_current_row.DecodeFixed().data));
      update_LRU_list();
      if (m_res_tmp_table) ChangeToUseTmpField();
      auto field1value = pseudo_context_table->field[1]->val_int();
      if (m_was_null) *m_was_null = field1value & WAS_NULL;
      if (field1value & NO_MATCHED_ROW) {
        if (m_res_tmp_table) RestoreCacheItems();
        change_status(Ptrc_status::END_OF_SCAN);
        return -1;  // End of current scan
      }
      change_status(Ptrc_status::FETCH_NEXT_TUPLE);
      m_current_row = m_current_row.DecodeFixed().next;
      return res;
    }
    change_status(Ptrc_status::FILLING_CACHE);
  } else if (status == Ptrc_status::FETCH_NEXT_TUPLE) {
    // No need to update *m_was_null, it should be the same as previous tuple
    if (m_current_row == nullptr) {
      if (m_res_tmp_table) RestoreCacheItems();
      change_status(Ptrc_status::END_OF_SCAN);
      return -1;  // End of current scan
    }
    LoadIntoTableBuffers(*m_res_tables, pointer_cast<const uchar *>(
                                            m_current_row.DecodeFixed().data));
    update_LRU_list();
    m_current_row = m_current_row.DecodeFixed().next;
    stats.cache_hits++;
    return res;
  }

  assert(status != Ptrc_status::END_OF_SCAN);

  if (status == Ptrc_status::FILLING_CACHE) {
    // If not, inner_path->Read(). Cache the result and return; If error
    // happens, set mode to skip.()
    // Allocations done by the source iterator shouldn't use PTRC's MEM_ROOT
    ptrc_mem_root.do_swap();
    res = m_source_iterator->Read();
    ptrc_mem_root.do_swap();

    if (res != 0) change_status(Ptrc_status::END_OF_SCAN);
    // Some error happens, return directly.
    if (res > 0) return res;
    // If we hit EOF, but this scan had previously found rows, we can return.
    // To check this, we compare the current key with the previous key. If
    // they're the same, the scan had started before this fetch. If they're
    // not the same, the scan just started, so it has found no rows, so we
    // will cache this information.
    if (res < 0) {
      if (!m_row_buffer->LastKeyStored().IsEmpty() &&
          key_buff.length() == m_row_buffer->LastKeyLengthStored() &&
          !memcmp(key_buff.ptr(), m_row_buffer->LastKeyStored().Decode().data(),
                  m_row_buffer->LastKeyLengthStored()))
        return res;
    }

    // Take care whether need call position to copy rowid if one row is
    // returned successfully.
    if (res == 0 && m_res_tables->store_rowids())
      RequestRowId(m_res_tables->tables(),
                   m_res_tables->tables_to_get_rowid_for());

    if (!((++stats.cache_misses) %
          m_thd->variables.partial_result_cache_check_hit_ratio_frequency) &&
        ((double)stats.cache_hits /
         ((double)stats.cache_hits + stats.cache_misses)) <
            m_thd->variables.partial_result_cache_min_hit_ratio) {
      if (!IsDisabled()) {
        DisablePtrc();
        Opt_trace_context *const trace = &m_thd->opt_trace;
        Opt_trace_object trace_wrapper(trace);
        Opt_trace_object ptrc_work(trace, "partial_result_cache");
        ptrc_work.add("disabled", true);
        ptrc_work.add_utf8("cause", "hit count is too low");
        if (m_res_tmp_table) RestoreCacheItems();
        return res;
      }
    }

    if (insert_into_LRU_list()) goto err;
    ptrc_context->exec_flags->value = 0;
    if (!res) {
      if (m_res_tmp_table && m_need_fill_res_tables) {
        // During save items in temp table field, mem_root should be changed
        // because items might be Item_subselect.
        ptrc_mem_root.do_swap();
        bool loc_res = FillTmpTable();
        ptrc_mem_root.do_swap();
        if (loc_res) goto err;
      }
    } else {
      ptrc_context->exec_flags->value |= NO_MATCHED_ROW;
    }

    if (m_was_null && *m_was_null) ptrc_context->exec_flags->value |= WAS_NULL;

    ptrc_context->exec_flags->save_in_field(pseudo_context_table->field[1],
                                            false);
    if (read_res_value(&res_buff, m_res_tables)) goto err;
    StoreRowResult store_row_result =
        m_row_buffer->StoreRow(key_buff.ptr(), key_buff.length(),
                               /*key_store_pos=*/nullptr, res_buff.ptr(),
                               res_buff.length(), /*val_store_pos=*/nullptr);
    stats.mem_used = m_thd->mem_root->allocated_size();
    if ((store_row_result == StoreRowResult::BUFFER_FULL) ||
        (store_row_result == StoreRowResult::FATAL_ERROR)) {
      // No space in hash buffers. We have to remove old records from it. We
      // use the LRU to decide which are old records.
      Key_pair *free_node = nullptr;
      if (likely(!(free_node = LRU_free_mem(
                       m_thd,
                       Key(pointer_cast<const char *>(key_buff.ptr()),
                           key_buff.length()),
                       res_buff.length())))) {
        // Still no space. Have to stop using the cache.
        // Undo the insertion made in the LRU (bad node is at head).
        if (LRU_list.head == LRU_list.tail)
          LRU_list.head = LRU_list.tail = nullptr;
        else {
          LRU_list.head = LRU_list.head->next;
          current_LRU_node->next->prev = nullptr;
        }
        destroy(current_LRU_node);
        stats.cache_overflows += 1;
        goto err;
      }
      // There is space.
      store_row_result = m_row_buffer->StoreRow(
          key_buff.ptr(), key_buff.length(),
          const_cast<char *>(free_node->first.first), res_buff.ptr(),
          res_buff.length(), const_cast<char *>(free_node->second.first));
      assert(store_row_result != StoreRowResult::FATAL_ERROR);
      if (store_row_result == StoreRowResult::BUFFER_FULL) goto err;
    }

    // Update the stored information for LRU node.
    current_LRU_node->set_data(
        (m_row_buffer->LastKeyStored().GetDataPointer()),
        ImmutableStringWithLength::RequiredBytesForEncode(
            m_row_buffer->LastKeyLengthStored()),
        m_row_buffer->LastRowStored().GetDataPointer(),
        LinkedImmutableString::RequiredBytesForEncode(
            m_row_buffer->LastRowLengthStored()));
  }

  return res;

err:
  stats.cache_misses++;
  if (m_res_tmp_table) RestoreCacheItems();
  change_status(Ptrc_status::BYPASS_MODE);
  return res;
}

void print(const AccessPath *path, String *str, THD *thd) {
  assert(path->type == AccessPath::PARTIAL_RESULT_CACHE);
  str->length(0);
  str->append("Result cache : cache keys(");
  const PathParameters *param = path->ptrc().param;
  param->key_tables->print(*str);
  str->append(")");
  if (!thd->lex->is_explain_analyze) return;

  Instrumentation stats;
  if (path->iterator) {
    PtrcIterator *iterator =
        down_cast<PtrcIterator *>(path->iterator->real_iterator());
    assert(iterator);
    stats = iterator->stats;
  }
  str->append(" (");
  str->append("Cache Hits: ");
  str->append_ulonglong(stats.cache_hits);
  str->append(", Cache Misses: ");
  str->append_ulonglong(stats.cache_misses);
  str->append(", Cache Evictions: ");
  str->append_ulonglong(stats.cache_evictions);
  str->append(", Cache Overflows: ");
  str->append_ulonglong(stats.cache_overflows);
  str->append(", Memory Usage: ");
  str->append_ulonglong(stats.mem_used);
  str->append(" )");
}

/// Clean partial result cache resource.
void cleanup(THD *thd) {
  if (thd->ptrc_objects) {
    assert(&thd->ptrc_objects->root == thd->ptrc_objects->saved_root);
    delete thd->ptrc_objects;
    thd->ptrc_objects = nullptr;
  }
}
}  // end of namespace ptrc
