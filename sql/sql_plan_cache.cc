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

#include "sql/sql_plan_cache.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <new>
#include <type_traits>
#include <utility>

#include "my_dbug.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/current_thd.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_subselect.h"
#include "sql/item_timefunc.h"
#include "sql/psi_memory_key.h"
#include "sql/sql_executor.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_parse.h"
#include "sql/sql_select.h"
#include "sql/table.h"
#include "template_utils.h"

template <typename T>
static T *make_cmp_op(THD *thd, Item_func *op) {
  assert(op->argument_count() <= 3);
  Item *args[3];
  for (uint i = 0; i < op->argument_count(); ++i) {
    args[i] = clone_if_transient(thd, op->get_arg(i));
    if (args[i] == nullptr) return nullptr;
  }

  if constexpr (std::is_base_of_v<Item_func_comparison, T>) {
    assert(op->argument_count() == 2);
    return new (thd->mem_root) T(args[0], args[1]);
  } else if constexpr (std::is_same_v<Item_func_if, T>) {
    assert(op->argument_count() == 3);
    return new (thd->mem_root) T(args[0], args[1], args[2]);
  } else {
    assert(op->argument_count() == 1);
    return new (thd->mem_root) T(args[0]);
  }
}

bool JOIN::shallow_clone(JOIN *orig) {
  if (orig == nullptr) return true;

  tables_list = query_block->leaf_tables;
  const_table_map = orig->const_table_map;
  where_cond = orig->where_cond;
  having_cond = orig->having_cond;
  having_for_explain = orig->having_for_explain;
  tables = orig->tables;
  primary_tables = orig->primary_tables;
  const_tables = orig->const_tables;
  tmp_tables = orig->tmp_tables;
  explain_flags = orig->explain_flags;
  calc_found_rows = orig->calc_found_rows;
  m_select_limit = orig->m_select_limit;
  send_group_parts = orig->send_group_parts;
  grouped = orig->grouped;
  group_optimized_away = orig->group_optimized_away;
  implicit_grouping = orig->implicit_grouping;
  need_tmp_before_win = orig->need_tmp_before_win;
  simple_group = orig->simple_group;
  simple_order = orig->simple_order;
  streaming_aggregation = orig->streaming_aggregation;
  m_ordered_index_usage = orig->m_ordered_index_usage;
  skip_sort_order = orig->skip_sort_order;
  select_distinct = orig->select_distinct;
  group_list = orig->group_list;
  order = orig->order;
  rollup_state = orig->rollup_state;
  best_read = orig->best_read;
  best_rowcount = orig->best_rowcount;
  m_windows_sort = orig->m_windows_sort;

  return false;
}

