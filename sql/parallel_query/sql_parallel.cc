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
#include "sql/handler.h"          // handler
#include "sql/sql_base.h"         // close_thread_tables, open_ltable
#include "sql/sql_class.h"        // THD
#include "sql/table.h"            // TABLE, Table_ref
#include "sql/transaction.h"      // trans_commit_stmt, trans_rollback_stmt

// ---------------------------------------------------------------------------
// PQ_global_stats instance
// ---------------------------------------------------------------------------

PQ_global_stats pq_global_stats;

const char *pq_execution_state_to_string(PQ_execution_state state) {
  switch (state) {
    case PQ_execution_state::DISABLED:
      return "disabled";
    case PQ_execution_state::NOT_ELIGIBLE:
      return "not_eligible";
    case PQ_execution_state::ELIGIBLE:
      return "eligible";
    case PQ_execution_state::ITERATOR_SELECTED:
      return "iterator_selected";
    case PQ_execution_state::EXECUTED:
      return "executed";
    case PQ_execution_state::FALLBACK_SERIAL:
      return "fallback_serial";
  }
  return "unknown";
}

uint pq_execution_state_to_uint(PQ_execution_state state) {
  return static_cast<uint>(state);
}

PQ_execution_state pq_execution_state_from_uint(uint state) {
  switch (static_cast<PQ_execution_state>(state)) {
    case PQ_execution_state::DISABLED:
    case PQ_execution_state::NOT_ELIGIBLE:
    case PQ_execution_state::ELIGIBLE:
    case PQ_execution_state::ITERATOR_SELECTED:
    case PQ_execution_state::EXECUTED:
    case PQ_execution_state::FALLBACK_SERIAL:
      return static_cast<PQ_execution_state>(state);
  }
  return PQ_execution_state::DISABLED;
}

void pq_set_execution_state(THD *thd, PQ_execution_state state) {
  if (thd == nullptr) return;
  thd->pq_execution_state = pq_execution_state_to_uint(state);
  thd->pq_executed = (state == PQ_execution_state::EXECUTED);
}

bool pq_open_worker_table(PQ_Worker_open_context *open_ctx) {
  if (open_ctx == nullptr || open_ctx->worker_thd == nullptr ||
      open_ctx->leader_table == nullptr || open_ctx->leader_table->s == nullptr ||
      open_ctx->worker_table != nullptr || open_ctx->worker_handler != nullptr) {
    return true;
  }

  THD *worker_thd = open_ctx->worker_thd;
  TABLE *leader_table = open_ctx->leader_table;
  TABLE_SHARE *share = leader_table->s;
  const char *alias = share->table_name.str;
  if (leader_table->pos_in_table_list != nullptr &&
      leader_table->pos_in_table_list->alias != nullptr) {
    alias = leader_table->pos_in_table_list->alias;
  }

  auto *worker_ref = new (worker_thd->mem_root)
      Table_ref(share->db.str, share->db.length, share->table_name.str,
                share->table_name.length, alias, TL_READ, MDL_SHARED_READ);
  if (worker_ref == nullptr) return true;

  TABLE *worker_table = open_ltable(worker_thd, worker_ref, TL_READ, 0);
  if (worker_table == nullptr || worker_table->file == nullptr ||
      worker_table == leader_table || worker_table->file == leader_table->file ||
      worker_table->record[0] == nullptr ||
      worker_table->record[0] == leader_table->record[0]) {
    trans_rollback_stmt(worker_thd);
    close_thread_tables(worker_thd);
    worker_thd->mdl_context.release_transactional_locks();
    return true;
  }

  open_ctx->worker_table = worker_table;
  open_ctx->worker_handler = worker_table->file;
  return false;
}

void pq_close_worker_table(PQ_Worker_open_context *open_ctx,
                           bool statement_error) {
  if (open_ctx == nullptr || open_ctx->worker_thd == nullptr) return;

  THD *worker_thd = open_ctx->worker_thd;
  if (statement_error) {
    trans_rollback_stmt(worker_thd);
  } else {
    trans_commit_stmt(worker_thd);
  }
  close_thread_tables(worker_thd);
  worker_thd->mdl_context.release_transactional_locks();
  open_ctx->worker_table = nullptr;
  open_ctx->worker_handler = nullptr;
}

// ---------------------------------------------------------------------------
// PQ_worker_info: transition_status
// ---------------------------------------------------------------------------

void PQ_worker_info::reset_open_context() {
  m_open_ctx = PQ_Worker_open_context{};
  m_open_ctx.worker_thd = m_worker_thd;
  m_open_ctx.worker_id = m_worker_id;
  m_open_ctx.mq_handle = m_mq_handle;
}

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

Gather_operator::~Gather_operator() { destroy(); }

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
    m_workers[i]->reset_open_context();
    m_workers[i]->m_open_ctx.actual_dop = m_dop;
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

bool Gather_operator::configure_worker_open_contexts(
    TABLE *leader_table, PQ_Leader_context *leader_ctx, uint actual_dop) {
  if (!m_initialized || m_workers == nullptr || m_exchange == nullptr ||
      leader_table == nullptr || leader_ctx == nullptr || actual_dop == 0 ||
      actual_dop != m_dop) {
    return true;
  }

  for (uint32 i = 0; i < m_dop; i++) {
    auto *worker = m_workers[i];
    if (worker == nullptr) return true;

    worker->reset_open_context();
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.leader_ctx = leader_ctx;
    worker->m_open_ctx.actual_dop = actual_dop;
    worker->m_open_ctx.mq_handle = worker->m_mq_handle;
  }

  return false;
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
        m_workers[i]->reset_open_context();
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

bool Gather_operator::run_worker_lifecycle_smoke(THD *leader_thd) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  if (start_workers(leader_thd)) {
    if (initialized_here) destroy();
    return true;
  }

  int wait_result = wait_for_workers(leader_thd);
  auto error_state = resolve_error_priority(leader_thd);
  if (wait_result != 0 || error_state.has_error()) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.worker_smoke_runs.fetch_add(1, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_exchange_row_stream_smoke(THD *leader_thd
                                                    [[maybe_unused]]) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  bool failed =
      nosort->run_synthetic_row_stream_smoke(&rows_read, &finishes_read);
  if (failed) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.exchange_smoke_rows.fetch_add(rows_read,
                                                std::memory_order_relaxed);
  pq_global_stats.exchange_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_exchange_row_image_smoke(THD *leader_thd
                                                   [[maybe_unused]],
                                                   TABLE *table) {
  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 rows_read = 0;
  uint32 finishes_read = 0;
  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  bool failed =
      nosort->run_synthetic_row_image_smoke(table, &rows_read, &finishes_read);
  if (failed) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.exchange_smoke_rows.fetch_add(rows_read,
                                                std::memory_order_relaxed);
  pq_global_stats.exchange_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
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
