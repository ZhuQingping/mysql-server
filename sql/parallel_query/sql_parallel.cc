/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use the software under the
   terms of the GNU General Public License, version 2.0,
   or the terms of any other license of the available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/parallel_query/sql_parallel.cc
  Parallel Query V1-MVP: Gather operator and worker lifecycle stub implementation.

  Phase 4 scope:
  - Implement PQ_worker_info::transition_status() with forward-only transitions.
  - Implement PQ_worker_manager::start/wait/abort/cleanup stubs.
  - Implement Gather_operator::init/destroy/start_workers/gather_rows/
    wait_for_workers/abort_workers/resolve_error_priority.
  - All stubs are inert: not called from any existing execution path.
  - No optimizer integration, no execution path change, no handler/InnoDB call.

  Memory ownership (Phase 4):
  - PQ_worker_info array allocated with new[].
  - Exchange_nosort created with new; its init() allocates MQ infrastructure.
  - All allocations freed in Gather_operator::destroy().
  - Phase 5/6 must switch to pq_mem_root for production safety.

  Inert guarantee:
  - No code in this file is reachable from normal SELECT execution.
  - Gather_operator is only constructible via explicit new, and no code
    path in Phase 4 creates one.
  - THD::pq_leader and THD::pq_worker_info remain nullptr unless
    Phase 5 explicitly sets them.
*/

#include "sql/parallel_query/sql_parallel.h"

#include <cassert>
#include <cstring>

#include "mysqld_error.h"         // ER_QUERY_INTERRUPTED
#include "sql/sql_class.h"        // THD

// ---------------------------------------------------------------------------
// PQ_global_stats instance
// ---------------------------------------------------------------------------

PQ_global_stats pq_global_stats;

// ---------------------------------------------------------------------------
// PQ_worker_info: transition_status
// ---------------------------------------------------------------------------

bool PQ_worker_info::transition_status(PQ_Worker_status new_status) {
  // Forward-only transition rules:
  // NOT_STARTED -> RUNNING -> FINISHED/ERROR/KILLED/ABORTED
  // Any -> ABORTED (abort can override any state except terminal)
  // Terminal states are final: no further transitions allowed.
  if (is_terminal()) {
    // Already in a terminal state; reject transition.
    return false;
  }

  switch (m_status) {
    case PQ_Worker_status::NOT_STARTED:
      // NOT_STARTED can transition to RUNNING or ABORTED
      if (new_status == PQ_Worker_status::RUNNING ||
          new_status == PQ_Worker_status::ABORTED) {
        m_status = new_status;
        return true;
      }
      break;

    case PQ_Worker_status::RUNNING:
      // RUNNING can transition to FINISHED, ERROR, KILLED, or ABORTED
      if (new_status == PQ_Worker_status::FINISHED ||
          new_status == PQ_Worker_status::ERROR ||
          new_status == PQ_Worker_status::KILLED ||
          new_status == PQ_Worker_status::ABORTED) {
        m_status = new_status;
        return true;
      }
      break;

    default:
      // Unexpected current state; reject.
      break;
  }

  return false;  // Transition rejected
}

// ---------------------------------------------------------------------------
// PQ_worker_manager: stub implementations
// ---------------------------------------------------------------------------

bool PQ_worker_manager::start(PQ_worker_info **workers, uint32 n_workers,
                              THD *leader_thd [[maybe_unused]]) {
  assert(workers != nullptr);
  assert(n_workers > 0);

  // Phase 4 stub: transition all workers to RUNNING without creating
  // real OS threads or worker THDs.
  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) return true;  // Invalid worker info
    if (!workers[i]->transition_status(PQ_Worker_status::RUNNING)) {
      return true;  // Transition failed (should not happen for NOT_STARTED)
    }
  }

  return false;  // Success
}

