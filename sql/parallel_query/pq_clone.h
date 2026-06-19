#ifndef PQ_CLONE_INCLUDE_H
#define PQ_CLONE_INCLUDE_H

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

#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/table.h"

class Item;
class Item_ident;
class THD;
class Query_block;
class JOIN;
struct ORDER;
class ORDER_with_src;

AccessPath *CopyRangeScanAccessPath(THD *thd, AccessPath *orig_path,
                                    TABLE *table);

ORDER *pq_dup_order(THD *thd, Query_block *select, ORDER *orig);

bool pq_dup_tabs(JOIN *pq_join, JOIN *join, bool gather);

JOIN *pq_make_join(THD *thd, JOIN *join);

extern void swap_column_names_of_unit_and_tmp_table(
    const mem_root_deque<Item *> &unit_items,
    const Create_col_name_list &tmp_table_col_names);

void reset_avg_property(mem_root_deque<Item *> &items);

bool pq_replace_base_item(Query_block *select);

bool check_resolved_order_item(
    const SQL_I_List<ORDER> &list, const Ref_item_array &base_items,
    const mem_root_unordered_map<uint, uint> &map_order);

std::vector<Query_block *> list_query_blocks_to_clone(Query_block *top_sl);

/// Similar to Change_current_select, but also pushes the
/// Name_resolution_context, conditionally if this is a subquery (if this is
/// the root query, the current_select and context are already in place).
class Switch_for_resolution_of_query_block {
 public:
  Switch_for_resolution_of_query_block(LEX *lex, Query_block *query_block,
                                       bool entering_subquery)
      : m_active(entering_subquery) {
    if (m_active) {
      m_lex = lex;
      m_saved_query_block = m_lex->current_query_block();
      m_lex->set_current_query_block(query_block);
      m_lex->push_context(&query_block->context);
    }
  }
  ~Switch_for_resolution_of_query_block() {
    if (m_active) {
      m_lex->pop_context();
      m_lex->set_current_query_block(m_saved_query_block);
    }
  }

 private:
  LEX *m_lex;
  Query_block *m_saved_query_block;
  bool m_active;
};

#endif  // PQ_CLONE_INCLUDE_H
