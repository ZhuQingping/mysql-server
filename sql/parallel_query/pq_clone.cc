/* Copyright (c) 2020, Huawei and/or its affiliates. All rights reserved.

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

#include "sql/parallel_query/pq_clone.h"
#include "include/my_dbug.h"
#include "include/mysql/psi/mysql_thread.h"
#include "include/scope_guard.h"
#include "securec.h"  // memcpy_s
#include "sql/item_sum.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/mysqld.h"
#include "sql/nested_join.h"
#include "sql/parallel_query/pq_optimizer.h"
#include "sql/parallel_query/pq_resolver.h"
#include "sql/parallel_query/sql_parallel.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_base.h"
#include "sql/sql_lex.h"
#include "sql/sql_opt_exec_shared.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_plan_cache.h"  // is_clone_for_plan_cache
#include "sql/sql_resolver.h"
#include "sql/sql_tmp_table.h"
#include "sql/sql_union.h"
#include "sql/system_variables.h"

class COND_CMP;
bool Table_ref::pq_copy(THD *thd, Table_ref *tbl_list) {
  effective_algorithm = tbl_list->effective_algorithm;
  set_tableno(tbl_list->tableno());
  set_derived_column_names(tbl_list->derived_column_names());

  // A clone made by pq_set_table_ref() may not have a name yet:

  if (!table_name && tbl_list->table_name_length) {
    table_name = strmake_root(thd->mem_root, tbl_list->table_name,
                              tbl_list->table_name_length);
    table_name_length = tbl_list->table_name_length;
  }

  if (!alias && tbl_list->alias) {
    is_alias = true;
    alias =
        strmake_root(thd->mem_root, tbl_list->alias, strlen(tbl_list->alias));
  }

  if (!db && tbl_list->db_length) {
    db = strmake_root(thd->mem_root, tbl_list->db, tbl_list->db_length);
    db_length = tbl_list->db_length;
  }

  return false;
}

bool POSITION::pq_copy(THD *thd, QEP_TAB *orig_tab) {
  POSITION *orig = orig_tab->position();
  rows_fetched = orig->rows_fetched;
  read_cost = orig->read_cost;
  filter_effect = orig->filter_effect;
  prefix_rowcount = orig->prefix_rowcount;
  prefix_cost = orig->prefix_cost;
  table = nullptr;
  Key_use *chosen_keyuses[MAX_REF_PARTS];
  // Clone all of Key_use objects. When clone Index_lookup, value of Key_use
  // will be cloned and assigned because TABLE is not ready by here and
  // key_use->val can't be fixed correctly. Note: here we use ref().key to
  // check instead of orig->key is because RANGE_SCAN might result in
  // orig->key is not null but ref().key == -1.
  assert(orig_tab->ref().key != -1 ||
         (orig_tab->ref().key == -1 && orig_tab->ref().key_parts == 0));
  // If this is not a REF access, we think that POSITION::key is not necessary
  // in the clone (RANGE access won't use it) so we don't clone it.
  if (orig_tab->ref().key != -1) {
    const bool for_plan_cache =
        plan_cache::is_clone_for_plan_cache(orig_tab->join()->query_block);
    const auto mem_root = for_plan_cache ? thd->mem_root : thd->pq_mem_root;
    Key_use_array *key_uses = new (mem_root) Key_use_array(mem_root);
    if (!key_uses) return true;
    uint keyparts, length;
    // JOIN::set_prefix_tables() skips const tables which thus have
    // prefix_tables()==0, and const tables with bit field record in
    // join->found_const_table_map, which initlized value is INNER_TABLE_BIT.
    table_map used_tables;
    if (orig_tab->join())
      used_tables =
          orig_tab->prefix_tables() | orig_tab->join()->found_const_table_map;
    else
      used_tables = orig_tab->prefix_tables() | INNER_TABLE_BIT;

    Key_use *keyuse = orig->key;
    // calc_length_and_keypart() expects the first Key_use passed in argument
    // to belong to the key number passed in argument (see the do...while()
    // loop there). So we advance key_use to satisfy this requirement.
    while (keyuse->key != (uint)orig_tab->ref().key) keyuse++;
    // Here we need call this function to recollect chosen_keyuses because we
    // must make sure POSITION::key are same as Index_lookup::items. Details can
    // be checked in create_ref_for_key. For example, in a join of table
    // A with table B with WHERE A.col1=val1 AND A.col1=B.colX AND
    // A.col2=val2, if table A is first in plan, A.col1=B.colX will be
    // filtered out because B is not available yet for the REF access
    // . orig->key will be different from chosen_keyuses after function
    // filter. So we have to call this function here.
    calc_length_and_keyparts(keyuse, orig_tab, orig_tab->ref().key, used_tables,
                             chosen_keyuses, &length, &keyparts, nullptr,
                             nullptr);
    assert(keyparts == orig_tab->ref().key_parts);
    for (size_t i = 0; i < keyparts; i++) {
      Key_use key_use;
      chosen_keyuses[i]->pq_clone(&key_use);
      key_uses->push_back(key_use);
    }
    if (null_terminate_key_use_list(key_uses)) return true;
    key = key_uses->begin();
  }

  ref_depend_map = orig->ref_depend_map;
  use_join_buffer = orig->use_join_buffer;
  sj_strategy = orig->sj_strategy;
  n_sj_tables = orig->n_sj_tables;
  dups_producing_tables = orig->dups_producing_tables;
  first_loosescan_table = orig->first_loosescan_table;
  loosescan_need_tables = orig->loosescan_need_tables;
  loosescan_key = orig->loosescan_key;
  loosescan_parts = orig->loosescan_parts;
  first_firstmatch_table = orig->first_firstmatch_table;
  first_firstmatch_rtbl = orig->first_firstmatch_rtbl;
  firstmatch_need_tables = orig->firstmatch_need_tables;
  first_dupsweedout_table = orig->first_dupsweedout_table;
  dupsweedout_tables = orig->dupsweedout_tables;
  sjm_scan_last_inner = orig->sjm_scan_last_inner;
  sjm_scan_need_tables = orig->sjm_scan_need_tables;

  return false;
}

bool QEP_TAB::pq_copy(THD *thd, QEP_TAB *orig) {
  m_reversed_access = orig->m_reversed_access;
  pq_div_tab = orig->pq_div_tab;
  pq_cut_tab = orig->pq_cut_tab;
  join_cache_flags = orig->join_cache_flags;
  using_dynamic_range = orig->using_dynamic_range;

  set_type(orig->type());
  set_index(orig->index());
  keys().merge(orig->keys());
  set_prefix_tables_map(orig->prefix_tables());
  set_added_tables_map(orig->added_tables());

  JOIN *join = this->join();
  if (!join) return true;

  const bool for_plan_cache =
      plan_cache::is_clone_for_plan_cache(orig->join()->query_block);
  const auto mem_root = for_plan_cache ? thd->mem_root : thd->pq_mem_root;
  if (orig->position()) {
    POSITION *position = new (mem_root) POSITION;
    if (!position || position->pq_copy(thd, orig)) return true;
    set_position(position);
  }

  // support semi-join
  set_first_inner(orig->first_inner());
  set_last_inner(orig->last_inner());
  set_first_upper(orig->first_upper());
  set_first_sj_inner(orig->first_sj_inner());
  set_last_sj_inner(orig->last_sj_inner());
  set_skip_records_in_range(orig->skip_records_in_range());

  // semi-join with FirstMatch
  firstmatch_return = orig->firstmatch_return;

  // semi-join with DuplicateWeedout is handled later

  // semi-join with loose index scan
  match_tab = orig->match_tab;
  loosescan_key_len = orig->loosescan_key_len;

  if (!for_plan_cache && orig->pq_cond) {
    pq_cond = orig->pq_cond->pq_clone(join->thd, join->query_block);
    if (!pq_cond || pq_cond->refix_fields(join->thd, &pq_cond) ||
        DBUG_EVALUATE_IF("pq_clone_error2", true, false)) {
      sql_print_warning("[Parallel query]: ICP condition pushdown failed");
      return true;
    }
  }

  // copy join_buffer info.
  op_type = orig->op_type;
  materialize_table = orig->materialize_table;
  needs_duplicate_removal = orig->needs_duplicate_removal;  // for distinct
  return false;
}

bool TABLE::pq_copy(THD *thd, void *select_arg, TABLE *orig) {
  Query_block *select = static_cast<Query_block *>(select_arg);
  assert(!plan_cache::is_clone_for_plan_cache(select));
  possible_quick_keys = orig->possible_quick_keys;
  covering_keys = orig->covering_keys;
  key_read = orig->key_read;
  null_row = orig->null_row;
  const_table = orig->const_table;
  pq_saved_const_table = orig->pq_saved_const_table;
  if (orig->is_nullable()) {
    set_nullable();
  }

  if (part_info) {
    part_info->pq_copy_from(orig->part_info);
  }

  file->pushed_idx_cond_keyno = orig->file->pushed_idx_cond_keyno;
  Item *index_pushdown = orig->file->pushed_idx_cond;
  // needs deep copy
  file->pushed_idx_cond =
      index_pushdown ? index_pushdown->pq_clone(thd, select) : nullptr;
  Item *copy_index_pushdown = file->pushed_idx_cond;
  if ((index_pushdown && !copy_index_pushdown) ||
      (copy_index_pushdown &&
       copy_index_pushdown->refix_fields(thd, &copy_index_pushdown))) {
    return true;
  }

  /*
    Partial result cache is decided in ptrc::CreateAccessPath(), in a
    cost-based way. So this is called for the single-threaded initial plan,
    and for the leader's and workers' plans. So the copies used by these
    threads must have good enough statistics:
  */
  file->stats.records = orig->file->stats.records;
  return false;
}

