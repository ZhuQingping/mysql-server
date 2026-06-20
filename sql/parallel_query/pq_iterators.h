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

  bool Init() override;
  int Read() override;

 private:
  QEP_TAB *m_tab;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  JOIN *m_join;
  Gather_operator *m_gather;
  const bool m_stable_output;
  AccessPath *m_root_access_path;
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

  bool Init() override;
  int Read() override;

 private:
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  PQTabType m_tab_type;
  Gather_operator *m_gather;
  QEP_TAB *m_tab;
  const bool m_need_rowid;
  MQueue_handle *m_handler;
};

/**
  Commercial PQ worker ref iterator skeleton.
*/
class PQRefIterator final : public TableRowIterator {
 public:
  PQRefIterator(THD *thd, TABLE *table, Index_lookup *ref, bool use_order,
                PQTabType tab_type, double expected_rows,
                ha_rows *examined_rows, Gather_operator *gather, QEP_TAB *tab);

  bool Init() override;
  int Read() override;

 private:
  Index_lookup *const m_ref;
  const bool m_use_order;
  PQTabType m_tab_type;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  Gather_operator *m_gather;
  QEP_TAB *m_tab;
};

unique_ptr_destroy_only<RowIterator> TryCreatePQSecondaryCoveringRangeIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *path,
    ha_rows *examined_rows, bool is_root_range_scan);

#endif  // SQL_PARALLEL_QUERY_PQ_ITERATORS_H
