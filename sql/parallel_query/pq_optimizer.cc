/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under
   the terms of the GNU General Public License, version 2.0,
   or the terms of any other license that is available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/parallel_query/pq_optimizer.cc
  Parallel Query V1-MVP: Conservative eligibility check and fallback skeleton.

  Phase 1 implementation: pq_check_query_block_eligible() and helpers.
  All uncertain cases default to serial fallback.
*/

#include "sql/parallel_query/pq_optimizer.h"

#include "include/thr_lock.h"     // Lock_descriptor, thr_lock_type
#include "sql/sql_class.h"        // THD
#include "sql/sql_lex.h"          // Query_block, LEX, Table_ref
#include "sql/sql_opt_exec_shared.h"  // JOIN_TAB, join_type, JT_ALL
#include "sql/sql_optimizer.h"    // JOIN
#include "sql/table.h"            // TABLE, TABLE_SHARE, Table_ref
#include "sql/parallel_query/pq_aggregate.h"  // pq_check_agg_supported

/**
  String representation of each PQUnsuiteReason value.
  Indexed by (int)reason, terminated by PQ_UNSUITED_REASON_COUNT sentinel.
*/
static const char *pq_unsuite_reason_names[] = {
    "NONE",                      // PQUnsuiteReason::NONE
    "DISABLED",                  // PQUnsuiteReason::DISABLED
    "WORKER_THD",                // PQUnsuiteReason::WORKER_THD
    "HYPERGRAPH_OPTIMIZER",      // PQUnsuiteReason::HYPERGRAPH_OPTIMIZER
    "NOT_SELECT",                // PQUnsuiteReason::NOT_SELECT
    "MULTI_QUERY_BLOCK",         // PQUnsuiteReason::MULTI_QUERY_BLOCK
    "NO_TABLE",                  // PQUnsuiteReason::NO_TABLE
    "MULTI_TABLE",               // PQUnsuiteReason::MULTI_TABLE
    "NON_INNODB",                // PQUnsuiteReason::NON_INNODB
    "TEMPORARY_TABLE",           // PQUnsuiteReason::TEMPORARY_TABLE
    "PARTITIONED_TABLE",         // PQUnsuiteReason::PARTITIONED_TABLE
    "FULLTEXT",                  // PQUnsuiteReason::FULLTEXT
    "LOCKING_READ",              // PQUnsuiteReason::LOCKING_READ
    "SERIALIZABLE",              // PQUnsuiteReason::SERIALIZABLE
    "HAS_SUBQUERY",              // PQUnsuiteReason::HAS_SUBQUERY
    "HAS_UNION",                 // PQUnsuiteReason::HAS_UNION
    "HAS_DERIVED_OR_VIEW",       // PQUnsuiteReason::HAS_DERIVED_OR_VIEW
    "HAS_WINDOW",                // PQUnsuiteReason::HAS_WINDOW
    "HAS_DISTINCT",              // PQUnsuiteReason::HAS_DISTINCT
    "HAS_ORDER_BY",              // PQUnsuiteReason::HAS_ORDER_BY
    "HAS_HAVING",                // PQUnsuiteReason::HAS_HAVING
    "HAS_GROUP_BY",              // PQUnsuiteReason::HAS_GROUP_BY
    "HAS_ROLLUP",                // PQUnsuiteReason::HAS_ROLLUP
    "HAS_SEMIJOIN",              // PQUnsuiteReason::HAS_SEMIJOIN
    "NON_FULL_TABLE_SCAN",       // PQUnsuiteReason::NON_FULL_TABLE_SCAN
    "COST_BELOW_THRESHOLD",      // PQUnsuiteReason::COST_BELOW_THRESHOLD
    "UNSUPPORTED_AGGREGATE",     // PQUnsuiteReason::UNSUPPORTED_AGGREGATE
    "UNSUPPORTED_BY_PHASE1",     // PQUnsuiteReason::UNSUPPORTED_BY_PHASE1
};