/*
 * copy table_ref info.
 *
 * @retval:
 *    false if copy successfully, and otherwise true.
 */
bool Index_lookup::pq_copy(JOIN *join, Index_lookup *ref, QEP_TAB *qep_tab) {
  THD *thd = join->thd;
  key_parts = ref->key_parts;
  key_length = ref->key_length;
  key_err = ref->key_err;
  key = ref->key;
  null_rejecting = ref->null_rejecting;
  depend_map = ref->depend_map;
  use_count = ref->use_count;
  disable_cache = ref->disable_cache;

  assert(key_parts && ref->key_buff);
  if (!(key_buff = (uchar *)thd->mem_calloc(ALIGN_SIZE(key_length))) ||
      !(key_buff2 = (uchar *)thd->mem_calloc(ALIGN_SIZE(key_length))) ||
      !(key_copy = (store_key **)thd->mem_calloc(
            (sizeof(store_key *) * (key_parts)))) ||
      !(items = (Item **)thd->mem_calloc(sizeof(Item *) * key_parts)) ||
      !(cond_guards = (bool **)thd->mem_calloc(sizeof(uint *) * key_parts)))
    return true;

  if (ref->null_ref_key) {
    null_ref_key = key_buff + (ref->null_ref_key - ref->key_buff);
  }

  if (key_length > 0) {
    assert(ref->key_buff != nullptr);
    auto cpy_size = ALIGN_SIZE(key_length);
    memcpy_s(key_buff, cpy_size, ref->key_buff, cpy_size);
    memcpy_s(key_buff2, cpy_size, ref->key_buff2, cpy_size);
  }

  assert(ref->items && ref->cond_guards);
  Key_use *keyuse = qep_tab->position()->key;

  const bool for_plan_cache =
      plan_cache::is_clone_for_plan_cache(join->query_block);

  // PLAN_CACHE_PORT in the new implementation, we cannot use the keyinfo
  // below. Indeed, all we have is the original TABLE, and if we read and keep
  // any of its pointer members (which is what get_store_key() does, in
  // essence, by creating Field which has a Field::table), it would be unsafe
  // as these pointers may point to garbage when the current execution ends
  // and the TABLE possibly gets recycled for another query. Fortunately, this
  // keyinfo is actually unnecessary, see get_store_key below.
  const KEY *keyinfo = nullptr;
  uchar *key_buff_tmp = nullptr;
  if (!for_plan_cache) {
    assert(qep_tab->table()->key_info);
    keyinfo = qep_tab->table()->key_info + key;
    key_buff_tmp = key_buff;
  }

  for (uint i = 0; i < key_parts; i++, keyuse++) {
    assert(ref->items[i]);
    if (for_plan_cache) {
      items[i] = clone_if_transient(thd, ref->items[i]);
      if (!items[i]) return true;
      // PLAN_CACHE_PORT use new function clone_if_transient() includes a call
      // to fix_fields
      keyuse->val = items[i];
      keyuse->table_ref = nullptr;  // set later when applying the cached plan.
      // PLAN_CACHE_PORT In the new implementation, we cannot create the
      // store_key at this moment (as we do not have a long-lived TABLE) ; and
      // actually it is not needed, as a store_key is created later in
      // replace_cache_key() when we have the proper TABLE as part of applying
      // the plan.
      cond_guards[i] = ref->cond_guards[i];
    } else {
      items[i] = ref->items[i]->pq_clone(thd, join->query_block);
      if (!items[i]) return true;
      if (!items[i]->fixed) {
        if (items[i]->refix_fields(thd, &items[i])) {
          return true;
        }
      }
      keyuse->val = items[i];
      keyuse->table_ref = qep_tab->table_ref;

      bool maybe_null = keyinfo->key_part[i].null_bit;
      if (ref->key_copy[i] != nullptr) {
        key_copy[i] = get_store_key(
            thd, keyuse->val, keyuse->used_tables, join->const_table_map,
            &keyinfo->key_part[i], key_buff_tmp, maybe_null);
      }
      key_buff_tmp += keyinfo->key_part[i].store_length;
      cond_guards[i] = ref->cond_guards[i];
    }
  }
  return false;
}

/**
   This does a complete clone of a Semijoin_mat_exec of a QEP_TAB. First it
   creates a shallow clone (which is the job of pq_clone()), then it also
   clones related substructures (nested joins, temporary table, etc), and it
   plugs the resulting Semijoin_mat_exec into 'tab'.

   @param thd  THD handler
   @param select New query block
   @param tab New QEP_TAB to host the clone
   @param orig_tab Old QEP_TAB hosting the source for the clone
   @returns true if error.
*/
static bool pq_clone_sj_mat_exec(THD *thd, Query_block *select, QEP_TAB *tab,
                                 QEP_TAB *orig_tab) {
  Semijoin_mat_exec *sj_mat_exec =
      orig_tab->sj_mat_exec()->pq_clone(thd, select);
  if (!sj_mat_exec) return true;
  tab->set_sj_mat_exec(sj_mat_exec);

  auto nj = sj_mat_exec->sj_nest->nested_join;
  for (auto it : nj->sj_inner_exprs) {
    if ((!it->fixed && it->refix_fields(thd, &it)))
      return true; /* purecov: inspected */
  }

  for (auto it : nj->sj_outer_exprs) {
    if ((!it->fixed && it->refix_fields(thd, &it)))
      return true; /* purecov: inspected */
  }

  sj_mat_exec->table_param = Temp_table_param();
  count_field_types(select, &sj_mat_exec->table_param, nj->sj_inner_exprs,
                    false, true);
  sj_mat_exec->table_param.bit_fields_as_long = true;
  char *name = strmake_root(thd->pq_mem_root, orig_tab->table_ref->table_name,
                            orig_tab->table_ref->table_name_length);
  TABLE *table;
  if (!(table = create_tmp_table(
            thd, &sj_mat_exec->table_param, nj->sj_inner_exprs, nullptr,
            true /* distinct */, true /* save_sum_fields */,
            thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS,
            HA_POS_ERROR /* rows_limit */, name)))
    return true; /* purecov: inspected */
  table->file->ha_extra(HA_EXTRA_IGNORE_DUP_KEY);
  table->reginfo.not_exists_optimize =
      orig_tab->table()->reginfo.not_exists_optimize;  // not exists optimized
                                                       // flag(ANTI-JOIN)
  sj_mat_exec->table = table;
  tab->set_table(table);
  tab->join()->sj_tmp_tables.push_back(table);
  tab->join()->sjm_exec_list.push_back(sj_mat_exec);

  return false;
}

/**
   Set QEP_TAB's table_ref. If we can find Table_ref in resolved leaf tables
   through table_no, then we directly set the found one as QEP_TAB's table_ref
   because the table_no is unique. Otherwise, we clone one Table_ref and assign
   it as table_ref.

   @param thd Thread handler.
   @param cloned_join The cloned JOIN object.
   @param tab The QEP_TAB to be filled with table_ref.
   @param orig_tbl The filled table_ref in original plan

   @returns true if clone error, and otherwise false.
*/
static bool pq_set_table_ref(THD *thd, JOIN *cloned_join, QEP_TAB *tab,
                             Table_ref *orig_tbl) {
  Table_ref *tbl = nullptr;
  Query_block *select = cloned_join->query_block;
  for (Table_ref *tr = cloned_join->tables_list; tr; tr = tr->next_leaf) {
    if (tr->tableno() == orig_tbl->tableno()) {
      tbl = tr;
      break;
    }
  }

  if (!tbl) {
    // This case can occur for a Table_ref created for a
    // semijoin-materialized temporary table. Indeed, in pq_dup_select() we
    // create clones of the Table_refs in leaf_tables
    // list. But if semijoin materialization is used, an additional Table_ref
    // exists out of this list, created by the optimization process. Thus every
    // worker needs to clone it when it clones the relevant QEP_TAB, which is
    // done here.
    tbl = new (thd->pq_mem_root) Table_ref();
    if (!tbl) return true;
    if (tbl->pq_copy(thd, orig_tbl)) return true;
    tbl->query_block = select;
    tbl->table = tab->table();
    tbl->table->pos_in_table_list = tbl;
    tab->table_ref = tbl;
    tbl->next_name_resolution_table =
        select->context.first_name_resolution_table;
    select->context.first_name_resolution_table = tbl;
  }

  assert(tbl);
  tab->table_ref = tbl;
  return false;
}

/// Builds a list of query blocks to clone: starting from one root query
/// block, we put this block first in the list, then we put all first-level
/// PQ-suitable correlated subqueries of this query block.
/// @param top_sl     the root query block; it is also added last to the list.
std::vector<Query_block *> list_query_blocks_to_clone(Query_block *top_sl) {
  std::vector<Query_block *> out{top_sl};
  for (auto u = top_sl->first_inner_query_expression(); u;
       u = u->next_query_expression()) {
    if (u->subquery_suite_for_parallel_query() ==
        PQSubqueryExecution::kMultipleByWorkers) {
      out.emplace_back(u->first_query_block());
    }
  }
  return out;
}

