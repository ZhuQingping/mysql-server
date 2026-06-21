/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under the
   terms of the GNU General Public License, version 2.0,
   or the terms of any other license of the available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_PARALLEL_PQ_INCLUDED
#define SQL_PARALLEL_PQ_INCLUDED

/**
  @file sql/parallel_query/sql_parallel.h
  Parallel Query V1-MVP: Gather operator, worker info, worker manager skeleton.

  Phase 4 scope:
  - Define Gather_operator: holds DOP, worker array, Exchange pointer,
    error state, completion state.
  - Define PQ_worker_info: per-worker metadata (id, status, THD pointer,
    error code).
  - Define PQ_worker_manager: interface for start / wait / abort / cleanup.
  - Reserve Exchange_nosort hookup field in Gather_operator.
  - Error priority: kill > leader fatal/OOM > worker fatal > MQ closed >
    normal finish.
  - All interfaces default inert: not called unless explicitly invoked.
  - No optimizer integration, no execution path change, no handler/InnoDB call.

  Design notes:
  - Gather_operator is the leader-side orchestrator. It creates workers,
    wires up Exchange_nosort, and collects results via round-robin MQ reads.
  - PQ_worker_info tracks each worker's state. Phase 4 stub does NOT
    create real OS threads; it only provides the interface and metadata.
  - PQ_worker_manager provides start/wait/abort/cleanup stubs. These stubs
    are inert and must not be called from any existing execution path.
  - When Phase 5/6 connect workers to real threads, PQ_worker_manager::start()
    will create a worker THD and launch it; currently it returns immediately.
  - Memory allocation uses new[]/delete[] for Phase 4 skeleton. Phase 5/6
    must switch to pq_mem_root (THD::pq_mem_root) for production use.

  THD integration:
  - Gather_operator is referenced via THD::pq_leader (void* opaque pointer).
  - PQ_worker_info is referenced via THD::pq_worker_info (void* opaque pointer).
  - Phase 4 does NOT modify THD initialization or destruction flow; the
    opaque pointers remain nullptr until Phase 5 explicitly sets them.
  - THD::pq_dop, THD::pq_error, THD::pq_mem_root are Phase 0 inert fields
    that Gather_operator reads but does not modify during Phase 4.

  Error priority (Phase A contract):
  1. KILL           -- THD::killed != 0 (leader or worker killed)
  2. LEADER_FATAL   -- leader OOM or unrecoverable error
  3. WORKER_FATAL   -- a worker hit a fatal error (e.g., InnoDB crash)
  4. MQ_CLOSED      -- all MQ producers detached (normal end-of-data)
  5. NORMAL_FINISH  -- all workers finished without error

  This priority determines what Gather_operator::wait() reports to the
  leader THD and what error code propagates to THD::pq_error.
*/

#include "sql/parallel_query/exchange.h"
#include "sql/parallel_query/pq_handler.h"

#include <atomic>
#include <cstdint>

#include "my_thread.h"

class THD;
class Gather_operator;
struct TABLE;

// ---------------------------------------------------------------------------
// PQ_execution_state: per-statement execution state contract
// ---------------------------------------------------------------------------

/**
  Per-statement PQ execution state.

  This execution-side contract is separate from optimizer eligibility fields
  such as JOIN::pq_eligible. Eligibility means the optimizer found a PQ
  candidate; execution state records whether the execution layer selected and
  ran PQ.
*/
enum class PQ_execution_state : uint {
  DISABLED = 0,
  NOT_ELIGIBLE,
  ELIGIBLE,
  ITERATOR_SELECTED,
  EXECUTED,
  FALLBACK_SERIAL
};

/** Convert PQ execution state to a stable diagnostic string. */
const char *pq_execution_state_to_string(PQ_execution_state state);

/** Return a stable integer representation for storage in THD. */
uint pq_execution_state_to_uint(PQ_execution_state state);

/** Convert a THD-stored integer to PQ execution state. */
PQ_execution_state pq_execution_state_from_uint(uint state);

/** Set the current statement's PQ execution state on a THD. */
void pq_set_execution_state(THD *thd, PQ_execution_state state);

/**
  Worker task selected before a scaffold thread is started.

  NOOP preserves the existing worker lifecycle smoke behavior. Other tasks are
  debug-gated execution experiments and must be configured by Gather_operator
  before PQ_worker_manager::start() creates the worker thread.
*/
enum class PQ_worker_task : uint {
  NOOP = 0,
  CALLBACK_LIMITED_PRODUCER,
  QUERY_RESULT_MQ_PROBE
};

// ---------------------------------------------------------------------------
// PQ_stats: resource statistics for parallel query execution
// ---------------------------------------------------------------------------

/**
  Per-query PQ execution statistics.

  PQ_stats tracks resource usage during a parallel query execution.
  It is attached to THD::pq_leader (via Gather_operator) and records:
  - Number of workers launched
  - Rows scanned by all workers
  - Execution time
  - Whether PQ was actually executed or fell back to serial

  Phase 8: PQ_stats is defined but not populated by a real PQ execution path
  yet. Real execution stats require Phase 6+ when workers are actually
  launched.
*/
struct PQ_stats {
  uint32 workers_launched{0};      ///< Number of workers actually launched
  uint64 rows_scanned{0};          ///< Total rows scanned by all workers
  uint64 rows_returned{0};         ///< Total rows returned to leader
  uint64 scan_time_us{0};          ///< Time spent in parallel scan (microseconds)
  bool pq_executed{false};         ///< True if PQ actually ran (not fallback)
  bool pq_fallback{false};         ///< True if fell back to serial

  /** Reset all counters. */
  void reset() {
    workers_launched = 0;
    rows_scanned = 0;
    rows_returned = 0;
    scan_time_us = 0;
    pq_executed = false;
    pq_fallback = false;
  }
};

// ---------------------------------------------------------------------------
// PQ global statistics counters
// ---------------------------------------------------------------------------

