/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef CDE_TUPLE_H
#define CDE_TUPLE_H

#include <iostream>
#include "dict/cde_dict.h"
#include "tuple/dstore_tuple_struct.h"

#include "my_alloc.h"
#include "sql/field.h"
#include "sql/table.h"
namespace CDE {
struct cde_field_def {
  uint32_t col_no;      /*!< column offset in new TABLE */
  uint32_t dict_col_no; /*!< column offset in dict TABLE */
  bool is_ascending;    /*!< true=ASC, false=DESC */
  /** not nullptr if is new added virtual column and used in new added index in
  inplace, otherwise is nullptr*/
  DictVirtualCol *vCol = nullptr;
  /** used only vCol is not null */
  const dd::Column *ddCol;
  /** used only vCol is not null, just simply the number of newly added virtual
   * columns */
  uint32_t virtualPos;
};

class cde_tuple_desc {
 public:
  /**
  Generate tupledesc memory object

  @param[in]      colNums         Col num of this tuple
  @param[in]      needInitDefVals Whether need init default value, set to true
                                  when process restart or reload dict table,
                                  and set to false when create table or for
                                  index.

  @return DSTORE::TupleDesc Tuple memory object generated
  */
  static DSTORE::TupleDesc GenerateTupleDescTemplate(
      uint32_t colNums, bool needInitDefVals = false);
  static DSTORE::TupleDesc build_heap_tuple_desc(DSTORE::Oid rel_oid,
                                                 const TABLE *m_form,
                                                 bool skipVirtual = false);
  /**
  Build heap tupledesc from DD

  @param[in]      relOid   Oid of this heap relation
  @param[in]      ddTable  DD table
  @param[in]      dictCols Dict col to init default value for tuple
  @param[in]      skipVirtual skip virtual columns

  @return TupleDesc build from dd and dict col
  */
  static DSTORE::TupleDesc BuildHeapTupleDescFromDD(DSTORE::Oid relOid,
                                                    const dd::Table *ddTable,
                                                    DictCol *dictCols,
                                                    bool skipVirtual = false);
  static DSTORE::TupleDesc build_index_tuple_desc(
      DSTORE::Oid index_oid, DSTORE::TupleDesc heap_tuple_desc, const KEY *key,
      uint32_t *index_cols, uint32_t *attrCols, DictCol *dictCols);
  static DSTORE::TupleDesc build_index_tuple_desc(
      DSTORE::Oid index_oid, DSTORE::TupleDesc heap_tuple_desc,
      const cde_field_def *fields, uint32_t n_fields, uint32_t *index_cols,
      uint32_t *attrCols, DictCol *dictCols);

  static size_t get_fixed_attr_size();
  static size_t get_tuple_desc_size(uint32_t col_nums);
  static size_t get_attr_size(uint32_t col_nums);
  static size_t GetInitDefValSize(uint32_t colNum);
  /**
  Get DstoreTupInitDefVal memory size needed for each entry, noticed that datum
  is a pointer, also need set a valid memory address.

  @return Aligned size of DstoreTupInitDefVal in memory.
  */
  static size_t GetDefValEntrySize();
  static DSTORE::TupleDesc clone(DSTORE::TupleDesc src_desc,
                                 MEM_ROOT *mem_root);
  cde_tuple_desc() {}
  ~cde_tuple_desc() {}
};
} /* namespace CDE */
#endif