static Item *pq_dup_condition(THD *thd, Query_block *select, Item *condition,
                              bool gather) {
  uchar FIX{1}, UNFIX{0};

  assert(condition);
  // We should reset Item_field's fixed status before pq_clone. This
  // is because that item's fixed status may influence Item_ref's
  // pq_clone, which needs to call "find_item_in_list()" to find its
  // referred item. During the process of finding item,
  // Item_field::eq() will determine that two item_fields are equal by
  // their fixed table. However, Item_field in "condition" here is
  // fixed to leader's table, while its cloned equal item in
  // Query_block->fields is referred to gather's table, thus
  // "Item_field::eq()" will return false even if they are equal
  // items.
  if (gather) condition->walk(&Item::set_fixed, enum_walk::PREFIX, &UNFIX);
  Item *cond = condition->pq_clone(thd, select);
  // When cloning finished, we should reset fixed status as FIXED.
  // Otherwise, leader will trigger the assert error when item
  // included in condition (e.g., JOIN condition contains the item in
  // select list) sends the result (i.e., Item::val_*()) to client.
  if (gather) condition->walk(&Item::set_fixed, enum_walk::PREFIX, &FIX);
  if (!cond || DBUG_EVALUATE_IF("pq_dup_tabs_error0", true, false))
    return nullptr;

  // resolve per-table conditions
  if (!cond->fixed && cond->refix_fields(thd, &cond)) return nullptr;
  if (gather) cond->walk(&Item::set_fixed, enum_walk::PREFIX, &UNFIX);
  return cond;
}

/**
 * duplicate QEP_TABs of a JOIN
 *
 * @param join     target top JOIN
 * @param top_orig original top JOIN
 * @param gather   true if working for the PQ leader
 *
 */
bool pq_dup_tabs(JOIN *join, JOIN *top_orig, bool gather) {
  auto orig = top_orig;
  assert(orig == orig->query_block->join);
  auto thd = join->thd;

  // this function is used by PQ and by plan caching, and plan caching does
  // not care about subqueries, so we have to make a distinction:
  const bool for_plan_cache =
      plan_cache::is_clone_for_plan_cache(orig->query_block);
  const auto query_blocks_to_clone =
      for_plan_cache ? std::vector<Query_block *>{orig->query_block}
                     : list_query_blocks_to_clone(orig->query_block);

  const auto mem_root =
      for_plan_cache ? join->thd->mem_root : join->thd->pq_mem_root;

  auto exit_guard =
      create_scope_guard([&]() { join->thd->clone_phase = THD::PQ_UNKONWN; });

  for (const auto query_block : query_blocks_to_clone) {
    if (!for_plan_cache) {
      orig = query_block->join;
      join = query_block->pq_last_clone()->join;
      assert(orig && join == orig->query_block->pq_last_clone()->join);
    }

    if (!join) {
      // At this stage, query block clones normally have a JOIN. But there are
      // corner cases. It can happen that refix_fields() for the top query's
      // clone does not go as it did for the top query's original, leading to
      // more always-false-condition simplification, leading to the subquery's
      // clone disappearance (Item_subselect::cleanup_after_removal). In that
      // case, there may be no JOIN anymore in this clone. We thus have
      // inconsistency between the original and clone of the subquery, and
      // cannot continue.
      return true;
    }
    SJ_TMP_TABLE *sjtbl = nullptr, *orig_sjtbl = nullptr;

    join->const_tables = orig->const_tables;
    join->primary_tables = orig->primary_tables;
    Query_block *select = join->query_block;
    assert(thd == join->thd);
    Switch_for_resolution_of_query_block switch_resol(
        thd->lex, select, query_block != top_orig->query_block);

    join->thd->clone_phase = THD::PQ_OPTIMIZE;
    // It can happen that orig->qep_tab==nullptr if this is a correlated
    // subquery executed by a worker and having a zero_result_cause. In this
    // case, the clone shouldn't get QEP_TABs either.
    if (orig->qep_tab) {
      // create qep_tab
      QEP_shared *qs = new (mem_root) QEP_shared[join->tables + 1];
      if (!qs) return true;
      join->qep_tab0 = new (mem_root) QEP_TAB[join->tables + 1];
      if (!join->qep_tab0) return true;
      join->qep_tab = join->qep_tab0;

      for (uint i = 0; i < join->tables; i++) {
        join->qep_tab[i].set_qs(&qs[i]);
        join->qep_tab[i].set_join(join);
        join->qep_tab[i].set_idx(i);
      }

      for (uint i = 0; i < join->tables; i++) {
        QEP_TAB *tab = &join->qep_tab[i];
        QEP_TAB *orig_tab = &orig->qep_tab[i];

        Table_ref *orig_tbl = orig_tab->table_ref;
        if (!orig_tbl) continue;
        if (tab->pq_copy(join->thd, orig_tab)) return true;

        // set sj-mat-exec structure
        if (orig_tab->sj_mat_exec()) {
          if (pq_clone_sj_mat_exec(join->thd, select, tab, orig_tab))
            return true;
        }

        // setup physic table object
        assert(orig_tbl->table == orig_tab->table());
        if (!for_plan_cache) {
          if (pq_set_table_ref(join->thd, join, tab, orig_tbl)) return true;
          tab->set_table(tab->table_ref->table);
          assert(tab->table() != orig_tbl->table);
          memcpy(tab->table()->record[0], orig_tbl->table->record[0],
                 tab->table()->s->reclength);
          if (tab->table()->pq_copy(join->thd, select, orig_tab->table()))
            return true;
          // In many cases, PQ won't set table's read_set by item::fix_field().
          // For example,
          // scenario 1: For BKA & outer join, fields would be
          // treated as index access fields, and it doesn't exists in fields
          // list, where/having condition, group by or order by fields.
          // scenario 2: For the following statements:
          // "SELECT * FROM t2 LEFT JOIN (SELECT * FROM t1 WHERE t1.pk IN
          // (SELECT pk FROM t3 WHERE t3.pk > 2)) t13 on (t13.pk = t2.pk)" PQ
          // won't set t3's read_set with t3.pk field because t3.pk is not
          // included in fields list.
          // There may be a lot of these scenarios. So here need copy original
          // table's read_set directly.
          bitmap_copy(tab->table()->read_set, orig_tab->table()->read_set);
          bitmap_copy(&tab->table()->read_set_internal,
                      &orig_tab->table()->read_set_internal);
        }
#ifndef NDEBUG
        else {
          assert(tab->table() == nullptr && tab->table_ref == nullptr);
        }
#endif
        if (orig_tab->ref().key_parts &&  // ref access is used
            tab->ref().pq_copy(join, &orig_tab->ref(), tab))
          return true;
        // set read key
        tab->set_keyread_optim();
      }

      for (uint i = 0; i < join->tables; i++) {
        QEP_TAB *tab = &join->qep_tab[i];
        QEP_TAB *orig_tab = &orig->qep_tab[i];
        Item *condition = orig_tab->condition();

        if (for_plan_cache) {
          if (condition && orig_tab->table_ref) {  // not a group-by tmp table
            Item *cond = clone_if_transient(join->thd, condition);
            if (!cond || DBUG_EVALUATE_IF("pq_dup_tabs_for_plan_cache_error0",
                                          true, false))
              return true;
            tab->set_condition(cond);
            tab->set_condition_optim();
          }
          // PLAN_CACHE_PORT:  No cloning of range_scan is actually needed, as
          // plan cache almost does not use this clone (only computes a list of
          // keys, which we can do from the original object instead).

          // PLAN_CACHE_PORT: as we do not clone Item_ref, no fiddling with
          // 'fixed' member is needed unlike what is done in pq_dup_condition().
          // We do call the new function clone_if_transient(), but it does not
          // make clones of Item_ref, it re-uses Item_ref (always a permanent
          // Item), so does need to resolve a clone, does not need to call
          // find_item_in_list().
        } else {
          if (orig_tab->range_scan()) {
            AccessPath *range_scan = CopyRangeScanAccessPath(
                join->thd, orig_tab->range_scan(), tab->table());
            if (!range_scan) return true;
            tab->set_range_scan(range_scan);
          }

          /**
             Clone conditions in qep_tab. The attached condition will form
            FilterIterator above these tables. However, if this table is a tmp
            table for group-by, we need not clone the condition as it will
            anyway be built when generating this tmp table in
            make_tmp_tables_info().
          */
          if (condition && orig_tab->table_ref) {  // not a group-by tmp table
            // We should reset Item_field's fixed status before pq_clone. This
            // is because that item's fixed status may influence Item_ref's
            // pq_clone, which needs to call "find_item_in_list()" to find its
            // referred item. During the process of finding item,
            // Item_field::eq() will determine that two item_fields are equal by
            // their fixed table. However, Item_field in "condition" here is
            // fixed to leader's table, while its cloned equal item in
            // Query_block->fields is referred to gather's table, thus
            // "Item_field::eq()" will return false even if they are equal
            // items.
            Item *cond = pq_dup_condition(join->thd, select, condition, gather);
            if (!cond) return true;
            tab->set_condition(cond);
            tab->set_condition_optim();
          }
        }

        /*
          copy the description of any SJ_TMP_TABLE used for Duplicate
          Weedout (DW) semi-join strategy.

          Each worker will get its own DW tmp table, thus eliminating duplicates
          among the rows it processes. Duplicates are NOT eliminated accross
          workers. That is correct, as long as no semi-join-inner table is used
          as divided table. Proof: all rows of worker 1 (W1) are different from
          rows of W2, as they include different rows of the divided table which
          are not considered duplicates (the table not being inner to a
          semi-join). So there cannot be duplicates between W1 and W2. There can
          be duplicates generated in W1 by joining with the semi-join-inner
          table but they are eliminated by W1's tmp table.
        */
        if (orig_tab->flush_weedout_table)  // entering a dups-weedout range
        {
          /*
            Make tab->flush_weedout_table point to a copy of
            orig->flush_weedout_table, with its own temporary table. We'll also
            store this copy in 'sjtbl', so it can be picked up by the last
            QEP_TAB in the range.

            This copying cannot be done earlier, like in QEP_TAB::pq_copy(), as
            we need all new QEP_TABs to be set up before we can define the
            temporary table.
          */
          assert(!sjtbl);  // no copy made yet
          orig_sjtbl = orig_tab->flush_weedout_table;
          std::array<SJ_TMP_TABLE_TAB, MAX_TABLES> sjtabs;
          // Transform the list of orig's QEP_TABs used in the original
          // temporary table, into a list of corresponding new QEP_TABs:
          auto last_sjtab = std::transform(
              orig_sjtbl->tabs, orig_sjtbl->tabs_end, sjtabs.begin(),
              [join, orig](auto &orig_sj_tab) {
                return SJ_TMP_TABLE_TAB{
                    join->qep_tab + (orig_sj_tab.qep_tab - orig->qep_tab), 0, 0,
                    0};
              });

          sjtbl =
              create_sj_tmp_table(join->thd, join, sjtabs.begin(), last_sjtab);
          if (sjtbl == nullptr) {
            return true;
          }
          tab->flush_weedout_table = sjtbl;
        }
        if (orig_tab->check_weed_out_table)  // last table in range
        {
          // should still point at orig's, and the copy should already exist
          assert((orig_sjtbl == orig_tab->check_weed_out_table) && sjtbl);
          tab->check_weed_out_table = sjtbl;
          sjtbl = orig_sjtbl = nullptr;  // leaving range, use the copy no more
        }
      }
      if (!for_plan_cache) {
        // Higher up in this function, refix_fields is called and could have
        // altered the information in tab->table()->record[0]: in the case of a
        // RIGHT JOIN with an impossible ON condition and an IN clause in the
        // WHERE, refix_fields call item_func_in::resolve_type which in turns
        // calls convert_constant_item: it is going to successively save in  the
        // field the values in the IN clause (without resetting it to its
        // original value as save_field_value is false in our case) and set the
        // field to not null even though it should be NULL because of the
        // impossible ON condition. With the memcpy, we simply restore the
        // information in tab->table()->record[0] to what it should be (nullity
        // and value stored in field).
        for (uint i = 0; i < join->tables; i++) {
          QEP_TAB *tab = &join->qep_tab[i];
          QEP_TAB *orig_tab = &orig->qep_tab[i];

          Table_ref *orig_tbl = orig_tab->table_ref;

          if (!orig_tbl) continue;
          memcpy(tab->table()->record[0], orig_tbl->table->record[0],
                 tab->table()->s->reclength);
        }
      }
    }  // end of if(orig->qep_tab)
  }    // end of for()
  return false;
}