/**
  Global PQ statistics counters, visible via SHOW STATUS.

  These are atomic counters that track aggregate PQ usage across
  all sessions. Updated at the end of each PQ query execution
  (or fallback). Displayed as SHOW STATUS LIKE 'Parallel%'.

  Phase 8: fallback is incremented when the execution iterator factory
  reaches a PQ-eligible table scan but returns nullptr because real worker
  execution is not enabled yet. Real execution counters require Phase 6+
  when workers are actually launched.
*/
struct PQ_global_stats {
  std::atomic<uint64> queries_executed{0};    ///< PQ queries that actually ran
  std::atomic<uint64> queries_fallback{0};    ///< PQ-eligible queries that fell back
  std::atomic<uint64> clone_probe_attempts{0};  ///< Clone probe attempts
  std::atomic<uint64> clone_probe_success{0};   ///< Clone probe successes
  std::atomic<uint64> clone_probe_fallback{0};  ///< Clone probe fallback
  std::atomic<uint64> clone_probe_unsupported{0};  ///< Missing clone contract
  std::atomic<uint64> clone_preflight_attempts{0};  ///< Contract preflights
  std::atomic<uint64> clone_preflight_unsupported{0};  ///< Contract rejects
  std::atomic<uint64> workers_launched{0};    ///< Total worker threads launched
  std::atomic<uint64> rows_scanned{0};        ///< Total rows scanned by PQ workers
  std::atomic<uint64> parallel_scan_lifecycle_smoke_attempts{0};  ///< DBUG smoke
  std::atomic<uint64> parallel_scan_lifecycle_fail_closed{0};  ///< Fail-closed
  std::atomic<uint64> parallel_scan_lifecycle_cleanup_calls{0};  ///< Cleanup calls
  std::atomic<uint64> probe_attempts{0};      ///< Handler PROBE attempts
  std::atomic<uint64> probe_success{0};       ///< Handler PROBE successes
  std::atomic<uint64> probe_unsupported{0};   ///< Handler PROBE unsupported
  std::atomic<uint64> probe_gate_unsupported{0};  ///< PROBE basic gate rejects
  std::atomic<uint64> probe_thread_budget_unsupported{0};  ///< PROBE thread budget rejects
  std::atomic<uint64> probe_init_unsupported{0};  ///< PROBE range init rejects
  std::atomic<uint64> ranges_built{0};        ///< Total InnoDB PQ ranges planned
  std::atomic<uint64> ranges_dispatched{0};   ///< InnoDB PQ ranges assigned
  std::atomic<uint64> empty_worker_ranges{0}; ///< Workers assigned no range
  std::atomic<uint64> secondary_range_probe_attempts{0};  ///< Secondary range probes
  std::atomic<uint64> secondary_range_probe_unsupported{0};  ///< Unsupported probes
  std::atomic<uint64> secondary_range_clone_attempts{0};  ///< Range clone probes
  std::atomic<uint64> secondary_range_clone_failed{0};  ///< Range clone failures
  std::atomic<uint64> secondary_ranges_built{0};  ///< Secondary ranges built
  std::atomic<uint64> secondary_rows_produced{0};  ///< Secondary rows produced
  std::atomic<uint64> secondary_rows_materialized_smoke{0};  ///< Mat smoke rows
  std::atomic<uint64> secondary_record_buffer_null_probes{0};  ///< Native RB null
  std::atomic<uint64> secondary_record_buffer_nonnull_probes{0};  ///< Native RB set
  std::atomic<uint64> secondary_visibility_attempts{0};  ///< Visibility probes
  std::atomic<uint64> secondary_visibility_supported{0};  ///< Fast-path ok
  std::atomic<uint64> secondary_visibility_unsupported{0};  ///< Unsupported
  std::atomic<uint64> secondary_ref_probe_attempts{0};  ///< Dep ref probes
  std::atomic<uint64> secondary_ref_probe_unsupported{0};  ///< Unsupported ref
  std::atomic<uint64> secondary_ref_empty_probes{0};  ///< Ref probes with no row
  std::atomic<uint64> secondary_ref_fallback_probes{0};  ///< Probe fallbacks
  std::atomic<uint64> secondary_ref_rows_produced{0};  ///< Dep ref rows
  std::atomic<uint64> worker_smoke_runs{0};   ///< Worker lifecycle smoke runs
  std::atomic<uint64> worker_producer_smoke_runs{0};  ///< Producer loop smoke runs
  std::atomic<uint64> worker_open_smoke_runs{0};  ///< Worker THD/TABLE smoke runs
  std::atomic<uint64> worker_handler_smoke_runs{0};  ///< Handler init/end smoke runs
  std::atomic<uint64> worker_attach_smoke_attempts{0};  ///< Attach smoke runs
  std::atomic<uint64> worker_attach_smoke_success{0};  ///< Attach smoke ok
  std::atomic<uint64> worker_attach_smoke_cleanup_calls{0};  ///< Attach cleanup
  std::atomic<uint64> worker_result_smoke_rows{0};  ///< Worker result frames
  std::atomic<uint64> worker_result_smoke_finishes{0};  ///< FINISH frames
  std::atomic<uint64> worker_result_smoke_errors{0};  ///< ERROR frames
  std::atomic<uint64> worker_result_smoke_workers{0};  ///< Smoke workers
  std::atomic<uint64> exchange_smoke_rows{0};      ///< Synthetic MQ rows read
  std::atomic<uint64> exchange_smoke_finishes{0};  ///< Synthetic FINISH tokens
  std::atomic<uint64> exchange_row_image_smoke_rows{0};  ///< Row-image smoke rows
  std::atomic<uint64> exchange_row_image_smoke_finishes{0};  ///< Row-image FINISH
  std::atomic<uint64> exchange_partial_group_smoke_rows{0};  ///< Partial groups
  std::atomic<uint64> exchange_partial_group_smoke_finishes{0};  ///< FINISH
  std::atomic<uint64> exchange_sort_smoke_runs{0};  ///< ORDER BY merge smoke
  std::atomic<uint64> exchange_sort_smoke_rows{0};  ///< ORDER BY smoke rows
  std::atomic<uint64> groupby_dop1_factory_attempts{0};  ///< GROUP BY hook
  std::atomic<uint64> groupby_dop1_factory_selected{0};  ///< PQ wrapper selected
  std::atomic<uint64> groupby_dop1_native_delegate_executed{0};  ///< Native delegate
  std::atomic<uint64> groupby_dop1_temp_table_executed{0};  ///< PQ temp-table path
  std::atomic<uint64> groupby_dop1_typed_count_executed{0};  ///< Typed COUNT path
  std::atomic<uint64> groupby_dop1_typed_minmax_executed{0};  ///< Typed MIN/MAX
  std::atomic<uint64> groupby_dop1_typed_sum_executed{0};  ///< Typed SUM path
  std::atomic<uint64> groupby_dop1_factory_fallback{0};  ///< Native fallback
  std::atomic<uint64> groupby_dop_partial_attempts{0};  ///< DOP partial hook
  std::atomic<uint64> groupby_dop_partial_selected{0};  ///< DOP partial selected
  std::atomic<uint64> groupby_dop_partial_worker_groups{0};  ///< Worker groups
  std::atomic<uint64> groupby_dop_partial_merged_groups{0};  ///< Merged groups
  std::atomic<uint64> groupby_dop_partial_fallback{0};  ///< DOP partial fallback
  std::atomic<uint64> groupby_commercial_attempts{0};  ///< Commercial agg probe
  std::atomic<uint64> groupby_commercial_selected{0};  ///< Commercial selected
  std::atomic<uint64> groupby_commercial_executed{0};  ///< Commercial executed
  std::atomic<uint64> groupby_commercial_fallback{0};  ///< Commercial fallback
  std::atomic<uint64> groupby_legacy_typed_selected{0};  ///< Legacy typed selected
  std::atomic<uint64> groupby_legacy_typed_executed{0};  ///< Legacy typed executed
  std::atomic<uint64> groupby_legacy_typed_fallback{0};  ///< Legacy typed fallback
  std::atomic<uint64> groupby_partial_payload_errors{0};  ///< Payload errors
  std::atomic<uint64> groupby_temp_shape_supported{0};  ///< Temp shape ok
  std::atomic<uint64> groupby_temp_shape_unsupported{0};  ///< Temp shape reject
  std::atomic<uint64> groupby_typed_smoke_groups{0};  ///< Typed groups built
  std::atomic<uint64> groupby_typed_smoke_sum{0};     ///< Typed SUM check
  std::atomic<uint64> callback_smoke_attempts{0};  ///< Callback smoke attempts
  std::atomic<uint64> callback_smoke_rows{0};      ///< Callback converted rows

