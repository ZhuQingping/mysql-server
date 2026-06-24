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

#include <cstring>
#include <vector>

#include "sql/parallel_query/sql_parallel.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"

bool Table_ref::pq_copy(THD *thd, Table_ref *tbl_list) {
  if (thd == nullptr || tbl_list == nullptr) return true;

  effective_algorithm = tbl_list->effective_algorithm;
  set_tableno(tbl_list->tableno());
  set_derived_column_names(tbl_list->derived_column_names());

  if (table_name == nullptr && tbl_list->table_name_length != 0) {
    table_name = strmake_root(thd->mem_root, tbl_list->table_name,
                              tbl_list->table_name_length);
    if (table_name == nullptr) return true;
    table_name_length = tbl_list->table_name_length;
  }

  if (alias == nullptr && tbl_list->alias != nullptr) {
    is_alias = true;
    alias =
        strmake_root(thd->mem_root, tbl_list->alias, strlen(tbl_list->alias));
    if (alias == nullptr) return true;
  }

  if (db == nullptr && tbl_list->db_length != 0) {
    db = strmake_root(thd->mem_root, tbl_list->db, tbl_list->db_length);
    if (db == nullptr) return true;
    db_length = tbl_list->db_length;
  }

  return false;
}

AccessPath *CopyRangeScanAccessPath(THD *thd, AccessPath *orig_path,
                                    TABLE *table) {
  if (thd == nullptr || orig_path == nullptr || table == nullptr) {
    return nullptr;
  }
  MEM_ROOT *const mem_root =
      thd->pq_mem_root != nullptr ? thd->pq_mem_root : thd->mem_root;
  if (mem_root == nullptr) return nullptr;

  assert(orig_path->type != AccessPath::INDEX_SKIP_SCAN);
  assert(orig_path->type != AccessPath::GROUP_INDEX_SKIP_SCAN);

  AccessPath *path = new (mem_root) AccessPath;
  if (path == nullptr) return nullptr;
  *path = *orig_path;
  path->iterator = nullptr;

  switch (path->type) {
    case AccessPath::INDEX_RANGE_SCAN: {
      const unsigned num_used_key_parts =
          path->index_range_scan().num_used_key_parts;
      const unsigned num_ranges = path->index_range_scan().num_ranges;
      if (orig_path->index_range_scan().used_key_part == nullptr ||
          orig_path->index_range_scan().ranges == nullptr) {
        return nullptr;
      }

      path->index_range_scan().used_key_part = static_cast<KEY_PART *>(
          mem_root->Alloc(sizeof(KEY_PART) * num_used_key_parts));
      if (path->index_range_scan().used_key_part == nullptr) return nullptr;
      memcpy(path->index_range_scan().used_key_part,
             orig_path->index_range_scan().used_key_part,
             sizeof(KEY_PART) * num_used_key_parts);
      for (unsigned i = 0; i < num_used_key_parts; ++i) {
        const uint16 key = path->index_range_scan().used_key_part[i].key;
        const uint16 part = path->index_range_scan().used_key_part[i].part;
        assert(part == i);
        if (key >= table->s->keys ||
            part >= table->key_info[key].user_defined_key_parts) {
          return nullptr;
        }
        path->index_range_scan().used_key_part[i].field =
            table->key_info[key].key_part[part].field;
      }

      path->index_range_scan().ranges = static_cast<QUICK_RANGE **>(
          mem_root->Alloc(sizeof(QUICK_RANGE *) * num_ranges));
      if (path->index_range_scan().ranges == nullptr) return nullptr;
      for (unsigned i = 0; i < num_ranges; ++i) {
        QUICK_RANGE *orig_range = orig_path->index_range_scan().ranges[i];
        if (orig_range == nullptr) return nullptr;
        path->index_range_scan().ranges[i] = new (mem_root) QUICK_RANGE(
            mem_root, orig_range->min_key, orig_range->min_length,
            orig_range->min_keypart_map, orig_range->max_key,
            orig_range->max_length, orig_range->max_keypart_map,
            orig_range->flag, orig_range->rkey_func_flag);
        if (path->index_range_scan().ranges[i] == nullptr) return nullptr;
      }
      break;
    }
    case AccessPath::INDEX_MERGE:
      path->index_merge().children =
          new (mem_root) Mem_root_array<AccessPath *>(mem_root);
      if (path->index_merge().children == nullptr) return nullptr;
      for (AccessPath *child : *orig_path->index_merge().children) {
        AccessPath *const cloned_child =
            CopyRangeScanAccessPath(thd, child, table);
        if (cloned_child == nullptr ||
            path->index_merge().children->push_back(cloned_child)) {
          return nullptr;
        }
      }
      path->index_merge().table = table;
      break;
    case AccessPath::ROWID_INTERSECTION:
      path->rowid_intersection().children =
          new (mem_root) Mem_root_array<AccessPath *>(mem_root);
      if (path->rowid_intersection().children == nullptr) return nullptr;
      for (AccessPath *child : *orig_path->rowid_intersection().children) {
        AccessPath *const cloned_child =
            CopyRangeScanAccessPath(thd, child, table);
        if (cloned_child == nullptr ||
            path->rowid_intersection().children->push_back(cloned_child)) {
          return nullptr;
        }
      }
      if (path->rowid_intersection().cpk_child != nullptr) {
        path->rowid_intersection().cpk_child = CopyRangeScanAccessPath(
            thd, orig_path->rowid_intersection().cpk_child, table);
        if (path->rowid_intersection().cpk_child == nullptr) return nullptr;
      }
      path->rowid_intersection().table = table;
      break;
    case AccessPath::ROWID_UNION:
      path->rowid_union().children =
          new (mem_root) Mem_root_array<AccessPath *>(mem_root);
      if (path->rowid_union().children == nullptr) return nullptr;
      for (AccessPath *child : *orig_path->rowid_union().children) {
        AccessPath *const cloned_child =
            CopyRangeScanAccessPath(thd, child, table);
        if (cloned_child == nullptr ||
            path->rowid_union().children->push_back(cloned_child)) {
          return nullptr;
        }
      }
      path->rowid_union().table = table;
      break;
    default:
      assert(false);
      return nullptr;
  }
  return path;
}