namespace plan_cache {

std::atomic<ulong> cached_plan_count{0};
std::atomic<ulong> cached_plan_invalidations{0};

enum class Cached_apply_result { HIT, RECOVERABLE_MISS, FATAL_ERROR };

Exec_context::~Exec_context() = default;

bool Exec_context::is_environment_changed(THD *thd) const {
  return ((thd->variables.optimizer_switch &
           interested_optimizer_switch_flags) ^
          optimizer_switch) ||
         character_set_client != thd->variables.character_set_client;
}

static bool is_prepared_select(THD *thd, Query_block *query_block) {
  return thd->stmt_arena != nullptr && !thd->stmt_arena->is_regular() &&
         thd->sp_runtime_ctx == nullptr &&
         query_block->parent_lex->sql_command == SQLCOM_SELECT;
}

static bool is_supported_scalar_subquery_item(Item_subselect *item) {
  if (item == nullptr) return false;
  if (item->substype() != Item_subselect::SINGLEROW_SUBS) return false;
  if (down_cast<Item_singlerow_subselect *>(item)->is_maxmin())
    return false;
  return !item->is_uncacheable();
}

static bool is_supported_scalar_subquery(Query_expression *unit) {
  return unit != nullptr && is_supported_scalar_subquery_item(unit->item);
}

static bool has_only_supported_scalar_subqueries(Query_block *query_block) {
  for (Query_expression *unit = query_block->first_inner_query_expression();
       unit != nullptr; unit = unit->next_query_expression()) {
    if (!is_supported_scalar_subquery(unit)) return false;
  }
  return true;
}

static bool has_unsupported_subquery_item(Item *item) {
  if (item == nullptr) return false;

  return WalkItem(item, enum_walk::PREFIX, [](Item *sub_item) {
    if (sub_item->type() == Item::FUNC_ITEM &&
        down_cast<Item_func *>(sub_item)->functype() ==
            Item_func::NOT_ALL_FUNC)
      return true;
    if (sub_item->type() != Item::SUBSELECT_ITEM) return false;
    return !is_supported_scalar_subquery_item(
        down_cast<Item_subselect *>(sub_item));
  });
}

static bool has_unsupported_subquery_item_in_select_list(
    Query_block *query_block) {
  for (Item *field : query_block->visible_fields()) {
    if (has_unsupported_subquery_item(field)) return true;
  }
  return false;
}

static bool has_pushed_index_condition(QEP_TAB *qep_tab) {
  if (qep_tab == nullptr) return false;
  TABLE *table = qep_tab->table();
  if (table == nullptr || table->file == nullptr) return false;
  return table->file->pushed_idx_cond != nullptr ||
         table->file->pushed_idx_cond_keyno != MAX_KEY;
}

static bool check_query_plan_cacheable(JOIN *join) {
  if (join == nullptr || join->query_block == nullptr) return false;

  THD *thd = join->thd;
  Query_block *query_block = join->query_block;
  Query_expression *unit = query_block->master_query_expression();
  if (!thd->variables.rds_plan_cache || !is_prepared_select(thd, query_block))
    return false;
  if (query_block->plan_cache_state == plan_cache_state::UNCACHEABLE)
    return false;
  if (query_block->leaf_table_count != 1 || query_block->leaf_tables == nullptr)
    return false;
  if (query_block->leaf_tables->next_leaf != nullptr) return false;
  if (unit->is_set_operation()) return false;
  if (unit->outer_query_block() != nullptr) {
    if (!is_supported_scalar_subquery(unit)) return false;
    if (!thd->change_list.is_empty()) return false;
  }
  if (has_unsupported_subquery_item(join->where_cond) ||
      has_unsupported_subquery_item(join->having_cond) ||
      has_unsupported_subquery_item_in_select_list(query_block))
    return false;
  if (query_block->first_inner_query_expression() != nullptr) {
    if (!has_only_supported_scalar_subqueries(query_block)) return false;
    if (!thd->change_list.is_empty()) return false;
  }
  if (!query_block->parent_lex->safe_to_cache_query ||
      query_block->parent_lex->set_var_list.elements != 0)
    return false;
  if (query_block->parent_lex->uses_stored_routines()) return false;
  if (query_block->parent_lex->using_hypergraph_optimizer()) return false;
  if (query_block->olap == ROLLUP_TYPE) return false;
  if (query_block->hidden_items_from_optimization > 0) return false;
  if (thd->locked_tables_mode == LTM_LOCK_TABLES ||
      thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES)
    return false;

  Table_ref *table_ref = query_block->leaf_tables;
  if (is_temporary_table(table_ref) || table_ref->schema_table ||
      table_ref->is_system_view)
    return false;
  if (table_ref->db != nullptr &&
      (is_infoschema_db(table_ref->db) || is_perfschema_db(table_ref->db)))
    return false;
  if (table_ref->db != nullptr &&
      table_ref->db_length == MYSQL_SCHEMA_NAME.length &&
      my_strcasecmp(system_charset_info, MYSQL_SCHEMA_NAME.str,
                    table_ref->db) == 0)
    return false;

  if (join->qep_tab != nullptr) {
    QEP_TAB *qep_tab = &join->qep_tab[0];
    TABLE *table = qep_tab->table();
    if (qep_tab->type() == JT_FT) return false;
    if (table != nullptr && table->s != nullptr &&
        table->s->is_secondary_engine())
      return false;
    if (has_pushed_index_condition(qep_tab)) return false;
  }

  return true;
}

static void fill_lifecycle_context(THD *thd, JOIN *join,
                                   Exec_context *context) {
  context->optimizer_switch =
      thd->variables.optimizer_switch &
      Exec_context::interested_optimizer_switch_flags;
  context->character_set_client = thd->variables.character_set_client;

  if (join->qep_tab == nullptr) return;
  TABLE *table = join->qep_tab[0].table();
  if (table == nullptr || table->s == nullptr) return;
  context->table_version = table->s->get_table_ref_version();
  if (table->file != nullptr) context->table_records = table->file->stats.records;
}

static bool is_supported_single_table_candidate(JOIN *join) {
  if (join == nullptr || join->query_block == nullptr ||
      join->qep_tab == nullptr)
    return false;
  Query_block *query_block = join->query_block;
  if (query_block->leaf_table_count != 1 || query_block->leaf_tables == nullptr)
    return false;
  if (join->tables < 1 || join->primary_tables != 1 || join->tmp_tables != 0)
    return false;
  if (!join->order.empty() || !join->group_list.empty() ||
      join->m_windows.elements != 0 || join->select_distinct ||
      join->need_tmp_before_win || join->implicit_grouping ||
      join->select_count || join->tmp_table_param.sum_func_count != 0 ||
      join->rollup_state != JOIN::RollupState::NONE ||
      join->having_cond != nullptr)
    return false;

  QEP_TAB *qep_tab = &join->qep_tab[0];
  if (!(qep_tab->range_scan() == nullptr && qep_tab->op_type == QEP_TAB::OT_NONE &&
        qep_tab->materialize_table == QEP_TAB::NO_SETUP &&
        !qep_tab->using_dynamic_range && !qep_tab->needs_duplicate_removal &&
        qep_tab->having == nullptr && qep_tab->tmp_table_param == nullptr &&
        qep_tab->filesort == nullptr &&
        qep_tab->filesort_pushed_order == nullptr &&
        qep_tab->invalidators == nullptr && !qep_tab->reversed_access()))
    return false;

  switch (qep_tab->type()) {
    case JT_ALL:
      if (join->const_tables != 0 || qep_tab->keyread_optim() ||
          qep_tab->ref().key != -1)
        return false;
      break;
    case JT_CONST:
    case JT_EQ_REF:
    case JT_REF_OR_NULL:
    case JT_REF: {
      Index_lookup &ref = qep_tab->ref();
      if (ref.key < 0 || ref.key_parts == 0 || ref.key_buff == nullptr ||
          ref.key_buff2 == nullptr || ref.items == nullptr ||
          ref.cond_guards == nullptr || ref.keypart_hash != nullptr ||
          qep_tab->position() == nullptr || qep_tab->position()->key == nullptr ||
          qep_tab->has_guarded_conds())
        return false;
      break;
    }
    default:
      return false;
  }

  for (uint i = 1; i < join->tables; ++i) {
    QEP_TAB *extra_qep_tab = &join->qep_tab[i];
    if (extra_qep_tab->table() != nullptr ||
        extra_qep_tab->type() != JT_UNKNOWN ||
        extra_qep_tab->range_scan() != nullptr ||
        extra_qep_tab->op_type != QEP_TAB::OT_NONE ||
        extra_qep_tab->condition() != nullptr)
      return false;
  }

  return true;
}

enum class Copy_ref_access_result { COPIED, UNSUPPORTED, ERROR };

static Copy_ref_access_result copy_ref_access(THD *thd,
                                              Query_block *query_block,
                                              QEP_TAB *orig_qep_tab,
                                              QEP_TAB *cached_qep_tab,
                                              POSITION *position) {
  Index_lookup &orig_ref = orig_qep_tab->ref();
  Index_lookup &ref = cached_qep_tab->ref();
  if (init_ref(thd, orig_ref.key_parts, orig_ref.key_length, orig_ref.key,
               &ref))
    return Copy_ref_access_result::ERROR;

  ref.key_err = orig_ref.key_err;
  ref.null_rejecting = orig_ref.null_rejecting;
  ref.depend_map = orig_ref.depend_map;
  ref.use_count = 0;
  ref.disable_cache = orig_ref.disable_cache;
  if (orig_ref.null_ref_key != nullptr)
    ref.null_ref_key = ref.key_buff + (orig_ref.null_ref_key - orig_ref.key_buff);

  Key_use *key = new (thd->mem_root) Key_use[orig_ref.key_parts];
  if (key == nullptr) return Copy_ref_access_result::ERROR;
  for (uint part_no = 0; part_no < orig_ref.key_parts; ++part_no) {
    key[part_no] = orig_qep_tab->position()->key[part_no];
    Item *key_value = key[part_no].val;
    Item *real_key_value = key_value != nullptr ? key_value->real_item() : nullptr;
    if (real_key_value != nullptr &&
        real_key_value->type() == Item::PARAM_ITEM) {
      key_value = real_key_value;
    } else {
      key_value = clone_if_transient(thd, key_value);
    }
    key[part_no].val = key_value;
    if (key[part_no].val == nullptr) {
      if (thd->is_error()) return Copy_ref_access_result::ERROR;
      set_uncacheable(query_block);
      return Copy_ref_access_result::UNSUPPORTED;
    }
    key[part_no].table_ref = nullptr;

    ref.items[part_no] = key[part_no].val;
    ref.cond_guards[part_no] = nullptr;
    ref.key_copy[part_no] = nullptr;
  }
  position->key = key;
  return Copy_ref_access_result::COPIED;
}

static bool copy_supported_single_table_qep(THD *thd, JOIN *orig_join,
                                            JOIN *cached_join,
                                            Exec_context *context) {
  if (!is_supported_single_table_candidate(orig_join)) return false;

  QEP_TAB *orig_qep_tab = &orig_join->qep_tab[0];
  POSITION *orig_position = orig_qep_tab->position();
  if (orig_position == nullptr) return false;

  QEP_shared *qep_shared = new (thd->mem_root) QEP_shared[orig_join->tables];
  if (qep_shared == nullptr) return true;
  QEP_TAB *qep_tab = new (thd->mem_root) QEP_TAB[orig_join->tables + 1];
  if (qep_tab == nullptr) return true;
  POSITION *position = new (thd->mem_root) POSITION(*orig_position);
  if (position == nullptr) return true;
  position->table = nullptr;
  position->key = nullptr;

  for (uint i = 0; i < orig_join->tables; ++i) {
    qep_tab[i].set_qs(&qep_shared[i]);
  }
  qep_tab[0].set_join(cached_join);
  qep_tab[0].set_idx(orig_qep_tab->idx());
  qep_tab[0].set_position(position);
  qep_tab[0].set_type(orig_qep_tab->type());
  qep_tab[0].set_index(orig_qep_tab->index());
  qep_tab[0].keys() = orig_qep_tab->keys();
  qep_tab[0].set_records(orig_qep_tab->records());
  qep_tab[0].set_prefix_tables(orig_qep_tab->prefix_tables(),
                               orig_qep_tab->prefix_tables() &
                                   ~orig_qep_tab->added_tables());
  qep_tab[0].set_skip_records_in_range(
      orig_qep_tab->skip_records_in_range());

  Item *condition = orig_qep_tab->condition();
  if (condition != nullptr) {
    condition = clone_if_transient(thd, condition);
    if (condition == nullptr) return thd->is_error();
  }
  qep_tab[0].set_condition(condition);
  qep_tab[0].table_ref = nullptr;
  qep_tab[0].ref_item_slice = orig_qep_tab->ref_item_slice;
  qep_tab[0].not_used_in_distinct = orig_qep_tab->not_used_in_distinct;
  qep_tab[0].lateral_derived_tables_depend_on_me =
      orig_qep_tab->lateral_derived_tables_depend_on_me;

  if (orig_qep_tab->ref().key != -1) {
    switch (copy_ref_access(thd, orig_join->query_block, orig_qep_tab,
                            &qep_tab[0], position)) {
      case Copy_ref_access_result::COPIED:
        break;
      case Copy_ref_access_result::UNSUPPORTED:
        return false;
      case Copy_ref_access_result::ERROR:
        return true;
    }
  }

  Item *where_cond = cached_join->where_cond;
  if (where_cond != nullptr) {
    where_cond = clone_if_transient(thd, where_cond);
    if (where_cond == nullptr) return thd->is_error();
    cached_join->where_cond = where_cond;
  }

  cached_join->qep_tab = qep_tab;
  context->supports_cached_hit = true;
  return false;
}

static bool replace_cached_ref_key(QEP_TAB *qep_tab) {
  Index_lookup *ref = &qep_tab->ref();
  if (ref->key < 0) return false;

  THD *const thd = qep_tab->join()->thd;
  KEY *const keyinfo = qep_tab->table()->key_info + ref->key;
  uchar *key_buff = ref->key_buff;
  const size_t key_length = ALIGN_SIZE(ref->key_length);
  std::memset(ref->key_buff, 0, key_length);
  std::memset(ref->key_buff2, 0, key_length);

  for (uint part_no = 0; part_no < ref->key_parts; ++part_no) {
    Key_use *keyuse = &qep_tab->position()->key[part_no];
    keyuse->table_ref = qep_tab->table_ref;
    if (keyuse->val != nullptr) keyuse->val->update_used_tables();

    const bool maybe_null = keyinfo->key_part[part_no].null_bit;
    store_key *key =
        get_store_key(thd, keyuse->val, qep_tab->join()->const_table_map,
                      qep_tab->join()->const_table_map,
                      &keyinfo->key_part[part_no], key_buff, maybe_null);
    if (key == nullptr || thd->is_error()) return true;
    if (key->copy() != store_key::STORE_KEY_OK) return true;

    ref->key_copy[part_no] = key->null_key ? key : nullptr;
    ref->items[part_no] = keyuse->val;
    key_buff += keyinfo->key_part[part_no].store_length;
  }
  ref->key_err = false;
  ref->use_count = 0;
  return false;
}

static void restore_cached_ref_key_read(QEP_TAB *qep_tab) {
  switch (qep_tab->type()) {
    case JT_EQ_REF:
    case JT_REF:
    case JT_REF_OR_NULL:
      break;
    default:
      return;
  }

  TABLE *table = qep_tab->table();
  const Index_lookup &ref = qep_tab->ref();
  if (table != nullptr && ref.key >= 0 && !table->key_read &&
      table->covering_keys.is_set(ref.key) && !table->no_keyread) {
    table->set_keyread(true);
  }
}

static void collect_item_params_from_item(Item *item,
                                          std::set<Item *> &params) {
  if (item == nullptr) return;

  WalkItem(item, enum_walk::POSTFIX, [&params](Item *sub_item) {
    if (sub_item->type() == Item::PARAM_ITEM) {
      params.insert(sub_item);
      return false;
    }

    if (sub_item->type() == Item::FUNC_ITEM) {
      Item_func *func = down_cast<Item_func *>(sub_item);
      if (func->functype() == Item_func::MULT_EQUAL_FUNC) {
        Item *const_arg = down_cast<Item_equal *>(sub_item)->const_arg();
        collect_item_params_from_item(const_arg, params);
      }
    }
    return false;
  });
}

static bool is_item_type_transformed(Item *old_value, Item *new_value) {
  if (old_value == nullptr || new_value == nullptr) return old_value != new_value;

  if (old_value->type() == Item::CACHE_ITEM)
    old_value = down_cast<Item_cache *>(old_value)->get_example();
  if (new_value->type() == Item::CACHE_ITEM)
    new_value = down_cast<Item_cache *>(new_value)->get_example();

  if (old_value->type() != new_value->type()) return true;

  if (old_value->type() == Item::FUNC_ITEM &&
      down_cast<Item_func *>(old_value)->functype() !=
          down_cast<Item_func *>(new_value)->functype())
    return true;

  return false;
}

static bool has_same_type_and_item_params(Item *old_value, Item *new_value) {
  std::set<Item *> old_params;
  collect_item_params(old_value, old_params);
  if (old_params.empty()) return true;

  if (is_item_type_transformed(old_value, new_value)) return false;

  std::set<Item *> new_params;
  collect_item_params(new_value, new_params);
  return old_params == new_params;
}

static bool item_tree_changes_keep_params(THD *thd) {
  I_List_iterator<Item_change_record> it(thd->change_list);
  Item_change_record *change;
  while ((change = it++)) {
    if (change->m_cancel) continue;
    if (!has_same_type_and_item_params(change->old_value, change->new_value))
      return false;
  }
  return true;
}

static thread_local Exec_context *active_clone_context = nullptr;

class Scoped_clone_context {
 public:
  explicit Scoped_clone_context(Exec_context *context)
      : saved_context(active_clone_context) {
    active_clone_context = context;
  }