  /** Reset all counters. */
  void reset() {
    queries_executed.store(0, std::memory_order_relaxed);
    queries_fallback.store(0, std::memory_order_relaxed);
    clone_probe_attempts.store(0, std::memory_order_relaxed);
    clone_probe_success.store(0, std::memory_order_relaxed);
    clone_probe_fallback.store(0, std::memory_order_relaxed);
    clone_probe_unsupported.store(0, std::memory_order_relaxed);
    clone_preflight_attempts.store(0, std::memory_order_relaxed);
    clone_preflight_unsupported.store(0, std::memory_order_relaxed);
    workers_launched.store(0, std::memory_order_relaxed);
    rows_scanned.store(0, std::memory_order_relaxed);
    parallel_scan_lifecycle_smoke_attempts.store(
        0, std::memory_order_relaxed);
    parallel_scan_lifecycle_fail_closed.store(0, std::memory_order_relaxed);
    parallel_scan_lifecycle_cleanup_calls.store(0,
                                                std::memory_order_relaxed);
    probe_attempts.store(0, std::memory_order_relaxed);
    probe_success.store(0, std::memory_order_relaxed);
    probe_unsupported.store(0, std::memory_order_relaxed);
    probe_gate_unsupported.store(0, std::memory_order_relaxed);
    probe_thread_budget_unsupported.store(0, std::memory_order_relaxed);
    probe_init_unsupported.store(0, std::memory_order_relaxed);
    ranges_built.store(0, std::memory_order_relaxed);
    ranges_dispatched.store(0, std::memory_order_relaxed);
    empty_worker_ranges.store(0, std::memory_order_relaxed);
    secondary_range_probe_attempts.store(0, std::memory_order_relaxed);
    secondary_range_probe_unsupported.store(0, std::memory_order_relaxed);
    secondary_range_clone_attempts.store(0, std::memory_order_relaxed);
    secondary_range_clone_failed.store(0, std::memory_order_relaxed);
    secondary_ranges_built.store(0, std::memory_order_relaxed);
    secondary_rows_produced.store(0, std::memory_order_relaxed);
    secondary_rows_materialized_smoke.store(0, std::memory_order_relaxed);
    secondary_record_buffer_null_probes.store(0, std::memory_order_relaxed);
    secondary_record_buffer_nonnull_probes.store(0, std::memory_order_relaxed);
    secondary_visibility_attempts.store(0, std::memory_order_relaxed);
    secondary_visibility_supported.store(0, std::memory_order_relaxed);
    secondary_visibility_unsupported.store(0, std::memory_order_relaxed);
    secondary_ref_probe_attempts.store(0, std::memory_order_relaxed);
    secondary_ref_probe_unsupported.store(0, std::memory_order_relaxed);
    secondary_ref_empty_probes.store(0, std::memory_order_relaxed);
    secondary_ref_fallback_probes.store(0, std::memory_order_relaxed);
    secondary_ref_rows_produced.store(0, std::memory_order_relaxed);
    worker_smoke_runs.store(0, std::memory_order_relaxed);
    worker_producer_smoke_runs.store(0, std::memory_order_relaxed);
    worker_open_smoke_runs.store(0, std::memory_order_relaxed);
    worker_handler_smoke_runs.store(0, std::memory_order_relaxed);
    worker_attach_smoke_attempts.store(0, std::memory_order_relaxed);
    worker_attach_smoke_success.store(0, std::memory_order_relaxed);
    worker_attach_smoke_cleanup_calls.store(0, std::memory_order_relaxed);
    worker_result_smoke_rows.store(0, std::memory_order_relaxed);
    worker_result_smoke_finishes.store(0, std::memory_order_relaxed);
    worker_result_smoke_errors.store(0, std::memory_order_relaxed);
    worker_result_smoke_workers.store(0, std::memory_order_relaxed);
    exchange_smoke_rows.store(0, std::memory_order_relaxed);
    exchange_smoke_finishes.store(0, std::memory_order_relaxed);
    exchange_row_image_smoke_rows.store(0, std::memory_order_relaxed);
    exchange_row_image_smoke_finishes.store(0, std::memory_order_relaxed);
    exchange_partial_group_smoke_rows.store(0, std::memory_order_relaxed);
    exchange_partial_group_smoke_finishes.store(0, std::memory_order_relaxed);
    exchange_sort_smoke_runs.store(0, std::memory_order_relaxed);
    exchange_sort_smoke_rows.store(0, std::memory_order_relaxed);
    groupby_dop1_factory_attempts.store(0, std::memory_order_relaxed);
    groupby_dop1_factory_selected.store(0, std::memory_order_relaxed);
    groupby_dop1_native_delegate_executed.store(0, std::memory_order_relaxed);
    groupby_dop1_temp_table_executed.store(0, std::memory_order_relaxed);
    groupby_dop1_typed_count_executed.store(0, std::memory_order_relaxed);
    groupby_dop1_typed_minmax_executed.store(0, std::memory_order_relaxed);
    groupby_dop1_typed_sum_executed.store(0, std::memory_order_relaxed);
    groupby_dop1_factory_fallback.store(0, std::memory_order_relaxed);
    groupby_dop_partial_attempts.store(0, std::memory_order_relaxed);
    groupby_dop_partial_selected.store(0, std::memory_order_relaxed);
    groupby_dop_partial_worker_groups.store(0, std::memory_order_relaxed);
    groupby_dop_partial_merged_groups.store(0, std::memory_order_relaxed);
    groupby_dop_partial_fallback.store(0, std::memory_order_relaxed);
    groupby_commercial_attempts.store(0, std::memory_order_relaxed);
    groupby_commercial_selected.store(0, std::memory_order_relaxed);
    groupby_commercial_executed.store(0, std::memory_order_relaxed);
    groupby_commercial_fallback.store(0, std::memory_order_relaxed);
    groupby_legacy_typed_selected.store(0, std::memory_order_relaxed);
    groupby_legacy_typed_executed.store(0, std::memory_order_relaxed);
    groupby_legacy_typed_fallback.store(0, std::memory_order_relaxed);
    groupby_partial_payload_errors.store(0, std::memory_order_relaxed);
    groupby_temp_shape_supported.store(0, std::memory_order_relaxed);
    groupby_temp_shape_unsupported.store(0, std::memory_order_relaxed);
    groupby_typed_smoke_groups.store(0, std::memory_order_relaxed);
    groupby_typed_smoke_sum.store(0, std::memory_order_relaxed);
    callback_smoke_attempts.store(0, std::memory_order_relaxed);
    callback_smoke_rows.store(0, std::memory_order_relaxed);
  }
};

