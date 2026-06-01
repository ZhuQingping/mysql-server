/* Copyright (c) 2025, Oracle and/or its affiliates.

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

/**
  @file
  Implementations of SQL resolver related to PQ .
*/

#include "sql/parallel_query/pq_resolver.h"

#include "sql/item_sum.h"
#include "sql/parallel_query/pq_clone.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"

// Check if there are fields with the same name in the temporary table.
static bool check_tmp_table_field_name(TABLE *table) {
  uint field_count = table->visible_field_count();
  Field **fields = table->visible_field_ptr();

  for (uint i = 1; i < field_count; i++) {
    const char *field_name_i = fields[i]->field_name;
    for (uint j = 0; j < i; j++) {
      if (!strcmp(field_name_i, fields[j]->field_name)) {
        return true;
      }
    }
  }
  return false;
}

bool Query_block::pq_check_table_list() {
  /*
    As long as one table doesn't pass the check, the query should not do PQ.
    For the choice of the divided/cut table, there are further checks in
    TABLE::suite_for_pq_division().
  */
  if (!join->qep_tab) return false;
  for (uint i = 0; i < join->tables; i++) {
    Table_ref *tbl_list = join->qep_tab[i].table_ref;
    if (!tbl_list || !tbl_list->table) continue;

    // skip system schemas
    if (tbl_list->db && tbl_list->db_length &&
        (is_sys_db(tbl_list->db) || is_infoschema_db(tbl_list->db) ||
         is_mysql_db(tbl_list->db) || is_perfschema_db(tbl_list->db))) {
      pq_unsuite_info = PQUnsuiteInfo::SYS_OR_TMP_TABLE;
      return true;
    }

    // Skip derived table or view. But fail if it's outer-correlated, as then
    // the PQ leader could not materialize it.
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
      return true;  // No support currently
    }

    // skip explicit table lock
    if (tbl_list->lock_descriptor().type > TL_READ_DEFAULT ||
        parent_lex->locking_clause) {
      pq_unsuite_info = PQUnsuiteInfo::TABLE_FUNCTION_OR_LOCK;
      return true;
    }

    auto tbl_share = tbl_list->table->s;
    if (tbl_share->tmp_table == TRANSACTIONAL_TMP_TABLE ||
        tbl_share->tmp_table == SYSTEM_TMP_TABLE) {
      pq_unsuite_info = PQUnsuiteInfo::SYS_OR_TMP_TABLE;
      return true;
    }

    if (tbl_list->query_block != this) {
      /*
        This is defensive programming. tlb_list->query_block should always be
        equal to 'this', but with subquery-to-derived it sometimes is not
        (this is a bug in Community MySQL). This in turn causes problems in
        pq_can_resolve_Item_field_in(). So we block this case.
      */
      pq_unsuite_info = PQUnsuiteInfo::INTER_UNSUITE;
      return true;
    }

    if ((tbl_share->tmp_table == INTERNAL_TMP_TABLE) &&
        check_tmp_table_field_name(tbl_list->table)) {
      pq_unsuite_info = PQUnsuiteInfo::TMP_TABLE_FIELD_NAME_SAME;
      return true;
    }
  }

  return false;
}

bool Query_block::record_map_order(
    SQL_I_List<ORDER> &list, mem_root_unordered_map<uint, uint> &map_order) {
  assert(list.size());
  assert(!map_order.size());

  uint first = 0, second = 0;
  for (ORDER *order = list.first; order; order = order->next) {
    second = 0;
    for (uint i = 0; i < base_ref_items.size(); i++) {
      if ((*order->item) == base_ref_items[i]) {
        map_order[first] = second;
        break;
      }
      second++;
    }
    if (second == base_ref_items.size()) return true;
    first++;
  }

  return false;
}

void Query_block::pq_backup() {
  auto query_blocks = list_query_blocks_to_clone(this);
  for (auto orig : query_blocks) {
    for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
         tbl_list = tbl_list->next_leaf) {
      // In the pq_dup_select function, set tbl_list->table->const_table
      // to false, and backup it first.
      tbl_list->table->pq_saved_const_table = tbl_list->table->const_table;
    }
  }
}

