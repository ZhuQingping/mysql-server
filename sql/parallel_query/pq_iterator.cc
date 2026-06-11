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
  - Init() always returns true (failure) => stub only; never actually
    invoked because TryCreate returns nullptr in Phase 5B.
  - Read() always returns -1 (EOF per RowIterator convention) => stub
    only; never actually invoked.
  - No real worker launch, no InnoDB PQ API calls, no MQ interaction.
*/

#include "sql/parallel_query/pq_iterator.h"

#include "my_base.h"
#include "sql/mysqld.h"       // innodb_hton
#include "sql/parallel_query/sql_parallel.h"  // pq_global_stats
#include "sql/sql_class.h"    // THD::variables, THD::pq_is_worker
#include "sql/sql_lex.h"      // LEX::is_explain
#include "sql/sql_optimizer.h"  // JOIN::pq_eligible
#include "sql/table.h"        // TABLE, TABLE_SHARE::db_type

// ---------------------------------------------------------------------------
// PQTableScanIterator implementation (Phase 5B stub)
// ---------------------------------------------------------------------------

PQTableScanIterator::PQTableScanIterator(THD *thd, TABLE *table, JOIN *join,
                                         double expected_rows,
                                         ha_rows *examined_rows)
    : TableRowIterator(thd, table),
      m_join(join),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_record(table->record[0]) {}

PQTableScanIterator::~PQTableScanIterator() {
  // Phase 5B: no resources to release (no workers, no Gather_operator).
  // Phase 6+ will release PQ_Leader_context, Gather_operator, worker THDs.
}

bool PQTableScanIterator::Init() {
  // Phase 5B: always return true (failure).
  // Note: this stub is never actually invoked because
  // TryCreatePQTableScanIterator returns nullptr in Phase 5B,
  // so no PQ iterator is ever created. The Init() stub exists
  // purely to satisfy the RowIterator interface contract.
  //
  // Production (Phase 6+) Init() will:
  // 1. Validate table handler is ha_innobase.
  // 2. Call handler->pq_leader_scan_init() to partition table.
  // 3. If pq_leader_scan_init() returns error (e.g., HA_ERR_UNSUPPORTED),
  //    Init() returns true. PQTableScanIterator must then fall back to
  //    an internal serial TableScanIterator member (there is no mechanism
  //    for CreateIteratorFromAccessPath() to re-create a serial iterator
  //    after returning a PQ one).
  // 4. Create Gather_operator, start workers.
  // 5. If any step fails, Init() returns true => in-iterator fallback.
  // 6. On success, Init() returns false.

  return true;  // Phase 5B: always fail (stub only, never called).
}

int PQTableScanIterator::Read() {
  // Phase 5B: always return -1 (EOF per RowIterator convention).
  // Note: this stub is never actually invoked because
  // TryCreatePQTableScanIterator returns nullptr in Phase 5B,
  // so no PQ iterator is ever created.
  //
  // RowIterator::Read() convention:
  // - 0  = row read successfully
  // - -1 = EOF (no more rows)
  // - 1  = error
  //
  // Production (Phase 6+) Read() will:
  // 1. Pull a row from Exchange_nosort (worker MQ round-robin).
  // 2. If row available, copy to m_record and return 0.
  // 3. If all workers finished (MQ closed), return -1 (EOF).
  // 4. If error from worker, return HandleError(error_code).

  return -1;  // Phase 5B: always EOF (stub only, never called).
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

  // All guards passed. However, in Phase 5B, we must not create a
  // PQTableScanIterator because there is no mechanism for
  // CreateIteratorFromAccessPath() to call Init() and fallback on
  // failure — once the factory returns an iterator, the caller uses
  // it directly. The safety guarantee is at the factory level:
  // TryCreatePQTableScanIterator never returns a PQ iterator in
  // Phase 5B, so the serial TableScanIterator path is always taken.
  //
  // Phase 6+ change: remove this early-return and create a real
  // PQTableScanIterator. PQTableScanIterator must then implement
  // in-iterator fallback to serial scan (hold a TableScanIterator
  // member that takes over on Init() failure), because
  // CreateIteratorFromAccessPath() cannot re-create a serial
  // iterator after returning a PQ one.
  //
  // Silence unused-parameter warnings for Phase 5B parameters that
  // will be used in Phase 6+.
  static_cast<void>(mem_root);
  static_cast<void>(expected_rows);
  static_cast<void>(examined_rows);

  // EXPLAIN may build iterators, but it is not a real statement execution
  // fallback. Keep PQ execution status counters tied to non-EXPLAIN execution.
  if (thd->lex != nullptr && thd->lex->is_explain()) {
    return nullptr;
  }

  // Phase 8 execution fallback: all PQ guards passed, but the real
  // PQ iterator is intentionally disabled until worker execution is ready.
  // Count this at iterator creation time rather than optimizer time so EXPLAIN
  // and optimizer re-entry do not inflate execution fallback statistics.
  pq_global_stats.queries_fallback.fetch_add(1, std::memory_order_relaxed);
  thd->pq_executed = false;

  return nullptr;

  // Phase 6+: uncomment the following when Init() can succeed:
  // return NewIterator<PQTableScanIterator>(thd, mem_root, table, join,
  //                                         expected_rows, examined_rows);
}
