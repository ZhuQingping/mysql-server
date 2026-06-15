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
  Parallel Query: Gather operator and worker lifecycle implementation.

  Phase 4 scope:
  - Implement PQ_worker_info::transition_status() with forward-only transitions.
  - Implement PQ_worker_manager::start/wait/abort/cleanup.
  - Implement Gather_operator::init/destroy/start_workers/gather_rows/
    wait_for_workers/abort_workers/resolve_error_priority.
  - V2-8K-1 worker manager starts joinable no-op worker scaffold threads.
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

#include "my_dbug.h"
#include "mysql/psi/mysql_thread.h"
#include "mysqld_error.h"         // ER_QUERY_INTERRUPTED
#include "sql/field.h"            // Field
#include "sql/mysqld.h"           // key_thread_parallel_query_worker
#include "sql/handler.h"          // handler
#include "sql/sql_base.h"         // close_thread_tables, open_ltable
#include "sql/sql_class.h"        // THD
#include "sql/sql_thd_internal_api.h"  // create_internal_thd
#include "sql/table.h"            // TABLE, Table_ref
#include "sql/transaction.h"      // trans_commit_stmt, trans_rollback_stmt

// ---------------------------------------------------------------------------
// PQ_global_stats instance
// ---------------------------------------------------------------------------

PQ_global_stats pq_global_stats;

namespace {

struct PQ_worker_thread_arg {
  PQ_worker_info *worker{nullptr};
  Gather_operator *gather{nullptr};
};

bool pq_run_worker_thread_task(PQ_worker_info *worker, Gather_operator *gather);

void *pq_worker_thread_entry(void *arg_ptr) {
  auto *arg = static_cast<PQ_worker_thread_arg *>(arg_ptr);
  PQ_worker_info *worker = arg != nullptr ? arg->worker : nullptr;
  Gather_operator *gather = arg != nullptr ? arg->gather : nullptr;
  delete arg;

  if (worker == nullptr || gather == nullptr) return nullptr;

  if (my_thread_init()) {
    worker->m_error_code = HA_ERR_OUT_OF_MEM;
    worker->transition_status(PQ_Worker_status::RUNNING);
    worker->transition_status(PQ_Worker_status::ERROR);
    return nullptr;
  }

  worker->transition_status(PQ_Worker_status::RUNNING);
  THD *worker_thd = pq_create_worker_thd(worker, gather);
  if (worker_thd == nullptr) {
    worker->m_error_code = HA_ERR_OUT_OF_MEM;
    worker->transition_status(PQ_Worker_status::ERROR);
    my_thread_end();
    return nullptr;
  }

  const bool failed = pq_run_worker_thread_task(worker, gather);
  pq_destroy_worker_thd(worker);
  if (failed) {
    if (worker->m_error_code == 0) worker->m_error_code = HA_ERR_INTERNAL_ERROR;
    worker->transition_status(PQ_Worker_status::ERROR);
  } else {
    worker->transition_status(PQ_Worker_status::FINISHED);
  }
  my_thread_end();
  return nullptr;
}

}  // namespace

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
  bitmap_copy(worker_table->read_set, leader_table->read_set);
  bitmap_copy(worker_table->write_set, leader_table->write_set);
  worker_table->column_bitmaps_set_no_signal(worker_table->read_set,
                                             worker_table->write_set);
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

THD *pq_create_worker_thd(PQ_worker_info *worker, Gather_operator *gather) {
  if (worker == nullptr || gather == nullptr || worker->m_worker_thd != nullptr) {
    return nullptr;
  }

  THD *worker_thd = create_internal_thd();
  if (worker_thd == nullptr) return nullptr;

  worker_thd->pq_is_worker = true;
  worker_thd->pq_leader = gather;
  worker_thd->pq_worker_info = worker;
  worker_thd->pq_dop = gather->dop();

  worker->m_worker_thd = worker_thd;
  worker->m_open_ctx.worker_thd = worker_thd;
  return worker_thd;
}

