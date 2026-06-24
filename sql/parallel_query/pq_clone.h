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
struct TABLE;
struct ORDER;
class ORDER_with_src;

AccessPath *CopyRangeScanAccessPath(THD *thd, AccessPath *orig_path,
                                    TABLE *table);

ORDER *pq_dup_order(THD *thd, Query_block *select, ORDER *orig);

bool pq_dup_tabs(JOIN *pq_join, JOIN *join, bool gather);

JOIN *pq_make_join(THD *thd, JOIN *join);

bool pq_dup_tabs_skeleton_preflight(JOIN *worker_join, JOIN *leader_join);

bool pq_bind_qep_tab_table_preflight(JOIN *worker_join, TABLE *worker_table,
                                     TABLE *leader_table);

struct PQ_qep_tab_table_attach_state {
  QEP_TAB *tab{nullptr};
  TABLE *worker_table{nullptr};
  Table_ref *worker_ref{nullptr};
  Query_block *saved_ref_query_block{nullptr};
  QEP_TAB *saved_worker_qep_tab{nullptr};
  bool attached{false};
};

bool pq_attach_qep_tab_table_smoke(JOIN *worker_join, TABLE *worker_table,
                                   TABLE *leader_table,
                                   PQ_qep_tab_table_attach_state *state);

void pq_detach_qep_tab_table_smoke(PQ_qep_tab_table_attach_state *state);

bool pq_clone_table_ref_preflight(THD *worker_thd, Table_ref *leader_ref);

bool pq_clone_table_scalar_preflight(THD *worker_thd, TABLE *worker_table,
                                     TABLE *leader_table);

bool pq_clone_position_scalar_preflight(QEP_TAB *leader_tab);

bool pq_clone_qep_tab_scalar_preflight(JOIN *worker_join,
                                       QEP_TAB *leader_tab);

bool pq_clone_range_scan_preflight(THD *worker_thd, TABLE *worker_table,
                                   QEP_TAB *leader_tab);

/**
  Run the M11-A clone contract preflight.

  The helper verifies whether the current statement has enough local contract
  support to create the first non-executable commercial pq_make_join() shell.
  It does not prove worker-plan execution.

  @retval false  Clone contract is not executable yet.
  @retval true   A guarded caller may create and immediately destroy a shell.
*/
bool pq_clone_contract_preflight(THD *thd, JOIN *join);

/**
  Run the M3 commercial clone activation probe.

  This is intentionally a diagnostic-only probe. It may create and immediately
  destroy a non-executable cloned JOIN shell, but it must not store a cloned
  JOIN, create a Gather_operator, start workers, or call handler/InnoDB.

  The caller may continue into the existing V2 smoke/fallback iterator path
  after this probe. This helper owns only clone-probe diagnostics.

  @retval false  Probe completed without fatal error; serial execution remains.
  @retval true   Fatal local error such as OOM.
*/
bool pq_clone_activation_probe(THD *thd, JOIN *join);

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