void Query_block::pq_restore() {
  auto restore_saved_list_ptrs = [](Group_list_ptrs *saved_list_ptrs) {
    if (!saved_list_ptrs) return;
    for (auto order : *saved_list_ptrs) {
      (*order->item)->walk(&Item::pq_restore, enum_walk::PREFIX, nullptr);
    }
  };

  auto query_blocks = list_query_blocks_to_clone(this);
  for (auto orig : query_blocks) {
    orig->m_pq_last_clone = nullptr;
    // restore tbl_list->table->const_table
    for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr;
         tbl_list = tbl_list->next_leaf) {
      tbl_list->table->const_table = tbl_list->table->pq_saved_const_table;
    }

    // Traverse all Item and call Item::pq_restore to perform the restore.
    orig->walk(&Item::pq_restore, enum_walk::PREFIX, nullptr);
    // Note that the walk above included orig->where_cond(), having_cond(),
    // group_list, order_list. Now handle their "copies":
    if (orig->saved_where_cond) {
      orig->saved_where_cond->walk(&Item::pq_restore, enum_walk::PREFIX,
                                   nullptr);
    }
    if (orig->saved_having_cond) {
      orig->saved_having_cond->walk(&Item::pq_restore, enum_walk::PREFIX,
                                    nullptr);
    }
    restore_saved_list_ptrs(orig->saved_group_list_ptrs);
    restore_saved_list_ptrs(orig->saved_order_list_ptrs);
  }
}

/*
 * determine whether suitable for parallel query
 */
bool Query_block::suite_for_parallel_query(THD *thd) {
  if (!thd->suite_for_parallel_query(&pq_unsuite_info)) {
    return false;
  }

  // Query with SQL_BUFFER_RESULT is not supported by PQ
  if (active_options() & OPTION_BUFFER_RESULT) {
    pq_unsuite_info = PQUnsuiteInfo::ROLLUP_WINDS_BUFFER;
    return false;
  }

  if (olap == ROLLUP_TYPE ||  // with rollup
      (row_value_list &&
       row_value_list->size() > 1) ||  // table value constructor
      saved_windows_elements)          // windows function
  {
    pq_unsuite_info = PQUnsuiteInfo::ROLLUP_WINDS_BUFFER;
    return false;
  }

  if (master_query_expression()->subquery_suite_for_parallel_query() ==
      PQSubqueryExecution::kImpossible) {
    pq_unsuite_info = PQUnsuiteInfo::UNSUPPORTED_SUBQUERY_TYPE;
    return false;
  }

  if (wrapped_in_intersect_except()) {
    pq_unsuite_info = PQUnsuiteInfo::UNSUPPORTED_INTERSECT_AND_EXCEPT;
    thd->no_pq = true;
    return false;
  }
  return true;
}

bool THD::suite_for_parallel_query(PQUnsuiteInfo *pq_info) const {
  // For simple select without pq hint, when force_parallel_execute is off,
  // no_pq is false and pq_dop is 0, we check pq_dop first to reduce one if
  // condition judgment.
  if (pq_dop == 0) {
    *pq_info = PQUnsuiteInfo::ZERO_DOP;
    return false;
  }

  if (no_pq) {
    *pq_info = PQUnsuiteInfo::NO_PQ;
    return false;
  }

  if (!(lex->sql_command == SQLCOM_SELECT ||
        lex->sql_command == SQLCOM_INSERT_SELECT ||
        lex->sql_command == SQLCOM_REPLACE_SELECT)) {
    *pq_info = PQUnsuiteInfo::UNSUPPORT_COMMAND;
    return false;
  }

  if (lex->in_execute_ps ||              // prepared statement
      in_sp_trigger ||                   // store procedure or trigger
      m_attachable_trx ||                // attachable transaction
      tx_isolation == ISO_SERIALIZABLE)  // serializable with locking reads
  {
    *pq_info = PQUnsuiteInfo::PREPARE_TRIGER_PROCEDURE;
    return false;
  }

  /* Parallel query only support InnoDB table */
  if (get_instance_storage_engine_type() != DB_TYPE_INNODB) {
    *pq_info = PQUnsuiteInfo::ONLY_SUPPORT_INNODB;
    return false;
  }

  /* Parallel query do not support hypergraph optimizer, since hypergraph
     optimizer doesn't create QEP_TABs, and PQ clones QEP_TABs. */
  if (lex->using_hypergraph_optimizer()) {
    *pq_info = PQUnsuiteInfo::HYPERGRAPH_OPTIMIZER;
    return false;
  }

  return true;
}

