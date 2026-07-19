#ifndef PQ_OPTIMIZER_INCLUDE_H
#define PQ_OPTIMIZER_INCLUDE_H

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

#include "sql/mem_root_array.h"  // Mem_root_array

class THD;
class Query_block;
struct ORDER;
class Item_ref;
class QEP_TAB;

// Defined in sql_lex.h
using Group_list_ptrs = Mem_root_array<ORDER *>;

/* Enumeration for the reasons why parallel query does not take effect. */
enum class PQUnsuiteInfo {
  INFO_NONE,
  INTER_UNSUITE,
  INTER_ERROR,
  SUBQUERY_TRANS,
  UNSUPPORT_COMMAND,
  PREPARE_TRIGER_PROCEDURE,
  HASH_JOIN_SPILL,
  SYS_OR_TMP_TABLE,
  VIEW_OR_DERIVED_TABLE,
  TABLE_FUNCTION_OR_LOCK,
  ROLLUP_WINDS_BUFFER,
  SWITCH_INSERT_SELECT,
  UNSUPPORT_FUNCTION_DATATYPE,
  UNSUPPORT_REF_TYPE,
  REF_WITH_SUBQUERY,
  OUTREF_WITH_SUMITEM,
  AGGR_DISTINCT,
  SUMITEM_OUTREF,
  UNSUPPORTED_SUBQUERY_TYPE,
  COST_OR_LATERAL,
  QUEUE_SATURATED,
  SCAN_RECORDS,
  ZERO_RESULT,
  ORDER_BY_SUBQUERY,
  NO_DIVIDED_TABLE,
  ONLY_FULL_GROUP_BY,
  RETRY_WITHOUT_PQ,
  ROLLBACK,
  SUB_MTIP_WORKER,
  MEMORY_LIMIT,
  IDLE_THREAD,
  DISABLED_IN_PQ_SUPPORT_FEATURES,
  OFFSET_PUSHDOWN_PRIO,
  PQ_COST_HIGHER,
  COUNT_DISTINCT_NOT_SUPPORT_NEED_TEMP,
  LIMIT_NO_ORDERBY,
  UNSUPPORTED_INTERSECT_AND_EXCEPT,
  TMP_TABLE_FIELD_NAME_SAME,
  IMPLICIT_AGG_AFTER_PREPARE,
  HAVING_WITH_OUTER_REF_IN_SUBQUERY,
  ONLY_SUPPORT_INNODB,
  NO_PQ,
  ZERO_DOP,
  HYPERGRAPH_OPTIMIZER,
  // Max enum member
  PQ_MAX_UNSUITE
};

/* @@pq_support_features_switch flags. */
#define PQ_SUPPORT_FEATURES_SWITCH_SIMPLE_AGG (1ULL << 0)
#define PQ_SUPPORT_FEATURES_SWITCH_COUNT_DISTINCT (1ULL << 1)

// Enables Parallel Query for queries containing correlated subqueries
#define PQ_SUPPORT_FEATURES_SWITCH_CORRELATED_SUBQUERY (1ULL << 2)

// Enables spill to disk for parallel hash join.
#define PQ_SUPPORT_FEATURES_SWITCH_HASH_JOIN_SPILL_TO_DISK (1ULL << 3)

// Enables parallel-query for INSERT SELECT statements. Implies that InnoDB
// will not put any row locks on the selected-from table. Requires row-based
// binary logging
#define PQ_SUPPORT_FEATURES_SWITCH_INSERT_SELECT (1ULL << 4)

// Including the switch in this set, makes its default 'on'
static constexpr const unsigned long long PQ_SUPPORT_FEATURES_SWITCH_DEFAULT{
    PQ_SUPPORT_FEATURES_SWITCH_SIMPLE_AGG |
    PQ_SUPPORT_FEATURES_SWITCH_CORRELATED_SUBQUERY};

void check_pq_suite_for_insert_select(THD *thd, Query_block *query_block);
void disable_pq_if_limit_without_orderby(THD *thd, Query_block *query_block,
                                         bool no_order_by,
                                         bool implicit_grouping);
void pq_save_join_group_list(THD *thd, Query_block *query_block,
                             ORDER *old_group_list, bool select_distinct,
                             Group_list_ptrs **join_group_list);
bool pq_not_support_ref(Item_ref *ref);
bool pq_check_not_support_tab(QEP_TAB *tab, bool check_cut_table);
#endif