int PQ_worker_manager::wait(PQ_worker_info **workers, uint32 n_workers,
                            THD *leader_thd [[maybe_unused]]) {
  assert(workers != nullptr);
  assert(n_workers > 0);

  // Phase 4 stub: transition all workers to FINISHED immediately.
  // In production (Phase 5), this will block on worker completion events,
  // check THD::killed, and apply error priority.
  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) continue;

    if (!workers[i]->is_terminal()) {
      // Force transition to FINISHED for stub.
      // Production code will wait for real worker status changes.
      workers[i]->transition_status(PQ_Worker_status::FINISHED);
    }
  }

  // Phase 4 stub: always return normal finish (0).
  // Production will compute result based on error priority.
  return 0;
}

void PQ_worker_manager::abort(PQ_worker_info **workers, uint32 n_workers) {
  if (workers == nullptr || n_workers == 0) return;

  // Phase 4 stub: transition all non-terminal workers to ABORTED.
  // In production (Phase 5), this will also:
  // - Send ABORT control token to each worker's MQ
  // - Set THD::killed on each worker THD
  // - Wait briefly for workers to acknowledge abort
  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) continue;

    if (!workers[i]->is_terminal()) {
      workers[i]->transition_status(PQ_Worker_status::ABORTED);
    }
  }
}

void PQ_worker_manager::cleanup(PQ_worker_info **workers, uint32 n_workers,
                                bool free_array) {
  if (workers == nullptr) return;

  // Free individual PQ_worker_info structs
  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] != nullptr) {
      // Note: PQ_worker_info does not own m_worker_thd or m_worker_ctx
      // in Phase 4 (they are nullptr). Phase 5 will need to free worker
      // THDs and contexts here.
      delete workers[i];
      workers[i] = nullptr;
    }
  }

  // Free the array itself if requested
  if (free_array) {
    delete[] workers;
  }
}

// ---------------------------------------------------------------------------
// Gather_operator: init / destroy
// ---------------------------------------------------------------------------

bool Gather_operator::init() {
  if (m_dop == 0) return true;  // Invalid DOP
  if (m_initialized) return false;  // Already initialized (no-op success)

  // Allocate worker info array.
  // RISK: uses new[] instead of pq_mem_root. Phase 5 must switch.
  m_workers = new PQ_worker_info *[m_dop];
  if (m_workers == nullptr) goto err;

  for (uint32 i = 0; i < m_dop; i++) {
    m_workers[i] = new PQ_worker_info(i);
    if (m_workers[i] == nullptr) goto err;

    // Wire MQ handle to Exchange (created below).
    // The handle pointer will be set after Exchange init.
  }

  // Create Exchange_nosort for round-robin row collection.
  // RISK: uses new instead of pq_mem_root. Phase 5 must switch.
  m_exchange = new Exchange_nosort(m_dop, m_ring_size);
  if (m_exchange == nullptr) goto err;

  // Initialize Exchange MQ infrastructure.
  if (m_exchange->init()) goto err;

  // Wire each worker's MQ handle to the Exchange handle for that worker.
  for (uint32 i = 0; i < m_dop; i++) {
    m_workers[i]->m_mq_handle = m_exchange->get_mq_handle(i);
  }

  m_initialized = true;
  m_all_finished = false;
  m_error_state.reset();

  return false;  // Success

err:
  // Cleanup partially allocated resources.
  destroy();
  return true;  // Failure
}

void Gather_operator::destroy() {
  // Clean up worker info array.
  // Note: PQ_worker_manager::cleanup handles the per-worker cleanup.
  if (m_workers != nullptr) {
    for (uint32 i = 0; i < m_dop; i++) {
      if (m_workers[i] != nullptr) {
        // Unlink MQ handle before freeing worker info.
        // Exchange owns the MQ handles, so we don't free them here.
        m_workers[i]->m_mq_handle = nullptr;
        delete m_workers[i];
        m_workers[i] = nullptr;
      }
    }
    delete[] m_workers;
    m_workers = nullptr;
  }

  // Clean up Exchange_nosort.
  if (m_exchange != nullptr) {
    m_exchange->cleanup();
    delete m_exchange;
    m_exchange = nullptr;
  }

  m_initialized = false;
  m_all_finished = false;
  m_error_state.reset();
}