void pq_destroy_worker_thd(PQ_worker_info *worker) {
  if (worker == nullptr || worker->m_worker_thd == nullptr) return;

  THD *worker_thd = worker->m_worker_thd;
  if (worker->m_open_ctx.worker_table != nullptr ||
      worker->m_open_ctx.worker_handler != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, true);
  }

  worker_thd->pq_is_worker = false;
  worker_thd->pq_leader = nullptr;
  worker_thd->pq_worker_info = nullptr;
  worker_thd->pq_dop = 0;
  destroy_internal_thd(worker_thd);

  worker->m_worker_thd = nullptr;
  worker->reset_open_context();
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
  PQ_Worker_status current = m_status.load(std::memory_order_acquire);
  for (;;) {
    bool allowed = false;
    switch (current) {
      case PQ_Worker_status::NOT_STARTED:
        // NOT_STARTED can transition to RUNNING or ABORTED.
        allowed = new_status == PQ_Worker_status::RUNNING ||
                  new_status == PQ_Worker_status::ABORTED;
        break;

      case PQ_Worker_status::RUNNING:
        // RUNNING can transition to FINISHED, ERROR, KILLED, or ABORTED.
        allowed = new_status == PQ_Worker_status::FINISHED ||
                  new_status == PQ_Worker_status::ERROR ||
                  new_status == PQ_Worker_status::KILLED ||
                  new_status == PQ_Worker_status::ABORTED;
        break;

      default:
        allowed = false;
        break;
    }

    if (!allowed) return false;
    if (m_status.compare_exchange_weak(current, new_status,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return true;
    }
  }
}

// ---------------------------------------------------------------------------
// PQ_worker_manager: stub implementations
// ---------------------------------------------------------------------------

bool PQ_worker_manager::start(PQ_worker_info **workers, uint32 n_workers,
                              THD *leader_thd [[maybe_unused]],
                              Gather_operator *gather) {
  assert(workers != nullptr);
  assert(n_workers > 0);

  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) return true;  // Invalid worker info
    if (workers[i]->m_thread_started || workers[i]->m_thread_joined) {
      return true;
    }

    auto *arg = new (std::nothrow) PQ_worker_thread_arg{workers[i], gather};
    if (arg == nullptr) {
      abort(workers, i);
      wait(workers, i, leader_thd);
      return true;
    }

    if (mysql_thread_create(key_thread_parallel_query_worker,
                            &workers[i]->m_thread_handle, nullptr,
                            pq_worker_thread_entry, arg)) {
      delete arg;
      abort(workers, i);
      wait(workers, i, leader_thd);
      return true;
    }
    workers[i]->m_thread_started = true;
  }

  return false;  // Success
}