ORDER *pq_dup_order(THD *, Query_block *, ORDER *) { return nullptr; }

bool pq_dup_tabs(JOIN *, JOIN *, bool) { return true; }

static bool pq_cstring_eq(const char *left, const char *right) {
  if (left == nullptr || right == nullptr) return left == right;
  return strcmp(left, right) == 0;
}

static bool pq_lex_cstring_eq(const LEX_CSTRING &left,
                              const LEX_CSTRING &right) {
  if (left.str == nullptr || right.str == nullptr) {
    return left.str == right.str && left.length == right.length;
  }
  return left.length == right.length &&
         memcmp(left.str, right.str, left.length) == 0;
}

static void pq_copy_position_scalar_fields(POSITION *dst,
                                           const POSITION *src) {
  dst->rows_fetched = src->rows_fetched;
  dst->read_cost = src->read_cost;
  dst->filter_effect = src->filter_effect;
  dst->prefix_rowcount = src->prefix_rowcount;
  dst->prefix_cost = src->prefix_cost;
  dst->table = nullptr;
  dst->key = nullptr;
  dst->ref_depend_map = src->ref_depend_map;
  dst->use_join_buffer = src->use_join_buffer;
  dst->sj_strategy = src->sj_strategy;
  dst->n_sj_tables = src->n_sj_tables;
  dst->dups_producing_tables = src->dups_producing_tables;
  dst->first_loosescan_table = src->first_loosescan_table;
  dst->loosescan_need_tables = src->loosescan_need_tables;
  dst->loosescan_key = src->loosescan_key;
  dst->loosescan_parts = src->loosescan_parts;
  dst->first_firstmatch_table = src->first_firstmatch_table;
  dst->first_firstmatch_rtbl = src->first_firstmatch_rtbl;
  dst->firstmatch_need_tables = src->firstmatch_need_tables;
  dst->cur_embedding_map = src->cur_embedding_map;
  dst->first_dupsweedout_table = src->first_dupsweedout_table;
  dst->dupsweedout_tables = src->dupsweedout_tables;
  dst->sjm_scan_last_inner = src->sjm_scan_last_inner;
  dst->sjm_scan_need_tables = src->sjm_scan_need_tables;
}

