/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/parallel_query/pq_iterator.cc
  Parallel Query V1-MVP: PQ table scan iterator skeleton implementation.

  Phase 5B scope:
  - PQTableScanIterator constructor, destructor, Init(), Read() stubs.
  - TryCreatePQTableScanIterator guarded factory.
  - V2-1 returns a PQ iterator for ordinary eligible execution.
  - Init() falls back to an owned serial TableScanIterator before workers,
    Exchange/Gather, worker THDs, or row streams are touched.
  - V2-2 may probe the handler leader init/end contract with DOP=1 before
    serial fallback.
  - No real worker launch, no MQ interaction.
*/

#include "sql/parallel_query/pq_iterator.h"

#include <limits>

#include "my_base.h"
#include "my_dbug.h"
#include "sql/debug_sync.h"
#include "sql/iterators/basic_row_iterators.h"  // TableScanIterator
#include "sql/iterators/timing_iterator.h"     // NewIterator
#include "sql/mysqld.h"       // innodb_hton
#include "sql/parallel_query/pq_clone.h"  // pq_clone_activation_probe
#include "sql/parallel_query/pq_group_aggregate_iterator.h"
#include "sql/parallel_query/sql_parallel.h"  // pq_global_stats
#include "sql/sql_class.h"    // THD::variables, THD::pq_is_worker
#include "sql/sql_lex.h"      // LEX::is_explain
#include "sql/sql_optimizer.h"  // JOIN::pq_eligible
#include "sql/table.h"        // TABLE, TABLE_SHARE::db_type

// ---------------------------------------------------------------------------
// PQTableScanIterator implementation (Phase 5B stub)
// ---------------------------------------------------------------------------

namespace {

constexpr uint64 kPQReadWaitTimeoutUs = 1000;

}  // namespace

PQTableScanIterator::PQTableScanIterator(THD *thd, MEM_ROOT *mem_root,
                                         TABLE *table, JOIN *join,
                                         double expected_rows,
                                         ha_rows *examined_rows)
    : TableRowIterator(thd, table),
      m_mem_root(mem_root),
      m_join(join),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_record(table->record[0]) {}

PQTableScanIterator::~PQTableScanIterator() {
  cleanup_pq_resources(true);
}