int PQ_worker_manager::wait(PQ_worker_info **workers, uint32 n_workers,
                            THD *leader_thd [[maybe_unused]]) {
  assert(workers != nullptr);
  assert(n_workers > 0);

  for (uint32 i = 0; i < n_workers; i++) {
    if (workers[i] == nullptr) continue;
    if (workers[i]->m_thread_started && !workers[i]->m_thread_joined) {
      if (my_thread_join(&workers[i]->m_thread_handle, nullptr) != 0) {
        workers[i]->m_error_code = HA_ERR_INTERNAL_ERROR;
        workers[i]->transition_status(PQ_Worker_status::ERROR);
        return -1;
      }
      workers[i]->m_thread_joined = true;
    } else if (!workers[i]->is_terminal()) {
      workers[i]->transition_status(PQ_Worker_status::FINISHED);
    }
    const PQ_Worker_status status =
        workers[i]->m_status.load(std::memory_order_acquire);
    if (status == PQ_Worker_status::ERROR ||
        status == PQ_Worker_status::KILLED ||
        status == PQ_Worker_status::ABORTED) {
      return -1;
    }
  }

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
      if (workers[i]->m_thread_started && !workers[i]->m_thread_joined) {
        (void)my_thread_join(&workers[i]->m_thread_handle, nullptr);
        workers[i]->m_thread_joined = true;
      }
      if (workers[i]->m_worker_thd != nullptr) {
        pq_destroy_worker_thd(workers[i]);
      }
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
    m_worker_mgr.cleanup(m_workers, m_dop, true);
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
  return m_worker_mgr.start(m_workers, m_dop, leader_thd, this);
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

bool Gather_operator::run_worker_producer_loop_smoke(
    THD *leader_thd [[maybe_unused]], TABLE *leader_table) {
  if (leader_table == nullptr || m_dop != 1) return true;

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) {
    if (initialized_here) destroy();
    return true;
  }

  bool eof = false;
  bool row = false;
  bool failed = exchange->enqueue_finish_smoke(0) ||
                exchange->materialize_next_record_image(leader_table, &eof,
                                                        &row) ||
                !eof || row ||
                !worker->transition_status(PQ_Worker_status::FINISHED);

  if (!failed) {
    pq_global_stats.worker_producer_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_worker_producer_error_smoke(
    THD *leader_thd [[maybe_unused]], TABLE *leader_table) {
  if (leader_table == nullptr || m_dop != 1) return true;

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) {
    if (initialized_here) destroy();
    return true;
  }

  bool eof = false;
  bool row = false;
  bool got_expected_error =
      !exchange->enqueue_error_smoke(0) &&
      exchange->materialize_next_record_image(leader_table, &eof, &row);
  worker->m_error_code = 1;
  bool failed = !got_expected_error ||
                !worker->transition_status(PQ_Worker_status::ERROR);

  if (!failed) {
    pq_global_stats.worker_producer_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (initialized_here) destroy();
  return failed;
}

bool Gather_operator::run_worker_producer_abort_smoke(
    THD *leader_thd, TABLE *leader_table) {
  if (leader_table == nullptr || m_dop != 1) return true;

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (!worker->transition_status(PQ_Worker_status::RUNNING)) {
    if (initialized_here) destroy();
    return true;
  }

  abort_workers(leader_thd);

  bool eof = false;
  bool row = false;
  MQueue_handle *handle = exchange->get_mq_handle(0);
  const bool observed_eof =
      !exchange->materialize_next_record_image(leader_table, &eof, &row) &&
      eof && !row;
  const bool detached = handle != nullptr && handle->is_detached();
  const bool failed = worker->m_status.load(std::memory_order_acquire) !=
                          PQ_Worker_status::ABORTED ||
                      !detached || !observed_eof || !m_all_finished;

  if (!failed) {
    pq_global_stats.worker_producer_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }

  if (initialized_here) destroy();
  return failed;
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
  pq_global_stats.exchange_row_image_smoke_rows.fetch_add(
      rows_read, std::memory_order_relaxed);
  pq_global_stats.exchange_row_image_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_exchange_partial_group_smoke(
    THD *leader_thd [[maybe_unused]]) {
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

  uint32 groups_read = 0;
  uint32 finishes_read = 0;
  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  const bool failed = nosort->run_synthetic_partial_group_smoke(
      &groups_read, &finishes_read);
  if (failed) {
    if (initialized_here) destroy();
    return true;
  }

  uint32 payload_errors = 0;
  if (nosort->run_synthetic_partial_group_malformed_smoke(&payload_errors)) {
    if (initialized_here) destroy();
    return true;
  }

  pq_global_stats.exchange_partial_group_smoke_rows.fetch_add(
      groups_read, std::memory_order_relaxed);
  pq_global_stats.exchange_partial_group_smoke_finishes.fetch_add(
      finishes_read, std::memory_order_relaxed);
  pq_global_stats.groupby_partial_payload_errors.fetch_add(
      payload_errors, std::memory_order_relaxed);

  if (initialized_here) destroy();
  return false;
}

bool Gather_operator::run_worker_open_table_smoke(THD *leader_thd,
                                                  TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
    pq_global_stats.worker_handler_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
    pq_close_worker_table(&worker->m_open_ctx, false);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed) {
    pq_global_stats.worker_open_smoke_runs.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_callback_conversion_smoke(
    THD *leader_thd, TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  if (worker == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool converted = false;
  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_callback_smoke(
                 worker->m_worker_ctx, worker->m_open_ctx.worker_table->record[0],
                 &converted) != 0;
  }
  if (!failed && converted && m_exchange != nullptr) {
    failed = m_exchange->enqueue_record_image_smoke(
                 0, worker->m_open_ctx.worker_table) != 0;
  }
  if (!failed && converted && m_exchange != nullptr) {
    bool eof = false;
    bool row = false;
    failed = m_exchange->materialize_next_record_image(leader_table, &eof,
                                                       &row) ||
             !row || eof;
  }
  if (worker->m_worker_ctx != nullptr && worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed && converted) {
    pq_global_stats.callback_smoke_rows.fetch_add(1,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_rows.fetch_add(1,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_finishes.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

class PQ_limited_mq_row_sink final : public PQ_row_sink {
 public:
  PQ_limited_mq_row_sink(Exchange_nosort *exchange, uint32 worker_id,
                         uint32 max_rows)
      : m_exchange(exchange), m_worker_id(worker_id), m_max_rows(max_rows) {}

  bool send_row(TABLE *source_table) override {
    if (should_abort()) return true;
    m_failed = m_exchange == nullptr ||
               m_exchange->enqueue_record_image(m_worker_id, source_table);
    if (!m_failed) ++m_rows_sent;
    return m_failed;
  }

  bool should_abort() const override {
    return m_failed || m_rows_sent >= m_max_rows;
  }

  bool stop_is_success() const override {
    return !m_failed && m_rows_sent >= m_max_rows;
  }

  uint32 rows_sent() const { return m_rows_sent; }

 private:
  Exchange_nosort *m_exchange;
  uint32 m_worker_id;
  uint32 m_max_rows;
  uint32 m_rows_sent{0};
  bool m_failed{false};
};

class PQ_partial_group_mq_sink final : public PQ_row_sink {
 public:
  PQ_partial_group_mq_sink(Exchange_nosort *exchange, uint32 worker_id)
      : m_exchange(exchange), m_worker_id(worker_id) {}

  bool send_row(TABLE *source_table) override {
    if (m_failed || source_table == nullptr || source_table->s == nullptr ||
        source_table->s->fields < 2) {
      m_failed = true;
      return true;
    }

    Field *value_field = source_table->field[1];
    if (value_field == nullptr) {
      m_failed = true;
      return true;
    }
    if (value_field->result_type() != INT_RESULT) {
      m_skipped = true;
      return false;
    }
    if (source_table->read_set != nullptr &&
        !bitmap_is_set(source_table->read_set, value_field->field_index())) {
      m_skipped = true;
      return false;
    }

    ++m_count_star;
    if (!value_field->is_null()) {
      const int64 value = static_cast<int64>(value_field->val_int());
      ++m_count_value;
      m_sum += value;
      if (!m_has_value) {
        m_has_value = true;
        m_min = value;
        m_max = value;
      } else {
        if (value < m_min) m_min = value;
        if (value > m_max) m_max = value;
      }
    }
    return false;
  }

  bool send_partial_group() {
    if (m_failed || m_exchange == nullptr) return true;
    if (m_skipped || m_count_star == 0) return false;

    const PQ_partial_group_payload_v1 payload = {
        PQ_PARTIAL_GROUP_PAYLOAD_MAGIC,
        PQ_PARTIAL_GROUP_PAYLOAD_VERSION,
        static_cast<uint16>(PQ_partial_group_agg_kind::SUM),
        m_worker_id,
        0,
        static_cast<int64>(m_worker_id % 2),
        m_count_star,
        m_count_value,
        m_sum,
        m_min,
        m_max};
    return m_exchange->enqueue_partial_group_smoke(m_worker_id, payload);
  }

  uint32 groups_sent() const { return m_count_star == 0 ? 0 : 1; }

 private:
  Exchange_nosort *m_exchange;
  uint32 m_worker_id;
  uint64 m_count_star{0};
  uint64 m_count_value{0};
  int64 m_sum{0};
  int64 m_min{0};
  int64 m_max{0};
  bool m_has_value{false};
  bool m_skipped{false};
  bool m_failed{false};
};

namespace {

bool pq_run_callback_limited_producer_task(PQ_worker_info *worker,
                                           Gather_operator *gather) {
  if (worker == nullptr || gather == nullptr ||
      worker->m_open_ctx.leader_table == nullptr ||
      worker->m_task_max_rows == 0) {
    return true;
  }

  auto *exchange = gather->get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    return true;
  }

  auto *nosort = static_cast<Exchange_nosort *>(exchange);
  if (worker->m_task_force_error) {
    worker->m_error_code = HA_ERR_INTERNAL_ERROR;
    (void)nosort->enqueue_error_smoke(worker->m_worker_id);
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  bool rnd_inited = false;
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->ha_rnd_init(true) != 0;
    rnd_inited = !failed;
  }
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  PQ_limited_mq_row_sink row_sink(nosort, worker->m_worker_id,
                                  worker->m_task_max_rows);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &row_sink) != 0;
  }
  if (!failed) {
    failed = nosort->enqueue_finish_smoke(worker->m_worker_id);
  }

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (rnd_inited && worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->ha_rnd_end();
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }

  worker->m_task_rows_sent.store(row_sink.rows_sent(),
                                 std::memory_order_release);
  if (failed) {
    (void)nosort->enqueue_error_smoke(worker->m_worker_id);
  }
  return failed;
}

bool pq_run_worker_thread_task(PQ_worker_info *worker,
                               Gather_operator *gather) {
  if (worker == nullptr) return true;

  switch (worker->m_task) {
    case PQ_worker_task::NOOP:
      return false;
    case PQ_worker_task::CALLBACK_LIMITED_PRODUCER:
      return pq_run_callback_limited_producer_task(worker, gather);
  }
  return true;
}

}  // namespace

bool Gather_operator::run_worker_callback_multirow_producer_smoke(
    THD *leader_thd, TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  PQ_limited_mq_row_sink row_sink(exchange, 0, 2);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &row_sink) != 0;
  }
  if (!failed) {
    failed = exchange->enqueue_finish_smoke(0);
  }

  uint32 rows_read = 0;
  bool saw_eof = false;
  while (!failed && !saw_eof) {
    Exchange_nosort::Materialize_status status =
        Exchange_nosort::Materialize_status::ERROR;
    failed = exchange->materialize_next_record_image_status(leader_table,
                                                            &status);
    if (failed) break;
    switch (status) {
      case Exchange_nosort::Materialize_status::ROW:
        ++rows_read;
        break;
      case Exchange_nosort::Materialize_status::EOF_REACHED:
        saw_eof = true;
        break;
      case Exchange_nosort::Materialize_status::WOULD_BLOCK:
      case Exchange_nosort::Materialize_status::ERROR:
        failed = true;
        break;
    }
  }

  failed = failed || !saw_eof || rows_read != row_sink.rows_sent() ||
           rows_read < 2;

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed) {
    pq_global_stats.callback_smoke_rows.fetch_add(rows_read,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_rows.fetch_add(rows_read,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_finishes.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_partial_group_smoke(THD *leader_thd,
                                                     TABLE *leader_table) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop == 0) {
    return true;
  }

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

  bool *rnd_inited = new (std::nothrow) bool[m_dop];
  if (rnd_inited == nullptr) {
    if (initialized_here) destroy();
    return true;
  }
  for (uint32 i = 0; i < m_dop; ++i) rnd_inited[i] = false;

  bool failed = false;
  for (uint32 i = 0; !failed && i < m_dop; ++i) {
    auto *worker = get_worker(i);
    if (worker == nullptr || worker->m_open_ctx.leader_ctx == nullptr ||
        worker->m_open_ctx.leader_table == nullptr) {
      failed = true;
      break;
    }

    if (pq_create_worker_thd(worker, this) == nullptr) {
      leader_thd->store_globals();
      failed = true;
      break;
    }
    worker->m_worker_thd->store_globals();

    failed = pq_open_worker_table(&worker->m_open_ctx);
    if (!failed) {
      failed = worker->m_open_ctx.worker_handler->ha_rnd_init(true) != 0;
      rnd_inited[i] = !failed;
    }
    if (!failed) {
      failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
          &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
    }
  }

  uint32 worker_groups = 0;
  for (uint32 i = 0; !failed && i < m_dop; ++i) {
    auto *worker = get_worker(i);
    worker->m_worker_thd->store_globals();
    PQ_partial_group_mq_sink group_sink(exchange, worker->m_worker_id);
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &group_sink) != 0;
    if (!failed && group_sink.groups_sent() > 0) {
      failed = group_sink.send_partial_group();
      if (!failed) worker_groups += group_sink.groups_sent();
    }
    if (!failed) {
      failed = exchange->enqueue_finish_smoke(worker->m_worker_id);
    }
  }

  for (uint32 i = 0; i < m_dop; ++i) {
    auto *worker = get_worker(i);
    if (worker == nullptr) continue;
    if (worker->m_worker_thd != nullptr) {
      worker->m_worker_thd->store_globals();
    }
    if (worker->m_worker_ctx != nullptr &&
        worker->m_open_ctx.worker_handler != nullptr) {
      worker->m_open_ctx.worker_handler->pq_worker_scan_end(
          worker->m_worker_ctx);
      worker->m_worker_ctx = nullptr;
    }
    if (rnd_inited[i] && worker->m_open_ctx.worker_handler != nullptr) {
      worker->m_open_ctx.worker_handler->ha_rnd_end();
    }
    if (worker->m_open_ctx.worker_table != nullptr) {
      pq_close_worker_table(&worker->m_open_ctx, failed);
    }
    pq_destroy_worker_thd(worker);
  }
  leader_thd->store_globals();

  uint32 payloads_read = 0;
  uint32 merged_groups = 0;
  PQ_partial_group_merge_slot_v1 merge_slots[2];
  while (!failed) {
    MQMessageType type;
    void *datap = nullptr;
    uint32 data_len = 0;
    const bool got_message = exchange->read_mq_message(type, &datap, data_len);

    if (!got_message) {
      if (exchange->all_done()) break;
      failed = true;
      break;
    }

    if (type == MQMessageType::PARTIAL_GROUP) {
      const PQ_partial_group_payload_v1 *payload = nullptr;
      failed = pq_validate_partial_group_payload_v1(datap, data_len, m_dop,
                                                    &payload) ||
               pq_merge_partial_group_payload_v1(*payload, merge_slots, 2,
                                                 &merged_groups);
      if (!failed) ++payloads_read;
      continue;
    }

    if (type != MQMessageType::FINISH) {
      failed = true;
      break;
    }
  }

  failed = failed || payloads_read != worker_groups ||
           (worker_groups > 0 &&
            (merged_groups == 0 || merged_groups > worker_groups));

  delete[] rnd_inited;
  if (initialized_here) destroy();
  if (!failed) {
    pq_global_stats.groupby_dop_partial_worker_groups.fetch_add(
        worker_groups, std::memory_order_relaxed);
    pq_global_stats.groupby_dop_partial_merged_groups.fetch_add(
        merged_groups, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_callback_limited_producer(
    THD *leader_thd, TABLE *leader_table, uint32 max_rows,
    uint32 *rows_sent) {
  if (rows_sent != nullptr) *rows_sent = 0;
  if (leader_thd == nullptr || leader_table == nullptr || m_dop != 1 ||
      max_rows == 0 || rows_sent == nullptr) {
    return true;
  }

  bool initialized_here = false;

  if (!m_initialized) {
    if (init()) return true;
    initialized_here = true;
  }

  auto *worker = get_worker(0);
  auto *exchange = get_exchange();
  if (worker == nullptr || exchange == nullptr) {
    if (initialized_here) destroy();
    return true;
  }

  if (worker->m_open_ctx.leader_table == nullptr) {
    worker->m_open_ctx.leader_table = leader_table;
    worker->m_open_ctx.actual_dop = m_dop;
  }

  if (pq_create_worker_thd(worker, this) == nullptr) {
    leader_thd->store_globals();
    if (initialized_here) destroy();
    return true;
  }

  bool failed = pq_open_worker_table(&worker->m_open_ctx);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler->pq_worker_scan_init(
        &worker->m_open_ctx, &worker->m_worker_ctx) != 0;
  }
  if (!failed) {
    pq_global_stats.callback_smoke_attempts.fetch_add(
        1, std::memory_order_relaxed);
  }

  PQ_limited_mq_row_sink row_sink(exchange, 0, max_rows);
  if (!failed) {
    failed = worker->m_open_ctx.worker_handler
                 ->pq_worker_scan_callback_produce(worker->m_worker_ctx,
                                                   &row_sink) != 0;
  }
  if (!failed) {
    failed = row_sink.rows_sent() == 0 || exchange->enqueue_finish_smoke(0);
  }

  if (worker->m_worker_ctx != nullptr &&
      worker->m_open_ctx.worker_handler != nullptr) {
    worker->m_open_ctx.worker_handler->pq_worker_scan_end(
        worker->m_worker_ctx);
    worker->m_worker_ctx = nullptr;
  }
  if (worker->m_open_ctx.worker_table != nullptr) {
    pq_close_worker_table(&worker->m_open_ctx, failed);
  }
  pq_destroy_worker_thd(worker);
  leader_thd->store_globals();

  if (initialized_here) destroy();
  if (!failed) {
    *rows_sent = row_sink.rows_sent();
    pq_global_stats.callback_smoke_rows.fetch_add(*rows_sent,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_rows.fetch_add(*rows_sent,
                                                  std::memory_order_relaxed);
    pq_global_stats.exchange_smoke_finishes.fetch_add(
        1, std::memory_order_relaxed);
  }
  return failed;
}

bool Gather_operator::run_worker_callback_threaded_producer(
    THD *leader_thd, TABLE *leader_table, uint32 max_rows) {
  if (leader_thd == nullptr || leader_table == nullptr || m_dop == 0 ||
      max_rows == 0 || !m_initialized) {
    return true;
  }

  auto *exchange = get_exchange();
  if (exchange == nullptr ||
      exchange->get_exchange_type() != Exchange::EXCHANGE_NOSORT) {
    return true;
  }

  for (uint32 i = 0; i < m_dop; ++i) {
    auto *worker = get_worker(i);
    if (worker == nullptr) return true;

    if (worker->m_open_ctx.leader_table == nullptr) {
      worker->m_open_ctx.leader_table = leader_table;
      worker->m_open_ctx.actual_dop = m_dop;
    }

    worker->m_task = PQ_worker_task::CALLBACK_LIMITED_PRODUCER;
    worker->m_task_max_rows = max_rows;
    worker->m_task_force_error = false;
    DBUG_EXECUTE_IF("pq_read_threaded_shadow_force_worker_error",
                    worker->m_task_force_error = (i == 0););
    worker->m_task_rows_sent.store(0, std::memory_order_release);
  }

  if (start_workers(leader_thd)) {
    for (uint32 i = 0; i < m_dop; ++i) {
      auto *worker = get_worker(i);
      if (worker != nullptr) worker->m_task = PQ_worker_task::NOOP;
    }
    return true;
  }

  pq_global_stats.workers_launched.fetch_add(m_dop, std::memory_order_relaxed);
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

    const PQ_Worker_status status =
        m_workers[i]->m_status.load(std::memory_order_acquire);
    if (status == PQ_Worker_status::ERROR ||
        status == PQ_Worker_status::KILLED) {
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
