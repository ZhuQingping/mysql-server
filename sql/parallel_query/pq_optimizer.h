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

enum class PQSavedOrderGroupContractStatus {
  READY,
  UNSUPPORTED_NULL_INPUT,
  UNSUPPORTED_MISSING_SAVED_HELPERS
};

enum class PQOrderByFilesortContractStatus {
  READY,
  UNSUPPORTED_NULL_INPUT,
  UNSUPPORTED_MISSING_SAVED_HELPERS,
  UNSUPPORTED_MISSING_RESTORED_ORDER
};

enum class PQOrderByEligibilityContractStatus {
  UNSUPPORTED_NULL_INPUT,
  UNSUPPORTED_NOT_ORDERED,
  UNSUPPORTED_SHAPE,
  FUTURE_CANDIDATE_EXECUTION_DISABLED,
  EXECUTABLE_UNREACHABLE
};

/**
  Compile-only saved ORDER/GROUP contract shape.

  This is a fail-closed sidecar for the commercial ORDER BY Filesort path. It
  captures only publicly available JOIN / Query_block state in this step. It
  intentionally does not own ORDER nodes, Item objects, or Query_block private
  saved list pointers, and it must not be used to construct Filesort yet.
*/
struct PQSavedOrderGroupContract {
  PQSavedOrderGroupContractStatus status{
      PQSavedOrderGroupContractStatus::UNSUPPORTED_NULL_INPUT};
  const char *detail{nullptr};

  bool has_order{false};
  bool has_group{false};
  bool has_having{false};
  bool grouped{false};
  bool group_optimized_away{false};
  bool implicit_grouping{false};
  bool need_tmp_before_win{false};
  bool simple_group{false};
  bool simple_order{false};
  bool streaming_aggregation{false};
  bool skip_sort_order{false};
  bool select_distinct{false};
  int ordered_index_usage{0};

  void reset() {
    status = PQSavedOrderGroupContractStatus::UNSUPPORTED_NULL_INPUT;
    detail = nullptr;
    has_order = false;
    has_group = false;
    has_having = false;
    grouped = false;
    group_optimized_away = false;
    implicit_grouping = false;
    need_tmp_before_win = false;
    simple_group = false;
    simple_order = false;
    streaming_aggregation = false;
    skip_sort_order = false;
    select_distinct = false;
    ordered_index_usage = 0;
  }

  bool ready() const {
    return status == PQSavedOrderGroupContractStatus::READY;
  }
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
  Build the current saved ORDER/GROUP contract sidecar.

  @retval true   The saved ORDER/GROUP state is complete enough for a later
                 Filesort contract step.
  @retval false  Unsupported or incomplete. Callers must fail closed.
*/
bool pq_build_saved_order_group_contract(
    Query_block *query_block, JOIN *join,
    PQSavedOrderGroupContract *contract);

/** Value-only copy for PQSavedOrderGroupContract sidecar diagnostics. */
bool pq_copy_saved_order_group_contract(
    const PQSavedOrderGroupContract &src,
    PQSavedOrderGroupContract *dst);

/**
  Fail-closed leader Filesort readiness contract.

  This shape does not allocate Filesort, Sort_param, or sort buffers. It only
  records whether the saved ORDER/GROUP sidecar is ready enough for a later
  Filesort lifecycle review.
*/
struct PQOrderByFilesortContract {
  PQOrderByFilesortContractStatus status{
      PQOrderByFilesortContractStatus::UNSUPPORTED_NULL_INPUT};
  const char *detail{nullptr};
  bool has_order{false};
  bool stable_sort_requested{false};
  int ordered_index_usage{0};
  bool saved_order_group_ready{false};
  bool restored_order_ready{false};
  uint restored_order_count{0};
  bool sidecar_clone_ready{false};

  void reset() {
    status = PQOrderByFilesortContractStatus::UNSUPPORTED_NULL_INPUT;
    detail = nullptr;
    has_order = false;
    stable_sort_requested = false;
    ordered_index_usage = 0;
    saved_order_group_ready = false;
    restored_order_ready = false;
    restored_order_count = 0;
    sidecar_clone_ready = false;
  }

