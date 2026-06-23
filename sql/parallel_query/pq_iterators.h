/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_PARALLEL_QUERY_PQ_ITERATORS_H
#define SQL_PARALLEL_QUERY_PQ_ITERATORS_H

#include "my_base.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/iterators/row_iterator.h"

class Gather_operator;
class JOIN;
class MQueue_handle;
class PQ_Leader_context;
class PQ_Worker_context;
class QEP_TAB;
class THD;
struct MEM_ROOT;
struct AccessPath;
struct Index_lookup;
struct TABLE;

/**
  Commercial PQ leader iterator skeleton.

  M2 only reserves the class boundary. Init() is fail-closed until worker plan
  clone, Query_result_mq, and handler/InnoDB commercial scan contracts are
  migrated in later stages.
*/
class ParallelScanIterator final : public TableRowIterator {
 public:
  ParallelScanIterator(THD *thd, QEP_TAB *tab, TABLE *table,
                       double expected_rows, ha_rows *examined_rows, JOIN *join,
                       Gather_operator *gather, bool stab_output = false,
                       AccessPath *access_path = nullptr);
  ~ParallelScanIterator() override;

  bool Init() override;
  int Read() override;

 private:
  enum class Lifecycle_state : uint8_t {
    CONSTRUCTED,
    INITIALIZING,
    RUNNING,
    ORDER_GATHER_VALIDATED,
    FAIL_CLOSED,
    CLEANED_UP,
  };

  void cleanup_lifecycle(bool init_failed);

  // Borrowed optimizer/executor pointers. ParallelScanIterator must not delete
  // or mutate ownership of these objects. The TABLE pointer held by the
  // TableRowIterator base class is also borrowed from the executor.
  QEP_TAB *m_tab;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  JOIN *m_join;
  // Borrowed in the current fail-closed skeleton. A future positive path may
  // allocate its own Gather_operator and set m_owns_gather.
  Gather_operator *m_gather;
  PQ_Leader_context *m_leader_ctx{nullptr};
  const bool m_stable_output;
  AccessPath *m_root_access_path;
  Lifecycle_state m_lifecycle_state{Lifecycle_state::CONSTRUCTED};
  bool m_owns_gather{false};
  bool m_cleanup_done{false};
  bool m_worker_started{false};
  bool m_no_fallback_commit{false};
  bool m_executed_counted{false};
};

/**
  Commercial PQ worker block-scan iterator skeleton.
*/
class PQblockScanIterator final : public TableRowIterator {
 public:
  PQblockScanIterator(THD *thd, TABLE *table, double expected_rows,
                      ha_rows *examined_rows, PQTabType tab_type,
                      Gather_operator *gather, QEP_TAB *tab,
                      bool need_rowid = false,
                      MQueue_handle *handler = nullptr);
  ~PQblockScanIterator() override;

  bool Init() override;
  int Read() override;
  int End();

 private:
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  PQTabType m_tab_type;
  Gather_operator *m_gather;
  QEP_TAB *m_tab;
  const bool m_need_rowid;
  MQueue_handle *m_handler;
  PQ_Worker_context *m_worker_ctx{nullptr};
  bool m_seen_eof{false};
  bool m_inited{false};
};

/**
  Commercial PQ worker ref iterator skeleton.
*/
class PQRefIterator final : public TableRowIterator {
 public:
  PQRefIterator(THD *thd, TABLE *table, Index_lookup *ref, bool use_order,
                PQTabType tab_type, double expected_rows,
                ha_rows *examined_rows, Gather_operator *gather, QEP_TAB *tab);
  ~PQRefIterator() override;

  bool Init() override;
  int Read() override;
  int End();

 private:
  Index_lookup *const m_ref;
  const bool m_use_order;
  PQTabType m_tab_type;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  Gather_operator *m_gather;
  QEP_TAB *m_tab;
  PQ_Worker_context *m_worker_ctx{nullptr};
  bool m_seen_eof{false};
  bool m_first_record_since_init{true};
  bool m_inited{false};
};

unique_ptr_destroy_only<RowIterator> TryCreatePQSecondaryCoveringRangeIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *path,
    ha_rows *examined_rows, bool is_root_range_scan);

unique_ptr_destroy_only<RowIterator> TryCreatePQSecondaryCoveringRefIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *path,
    ha_rows *examined_rows, bool is_root_ref);

/**
  Run the DBUG-only ParallelScanIterator lifecycle smoke.

  The helper constructs the commercial iterator skeleton directly, calls Init()
  and expects the current fail-closed result. It must not call Read(), handler
  scan APIs, worker launch, or the normal AccessPath factory.

  @retval false  Expected fail-closed lifecycle path completed
  @retval true   Unexpected success or invalid input
*/
bool pq_run_parallel_scan_lifecycle_smoke(THD *thd);

#endif  // SQL_PARALLEL_QUERY_PQ_ITERATORS_H