/*
 * clone order structure
 */
ORDER *pq_dup_order(THD *thd, Query_block *select, ORDER *orig) {
  const bool for_plan_cache = plan_cache::is_clone_for_plan_cache(select);
  const auto mem_root = for_plan_cache ? thd->mem_root : thd->pq_mem_root;
  ORDER *order = new (mem_root) ORDER();
  if (!order) return nullptr;
  order->next = nullptr;
  if (for_plan_cache) {
    // PLAN_CACHE_PORT By adding a small restriction (see
    // hidden_items_from_optimization in sql/sql_plan_cache.cc file),
    // we ensure that orig->item points into the Query_block's ref_item_array,
    // and that such array contains only permanent items. This makes it
    // much simpler to clone an Item used by an ORDER object: we do not need
    // to clone it: it's a permanent Item, at a permanent place in the array.
    order->item_initial = orig->item_initial;
    order->item = orig->item;
#ifndef NDEBUG
    // check they're permanent indeed
    auto &transient_items =
        thd->lex->current_query_block()
            ->cached_plan->plan_cache_exec_context->transient_items;
    assert(transient_items.count(order->item_initial) == 0);
    assert(transient_items.count(*order->item) == 0);
#endif
  } else {
    if (orig->item == &orig->item_initial) {
      // storage for initial item
      order->item_initial = orig->item_initial->pq_clone(thd, select);
      if (!order->item_initial) return nullptr;
      order->item = &order->item_initial;
    } else {
      order->item = (Item **)mem_root->Alloc(sizeof(Item *));
      if (!order->item) return nullptr;
      (*order->item) = (*orig->item)->pq_clone(thd, select);
      if (!(*order->item)) return nullptr;
      order->item_initial = *order->item;
    }
  }
  order->direction = orig->direction;
  order->in_field_list = orig->in_field_list;
  order->used_alias = orig->used_alias;
  order->field_in_tmp_table = nullptr;
  order->buff = nullptr;
  order->used = 0;
  order->depend_map = 0;
  order->is_explicit = orig->is_explicit;

  return order;
}

/**
  clone the Query_block group/order list, using orig saved group/order list
  and orig group/order list.

  @param[in]  thd                  cloned THD handler
  @param[in]  select               cloned query block
  @param[in]  orig_saved_list_ptrs orig saved group/order list
  @param[in]  orig_order_list      orig group/order list
  @param[out] select_order_list    cloned group/order list
*/
static inline bool pq_dup_order_list(
    THD *thd, Query_block *select, const Group_list_ptrs *orig_saved_list_ptrs,
    const SQL_I_List<ORDER> &orig_order_list,
    SQL_I_List<ORDER> &select_order_list) {
  auto add_dup_order_to_list =
      [thd, select, &select_order_list](const ORDER *orig_order) -> bool {
    auto order_new = pq_dup_order(thd, select, const_cast<ORDER *>(orig_order));
    if (!order_new) return true;
    select_order_list.link_in_list(order_new, &order_new->next);
    return false;
  };

  if (!orig_saved_list_ptrs) {
    for (const auto &order : orig_order_list) {
      if (add_dup_order_to_list(&order)) return true;
    }
  } else {
    for (const auto order : *orig_saved_list_ptrs) {
      if (add_dup_order_to_list(order)) return true;
    }
  }
  return false;
}

/// check the cloned item in group_list and order_list is resolved
/// to the right position in base items.
bool check_resolved_order_item(
    const SQL_I_List<ORDER> &list, const Ref_item_array &base_items,
    const mem_root_unordered_map<uint, uint> &map_order) {
  if (!list.size()) {
    assert(map_order.size() == 0);
    return false;
  }

  uint first = 0, second = 0;
  for (ORDER *tmp = list.first; tmp; tmp = tmp->next) {
    second = 0;
    for (uint i = 0; i < base_items.size(); i++) {
      if ((*tmp->item) == base_items[i]) break;
      second++;
    }
    if (second == base_items.size()) return true;

    // check the matched items
    auto iter = map_order.find(first);
    if (iter != map_order.end()) {
      bool need_alias_item = true;
      if (second == iter->second ||
          (items_equal_after_resolve((*tmp->item), base_items[second],
                                     need_alias_item) &&
           !need_alias_item)) {
        first++;
        continue;
      }
    }

    return true;
  }

  return false;
}