  bool ready() const {
    return status == PQOrderByFilesortContractStatus::READY;
  }
};

bool pq_build_orderby_filesort_contract(
    Query_block *query_block, JOIN *join,
    PQOrderByFilesortContract *contract);

struct PQOrderByEligibilityContract {
  PQOrderByEligibilityContractStatus status{
      PQOrderByEligibilityContractStatus::UNSUPPORTED_NULL_INPUT};
  const char *detail{nullptr};

  bool has_order{false};
  bool single_table{false};
  bool simple_order{false};
  bool asc_only{false};
  bool has_limit{false};
  bool select_distinct{false};
  bool has_group{false};
  bool has_having{false};
  bool has_window{false};
  bool filesort_required{false};
  bool full_scan{false};
  bool execution_disabled{true};

  void reset() {
    status = PQOrderByEligibilityContractStatus::UNSUPPORTED_NULL_INPUT;
    detail = nullptr;
    has_order = false;
    single_table = false;
    simple_order = false;
    asc_only = false;
    has_limit = false;
    select_distinct = false;
    has_group = false;
    has_having = false;
    has_window = false;
    filesort_required = false;
    full_scan = false;
    execution_disabled = true;
  }

  bool future_candidate_disabled() const {
    return status ==
               PQOrderByEligibilityContractStatus::
                   FUTURE_CANDIDATE_EXECUTION_DISABLED &&
           execution_disabled;
  }
};

bool pq_build_orderby_eligibility_contract(
    THD *thd, Query_block *query_block, JOIN *join,
    PQOrderByEligibilityContract *contract);

enum class PQOrderByExecutionPreflightStatus {
  UNSUPPORTED_NULL_INPUT,
  BLOCKED_ELIGIBILITY,
  BLOCKED_EXECUTION_DISABLED,
  READY_UNREACHABLE
};

struct PQOrderByExecutionPreflight {
  PQOrderByExecutionPreflightStatus status{
      PQOrderByExecutionPreflightStatus::UNSUPPORTED_NULL_INPUT};
  const char *detail{nullptr};
  PQOrderByEligibilityContract eligibility;

  bool eligibility_candidate_disabled{false};
  bool saved_order_group_runtime_ready{false};
  bool filesort_runtime_ready{false};
  bool sort_param_runtime_ready{false};
  bool worker_order_frame_producer_ready{false};
  bool exchange_sort_heap_read_ready{false};
  bool leader_materialization_ready{false};
  bool rowid_tiebreak_ready{false};
  bool default_ordered_read_ready{false};
  bool kill_detach_error_diagnostics_ready{false};
  bool execution_disabled{true};

  void reset() {
    status = PQOrderByExecutionPreflightStatus::UNSUPPORTED_NULL_INPUT;
    detail = nullptr;
    eligibility.reset();
    eligibility_candidate_disabled = false;
    saved_order_group_runtime_ready = false;
    filesort_runtime_ready = false;
    sort_param_runtime_ready = false;
    worker_order_frame_producer_ready = false;
    exchange_sort_heap_read_ready = false;
    leader_materialization_ready = false;
    rowid_tiebreak_ready = false;
    default_ordered_read_ready = false;
    kill_detach_error_diagnostics_ready = false;
    execution_disabled = true;
  }

  bool blocked_by_execution_disabled() const {
    return status ==
               PQOrderByExecutionPreflightStatus::
                   BLOCKED_EXECUTION_DISABLED &&
           eligibility_candidate_disabled && execution_disabled;
  }

  bool blocked_by_eligibility() const {
    return status == PQOrderByExecutionPreflightStatus::BLOCKED_ELIGIBILITY;
  }
};

bool pq_build_orderby_execution_preflight(
    THD *thd, Query_block *query_block, JOIN *join,
    PQOrderByExecutionPreflight *preflight);

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