/** Global PQ stats instance. Defined in sql_parallel.cc. */
extern PQ_global_stats pq_global_stats;

struct PQ_worker_info;
class Gather_operator;

/**
  Open an independent worker TABLE through the normal SQL open path.

  This is a V2-8C scaffold helper. It consumes a SQL-owned
  PQ_Worker_open_context, builds a worker-local Table_ref from leader TABLE
  metadata, calls open_ltable(), and fills worker_table/worker_handler on
  success. It does not start a worker thread or enable real row production.

  @retval false  Worker TABLE opened and context filled
  @retval true   Open failed or independence gates failed
*/
bool pq_open_worker_table(PQ_Worker_open_context *open_ctx);

/**
  Close a worker TABLE opened by pq_open_worker_table().

  @param open_ctx         Worker open context to clear
  @param statement_error  true to rollback the worker statement transaction
*/
void pq_close_worker_table(PQ_Worker_open_context *open_ctx,
                           bool statement_error);

/**
  Create a background THD for a PQ worker and bind it to worker metadata.

  This helper only prepares lifecycle metadata. It is intended to run from the
  future worker OS thread entry; callers must not use it for real row
  production until the worker execution path is enabled.
*/
THD *pq_create_worker_thd(PQ_worker_info *worker, Gather_operator *gather);

/** Destroy a THD created by pq_create_worker_thd(). */
void pq_destroy_worker_thd(PQ_worker_info *worker);

// ---------------------------------------------------------------------------
// PQ_worker_info: per-worker metadata
// ---------------------------------------------------------------------------

/**
  Per-worker information structure.

  Each PQ_worker_info tracks a single worker's identity, status, and
  error state. Gather_operator holds an array of PQ_worker_info pointers.

  V2-8K-1: PQ_worker_info owns a joinable scaffold worker thread handle.
  The thread creates and destroys its worker THD inside the worker thread
  context; row production is added by later V2-8K steps.
*/
struct PQ_worker_info {
  uint32 m_worker_id{0};            ///< Worker index (0 .. DOP-1)
  std::atomic<PQ_Worker_status> m_status{
      PQ_Worker_status::NOT_STARTED};  ///< Current status
  THD *m_worker_thd{nullptr};       ///< Worker THD (nullptr until Phase 5)
  int m_error_code{0};              ///< Error code if status == ERROR/KILLED
  MQueue_handle *m_mq_handle{nullptr};  ///< MQ handle for this worker's queue
  PQ_Worker_context *m_worker_ctx{nullptr};  ///< Worker scan context (Phase 6)
  PQ_Worker_open_context m_open_ctx;  ///< Stable SQL-owned worker open carrier
  my_thread_handle m_thread_handle{};  ///< Joinable worker thread handle
  bool m_thread_started{false};        ///< Thread was successfully created
  bool m_thread_joined{false};         ///< Thread has been joined
  PQ_worker_task m_task{PQ_worker_task::NOOP};  ///< Thread entry task
  uint32 m_task_max_rows{0};          ///< Task-specific callback row limit
  std::atomic<uint32> m_task_rows_sent{0};  ///< Rows enqueued by worker task
  bool m_task_force_error{false};     ///< Debug task injects worker ERROR