/// @param top_orig top original query block to clone
/// @returns a new query block, or nullptr if error.
static Query_block *pq_dup_select(THD *thd, Query_block *top_orig) {
  /*
    If we come in this function for a subquery, this subquery should be the
    root of "an independent PQ phase": something which creates workers for
    itself. A non-correlated derived table is such thing. A non-correlated
    scalar subquery is. A correlated subquery is not.
  */
  assert(top_orig->master_query_expression()
             ->subquery_suite_for_parallel_query() !=
         PQSubqueryExecution::kMultipleByWorkers);
  /*
    This is the root query block of an independent PQ phase, we're going to
    duplicate the tree of UNITs. All members of this tree share the same THD.
  */
  LEX *lex = new (thd->pq_mem_root) LEX();
  if (!lex) return nullptr;
  lex->reset();
  lex->result = top_orig->parent_lex->result;
  lex->sql_command = top_orig->parent_lex->sql_command;
  lex->set_ignore(top_orig->parent_lex->is_ignore());
  lex->is_explain_analyze = top_orig->parent_lex->is_explain_analyze;
  thd->lex = lex;
  lex->thd = thd;
  thd->query_plan.set_query_plan(SQLCOM_SELECT, lex, false);
  Query_block *top_select = lex->new_query(nullptr);  // new top query
  if (!top_select || DBUG_EVALUATE_IF("dup_select_abort1", true, false))
    return nullptr;
  top_orig->pq_link_clone(top_select);

  lex->set_current_query_block(top_select);
  lex->unit = top_select->master_query_expression();
  thd->lex->query_block = top_select;

  // We now have a Query_expression and a Query_block. Let's build the rest of
  // the tree of underlying Query_expressions.
  auto query_blocks_to_clone = list_query_blocks_to_clone(top_orig);

  for (auto orig : query_blocks_to_clone) {
    if (orig == top_orig) continue;  // has already been handled above
    Query_expression *orig_unit = orig->master_query_expression();
    assert(orig_unit);
    assert(orig_unit->subquery_suite_for_parallel_query() ==
           PQSubqueryExecution::kMultipleByWorkers);
    assert(orig_unit->first_query_block() == orig && !orig->next_query_block());
    assert(orig->m_suite_for_pq);
    // clone this subquery as a part of the parent query. A subquery
    // which is at the root of an independent PQ phase is NOT cloned here, it is
    // handled by the separate phase.
    assert(top_select->parsing_place == CTX_NONE);
    // a parsing_place is needed as it influences the value of outer_context
    // used by the new subquery.
    top_select->parsing_place = orig_unit->place();
    auto sub_select = lex->new_query(top_select);
    top_select->parsing_place = CTX_NONE;
    if (!sub_select) return nullptr;
    orig->pq_link_clone(sub_select);
    // set_context() in LEX::new_query() has pushed a new name resolution
    // context to the context stack, making it the current context (as
    // returned by LEX::current_context()). As we move to creating next query
    // blocks, we must pop it, to make the context of top_select the current
    // one, again.
    lex->pop_context();
  }

  thd->mark_used_columns = MARK_COLUMNS_READ;
  thd->clone_phase = THD::PQ_PREPARE;

  for (auto orig : query_blocks_to_clone) {
    Query_block *select = orig->pq_last_clone();
    Switch_for_resolution_of_query_block switch_resol(lex, select,
                                                      orig != top_orig);

    select->select_number = orig->select_number;
    select->with_sum_func = orig->with_sum_func;
    select->n_child_sum_items = orig->n_child_sum_items;
    select->n_sum_items = orig->n_sum_items;
    select->select_n_having_items = orig->select_n_having_items;
    select->select_n_where_fields = orig->select_n_where_fields;
    select->m_active_options = orig->m_active_options;
    select->parallel_exec = orig->parallel_exec;
    select->hidden_group_field_count = orig->hidden_group_field_count;
    select->hidden_order_field_count = orig->hidden_order_field_count;
    select->check_map_group_to_base = orig->check_map_group_to_base;
    select->check_map_order_to_base = orig->check_map_order_to_base;
    select->opt_hints_qb = orig->opt_hints_qb;
    select->nest_level = orig->nest_level;
    select->uncacheable = orig->uncacheable;
    select->master_query_expression()->uncacheable =
        orig->master_query_expression()->uncacheable;

    // completely cleaned for PQ worker
    select->master_query_expression()->cleaned = Query_expression::UC_CLEAN;

    // phase 1. clone tables and open/lock them
    for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
         tbl_list = tbl_list->next_leaf) {
      // We want to push everything to worker, mark const table non-const
      tbl_list->table->const_table = false;
      LEX_CSTRING *db_name =
          new (thd->pq_mem_root) LEX_CSTRING{tbl_list->db, tbl_list->db_length};
      LEX_CSTRING *tbl_name = new (thd->pq_mem_root)
          LEX_CSTRING{tbl_list->table_name, tbl_list->table_name_length};
      Table_ident *tbl_ident =
          new (thd->pq_mem_root) Table_ident(*db_name, *tbl_name);
      tbl_ident->sel = tbl_list->is_view_or_derived()
                           ? tbl_list->derived_query_expression()
                           : nullptr;
      char *db_alias = strmake_root(thd->pq_mem_root, tbl_list->alias,
                                    strlen(tbl_list->alias));
      if (!db_name || !tbl_name || !tbl_ident || !db_alias) return nullptr;
      auto new_tbl_list =
          select->add_table_to_list(thd, tbl_ident, db_alias, 0);
      if (!new_tbl_list) return nullptr;
      new_tbl_list->opt_hints_qb = tbl_list->opt_hints_qb;
      new_tbl_list->opt_hints_table = tbl_list->opt_hints_table;
    }

    assert(select->context.query_block == select);
    select->context.table_list = select->context.first_name_resolution_table =
        select->m_table_list.first;
  }

  // As a parallel query involves a leader and workers, which acquire and
  // release locks successively and in different threads, deadlocks may occur.
  // When the worker starts, the leader already has locks on tables involved
  // by the query, which it can lend to the worker. But the worker will also
  // need schema locks (which the leader has acquired then released early),
  // DD-system-table locks (which the leader has not acquired if they were in
  // cache, or has acquired then released early), so lock-lending from leader
  // may not always be sufficient. We set a timeout to accomodate this. The
  // timeout value is not too short, so that a highly loaded system doesn't
  // trigger it; moreover we do not expect deadlocks to be frequent in real
  // life - they should occur only when a concurrent DDL starts after the
  // leader, takes _exclusive_ locks on the schema or DD-system-tables, thus
  // blocking the worker. See issue 559.
  {
    auto save_timeout = thd->variables.lock_wait_timeout;
    thd->variables.lock_wait_timeout = 5;  // seconds
    auto restore_timeout = create_scope_guard([thd, save_timeout]() {
      thd->variables.lock_wait_timeout = save_timeout;
    });

    // phase 1. open tables and lock them
    if (open_tables_for_query(thd, thd->lex->query_tables, 0)) {
      return nullptr;
    }

    for (auto orig : query_blocks_to_clone) {
      Query_block *select = orig->pq_last_clone();
      Switch_for_resolution_of_query_block switch_resol(lex, select,
                                                        orig != top_orig);
      if (select->setup_tables(thd, select->get_table_list(), false)) {
        return nullptr;
      }
    }

    if (lock_tables(thd, thd->lex->query_tables, thd->lex->table_count, 0)) {
      return nullptr;
    }
  }  // timeout variable is restored here as guard goes out of scope

  for (auto orig : query_blocks_to_clone) {
    Query_block *select = orig->pq_last_clone();
    Switch_for_resolution_of_query_block switch_resol(lex, select,
                                                      orig != top_orig);

    assert(select->m_current_table_nest->empty());
    Table_ref *it = select->leaf_tables, *last_it = nullptr;
    for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
         tbl_list = tbl_list->next_leaf) {
      if (!it || it->pq_copy(thd, tbl_list)) {
        return nullptr;
      }

      if (!it->table) {
        Query_expression *derived = it->derived_query_expression();
        assert(derived);
        ulonglong create_options =
            derived->first_query_block()->active_options() |
            TMP_TABLE_ALL_COLUMNS;

        if (tbl_list->derived_column_names()) {
          swap_column_names_of_unit_and_tmp_table(
              *derived->get_unit_column_types(),
              *tbl_list->derived_column_names());
        }

        bool is_distinct = derived->can_materialize_directly_into_result() &&
                           derived->has_top_level_distinct();
        Temp_table_param tmp_table_param = Temp_table_param();
        count_field_types(orig, &tmp_table_param,
                          *derived->get_unit_column_types(), false, true);
        tmp_table_param.skip_create_table = true;
        tmp_table_param.bit_fields_as_long = false;
        /*
          The original derived tmp table has been created by
          Query_result_union::create_result_table(), which may have forced a
          disk-based table. Imitate its decision.
        */
        if (tbl_list->table->hash_field != nullptr)
          tmp_table_param.force_hash_field_for_unique = true;
        /*
          Likewise, if the original tmp table is a recursive CTE, the same
          function has disabled PK creation. If that table has no PK, our copy
          mustn't have any.
        */
        if (derived != nullptr) {
          Query_term_set_op *op = nullptr;
          if (!derived->is_simple() &&
              derived->can_materialize_directly_into_result()) {
            op = derived->set_operation();
          }
          if (op && (op->term_type() == QT_INTERSECT ||
                     op->term_type() == QT_EXCEPT)) {
            tmp_table_param.m_operation = op->term_type() == QT_INTERSECT
                                              ? Temp_table_param::TTP_INTERSECT
                                              : Temp_table_param::TTP_EXCEPT;
            tmp_table_param.m_last_operation_is_distinct =
                op->m_last_distinct > 0;
          }
        }
        if (tbl_list->table->s->primary_key == MAX_KEY)
          tmp_table_param.can_use_pk_for_unique = false;

        bool force_disk_table = tbl_list->table->s->db_type() == innodb_hton;

        /** restore derived types property */
        reset_avg_property(*derived->get_unit_column_types());
        mem_root_deque<Item *> visible_fields(thd->pq_mem_root);
        for (Item *item : VisibleFields(*derived->get_unit_column_types())) {
          visible_fields.push_back(item);
          item->swap_pq_derived_info();
        }
        TABLE *table = create_tmp_table(
            thd, &tmp_table_param, visible_fields, nullptr, is_distinct, true,
            create_options, HA_POS_ERROR, it->alias, force_disk_table);

        if (tbl_list->derived_column_names()) {  // swap back
          swap_column_names_of_unit_and_tmp_table(
              *derived->get_unit_column_types(),
              *tbl_list->derived_column_names());
        }
        if (!table) {
          return nullptr;
        }
        // Put table in 'it' (Query_block->leaf_tables) so
        // that the table is closed and freed during the Query_block clean up
        table->pq_shared_table = true;
        table->pos_in_table_list = it;
        it->table = table;
        // (3): Arises in derived table with ROLLUP and a window function like
        // RANK(). If the Item_rank (from the window function) has
        // maybe_null=false before optimization,
        // Query_block::resolve_rollup_wfs, called during optimization of the
        // derived table, will set it to true. This will lead to a larger number
        // of null_fields in the pq tmp table (which uses items
        // post-optimisation) than in the matching non pq tmp table (which uses
        // items pre-optimisation) which in turns leads to PQ giving wrong
        // results.
        if (table->s->fields != tbl_list->table->s->fields ||
            table->s->reclength != tbl_list->table->s->reclength ||
            table->s->null_fields != tbl_list->table->s->null_fields) {  //(3)
          return nullptr;
        }

        // Verify equality of 1) if there is a PK 2) a hash field 3) the engine
        assert(tbl_list->table->s->primary_key == table->s->primary_key);
        assert((bool)(tbl_list->table->hash_field) ==
               (bool)(table->hash_field));
        assert(tbl_list->table->file->ht == table->file->ht);

        for (auto item : visible_fields) {
          item->swap_pq_derived_info();
        }

        auto model = tbl_list->table->s;
        assert(table->s->keys <= 1);  // create_tmp_table() creates 0 or 1 key
        if (table->s->keys <
            model->keys) {  // the clone has less keys than the model,
          // make room and build each missing key based on the model.
          if (table->alloc_tmp_keys(model->keys, model->key_parts, true))
            return nullptr;
          for (auto keyno = table->s->keys; keyno < model->keys; ++keyno) {
            auto &key = model->key_info[keyno];
            Field_map map;
            for (auto partno = 0U; partno < key.user_defined_key_parts;
                 ++partno)
              map.set_bit(key.key_part[partno].field->field_index());
            if (!table->add_tmp_key(&map, false, true,
                                    model->key_info[keyno].name))
              return nullptr;
          }
        }
        // During EXPLAIN, copy the Common_table_expr for CTE Materialize to
        // avoid losing the CTE keyword when printing the Materialize operator.
        if (orig->join->thd->lex->is_explain() &&
            orig->join->thd->is_pq_leader() && orig == top_orig &&
            tbl_list->common_table_expr()) {
          Common_table_expr *common_table_expr =
              new (thd->pq_mem_root) Common_table_expr(thd->pq_mem_root);
          common_table_expr->recursive =
              tbl_list->common_table_expr()->recursive;
          common_table_expr->name = tbl_list->common_table_expr()->name;
          common_table_expr->tmp_tables.push_back(it);
          it->set_common_table_expr(common_table_expr);
        }
      }
      if (it->table->file) {
        // reset do_parallel_scan in case of open table from table_cache
        it->table->file->do_parallel_scan = false;
      }
      if (tbl_list->table->is_nullable()) it->table->set_nullable();

      it->table->reginfo.not_exists_optimize =
          tbl_list->table->reginfo.not_exists_optimize;
      it->set_tableno(tbl_list->tableno());
      last_it = it;
      it = it->next_leaf;
      last_it->next_name_resolution_table = it;
    }
    select->context.last_name_resolution_table = last_it;

    uint n_elements = orig->base_ref_items.size();
    Item **array = static_cast<Item **>(
        thd->pq_mem_root->Alloc(sizeof(Item *) * n_elements));
    if (array == nullptr ||
        DBUG_EVALUATE_IF("dup_select_abort2", true, false)) {
      return nullptr;
    }
    select->base_ref_items = Ref_item_array(array, n_elements);
    // phase 2. clone select fields list. We clone all items (including
    // visible and hidden) in fields. During this process, we should
    // maintain the order relationship between "fields" and "base_ref_items".
    // Specifically, we first push_back "item in select list" into "fields", and
    // then push_front the items in "group-by", "order-by", and "HAVING" that is
    // not in "select_list" into fields. Consequently, the order of item in
    // "fields" is [H1, H2, .., Hm | V1, V2, .. Vn], where "Hi" is the i-th
    // hidden item and "Vj" is the j-th visible item.
    //
    // At the same time, these items are pushed into "base_ref_items" according
    // to the order "[V1, V2, .., Vn | Hm, .., H2, H1]". That is, if item is the
    // k-th visible item, its position in base_ref_items is k. On the contrary,
    // it is hidden item and its position is n + (m - k + 1) = size(fields) - k
    // + 1.
    //
    // For example, consider the query "select sum(a) from t1 group by c, d
    // having c/d > 1", we have "fields = [d, c, | sum(a)]", and "base_ref_item
    // = [sum(a), c, d]".

    // clone and fill visible items with push_back
    uint rank_order = 0;
    for (Item *item : orig->fields) {
      if (!item->hidden) {
        auto new_item = item->pq_clone(thd, select);
        if (!new_item) return nullptr;
        select->fields.push_back(new_item);
        select->base_ref_items[rank_order] = new_item;
        rank_order++;
      }
    }

    // clone and fill hidden items with push_front
    rank_order = 0;
    uint hidden_item_start_position = CountVisibleFields(orig->fields);
    for (auto iter = orig->fields.rbegin(); iter != orig->fields.rend();
         ++iter) {
      Item *item = *iter;
      if (item->hidden) {
        auto new_item = item->pq_clone(thd, select);
        if (!new_item) return nullptr;
        select->fields.push_front(new_item);
        select->base_ref_items[hidden_item_start_position + rank_order] =
            new_item;
        rank_order++;
      }
    }

    // phase 3. duplicate group list. For template query_block, we use
    // leader's saved_group_list_ptrs to get access to original
    // group_list, and then copy it to template. For worker's query_block,
    // we directly use template's info to generate its group_list.
    if (pq_dup_order_list(thd, select, orig->saved_group_list_ptrs,
                          orig->group_list, select->group_list)) {
      return nullptr;
    }

    if (pq_dup_order_list(thd, select, orig->saved_order_list_ptrs,
                          orig->order_list, select->order_list)) {
      return nullptr;
    }

    /** mainly used for optimized_group_by */
    if (select->group_list.elements) {
      if (select->save_order_properties(thd, &select->group_list,
                                        &select->saved_group_list_ptrs))
        return nullptr;
    }

    if (select->order_list.elements) {
      if (select->save_order_properties(thd, &select->order_list,
                                        &select->saved_order_list_ptrs))
        return nullptr;
    }

    // phase 5. duplicate where cond
    if (orig->where_cond()) {
      Item *used_cond =
          orig->saved_where_cond ? orig->saved_where_cond : orig->where_cond();
      if (auto new_cond = used_cond->pq_clone(thd, select))
        select->set_where_cond(new_cond);
      else
        return nullptr;
    } else
      select->set_where_cond(nullptr);

    // clean the resolved flag
    for (auto item : select->fields) {
      item->is_resolved_hidden = false;
    }

    // phase 6. duplicate having cond
    if (orig->having_cond()) {
      Item *used_having = orig->saved_having_cond ? orig->saved_having_cond
                                                  : orig->having_cond();
      if (auto new_cond = used_having->pq_clone(thd, select))
        select->set_having_cond(new_cond);
      else
        return nullptr;
    } else
      select->set_having_cond(nullptr);

  }  // end of for()

  // phase 7: allow local set functions in HAVING and ORDER BY
  lex->allow_sum_func |= (nesting_map)1 << top_select->nest_level;

  top_select->set_query_result(lex->result);
  return top_select;
}

