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

#include "sql/partial_result_cache.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>

#include "field_types.h"
#include "my_alloc.h"
#include "my_base.h"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/create_field.h"
#include "sql/current_thd.h"
#include "sql/error_handler.h"
#include "sql/item.h"
#include "sql/item_subselect.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/opt_costmodel.h"
#include "sql/opt_hints.h"
#include "sql/opt_statistics.h"
#include "sql/opt_trace.h"
#include "sql/psi_memory_key.h"
#include "sql/query_options.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_planner.h"
#include "sql/sql_select.h"
#include "sql/sql_tmp_table.h"
#include "sql/table.h"
#include "sql/temp_table_param.h"
#include "sql/thr_malloc.h"
#include "sql/visible_fields.h"
#include "tables_contained_in.h"

namespace ptrc {

struct PTRC_context {
  PTRC_context() : item_addr(new Item_uint(0)), exec_flags(new Item_uint(0)) {}
  bool is_valid() const { return item_addr != nullptr && exec_flags != nullptr; }

  Item_uint *item_addr{nullptr};
  Item_uint *exec_flags{nullptr};
};

void PathParameters::set_was_null(Item_in_subselect *sub) {
  was_null = &sub->was_null;
}

PathParameters::~PathParameters() {
  destroy(key_tables);
  destroy(res_tables);
  if (pseudo_context_table != nullptr) {
    close_tmp_table(pseudo_context_table);
    free_tmp_table(pseudo_context_table);
  }
  if (res_tmp_table != nullptr) {
    close_tmp_table(res_tmp_table);
    free_tmp_table(res_tmp_table);
  }
}

class PtrcMemRoot {
 public:
  explicit PtrcMemRoot(THD *thd) : m_thd(thd) {
    assert(&m_thd->ptrc_objects->root == m_thd->ptrc_objects->saved_root);
    do_swap();
  }

  ~PtrcMemRoot() { do_swap(); }

  PtrcMemRoot(const PtrcMemRoot &) = delete;
  PtrcMemRoot &operator=(const PtrcMemRoot &) = delete;

  void do_swap() { std::swap(m_thd->mem_root, m_thd->ptrc_objects->saved_root); }