  PQ_worker_info() { reset_open_context(); }

  explicit PQ_worker_info(uint32 id) : m_worker_id(id) {
    reset_open_context();
  }

  /** Reset borrowed open-context fields while preserving worker identity. */
  void reset_open_context();

  /**
    Transition worker status.

    Only allows forward transitions: NOT_STARTED -> RUNNING -> FINISHED/ERROR/
    KILLED/ABORTED. Invalid transitions are ignored (Phase 4 conservative).

    @param new_status  Target status
    @retval true       Transition accepted
    @retval false      Transition rejected (invalid)
  */
  bool transition_status(PQ_Worker_status new_status);

  /** Check if worker is in a terminal state. */
  bool is_terminal() const {
    const PQ_Worker_status status = m_status.load(std::memory_order_acquire);
    return status == PQ_Worker_status::FINISHED ||
           status == PQ_Worker_status::ERROR ||
           status == PQ_Worker_status::KILLED ||
           status == PQ_Worker_status::ABORTED;
  }
};

// ---------------------------------------------------------------------------
// PQ_worker_manager: worker lifecycle interface
// ---------------------------------------------------------------------------

/**
  Worker lifecycle manager.

  PQ_worker_manager provides start/wait/abort/cleanup for all workers.
  V2-8K-1: start() creates joinable no-op worker threads that bind worker THDs
  inside their own thread context and immediately finish. wait() joins them.
  abort() sets status but does not yet wake handler waits. cleanup() frees
  PQ_worker_info array.

  The scaffold is only used from guarded PQ smoke/debug paths. Later V2-8K
  steps replace the no-op thread entry with real row production without
  changing the manager interface.

  Memory note:
  - PQ_worker_info array is allocated with new[] in Phase 4.
  - Phase 5 must switch to pq_mem_root allocation.
  - Risk: if a worker OOM occurs before pq_mem_root is wired,
    cleanup() may not free all resources correctly.
*/
class PQ_worker_manager {
 public:
  PQ_worker_manager() = default;
  ~PQ_worker_manager() = default;

  /**
    Start all workers.

    V2-8K-1: creates joinable scaffold worker threads.

    @param workers    Array of PQ_worker_info pointers
    @param n_workers  Number of workers (= DOP)
    @param leader_thd Leader THD (for kill-check in later phases)
    @param gather     Owning Gather_operator passed to worker THD setup

    @retval false  Success
    @retval true   Failure
  */
  bool start(PQ_worker_info **workers, uint32 n_workers, THD *leader_thd,
             Gather_operator *gather);

  /**
    Wait for all workers to reach a terminal state.

    V2-8K-1: joins all started scaffold worker threads.

    In production (Phase 5), this will:
    - Poll worker status and THD::killed
    - Block on worker completion events
    - Apply error priority: kill > leader fatal > worker fatal > MQ closed >
      normal finish
    - Propagate highest-priority error to THD::pq_error

    @param workers    Array of PQ_worker_info pointers
    @param n_workers  Number of workers
    @param leader_thd Leader THD

    @retval  0  All workers finished normally
    @retval -1  Error (kill / OOM / worker fatal)
    @retval  1  MQ closed but no error (normal end-of-data)
  */
  int wait(PQ_worker_info **workers, uint32 n_workers, THD *leader_thd);

  /**
    Abort all workers.

    Phase 4 stub: transitions all workers from any non-terminal state to
    ABORTED. Does NOT send ABORT token to MQ or signal worker threads.

    In production (Phase 5), this will:
    - Send ABORT control token to each worker's MQ
    - Set THD::killed on each worker THD
    - Wait briefly for workers to acknowledge abort

    @param workers    Array of PQ_worker_info pointers
    @param n_workers  Number of workers
  */
  void abort(PQ_worker_info **workers, uint32 n_workers);

  /**
    Clean up worker resources.

    Phase 4: frees the PQ_worker_info array and resets all handles.

    In production (Phase 5), this will also:
    - Join worker threads (if not already joined)
    - Free worker THDs
    - Free worker MEM_ROOTs

    @param workers       Array of PQ_worker_info pointers
    @param n_workers     Number of workers
    @param free_array    If true, also delete[] the workers array itself
  */
  void cleanup(PQ_worker_info **workers, uint32 n_workers, bool free_array);
};

// ---------------------------------------------------------------------------
// Gather_operator: leader-side orchestrator
// ---------------------------------------------------------------------------