static bool pq_position_scalar_fields_equal(const POSITION *left,
                                            const POSITION *right) {
  return left->rows_fetched == right->rows_fetched &&
         left->read_cost == right->read_cost &&
         left->filter_effect == right->filter_effect &&
         left->prefix_rowcount == right->prefix_rowcount &&
         left->prefix_cost == right->prefix_cost &&
         left->table == nullptr && left->key == nullptr &&
         left->ref_depend_map == right->ref_depend_map &&
         left->use_join_buffer == right->use_join_buffer &&
         left->sj_strategy == right->sj_strategy &&
         left->n_sj_tables == right->n_sj_tables &&
         left->dups_producing_tables == right->dups_producing_tables &&
         left->first_loosescan_table == right->first_loosescan_table &&
         left->loosescan_need_tables == right->loosescan_need_tables &&
         left->loosescan_key == right->loosescan_key &&
         left->loosescan_parts == right->loosescan_parts &&
         left->first_firstmatch_table == right->first_firstmatch_table &&
         left->first_firstmatch_rtbl == right->first_firstmatch_rtbl &&
         left->firstmatch_need_tables == right->firstmatch_need_tables &&
         left->cur_embedding_map == right->cur_embedding_map &&
         left->first_dupsweedout_table == right->first_dupsweedout_table &&
         left->dupsweedout_tables == right->dupsweedout_tables &&
         left->sjm_scan_last_inner == right->sjm_scan_last_inner &&
         left->sjm_scan_need_tables == right->sjm_scan_need_tables;
}

static void pq_copy_qep_tab_scalar_fields(QEP_TAB *dst, QEP_TAB *src) {
  dst->set_reversed_access(src->reversed_access());
  dst->using_dynamic_range = src->using_dynamic_range;
  dst->set_type(src->type());
  dst->set_index(src->index());
  dst->keys().merge(src->keys());
  dst->set_prefix_tables(src->prefix_tables(),
                         src->prefix_tables() & ~src->added_tables());
  dst->set_first_inner(src->first_inner());
  dst->set_last_inner(src->last_inner());
  dst->set_first_upper(src->first_upper());
  dst->set_first_sj_inner(src->first_sj_inner());
  dst->set_last_sj_inner(src->last_sj_inner());
  dst->set_skip_records_in_range(src->skip_records_in_range());
  dst->firstmatch_return = src->firstmatch_return;
  dst->match_tab = src->match_tab;
  dst->loosescan_key_len = src->loosescan_key_len;
  dst->op_type = src->op_type;
  dst->materialize_table = src->materialize_table;
  dst->needs_duplicate_removal = src->needs_duplicate_removal;
}

static bool pq_qep_tab_scalar_fields_equal(QEP_TAB *left, QEP_TAB *right) {
  return left->reversed_access() == right->reversed_access() &&
         left->using_dynamic_range == right->using_dynamic_range &&
         left->type() == right->type() && left->index() == right->index() &&
         left->keys() == right->keys() &&
         left->prefix_tables() == right->prefix_tables() &&
         left->added_tables() == right->added_tables() &&
         left->first_inner() == right->first_inner() &&
         left->last_inner() == right->last_inner() &&
         left->first_upper() == right->first_upper() &&
         left->first_sj_inner() == right->first_sj_inner() &&
         left->last_sj_inner() == right->last_sj_inner() &&
         left->skip_records_in_range() == right->skip_records_in_range() &&
         left->firstmatch_return == right->firstmatch_return &&
         left->match_tab == right->match_tab &&
         left->loosescan_key_len == right->loosescan_key_len &&
         left->op_type == right->op_type &&
         left->materialize_table == right->materialize_table &&
         left->needs_duplicate_removal == right->needs_duplicate_removal &&
         left->table() == nullptr && left->table_ref == nullptr &&
         left->condition() == nullptr && left->range_scan() == nullptr &&
         left->position() == nullptr;
}

static void pq_reset_qep_tab_scalar_smoke_fields(QEP_TAB *tab) {
  tab->set_reversed_access(false);
  tab->using_dynamic_range = false;
  tab->set_type(JT_UNKNOWN);
  tab->set_index(0);
  tab->keys().clear_all();
  tab->set_prefix_tables(0, 0);
  tab->set_first_inner(NO_PLAN_IDX);
  tab->set_last_inner(NO_PLAN_IDX);
  tab->set_first_upper(NO_PLAN_IDX);
  tab->set_first_sj_inner(NO_PLAN_IDX);
  tab->set_last_sj_inner(NO_PLAN_IDX);
  tab->set_skip_records_in_range(false);
  tab->firstmatch_return = NO_PLAN_IDX;
  tab->match_tab = NO_PLAN_IDX;
  tab->loosescan_key_len = 0;
  tab->op_type = QEP_TAB::OT_NONE;
  tab->materialize_table = QEP_TAB::NO_SETUP;
  tab->needs_duplicate_removal = false;
}

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