/// determine whether two already resolved items are equal
bool items_equal_after_resolve(Item *find, Item *item, bool &need_alias_item) {
  bool matched = false;
  if (current_thd->has_pq)
    matched = item ? find->eq_with_binary_cmp_arg(item, false) : false;
  else
    matched = item ? find->eq(item, false) : false;
  if (!matched) return false;

  bool is_ref_by_name =
      (find->type() == Item::FIELD_ITEM || find->type() == Item::REF_ITEM);
  const char *db_name{nullptr}, *table_name{nullptr};
  const char *field_name{nullptr}, *item_name{nullptr};
  if (is_ref_by_name) {
    db_name = ((Item_ident *)find)->db_name;
    table_name = ((Item_ident *)find)->table_name;
    item_name = ((Item_ident *)find)->item_name.ptr();
    field_name = ((Item_ident *)find)->field_name;
  }

  bool is_derived{false};
  const Item_field *item_field{nullptr};
  if (current_thd->has_pq && item->real_item()->type() == Item::FIELD_ITEM) {
    item_field = down_cast<const Item_field *>(item->real_item());
    is_derived = item_field->field && item_field->field->table &&
                 item_field->field->table->pos_in_table_list &&
                 item_field->field->table->pos_in_table_list->is_derived();
  }

  // When we use resolved item_name, table_name and field_name to
  // compare two Item_field (or Item_ref), they must have the same
  // name including alias if they are ident.
  if (field_name && item->real_item()->type() == Item::FIELD_ITEM) {
    bool is_marker_equal = true;
    // During create_tmp_table, the Item::marker field is set to
    // Item::MARKER_BIT for items in the GROUP BY clause. Therefore, marker
    // field validation is required specifically for BIT fields to prevent
    // the group item from being matched with the SELECT list item during
    // find_order_in_list. For example, in the query
    // 'SELECT BIN(a) FROM t1 GROUP BY a', there will be two Item_field
    // instances for 'a' in JOIN::fields, and the only difference between
    // them is the Item::marker field.
    if (item->data_type() == MYSQL_TYPE_BIT) {
      is_marker_equal = (find->marker == item->marker);
    }
    // check the alias to prevent one item to match multiple items
    auto item_base = down_cast<Item_ident *>(item);
    // derived table, item_base->db_name may be empty string.
    const char *cmp_db_name =
        (is_derived && item_field->field->orig_db_name && db_name &&
         !strcmp(db_name, item_field->field->orig_db_name))
            ? item_field->field->orig_db_name
            : item_base->db_name;
    if (table_name) {
      if (field_name && item_base->table_name &&
          !my_strcasecmp(system_charset_info, field_name,
                         item_base->field_name) &&
          !my_strcasecmp(table_alias_charset, table_name,
                         item_base->table_name) &&
          item_name && item->item_name.ptr() &&
          !strcmp(item_name, item->item_name.ptr()) &&
          (!db_name || (cmp_db_name && !strcmp(db_name, cmp_db_name))) &&
          is_marker_equal) {
        // found one perfect match
        if (db_name) need_alias_item = false;
      } else {
        matched = false;
      }
    }
  } else if (!table_name) {
    need_alias_item = false;
  } else if (table_name && item->type() == Item::REF_ITEM &&
             ((Item_ref *)item)->ref_type() == Item_ref::VIEW_REF) {
    Item_ident *item_ref = (Item_ident *)item;
    if (item_ref->table_name &&
        !my_strcasecmp(table_alias_charset, item_ref->table_name, table_name) &&
        item_ref->item_name.ptr() &&
        !strcmp(item_name, item->item_name.ptr()) &&
        (!field_name || (item_ref->field_name &&
                         !my_strcasecmp(system_charset_info,
                                        item_ref->field_name, field_name))) &&
        (!db_name ||
         (item_ref->db_name && !strcmp(item_ref->db_name, db_name)))) {
      need_alias_item = false;
    } else {
      matched = false;
    }
  }

  // @TODO: add more cases for comparing two resolved item
  return matched;
}

Item **find_item_in_base_items(Ref_item_array ref_item_array,
                               mem_root_deque<Item *> *fields, Item *find,
                               uint &pos_in_ref) {
  uint rank_order = 0, last_idx = 0;
  bool has_found_equal_item = false, need_alias_item = true;
  uint hidden_item_start_position = CountVisibleFields(*fields);

  if (find->hidden) {
    for (auto iter = fields->rbegin(); iter != fields->rend(); ++iter) {
      // found the first matched item
      need_alias_item = true;
      if ((*iter)->hidden) {
        if (items_equal_after_resolve(find, *iter, need_alias_item)) {
          last_idx = rank_order;
          has_found_equal_item = true;
          if (!(*iter)->is_resolved_hidden && !need_alias_item) break;
        }
        rank_order++;
      }
    }
  } else {
    for (auto iter = fields->begin(); iter != fields->end(); ++iter) {
      // found the last matched item, which is consistent with non-parallel
      // query behavior
      need_alias_item = true;
      if (!(*iter)->hidden) {
        if (items_equal_after_resolve(find, *iter, need_alias_item)) {
          assert(rank_order < hidden_item_start_position);
          has_found_equal_item = true;
          last_idx = rank_order;
          if (!need_alias_item) break;
        }
        rank_order++;
      }
    }
  }

  if (has_found_equal_item) {
    pos_in_ref =
        find->hidden ? hidden_item_start_position + last_idx : last_idx;
    ref_item_array[pos_in_ref]->is_resolved_hidden = true;
    return &ref_item_array[pos_in_ref];
  }
  return nullptr;
}