/**
  Gather_operator: the leader-side orchestrator for parallel query execution.

  Gather_operator holds:
  - DOP (degree of parallelism, from THD::pq_dop or explicit setting)
  - Worker array (PQ_worker_info pointers, one per DOP)
  - Exchange pointer (Exchange_nosort for round-robin MQ reads)
  - Error state (highest-priority error per Phase A contract)
  - Completion state (all workers finished / aborted)

  Phase 4 lifecycle:
  1. Construct with DOP and optional ring_size.
  2. init() allocates PQ_worker_info array and Exchange_nosort.
  3. start_workers() (stub) would launch workers; Phase 4 inert.
  4. gather_rows() (stub) would read from Exchange; Phase 4 inert.
  5. handle_error() applies error priority.
  6. destroy() frees all resources.

  Inert guarantee:
  - No Gather_operator is created by any existing execution path.
  - THD::pq_leader remains nullptr until Phase 5 explicitly constructs one.
  - All methods are safe to call but produce no observable effect on
    normal SELECT execution.

  Memory risk:
  - Phase 4 uses new[]/delete[] for PQ_worker_info array.
  - Exchange_nosort also uses new[]/delete[] internally (from Phase 3).
  - Phase 5/6 must replace both with pq_mem_root allocation.
  - If pq_mem_root is not wired before worker launch, OOM in a worker
    cannot be safely reclaimed. This is documented as a Phase 5 risk.
*/
class Gather_operator {
 public:
  /**
    Error priority levels per Phase A contract.

    Higher numeric value = higher priority. When multiple errors exist,
    the highest-priority error is propagated to THD::pq_error.
  */
  enum ErrorPriority {
    PRIORITY_NORMAL_FINISH = 0,  ///< All workers finished normally
    PRIORITY_MQ_CLOSED = 1,      ///< MQ producers detached (end-of-data)
    PRIORITY_WORKER_FATAL = 2,   ///< Worker hit fatal error
    PRIORITY_LEADER_FATAL = 3,   ///< Leader OOM or unrecoverable error
    PRIORITY_KILL = 4            ///< THD killed (KILL QUERY / SHUTDOWN)
  };

  /**
    Gather error state: records the highest-priority error found.
  */
  struct GatherErrorState {
    ErrorPriority priority{PRIORITY_NORMAL_FINISH};
    int error_code{0};           ///< Error code from the highest-priority source
    uint32 source_worker_id{0};  ///< Worker ID that caused the error (if any)

    /** Reset to default (normal finish). */
    void reset() {
      priority = PRIORITY_NORMAL_FINISH;
      error_code = 0;
      source_worker_id = 0;
    }

    /** Check if an error has been recorded. */
    bool has_error() const { return priority > PRIORITY_MQ_CLOSED; }

    /** Check if a kill has been recorded (highest priority). */
    bool is_killed() const { return priority == PRIORITY_KILL; }
  };

 private:
  uint32 m_dop{0};                    ///< Degree of parallelism
  PQ_worker_info **m_workers{nullptr}; ///< Array of worker info pointers
  Exchange_nosort *m_exchange{nullptr}; ///< Exchange for row collection
  PQ_worker_manager m_worker_mgr;      ///< Worker lifecycle manager
  GatherErrorState m_error_state;      ///< Highest-priority error
  PQ_stats m_stats;                    ///< Per-query execution stats
  bool m_all_finished{false};          ///< All workers reached terminal state
  bool m_initialized{false};           ///< init() has been called
  uint32 m_ring_size{PQ_MQ_DEFAULT_RING_SIZE};  ///< MQ ring buffer size

 public:
  Gather_operator() = default;

  explicit Gather_operator(uint32 dop, uint32 ring_size = PQ_MQ_DEFAULT_RING_SIZE)
      : m_dop(dop), m_ring_size(ring_size) {}

  ~Gather_operator();

  /**
    Initialize the Gather_operator: allocate workers and Exchange.

    Creates the PQ_worker_info array (one per DOP) and Exchange_nosort.
    Exchange::init() creates MQ infrastructure for each worker.

    Phase 4: init() is inert; not called from any execution path.

    @retval false  Success
    @retval true   Failure (OOM or invalid DOP)
  */
  bool init();

  /**
    Configure stable per-worker open contexts after leader probe succeeds.

    This only fills SQL-owned carrier metadata. It does not create worker THDs,
    open worker TABLE objects, call handler worker init, or enable real row
    production.

    @param leader_table  Leader TABLE for metadata/reference checks
    @param leader_ctx    Handler leader context from pq_leader_scan_init()
    @param actual_dop    Actual DOP selected by the handler

    @retval false  Contexts configured
    @retval true   Gather is not initialized or input is invalid
  */
  bool configure_worker_open_contexts(TABLE *leader_table,
                                      PQ_Leader_context *leader_ctx,
                                      uint actual_dop);

  /**
    Destroy the Gather_operator: free workers, Exchange, and all MQ resources.

    Phase 4: destroy() is inert; not called from any execution path.
    It is safe to call even if init() was not called (no-op).
  */
  void destroy();

  /**
    Start all workers.

    Phase 4 stub: delegates to PQ_worker_manager::start().
    Inert; does not create OS threads or modify execution paths.

    @param leader_thd  Leader THD (for kill-check in Phase 5)

    @retval false  Success (stub: always succeeds if initialized)
    @retval true   Failure (stub: fails if not initialized)
  */
  bool start_workers(THD *leader_thd);

  /**
    Gather rows from workers via Exchange_nosort.

    Phase 4 stub: delegates to Exchange_nosort::read_mq_message() but
    returns immediately because no real workers are producing data.
    Inert; does not modify execution paths.

    In production (Phase 5+), this will:
    - Loop calling read_mq_message() until all workers finish
    - Convert MQ data to record[0] format (Phase 5)
    - Check THD::killed between iterations
    - Handle error priority propagation

    @param[out] type      Message type from MQ
    @param[out] datap     Data pointer from MQ
    @param[out] data_len  Data length from MQ

    @retval true   Row data available (stub: never true)
    @retval false  All workers done or no data (stub: always false)
  */
  bool gather_rows(MQMessageType &type, void **datap, uint32 &data_len);

  /**
    Wait for all workers to finish.

    Phase 4 stub: delegates to PQ_worker_manager::wait().
    Inert; does not block or modify execution paths.

    @param leader_thd  Leader THD

    @retval  0  All workers finished normally
    @retval -1  Error (kill / OOM / worker fatal)
    @retval  1  MQ closed but no error (normal end-of-data)
  */
  int wait_for_workers(THD *leader_thd);

