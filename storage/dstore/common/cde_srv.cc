/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include "common/cde_srv.h"

#include "dml/cde_heap.h"
#include "tuple/dstore_memheap_tuple.h"
#include "tuple/dstore_tuple_interface.h"

namespace CDE {

void dstore_handler_t::DecodeDstoreRow(DSTORE::TupleDescData *tupleDesc,
                                       DSTORE::Datum *values,
                                       DSTORE::HeapTuple *tuple, uchar *buf,
                                       const TABLE *table, MEM_ROOT *memRoot,
                                       bool allowBlobMemRootClearForReuse) {
  if (m_numColumnsToDecode == 0) {
    return;
  }

  DSTORE::TupleAttrContext attrContext = {tupleDesc, values, m_nulls, 0, true};
  // In order to properly deform column N in a row, all columns preceding column
  // N must also be decoded as the decoding of one column calculates the offset
  // to the next column. So begin at 0, and decode all columns up until the last
  // one we need.
  const int start = 0;

  // Decode up to the last column we need, but pass in one-past-the-end as
  // that's what the API expects.
  tuple->DeformTuplePart(attrContext, start, m_deformEndIndex + 1);

  CDE_ASSERT(nullptr != rel_info);

  DstoreHeapDataToMysql(table, buf, values, m_nulls, memRoot,
                        allowBlobMemRootClearForReuse, m_columnsToDecode,
                        m_numColumnsToDecode,
                        rel_info->rd_storage_releation->attr);
}
}  // namespace CDE