 private:
  THD *m_thd;
};

Memory_objects::Memory_objects() : root(), saved_root(&root), params(&root) {}

Memory_objects::~Memory_objects() {
  for (auto *param : params) destroy(param);
}

static table_map ConvertQepTabMapToTableMap(JOIN *join, qep_tab_map tables) {
  table_map map = 0;
  for (QEP_TAB *tab : TablesContainedIn(join, tables)) {
    if (tab->table_ref != nullptr) map |= tab->table_ref->map();
  }
  return map;
}

static bool fill_table_collection(const std::set<Field *> *cols,
                                  std::set<pack_rows::Table *> *tables) {
  THD *thd = current_thd;
  std::map<TABLE *, pack_rows::Table *> table_map;
  for (Field *field : *cols) {
    TABLE *table = field->table;
    if (table->pos_in_table_list != nullptr) {
      /*
        Unused subqueries may still be optimized. If an outer reference belongs
        to a table that was eliminated before optimization, PTRC cannot use it
        as a stable cache key.
      */
      JOIN_TAB *join_tab = table->reginfo.join_tab;
      QEP_TAB *qep_tab = table->reginfo.qep_tab;
      const QEP_shared_owner *tab =
          join_tab != nullptr ? implicit_cast<QEP_shared_owner *>(join_tab)
                              : implicit_cast<QEP_shared_owner *>(qep_tab);
      if (tab == nullptr) return true;
    }

    pack_rows::Table *packed_table = nullptr;
    auto it = table_map.find(table);
    if (it == table_map.end()) {
      packed_table = new (thd->mem_root) pack_rows::Table(
          table, /*store_contents_of_null_rows=*/false);
      if (packed_table == nullptr) return true;
      tables->emplace(packed_table);
      table_map.emplace(table, packed_table);
    } else {
      packed_table = it->second;
    }
    packed_table->AddColumn(field);
  }
  return false;
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

  TABLE *table = create_tmp_table_from_fields(thd, field_list);
  if (table == nullptr) return nullptr;
  bitmap_set_all(table->read_set);
  return table;
}

static TableCollection *make_key_cols_tables(const THD *thd,
                                             Item_subselect *sub_sel,
                                             qep_tab_map outer_tables,
                                             JOIN *ptrc_join,
                                             TableCollection *tc) {
  assert(sub_sel == nullptr || ptrc_join == nullptr);

  Item_subselect *sub =
      sub_sel != nullptr
          ? sub_sel
          : ptrc_join->query_block->master_query_expression()->item;
  std::set<pack_rows::Table *> tables;
  if (sub != nullptr) {
    Dependent_item_params params;
    params.nest_level = sub->unit->first_query_block()->nest_level;
    params.first_select = sub->unit->first_query_block();
    sub->get_item_params(thd, &params);
    if (sub_sel != nullptr && params.parameters.empty()) return nullptr;
    if (!params.parameters.empty() &&
        fill_table_collection(&params.parameters, &tables)) {
      return nullptr;
    }
  }

  if (ptrc_join != nullptr) {
    for (QEP_TAB *tab : TablesContainedIn(ptrc_join, outer_tables)) {
      TABLE *table = tab->table();
      if (table == nullptr ||
          bitmap_is_clear_all(&table->read_set_internal)) {
        continue;
      }
      tc->AddTable(table, /*store_contents_of_null_rows=*/false);
    }
  }

  for (pack_rows::Table *table : tables) {
    tc->AddTable(table);
    destroy(table);
  }

  if (tc->has_blob_column()) return nullptr;
  return tc;
}

static TableCollection *make_res_cols_tables(THD *thd,
                                             Query_block *query_block,
                                             const mem_root_deque<Item *> *res_items,
                                             TABLE **res_tmp_table,
                                             TABLE **context_table,
                                             qep_tab_map inner_tables,
                                             const JOIN *ptrc_join,
                                             TableCollection *tc) {
  if (ptrc_join != nullptr) {
    for (QEP_TAB *tab : TablesContainedIn(ptrc_join, inner_tables)) {
      TABLE *table = tab->table();
      if (table == nullptr) return nullptr;
      tab->ref().disable_cache = true;
      tc->AddTable(table, /*store_contents_of_null_rows=*/false);
    }
  } else {
    Temp_table_param tmp_table_param;
    mem_root_deque<Item *> visible_fields(thd->mem_root);
    for (Item *item : VisibleFields(*res_items)) {
      if (visible_fields.push_back(item)) return nullptr;
    }
    tmp_table_param.skip_create_table = true;
    tmp_table_param.precomputed_group_by = true;
    count_field_types(query_block, &tmp_table_param, visible_fields,
                      /*reset_with_sum_func=*/false,
                      /*save_sum_fields=*/false);

    TABLE *table = create_tmp_table(
        thd, &tmp_table_param, visible_fields, /*group=*/nullptr,
        /*distinct=*/false, /*save_sum_fields=*/false,
        thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS, HA_POS_ERROR, "");
    if (table == nullptr) return nullptr;
    *res_tmp_table = table;

    std::set<Field *> res_fields;
    for (uint i = 0; i < table->s->fields; i++) res_fields.insert(table->field[i]);
    std::set<pack_rows::Table *> tables;
    if (fill_table_collection(&res_fields, &tables)) return nullptr;
    for (pack_rows::Table *packed_table : tables) {
      tc->AddTable(packed_table);
      destroy(packed_table);
    }
  }

  if (tc->has_blob_column()) return nullptr;
  *context_table = make_context_pseudo_table(thd);
  if (*context_table == nullptr) return nullptr;
  tc->AddTable(*context_table, /*store_contents_of_null_rows=*/false);
  return tc;
}

static rec_per_key_t get_record_per_key(const pack_rows::Table *table) {
  TABLE *tab = table->table;
  rec_per_key_t keys_per_group = 0;
  uint idx = UINT_MAX;
  uint max_part_no = 0;

  if (tab == nullptr || tab->s->keys == 0) return 0;

  if (tab->key_read) {
    for (idx = 0; idx < tab->s->keys; ++idx) {
      if (!tab->key_info[idx].supports_records_per_key()) continue;
      if (tab->covering_keys.is_set(idx)) break;
    }
    if (idx >= tab->s->keys) return 0;
    max_part_no = tab->key_info[idx].actual_key_parts - 1;
  } else {
    for (idx = 0; idx < tab->s->keys; ++idx) {
      const KEY *cur_index_info = &tab->key_info[idx];
      if (!cur_index_info->supports_records_per_key()) continue;

      bool found = true;
      max_part_no = 0;
      KEY_PART_INFO *end_part =
          cur_index_info->key_part + cur_index_info->actual_key_parts;
      for (const pack_rows::Column &col : table->columns) {
        Field *field = col.field;
        KEY_PART_INFO *cur_part = cur_index_info->key_part;
        for (uint i = 0; cur_part != end_part; i++, cur_part++) {
          if (!field->eq(cur_part->field)) continue;
          if (i > max_part_no) max_part_no = i;
          break;
        }
        if (cur_part == end_part) {
          found = false;
          break;
        }
      }
      if (found) break;
    }
  }

  if (idx >= tab->s->keys) return 0;

  KEY *key_info = &tab->key_info[idx];
  if (key_info->has_records_per_key(max_part_no))
    keys_per_group = key_info->records_per_key(max_part_no);
  else
    keys_per_group = guess_rec_per_key(tab, key_info, max_part_no + 1);

  return keys_per_group;
}

static void estimate_num_groups(JOIN *src_join, const TableCollection *tables,
                                double *distinct_rows, double *est_rows,
                                bool is_sub) {
  double rows = 1.0;
  double dist_rows = 1.0;

  for (const pack_rows::Table &table : tables->tables()) {
    Table_ref *tl = table.table->pos_in_table_list;
    if (tl == nullptr) continue;

    JOIN_TAB *join_tab = table.table->reginfo.join_tab;
    QEP_TAB *qep_tab = table.table->reginfo.qep_tab;
    const QEP_shared_owner *tab =
        join_tab != nullptr ? implicit_cast<QEP_shared_owner *>(join_tab)
                            : implicit_cast<QEP_shared_owner *>(qep_tab);
    assert(tab != nullptr);

    if (tab->type() == JT_EQ_REF || tab->type() == JT_CONST ||
        tab->type() == JT_SYSTEM) {
      continue;
    }

    POSITION *const pos = tab->position();
    if (pos == nullptr) {
      *distinct_rows = 1.0;
      *est_rows = 1.0;
      return;
    }

    const double rowcount = pos->rows_fetched * pos->filter_effect;
    const double rec_per_key = get_record_per_key(&table);
    if (rec_per_key > 0)
      dist_rows *= rowcount / rec_per_key;
    else
      dist_rows *= rowcount;
    rows *= rowcount;
  }

  *distinct_rows = dist_rows != 0 ? dist_rows : 1;
  if (is_sub)
    *est_rows = std::max(rows, static_cast<double>(src_join->best_rowcount));
  else
    *est_rows = rows;
  if (*est_rows == 0) *est_rows = 1;
}

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

static void SetCostOnPtrcPath(THD *thd, Item_subselect *sub, JOIN *join,
                              AccessPath *path) {
  const Cost_model_server &cost_model = *thd->cost_model();
  const double cpu_tuple_cost = cost_model.row_evaluate_cost(1);
  const double cpu_operator_cost = cost_model.tmptable_readwrite_cost(
      Cost_model_server::MEMORY_TMPTABLE, 1, 0);

  if (sub != nullptr) {
    path->cost = path->ptrc().child->cost +
                 sub->unit->first_query_block()->join->best_rowcount *
                     cpu_operator_cost;
    return;
  }

  const double hash_mem_bytes =
      thd->variables.partial_result_cache_max_mem_size;
  PathParameters *param = path->ptrc().param;
  double est_entry_bytes = ComputeRowSizeUpperBound(*param->key_tables);
  est_entry_bytes += ComputeRowSizeUpperBound(*param->res_tables);
  if (est_entry_bytes <= 0) est_entry_bytes = 1;

  const double est_cache_entries = floor(hash_mem_bytes / est_entry_bytes);
  double ndistinct;
  double est_rows;
  estimate_num_groups(join, param->key_tables, &ndistinct, &est_rows,
                      /*is_sub=*/false);
  if (ndistinct <= 0) ndistinct = 1;

  const double evict_ratio =
      1.0 - std::min(est_cache_entries, ndistinct) / ndistinct;
  double hit_ratio =
      1.0 / ndistinct * std::min(est_cache_entries, ndistinct) -
      ndistinct / est_rows;
  hit_ratio = std::max(hit_ratio, 0.0);

  double total_cost = path->ptrc().child->cost * (1.0 - hit_ratio);
  total_cost += cpu_tuple_cost;
  total_cost += cpu_tuple_cost * evict_ratio;

  const double calls = std::min(est_rows, static_cast<double>(join->best_rowcount));
  total_cost += cpu_tuple_cost / 10.0 * evict_ratio * calls;
  total_cost += cpu_tuple_cost + cpu_operator_cost * (1.0 - hit_ratio) * calls;

  path->set_num_output_rows(path->ptrc().child->num_output_rows());
  path->cost = total_cost;
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

  if (sub != nullptr) {
    join = sub->unit->first_query_block()->join;
    inner_tables = TablesBetween(0, join->tables);
  }

  assert(join != nullptr);
  if (join->query_block->opt_hints_qb != nullptr) {
    join->query_block->opt_hints_qb->apply_ptrc_hints(
        join, &force_tab_map, &ignore_tab_map, &force_subquery_ptrc,
        &ignore_subquery_ptrc, &force_join_ptrc, &ignore_join_ptrc);
  }

  if (sub != nullptr) {
    *force_to_use = force_subquery_ptrc;
    *force_not_use = ignore_subquery_ptrc;
    return;
  }

  *force_to_use = force_join_ptrc;
  *force_not_use = ignore_join_ptrc;
  if (*force_to_use || *force_not_use) return;
  if (force_tab_map == 0 && ignore_tab_map == 0) return;

  const table_map inner_table_map =
      ConvertQepTabMapToTableMap(join, inner_tables);
  if ((inner_table_map & ~force_tab_map) == 0) *force_to_use = true;
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

  for (auto qb = query_block; qb != nullptr; qb = qb->outer_query_block()) {
    if (qb->join == nullptr || qb->join->plan_is_const()) {
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
  if (parent != nullptr) {
    Item_subselect *sub = query_block->master_query_expression()->item;
    if (sub != nullptr && sub->has_aggregation()) {
      ptrc_work->add("chosen", false);
      ptrc_work->add_utf8(
          "cause",
          "Subquery referencing outer-query aggregation is not supported");
      return false;
    }

    for (Item *having :
         {parent->join->having_cond, parent->join->pushed_having_cond}) {
      if (having != nullptr &&
          having->walk(&Item::find_item_processor, enum_walk::SUBQUERY_POSTFIX,
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

static void trace_tables_info(JOIN *join, qep_tab_map tables,
                              Opt_trace_array *trace_array) {
  THD *const thd = join->thd;
  for (QEP_TAB *tab : TablesContainedIn(join, tables)) {
    Table_ref *const tr = tab->table_ref;
    if (tr == nullptr) continue;
    StringBuffer<32> str;
    tr->print(thd, &str,
              enum_query_type(QT_TO_SYSTEM_CHARSET | QT_SHOW_SELECT_NUMBER |
                              QT_NO_DEFAULT_DB | QT_DERIVED_TABLE_ONLY_ALIAS));
    trace_array->add_utf8(str.ptr(), str.length());
  }
}

static void reset_record(TableCollection *const tc) {
  for (const pack_rows::Table &tbl : tc->tables()) {
    TABLE *table = tbl.table;
    if (!table->has_null_row() && !table->const_table &&
        table->record[0] != nullptr) {
      memset(table->record[0], 0, table->s->reclength);
    }
  }
}

AccessPath *CreateAccessPath(THD *thd, Item_subselect *sub,
                             AccessPath *inner_path,
                             qep_tab_map ptrc_key_tables,
                             qep_tab_map ptrc_res_tables, JOIN *ptrc_join) {
  assert(inner_path != nullptr);

  if (sub == nullptr && ptrc_join == nullptr) return inner_path;
  if (sub != nullptr && !sub->unit->is_simple()) return inner_path;

  bool force_to_use = false;
  bool force_not_use = false;

  Opt_trace_context *const trace = &thd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object ptrc_work(trace, "partial_result_cache");
  ptrc_work.add_utf8("caching_result_of",
                     sub != nullptr ? "subquery" : "nested_loop_join");
  if (ptrc_join != nullptr) {
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
  if ((!force_to_use && !is_ptrc_enabled(thd)) || force_not_use) {
    ptrc_work.add("chosen", false);
    ptrc_work.add_utf8("cause", "Disabled by optimizer_switch or hint");
    return inner_path;
  }

  bool deterministic = true;
  Item_in_subselect *in_subs = dynamic_cast<Item_in_subselect *>(sub);
  if (ptrc_join != nullptr && ptrc_join->where_cond != nullptr &&
      (ptrc_join->where_cond->used_tables() & RAND_TABLE_BIT)) {
    deterministic = false;
  } else if (in_subs != nullptr &&
             (in_subs->left_expr->used_tables() & RAND_TABLE_BIT)) {
    deterministic = false;
  }

  Query_block *query_block =
      sub != nullptr ? sub->unit->first_query_block() : ptrc_join->query_block;
  if (!is_ptrc_suitable(thd, query_block, inner_path, deterministic,
                        &ptrc_work)) {
    return inner_path;
  }

  if (thd->ptrc_objects == nullptr) {
    thd->ptrc_objects = new (std::nothrow) Memory_objects();
    if (thd->ptrc_objects == nullptr) return inner_path;
    MEM_ROOT *root = &thd->ptrc_objects->root;
    init_sql_alloc(key_memory_ptrc_mem_root, root, 16 * 1024);
    root->set_max_capacity(thd->variables.partial_result_cache_max_mem_size);
  }

  PtrcMemRoot ptrc_mem_root(thd);

  TableCollection *key_tables = new (thd->mem_root) TableCollection();
  if (key_tables == nullptr) return inner_path;

  auto free_key_tables =
      create_scope_guard([key_tables] { destroy(key_tables); });
  if (make_key_cols_tables(thd, sub, ptrc_key_tables, ptrc_join, key_tables) ==
      nullptr) {
    return inner_path;
  }
  if (key_tables->tables().empty()) return inner_path;

  JOIN *join = ptrc_join;
  if (sub != nullptr) {
    Query_block *outer_query_block = sub->unit->outer_query_block();
    if (outer_query_block == nullptr) return inner_path;
    join = outer_query_block->join;
  }
  if (join == nullptr) return inner_path;

  if (!force_to_use) {
    const double ratio = estimate_distinct_val_ratio(
        join, key_tables, &ptrc_work, /*is_sub=*/sub != nullptr);
    ptrc_work.add("estimate_distinct_val_ratio", ratio);
    if (ratio < get_min_cost_threshold(thd)) {
      ptrc_work.add("chosen", false);
      ptrc_work.add_utf8("cause", "cost below threshold");
      return inner_path;
    }
  }

  AccessPath *path = new (thd->mem_root) AccessPath;
  if (path == nullptr) return inner_path;
  path->type = AccessPath::PARTIAL_RESULT_CACHE;
  path->ptrc().child = inner_path;

  PathParameters *param = new (thd->mem_root) PathParameters();
  if (param == nullptr) return inner_path;
  if (thd->ptrc_objects->params.push_back(param)) {
    destroy(param);
    return inner_path;
  }

  path->ptrc().param = param;
  param->key_tables = key_tables;
  free_key_tables.release();

  if (sub != nullptr) {
    param->res_items = sub->unit->first_query_block()->join->fields;
    if (sub->substype() == Item_subselect::IN_SUBS ||
        sub->substype() == Item_subselect::ALL_SUBS ||
        sub->substype() == Item_subselect::ANY_SUBS ||
        sub->substype() == Item_subselect::EXISTS_SUBS) {
      if (down_cast<Item_exists_subselect *>(sub)->strategy ==
          Subquery_strategy::SUBQ_EXISTS) {
        param->is_exists_subquery = true;
      }
    }
  }

  param->res_tables = new (thd->mem_root) TableCollection();
  if (param->res_tables == nullptr) return inner_path;
  if (make_res_cols_tables(thd, sub != nullptr ? sub->unit->first_query_block()
                                               : nullptr,
                           param->res_items, &param->res_tmp_table,
                           &param->pseudo_context_table, ptrc_res_tables,
                           ptrc_join, param->res_tables) == nullptr) {
    return inner_path;
  }
  if (in_subs != nullptr) param->set_was_null(in_subs);

  SetCostOnPtrcPath(thd, sub, join, path);
  reset_record(param->key_tables);
  reset_record(param->res_tables);

  ptrc_work.add("chosen", true);
  if (force_to_use) ptrc_work.add_utf8("cause", "Hint forces to use");
  return path;
}

static bool reserve_space(String *buffer, TableCollection *tables) {
  DBUG_EXECUTE_IF("ptrc_initrowbuffer_reserve_space_fail", {
    my_error(ER_OUTOFMEMORY, MYF(0), 20210404);
    return true;
  });
  if (!tables->has_blob_column()) {
    const size_t upper_row_size = ComputeRowSizeUpperBound(*tables);
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

  m_thd->mem_root->set_error_for_capacity_exceeded(true);
  Dummy_error_handler error_handler;
  m_thd->push_internal_handler(&error_handler);

  bool res = true;
  if (m_row_buffer->Init(true) || error_handler.is_error_handled()) goto final;
  m_current_row = LinkedImmutableString{nullptr};

  if (reserve_space(&key_buff, m_key_tables) ||
      reserve_space(&res_buff, m_res_tables)) {
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
      m_key_tables(key_tables),
      m_res_tables(res_tables),
      m_thd(thd),
      m_source_iterator(std::move(source)),
      m_res_items(res_items),
      m_res_tmp_table(res_tmp_table),
      m_was_null(was_null),
      m_need_fill_res_tables(need_fill_res_tables) {
  assert(m_source_iterator != nullptr);
}

PtrcIterator::~PtrcIterator() {
  destroy(m_row_buffer);
  LRU_node *node = LRU_list.head;
  while (node != nullptr) {
    LRU_list.head = node->next;
    destroy(node);
    node = LRU_list.head;
  }
  destroy(ptrc_context);
  if (m_res_tmp_table != nullptr) RestoreCacheItems();
  delete saved_res_items;
  delete cached_res_fields;
}

bool PtrcIterator::Init() {
  if (status == Ptrc_status::BYPASS_MODE) return m_source_iterator->Init();

  bool res = true;
  if (m_row_buffer == nullptr || !m_row_buffer->Initialized()) {
    PrepareForRequestRowId(m_res_tables->tables(),
                           m_res_tables->tables_to_get_rowid_for());
    PtrcMemRoot ptrc_mem_root(m_thd);
    if (ptrc_context == nullptr) {
      ptrc_context = new (m_thd->mem_root) PTRC_context();
      if (ptrc_context == nullptr || !ptrc_context->is_valid()) goto final;
    }
    pseudo_context_table = m_res_tables->tables().back().table;
    if (InitRowBuffer()) goto final;
    if (m_res_tmp_table != nullptr) {
      saved_res_items = new mem_root_deque<Item *>(m_thd->mem_root);
      DBUG_EXECUTE_IF("fail_alloc_saved_res_items", {
        delete saved_res_items;
        saved_res_items = nullptr;
      });
      if (saved_res_items == nullptr) goto final;
      cached_res_fields = new mem_root_deque<Item *>(m_thd->mem_root);
      DBUG_EXECUTE_IF("fail_alloc_cached_res_fields", {
        delete cached_res_fields;
        cached_res_fields = nullptr;
      });
      if (cached_res_fields == nullptr) goto final;

      num_hidden_fields = CountHiddenFields(*m_res_items);
      for (uint i = 0; i < m_res_tmp_table->s->fields; i++) {
        Item_field *field = new Item_field(m_res_tmp_table->field[i]);
        if (field == nullptr) goto final;
        if (cached_res_fields->push_back(field) ||
            saved_res_items->push_back((*m_res_items)[i + num_hidden_fields])) {
          goto final;
        }
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
  for (uint i = 0; i < m_res_tmp_table->s->fields; i++) {
    type_conversion_status status =
        (*m_res_items)[i + num_hidden_fields]->save_in_field(
            m_res_tmp_table->field[i], false);
    if (status != TYPE_OK && m_res_tmp_table->in_use->is_error()) return true;
  }
  return false;
}

static bool read_keys_value(String *key_buff, TableCollection *tables) {
  return StoreFromTableBuffers(*tables, key_buff);
}

static bool read_res_value(String *res_buff, TableCollection *tables) {
  return StoreFromTableBuffers(*tables, res_buff);
}

void PtrcIterator::ChangeToUseTmpField() {
  std::copy(cached_res_fields->begin(), cached_res_fields->end(),
            m_res_items->begin() + num_hidden_fields);
}

void PtrcIterator::RestoreCacheItems() {
  if (saved_res_items != nullptr) {
    std::copy(saved_res_items->begin(), saved_res_items->end(),
              m_res_items->begin() + num_hidden_fields);
  }
}

bool PtrcIterator::insert_into_LRU_list() {
  LRU_node *node = new (m_thd->mem_root) LRU_node;
  if (node == nullptr) return true;

  LRU_list_type *LRU = &LRU_list;
  if (LRU->head == nullptr) {
    LRU->head = node;
    LRU->tail = node;
  } else {
    node->next = LRU->head;
    LRU->head->prev = node;
    LRU->head = node;
  }

  current_LRU_node = node;
  const ulonglong addr = reinterpret_cast<ulonglong>(node);
  static_assert(sizeof(addr) == sizeof(ptrc_context->item_addr->value) &&
                    sizeof(addr) >= sizeof(node),
                "cannot store pointer into BIGINT");
  ptrc_context->item_addr->value = addr;
  ptrc_context->item_addr->save_in_field(pseudo_context_table->field[0],
                                         false);
  return false;
}

void PtrcIterator::update_LRU_list() {
  assert(LRU_list.head != nullptr);
  LRU_node *node =
      reinterpret_cast<LRU_node *>(pseudo_context_table->field[0]->val_int());
  if (LRU_list.head == node) return;

  if (node == LRU_list.tail) LRU_list.tail = node->prev;
  node->prev->next = node->next;
  if (node->next != nullptr) node->next->prev = node->prev;
  node->next = LRU_list.head;
  node->prev = nullptr;
  LRU_list.head->prev = node;
  LRU_list.head = node;
}

Key_pair *PtrcIterator::LRU_free_mem(THD *thd, const Key &key,
                                     size_t res_size) {
  Key_pair *res = nullptr;
  std::list<Key_pair *> &free_list = LRU_list.free_list;
  const size_t required_key_bytes =
      ImmutableStringWithLength::RequiredBytesForEncode(key.size());
  const size_t required_row_bytes =
      LinkedImmutableString::RequiredBytesForEncode(res_size);
  ImmutableStringWithLength need_delete_node{nullptr};
  size_t expected_key_size = 0;
  size_t expected_value_size = 0;

  if (!free_list.empty()) {
    for (auto it = free_list.begin(); it != free_list.end(); ++it) {
      if ((*it)->first.second >= required_key_bytes &&
          (*it)->second.second >= required_row_bytes) {
        Key_pair *tmp = *it;
        free_list.erase(it);
        return tmp;
      }
    }
  }

  if (LRU_list.head == nullptr || LRU_list.tail == LRU_list.head)
    return nullptr;

  LRU_node *start_batch = LRU_list.tail;
  LRU_node *tail = LRU_list.tail;
  LRU_node *prev_node = tail->prev;
  do {
    if (tail == LRU_list.head) break;
    if (tail->key_data.Decode() == key) {
      tail = prev_node;
      prev_node = prev_node->prev;
      continue;
    }
    if (!(tail->key_data == start_batch->key_data)) start_batch = tail;

    if (tail->key_size >= required_key_bytes &&
        tail->row_size >= required_row_bytes) {
      expected_key_size = tail->key_size;
      expected_value_size = tail->row_size;
      prev_node = start_batch->prev;
      tail = start_batch;
      need_delete_node = start_batch->key_data;
      while (tail->key_data == need_delete_node) {
        DBUG_EXECUTE_IF("key_pair_key_null_pointer_injection",
                        return nullptr;);
        Key_pair *key_pair = new (thd->mem_root) Key_pair(
            {tail->key_data.GetDataPointer(), tail->key_size},
            {tail->row_data.GetDataPointer(), tail->row_size});
        if (key_pair == nullptr) return nullptr;
        if (tail->key_size == expected_key_size &&
            tail->row_size == expected_value_size && res == nullptr) {
          res = key_pair;
        } else {
          free_list.emplace_back(key_pair);
        }

        assert(prev_node != nullptr);
        prev_node->next = tail->next;
        if (tail->next != nullptr)
          tail->next->prev = prev_node;
        else
          LRU_list.tail = prev_node;

        destroy(tail);
        tail = prev_node;
        prev_node = prev_node->prev;
        if (tail == LRU_list.head) break;
        stats.cache_evictions++;
      }
      break;
    }

    tail = prev_node;
    prev_node = prev_node->prev;
  } while (true);

  if (res != nullptr && need_delete_node.Decode().data() != nullptr)
    m_row_buffer->erase(need_delete_node.Decode());
  return res;
}

static const auto NO_MATCHED_ROW = 1ULL;
static const auto WAS_NULL = 1ULL << 1;

int PtrcIterator::Read() {
  if (status == Ptrc_status::BYPASS_MODE) {
    const int error = m_source_iterator->Read();
    if (error == 0 && m_res_tables->store_rowids()) {
      RequestRowId(m_res_tables->tables(),
                   m_res_tables->tables_to_get_rowid_for());
    }
    return error;
  }

  assert(m_row_buffer->Initialized());
  PtrcMemRoot ptrc_mem_root(m_thd);
  int res = 0;

  if (status == Ptrc_status::LOOKUP || status == Ptrc_status::END_OF_SCAN) {
    if (read_keys_value(&key_buff, m_key_tables)) goto err;
    change_status(Ptrc_status::LOOKUP);
    m_current_row =
        m_row_buffer
            ->find(Key(reinterpret_cast<const char *>(key_buff.ptr()),
                       key_buff.length()))
            .value_or(LinkedImmutableString{nullptr});
    if (m_current_row != nullptr) {
      stats.cache_hits++;
      LoadIntoTableBuffers(
          *m_res_tables,
          reinterpret_cast<const uchar *>(m_current_row.DecodeFixed().data));
      update_LRU_list();
      if (m_res_tmp_table != nullptr) ChangeToUseTmpField();
      const auto flags = pseudo_context_table->field[1]->val_int();
      if (m_was_null != nullptr) *m_was_null = flags & WAS_NULL;
      if (flags & NO_MATCHED_ROW) {
        if (m_res_tmp_table != nullptr) RestoreCacheItems();
        change_status(Ptrc_status::END_OF_SCAN);
        return -1;
      }
      change_status(Ptrc_status::FETCH_NEXT_TUPLE);
      m_current_row = m_current_row.DecodeFixed().next;
      return res;
    }
    change_status(Ptrc_status::FILLING_CACHE);
  } else if (status == Ptrc_status::FETCH_NEXT_TUPLE) {
    if (m_current_row == nullptr) {
      if (m_res_tmp_table != nullptr) RestoreCacheItems();
      change_status(Ptrc_status::END_OF_SCAN);
      return -1;
    }
    LoadIntoTableBuffers(
        *m_res_tables,
        reinterpret_cast<const uchar *>(m_current_row.DecodeFixed().data));
    update_LRU_list();
    m_current_row = m_current_row.DecodeFixed().next;
    stats.cache_hits++;
    return res;
  }

  assert(status != Ptrc_status::END_OF_SCAN);

  if (status == Ptrc_status::FILLING_CACHE) {
    ptrc_mem_root.do_swap();
    res = m_source_iterator->Read();
    ptrc_mem_root.do_swap();

    if (res != 0) change_status(Ptrc_status::END_OF_SCAN);
    if (res > 0) return res;
    if (res < 0) {
      if (!m_row_buffer->LastKeyStored().IsEmpty() &&
          key_buff.length() == m_row_buffer->LastKeyLengthStored() &&
          !memcmp(key_buff.ptr(), m_row_buffer->LastKeyStored().Decode().data(),
                  m_row_buffer->LastKeyLengthStored())) {
        return res;
      }
    }

    if (res == 0 && m_res_tables->store_rowids()) {
      RequestRowId(m_res_tables->tables(),
                   m_res_tables->tables_to_get_rowid_for());
    }

    if (!((++stats.cache_misses) %
          m_thd->variables.partial_result_cache_check_hit_ratio_frequency) &&
        (static_cast<double>(stats.cache_hits) /
         (static_cast<double>(stats.cache_hits) + stats.cache_misses)) <
            m_thd->variables.partial_result_cache_min_hit_ratio) {
      if (!IsDisabled()) {
        DisablePtrc();
        Opt_trace_context *const trace = &m_thd->opt_trace;
        Opt_trace_object trace_wrapper(trace);
        Opt_trace_object ptrc_work(trace, "partial_result_cache");
        ptrc_work.add("disabled", true);
        ptrc_work.add_utf8("cause", "hit count is too low");
        if (m_res_tmp_table != nullptr) RestoreCacheItems();
        return res;
      }
    }

    if (insert_into_LRU_list()) goto err;
    ptrc_context->exec_flags->value = 0;
    if (res == 0) {
      if (m_res_tmp_table != nullptr && m_need_fill_res_tables) {
        ptrc_mem_root.do_swap();
        const bool failed = FillTmpTable();
        ptrc_mem_root.do_swap();
        if (failed) goto err;
      }
    } else {
      ptrc_context->exec_flags->value |= NO_MATCHED_ROW;
    }
    if (m_was_null != nullptr && *m_was_null)
      ptrc_context->exec_flags->value |= WAS_NULL;

    ptrc_context->exec_flags->save_in_field(pseudo_context_table->field[1],
                                            false);
    if (read_res_value(&res_buff, m_res_tables)) goto err;

    StoreRowResult store_row_result = m_row_buffer->StoreRow(
        key_buff.ptr(), key_buff.length(), /*key_store_pos=*/nullptr,
        res_buff.ptr(), res_buff.length(), /*val_store_pos=*/nullptr);
    stats.mem_used = m_thd->mem_root->allocated_size();
    if (store_row_result == StoreRowResult::BUFFER_FULL ||
        store_row_result == StoreRowResult::FATAL_ERROR) {
      Key_pair *free_node = LRU_free_mem(
          m_thd,
          Key(reinterpret_cast<const char *>(key_buff.ptr()),
              key_buff.length()),
          res_buff.length());
      if (free_node == nullptr) {
        if (LRU_list.head == LRU_list.tail) {
          LRU_list.head = nullptr;
          LRU_list.tail = nullptr;
        } else {
          LRU_list.head = LRU_list.head->next;
          current_LRU_node->next->prev = nullptr;
        }
        destroy(current_LRU_node);
        stats.cache_overflows++;
        goto err;
      }

      store_row_result = m_row_buffer->StoreRow(
          key_buff.ptr(), key_buff.length(),
          const_cast<char *>(free_node->first.first), res_buff.ptr(),
          res_buff.length(), const_cast<char *>(free_node->second.first));
      assert(store_row_result != StoreRowResult::FATAL_ERROR);
      if (store_row_result == StoreRowResult::BUFFER_FULL) goto err;
    }

    current_LRU_node->set_data(
        m_row_buffer->LastKeyStored().GetDataPointer(),
        ImmutableStringWithLength::RequiredBytesForEncode(
            m_row_buffer->LastKeyLengthStored()),
        m_row_buffer->LastRowStored().GetDataPointer(),
        LinkedImmutableString::RequiredBytesForEncode(
            m_row_buffer->LastRowLengthStored()));
  }

  return res;

err:
  stats.cache_misses++;
  if (m_res_tmp_table != nullptr) RestoreCacheItems();
  change_status(Ptrc_status::BYPASS_MODE);
  return res;
}

void PtrcIterator::SetNullRowFlag(bool is_null_row) {
  m_source_iterator->SetNullRowFlag(is_null_row);
}

void PtrcIterator::UnlockRow() {}

void PtrcIterator::StartPSIBatchMode() {
  m_source_iterator->StartPSIBatchMode();
}

void PtrcIterator::EndPSIBatchModeIfStarted() {
  m_source_iterator->EndPSIBatchModeIfStarted();
}

bool is_ptrc_enabled(const THD *thd) {
  return thd->optimizer_switch_flag(OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE);
}

double get_min_cost_threshold(const THD *thd) {
  return thd->variables.partial_result_cache_cost_threshold;
}

void cleanup(THD *thd) {
  if (thd->ptrc_objects == nullptr) return;

  assert(&thd->ptrc_objects->root == thd->ptrc_objects->saved_root);
  delete thd->ptrc_objects;
  thd->ptrc_objects = nullptr;
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
  if (path->iterator != nullptr) {
    PtrcIterator *iterator =
        down_cast<PtrcIterator *>(path->iterator->real_iterator());
    assert(iterator != nullptr);
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

}  // namespace ptrc
