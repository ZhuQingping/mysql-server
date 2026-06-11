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

class THD;
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
  std::atomic<uint64> workers_launched{0};    ///< Total worker threads launched
  std::atomic<uint64> rows_scanned{0};        ///< Total rows scanned by PQ workers
  std::atomic<uint64> ranges_built{0};        ///< Total InnoDB PQ ranges planned
  std::atomic<uint64> worker_smoke_runs{0};   ///< Worker lifecycle smoke runs
  std::atomic<uint64> exchange_smoke_rows{0};      ///< Synthetic MQ rows read
  std::atomic<uint64> exchange_smoke_finishes{0};  ///< Synthetic FINISH tokens

  /** Reset all counters. */
  void reset() {
    queries_executed.store(0, std::memory_order_relaxed);
    queries_fallback.store(0, std::memory_order_relaxed);
    workers_launched.store(0, std::memory_order_relaxed);
    rows_scanned.store(0, std::memory_order_relaxed);
    ranges_built.store(0, std::memory_order_relaxed);
    worker_smoke_runs.store(0, std::memory_order_relaxed);
    exchange_smoke_rows.store(0, std::memory_order_relaxed);
    exchange_smoke_finishes.store(0, std::memory_order_relaxed);
  }
};

/** Global PQ stats instance. Defined in sql_parallel.cc. */
extern PQ_global_stats pq_global_stats;

// ---------------------------------------------------------------------------
// PQ_worker_info: per-worker metadata
// ---------------------------------------------------------------------------

/**
  Per-worker information structure.

  Each PQ_worker_info tracks a single worker's identity, status, and
  error state. Gather_operator holds an array of PQ_worker_info pointers.

  Phase 4: no real OS thread is created. PQ_worker_info exists only as
  metadata for the future worker lifecycle. Status stays NOT_STARTED.
*/
struct PQ_worker_info {
  uint32 m_worker_id{0};            ///< Worker index (0 .. DOP-1)
  PQ_Worker_status m_status{PQ_Worker_status::NOT_STARTED};  ///< Current status
  THD *m_worker_thd{nullptr};       ///< Worker THD (nullptr until Phase 5)
  int m_error_code{0};              ///< Error code if status == ERROR/KILLED
  MQueue_handle *m_mq_handle{nullptr};  ///< MQ handle for this worker's queue
  PQ_Worker_context *m_worker_ctx{nullptr};  ///< Worker scan context (Phase 6)

  PQ_worker_info() = default;

  explicit PQ_worker_info(uint32 id) : m_worker_id(id) {}

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
    return m_status == PQ_Worker_status::FINISHED ||
           m_status == PQ_Worker_status::ERROR ||
           m_status == PQ_Worker_status::KILLED ||
           m_status == PQ_Worker_status::ABORTED;
  }
};

// ---------------------------------------------------------------------------
// PQ_worker_manager: worker lifecycle interface
// ---------------------------------------------------------------------------

/**
  Worker lifecycle manager.

  PQ_worker_manager provides start/wait/abort/cleanup for all workers.
  Phase 4: all methods are stubs. start() does NOT create OS threads.
  wait() returns immediately. abort() sets status but does not signal.
  cleanup() frees PQ_worker_info array.

  These stubs must NOT be called from any existing execution path.
  They exist so that Phase 5/6 can replace the stubs with real thread
  creation and synchronization without changing the interface.

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

    Phase 4 stub: transitions all PQ_worker_info from NOT_STARTED to RUNNING
    but does NOT create OS threads or worker THDs.

    @param workers    Array of PQ_worker_info pointers
    @param n_workers  Number of workers (= DOP)
    @param leader_thd Leader THD (for kill-check in Phase 5)

    @retval false  Success (stub: always succeeds)
    @retval true   Failure (stub: never fails)
  */
  bool start(PQ_worker_info **workers, uint32 n_workers, THD *leader_thd);

  /**
    Wait for all workers to reach a terminal state.

    Phase 4 stub: transitions all workers to FINISHED immediately.

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