  /**
    Run a V2-5 worker lifecycle smoke pass.

    This exercises worker metadata start/wait/error-priority cleanup without
    creating OS threads, reading InnoDB rows, or sending row data through MQ.
    It must not update real execution counters such as
    Parallel_queries_executed, Parallel_workers_launched, or
    Parallel_rows_scanned.

    @param leader_thd  Leader THD

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_lifecycle_smoke(THD *leader_thd);

  /**
    Run a V2-8J worker producer loop skeleton smoke.

    This does not create an OS worker thread or read InnoDB rows. It exercises
    the minimal producer lifecycle contract: worker metadata transitions to
    RUNNING, sends a typed FINISH token, leader observes EOF via Exchange, and
    worker transitions to FINISHED.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_producer_loop_smoke(THD *leader_thd, TABLE *leader_table);

  /**
    Run a V2-8J worker producer ERROR smoke.

    This verifies the controlled ERROR path: worker metadata transitions to
    RUNNING, sends a typed ERROR token, leader observes an error via Exchange,
    and worker transitions to ERROR. The error is expected and consumed inside
    the smoke; it must not affect the user query.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_producer_error_smoke(THD *leader_thd, TABLE *leader_table);

  /**
    Run a V2-8J worker producer abort smoke.

    This verifies the controlled leader abort path: worker metadata transitions
    to RUNNING, leader aborts workers, MQ consumer side is detached, leader
    observes EOF via Exchange, and worker transitions to ABORTED. The abort is
    expected and consumed inside the smoke; it must not affect the user query.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_producer_abort_smoke(THD *leader_thd, TABLE *leader_table);

  /**
    Run a V2-6 synthetic Exchange/Gather row-stream smoke pass.

    This pre-fills MQ handles with synthetic ROW/FINISH tokens and consumes
    them through Exchange_nosort. It does not return rows to SQL execution and
    must not update real execution counters.

    @param leader_thd  Leader THD

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_exchange_row_stream_smoke(THD *leader_thd);

  /**
    Run a V2-8B synthetic row-image materialization smoke pass.

    This pre-fills MQ handles with typed ROW/FINISH messages, copies each ROW
    payload into leader table->record[0], and leaves real PQ execution disabled.
    It must not update real execution counters.

    @param leader_thd  Leader THD
    @param table       Leader TABLE whose record[0] receives synthetic rows

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_exchange_row_image_smoke(THD *leader_thd, TABLE *table);

  /**
    Run a V2-12A-2 synthetic GROUP BY partial aggregate wire-protocol smoke.

    This pre-fills MQ handles with typed PARTIAL_GROUP/FINISH messages and
    verifies Exchange can decode them. It does not merge aggregate states and
    must not update real execution counters.

    @param leader_thd  Leader THD

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_exchange_partial_group_smoke(THD *leader_thd);

  /**
    Run an M8 synthetic ORDER BY gather-merge smoke pass.

    This uses Exchange_sort and binary_heap with fixed worker-local sorted
    streams. It verifies ASC, DESC, and rowid tie-break ordering without
    opening the real worker ORDER BY path.

    @param leader_thd  Leader THD

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_exchange_sort_smoke(THD *leader_thd);

  /**
    Run a V2-8C worker THD/TABLE lifecycle smoke pass.

    This creates worker THD metadata, opens an independent worker TABLE through
    pq_open_worker_table(), closes it, and destroys the worker THD. It does not
    start OS worker threads, call handler worker scan init, or produce rows.

    @param leader_thd    Leader THD to restore as current THD after smoke
    @param leader_table  Leader TABLE used as metadata source

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_open_table_smoke(THD *leader_thd, TABLE *leader_table);

  /**
    Run an M11-D4a worker attach contract smoke.

    This debug-only helper opens an independent worker TABLE, initializes and
    ends a typed worker scan context, closes the worker TABLE, and destroys the
    worker THD. It does not start a worker thread, produce rows, use
    Query_result_mq, or materialize data into the leader result.

    @retval false  Attach smoke completed
    @retval true   Attach smoke failed
  */
  bool run_worker_attach_contract_smoke(THD *leader_thd, TABLE *leader_table,
                                        PQ_Leader_context *leader_ctx);

  /**
    Run a V2-8G EXECUTE read-view callback conversion smoke pass.

    This creates a worker THD/TABLE, initializes a worker handler context, and
    asks the engine to attempt conversion of at most one Parallel_reader
    callback row into the worker TABLE record buffer. It must not return that
    row to SQL execution or update real execution counters. Small/empty tables
    may complete without a callback row; attempts and converted rows are
    counted separately.

    @param leader_thd    Leader THD to restore as current THD after smoke
    @param leader_table  Leader TABLE used as metadata source

    @retval false  Smoke pass completed or table was empty
    @retval true   Smoke pass failed
  */
  bool run_worker_callback_conversion_smoke(THD *leader_thd,
                                            TABLE *leader_table);