const char *pq_unsuite_reason_to_string(PQUnsuiteReason reason) {
  int idx = static_cast<int>(reason);
  if (idx >= 0 && idx < static_cast<int>(PQUnsuiteReason::PQ_UNSUITED_REASON_COUNT))
    return pq_unsuite_reason_names[idx];
  return "UNKNOWN";
}

const char *pq_v1_explain_eligible_label() {
  return "eligible, execution disabled, serial fallback";
}

/**
  Helper: set info and return false (not eligible).
  @param info  Output disqualification info
  @param r     The reason
  @param d     Detail string (static constant)
  @retval false  Always returns false (not eligible)
*/
static bool pq_reject(PQUnsuiteInfo *info, PQUnsuiteReason r,
                      const char *d = nullptr) {
  info->reason = r;
  info->detail = d;
  return false;
}

/**
  Check whether a single leaf table meets PQ eligibility requirements.

  MVP eligibility requires:
  - InnoDB engine
  - Non-temporary
  - Non-partitioned
  - Non-fulltext
  - Non-view/derived/CTE

  @param thd         Thread context
  @param table_ref   The Table_ref (leaf table) to check
  @param info        Output: set reason if disqualified

  @retval true   Table passes PQ eligibility
  @retval false  Table is disqualified
*/
static bool pq_check_single_table(Table_ref *table_ref,
                                  PQUnsuiteInfo *info) {
  // Must not be a view or derived table
  if (table_ref->is_view_or_derived()) {
    return pq_reject(info, PQUnsuiteReason::HAS_DERIVED_OR_VIEW,
                     "base table is view or derived");
  }

  TABLE *table = table_ref->table;
  if (table == nullptr) {
    // Table not opened yet - cannot reliably check. Fallback.
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "table not opened");
  }

  TABLE_SHARE *share = table->s;

  // Must be InnoDB engine
  if (ha_legacy_type(share->db_type()) != DB_TYPE_INNODB) {
    return pq_reject(info, PQUnsuiteReason::NON_INNODB,
                     "table engine is not InnoDB");
  }

  // Must not be temporary
  if (share->tmp_table != NO_TMP_TABLE) {
    return pq_reject(info, PQUnsuiteReason::TEMPORARY_TABLE,
                     "table is temporary");
  }

  // Must not be partitioned
  if (share->m_part_info != nullptr) {
    return pq_reject(info, PQUnsuiteReason::PARTITIONED_TABLE,
                     "table is partitioned");
  }

  // Must not have fulltext index involvement
  // Check: if the table has any fulltext key defined, conservatively reject.
  // A more precise check would verify if the query actually uses FTS,
  // but Phase 1 errs on the side of safety.
  for (uint i = 0; i < share->keys; i++) {
    if ((share->key_info[i].flags & HA_FULLTEXT) != 0) {
      return pq_reject(info, PQUnsuiteReason::FULLTEXT,
                       "table has fulltext index");
    }
  }

  return true;  // Table passes all checks
}