/**
  resolve item in ORDER BY/GROUP BY/HAVING with base_ref_item.
  Requirement: the resolved item must be found in base_ref_item.

  @param[in] thd                    Pointer to current thread structure
  @param[in] ref_item_array         All select, group and order by fields
  @param[in] fields                 List of fields to search in (usually
    SELECT list)
  @param[in] find                   the item need to be resolved
  @param[out] pos_in_ref            the position in base_ref_item

  @retval: found item if Ok and otherwise null
 */

Item **resolve_item_in_base_ref_items(THD *thd, Ref_item_array ref_item_array,
                                      mem_root_deque<Item *> *fields,
                                      uint &pos_in_ref, Item *item) {
  // As leader has resolved all order_items and worker has cloned all
  // resolved fields from leader, "order_item" must can be found in fields.
  // At the same time, unambiguous check can be omitted as this check has
  // been performed in leader.
  Item **found =
      find_item_in_base_items(ref_item_array, fields, item, pos_in_ref);
  // release-version will report PQ error rather than core-dump
  if (!found || (!(*found)->fixed && (*found)->refix_fields(thd, found)))
    return nullptr;
  return found;
}

void Query_block::check_suite_for_pq_after_prepare(THD *thd) {
  if (m_suite_for_pq) {
    MEM_ROOT *mem_root = thd->pq_mem_root;
    thd->pq_mem_root = thd->mem_root;
    ulong saved_privilege = thd->want_privilege;

    if (group_list.elements) {
      if (save_order_properties(thd, &group_list, &saved_group_list_ptrs)) {
        m_suite_for_pq = false;
        goto end;
      }

      check_map_group_to_base = new (thd->pq_mem_root)
          mem_root_unordered_map<uint, uint>(thd->pq_mem_root);
      if (!check_map_group_to_base ||
          record_map_order(group_list, *check_map_group_to_base)) {
        m_suite_for_pq = false;
        goto end;
      }
    }

    if (order_list.elements) {
      if (save_order_properties(thd, &order_list, &saved_order_list_ptrs)) {
        m_suite_for_pq = false;
        goto end;
      }

      check_map_order_to_base = new (thd->pq_mem_root)
          mem_root_unordered_map<uint, uint>(thd->pq_mem_root);
      if (!check_map_order_to_base ||
          record_map_order(order_list, *check_map_order_to_base)) {
        m_suite_for_pq = false;
        goto end;
      }
    }

    // clone where and having condition before optimization. These cloned
    // items use THD::mem_root to allocate memory.
    thd->want_privilege = 0;
    // try to clone item in where condition. Note that setup_cond() in
    // prepare phase may be skipped later in PQ.
    pq_try_clone_item = true;

    if (where_cond() &&
        !(saved_where_cond = where_cond()->pq_clone(thd, this))) {
      m_suite_for_pq = false;
      goto end;
    }

    if (having_cond() &&
        !(saved_having_cond = having_cond()->pq_clone(thd, this))) {
      m_suite_for_pq = false;
      goto end;
    }

  end:
    if (!m_suite_for_pq) pq_unsuite_info = PQUnsuiteInfo::INTER_ERROR;
    pq_try_clone_item = false;
    thd->want_privilege = saved_privilege;
    thd->pq_mem_root = mem_root;
  }
}

bool find_order_in_list_for_pq(THD *thd, Ref_item_array &ref_item_array,
                               ORDER *order, mem_root_deque<Item *> *fields) {
  Item *order_item = *order->item;
  uint pos_in_ref;
  order->item = resolve_item_in_base_ref_items(thd, ref_item_array, fields,
                                               pos_in_ref, order_item);
  if (!order->item) return true;
  // the position of "order_item" in fields
  uint pos_in_fields = fields->size() - 1 - pos_in_ref;
  // set referenced_by, see comments at the end.
  if (order_item->type() == Item::SUM_FUNC_ITEM)
    down_cast<Item_sum *>(order_item)->referenced_by[0] =
        &(*fields)[pos_in_fields];
  return false;
}