// ---------------------------------------------------------------------------
// Gather_operator: start_workers (stub)
// ---------------------------------------------------------------------------

bool Gather_operator::start_workers(THD *leader_thd) {
  if (!m_initialized) return true;  // Not initialized

  // Phase 4 stub: delegate to PQ_worker_manager::start().
  // In production (Phase 5), this will also:
  // - Create worker THDs
  // - Launch worker threads
  // - Wire worker THD::pq_worker_info to PQ_worker_info
  // - Wire worker THD::pq_leader to this Gather_operator
  return m_worker_mgr.start(m_workers, m_dop, leader_thd);
}

// ---------------------------------------------------------------------------
// Gather_operator: gather_rows (stub)
// ---------------------------------------------------------------------------

bool Gather_operator::gather_rows(MQMessageType &type, void **datap,
                                  uint32 &data_len) {
  if (!m_initialized || m_all_finished) {
    type = MQMessageType::FINISH;
    *datap = nullptr;
    data_len = 0;
    return false;
  }

  // Phase 4 stub: call Exchange_nosort::read_mq_message() but no real
  // workers are producing data, so it will return false (no data).
  // In production (Phase 5+), this will loop reading rows until all
  // workers finish, converting MQ data to record[0] format.
  bool result = m_exchange->read_mq_message(type, datap, data_len);

  // Check for all-done state
  if (!result && type == MQMessageType::FINISH) {
    m_all_finished = true;
  }

  return result;
}

// ---------------------------------------------------------------------------
// Gather_operator: wait_for_workers (stub)
// ---------------------------------------------------------------------------

int Gather_operator::wait_for_workers(THD *leader_thd) {
  if (!m_initialized) return -1;  // Not initialized

  // Phase 4 stub: delegate to PQ_worker_manager::wait().
  // Production will block on real worker completion events.
  int result = m_worker_mgr.wait(m_workers, m_dop, leader_thd);

  if (result == 0) {
    m_all_finished = true;
  }

  return result;
}

// ---------------------------------------------------------------------------
// Gather_operator: abort_workers (stub)
// ---------------------------------------------------------------------------

void Gather_operator::abort_workers(THD *leader_thd [[maybe_unused]]) {
  if (!m_initialized) return;

  // Phase 4 stub: abort worker manager + abort MQ consumer side.
  // In production (Phase 5), this will also:
  // - Send ABORT token to each worker's MQ
  // - Set THD::killed on leader and worker THDs
  m_worker_mgr.abort(m_workers, m_dop);

  // Abort MQ consumer side on all handles.
  // This tells workers that the leader is no longer reading.
  if (m_exchange != nullptr) {
    for (uint32 i = 0; i < m_dop; i++) {
      MQueue_handle *handle = m_exchange->get_mq_handle(i);
      if (handle != nullptr) {
        handle->abort_consumer();
      }
    }
  }

  m_all_finished = true;
}

// ---------------------------------------------------------------------------
// Gather_operator: resolve_error_priority
// ---------------------------------------------------------------------------