/**
  Check whether the query block's access path is a full table scan.

  MVP only supports full table scan on the single base table.
  Any secondary index access, range scan, ref access, ICP, or MRR
  must fallback to serial.

  Phase 7: improved robustness for EXPLAIN paths. We try both
  best_ref[] (available during optimization) and qep_tab[]
  (available after final plan construction). Either source
  that provides a valid JT_ALL type is sufficient to confirm full table scan
  eligibility. Any const, range, ref, index or covering-index path must remain
  serial in V1.

  @param join  The JOIN object (may be nullptr if not yet optimized)
  @param info  Output: set reason if disqualified

  @retval true   Access path is full table scan
  @retval false  Access path is something else or cannot be determined
*/
static bool pq_check_full_table_scan(JOIN *join, PQUnsuiteInfo *info) {
  if (join == nullptr) {
    // JOIN not yet constructed - cannot check access path. Fallback.
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "JOIN not available for access path check");
  }

  // If there are tmp_tables, the query involves materialization steps.
  if (join->tmp_tables > 0) {
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "tmp tables present");
  }

  // Check the access type using best_ref[] (from make_join_plan).
  // best_ref[] is populated after make_join_plan() and contains
  // the optimizer's selected access path for each table. The preceding
  // eligibility checks already require a single base table, so best_ref[0]
  // is enough to catch const, range, index and full-scan decisions.
  join_type access_type = JT_UNKNOWN;

  // Try best_ref[] first (available during optimization).
  if (join->best_ref != nullptr && join->primary_tables > 0 &&
      join->best_ref[0] != nullptr) {
    JOIN_TAB *first_tab = join->best_ref[0];
    access_type = first_tab->type();
  }

  // If best_ref[] didn't give a valid type, try qep_tab[]
  // (available after the final plan is constructed).
  if (access_type == JT_UNKNOWN && join->qep_tab != nullptr &&
      join->primary_tables > 0) {
    access_type = join->qep_tab[0].type();
  }

  // If we couldn't determine the access type from either source,
  // conservatively fallback.
  if (access_type == JT_UNKNOWN) {
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "cannot determine access path type");
  }

  // Only JT_ALL (full table scan) is eligible for PQ in MVP.
  // Any other access type (ref, range, index scan, etc.) must fallback.
  if (access_type != JT_ALL) {
    return pq_reject(info, PQUnsuiteReason::NON_FULL_TABLE_SCAN,
                     pq_unsuite_reason_to_string(
                         PQUnsuiteReason::NON_FULL_TABLE_SCAN));
  }

  return true;  // Passed full table scan check
}

