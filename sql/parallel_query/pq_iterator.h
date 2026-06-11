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

#ifndef PQ_ITERATOR_INCLUDED
#define PQ_ITERATOR_INCLUDED

/**
  @file sql/parallel_query/pq_iterator.h
  Parallel Query V1-MVP: PQ table scan iterator skeleton.

  Phase 5B scope:
  - Define PQTableScanIterator: a TableRowIterator subclass that will
    eventually orchestrate parallel table scans via InnoDB PQ API.
  - Define TryCreatePQTableScanIterator helper: guarded factory that
    returns nullptr if any PQ condition is not met, forcing fallback
    to serial TableScanIterator.
  - V2-1 does NOT start real worker threads, does NOT call InnoDB
    PQ API for real scanning, and does NOT change default query behavior.
  - V2-1 lets ordinary eligible execution return a PQTableScanIterator.
    Init() falls back to an owned serial TableScanIterator before any worker,
    InnoDB PQ API, or irreversible handler state is touched.
  - EXPLAIN still does not create a PQ iterator and therefore never increments
    execution fallback counters.

  Design rationale:
  - PQTableScanIterator inherits TableRowIterator to reuse UnlockRow(),
    SetNullRowFlag(), HandleError(), and StartPSIBatchMode().
  - In production (Phase 6+), PQTableScanIterator will:
    1. Init(): call handler pq_leader_scan_init() to partition the table,
       then start workers via Gather_operator.
    2. Read(): pull rows from Exchange_nosort (worker results).
    3. End(): signal workers to stop, join threads, release resources.
  - V2-1 enters the PQ iterator shape but delegates execution to an owned
    serial TableScanIterator until real worker execution is ready.

  Fallback guarantee:
  - TryCreatePQTableScanIterator checks: parallel_query ON, join != nullptr,
    join->pq_eligible == true, table is InnoDB, AccessPath is TABLE_SCAN.
  - If any condition fails, returns nullptr => serial path taken.
  - In V2-1, ordinary eligible execution returns a PQ iterator. That iterator
    owns a serial fallback iterator and uses it from Init().
*/

#include "my_alloc.h"            // unique_ptr_destroy_only
#include "my_base.h"             // ha_rows
#include "sql/iterators/row_iterator.h"

#include <cassert>

class THD;
struct TABLE;
class JOIN;
struct MEM_ROOT;
class Gather_operator;
class PQ_Leader_context;

/**
  Parallel table scan iterator skeleton.

  V2-1: ownership and safe fallback scaffold. No real worker threads are
  launched. Init() switches to an owned serial TableScanIterator before any
  irreversible PQ state is created.

  In production (Phase 6+):
  - Init() will call handler->pq_leader_scan_init() to set up InnoDB
    parallel scan context, then start DOP workers via Gather_operator.
  - Read() will pull rows from Exchange_nosort (worker result channel).
    Returns 0 for a row, -1 for EOF, or 1 for error.
  - The iterator owns the Gather_operator and is responsible for
    cleanup on error or end-of-scan.
*/
class PQTableScanIterator final : public TableRowIterator {
 public:
  /**
    Construct a PQ table scan iterator.

    @param thd            Thread context
    @param table          Table to scan
    @param join           JOIN object (must have pq_eligible == true)
    @param expected_rows  Expected row count for record buffer scaling
    @param examined_rows  Pointer to examined_rows counter (may be nullptr)
  */
  PQTableScanIterator(THD *thd, MEM_ROOT *mem_root, TABLE *table, JOIN *join,
                      double expected_rows, ha_rows *examined_rows);

  ~PQTableScanIterator() override;

  /**
    Initialize the parallel scan.

    V2-1/V2-2 safe fallback: optionally probe the handler leader init/end
    contract with DOP=1, then create and initialize an owned serial
    TableScanIterator before workers, exchange, or row streams are touched.

    In production (Phase 6+):
    - Call handler->pq_leader_scan_init() to partition the table.
    - If pq_leader_scan_init() returns HA_ERR_UNSUPPORTED, continue with
      in-iterator serial fallback. CreateIteratorFromAccessPath() cannot
      re-create a serial iterator after returning a PQ one.
    - If pq_leader_scan_init() returns a fatal handler error, Init() returns
      true and reports the error instead of masking it as fallback.
    - If successful, create Gather_operator and start workers.
    - If worker start fails, Init() returns true => in-iterator fallback.

    @retval false  Success (Phase 5B: never reached)
    @retval true   Failure (Phase 5B: always, but never called)
  */
  bool Init() override;