Gather_operator::GatherErrorState Gather_operator::resolve_error_priority(
    THD *leader_thd) {
  GatherErrorState result;
  result.reset();

  // ================================================================
  // Priority 1: KILL -- check leader THD killed flag.
  // Phase 8: now actually checks THD::killed instead of stub comment.
  // If the leader is killed (KILL QUERY, KILL CONNECTION, or
  // max_execution_time exceeded), this takes absolute priority.
  // ================================================================
  if (leader_thd != nullptr && leader_thd->killed != 0) {
    result.priority = PRIORITY_KILL;
    result.error_code = ER_QUERY_INTERRUPTED;
    result.source_worker_id = 0;  // Kill originated from leader, not a worker
    m_error_state = result;
    return result;
  }

  // ================================================================
  // Priority 2: LEADER_FATAL -- check for OOM or unrecoverable error.
  // Phase 8: checks THD::pq_error for leader-side errors.
  // ================================================================
  if (leader_thd != nullptr && leader_thd->pq_error != 0) {
    if (result.priority < PRIORITY_LEADER_FATAL) {
      result.priority = PRIORITY_LEADER_FATAL;
      result.error_code = leader_thd->pq_error;
      result.source_worker_id = 0;  // Leader-side error
    }
  }

  // ================================================================
  // Priority 3: WORKER_FATAL -- check each worker's error code.
  // ================================================================
  for (uint32 i = 0; i < m_dop; i++) {
    if (m_workers[i] == nullptr) continue;

    if (m_workers[i]->m_status == PQ_Worker_status::ERROR ||
        m_workers[i]->m_status == PQ_Worker_status::KILLED) {
      // Worker fatal: update result if higher priority than current.
      if (result.priority < PRIORITY_WORKER_FATAL) {
        result.priority = PRIORITY_WORKER_FATAL;
        result.error_code = m_workers[i]->m_error_code;
        result.source_worker_id = m_workers[i]->m_worker_id;
      }
    }
  }

  // ================================================================
  // Priority 4: MQ_CLOSED -- check if Exchange reports all workers done.
  // ================================================================
  if (m_all_finished && !result.has_error()) {
    result.priority = PRIORITY_MQ_CLOSED;
    result.error_code = 0;
  }

  // ================================================================
  // Priority 5: NORMAL_FINISH -- default if no errors found.
  // ================================================================
  if (!result.has_error() && !m_all_finished) {
    result.priority = PRIORITY_NORMAL_FINISH;
  }

  m_error_state = result;
  return result;
}

// ---------------------------------------------------------------------------
// Gather_operator: check_leader_kill
// ---------------------------------------------------------------------------

bool Gather_operator::check_leader_kill(THD *leader_thd) {
  if (leader_thd == nullptr) return false;

  // Check THD::killed flag (set by KILL QUERY, KILL CONNECTION, or shutdown).
  if (leader_thd->killed != 0) {
    return true;
  }

  // Check max_execution_time exceeded.
  // In MySQL 8.0, max_execution_time is checked via
  // THD->get_stmt_da()->statement_warn_area().has_warn_expr().
  // For simplicity, Phase 8 checks if max_execution_time_set > 0 and
  // the timeout has been exceeded. The actual timeout logic is handled
  // by MySQL's execution timer; here we just check the killed flag
  // which is also set on timeout.
  // Note: MySQL sets THD::killed = THD::KILL_QUERY on max_execution_time
  // timeout, so the killed check above already covers this.

  return false;
}

// ---------------------------------------------------------------------------
// Gather_operator: propagate_kill_to_workers
// ---------------------------------------------------------------------------

void Gather_operator::propagate_kill_to_workers(THD *leader_thd
                                                 [[maybe_unused]]) {
  if (!m_initialized) return;

  // Set all worker statuses to KILLED.
  for (uint32 i = 0; i < m_dop; i++) {
    if (m_workers[i] == nullptr) continue;
    if (!m_workers[i]->is_terminal()) {
      m_workers[i]->transition_status(PQ_Worker_status::KILLED);
    }
  }

  // Close MQ producer side on all handles to signal workers that
  // the leader has stopped reading. This causes workers to detect
  // MQ_DETACHED on their next send() call and exit.
  if (m_exchange != nullptr) {
    for (uint32 i = 0; i < m_dop; i++) {
      MQueue_handle *handle = m_exchange->get_mq_handle(i);
      if (handle != nullptr) {
        handle->abort_consumer();
      }
    }
  }

  // Phase 5+ will also:
  // - Set THD::killed on each worker THD
  // - Send ABORT control token to each worker's MQ

  m_all_finished = true;
}
