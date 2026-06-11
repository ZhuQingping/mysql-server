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

#include "my_base.h"
#include "sql/iterators/basic_row_iterators.h"  // TableScanIterator
#include "sql/iterators/timing_iterator.h"     // NewIterator
#include "sql/mysqld.h"       // innodb_hton
#include "sql/parallel_query/sql_parallel.h"  // pq_global_stats
#include "sql/sql_class.h"    // THD::variables, THD::pq_is_worker
#include "sql/sql_lex.h"      // LEX::is_explain
#include "sql/sql_optimizer.h"  // JOIN::pq_eligible
#include "sql/table.h"        // TABLE, TABLE_SHARE::db_type

// ---------------------------------------------------------------------------
// PQTableScanIterator implementation (Phase 5B stub)
// ---------------------------------------------------------------------------

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

  // V2-2 bridge smoke: prove the handler can create and release a SQL-visible
  // leader context without starting workers or reading rows. Unsupported
  // engines/states still use the V2-1 serial fallback path; real handler
  // errors are reported before serial iterator state is initialized.
  uint actual_dop = 0;
  uint requested_dop = thd()->variables.parallel_default_dop;
  if (requested_dop == 0) requested_dop = 1;
  int error = table()->file->pq_leader_scan_init(
      thd(), &m_leader_ctx, PQ_leader_scan_mode::PROBE, requested_dop,
      &actual_dop, false);
  if (error == 0) {
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
    cleanup_pq_resources(false);

    PQ_Leader_context *execute_ctx = nullptr;
    uint execute_dop = 0;
    error = table()->file->pq_leader_scan_init(
        thd(), &execute_ctx, PQ_leader_scan_mode::EXECUTE, 1, &execute_dop,
        false);
    if (error == 0) {
      Gather_operator callback_smoke(1);
      (void)(callback_smoke.init() ||
             callback_smoke.configure_worker_open_contexts(table(), execute_ctx,
                                                           1) ||
             callback_smoke.run_worker_callback_conversion_smoke(thd(),
                                                                 table()));
      table()->file->pq_leader_scan_end(execute_ctx);
    } else if (error != HA_ERR_UNSUPPORTED) {
      PrintError(error);
      return true;
    }
  } else if (error != HA_ERR_UNSUPPORTED) {
    cleanup_pq_resources(true);
    PrintError(error);
    return true;
  }

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

int PQTableScanIterator::Read() {
  assert(m_serial_iterator != nullptr);
  return m_serial_iterator->Read();
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

  return NewIterator<PQTableScanIterator>(thd, mem_root, mem_root, table, join,
                                          expected_rows, examined_rows);
}
