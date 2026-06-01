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

#ifndef __CDE_HEAP_H__
#define __CDE_HEAP_H__

#include "field_types.h"
#include "sql/field.h"
#include "sql/table.h"

#include "common/dstore_common_utils.h"
#include "systable/dstore_relation.h"

#include "cde_dml_ctx.h"
#include "common/cde_alloc.h"
#include "dict/cde_dict.h"
#include "dict/cde_relation.h"
#include "heap/dstore_heap_interface.h"
#include "heap/dstore_heap_struct.h"
namespace CDE {

constexpr int16_t DSTORE_LOB_LEN_ATTR = -1;
constexpr bool DSTORE_LOB_BYVAL_ATTR = false;
constexpr char DSTORE_LOB_ALIGN_ATTR = 'c';

int CdeDatatypeMysqlToDstore(const Field *mysql_field, Oid *base_oid,
                             Oid *expand_oid, bool is_compact,
                             bool *isUnsigned);

/**
Covert dd column's datatype to dstore type.

@param[in]          ddCol     DD column to covert.
@param[in,out]      baseOid   Base oid type.
@param[in,out]      expandOid Expand oid type, eg char/bin.
@param[in]          isCompact Whether need compact for char type.
@param[in]          isUnsigned Boolean value indicating whether the type is
unsigned.

@return CDE_OK if success else error code.
*/
int DDTypeToDstore(const dd::Column *ddCol, Oid *baseOid, Oid *expandOid,
                   bool isCompact, bool *isUnsigned);
int CdeSetPrefixOid(uint32_t actual_n_fields, session_index_info *idx_dict,
                    Oid *index_col_oids);

/**
Convert dstore heap datum into dstore index datum. Only fields of the index
who have prefix length will be convert to a new varlena, fields of the index
who have no prefix length will just use the heap datum.

@param[in]     values    Datum array of all columns of heap.
@param[in]     isNulls   Null_flag array of all columns of heap.
@param[in/out] ctx       Dml context which contains the tuples of all indexes.

return CDE_SUCC if success, otherwise return CDE_FAIL.
*/
bool CdeDatumHeapToIndex(Datum *values, bool *isNulls, dml_ctx *ctx);

uint32_t GetAtMostNMbchars(uint32_t charset_no, uint32_t mbminlen,
                           uint32_t mbmaxlen, uint32_t prefix_len,
                           uint32_t data_len, const unsigned char *str);

/**
Convert a row from MySQL format to Dstore format when inserting a row.

@param[in]      table         the TABLE struct containing metadata
@param[in]      mysql_ptr_old the mysql row
@param[in,out] mysql_ptr_new the dstore row
@param[in]      tuple         tuple for the row
@param[in]      upd_ctx       the context of insert.
@param[in]      attr          attributes of tuple
@param[in]      skipVirtual   skip virtual columns

@return CDE_OK if success else error code.
*/
int MysqlRecordToDstoreForInsert(const TABLE *table,
                                 const unsigned char *mysql_ptr,
                                 dml_tuple_t *tuple, dml_ctx *ctx,
                                 DSTORE::TupleDesc attr, bool skipVirtual);

/**
Convert a row rom MySQL format to Dstore format when updating a row.

@param[in]      table         the TABLE struct containing metadata
@param[in]      mysql_ptr_old the mysql row
@param[in,out] mysql_ptr_new the dstore row
@param[in]      upd_ctx       the context of update.
@param[in]      tuple         tuple for new row
@param[in]      tuple_old     tuple for old row
@param[in]      attr          attributes of tuple
@param[in]      skipVirtual   skip virtual columns

@return CDE_OK if success else error code.
*/
int MysqlRecordToDstoreForUpd(const TABLE *table,
                              const unsigned char *mysql_ptr_old,
                              const unsigned char *mysql_ptr_new,
                              dml_upd_ctx *upd_ctx, dml_tuple_t *tuple,
                              dml_tuple_t *tupleOld, DSTORE::TupleDesc attr,
                              bool skipVirtual);

DSTORE::Datum CdeSetVarlenaWithPrefix(bool check_for_fk, DSTORE::Oid type_oid,
                                      DSTORE::Datum &src_datum,
                                      const cde_dict_field_t *field_dict,
                                      CdeVarlena *for_varlena,
                                      bool parseFromDstore = false);
// todo: refactor
int key_mysql_to_dstore(const TABLE *table, session_index_info *idx_dict,
                        uint32_t index_col_num,
                        const unsigned char *mysql_key_ptr,
                        uint32_t mysql_key_len, DSTORE::Datum *index_values,
                        bool *index_is_nulls, DSTORE::TupleDesc attr);

/**
Convert a row in the DStore format to a row in the MySQL format.
Note this is used for heap table read.

@param[in]  table         the TABLE struct containing metadata
@param[in]  table_handler cde dict table struct
@param[out] mysql_ptr     mysql fields data
@param[in]  values        dstore data
@param[in]  is_nulls      columns is null or not
@param[in]  blobMemRoot   MEM_ROOT to use for memory allocation
@param[in]  allowBlobMemRootClearForReuse allow clear of blobMemRoot
                                          before usage
@return void.
*/
void DstoreHeapDataToMysql(const TABLE *table, unsigned char *mysql_ptr,
                           DSTORE::Datum *values, bool *is_nulls,
                           MEM_ROOT *blobMemRoot,
                           bool allowBlobMemRootClearForReuse,
                           const MysqlAndDstoreIndex *columnsToDecode,
                           size_t numColumnsToDecode,
                           const DSTORE::TupleDesc heapTupleDesc);
/**
Convert a row in the DStore format to a row in the MySQL format.
Note this is used for covering index read, no need to read heap table.

@param[in]  table         the TABLE struct containing metadata
@param[in]  table_handler cde dict table struct
@param[out] mysql_ptr     mysql fields data
@param[in]  values        dstore data
@param[in]  is_nulls      columns is null or not
@param[in]  index_cols    field number in heap table of index fields
@param[in]  attr_cols     field number in heap tupledesc attr of index fields
@param[in]  index_col_num index fields num

@return void.
*/
void DstoreIndexDataToMysql(const TABLE *table, unsigned char *mysql_ptr,
                            DSTORE::Datum *values, bool *is_nulls,
                            const uint32_t *index_cols,
                            const uint32_t *attr_cols, uint32_t index_col_num,
                            DSTORE::TupleDesc attr,
                            MEM_ROOT *blobMemRoot = nullptr,
                            bool onlyVirtualsFromReadSet = false);

/**
Gets the length of the specified type.
@param[in] type            The OID of the type.
@param[in] typeCache       The type cache.
@param[in] fieldPackedLen  The pack length of the field.

@return Returns the length of the specified type.
 */
int16_t CdeGetTypeRealLen(Oid type, const DSTORE::TypeCache *typeCache,
                          uint32_t fieldPackedLen);
/**
Determine whether the attribute of a given type is passed by value.
@param[in] type       The OID of the attribute.
@param[in] typeCache  The type cache.

@return Returns true if the attribute is passed by value, otherwise returns
false.
 */
bool CdeGetAttByVal(Oid type, const DSTORE::TypeCache *typeCache);

/**
Determines the attribute alignment based on the given type.
@param[in] type      The type of the attribute.
@param[in] typeCache The type cache.

@return The alignment character corresponding to the type.
 */
char CdeGetAttAlign(Oid type, const DSTORE::TypeCache *typeCache);

/**
Convert a row in the MySQL format to a row in the Dstore format.

@param[in]      mysqlPtr       mysql fields data
@param[in]      typeOid        dstore type
@param[in]      mysqlField     mysql field
@param[in]      useIndexFormat whether use to key data
@param[in,out]  dstorePtr      the result data ptr
@param[in]      varlena        variable length data descriptor
@param[in,out]  isAllocBlobMem whether alloced memory for blob data

@return CDE_OK or error code.
*/
int StoreMysqlFieldToDstoreFormat(const unsigned char *mysqlPtr, Oid typeOid,
                                  const Field *mysqlField, bool useIndexFormat,
                                  Datum &dstorePtr, CdeVarlena &varlena,
                                  bool *isAllocBlobMem = nullptr);

/**
Fetch the BLOB field from the tuple.

@param[in]      relation             dstore table handler
@param[in]      tuple                the tuple that contain BLOB columns
@param[in]      dstoreSnapshot       the snapshot of current tranction
@param[in]      isConsistentRead     wether is a consistent read

@return the blob tuple.
*/
DSTORE::HeapTuple *FetchBlobFiled(StorageRelation relation,
                                  DSTORE::HeapTuple *tuple,
                                  DSTORE::Snapshot dstoreSnapshot,
                                  bool isConsistentRead);

/**
Get virtual column value by calling server callback function

@param[in]      currentThd       current thd
@param[in]      mysqlTable       mysql table object
@param[in]      mysqlRec         mysql record
@param[in]      vCol             virtual column
@param[in]      typeOid          virtual column dstore type
@param[in/out]  tuple            tupe saved virtual column's value

@return CDE_OK or error code.
*/
int StoreVirtualFieldValueToDstore(THD *currentThd, TABLE *mysqlTable,
                                   unsigned char *mysqlRec,
                                   DictVirtualCol *vCol, Oid typeOid,
                                   dml_tuple_t *tuple);

} /* namespace CDE */
#endif  // __CDE_HEAP_H__