bool pq_bind_qep_tab_table_preflight(JOIN *worker_join, TABLE *worker_table,
                                     TABLE *leader_table) {
  pq_global_stats.worker_qep_tab_table_bind_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (worker_join == nullptr || worker_join->qep_tab == nullptr ||
      worker_table == nullptr || leader_table == nullptr ||
      worker_table == leader_table || worker_table->file == nullptr ||
      leader_table->file == nullptr || worker_table->file == leader_table->file ||
      worker_table->record[0] == nullptr || leader_table->record[0] == nullptr ||
      worker_table->record[0] == leader_table->record[0] ||
      worker_table->pos_in_table_list == nullptr ||
      worker_join->const_tables >= worker_join->primary_tables ||
      worker_join->const_tables != 0 || worker_join->primary_tables != 1) {
    pq_global_stats.worker_qep_tab_table_bind_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  QEP_TAB *const tab = &worker_join->qep_tab[worker_join->const_tables];
  Table_ref *const worker_ref = worker_table->pos_in_table_list;
  Table_ref *const leader_ref = leader_table->pos_in_table_list;
  QEP_TAB *const saved_worker_qep_tab = worker_table->reginfo.qep_tab;
  Query_block *const saved_ref_query_block = worker_ref->query_block;

  bool failed = tab->join() != worker_join || tab->idx() != 0 ||
                tab->table() != nullptr || tab->table_ref != nullptr ||
                tab->condition() != nullptr || tab->range_scan() != nullptr ||
                worker_table->s == nullptr || leader_table->s == nullptr ||
                leader_ref == nullptr || worker_ref == leader_ref ||
                worker_ref->table != worker_table ||
                leader_ref->table != leader_table ||
                saved_worker_qep_tab != nullptr ||
                !pq_lex_cstring_eq(worker_table->s->db, leader_table->s->db) ||
                !pq_lex_cstring_eq(worker_table->s->table_name,
                                   leader_table->s->table_name) ||
                !pq_cstring_eq(worker_ref->alias, leader_ref->alias) ||
                !bitmap_cmp(worker_table->read_set, leader_table->read_set) ||
                !bitmap_cmp(worker_table->write_set, leader_table->write_set);

  if (failed) {
    pq_global_stats.worker_qep_tab_table_bind_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  worker_ref->query_block = worker_join->query_block;
  tab->table_ref = worker_ref;
  tab->set_table(worker_table);

  failed = tab->table() != worker_table || tab->table_ref != worker_ref ||
           worker_table->reginfo.qep_tab != tab ||
           worker_ref->query_block != worker_join->query_block ||
           tab->condition() != nullptr || tab->range_scan() != nullptr;

  tab->set_table(nullptr);
  worker_table->reginfo.qep_tab = saved_worker_qep_tab;
  tab->table_ref = nullptr;
  worker_ref->query_block = saved_ref_query_block;

  if (failed || tab->table() != nullptr || tab->table_ref != nullptr ||
      worker_ref->query_block != saved_ref_query_block ||
      worker_table->reginfo.qep_tab != saved_worker_qep_tab) {
    pq_global_stats.worker_qep_tab_table_bind_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  pq_global_stats.worker_qep_tab_table_bind_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool pq_clone_table_ref_preflight(THD *worker_thd, Table_ref *leader_ref) {
  pq_global_stats.worker_table_ref_clone_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (worker_thd == nullptr || worker_thd->mem_root == nullptr ||
      leader_ref == nullptr) {
    pq_global_stats.worker_table_ref_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  Table_ref *const cloned_ref = new (worker_thd->mem_root) Table_ref();
  if (cloned_ref == nullptr || cloned_ref->pq_copy(worker_thd, leader_ref)) {
    pq_global_stats.worker_table_ref_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  const bool failed =
      cloned_ref->tableno() != leader_ref->tableno() ||
      !pq_cstring_eq(cloned_ref->alias, leader_ref->alias) ||
      cloned_ref->table_name_length != leader_ref->table_name_length ||
      cloned_ref->db_length != leader_ref->db_length ||
      (leader_ref->table_name_length != 0 &&
       (cloned_ref->table_name == nullptr ||
        cloned_ref->table_name == leader_ref->table_name ||
        memcmp(cloned_ref->table_name, leader_ref->table_name,
               leader_ref->table_name_length) != 0)) ||
      (leader_ref->db_length != 0 &&
       (cloned_ref->db == nullptr || cloned_ref->db == leader_ref->db ||
        memcmp(cloned_ref->db, leader_ref->db, leader_ref->db_length) != 0));

  if (failed) {
    pq_global_stats.worker_table_ref_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  pq_global_stats.worker_table_ref_clone_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool pq_clone_position_scalar_preflight(QEP_TAB *leader_tab) {
  pq_global_stats.worker_position_clone_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (leader_tab == nullptr || leader_tab->position() == nullptr ||
      leader_tab->ref().key != -1) {
    pq_global_stats.worker_position_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  POSITION cloned_position{};
  pq_copy_position_scalar_fields(&cloned_position, leader_tab->position());
  if (!pq_position_scalar_fields_equal(&cloned_position,
                                       leader_tab->position())) {
    pq_global_stats.worker_position_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  pq_global_stats.worker_position_clone_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool pq_clone_qep_tab_scalar_preflight(JOIN *worker_join, QEP_TAB *leader_tab) {
  pq_global_stats.worker_qep_tab_scalar_clone_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (worker_join == nullptr || worker_join->qep_tab == nullptr ||
      leader_tab == nullptr || leader_tab->idx() < 0 ||
      leader_tab->idx() >= static_cast<plan_idx>(worker_join->tables)) {
    pq_global_stats.worker_qep_tab_scalar_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  QEP_TAB *const cloned_tab = &worker_join->qep_tab[leader_tab->idx()];
  if (cloned_tab->table() != nullptr || cloned_tab->table_ref != nullptr ||
      cloned_tab->condition() != nullptr || cloned_tab->range_scan() != nullptr ||
      cloned_tab->position() != nullptr) {
    pq_global_stats.worker_qep_tab_scalar_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  pq_copy_qep_tab_scalar_fields(cloned_tab, leader_tab);
  const bool failed =
      !pq_qep_tab_scalar_fields_equal(cloned_tab, leader_tab);
  pq_reset_qep_tab_scalar_smoke_fields(cloned_tab);

  if (failed || cloned_tab->table() != nullptr ||
      cloned_tab->table_ref != nullptr || cloned_tab->condition() != nullptr ||
      cloned_tab->range_scan() != nullptr || cloned_tab->position() != nullptr) {
    pq_global_stats.worker_qep_tab_scalar_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  pq_global_stats.worker_qep_tab_scalar_clone_success.fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

bool pq_clone_range_scan_preflight(THD *worker_thd, TABLE *worker_table,
                                   QEP_TAB *leader_tab) {
  pq_global_stats.worker_range_scan_clone_attempts.fetch_add(
      1, std::memory_order_relaxed);

  if (worker_thd == nullptr || worker_table == nullptr || leader_tab == nullptr ||
      leader_tab->table() == nullptr || leader_tab->range_scan() == nullptr ||
      leader_tab->range_scan()->type != AccessPath::INDEX_RANGE_SCAN) {
    pq_global_stats.worker_range_scan_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  AccessPath *const cloned_range =
      CopyRangeScanAccessPath(worker_thd, leader_tab->range_scan(), worker_table);
  if (cloned_range == nullptr ||
      cloned_range == leader_tab->range_scan() ||
      cloned_range->type != leader_tab->range_scan()->type ||
      cloned_range->index_range_scan().used_key_part ==
          leader_tab->range_scan()->index_range_scan().used_key_part ||
      cloned_range->index_range_scan().ranges ==
          leader_tab->range_scan()->index_range_scan().ranges ||
      cloned_range->index_range_scan().num_used_key_parts !=
          leader_tab->range_scan()->index_range_scan().num_used_key_parts ||
      cloned_range->index_range_scan().num_ranges !=
          leader_tab->range_scan()->index_range_scan().num_ranges) {
    pq_global_stats.worker_range_scan_clone_unsupported.fetch_add(
        1, std::memory_order_relaxed);
    return true;
  }

  for (unsigned i = 0; i < cloned_range->index_range_scan().num_used_key_parts;
       ++i) {
    if (cloned_range->index_range_scan().used_key_part[i].field == nullptr ||
        cloned_range->index_range_scan().used_key_part[i].field->table !=
            worker_table) {
      pq_global_stats.worker_range_scan_clone_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
  }
  for (unsigned i = 0; i < cloned_range->index_range_scan().num_ranges; ++i) {
    if (cloned_range->index_range_scan().ranges[i] == nullptr ||
        cloned_range->index_range_scan().ranges[i] ==
            leader_tab->range_scan()->index_range_scan().ranges[i]) {
      pq_global_stats.worker_range_scan_clone_unsupported.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
  }

  pq_global_stats.worker_range_scan_clone_success.fetch_add(
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
