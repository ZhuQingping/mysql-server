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

#ifndef PQ_OPTIMIZER_INCLUDED
#define PQ_OPTIMIZER_INCLUDED

/**
  @file sql/parallel_query/pq_optimizer.h
  Parallel Query V1-MVP: Conservative eligibility check and fallback skeleton.

  Phase 1 scope:
  - Define PQUnsuiteReason enum and PQUnsuiteInfo struct.
  - Provide pq_check_query_block_eligible() for read-only eligibility check.
  - All uncertain cases default to fallback serial.
  - No plan rewrite, no worker start, no handler/InnoDB call.
*/

#include "include/my_sqlcommand.h"  // enum_sql_command, SQLCOM_SELECT
#include "sql/handler.h"            // enum_tx_isolation, ISO_SERIALIZABLE,
                                    // ha_legacy_type, DB_TYPE_INNODB
#include "sql/parallel_query/pq_aggregate.h"  // PQ_agg_type

class THD;
class Query_block;
class JOIN;
struct TABLE;
class Table_ref;

/**
  Reason why a query block is not suitable for parallel execution.

  Phase 1 covers a conservative subset. Any check that cannot be reliably
  decided in this phase should return UNSUPPORTED_BY_PHASE1.
*/
enum class PQUnsuiteReason {
  NONE,                     // Eligible (no disqualification reason)
  DISABLED,                 // parallel_query system variable is OFF
  WORKER_THD,               // This THD is already a PQ worker
  HYPERGRAPH_OPTIMIZER,     // Query uses hypergraph optimizer
  NOT_SELECT,               // Statement is not a simple SELECT
  MULTI_QUERY_BLOCK,         // More than one query block (UNION/subquery)
  NO_TABLE,                 // No base tables in query block
  MULTI_TABLE,              // More than one base table
  NON_INNODB,               // Table is not InnoDB
  TEMPORARY_TABLE,          // Table is temporary
  PARTITIONED_TABLE,        // Table is partitioned
  FULLTEXT,                 // Query has full-text search functions
  LOCKING_READ,             // Locking read (FOR UPDATE / LOCK IN SHARE MODE)
  SERIALIZABLE,             // Transaction isolation is SERIALIZABLE
  HAS_SUBQUERY,             // Has correlated/uncorrelated subquery
  HAS_UNION,                // Query expression uses UNION
  HAS_DERIVED_OR_VIEW,      // References derived table or view
  HAS_WINDOW,               // Has window functions
  HAS_DISTINCT,             // Has DISTINCT
  HAS_ORDER_BY,             // Has ORDER BY
  HAS_HAVING,               // Has HAVING clause (conservative reject)
  HAS_GROUP_BY,             // Has GROUP BY (explicit or implicit)
  GROUP_BY_ROLLUP,          // GROUP BY WITH ROLLUP is not supported
  GROUP_BY_HAVING,          // GROUP BY with HAVING is not supported
  GROUP_BY_UNSUPPORTED_EXPR,       // GROUP BY key is not a direct field
  GROUP_BY_UNSUPPORTED_AGGREGATE,  // GROUP BY aggregate is not supported
  GROUP_BY_PARTIAL_AGG_UNSUPPORTED,  // Partial GROUP BY not implemented yet
  HAS_ROLLUP,               // Has WITH ROLLUP
  HAS_SEMIJOIN,             // Has semi-join or anti-join nest
  NON_FULL_TABLE_SCAN,      // Access path is not full table scan
  COST_BELOW_THRESHOLD,     // Estimated cost below parallel_cost_threshold
  UNSUPPORTED_AGGREGATE,    // Has aggregate function not supported by PQ V1
  UNSUPPORTED_BY_PHASE1,    // Check deferred to later phase

  // Keep last - sentinel for array size and iteration
  PQ_UNSUITED_REASON_COUNT
};

/**
  Struct recording why a query block was disqualified from PQ.

  Phase 1 uses static string constants for detail; no dynamic allocation.
*/
struct PQUnsuiteInfo {
  PQUnsuiteReason reason{PQUnsuiteReason::NONE};
  const char *detail{nullptr};

  /// Reset to default state
  void reset() {
    reason = PQUnsuiteReason::NONE;
    detail = nullptr;
  }

  /// True if no disqualification reason recorded
  bool is_eligible() const { return reason == PQUnsuiteReason::NONE; }
};

/**
  Check whether a query block is eligible for parallel execution.

  This is a read-only, pure decision function. It does NOT:
  - Modify the execution plan
  - Start any workers
  - Call any handler/InnoDB PQ API
  - Issue warnings or errors
  - Allocate dynamic memory for workers

  @param thd          Thread context
  @param query_block  The query block being checked
  @param join         The JOIN object (may be nullptr if not yet optimized)
  @param info         Output: filled with disqualification reason if not
                      eligible. Caller must provide a valid pointer.

  @retval true   Query block is a PQ candidate (info->reason == NONE)
  @retval false  Query block must execute serially (info->reason set)
*/
bool pq_check_query_block_eligible(THD *thd, Query_block *query_block,
                                   JOIN *join, PQUnsuiteInfo *info);

/**
  Convert a PQUnsuiteReason to a human-readable string.

  @param reason  The disqualification reason

  @retval  Static string constant (never nullptr)
*/
const char *pq_unsuite_reason_to_string(PQUnsuiteReason reason);

/**
  Return the stable EXPLAIN annotation used for PQ-eligible V1 queries.

  V1 may identify a query as a PQ candidate, but real PQ workers are still
  disabled and TryCreatePQTableScanIterator() falls back to the serial table
  scan path. Keep EXPLAIN explicit so it does not imply true parallel
  execution.
*/
const char *pq_v1_explain_eligible_label();

/**
  Write eligibility result into Phase 0/1 inert fields.

  Helper that sets Query_block::pq_candidate and JOIN::pq_eligible
  based on the eligibility check result.

  NOTE: This function intentionally sets Query_block::pq_unsuite_info to nullptr.
  This clears any stale pointer from re-optimization or subsequent phases.
  The PQUnsuiteInfo object passed to pq_check_query_block_eligible() is
  typically a stack-allocated local in JOIN::optimize(). Storing its address
  in the query block would create a dangling pointer after optimize() returns.

  Starting from Phase 7, the PQUnsuiteReason is persisted in
  JOIN::pq_unsuitable_reason for EXPLAIN visibility.

  @param query_block  The query block to mark
  @param join         The JOIN to mark (may be nullptr)
  @param eligible     Whether the query block is eligible
  @param reason       The PQUnsuiteReason (NONE if eligible)
*/
void pq_mark_query_block_result(Query_block *query_block, JOIN *join,
                                bool eligible, PQUnsuiteReason reason);

#endif  // PQ_OPTIMIZER_INCLUDED