  /**
    Run an M4a Query_result_mq worker-result wire contract smoke.

    This validates the commercial worker-result frame boundary through a local
    MQueue handle. It does not create a worker plan, start a worker, read
    InnoDB rows, or materialize data into the leader TABLE.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_query_result_mq_contract_smoke(THD *leader_thd);

  /**
    Run an M4b Query_result_mq send_data/send_eof smoke.

    This validates the minimal worker result path through Query_result_mq
    itself. It builds synthetic Items and local MQ frames only; it does not
    attach Query_result_mq to real worker execution or InnoDB row production.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_query_result_mq_send_data_smoke(THD *leader_thd);

  /**
    Run an M11-B2 Query_result_mq leader adapter smoke.

    This validates that a synthetic PQWR ROW frame can be decoded into
    leader-side field views. It does not materialize rows into TABLE,
    attach Query_result_mq to worker execution, or start workers.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_query_result_mq_adapter_smoke(THD *leader_thd);

  /**
    Run an M11-B3b local Query_result_mq wiring smoke.

    This sends two local Query_result_mq ROW frames through a local MQueue and
    decodes them with the PQWR leader adapter. It does not start worker
    threads, attach cloned JOIN, touch handler/InnoDB, or return decoded data
    as user SQL result.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_query_result_mq_wiring_smoke(THD *leader_thd);

  /**
    Run an M11-B3c worker-thread Query_result_mq probe.

    This starts one debug/smoke worker thread. The worker uses only its worker
    THD, per-worker MQueue_handle, local Query_result_mq, and controlled Item
    values to send PQWR ROW/FINISH frames. The leader waits for the worker and
    decodes those frames. It does not open worker TABLEs, call handler/InnoDB,
    attach cloned JOIN, or return decoded data as user SQL result.

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_query_result_mq_threaded_probe_smoke(THD *leader_thd);

  /**
    Run a limited V2-8J callback multi-row producer smoke pass.

    This uses the push-style handler callback producer and a SQL-owned row sink
    that deep-copies at most two worker record images into Exchange. The leader
    drains those rows plus FINISH through the materialize status helper. It does
    not connect to PQTableScanIterator::Read().

    @param leader_thd    Leader THD to restore as current THD after smoke
    @param leader_table  Leader TABLE used as metadata source

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_callback_multirow_producer_smoke(THD *leader_thd,
                                                   TABLE *leader_table);

  /**
    Run a worker-local partial GROUP BY producer smoke pass.

    Each configured worker opens its own TABLE/handler, scans its assigned
    callback range, accumulates one local integer partial group, sends it as a
    PARTIAL_GROUP message, and sends FINISH. The leader drains and merges those
    payloads through Exchange. This does not open SQL GROUP BY DOP>1 execution.

    @param leader_thd    Leader THD to restore as current THD after smoke
    @param leader_table  Leader TABLE used as metadata source

    @retval false  Smoke pass completed
    @retval true   Smoke pass failed
  */
  bool run_worker_partial_group_smoke(THD *leader_thd, TABLE *leader_table);

  bool run_worker_partial_group_merge(
      THD *leader_thd, TABLE *leader_table, uint32 group_field_index,
      uint32 value_field_index, PQ_partial_group_agg_kind agg_kind,
      PQ_partial_group_merge_slot_v1 *merge_slots, uint32 slot_count,
      uint32 *worker_groups, uint32 *merged_groups);

  /**
    Produce a bounded callback row stream into this gather's Exchange.

    This is the V2-8J shadow Read() producer bridge. It opens one worker
    THD/TABLE/handler in the configured EXECUTE context, sends up to max_rows
    ROW messages through a SQL-owned sink, then sends FINISH. The caller owns
    subsequent Exchange consumption.

    @param leader_thd    Leader THD
    @param leader_table  Leader TABLE for worker open context
    @param max_rows      Maximum rows to enqueue
    @param[out] rows_sent Number of ROW messages enqueued

    @retval false  Producer completed and FINISH was enqueued
    @retval true   Failure
  */
  bool run_worker_callback_limited_producer(THD *leader_thd,
                                            TABLE *leader_table,
                                            uint32 max_rows,
                                            uint32 *rows_sent);

  /**
    Start a worker-thread callback producer into this gather's Exchange.

    This is the V2-8K-2 debug-only bridge. It configures one worker task and
    returns immediately after the worker thread has been created. The caller
    owns subsequent leader-side Exchange consumption through Read().

    @param leader_thd    Leader THD
    @param leader_table  Leader TABLE for worker open context
    @param max_rows      Maximum rows to enqueue; UINT32_MAX means natural EOF

    @retval false  Worker thread started
    @retval true   Failure
  */
  bool run_worker_callback_threaded_producer(THD *leader_thd,
                                             TABLE *leader_table,
                                             uint32 max_rows);

  /**
    Abort all workers and close MQ producers.

    Phase 4 stub: delegates to PQ_worker_manager::abort() and
    aborts MQ consumer side on Exchange handles.

    @param leader_thd  Leader THD (for kill propagation)
  */
  void abort_workers(THD *leader_thd);

  /**
    Apply error priority from worker error states.

    Scans all PQ_worker_info error codes and THD::killed, then
    selects the highest-priority error per Phase A contract:
    kill > leader fatal/OOM > worker fatal > MQ closed > normal finish.

    @param leader_thd  Leader THD (for THD::killed check)

    @return  GatherErrorState with the highest-priority error
  */
  GatherErrorState resolve_error_priority(THD *leader_thd);

  // --- Accessors (for Phase 5 integration) ---

  /** Get DOP. */
  uint32 dop() const { return m_dop; }

  /** Get worker info for a specific worker. */
  PQ_worker_info *get_worker(uint32 id) const {
    return (m_workers && id < m_dop) ? m_workers[id] : nullptr;
  }

  /** Get Exchange_nosort pointer. */
  Exchange_nosort *get_exchange() const { return m_exchange; }

  /** Get error state. */
  const GatherErrorState &error_state() const { return m_error_state; }

  /** Check if all workers finished. */
  bool all_finished() const { return m_all_finished; }

  /** Check if initialized. */
  bool is_initialized() const { return m_initialized; }

  /** Get per-query stats. */
  PQ_stats &stats() { return m_stats; }
  const PQ_stats &stats() const { return m_stats; }

  /**
    Check if leader THD has been killed or timed out.

    Phase 8: checks THD::killed and max_execution_time.
    Returns true if the leader should abort PQ execution.

    @param leader_thd  Leader THD to check

    @retval true   Leader is killed or timed out
    @retval false  Leader is still running
  */
  bool check_leader_kill(THD *leader_thd);

  /**
    Propagate kill signal to all workers.

    Phase 8: sets all worker statuses to KILLED and closes MQ producer
    side on all handles. Phase 5+ will also set THD::killed on each
    worker THD and send ABORT control tokens.

    @param leader_thd  Leader THD (source of kill signal)
  */
  void propagate_kill_to_workers(THD *leader_thd);
};

#endif  // SQL_PARALLEL_PQ_INCLUDED