  Scoped_clone_context(const Scoped_clone_context &) = delete;
  Scoped_clone_context &operator=(const Scoped_clone_context &) = delete;

  ~Scoped_clone_context() { active_clone_context = saved_context; }

 private:
  Exec_context *saved_context;
};

static bool capture_transient_item_state(THD *thd, Query_block *query_block,
                                         MEM_ROOT *transient_mem_root,
                                         Item *transient_item_list,
                                         Exec_context *context) {
  Scoped_clone_context scoped_clone_context(context);

  try {
    auto &transient_items_map = context->transient_items;
    for (Item *item = transient_item_list; item != nullptr;
         item = item->next_free) {
      if (!transient_mem_root->Contains(item)) {
        set_uncacheable(query_block);
        return false;
      }
      transient_items_map[item] = nullptr;
    }

    auto &new_change_list = context->new_change_list;
    I_List_iterator<Item_change_record> it(thd->change_list);
    Item_change_record *change;
    while ((change = it++)) {
      if (change->m_cancel) continue;
      if (!transient_mem_root->Contains(change->place)) {
        new_change_list.push_back({change->place, change->new_value});
      }
    }
    std::reverse(new_change_list.begin(), new_change_list.end());

    for (auto &change : new_change_list) {
      if (change.second != nullptr &&
          transient_mem_root->Contains(change.second)) {
        Item *cloned_item = clone_if_transient(thd, change.second);
        if (cloned_item == nullptr) {
          if (thd->is_error()) return true;
          set_uncacheable(query_block);
          return false;
        }
        change.second = cloned_item;
      }
    }
  } catch (const std::bad_alloc &) {
    return true;
  }

  return false;
}

bool cache_plan(JOIN *join) {
  if (join == nullptr || join->query_block == nullptr) return false;
  Query_block *query_block = join->query_block;
  if (is_ready(query_block)) return false;
  if (!check_query_plan_cacheable(join)) return false;

  THD *thd = join->thd;
  if (!item_tree_changes_keep_params(thd)) {
    set_uncacheable(query_block);
    return false;
  }

  MEM_ROOT *transient_mem_root = thd->mem_root;
  Item *transient_item_list = thd->item_list();

  query_block->plan_cache_state = plan_cache_state::START;

  MEM_ROOT mem_root{key_memory_plan_cache_mem_root, 1024};
  Query_arena arena(&mem_root, Query_arena::STMT_PREPARED), arena_backup;
  thd->swap_query_arena(arena, &arena_backup);

  JOIN *join_local = new (thd->mem_root) JOIN(thd, query_block);
  auto error_guard = create_scope_guard([&]() {
    query_block->cached_plan = nullptr;
    if (join_local != nullptr) {
      join_local->destroy();
      ::destroy(join_local);
    }
    thd->swap_query_arena(arena_backup, &arena);
    cleanup_items(arena.item_list());
    arena.free_items();
    set_uncacheable(query_block);
  });

  if (join_local == nullptr) return true;
  Exec_context *context = new (thd->mem_root) Exec_context(thd->mem_root);
  if (context == nullptr) return true;
  join_local->plan_cache_exec_context = context;

  if (join_local->shallow_clone(join)) return true;
  join_local->tables_list = nullptr;
  fill_lifecycle_context(thd, join, context);
  query_block->cached_plan = join_local;
  if (capture_transient_item_state(thd, query_block, transient_mem_root,
                                   transient_item_list, context))
    return true;
  if (query_block->plan_cache_state == plan_cache_state::UNCACHEABLE)
    return false;
  {
    Scoped_clone_context scoped_clone_context(context);
    if (copy_supported_single_table_qep(thd, join, join_local, context))
      return true;
  }
  if (!context->supports_cached_hit) {
    set_uncacheable(query_block);
    return false;
  }
  if (query_block->plan_cache_state == plan_cache_state::UNCACHEABLE)
    return false;

  void *root_storage = mem_root.Alloc(sizeof(MEM_ROOT));
  if (root_storage == nullptr) return true;
  void *arena_storage = mem_root.Alloc(sizeof(Query_arena));
  if (arena_storage == nullptr) return true;

  thd->swap_query_arena(arena_backup, &arena);
  error_guard.release();

  MEM_ROOT *mem_root_heap = new (root_storage) MEM_ROOT(std::move(mem_root));
  Query_arena *arena_heap = new (arena_storage) Query_arena();
  Query_arena empty_arena;
  arena_heap->swap_query_arena(arena, &empty_arena);
  arena.set_query_arena(empty_arena);
  arena_heap->mem_root = mem_root_heap;
  cleanup_items(arena_heap->item_list());

  context->arena = arena_heap;
  context->counted = true;
  cached_plan_count.fetch_add(1, std::memory_order_relaxed);
  set_ready(query_block);
  return false;
}

void reset_cached_plan(JOIN *cached_plan) {
  cached_plan->zero_result_cause = nullptr;
  cached_plan->set_plan_state(JOIN::NO_PLAN);
  cached_plan->set_root_access_path(nullptr);
  cached_plan->ref_items = nullptr;
  cached_plan->tmp_fields = nullptr;
  cached_plan->current_ref_item_slice = REF_SLICE_SAVED_BASE;

  if (cached_plan->qep_tab != nullptr) {
    for (uint i = 0; i < cached_plan->tables; ++i)
      cached_plan->qep_tab[i].set_range_scan(nullptr);
  }
}

void detach_cached_plan(JOIN *cached_plan) {
  cached_plan->set_root_access_path(nullptr);
  cached_plan->ref_items = nullptr;
  cached_plan->tmp_fields = nullptr;
  cached_plan->current_ref_item_slice = REF_SLICE_SAVED_BASE;

  if (cached_plan->qep_tab != nullptr) {
    for (uint i = 0; i < cached_plan->tables; ++i) {
      QEP_TAB *qep_tab = &cached_plan->qep_tab[i];
      TABLE *table = qep_tab->table();
      if (table != nullptr) {
        table->reginfo.qep_tab = nullptr;
        if (table->file != nullptr) table->set_keyread(false);
      }
      qep_tab->table_ref = nullptr;
      qep_tab->set_range_scan(nullptr);
      qep_tab->set_table(nullptr);
    }
  }

  cached_plan->set_plan_state(JOIN::NO_PLAN);
}

void cleanup_cached_plan_items(JOIN *cached_plan) {
  if (cached_plan == nullptr ||
      cached_plan->plan_cache_exec_context == nullptr ||
      cached_plan->plan_cache_exec_context->arena == nullptr)
    return;

  cleanup_items(cached_plan->plan_cache_exec_context->arena->item_list());
}

bool has_bound_qep_table(JOIN *cached_plan) {
  if (cached_plan == nullptr || cached_plan->qep_tab == nullptr) return false;
  for (uint i = 0; i < cached_plan->tables; ++i) {
    if (cached_plan->qep_tab[i].table() != nullptr) return true;
  }
  return false;
}

void cleanup_failed_cached_plan_apply(THD *thd, JOIN *cached_plan) {
  thd->rollback_item_tree_changes();

  if (has_bound_qep_table(cached_plan)) {
    cached_plan->cleanup();
    if (cached_plan->is_cached_plan_hit()) cached_plan->destroy();
  }
  detach_cached_plan(cached_plan);
}

static bool has_fatal_cached_apply_error(THD *thd) {
  return thd->is_error() || thd->killed.load() != THD::NOT_KILLED;
}

static Cached_apply_result cached_apply_failure_result(THD *thd) {
  return has_fatal_cached_apply_error(thd)
             ? Cached_apply_result::FATAL_ERROR
             : Cached_apply_result::RECOVERABLE_MISS;
}

Cached_apply_result apply_cached_plan(JOIN *cached_plan) {
  Exec_context *context = cached_plan->plan_cache_exec_context;
  Query_block *query_block = cached_plan->query_block;
  THD *thd = cached_plan->thd;

  if (context == nullptr || !context->supports_cached_hit ||
      cached_plan->qep_tab == nullptr)
    return Cached_apply_result::RECOVERABLE_MISS;

  assert(query_block->leaf_table_count == 1);
  Table_ref *table_ref = query_block->leaf_tables;
  if (table_ref == nullptr || table_ref->table == nullptr)
    return Cached_apply_result::RECOVERABLE_MISS;

  bind_fields(context->arena->item_list());
  reset_cached_plan(cached_plan);

  auto error_guard = create_scope_guard([thd, cached_plan]() {
    cleanup_failed_cached_plan_apply(thd, cached_plan);
  });

  DBUG_EXECUTE_IF("plan_cache_apply_assert_detached_before_rebind", {
    assert(cached_plan->qep_tab == nullptr ||
           cached_plan->qep_tab[0].table() == nullptr);
  });

  for (auto &change : context->new_change_list) {
    if (*change.first != change.second) {
      thd->change_item_tree(change.first, change.second);
    }
    auto item_field = dynamic_cast<Item_field *>(change.second);
    if (item_field != nullptr && item_field->field != nullptr &&
        item_field->field->is_gcol()) {
      item_field->field->table->mark_column_used(item_field->field,
                                                 MARK_COLUMNS_READ);
    }
  }

  cached_plan->lock = thd->lock;
  cached_plan->fields = &query_block->fields;
  cached_plan->tables_list = table_ref;
  cached_plan->tmp_tables = 0;

  DBUG_EXECUTE_IF("plan_cache_apply_before_alloc_slices_fail",
                  return Cached_apply_result::RECOVERABLE_MISS;);
  if (cached_plan->alloc_indirection_slices())
    return cached_apply_failure_result(thd);
  cached_plan->ref_items[REF_SLICE_ACTIVE] = query_block->base_ref_items;

  QEP_TAB *qep_tab = &cached_plan->qep_tab[0];
  qep_tab->set_table(table_ref->table);
  qep_tab->table_ref = table_ref;
  qep_tab->set_range_scan(nullptr);
  table_ref->table->const_table = false;
  table_ref->table->set_not_started();
  if (replace_cached_ref_key(qep_tab)) return cached_apply_failure_result(thd);
  if (qep_tab->type() == JT_CONST && read_const_maybe_key_read(qep_tab))
    return cached_apply_failure_result(thd);
  restore_cached_ref_key_read(qep_tab);

  DBUG_EXECUTE_IF("plan_cache_apply_after_bind_fail",
                  return Cached_apply_result::RECOVERABLE_MISS;);

  if (cached_plan->where_cond != nullptr)
    cached_plan->where_cond->update_used_tables();
  if (qep_tab->condition() != nullptr) qep_tab->condition()->update_used_tables();

  cached_plan->create_access_paths();
  DBUG_EXECUTE_IF("plan_cache_apply_after_access_paths_fail",
                  return Cached_apply_result::RECOVERABLE_MISS;);
  DBUG_EXECUTE_IF("plan_cache_apply_after_access_paths_error", {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return Cached_apply_result::FATAL_ERROR;
  });
  if (has_fatal_cached_apply_error(thd))
    return Cached_apply_result::FATAL_ERROR;
  if (cached_plan->push_to_engines()) return cached_apply_failure_result(thd);
  DBUG_EXECUTE_IF("plan_cache_apply_after_push_to_engines_fail",
                  return Cached_apply_result::RECOVERABLE_MISS;);
  cached_plan->set_plan_state(JOIN::PLAN_READY);
  count_field_types(query_block, &cached_plan->tmp_table_param,
                    *cached_plan->fields, false, false);
  set_ready(query_block);
  cached_plan->set_optimized();
  error_guard.release();
  return Cached_apply_result::HIT;
}

Cached_apply_result apply_cached_plan_if_suitable(THD *thd,
                                                  Query_block *query_block) {
  JOIN *cached_plan = query_block->cached_plan;
  if (cached_plan == nullptr) return Cached_apply_result::RECOVERABLE_MISS;
  Exec_context *context = cached_plan->plan_cache_exec_context;
  Table_ref *table_ref = query_block->leaf_tables;
  if (context == nullptr || table_ref == nullptr || table_ref->table == nullptr)
    return Cached_apply_result::RECOVERABLE_MISS;
  if (!thd->change_list.is_empty())
    return Cached_apply_result::RECOVERABLE_MISS;
  if (table_ref->table->s == nullptr ||
      table_ref->table->s->is_secondary_engine())
    return Cached_apply_result::RECOVERABLE_MISS;
  const int row_count_error = table_ref->fetch_number_of_rows();
  if (row_count_error) {
    if (table_ref->table->file != nullptr)
      table_ref->table->file->print_error(row_count_error, MYF(0));
    else
      my_error(ER_UNKNOWN_ERROR, MYF(0));
    return Cached_apply_result::FATAL_ERROR;
  }
  if (table_ref->table->s->get_table_ref_version() != context->table_version)
    return Cached_apply_result::RECOVERABLE_MISS;
  if (context->is_environment_changed(thd))
    return Cached_apply_result::RECOVERABLE_MISS;
  if (table_ref->table->file != nullptr &&
      context->is_table_stats_changed_sharply(
          table_ref->table->file->stats.records,
          thd->variables.rds_plan_cache_allow_change_ratio))
    return Cached_apply_result::RECOVERABLE_MISS;
  const Cached_apply_result result = apply_cached_plan(cached_plan);
  if (result != Cached_apply_result::HIT) return result;

  thd->lock_query_plan();
  query_block->join = cached_plan;
  thd->unlock_query_plan();

  cached_plan->set_cached_plan_hit();
  thd->status_var.cached_plan_hits++;
  return Cached_apply_result::HIT;
}

bool exec_cached_plan(Query_block *query_block) {
  if (query_block == nullptr) return true;
  THD *thd = query_block->parent_lex->thd;
  if (thd == nullptr || !thd->variables.rds_plan_cache ||
      !is_ready(query_block))
    return true;
  if (query_block->master_query_expression()->set_limit(thd, query_block))
    return true;
  const Cached_apply_result result =
      apply_cached_plan_if_suitable(thd, query_block);
  if (result == Cached_apply_result::HIT) return false;
  invalidate_cached_plan(query_block);
  if (result == Cached_apply_result::FATAL_ERROR) return false;
  return true;
}

bool is_ready(Query_block *query_block) {
  return query_block != nullptr &&
         query_block->plan_cache_state == plan_cache_state::READY;
}

void set_uncacheable(Query_block *query_block) {
  if (query_block == nullptr) return;
  query_block->plan_cache_state = plan_cache_state::UNCACHEABLE;
}

void set_ready(Query_block *query_block) {
  if (query_block == nullptr) return;
  query_block->plan_cache_state = plan_cache_state::READY;
}

void invalidate_cached_plan(Query_block *query_block) {
  if (query_block == nullptr) return;
  JOIN *plan = query_block->cached_plan;
  if (plan != nullptr) {
    if (!plan->is_cached_plan_hit()) plan->destroy();
    destroy_cached_plan(plan);
    cached_plan_invalidations.fetch_add(1, std::memory_order_relaxed);
  }
  query_block->cached_plan = nullptr;
  query_block->plan_cache_state = plan_cache_state::NONE;
}

void destroy_cached_plan(JOIN *join) {
  if (join == nullptr) return;

  Exec_context *context = join->plan_cache_exec_context;
  Query_arena *arena = context != nullptr ? context->arena : nullptr;
  const bool counted = context != nullptr && context->counted;

  ::destroy(join);

  if (arena != nullptr) {
    MEM_ROOT *mem_root = arena->mem_root;
    cleanup_items(arena->item_list());
    arena->free_items();
    if (mem_root != nullptr) {
      MEM_ROOT root(std::move(*mem_root));
      ::destroy(arena);
    } else {
      ::destroy(arena);
    }
  }

  if (counted) cached_plan_count.fetch_sub(1, std::memory_order_relaxed);
}

void collect_item_params(Item *item, std::set<Item *> &params) {
  assert(current_thd != nullptr);
  if (current_thd == nullptr || !current_thd->variables.rds_plan_cache) return;
  collect_item_params_from_item(item, params);
}

void cmp_item_params_after_reduce_cond(THD *thd,
                                       const std::set<Item *> &old_params,
                                       Item *condition) {
  if (thd == nullptr || !thd->variables.rds_plan_cache || old_params.empty())
    return;

  if (condition == nullptr) {
    set_uncacheable(thd->lex->current_query_block());
    return;
  }

  std::set<Item *> new_params;
  collect_item_params(condition, new_params);
  if (old_params != new_params) set_uncacheable(thd->lex->current_query_block());
}

}  // namespace plan_cache

