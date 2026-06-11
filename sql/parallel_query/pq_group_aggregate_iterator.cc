/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in license.xml
   elsewhere in this distribution.  You may use this software under the
   terms of the GNU General Public License, version 2.0,
   or the terms of any other license of the available in this distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/parallel_query/pq_group_aggregate_iterator.h"

#include "sql/join_optimizer/access_path.h"  // AccessPath
#include "sql/sql_class.h"                   // THD
#include "sql/sql_optimizer.h"               // JOIN

unique_ptr_destroy_only<RowIterator> TryCreatePQGroupAggregateIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *aggregate_path) {
  (void)mem_root;

  if (thd == nullptr || join == nullptr || aggregate_path == nullptr) {
    return nullptr;
  }

  if (aggregate_path->type != AccessPath::AGGREGATE) {
    return nullptr;
  }

  if (!thd->variables.parallel_query ||
      !thd->variables.parallel_query_experimental_groupby_dop1 ||
      thd->variables.parallel_default_dop != 1) {
    return nullptr;
  }

  if (!join->pq_eligible || aggregate_path->aggregate().rollup ||
      aggregate_path->aggregate().child == nullptr ||
      aggregate_path->aggregate().child->type != AccessPath::TABLE_SCAN) {
    return nullptr;
  }

  /*
    V2-12A-3.2 stops here intentionally. Later substeps will replace this
    nullptr with a real PQGroupAggregateIterator after typed state, SQL
    result-row construction, and child iterator ownership are implemented.
  */
  return nullptr;
}