bool pq_replace_base_item(Query_block *select) {
  // replace item in fields, as they may contain aggregate (or ref) Item.
  // These item should refer to the same base item in ref_base_item.
  // E.g., "select sum(a)/count(*) from t1".
  for (Item *item : select->fields) {
    if (item->walk(&Item::replace_with_base_item, enum_walk::PREFIX,
                   (uchar *)select))
      return true;
  }

  // clean the resolved flag
  for (auto item : select->fields) {
    item->is_resolved_hidden = false;
  }

  // replace item in having
  if (select->having_cond() && (select->having_cond()->has_aggregation() ||
                                select->having_cond()->has_grouping_func())) {
    Item *having_item = select->having_cond();
    if (having_item->walk(&Item::replace_with_base_item, enum_walk::PREFIX,
                          (uchar *)select)) {
      return true;
    }
  }

  return false;
}

/**
 * resolve query block, setup tables list, fields list, group list\order list
 *
 * @select: query block
 *
 */
static bool pq_select_prepare(THD *thd, Query_block *select,
                              mem_root_deque<Item *> &orig_fields) {
  // replace with base ref items
  if (pq_replace_base_item(select)) {
    return true;
  }

  // setup all fields
  thd->mark_used_columns = MARK_COLUMNS_READ;
  select->resolve_place = Query_block::RESOLVE_SELECT_LIST;
  // select->fields is setup in sql_parse and setup_fields.
  // pq_query_block->fields is cloned from orig_fields and orig_fields
  // has been updated in Query_block::prepare function.
  // So split_sum_funcs/want_privilege/column_update set to false.
  if (setup_fields(thd, /*want_privilege=*/false, /*allow_sum_func=*/true,
                   /*split_sum_funcs=*/false, /*column_update=*/false, nullptr,
                   select->get_fields_list(), select->base_ref_items, true))
    return true;
  select->resolve_place = Query_block::RESOLVE_NONE;

  // clean the resolved flag for having
  for (auto item : select->fields) {
    item->is_resolved_hidden = false;
    // setup_fields() may have created an unsupported Item_outer_ref. Even
    // though the initial call to setup_fields() has not created any (we would
    // have caught it with pq_not_support_ref() and not enabled PQ), it
    // happens that this second call behaves differently. Example:
    // SELECT (SELECT ... HAVING tx.c0) FROM tx GROUP BY tx.c0;
    // Initially, in the subquery, as c0 is in HAVING it is Item_ref
    // (see PTI_simple_ident_q_3d::itemize()).
    // Then Item_ref::refix_fields() replaces it with Item_field.
    // Then PQ copies this Item_field to make another Item_field, and
    // Item_field::refix_fields() replaces it (in fix_outer_field()) with
    // Item_outer_ref (unsupported). Observe
    // Item_ref A -> Item_field B (originally)
    // vs
    // Item_field copy-of-B -> Item_outer_ref C (in PQ)
    // So we cannot continue.
    if (item->type() == Item::REF_ITEM) {
      Item_ref *item_ref = down_cast<Item_ref *>(item);
      if (pq_not_support_ref(item_ref)) {
        return true;
      }
    }
  }

  // setup GROUP BY clause
  if (select->group_list.elements && select->setup_group(thd)) return true;

  // setup where conditions
  if (select->setup_conds(thd)) return true;

  // setup ORDER BY clause
  if (select->order_list.elements &&
      setup_order(thd, select->base_ref_items, select->get_table_list(),
                  select->get_fields_list(), select->order_list.first))
    return true;

  // copy having condition
  if (select->having_cond()) {
    assert(select->having_cond()->is_bool_func());
    thd->where = "having clause";
    select->having_fix_field = true;
    select->resolve_place = Query_block::RESOLVE_HAVING;
    if (!select->having_cond()->fixed &&
        (select->having_cond()->refix_fields(thd, select->having_cond_ref()) ||
         select->having_cond()->check_cols(1)))
      return true;

    select->having_fix_field = false;
    select->resolve_place = Query_block::RESOLVE_NONE;
  }

  // setup final order-by
  if (select->order_list.elements && select->setup_order_final(thd))
    return true;

  // check item's property and resolved group/order item: we record the map
  // {order_item -> base_item} in check_map_group(order)_to_base. When we have
  // resolved group (order) item, we check the resolved order whether equals to
  // the recorded order. If not, the resolved process in parallel query may be
  // error and we directly return true and report PQ error.
  if (select->fields.size() != orig_fields.size() ||
      (select->check_map_group_to_base &&
       check_resolved_order_item(select->group_list, select->base_ref_items,
                                 *select->check_map_group_to_base)) ||
      (select->check_map_order_to_base &&
       check_resolved_order_item(select->order_list, select->base_ref_items,
                                 *select->check_map_order_to_base))) {
    return true;
  }
  for (uint i = 0; i < select->fields.size(); i++) {
    auto item = select->fields.operator[](i);
    auto orig_item = orig_fields.operator[](i);
    if (!item || !item->real_item() ||
        (item->real_item()->type() != orig_item->real_item()->type()) ||
        /*
          After PQ clone, fix_field process is re-executed, 'm_data_type' may be
          recalculated and changed, resulting in failure of consistency check
          between leader/worker tables in JOIN::make_worker_tmp_table process
          due to m_data_type inequality.
        */
        (item->real_item()->data_type() !=
         orig_item->real_item()->data_type())) {
      return true;
    }
  }
  return false;
}

