#ifndef SQL_ITERATORS_PQ_ITERATORS_H_
#define SQL_ITERATORS_PQ_ITERATORS_H_

/* Copyright (c) 2025, Oracle and/or its affiliates.

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

/**
  @file
  Iterators define related to parallel query execution.
 */

#include "sql/iterators/row_iterator.h"
#include "sql/parallel_query/sql_parallel.h"

class JOIN;
class THD;
struct TABLE;
struct AccessPath;
class MQueue_handle;
class MQ_record_gather;
struct Index_lookup;

/**
 * Parallel scan iterator, which is used in parallel leader
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
  int End() override;

 public:
  void UnlockRow() override {}
  void SetNullRowFlag(bool) override {}
  void StartPSIBatchMode() override {}
  void EndPSIBatchModeIfStarted() override {}

 private:
  uchar *const m_record;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  uint m_dop;
  JOIN *m_join;
  Gather_operator *m_gather;
  MQ_record_gather *m_record_gather;
  ORDER *m_order; /** use for records merge sort */
  QEP_TAB *m_tab;

  bool m_stable_sort; /** determine whether using stable sort */
  uint m_ref_length;

  // materialize the inner subquery
  AccessPath *m_root_access_path{nullptr};

 private:
  /** construct filesort on leader when needing stab_output or merge_sort */
  bool pq_make_filesort(Filesort **sort);
  /** init m_record_gather */
  bool pq_init_record_gather();
  /** launch worker threads to execute parallel query */
  bool pq_launch_worker();
  /** wait all workers finished */
  void pq_wait_workers_finished();
  /** outoput parallel query error code */
  int pq_error_code();
  /** init the execution tree for materializing subquery */
  bool para_exec_init(AccessPath *path);
  /** execute scalar uncorrelated subquery */
  bool exec_scalar_uncorrelated_subquery();
};

class PQ_worker_manager;

/**
 * block scan iterator, which is used is in parallel worker.
 * a whole table is cut into many blocks for parallel scan
 */
class PQblockScanIterator final : public TableRowIterator {
 public:
  PQblockScanIterator(THD *thd, TABLE *table, double expected_rows,
                      ha_rows *examined_rows, PQTabType tabType,
                      Gather_operator *gather, QEP_TAB *tab,
                      bool need_rowid = false,
                      MQueue_handle *handler = nullptr);
  ~PQblockScanIterator() override;

  bool Init() override;
  int Read() override;
  int End() override;

 private:
  uchar *const m_record;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  void *m_pq_ctx;  // parallel query context
  uint keyno;
  Gather_operator *m_gather;
  PQTabType m_tabType;
  bool m_need_rowid;
  MQueue_handle *m_handler{nullptr};

  bool m_inited{false};
  /// When we use record buffer for caching records (in MySQL's record[0]
  /// format), we should retain worker's QEP_TAB structure to manage the record
  /// buffer.
  QEP_TAB *m_tab{nullptr};

  /// Whether the iterator has seen EOF from the handler during a Read() call.
  /// After this has been set to "true", it will remain "true" until the next
  /// call to Init(). The effect is that calling Read() after EOF returns EOF
  /// again.
  bool m_seen_eof{false};
  bool m_is_mvi_unique_filter_enabled{false};
};

/**
  Like RefIterator, but used in parallel Query
*/
class PQRefIterator final : public TableRowIterator {
 public:
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  PQRefIterator(THD *thd, TABLE *table, Index_lookup *ref, bool use_order,
                PQTabType tabType, double expected_rows, ha_rows *examined_rows,
                Gather_operator *gather, QEP_TAB *tab)
      : TableRowIterator(thd, table),
        m_ref(ref),
        m_use_order(use_order),
        m_tab(tab),
        m_expected_rows(expected_rows),
        m_examined_rows(examined_rows),
        m_gather(gather),
        m_tabType(tabType) {}
  ~PQRefIterator();

  bool Init() override;
  int Read() override;
  int End() override;

 private:
  Index_lookup *const m_ref;
  const bool m_use_order;
  QEP_TAB *const m_tab;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  bool m_first_record_since_init;
  Gather_operator *m_gather;
  PQTabType m_tabType;
  bool m_inited{false};

  /// Whether the iterator has seen EOF from the handler during a Read() call.
  /// After this has been set to "true", it will remain "true" until the next
  /// call to Init(). The effect is that calling Read() after EOF returns EOF
  /// again.
  bool m_seen_eof{false};
  bool m_is_mvi_unique_filter_enabled{false};
};

#endif  // SQL_ITERATORS_PQ_ITERATORS_H_