bool pq_check_query_block_eligible(THD *thd, Query_block *query_block,
                                   JOIN *join, PQUnsuiteInfo *info) {
  // Initialize info to eligible state
  info->reset();

  // ================================================================
  // 1. Global/session switch: parallel_query must be ON
  // ================================================================
  if (!thd->variables.parallel_query) {
    return pq_reject(info, PQUnsuiteReason::DISABLED,
                     "parallel_query is OFF");
  }

  // ================================================================
  // 2. This THD must not be a PQ worker (no recursive parallelism)
  // ================================================================
  if (thd->pq_is_worker) {
    return pq_reject(info, PQUnsuiteReason::WORKER_THD,
                     "THD is already a PQ worker");
  }

  // ================================================================
  // 3. Must use traditional optimizer, not hypergraph
  // ================================================================
  if (thd->lex->using_hypergraph_optimizer()) {
    return pq_reject(info, PQUnsuiteReason::HYPERGRAPH_OPTIMIZER,
                     "hypergraph optimizer in use");
  }

  // ================================================================
  // 4. Must be a simple SELECT statement
  // ================================================================
  if (thd->lex->sql_command != SQLCOM_SELECT) {
    return pq_reject(info, PQUnsuiteReason::NOT_SELECT,
                     "statement is not SELECT");
  }

  // ================================================================
  // 5. Must be a simple query block (no UNION, no subquery wrapping)
  // ================================================================
  if (!query_block->is_simple_query_block()) {
    return pq_reject(info, PQUnsuiteReason::MULTI_QUERY_BLOCK,
                     "query has multiple query blocks (UNION/subquery)");
  }

  // ================================================================
  // 6. Must have at least one table
  // ================================================================
  if (!query_block->has_tables()) {
    return pq_reject(info, PQUnsuiteReason::NO_TABLE,
                     "no tables in query block");
  }

  // ================================================================
  // 7. Must have exactly one base table (no joins)
  // ================================================================
  if (query_block->table_count() > 1) {
    return pq_reject(info, PQUnsuiteReason::MULTI_TABLE,
                     "more than one table");
  }

  // ================================================================
  // 8. Must not have DISTINCT
  // ================================================================
  if (query_block->is_distinct()) {
    return pq_reject(info, PQUnsuiteReason::HAS_DISTINCT,
                     "query has DISTINCT");
  }

  // ================================================================
  // 9. Must not have ORDER BY
  // ================================================================
  if (query_block->is_ordered()) {
    return pq_reject(info, PQUnsuiteReason::HAS_ORDER_BY,
                     "query has ORDER BY");
  }

  // ================================================================
  // 10. Must not have explicit GROUP BY
  //     Phase 8: explicit GROUP BY is still serial fallback.
  //     Explicit GROUP BY with aggregates requires gather-merge or
  //     partial aggregation per group, which is Phase 9 territory.
  // ================================================================
  if (query_block->is_explicitly_grouped()) {
    return pq_reject(info, PQUnsuiteReason::HAS_GROUP_BY,
                     "query has GROUP BY");
  }

  // ================================================================
  // 11. Implicit grouping (aggregate without GROUP BY):
  //     Phase 8 expansion: allow implicit grouping if all aggregates
  //     are PQ-compatible (COUNT, SUM, AVG, MIN, MAX).
  //     Unsupported aggregates (GROUP_CONCAT, STD, VAR, BIT_AND, etc.)
  //     still fall back to serial.
  // ================================================================
  if (query_block->is_implicitly_grouped()) {
    // Check whether all aggregate Items are PQ-compatible.
    if (!pq_check_agg_supported(query_block)) {
      return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_AGGREGATE,
                       "query has unsupported aggregate function");
    }
    // All aggregates are PQ-compatible. Continue eligibility check.
    // Note: implicit grouping queries are eligible but still fall
    // back to serial execution because TryCreatePQTableScanIterator
    // returns nullptr in Phase 8 (no real workers). Phase 6+ will
    // activate parallel aggregation when InnoDB PQ scan is ready.
  }

  // ================================================================
  // 11b. Must not have HAVING clause.
  //      Phase 8: conservatively reject HAVING because:
  //      - HAVING can reference aggregates that are NOT in the SELECT
  //        list (hidden aggregates), which pq_check_agg_supported()
  //        would miss if it only checks visible_fields().
  //      - HAVING is evaluated after aggregation; PQ V1 workers
  //        send partial aggregates, and the leader must apply HAVING
  //        after combining them, which is Phase 9 territory.
  // ================================================================
  if (query_block->having_cond() != nullptr) {
    return pq_reject(info, PQUnsuiteReason::HAS_HAVING,
                     "query has HAVING clause");
  }

  // ================================================================
  // 12. Must not have WITH ROLLUP
  // ================================================================
  // olap_type is defined in parser_yystype.h:
  //   enum olap_type { UNSPECIFIED_OLAP_TYPE, ROLLUP_TYPE };
  if (query_block->olap != UNSPECIFIED_OLAP_TYPE) {
    return pq_reject(info, PQUnsuiteReason::HAS_ROLLUP,
                     "query has WITH ROLLUP");
  }

  // ================================================================
  // 13. Must not have window functions
  // ================================================================
  if (query_block->has_windows()) {
    return pq_reject(info, PQUnsuiteReason::HAS_WINDOW,
                     "query has window functions");
  }

  // ================================================================
  // 14. Must not have full-text functions
  // ================================================================
  if (query_block->has_ft_funcs()) {
    return pq_reject(info, PQUnsuiteReason::FULLTEXT,
                     "query has full-text search");
  }

  // ================================================================
  // 15. Must not have semi-join or anti-join nests
  //     has_sj_candidates() checks for subqueries that are
  //     semi-join candidates during resolution. If any exist,
  //     conservatively reject. If none, but we cannot fully
  //     verify (sj_nests is updated during optimization), we
  //     still proceed because single-table queries should not
  //     have semi-joins.
  // ================================================================
  if (query_block->has_sj_candidates()) {
    return pq_reject(info, PQUnsuiteReason::HAS_SEMIJOIN,
                     "query has semi-join candidates");
  }

  // ================================================================
  // 16. Must not reference derived tables, views, or CTEs in the
  //     leaf table list (this overlaps with single-table check below,
  //     but we check the leaf list explicitly too)
  // ================================================================
  for (Table_ref *tr = query_block->leaf_tables; tr != nullptr;
       tr = tr->next_leaf) {
    if (tr->is_view_or_derived()) {
      return pq_reject(info, PQUnsuiteReason::HAS_DERIVED_OR_VIEW,
                       "leaf table is view or derived");
    }
  }

  // ================================================================
  // 17. Transaction isolation must not be SERIALIZABLE
  // ================================================================
  if (thd->tx_isolation == ISO_SERIALIZABLE) {
    return pq_reject(info, PQUnsuiteReason::SERIALIZABLE,
                     "transaction isolation is SERIALIZABLE");
  }

  // ================================================================
  // 18. Must not be a locking read (FOR UPDATE / LOCK IN SHARE MODE)
  //     Check lock_descriptor() on all leaf tables AND check
  //     the top-level table list for lock descriptors.
  //     A locking read uses TL_READ_WITH_SHARED_LOCKS or stronger.
  //     For single-table queries, the leaf table and top-level table
  //     should be the same Table_ref, but we check both to be safe.
  // ================================================================
  for (Table_ref *tr = query_block->leaf_tables; tr != nullptr;
       tr = tr->next_leaf) {
    thr_lock_type lt = tr->lock_descriptor().type;
    // Check both lock_descriptor() and updating flag.
    // For FOR UPDATE, lock_descriptor().type should be TL_WRITE
    // and updating should be true. For LOCK IN SHARE MODE,
    // lock_descriptor().type should be TL_READ_WITH_SHARED_LOCKS.
    if (lt >= TL_READ_WITH_SHARED_LOCKS || tr->updating) {
      return pq_reject(info, PQUnsuiteReason::LOCKING_READ,
                       "locking read detected");
    }
  }

  // Also check the top-level table list (for cases where
  // leaf_tables may not have lock descriptors propagated).
  for (Table_ref *tr = query_block->get_table_list(); tr != nullptr;
       tr = tr->next_local) {
    thr_lock_type lt = tr->lock_descriptor().type;
    if (lt >= TL_READ_WITH_SHARED_LOCKS || tr->updating) {
      return pq_reject(info, PQUnsuiteReason::LOCKING_READ,
                       "locking read detected");
    }
  }

  // ================================================================
  // 19. Single base table must pass InnoDB/temporary/partition/FTS
  //     checks
  // ================================================================
  Table_ref *single_table = query_block->get_table_list();
  if (!pq_check_single_table(single_table, info)) {
    return false;
  }

  // ================================================================
  // 20. Access path must be full table scan (MVP only supports scan)
  // ================================================================
  if (!pq_check_full_table_scan(join, info)) {
    return false;
  }

  // ================================================================
  // 21. Estimated cost must reach parallel_cost_threshold
  // ================================================================
  if (join != nullptr) {
    if (join->best_read < static_cast<double>(
            thd->variables.parallel_cost_threshold)) {
      return pq_reject(info, PQUnsuiteReason::COST_BELOW_THRESHOLD,
                       "estimated cost below threshold");
    }
  } else {
    // JOIN not yet optimized - cannot check cost. Fallback.
    return pq_reject(info, PQUnsuiteReason::UNSUPPORTED_BY_PHASE1,
                     "JOIN not available for cost check");
  }

  // ================================================================
  // All checks passed - eligible for PQ (within Phase 1 MVP scope)
  // ================================================================
  info->reason = PQUnsuiteReason::NONE;
  info->detail = nullptr;
  return true;
}

/**
  Write eligibility result into Query_block and JOIN boolean fields.

  Does NOT set Query_block::pq_unsuite_info because the PQUnsuiteInfo
  object is typically stack-allocated and would become a dangling pointer.
  Reason storage is deferred until PQUnsuiteInfo can be allocated on a
  long-lived MEM_ROOT.
*/
void pq_mark_query_block_result(Query_block *query_block, JOIN *join,
                                bool eligible,
                                PQUnsuiteReason reason) {
  if (query_block == nullptr) return;

  query_block->pq_candidate = eligible;

  // Explicitly clear pq_unsuite_info to avoid stale pointers from
  // re-optimization or subsequent phases. Reason storage is deferred
  // until PQUnsuiteInfo can be allocated on a long-lived MEM_ROOT.
  query_block->pq_unsuite_info = nullptr;

  if (join != nullptr) {
    join->pq_eligible = eligible;
    join->pq_unsuitable_reason = reason;
  }
}
