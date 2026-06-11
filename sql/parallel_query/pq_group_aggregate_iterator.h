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

#ifndef PQ_GROUP_AGGREGATE_ITERATOR_INCLUDED
#define PQ_GROUP_AGGREGATE_ITERATOR_INCLUDED

#include "my_alloc.h"     // unique_ptr_destroy_only
#include "my_inttypes.h"  // uint32, uint64

class JOIN;
class RowIterator;
class THD;
struct AccessPath;
struct MEM_ROOT;

/**
  Try to create a PQ GROUP BY aggregate iterator for an AGGREGATE access path.

  V2-12A-3.2 only establishes the safe factory hook. It deliberately returns
  nullptr after guard checks, so the native AggregateIterator remains the only
  executable path.
*/
unique_ptr_destroy_only<RowIterator> TryCreatePQGroupAggregateIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *aggregate_path);

bool RunPQGroupAggregateTypedStateSmoke(uint32 *groups_built,
                                        uint64 *sum_total);

#endif  // PQ_GROUP_AGGREGATE_ITERATOR_INCLUDED