Item *clone_if_transient(THD *thd, Item *item) {
  if (thd == nullptr || item == nullptr) return item;

  plan_cache::Exec_context *context = plan_cache::active_clone_context;
  if (context == nullptr) return item;

  auto transient = context->transient_items.find(item);
  if (transient == context->transient_items.end()) return item;
  if (transient->second != nullptr) return transient->second;

  DBUG_EXECUTE_IF("plan_cache_clone_if_transient_fail", return nullptr;);

  Item *clone = nullptr;
  switch (item->type()) {
    case Item::FUNC_ITEM:
    case Item::COND_ITEM: {
      auto *func = down_cast<Item_func *>(item);
      switch (func->functype()) {
        case Item_func::EQ_FUNC:
          clone = make_cmp_op<Item_func_eq>(thd, func);
          break;
        case Item_func::LT_FUNC:
          clone = make_cmp_op<Item_func_lt>(thd, func);
          break;
        case Item_func::GT_FUNC:
          clone = make_cmp_op<Item_func_gt>(thd, func);
          break;
        case Item_func::LE_FUNC:
          clone = make_cmp_op<Item_func_le>(thd, func);
          break;
        case Item_func::GE_FUNC:
          clone = make_cmp_op<Item_func_ge>(thd, func);
          break;
        case Item_func::NE_FUNC:
          clone = make_cmp_op<Item_func_ne>(thd, func);
          break;
        case Item_func::ISNULL_FUNC:
          clone = make_cmp_op<Item_func_isnull>(thd, func);
          break;
        case Item_func::ISNOTNULL_FUNC:
          clone = make_cmp_op<Item_func_isnotnull>(thd, func);
          break;
        case Item_func::IF_FUNC:
          clone = make_cmp_op<Item_func_if>(thd, func);
          break;
        case Item_func::TRUE_FUNC:
          clone = new (thd->mem_root) Item_func_true();
          break;
        case Item_func::FALSE_FUNC:
          clone = new (thd->mem_root) Item_func_false();
          break;
        case Item_func::COND_AND_FUNC: {
          auto *cond = down_cast<Item_cond_and *>(item);
          auto *clone_cond = new (thd->mem_root) Item_cond_and(thd, cond);
          if (clone_cond == nullptr) return nullptr;
          for (Item &arg_item : *cond->argument_list()) {
            Item *arg = clone_if_transient(thd, &arg_item);
            if (arg == nullptr) return nullptr;
            if (clone_cond->argument_list()->push_back(arg, thd->mem_root))
              return nullptr;
          }
          clone = clone_cond;
          break;
        }
        case Item_func::COND_OR_FUNC: {
          auto *cond = down_cast<Item_cond_or *>(item);
          auto *clone_cond = new (thd->mem_root) Item_cond_or(thd, cond);
          if (clone_cond == nullptr) return nullptr;
          for (Item &arg_item : *cond->argument_list()) {
            Item *arg = clone_if_transient(thd, &arg_item);
            if (arg == nullptr) return nullptr;
            if (clone_cond->argument_list()->push_back(arg, thd->mem_root))
              return nullptr;
          }
          clone = clone_cond;
          break;
        }
        case Item_func::TYPECAST_FUNC: {
          Item *arg = clone_if_transient(thd, func->get_arg(0));
          if (arg == nullptr) return nullptr;
          switch (func->data_type()) {
            case MYSQL_TYPE_DATETIME:
              clone = new (thd->mem_root) Item_typecast_datetime(arg, false);
              break;
            case MYSQL_TYPE_DATE:
              clone = new (thd->mem_root) Item_typecast_date(arg, false);
              break;
            case MYSQL_TYPE_TIME:
              clone = new (thd->mem_root) Item_typecast_time(arg);
              break;
            case MYSQL_TYPE_DOUBLE:
              clone = new (thd->mem_root) Item_typecast_real(arg);
              break;
            default:
              break;
          }
          break;
        }
        case Item_func::DATETIME_LITERAL: {
          auto *datetime = down_cast<Item_datetime_literal *>(item);
          MYSQL_TIME ltime;
          datetime->get_date(&ltime, 0);
          clone = new (thd->mem_root)
              Item_datetime_literal(&ltime, datetime->decimals,
                                    thd->variables.time_zone);
          break;
        }
        default:
          break;
      }
      break;
    }
    case Item::CACHE_ITEM: {
      auto *cache = down_cast<Item_cache *>(item);
      Item *example = cache->get_example();
      if (example == nullptr) return nullptr;
      example = clone_if_transient(thd, example);
      if (example == nullptr) return nullptr;
      Item_cache *clone_cache =
          Item_cache::get_cache(example, item->result_type());
      if (clone_cache == nullptr) return nullptr;
      if (clone_cache->setup(example)) return nullptr;
      clone_cache->store(example);
      clone = clone_cache;
      break;
    }
    case Item::FIELD_ITEM: {
      auto *field_item = down_cast<Item_field *>(item);
      if (field_item->field == nullptr || !field_item->field->is_gcol())
        return nullptr;
      auto *field_clone = new (thd->mem_root) Item_field(field_item->field);
      if (field_clone == nullptr) return nullptr;
      field_clone->table_ref = nullptr;
      clone = field_clone;
      break;
    }
    default:
      break;
  }

  if (clone == nullptr && item->basic_const_item()) {
    clone = item->clone_item();
  }

  if (clone == nullptr) {
    if (auto *time_literal = dynamic_cast<Item_time_literal *>(item)) {
      MYSQL_TIME ltime;
      time_literal->get_time(&ltime);
      clone = new (thd->mem_root)
          Item_time_literal(&ltime, time_literal->decimals);
    } else if (auto *date_literal = dynamic_cast<Item_date_literal *>(item)) {
      MYSQL_TIME ltime;
      date_literal->get_date(&ltime, 0);
      clone = new (thd->mem_root) Item_date_literal(&ltime);
    }
  }

  if (clone == nullptr) return nullptr;

  if (!clone->fixed) {
    Item *fixed_clone = clone;
    const uint8 saved_context = thd->lex->context_analysis_only;
    thd->lex->context_analysis_only |= CONTEXT_ANALYSIS_ONLY_VIEW;
    const bool error = clone->fix_fields(thd, &fixed_clone);
    thd->lex->context_analysis_only = saved_context;
    if (error || fixed_clone != clone) return nullptr;
  }

  transient->second = clone;
  return clone;
}
