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

#include "sql/parallel_query/pq_iterators.h"

ParallelScanIterator::ParallelScanIterator(
    THD *thd, QEP_TAB *tab, TABLE *table, double expected_rows,
    ha_rows *examined_rows, JOIN *join, Gather_operator *gather,
    bool stab_output, AccessPath *access_path)
    : TableRowIterator(thd, table),
      m_tab(tab),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_join(join),
      m_gather(gather),
      m_stable_output(stab_output),
      m_root_access_path(access_path) {}

bool ParallelScanIterator::Init() { return true; }

int ParallelScanIterator::Read() { return 1; }

PQblockScanIterator::PQblockScanIterator(
    THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows,
    PQTabType tab_type, Gather_operator *gather, QEP_TAB *tab, bool need_rowid,
    MQueue_handle *handler)
    : TableRowIterator(thd, table),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_tab_type(tab_type),
      m_gather(gather),
      m_tab(tab),
      m_need_rowid(need_rowid),
      m_handler(handler) {}

bool PQblockScanIterator::Init() { return true; }

int PQblockScanIterator::Read() { return 1; }

PQRefIterator::PQRefIterator(THD *thd, TABLE *table, Index_lookup *ref,
                             bool use_order, PQTabType tab_type,
                             double expected_rows, ha_rows *examined_rows,
                             Gather_operator *gather, QEP_TAB *tab)
    : TableRowIterator(thd, table),
      m_ref(ref),
      m_use_order(use_order),
      m_tab_type(tab_type),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows),
      m_gather(gather),
      m_tab(tab) {}

bool PQRefIterator::Init() { return true; }

int PQRefIterator::Read() { return 1; }
