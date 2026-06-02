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
  - Phase 5B does NOT start real worker threads, does NOT call InnoDB
    PQ API for real scanning, and does NOT change default query behavior.
  - PQTableScanIterator::Init() and Read() are conservative stubs.
    Init() always returns true (failure) in Phase 5B; however, this stub
    is never actually invoked because TryCreatePQTableScanIterator always
    returns nullptr. The safety mechanism is at the factory level: the
    guarded factory returns nullptr in Phase 5B, so no PQ iterator is ever
    created, and the serial TableScanIterator path is always taken.
  - There is no mechanism in CreateIteratorFromAccessPath() to call Init()
    and fallback on failure — the factory must guarantee that a returned
    iterator will work. Hence, Phase 5B never returns a PQ iterator.

  Design rationale:
  - PQTableScanIterator inherits TableRowIterator to reuse UnlockRow(),
    SetNullRowFlag(), HandleError(), and StartPSIBatchMode().
  - In production (Phase 6+), PQTableScanIterator will:
    1. Init(): call handler pq_leader_scan_init() to partition the table,
       then start workers via Gather_operator.
    2. Read(): pull rows from Exchange_nosort (worker results).
    3. End(): signal workers to stop, join threads, release resources.
  - Phase 5B skeleton only creates the class shape; it never enters
    the PQ execution path because Init() returns true (error/fallback).

  Fallback guarantee:
  - TryCreatePQTableScanIterator checks: parallel_query ON, join != nullptr,
    join->pq_eligible == true, table is InnoDB, AccessPath is TABLE_SCAN.
  - If any condition fails, returns nullptr => serial path taken.
  - In Phase 5B, even if all conditions pass, returns nullptr => serial path
    taken. There is no mechanism in CreateIteratorFromAccessPath() to call
    Init() and create a fallback iterator; the factory must guarantee that
    any returned iterator will work.
*/

#include "my_alloc.h"            // unique_ptr_destroy_only
#include "my_base.h"             // ha_rows
#include "sql/iterators/row_iterator.h"

class THD;
struct TABLE;
class JOIN;
struct MEM_ROOT;

/**
  Parallel table scan iterator skeleton.

  Phase 5B: conservative stub. Init() returns true (failure), Read()
  returns -1 (EOF per RowIterator convention). No real worker threads
  are launched. Note: these stubs are never actually invoked because
  TryCreatePQTableScanIterator always returns nullptr in Phase 5B,
  forcing serial TableScanIterator to be used instead.

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
  PQTableScanIterator(THD *thd, TABLE *table, JOIN *join,
                      double expected_rows, ha_rows *examined_rows);

  ~PQTableScanIterator() override;

  /**
    Initialize the parallel scan.

    Phase 5B stub: always returns true (failure). Note: this stub is
    never actually invoked because TryCreatePQTableScanIterator returns
    nullptr in Phase 5B, so no PQ iterator is ever created. The Init()
    stub exists purely to satisfy the RowIterator interface contract.

    In production (Phase 6+):
    - Call handler->pq_leader_scan_init() to partition the table.
    - If pq_leader_scan_init() returns HA_ERR_UNSUPPORTED or any error,
      Init() returns true (failure). The factory (TryCreate) must then
      implement in-iterator fallback to serial scan, because
      CreateIteratorFromAccessPath() cannot re-create a serial iterator
      after returning a PQ one.
    - If successful, create Gather_operator and start workers.
    - If worker start fails, Init() returns true => in-iterator fallback.

    @retval false  Success (Phase 5B: never reached)
    @retval true   Failure (Phase 5B: always, but never called)
  */
  bool Init() override;

  /**
    Read one row from the parallel scan result channel.

    Phase 5B stub: always returns -1 (EOF per RowIterator convention).
    Note: this stub is never actually invoked because TryCreate returns
    nullptr in Phase 5B.

    In production (Phase 6+):
    - Pull rows from Exchange_nosort (round-robin from worker MQs).
    - Return 0 for a row, -1 when all workers finish (EOF),
      or 1 for error (via HandleError).

    @retval 0    Row read successfully (Phase 5B: never reached)
    @retval -1   EOF — no more rows (Phase 5B: always, but never called)
    @retval 1    Error
  */
  int Read() override;

 private:
  JOIN *m_join;                ///< JOIN context for PQ eligibility
  double m_expected_rows;      ///< Expected rows for buffer scaling
  ha_rows *m_examined_rows;    ///< Examined rows counter
  uchar *m_record;             ///< Record buffer (table->record[0])
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

  Phase 5B: even when all guards pass, this function returns nullptr.
  This is NOT because Init() fails and the caller falls back — there
  is no such mechanism in CreateIteratorFromAccessPath(). Rather, the
  safety guarantee is that TryCreatePQTableScanIterator never returns
  a PQ iterator in Phase 5B, so the serial path is always taken.
  Phase 6+ will change this to return a real PQ iterator, but must
  then implement in-iterator fallback (PQTableScanIterator holds a
  TableScanIterator member that takes over on Init() failure), because
  CreateIteratorFromAccessPath() cannot re-create iterators after
  returning.

  If any guard fails, returns nullptr. The caller must then create
  a serial TableScanIterator.

  @param thd            Thread context
  @param mem_root       MEM_ROOT for iterator allocation
  @param table          Table to scan
  @param join           JOIN object (may be nullptr)
  @param expected_rows  Expected row count for record buffer scaling
  @param examined_rows  Pointer to examined_rows counter (may be nullptr)

  @return non-null  PQTableScanIterator created (Phase 6+: when Init() can succeed)
  @return nullptr   Conditions not met or PQ not ready (Phase 5B: always nullptr)
*/
unique_ptr_destroy_only<RowIterator> TryCreatePQTableScanIterator(
    THD *thd, MEM_ROOT *mem_root, TABLE *table, JOIN *join,
    double expected_rows, ha_rows *examined_rows);

#endif  // PQ_ITERATOR_INCLUDED