JOIN *pq_make_join(THD *thd, JOIN *join) {
  /*
    When we come here, all correlated subqueries have been optimized already
    for sure. Non-correlated subqueries (or derived tables), which will
    execute in PQ "independently" (before us) have been optimized too and have
    done pq_make_join() already. They have their own THD, LEX, locked tables,
    which is ok as their THD is different. Thus, it is the right moment to
    clone the unit, and the units of correlated subqueries, into a single
    THD&LEX.
  */
  auto clone_phase_guard =
      create_scope_guard([thd]() { thd->clone_phase = THD::PQ_UNKONWN; });
  JOIN *pq_join = nullptr;
  Query_block *select = pq_dup_select(thd, join->query_block);
  if (!select) return nullptr;

  auto top_select = select;
  auto top_origjoin = join;

  /*
    Unlike in other functions which use list_query_blocks_to_clone(), we must
    process the subquery first and top query last (hence the erase and emplace
    below), because the resolution of the top query needs a resolved subquery.
    For example, for "WHERE x>(select ...)", the top query calls
    subquery_unit->cols() to check that it has one column, but cols() asserts
    that the subq is already resolved. Other functions do need the other order
    of processing, though.
  */
  auto query_blocks_to_clone =
      list_query_blocks_to_clone(top_origjoin->query_block);
  // delete first and move it last
  query_blocks_to_clone.erase(query_blocks_to_clone.begin());
  query_blocks_to_clone.emplace_back(top_origjoin->query_block);

  for (auto query_block : query_blocks_to_clone) {
    select = query_block->pq_last_clone();
    join = query_block->join;

    Switch_for_resolution_of_query_block switch_resol(
        thd->lex, select, query_block != top_origjoin->query_block);

    if (pq_select_prepare(thd, select, join->query_block->fields)) {
      goto err;
    }
    Table_ref *it = select->leaf_tables;
    pq_join = new (thd->pq_mem_root) JOIN(thd, select);
    if (!pq_join || DBUG_EVALUATE_IF("dup_join_abort", true, false)) {
      goto err;
    }
    if (pq_join->pq_copy_from(join)) goto err;

    /** init cost model */
    while (it) {
      it->table->init_cost_model(pq_join->cost_model());
      it = it->next_leaf;
    }

    pq_join->query_expression()->set_prepared();
    /**
     * limit cannot push down to worker, for the cases:
     *   (1) with aggregation
     *   (2) with sorting after optimized-group-by.
     * Only the top query is splitting jobs into workers and is concerned here.
     */
    if (select == top_select && join->query_expression()->select_limit_cnt) {
      if (join->query_block->with_sum_func ||  // c1
          (join->pq_rebuilt_group &&           // c2
           join->pq_last_sort_idx >= (int)join->primary_tables)) {
        pq_join->m_select_limit = HA_POS_ERROR;  // no limit
        pq_join->query_expression()->select_limit_cnt = HA_POS_ERROR;
      }
    }
  }
  return pq_join;  // returns the top join

err:
  return nullptr;
}

Semijoin_mat_exec *Semijoin_mat_exec::pq_clone(THD *thd, Query_block *select) {
  // Let leader allocate space for shared semi-join materialized table when
  // clone gather's execution plan.
  if (!m_pq_shared_info) {
    m_pq_shared_info =
        (PQ_shared_info **)thd->pq_mem_root->Alloc(sizeof(PQ_shared_info *));
    if (!m_pq_shared_info) return nullptr;
    *m_pq_shared_info = nullptr;  // not pointing anywhere, yet
  }

  assert(sj_nest);
  NESTED_JOIN *orig_nj = sj_nest->nested_join;
  NESTED_JOIN *nj = new (thd->pq_mem_root) NESTED_JOIN();
  if (!nj) return nullptr;

  nj->first_nested = orig_nj->first_nested;
  nj->natural_join_processed = orig_nj->natural_join_processed;
  nj->nj_counter = orig_nj->nj_counter;
  nj->nj_total = orig_nj->nj_total;
  nj->not_null_tables = orig_nj->not_null_tables;
  nj->query_block_id = orig_nj->query_block_id;
  nj->sj_corr_tables = orig_nj->sj_corr_tables;
  nj->sj_depends_on = orig_nj->sj_depends_on;
  nj->sj_enabled_strategies = orig_nj->sj_enabled_strategies;
  nj->used_tables = orig_nj->used_tables;
  for (Item *it : orig_nj->sj_inner_exprs) {
    Item *it1 = it->pq_clone(thd, select);
    if (!it1) return nullptr;
    nj->sj_inner_exprs.push_back(it1);
  }

  for (Item *it : orig_nj->sj_outer_exprs) {
    Item *it1 = it->pq_clone(thd, select);
    if (!it1) return nullptr;
    nj->sj_outer_exprs.push_back(it1);
  }

  // Find the cloned table_list that has the same table_no with original
  // table_list and then add it to nj->join_list.
  for (Table_ref *tbl : orig_nj->m_tables) {
    for (Table_ref *tr = select->leaf_tables; tr != nullptr;
         tr = tr->next_leaf) {
      if (tr->tableno() == tbl->tableno()) {
        nj->m_tables.push_back(tr);
        break;
      }
      assert(tr);
    }
  }

  Table_ref *sj_nest1 = new (thd->pq_mem_root) Table_ref();
  if (!sj_nest1) return nullptr;

  sj_nest1->nested_join = nj;
  if (sj_nest->alias != nullptr) {
    sj_nest1->is_alias = true;
    sj_nest1->alias =
        strmake_root(thd->mem_root, sj_nest->alias, strlen(sj_nest->alias));
  }

  Semijoin_mat_exec *sj_mat_exec = new (thd->pq_mem_root)
      Semijoin_mat_exec(sj_nest1, is_scan, table_count, mat_table_index,
                        inner_table_index, m_pq_shared_info);

  return sj_mat_exec;
}