bool PQTableScanIterator::Init() {
  assert(m_join != nullptr);

  pq_set_execution_state(thd(), PQ_execution_state::ITERATOR_SELECTED);

  auto init_serial_fallback = [&]() -> bool {
    // V2-1 safe fallback window: no worker, Gather/Exchange, or row stream has
    // been initialized. Build the same serial table scan the caller would have
    // built when TryCreatePQTableScanIterator returned nullptr.
    assert(can_fallback_serial());
    if (m_serial_iterator == nullptr) {
      m_serial_iterator = NewIterator<TableScanIterator>(
          thd(), m_mem_root, table(), m_expected_rows, m_examined_rows);
      if (m_serial_iterator == nullptr) return true;
    }

    if (!m_fallback_counted) {
      pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
      m_fallback_counted = true;
    }
    pq_set_execution_state(thd(), PQ_execution_state::FALLBACK_SERIAL);
    return m_serial_iterator->Init();
  };

  if (table() == nullptr || table()->s == nullptr || table()->s->blob_fields > 0) {
    return init_serial_fallback();
  }

  // V2-2 bridge smoke: prove the handler can create and release a SQL-visible
  // leader context without starting workers or reading rows. Unsupported
  // engines/states still use the V2-1 serial fallback path; real handler
  // errors are reported before serial iterator state is initialized.
  uint actual_dop = 0;
  uint requested_dop = thd()->variables.parallel_default_dop;
  if (requested_dop == 0) requested_dop = 1;
  const bool threaded_read_shadow_path =
      should_enter_threaded_read_shadow_path(requested_dop);
  const bool read_shadow_path =
      !threaded_read_shadow_path && should_enter_read_shadow_path(requested_dop);
  pq_global_stats.probe_attempts.fetch_add(1, std::memory_order_relaxed);
  int error = table()->file->pq_leader_scan_init(
      thd(), &m_leader_ctx, PQ_leader_scan_mode::PROBE, requested_dop,
      &actual_dop, false);
  bool probe_supported = false;
  if (error == 0) {
    pq_global_stats.probe_success.fetch_add(1, std::memory_order_relaxed);
    probe_supported = true;
    uint smoke_dop = actual_dop > 0 ? actual_dop : requested_dop;
    m_gather = new Gather_operator(smoke_dop);
    if (m_gather == nullptr || m_gather->init() ||
        m_gather->configure_worker_open_contexts(table(), m_leader_ctx,
                                                 smoke_dop) ||
        m_gather->run_worker_lifecycle_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator open_smoke(1);
    if (open_smoke.init() ||
        open_smoke.configure_worker_open_contexts(table(), m_leader_ctx, 1) ||
        open_smoke.run_worker_open_table_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_exchange_row_image_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_contract_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_send_data_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_adapter_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    if (m_gather->run_query_result_mq_wiring_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator threaded_result_smoke(1);
    if (threaded_result_smoke.run_query_result_mq_threaded_probe_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator partial_group_smoke(smoke_dop);
    if (partial_group_smoke.init() ||
        partial_group_smoke.run_exchange_partial_group_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator orderby_smoke(3);
    if (orderby_smoke.run_exchange_sort_smoke(thd())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    uint32 typed_group_smoke_groups = 0;
    uint64 typed_group_smoke_sum = 0;
    if (RunPQGroupAggregateTypedStateSmoke(&typed_group_smoke_groups,
                                           &typed_group_smoke_sum)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    pq_global_stats.groupby_typed_smoke_groups.fetch_add(
        typed_group_smoke_groups, std::memory_order_relaxed);
    pq_global_stats.groupby_typed_smoke_sum.fetch_add(
        typed_group_smoke_sum, std::memory_order_relaxed);
    Gather_operator producer_smoke(1);
    if (producer_smoke.init() ||
        producer_smoke.run_worker_producer_loop_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator producer_error_smoke(1);
    if (producer_error_smoke.init() ||
        producer_error_smoke.run_worker_producer_error_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    Gather_operator producer_abort_smoke(1);
    if (producer_abort_smoke.init() ||
        producer_abort_smoke.run_worker_producer_abort_smoke(thd(), table())) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }
    cleanup_pq_resources(false);

    if (!read_shadow_path && !threaded_read_shadow_path) {
      PQ_Leader_context *partial_execute_ctx = nullptr;
      uint partial_execute_dop = 0;
      error = table()->file->pq_leader_scan_init(
          thd(), &partial_execute_ctx, PQ_leader_scan_mode::EXECUTE,
          smoke_dop, &partial_execute_dop, false);
      if (error == 0) {
        const uint worker_partial_dop =
            partial_execute_dop > 0 ? partial_execute_dop : smoke_dop;
        Gather_operator worker_partial_smoke(worker_partial_dop);
        const bool partial_failed =
            worker_partial_smoke.init() ||
            worker_partial_smoke.configure_worker_open_contexts(
                table(), partial_execute_ctx, worker_partial_dop) ||
            worker_partial_smoke.run_worker_partial_group_smoke(thd(),
                                                                table());
        table()->file->pq_leader_scan_end(partial_execute_ctx);
        if (partial_failed) {
          PrintError(HA_ERR_OUT_OF_MEM);
          return true;
        }
      } else if (error != HA_ERR_UNSUPPORTED) {
        PrintError(error);
        return true;
      }

      PQ_Leader_context *execute_ctx = nullptr;
      uint execute_dop = 0;
      error = table()->file->pq_leader_scan_init(
          thd(), &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
          false);
      if (error == 0) {
        Gather_operator callback_smoke(1);
        (void)(callback_smoke.init() ||
               callback_smoke.configure_worker_open_contexts(
                   table(), execute_ctx, 1) ||
               callback_smoke.run_worker_callback_conversion_smoke(thd(),
                                                                   table()));
        Gather_operator callback_multirow_smoke(1);
        (void)(callback_multirow_smoke.init() ||
               callback_multirow_smoke.configure_worker_open_contexts(
                   table(), execute_ctx, 1) ||
               callback_multirow_smoke.run_worker_callback_multirow_producer_smoke(
                   thd(), table()));
        table()->file->pq_leader_scan_end(execute_ctx);
      } else if (error != HA_ERR_UNSUPPORTED) {
        PrintError(error);
        return true;
      }
    }
  } else if (error == HA_ERR_UNSUPPORTED) {
    pq_global_stats.probe_unsupported.fetch_add(1, std::memory_order_relaxed);
  } else {
    cleanup_pq_resources(true);
    PrintError(error);
    return true;
  }

  if ((read_shadow_path || threaded_read_shadow_path) && probe_supported) {
    const uint threaded_execute_dop =
        threaded_read_shadow_path ? requested_dop : 1;
    uint execute_dop = 0;
    error = table()->file->pq_leader_scan_init(
        thd(), &m_leader_ctx, PQ_leader_scan_mode::EXECUTE,
        threaded_execute_dop, &execute_dop, false);
    if (error != 0) {
      cleanup_pq_resources(true);
      PrintError(error);
      return true;
    }

    const uint gather_dop = execute_dop > 0 ? execute_dop : threaded_execute_dop;
    m_gather = new Gather_operator(gather_dop);
    if (m_gather == nullptr || m_gather->init() ||
        m_gather->configure_worker_open_contexts(table(), m_leader_ctx,
                                                 gather_dop)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_OUT_OF_MEM);
      return true;
    }

    if (threaded_read_shadow_path) {
      if (m_gather->run_worker_callback_threaded_producer(
              thd(), table(), std::numeric_limits<uint32>::max())) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
      DEBUG_SYNC(thd(), "pq_read_threaded_worker_started");
      DBUG_EXECUTE_IF("pq_read_threaded_shadow_abort_after_start", {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      });
    } else {
      uint32 rows_produced = 0;
      if (m_gather->run_worker_callback_limited_producer(thd(), table(), 2,
                                                         &rows_produced)) {
        cleanup_pq_resources(true);
        PrintError(HA_ERR_INTERNAL_ERROR);
        return true;
      }
    }

    mark_pq_started();
    return false;
  }

  return init_serial_fallback();
}

void PQTableScanIterator::cleanup_pq_resources(bool abort_workers) {
  if (m_gather != nullptr) {
    if (abort_workers && m_gather->is_initialized()) {
      m_gather->abort_workers(thd());
    }
    m_gather->destroy();
    delete m_gather;
    m_gather = nullptr;
  }

  if (m_leader_ctx != nullptr) {
    table()->file->pq_leader_scan_end(m_leader_ctx);
    m_leader_ctx = nullptr;
  }
}

bool PQTableScanIterator::should_enter_read_shadow_path(
    uint requested_dop) const {
  bool enabled = false;
  DBUG_EXECUTE_IF("pq_read_shadow_path", enabled = true;);
  return enabled && requested_dop > 0 && table() != nullptr &&
         table()->s != nullptr && table()->s->blob_fields == 0 &&
         table()->s->reclength > 0;
}

bool PQTableScanIterator::should_enter_threaded_read_shadow_path(
    uint requested_dop) const {
  bool dop1_enabled =
      thd() != nullptr &&
      thd()->variables.parallel_query_experimental_threaded_dop1;
  bool dop2_enabled =
      thd() != nullptr &&
      thd()->variables.parallel_query_experimental_threaded_dop;
  bool dop4_enabled =
      thd() != nullptr &&
      thd()->variables.parallel_query_experimental_threaded_dop4;
  DBUG_EXECUTE_IF("pq_read_threaded_shadow_path", dop1_enabled = true;);
  DBUG_EXECUTE_IF("pq_read_threaded_dop2_shadow_path", {
    dop2_enabled = true;
  });
  DBUG_EXECUTE_IF("pq_read_threaded_dop4_shadow_path", {
    dop4_enabled = true;
  });
  return ((dop1_enabled && requested_dop == 1) ||
          (dop2_enabled && requested_dop == 2) ||
          (dop4_enabled && requested_dop == 4)) &&
         table() != nullptr &&
         table()->s != nullptr && table()->s->blob_fields == 0 &&
         table()->s->reclength > 0;
}

int PQTableScanIterator::Read() {
  if (m_serial_iterator != nullptr) {
    return m_serial_iterator->Read();
  }

  assert(m_runtime_state == Runtime_state::PQ_STARTED ||
         m_runtime_state == Runtime_state::PQ_ROW_RETURNED);
  assert(m_gather != nullptr);
  assert(m_gather->get_exchange() != nullptr);

  auto *exchange = m_gather->get_exchange();

  for (;;) {
    if (m_gather->check_leader_kill(thd())) {
      m_gather->propagate_kill_to_workers(thd());
      cleanup_pq_resources(true);
      thd()->send_kill_message();
      return 1;
    }

    Exchange_nosort::Materialize_status status =
        Exchange_nosort::Materialize_status::ERROR;
    if (exchange->materialize_next_record_image_status(table(), &status)) {
      cleanup_pq_resources(true);
      PrintError(HA_ERR_INTERNAL_ERROR);
      return 1;
    }

    if (status == Exchange_nosort::Materialize_status::ROW) {
      if (!m_executed_counted) {
        mark_pq_row_returned();
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(1,
                                                   std::memory_order_relaxed);
        m_executed_counted = true;
      }
      pq_global_stats.rows_scanned.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }

    if (status == Exchange_nosort::Materialize_status::EOF_REACHED) {
      if (!m_executed_counted) {
        pq_set_execution_state(thd(), PQ_execution_state::EXECUTED);
        pq_global_stats.queries_executed.fetch_add(1,
                                                   std::memory_order_relaxed);
        m_executed_counted = true;
      }
      cleanup_pq_resources(false);
      return -1;
    }

    if (status == Exchange_nosort::Materialize_status::WOULD_BLOCK) {
      exchange->wait_for_message(kPQReadWaitTimeoutUs);
      continue;
    }

    cleanup_pq_resources(true);
    PrintError(HA_ERR_INTERNAL_ERROR);
    return 1;
  }
}

void PQTableScanIterator::UnlockRow() {
  if (m_serial_iterator != nullptr) {
    m_serial_iterator->UnlockRow();
  } else {
    TableRowIterator::UnlockRow();
  }
}

void PQTableScanIterator::SetNullRowFlag(bool is_null_row) {
  if (m_serial_iterator != nullptr) {
    m_serial_iterator->SetNullRowFlag(is_null_row);
  } else {
    TableRowIterator::SetNullRowFlag(is_null_row);
  }
}

void PQTableScanIterator::StartPSIBatchMode() {
  if (m_serial_iterator != nullptr) {
    m_serial_iterator->StartPSIBatchMode();
  } else {
    TableRowIterator::StartPSIBatchMode();
  }
}

void PQTableScanIterator::EndPSIBatchModeIfStarted() {
  if (m_serial_iterator != nullptr) {
    m_serial_iterator->EndPSIBatchModeIfStarted();
  } else {
    if (m_gather != nullptr || m_leader_ctx != nullptr) {
      cleanup_pq_resources(true);
    }
    TableRowIterator::EndPSIBatchModeIfStarted();
  }
}

// ---------------------------------------------------------------------------
// TryCreatePQTableScanIterator guarded factory
// ---------------------------------------------------------------------------

unique_ptr_destroy_only<RowIterator> TryCreatePQTableScanIterator(
    THD *thd, MEM_ROOT *mem_root, TABLE *table, JOIN *join,
    double expected_rows, ha_rows *examined_rows) {
  // -----------------------------------------------------------------
  // Guard 1: parallel_query system variable must be ON.
  // Default is OFF, so this guard alone ensures no behavior change
  // unless the user explicitly enables PQ.
  // -----------------------------------------------------------------
  if (!thd->variables.parallel_query) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 2: JOIN must exist. TABLE_SCAN paths that have no JOIN
  // (e.g., certain DDL/internal scans) must always go serial.
  // -----------------------------------------------------------------
  if (join == nullptr) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 3: optimizer must have marked this query as PQ-eligible.
  // pq_eligible == false means the optimizer found a reason to
  // reject PQ (e.g., subquery, UNION, non-SELECT, hypergraph).
  // -----------------------------------------------------------------
  if (!join->pq_eligible) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 4: table must use InnoDB engine. PQ V1 only supports
  // InnoDB clustered full scan. Other engines must go serial.
  // -----------------------------------------------------------------
  if (table->s->db_type() != innodb_hton) {
    return nullptr;
  }

  // -----------------------------------------------------------------
  // Guard 5: THD must not be a PQ worker. No recursive/nested
  // parallelism is allowed. A worker THD executing its own plan
  // must not spawn sub-workers.
  // -----------------------------------------------------------------
  if (thd->pq_is_worker) {
    return nullptr;
  }

  // EXPLAIN may build iterators, but it is not a real statement execution
  // fallback. Keep PQ execution status counters tied to non-EXPLAIN execution.
  if (thd->lex != nullptr && thd->lex->is_explain()) {
    pq_set_execution_state(thd, PQ_execution_state::ELIGIBLE);
    return nullptr;
  }

  if (pq_clone_activation_probe(thd, join)) return nullptr;

  return NewIterator<PQTableScanIterator>(thd, mem_root, mem_root, table, join,
                                          expected_rows, examined_rows);
}