  /**
    Read one row from the parallel scan result channel.

    V2-1 fallback: delegate to the owned serial TableScanIterator.

    In production (Phase 6+):
    - Pull rows from Exchange_nosort (round-robin from worker MQs).
    - Return 0 for a row, -1 when all workers finish (EOF),
      or 1 for error (via HandleError).

    @retval 0    Row read successfully (Phase 5B: never reached)
    @retval -1   EOF — no more rows (Phase 5B: always, but never called)
    @retval 1    Error
  */
  int Read() override;
  void UnlockRow() override;
  void SetNullRowFlag(bool is_null_row) override;
  void StartPSIBatchMode() override;
  void EndPSIBatchModeIfStarted() override;

 private:
  enum class Runtime_state {
    SAFE_FALLBACK,
    PQ_STARTED,
    PQ_ROW_RETURNED,
  };

  /** Release any PQ resources owned by this iterator. */
  void cleanup_pq_resources(bool abort_workers);

  /** @return true while serial fallback is still allowed. */
  bool can_fallback_serial() const {
    return m_runtime_state == Runtime_state::SAFE_FALLBACK;
  }

  /** Mark the no-fallback point where real workers or row stream start. */
  void mark_pq_started() {
    assert(can_fallback_serial());
    m_runtime_state = Runtime_state::PQ_STARTED;
  }

  /** Mark that a real PQ row has been returned to the SQL executor. */
  void mark_pq_row_returned() {
    assert(m_runtime_state == Runtime_state::PQ_STARTED ||
           m_runtime_state == Runtime_state::PQ_ROW_RETURNED);
    m_runtime_state = Runtime_state::PQ_ROW_RETURNED;
  }

  MEM_ROOT *m_mem_root;        ///< MEM_ROOT used for owned fallback iterator
  JOIN *m_join;                ///< JOIN context for PQ eligibility
  double m_expected_rows;      ///< Expected rows for buffer scaling
  ha_rows *m_examined_rows;    ///< Examined rows counter
  uchar *m_record;             ///< Record buffer (table->record[0])
  unique_ptr_destroy_only<RowIterator> m_serial_iterator;  ///< V2-1 fallback
  PQ_Leader_context *m_leader_ctx{nullptr};  ///< Handler leader context
  Gather_operator *m_gather{nullptr};        ///< Worker lifecycle owner
  Runtime_state m_runtime_state{Runtime_state::SAFE_FALLBACK};
  bool m_fallback_counted{false};  ///< Count per query iterator, not per Init()
};

/**
  Guarded factory: try to create a PQTableScanIterator if all conditions
  are met, otherwise return nullptr to indicate serial fallback is needed.

  Conditions checked (all must pass):
  1. parallel_query system variable is ON
  2. join != nullptr (TABLE_SCAN path has a valid JOIN)
  3. join->pq_eligible == true (optimizer marked this query as PQ-eligible)
  4. table uses InnoDB engine (innodb_hton check)
  5. THD is not a PQ worker (no recursive parallelism)

  V2-1: ordinary eligible execution returns a PQTableScanIterator. EXPLAIN
  still returns nullptr so explain-only paths do not affect execution counters.

  If any guard fails, returns nullptr. The caller must then create
  a serial TableScanIterator.

  @param thd            Thread context
  @param mem_root       MEM_ROOT for iterator allocation
  @param table          Table to scan
  @param join           JOIN object (may be nullptr)
  @param expected_rows  Expected row count for record buffer scaling
  @param examined_rows  Pointer to examined_rows counter (may be nullptr)

  @return non-null  PQTableScanIterator created for ordinary eligible execution
  @return nullptr   Conditions not met, EXPLAIN, or PQ not ready for that path
*/
unique_ptr_destroy_only<RowIterator> TryCreatePQTableScanIterator(
    THD *thd, MEM_ROOT *mem_root, TABLE *table, JOIN *join,
    double expected_rows, ha_rows *examined_rows);

#endif  // PQ_ITERATOR_INCLUDED