void System_variables::pq_copy_from(struct System_variables orig) {
  // keep them sorted for readability
  big_tables = orig.big_tables;
  collation_connection = orig.collation_connection;
  div_precincrement = orig.div_precincrement;
  explicit_defaults_for_timestamp = orig.explicit_defaults_for_timestamp;
  internal_tmp_mem_storage_engine = orig.internal_tmp_mem_storage_engine;
  join_buff_size = orig.join_buff_size;
  lc_time_names = orig.lc_time_names;
  max_heap_table_size = orig.max_heap_table_size;
  max_sort_length = orig.max_sort_length;
  my_aes_mode = orig.my_aes_mode;
  option_bits = orig.option_bits;
  parallel_batch_max_mem_size = orig.parallel_batch_max_mem_size;
  parallel_batch_max_slot = orig.parallel_batch_max_slot;
  pq_support_features_switch = orig.pq_support_features_switch;
  pq_hash_join_max_hash_table_refills =
      orig.pq_hash_join_max_hash_table_refills;
  pseudo_thread_id = orig.pseudo_thread_id;
  sortbuff_size = orig.sortbuff_size;
  sql_mode = orig.sql_mode;
  time_zone = orig.time_zone;
  transaction_isolation = orig.transaction_isolation;

  /**
    Partial result cache variables
  */
  optimizer_switch = orig.optimizer_switch;
  partial_result_cache_max_mem_size = orig.partial_result_cache_max_mem_size;
  partial_result_cache_cost_threshold =
      orig.partial_result_cache_cost_threshold;
  partial_result_cache_min_hit_ratio = orig.partial_result_cache_min_hit_ratio;
  partial_result_cache_check_hit_ratio_frequency =
      orig.partial_result_cache_check_hit_ratio_frequency;
}

bool System_status_var::pq_merge_status(
    const struct System_status_var &worker) {
  // Currently PQ only support for select query, no need to merge
  // DML related status vars(e.g. ha_update_count, ha_write_count,
  // ha_commit_count, ha_rollback_count, ha_savepoint_count, etc.)
  // PQ not support prepared statement, no need to merge prepare &
  // execute related status vars(e.g. com_stmt_prepare, com_stmt_execute)
  created_tmp_disk_tables += worker.created_tmp_disk_tables;
  created_tmp_tables += worker.created_tmp_tables;
  filesort_range_count += worker.filesort_range_count;
  filesort_rows += worker.filesort_rows;
  filesort_scan_count += worker.filesort_scan_count;
  ha_read_first_count += worker.ha_read_first_count;
  ha_read_last_count += worker.ha_read_last_count;
  ha_read_key_count += worker.ha_read_key_count;
  ha_read_next_count += worker.ha_read_next_count;
  ha_read_prev_count += worker.ha_read_prev_count;
  ha_read_rnd_count += worker.ha_read_rnd_count;
  ha_read_rnd_next_count += worker.ha_read_rnd_next_count;
  return false;
}

void THD::pq_copy_from(THD *thd) {
  variables.pq_copy_from(thd->variables);
  start_time = thd->start_time;
  user_time = thd->user_time;
  m_query_string = thd->m_query_string;
  tx_isolation = thd->tx_isolation;
  tx_read_only = thd->tx_read_only;
  previous_found_rows = thd->previous_found_rows;
  has_pq = thd->has_pq;
  pq_dop = thd->pq_dop;
  running_explain_analyze = thd->running_explain_analyze;
  arg_of_last_insert_id_function = thd->arg_of_last_insert_id_function;
  first_successful_insert_id_in_prev_stmt =
      thd->first_successful_insert_id_in_prev_stmt;
  first_successful_insert_id_in_prev_stmt_for_binlog =
      thd->first_successful_insert_id_in_prev_stmt_for_binlog;
  first_successful_insert_id_in_cur_stmt =
      thd->first_successful_insert_id_in_cur_stmt;
  stmt_depends_on_first_successful_insert_id_in_prev_stmt =
      thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt;
  if (lex && thd->lex) {
    lex->is_explain_analyze = thd->lex->is_explain_analyze;
  }
  current_stmt_binlog_format = thd->current_stmt_binlog_format;
}

bool THD::pq_merge_status(THD *thd) {
  status_var.pq_merge_status(thd->status_var);
  // For gather, merge worker's found_rows to gather's pq_current_found_rows
  // For leader, copy pq_current_found_rows from gather
  if (is_pq_worker())
    pq_current_found_rows += thd->current_found_rows;
  else
    pq_current_found_rows = thd->pq_current_found_rows;
  return false;
}

// @TODO: remove it later when item_sum_avg has no
// need to be wrapped.
void reset_avg_property(mem_root_deque<Item *> &items) {
  Item *item = nullptr;
  for (uint i = 0; i < items.size(); i++) {
    item = items[i];
    if (item->type() == Item::SUM_FUNC_ITEM) {
      auto item_sum = down_cast<Item_sum *>(item);
      if (item_sum->sum_func() == Item_sum::AVG_FUNC) {
        // See need_extra(): it may actually be Item_rollup_sum_switcher.
        auto item_sum_avg = dynamic_cast<Item_sum_avg *>(item_sum);
        if (item_sum_avg != nullptr) item_sum_avg->restore_orig_info();
      }
    }
  }
}

/*
 * clone qep_tab->range_scan()
 */
AccessPath *CopyRangeScanAccessPath(THD *thd, AccessPath *orig_path,
                                    TABLE *table) {
  // For INDEX_SKIP_SCAN and GROUP_INDEX_SKIP_SCAN types, the condition for
  // using these optimizations is that the query involves only a single
  // table, see get_best_skip_scan and get_best_group_min_max functions.
  // Therefore, these types of statements will not apply PQ optimization,
  // as PQ optimization only supports table/index/range scan types,
  // see pq_not_support_scantype function. Consequently, such statements will
  // not use PQ because no splittable table can be found.
  assert((orig_path->type != AccessPath::INDEX_SKIP_SCAN) &&
         (orig_path->type != AccessPath::GROUP_INDEX_SKIP_SCAN));

  AccessPath *path = new (thd->pq_mem_root) AccessPath;
  *path = *orig_path;
  switch (path->type) {
    case AccessPath::INDEX_RANGE_SCAN: {
      unsigned num_used_key_parts = path->index_range_scan().num_used_key_parts;
      unsigned num_ranges = path->index_range_scan().num_ranges;
      path->index_range_scan().used_key_part = static_cast<KEY_PART *>(
          thd->mem_root->Alloc(sizeof(KEY_PART) * num_used_key_parts));
      memcpy(path->index_range_scan().used_key_part,
             orig_path->index_range_scan().used_key_part,
             sizeof(KEY_PART) * num_used_key_parts);
      for (unsigned i = 0; i < num_used_key_parts; ++i) {
        uint16 key = path->index_range_scan().used_key_part[i].key;
        uint16 part = path->index_range_scan().used_key_part[i].part;
        assert(part == i);
        path->index_range_scan().used_key_part[i].field =
            table->key_info[key].key_part[i].field;
      }
      // Copy QUICK_RANGE
      path->index_range_scan().ranges = static_cast<QUICK_RANGE **>(
          thd->mem_root->Alloc(sizeof(QUICK_RANGE *) * num_ranges));
      for (unsigned i = 0; i < num_ranges; ++i) {
        QUICK_RANGE *orig_range = orig_path->index_range_scan().ranges[i];
        path->index_range_scan().ranges[i] = new (thd->mem_root) QUICK_RANGE(
            thd->mem_root, orig_range->min_key, orig_range->min_length,
            orig_range->min_keypart_map, orig_range->max_key,
            orig_range->max_length, orig_range->max_keypart_map,
            orig_range->flag, orig_range->rkey_func_flag);
      }
      break;
    }
    case AccessPath::INDEX_MERGE:
      path->index_merge().children =
          new (thd->mem_root) Mem_root_array<AccessPath *>(thd->mem_root);
      for (auto child : *orig_path->index_merge().children) {
        path->index_merge().children->push_back(
            CopyRangeScanAccessPath(thd, child, table));
      }
      path->index_merge().table = table;
      break;
    case AccessPath::ROWID_INTERSECTION:
      path->rowid_intersection().children =
          new (thd->mem_root) Mem_root_array<AccessPath *>(thd->mem_root);
      for (auto child : *orig_path->rowid_intersection().children) {
        path->rowid_intersection().children->push_back(
            CopyRangeScanAccessPath(thd, child, table));
      }
      if (path->rowid_intersection().cpk_child) {
        path->rowid_intersection().cpk_child = CopyRangeScanAccessPath(
            thd, orig_path->rowid_intersection().cpk_child, table);
      }
      path->rowid_intersection().table = table;
      break;
    case AccessPath::ROWID_UNION:
      path->rowid_union().children =
          new (thd->mem_root) Mem_root_array<AccessPath *>(thd->mem_root);
      for (auto child : *orig_path->rowid_union().children) {
        path->rowid_union().children->push_back(
            CopyRangeScanAccessPath(thd, child, table));
      }
      path->rowid_union().table = table;
      break;
    default:
      assert(false);
  }
  return path;
}
