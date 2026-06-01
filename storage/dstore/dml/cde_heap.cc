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

#include "securec.h"

#include "dd_table_share.h"
#include "field_types.h"
#include "my_byteorder.h"
#include "my_inttypes.h"
#include "mysql_com.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/table.h"

#include "catalog/dstore_fake_attribute.h"
#include "errorcode/dstore_heap_error_code.h"
#include "systable/systable_type.h"
#include "tuple/dstore_tuple_interface.h"

#include "common/cde_alloc.h"
#include "common/cde_compare_utils.h"
#include "common/cde_def.h"
#include "common/cde_errorcode.h"
#include "common/cde_trxmgr.h"
#include "common/cde_typecache.h"
#include "ddl/dd_helper.h"
#include "dml/cde_btree.h"
#include "dml/cde_heap.h"
#include "framework/dstore_thread_interface.h"
#include "handler/ha_cde.h"
#include "transaction/dstore_transaction_struct.h"

using DSTORE::DSTORE_INVALID_BLOCK_NUMBER;
using DSTORE::DSTORE_INVALID_OID;
using DSTORE::DSTORE_SUCC;
using DSTORE::FormData_pg_attribute;
using DSTORE::HEAP_ERROR_TUPLE_IS_DELETED;
using DSTORE::INVALID_DATUM;
using DSTORE::INVALID_VFS_FILE_ID;
using DSTORE::ItemPointer;
using DSTORE::ItemPointerData;
using DSTORE::PageId;
using DSTORE::RelationKind;
using DSTORE::RetStatus;
using DSTORE::StorageRelation;
using DSTORE::StorageRelationData;
using DSTORE::SYS_PARTTYPE_NON_PARTITIONED_RELATION;
using DSTORE::SYS_RELPERSISTENCE_PERMANENT;
using DSTORE::SysClassTupDef;
using DSTORE::TablespaceId;
using DSTORE::TupleDesc;
using DSTORE::TupleDescData;

namespace CDE {

static inline bool CdeCheckMysqlIntUnsigned(const Field *mysql_field) {
  return mysql_field->is_flag_set(UNSIGNED_FLAG);
}

static inline bool CheckDDIntUnsigned(const dd::Column *ddCol) {
  return ddCol->is_unsigned();
}

static inline int CdeHandleMysqlEnumType(const Field *mysql_field,
                                         Oid *base_oid, Oid *expand_oid,
                                         bool *isUnsigned) {
  uint32_t length = mysql_field->pack_length();
  if (length == 1) {
    *base_oid = INT1OID;
    *expand_oid = INT1OID;
    *isUnsigned = true;
    return CDE_OK;
  } else if (length == 2) {
    *base_oid = INT2OID;
    *expand_oid = INT2OID;
    *isUnsigned = true;
    return CDE_OK;
  }
  return CDE_ERROR;
}

/**
Convert SET type to DStore representation based on the number of elements in the
set.

@param[in]          mysqlField
@param[in,out]      baseOid
@param[in,out]      expandOid
@param[in,out]      isUnsigned

@return CDE_OK      Covert success.
@return CDE_ERROR   Covert failed.
 */
static inline int CdeHandleMysqlSetType(const Field *mysqlField, Oid *baseOid,
                                        Oid *expandOid, bool *isUnsigned) {
  uint32_t length = mysqlField->pack_length();

  switch (length) {
    case 1:
      *baseOid = INT1OID;
      *expandOid = INT1OID;
      break;
    case 2:
      *baseOid = INT2OID;
      *expandOid = INT2OID;
      break;
    case 3:
      *baseOid = INT3OID;
      *expandOid = INT3OID;
      break;
    case 4:
      *baseOid = INT4OID;
      *expandOid = INT4OID;
      break;
    case 8:
      *baseOid = INT8OID;
      *expandOid = INT8OID;
      break;
    default:
      return CDE_ERROR;
  }

  *isUnsigned = true;
  return CDE_OK;
}

/**
Covert dd enum type to dstore based on enum dataset nums.

@param[in]          ddCol
@param[in,out]      baseOid
@param[in,out]      expandOid

@return CDE_OK      Covert success.
@return CDE_ERROR   Covert failed.
*/
static inline int HandleDDEnumType(const dd::Column *ddCol, Oid *baseOid,
                                   Oid *expandOid, bool *isUnsigned) {
  uint32_t length = DDGetColumnPackLength(ddCol);
  if (length == 1) {
    *baseOid = INT1OID;
    *expandOid = INT1OID;
    *isUnsigned = true;
    return CDE_OK;
  } else if (length == 2) {
    *baseOid = INT2OID;
    *expandOid = INT2OID;
    *isUnsigned = true;
    return CDE_OK;
  }
  return CDE_ERROR;
}

/**
Convert SET type to DStore representation based on the number of elements in the
set.

@param[in]          ddCol
@param[in,out]      baseOid
@param[in,out]      expandOid
@param[in,out]      isUnsigned

@return CDE_OK      Covert success.
@return CDE_ERROR   Covert failed.
*/
static inline int HandleDDSetType(const dd::Column *ddCol, Oid *baseOid,
                                  Oid *expandOid, bool *isUnsigned) {
  uint32_t length = DDGetColumnPackLength(ddCol);

  switch (length) {
    case 1:
      *baseOid = INT1OID;
      *expandOid = INT1OID;
      break;
    case 2:
      *baseOid = INT2OID;
      *expandOid = INT2OID;
      break;
    case 3:
      *baseOid = INT3OID;
      *expandOid = INT3OID;
      break;
    case 4:
      *baseOid = INT4OID;
      *expandOid = INT4OID;
      break;
    case 8:
      *baseOid = INT8OID;
      *expandOid = INT8OID;
      break;
    default:
      return CDE_ERROR;
  }

  *isUnsigned = true;
  return CDE_OK;
}

int16_t CdeGetTypeRealLen(Oid type, const DSTORE::TypeCache *typeCache,
                          uint32_t fieldPackedLen) {
  uint32_t len;
  switch (type) {
    case CDE_BIT_OID:
    case CDE_BIN_OID:
      len = fieldPackedLen;
      CDE_ASSERT(len < CDE_TYPE_MAX_LEN);
      return static_cast<int16_t>(len);
    default:
      return typeCache->attlen;
  }
  return 0;
}

// see: TYPE_CACHE_TABLE
int CdeDatatypeMysqlToDstore(const Field *mysql_field, Oid *base_oid,
                             Oid *expand_oid, bool is_compact,
                             bool *isUnsigned __attribute__((unused))) {
  int ret = CDE_OK;
  CDE_ASSERT((base_oid != nullptr) && (expand_oid != nullptr));
  enum_field_types mysql_type = mysql_field->type();
  enum_field_types mysql_real_type = mysql_field->real_type();
  uint32_t charset_no;

  if (mysql_real_type == MYSQL_TYPE_ENUM) {
    return CdeHandleMysqlEnumType(mysql_field, base_oid, expand_oid,
                                  isUnsigned);
  }
  if (mysql_real_type == MYSQL_TYPE_SET) {
    return CdeHandleMysqlSetType(mysql_field, base_oid, expand_oid, isUnsigned);
  }
  switch (mysql_type) {
    case MYSQL_TYPE_TINY:
      *base_oid = INT1OID;
      *expand_oid = INT1OID;
      *isUnsigned = CdeCheckMysqlIntUnsigned(mysql_field);
      break;
    case MYSQL_TYPE_SHORT:
      *base_oid = INT2OID;
      *expand_oid = INT2OID;
      *isUnsigned = CdeCheckMysqlIntUnsigned(mysql_field);
      break;
    case MYSQL_TYPE_LONG:
      *base_oid = INT4OID;
      *expand_oid = INT4OID;
      *isUnsigned = CdeCheckMysqlIntUnsigned(mysql_field);
      break;
    case MYSQL_TYPE_LONGLONG:
      *base_oid = INT8OID;
      *expand_oid = INT8OID;
      *isUnsigned = CdeCheckMysqlIntUnsigned(mysql_field);
      break;
    case MYSQL_TYPE_INT24:
      *base_oid = INT3OID;
      *expand_oid = INT3OID;
      *isUnsigned = CdeCheckMysqlIntUnsigned(mysql_field);
      break;
    case MYSQL_TYPE_FLOAT:
      *base_oid = FLOAT4OID;
      *expand_oid = FLOAT4OID;
      break;
    case MYSQL_TYPE_DOUBLE:
      *base_oid = FLOAT8OID;
      *expand_oid = FLOAT8OID;
      break;
    case MYSQL_TYPE_STRING:
      if (mysql_real_type == MYSQL_TYPE_STRING) {
        charset_no = mysql_field->charset()->number;
        if (charset_no > MAX_COLLATION_NUM) {
          CDE_LOG_ERROR("invalid collation num[%u] from server.", charset_no);
          ret = CDE_ERROR;
          break;
        }
        if (is_compact) {
          *base_oid = CDE_COMPACT_CHAR_OID;
          *expand_oid = CDE_COMPACT_CHAR_OID | (charset_no << OID_COLL_SHIFT);
        } else {
          *base_oid = CDE_CHAR_OID;
          *expand_oid = CdeGetCharOid(mysql_field->pack_length()) |
                        (charset_no << OID_COLL_SHIFT);
        }
        break;
      }
      [[fallthrough]];
    case MYSQL_TYPE_NEWDECIMAL:
      *base_oid = CDE_BIN_OID;
      *expand_oid = CdeGetBinOid(mysql_field->pack_length());
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_BOOL:
      *base_oid = INT1OID;
      *expand_oid = INT1OID;
      *isUnsigned = true;
      break;
    case MYSQL_TYPE_VARCHAR:
      charset_no = mysql_field->charset()->number;
      if (charset_no > MAX_COLLATION_NUM) {
        CDE_LOG_ERROR("invalid collation num[%u] from server.", charset_no);
        ret = CDE_ERROR;
        break;
      }
      *base_oid = CdeGetVarcharOid(mysql_field->get_length_bytes());
      *expand_oid = (*base_oid) | (charset_no << OID_COLL_SHIFT);
      break;
    case MYSQL_TYPE_DATE:
      *base_oid = DATEOID;
      *expand_oid = DATEOID;
      break;
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
      *base_oid = TIMESTAMPOID;
      *expand_oid = TIMESTAMPOID;
      break;
    case MYSQL_TYPE_TIMESTAMP:
      *base_oid = TIMESTAMPTZOID;
      *expand_oid = TIMESTAMPTZOID;
      break;
    case MYSQL_TYPE_BIT:
      *base_oid = CDE_BIT_OID;
      *expand_oid = CdeGetBitOid(mysql_field->pack_length());
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      *base_oid =
          (mysql_field->charset() == &my_charset_bin) ? BLOBOID : CLOBOID;
      *expand_oid = *base_oid;
      break;
    case MYSQL_TYPE_JSON:
      *base_oid = BLOBOID;
      *expand_oid = BLOBOID;
      break;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_TYPED_ARRAY:
    default:
      *base_oid = CDE_INVALIDOID;
      *expand_oid = *base_oid;
      CDE_LOG_ERROR("not support data type[%u] from server", mysql_type);
      my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "special data types");
      ret = HA_ERR_UNSUPPORTED;
      break;
  }
  return ret;
}

int DDTypeToDstore(const dd::Column *ddCol, Oid *baseOid, Oid *expandOid,
                   bool isCompact, bool *isUnsigned __attribute__((unused))) {
  int ret = CDE_OK;
  enum_field_types mysqlType, mysqlRealType;
  DDTypeToMysql(ddCol->type(), mysqlType, mysqlRealType);
  uint32_t charsetNo;
  CHARSET_INFO *columnCharset;
  std::string errorMessage;

  if (mysqlRealType == MYSQL_TYPE_ENUM) {
    return HandleDDEnumType(ddCol, baseOid, expandOid, isUnsigned);
  }
  if (mysqlRealType == MYSQL_TYPE_SET) {
    return HandleDDSetType(ddCol, baseOid, expandOid, isUnsigned);
  }

  switch (mysqlType) {
    case MYSQL_TYPE_TINY:
      *baseOid = INT1OID;
      *expandOid = INT1OID;
      *isUnsigned = CheckDDIntUnsigned(ddCol);
      break;
    case MYSQL_TYPE_SHORT:
      *baseOid = INT2OID;
      *expandOid = INT2OID;
      *isUnsigned = CheckDDIntUnsigned(ddCol);
      break;
    case MYSQL_TYPE_LONG:
      *baseOid = INT4OID;
      *expandOid = INT4OID;
      *isUnsigned = CheckDDIntUnsigned(ddCol);
      break;
    case MYSQL_TYPE_LONGLONG:
      *baseOid = INT8OID;
      *expandOid = INT8OID;
      *isUnsigned = CheckDDIntUnsigned(ddCol);
      break;
    case MYSQL_TYPE_INT24:
      *baseOid = INT3OID;
      *expandOid = INT3OID;
      *isUnsigned = CheckDDIntUnsigned(ddCol);
      break;
    case MYSQL_TYPE_FLOAT:
      *baseOid = FLOAT4OID;
      *expandOid = FLOAT4OID;
      break;
    case MYSQL_TYPE_DOUBLE:
      *baseOid = FLOAT8OID;
      *expandOid = FLOAT8OID;
      break;
    case MYSQL_TYPE_STRING:
      if (mysqlRealType == MYSQL_TYPE_STRING) {
        charsetNo = GetColumnCharsetNo(ddCol);
        if (charsetNo > MAX_COLLATION_NUM) {
          CDE_LOG_ERROR("invalid collation num[%u] from server.", charsetNo);
          ret = CDE_ERROR;
          break;
        }
        if (isCompact) {
          *baseOid = CDE_COMPACT_CHAR_OID;
          *expandOid = CDE_COMPACT_CHAR_OID | (charsetNo << OID_COLL_SHIFT);
        } else {
          *baseOid = CDE_CHAR_OID;
          *expandOid = CdeGetCharOid(DDGetColumnPackLength(ddCol)) |
                       (charsetNo << OID_COLL_SHIFT);
        }
        break;
      }
      [[fallthrough]];
    case MYSQL_TYPE_NEWDECIMAL:
      *baseOid = CDE_BIN_OID;
      *expandOid = CdeGetBinOid(DDGetColumnPackLength(ddCol));
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_BOOL:
      *baseOid = INT1OID;
      *expandOid = INT1OID;
      *isUnsigned = true;
      break;
    case MYSQL_TYPE_VARCHAR:
      charsetNo = GetColumnCharsetNo(ddCol);
      if (charsetNo > MAX_COLLATION_NUM) {
        CDE_LOG_ERROR("invalid collation num[%u] from server.", charsetNo);
        ret = CDE_ERROR;
        break;
      }
      *baseOid = CdeGetVarcharOid(GetVarcharColumnLengthBytes(ddCol));
      *expandOid = (*baseOid) | (charsetNo << OID_COLL_SHIFT);
      break;
    case MYSQL_TYPE_DATE:
      *baseOid = DATEOID;
      *expandOid = DATEOID;
      break;
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
      *baseOid = TIMESTAMPOID;
      *expandOid = TIMESTAMPOID;
      break;
    case MYSQL_TYPE_TIMESTAMP:
      *baseOid = TIMESTAMPTZOID;
      *expandOid = TIMESTAMPTZOID;
      break;
    case MYSQL_TYPE_BIT:
      *baseOid = CDE_BIT_OID;
      *expandOid = CdeGetBitOid(DDGetColumnPackLength(ddCol));
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      columnCharset = GetColumnCharset(ddCol);
      *baseOid = (columnCharset == &my_charset_bin) ? BLOBOID : CLOBOID;
      *expandOid = *baseOid;
      break;
    case MYSQL_TYPE_JSON:
      *baseOid = BLOBOID;
      *expandOid = BLOBOID;
      break;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_TYPED_ARRAY:
    default:
      *baseOid = CDE_INVALIDOID;
      *expandOid = *baseOid;
      CDE_LOG_ERROR("not support data type[%u] from server", mysqlType);
      my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "special data types");
      ret = HA_ERR_UNSUPPORTED;
      break;
  }
  return ret;
}

static void CdeVarlenaCmpctCharDstoreToMysql(unsigned char *mysql_ptr,
                                             const Datum &dstore_ptr,
                                             uint32_t max_len) {
  CdeVarlena *varlena =
      reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(dstore_ptr) &
                                     static_cast<uintptr_t>(DEREF_PTR_MASK));
  uint16_t char_len = (varlena->len < varlena->prefix_n_chars)
                          ? varlena->prefix_n_chars
                          : varlena->len;
  memcpy_s(mysql_ptr, char_len, varlena->data, char_len);
  CDE_ASSERT(max_len >= char_len);
  uint32_t remain_len = max_len - char_len;
  if (remain_len > 0)
    memset_s(mysql_ptr + char_len, remain_len, 0x20, remain_len);
}

static void CdeCmpctCharDstoreToMysql(unsigned char *mysql_ptr,
                                      const Datum &dstore_ptr,
                                      uint32_t max_len) {
  // mysql has already fully padded mysql_ptr with 0x20
  uint16_t head_len = CdeGetCmpctCharHeadLen((char *)dstore_ptr);
  uint16_t char_len = CdeGetCmpctCharLen((char *)dstore_ptr);
  memcpy_s(mysql_ptr, char_len, (char *)dstore_ptr + head_len, char_len);
  CDE_ASSERT(max_len >= char_len);
  uint32_t remain_len = max_len - char_len;
  if (remain_len > 0) {
    // see: row_sel_field_store_in_mysql_format_func
    memset_s(mysql_ptr + char_len, remain_len, 0x20, remain_len);
  }
}

static void CdeVarlenaVarcharDstoreToMysql(unsigned char *mysql_ptr,
                                           uint8_t length_bytes,
                                           const Datum &dstore_ptr) {
  CdeVarlena *varlena =
      reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(dstore_ptr) &
                                     static_cast<uintptr_t>(DEREF_PTR_MASK));
  CDE_ASSERT((length_bytes == MYSQL_STR_HEAD_LENTH1) ||
             (length_bytes == MYSQL_STR_HEAD_LENTH2));
  if (length_bytes == MYSQL_STR_HEAD_LENTH1) {
    CDE_ASSERT(varlena->len <= MAX_U8);
    uint8_t len = varlena->len;
    memcpy_s(mysql_ptr, length_bytes, (uint8_t *)&len, length_bytes);
  } else if (length_bytes == MYSQL_STR_HEAD_LENTH2) {
    CDE_ASSERT(varlena->len <= MAX_U16);
    CdeWriteTo2LittleEndian((uint8_t *)mysql_ptr, varlena->len);
  }
  memcpy_s(mysql_ptr + length_bytes, varlena->len, varlena->data, varlena->len);
}

static void CdeVarcharDstoreToMysql(unsigned char *mysql_ptr,
                                    uint8_t length_bytes,
                                    const Datum &dstore_ptr) {
  uint16_t data_len = 0;
  CDE_ASSERT((length_bytes == MYSQL_STR_HEAD_LENTH1) ||
             (length_bytes == MYSQL_STR_HEAD_LENTH2));
  if (length_bytes == MYSQL_STR_HEAD_LENTH1) {
    data_len = static_cast<uint16_t>(*(uint8_t *)(dstore_ptr));
  } else if (length_bytes == MYSQL_STR_HEAD_LENTH2) {
    data_len = CdeReadFrom2LittleEndian((uint8_t *)dstore_ptr);
  }
  memcpy_s(mysql_ptr, data_len + length_bytes, (void *)dstore_ptr,
           data_len + length_bytes);
}

/**
Convert Data Format (Dstore to MySQL)
Dstore Format:
-----------------------------------------
| total length |  tag  |  tag  |  data  |
-----------------------------------------
|    30 bit    | 1 bit | 1 bit |        |
-----------------------------------------
data size(Bytes) = total length(Bytes) - 4(Bytes)
MySQL Format:
------------------------------------------------
| data length bytes  | data pointer            |
------------------------------------------------
| TINYBLOB - 1 btye  |                         |
| BLOB -2 btye       |                         |
| MEDIUMBLOB -3 btye | portable_sizeof_char_ptr|
| LONGBLOB -4 btye   |                         |
------------------------------------------------
lengthBytes = mysqlBufferSize - portable_sizeof_char_ptr

@param[in,out]  mysqlPtr  the buffer to store BLOB data in MySQL format.
@param[in]  mysqlBufferSize  determines into how many bytes the blob
length is stored, the space for the length may vary from 1 to 4 bytes + 8
bytes(portable_sizeof_char_ptr)
@param[in]  dstorePtr the BLOB data returned from dstore.
@param[in]  blobMemRoot  MEM_ROOT to use for memory allocation.

@return void.
*/
static void DstoreLobToMySQL(unsigned char *mysqlPtr, uint32_t mysqlBufferSize,
                             const Datum &dstorePtr, MEM_ROOT *blobMemRoot) {
  errno_t err = 0;
  uint32_t totalLen = *(uint32_t *)(dstorePtr);
  uint32_t blobDataLen =
      ((totalLen >> 2) & 0x3FFFFFFF) - DSTORE_LOB_HEADER_BYTES;
  uint32_t lengthBytes = mysqlBufferSize - portable_sizeof_char_ptr;
  std::vector<uint32_t> lengthBytesMap = {0xFF, 0xFFFF, 0xFFFFFF,
                                          DSTORE_LOB_MAX_SIZE};

  /* copy data length */
  if (blobDataLen > lengthBytesMap[lengthBytes - 1]) {
    CDE_LOG_FATAL(
        "data length is invalid, data length is 0x%X, length bytes is %d.",
        blobDataLen, lengthBytes);
    return;
  }

  CdeWriteToNLittleEndian(mysqlPtr, lengthBytes, blobDataLen);

  if (blobMemRoot == nullptr) {
    CDE_LOG_FATAL("blobMemRoot is nullptr");
    return;
  }
  /* copy data, clear memory in ha_cde::reset.*/
  char *blobData = static_cast<char *>(memdup_root(
      blobMemRoot, (unsigned char *)dstorePtr + DSTORE_LOB_HEADER_BYTES,
      blobDataLen));
  if (blobData == nullptr) {
    CDE_LOG_FATAL("alloc blob data fail");
    return;
  }

  /* copy data pointer */
  err = memcpy_s(mysqlPtr + lengthBytes, sizeof(void *), &blobData,
                 sizeof(void *));
  if (unlikely(err != 0)) {
    CDE_LOG_FATAL("memcpy_s data pointer fail, ret: %d", err);
    return;
  }
  return;
}

/**
Explanation of value-passing copies:
In the dstore project, when storing numerical fields in the storage table, the
transmission of Datum type data is relied upon. The definition of Datum itself
is: using Datum = uintptr_t; usually, it is typedef unsigned long int uintptr_t;
The length of this type is designed to accommodate all numerical types, so it is
estimated to be larger than int32 and int16. When writing data, dstore
internally, after obtaining the Datum data passed by the mysql handler, calls
StoreAttByVal to save the Datum data into HeapDiskTuple. For numerical types,
truncation of the high bits occurs during the conversion process, retaining only
the length of the numerical type (e.g., sizeof(int32), retaining only the lower
32 bits of the Datum data), refer to the implementation of StoreAttByVal for
details. Therefore, when the handler layer writes data to dstore and converts
it, to avoid byte alignment warnings (the original code contains dstore_ptr =
*(const int32_t *)mysql_ptr; which generates byte alignment warnings), mysql_ptr
can be assembled byte by byte and assigned to Datum type data, only assembling
the field type length, without handling the high bits, as dstore will also
truncate the high bits of Datum. The implementation of byte-by-byte assembly can
be replaced with memcpy, with equivalent effects and slightly better
performance.
-- It should be particularly noted that currently, the storage in the dstore
project is tightly packed storage, consistent with InnoDB, which is why there
are uBSan byte alignment warnings. Compared to PG, PG performs strong byte
alignment on data addresses when storing data. If a field is not naturally
aligned, additional padding space is added to ensure field address alignment.
Therefore, PG's forced type conversion, such as Int32GetDatum(*((int32 *)(T))),
has no byte alignment risk.

* Store MySQL field data in Dstore format
*
* @param mysql_ptr Pointer to MySQL field data
* @param type_oid OID of the field data type
* @param mysql_field MySQL field information
* @param use_index_format Whether to use index format
* @param dstore_ptr Pointer to Dstore format data
* @param varlena Variable length data
* @param isAllocBlobMem Whether to allocate Blob memory
* @return Return code, CDE_OK indicates success, other values indicate failure
*/
__attribute__((hot)) int StoreMysqlFieldToDstoreFormat(
    const unsigned char *mysql_ptr, Oid type_oid, const Field *mysql_field,
    bool use_index_format, Datum &dstore_ptr, CdeVarlena &varlena,
    bool *isAllocBlobMem) {
  uint32_t length, mbmaxlen, mbminlen;
  Oid base_type_oid = CdeConvertDatatypeOid(type_oid);
  int ret = CDE_OK;
  errno_t err = 0;
  DBUG_EXECUTE_IF("field_mysql_to_dstore_injection",
                  base_type_oid = CDE_DATATYPE_OID_MAX;);

  switch (base_type_oid) {
    case CDE_COMPACT_CHAR_OID:
      length = mysql_field->pack_length();
      mbmaxlen = mysql_field->charset()->mbmaxlen;
      mbminlen = mysql_field->charset()->mbminlen;
      CDE_ASSERT((length % mbmaxlen) == 0);
      dstore_ptr = CdeSetCmpctCharDatum(mysql_ptr, length, length, mbmaxlen,
                                        mbminlen, &varlena);
      break;
    case INT2OID:
      err =
          memcpy_s(&dstore_ptr, sizeof(uint16_t), mysql_ptr, sizeof(uint16_t));
      break;
    case INT4OID:
    case FLOAT4OID:
      err =
          memcpy_s(&dstore_ptr, sizeof(uint32_t), mysql_ptr, sizeof(uint32_t));
      break;
    case INT3OID:
      length = mysql_field->pack_length();
      err = memcpy_s(&dstore_ptr, length, mysql_ptr, length);
      break;
    case INT1OID:
      err = memcpy_s(&dstore_ptr, sizeof(uint8_t), mysql_ptr, sizeof(uint8_t));
      break;
    case CDE_VARCHAR1B_OID:
    case CDE_VARCHAR2B_OID:
      dstore_ptr = CdeSetVarcharDatumFromMysql(
          mysql_ptr,
          use_index_format ? MYSQL_STR_HEAD_LENTH2
                           : (uint32_t)mysql_field->get_length_bytes(),
          &varlena);
      break;
    case CDE_BIN_OID:
    case CDE_BIT_OID:
      dstore_ptr = (Datum)mysql_ptr;
      break;
    case FLOAT8OID:
    case INT8OID:
      err =
          memcpy_s(&dstore_ptr, sizeof(uint64_t), mysql_ptr, sizeof(uint64_t));
      break;
    case DATEOID:
      err = ConvertMysqlDateBinaryToDstore(
          mysql_ptr, mysql_field->pack_length(), dstore_ptr);
      break;
    case TIMESTAMPOID:
    case TIMESTAMPTZOID:
      err = ConvertMysqlTimestampBinaryToDstore(
          mysql_ptr, mysql_field->pack_length(), dstore_ptr);
      break;
    case CDE_CHAR_OID:
      length = mysql_field->pack_length();
      dstore_ptr =
          CdeSetRdntCharDatum(mysql_ptr, length, (uint32_t)0, &varlena);
      break;
    case BLOBOID:
    case CLOBOID:
      if (!use_index_format && isAllocBlobMem != nullptr) {
        int32_t errorMsg;
        dstore_ptr = MysqlLobDataToDstore(mysql_ptr, mysql_field, &varlena,
                                          &errorMsg, isAllocBlobMem);
        ret = errorMsg;
      }
      break;
    default:
      ret = HA_ERR_UNSUPPORTED;
      break;
  }
  CDE_ASSERT(err == 0);
  return ret;
}

/**
Explanation of value-passing copies:
When reading data from HeapDiskTuple, FetchAtt only retrieves the memory value
of the actual length of the field and assigns it to dstore_ptr (Datum type),
with the higher bits filled with 0. Please refer to the FetchAtt function for
details. Therefore, when the handler layer reads and converts data from dstore,
to avoid byte alignment warnings (similar to *(int32_t *)mysql_ptr = dstore_ptr;
in the original code, which may cause byte alignment warnings), the Datum type
dstore_ptr can be assigned byte by byte to mysql_ptr (the implementation of
byte-by-byte assembly can use memcpy as a replacement, with equivalent effect
and slightly better performance), and only the memory size of the field length
is processed. The memory exceeding the field size is not processed, as the part
exceeding dstore_ptr has already been initialized to 0.

* Copies a DStore column to a MySQL field.

@param[out] mysql_ptr        mysql field data
@param[in]  type_oid         cde data type oid
@param[in]  mysql_field      the mysql field object
@param[in]  dstore_ptr       dstore data
@param[in]  blobMemRoot      MEM_ROOT to use for memory allocation

@return void.
*/
__attribute__((hot)) static void DstoreColToMySQL(unsigned char *mysql_ptr,
                                                  Oid type_oid,
                                                  Field *mysql_field,
                                                  const Datum &dstore_ptr,
                                                  MEM_ROOT *blobMemRoot) {
  Oid base_type_oid = CdeConvertDatatypeOid(type_oid);
  bool convertFromVarlena =
      (((uint64_t)dstore_ptr & DEREF_TAG_MASK) == DEREF_TAG);
  errno_t err = 0;
  switch (base_type_oid) {
    case CDE_VARCHAR1B_OID:
    case CDE_VARCHAR2B_OID:
      if (convertFromVarlena)
        CdeVarlenaVarcharDstoreToMysql(
            mysql_ptr, mysql_field->get_length_bytes(), dstore_ptr);
      else
        CdeVarcharDstoreToMysql(mysql_ptr, mysql_field->get_length_bytes(),
                                dstore_ptr);
      break;
    case CDE_COMPACT_CHAR_OID:
      if (convertFromVarlena)
        CdeVarlenaCmpctCharDstoreToMysql(mysql_ptr, dstore_ptr,
                                         mysql_field->pack_length());
      else
        CdeCmpctCharDstoreToMysql(mysql_ptr, dstore_ptr,
                                  mysql_field->pack_length());
      break;
    case DATEOID:
      err = CdeConvertDstoreDateBinaryToMysql(
          mysql_ptr, mysql_field->pack_length(), dstore_ptr);
      break;
    case TIMESTAMPOID:
    case TIMESTAMPTZOID:
      err = CdeConvertDstoreTimestampBinaryToMysql(
          mysql_ptr, mysql_field->pack_length(), dstore_ptr);
      break;
    case CDE_BIN_OID:
    case CDE_CHAR_OID:
    case CDE_BIT_OID:
      err = memcpy_s(mysql_ptr, mysql_field->pack_length(), (void *)dstore_ptr,
                     mysql_field->pack_length());
      break;
    case INT4OID:
    case FLOAT4OID:
      err =
          memcpy_s(mysql_ptr, sizeof(uint32_t), &dstore_ptr, sizeof(uint32_t));
      break;
    case INT1OID:
      err = memcpy_s(mysql_ptr, sizeof(uint8_t), &dstore_ptr, sizeof(uint8_t));
      break;
    case INT2OID:
      err =
          memcpy_s(mysql_ptr, sizeof(uint16_t), &dstore_ptr, sizeof(uint16_t));
      break;
    case FLOAT8OID:
    case INT8OID:
      err =
          memcpy_s(mysql_ptr, sizeof(uint64_t), &dstore_ptr, sizeof(uint64_t));
      break;
    case INT3OID:
      CdeWriteToNLittleEndian(mysql_ptr, MYSQL_STORE_BYTE3,
                              CdeReadFrom3LittleEndian<DSTORE::Datum *>(
                                  const_cast<DSTORE::Datum *>(&dstore_ptr)));
      break;
    case BLOBOID:
    case CLOBOID:
      DstoreLobToMySQL(mysql_ptr, mysql_field->pack_length(), dstore_ptr,
                       blobMemRoot);
      break;
    default:
      CDE_ASSERT(0);
      break;
  }
  CDE_ASSERT(err == 0);
}

Datum CdeSetVarlenaWithPrefix(bool check_for_fk, Oid type_oid, Datum &src_datum,
                              const cde_dict_field_t *field_dict,
                              CdeVarlena *for_varlena, bool parseFromDstore) {
  uint32_t prefix_len = check_for_fk ? field_dict->len : field_dict->prefix_len;
  if (0 == prefix_len) return src_datum;

  uint32_t charset_no = (type_oid >> 16) & MAX_COLLATION_NUM;
  type_oid = CdeRmCollFromOid(type_oid);
  CdeVarlena *varlena;
  CdeVarlena tupleVarLena;
  if (parseFromDstore) {
    uint16_t len, head_byte;
    switch (type_oid) {
      case CDE_VARCHAR1B_OID:
      case CDE_VARCHAR2B_OID:
        CdeParseVarchar(src_datum, type_oid, tupleVarLena.data, len, head_byte);
        break;
      case CDE_COMPACT_CHAR_OID:
        CdeParseCmpctChar(src_datum, tupleVarLena.data, len, head_byte);
        break;
      default:
        CDE_ASSERT(0);
    }
    tupleVarLena.len = len;
    varlena = &tupleVarLena;
  } else {
    CDE_ASSERT(((uint64_t)src_datum & DEREF_TAG_MASK) == DEREF_TAG);
    varlena =
        reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(src_datum) &
                                       static_cast<uintptr_t>(DEREF_PTR_MASK));
  }

  const unsigned char *mysql_ptr = varlena->data;
  uint32_t mysql_str_len = varlena->len;
  uint32_t str_bytes =
      GetAtMostNMbchars(charset_no, field_dict->mbminlen, field_dict->mbmaxlen,
                        prefix_len, mysql_str_len, mysql_ptr);
  if (check_for_fk && (str_bytes < mysql_str_len)) {
    return INVALID_DATUM;
  }

  switch (type_oid) {
    case CDE_COMPACT_CHAR_OID:
      return CdeSetCmpctCharDatum(mysql_ptr, str_bytes, prefix_len,
                                  field_dict->mbmaxlen, field_dict->mbminlen,
                                  for_varlena);
    case CDE_VARCHAR1B_OID:
    case CDE_VARCHAR2B_OID:
      return CdeSetVarcharDatum(mysql_ptr, str_bytes, for_varlena);
    default:
      if (CdeConvertDatatypeOid(type_oid) != CDE_CHAR_OID) {
        CDE_LOG_ERROR("invalid type oid: %u", type_oid);
        return INVALID_DATUM;
      }
      return CdeSetRdntCharDatum(mysql_ptr, str_bytes, prefix_len, for_varlena);
  }
  return INVALID_DATUM;
}

// only char index in redundent need to fix oid to prefix_len in index_read
int CdeSetPrefixOid(uint32_t actual_n_fields, session_index_info *idx_dict,
                    Oid *index_col_oids) {
  CDE_ASSERT(idx_dict != nullptr);
  CDE_ASSERT(idx_dict->shared_dict->fields != nullptr);
  for (uint32_t i = 0; i < actual_n_fields; i++) {
    cde_dict_field_t field = idx_dict->shared_dict->fields[i];
    Oid type_oid = idx_dict->rel->index->opcinType[i];
    Oid base_type_oid = CdeConvertDatatypeOid(type_oid);
    uint32_t prefix_len = field.prefix_len;
    if (prefix_len > 0 && base_type_oid == CDE_CHAR_OID) {
      // TODO: Currently, the CDE_CHAR_OID type is not effective. This code
      // branch is unreachable. Temporarily skipping the adaptation for passing
      // column attribute information to fnExtra to avoid unnecessary
      // development time. If this scenario is supported in the future, the
      // column information passing mechanism will need to be implemented.
      CDE_ASSERT(0);
      uint32_t charset_no = (type_oid >> 16) & MAX_COLLATION_NUM;
      index_col_oids[i] =
          CdeGetCharOid(prefix_len) | (charset_no << OID_COLL_SHIFT);
    }
  }
  return CDE_OK;
}

/** This function is used to find the storage length in bytes of the first n
 characters for prefix indexes using a multibyte character set. The function
 finds charset information and returns length of prefix_len characters in the
 index field in bytes.
 @return number of bytes occupied by the first n characters */
// see innobase_get_at_most_n_mbchars
uint32_t CdeGetAtMostNMbchars(
    uint32_t charset_no, /*!< in: character set id */
    uint32_t prefix_len, /*!< in: prefix length in bytes of the index
                      (this has to be divided by mbmaxlen to get the
                      number of CHARACTERS n in the prefix) */
    uint32_t data_len,   /*!< in: length of the string in bytes */
    const char *str)     /*!< in: character string */
{
  uint32_t char_length;  /*!< character length in bytes */
  uint32_t n_chars;      /*!< number of characters in prefix */
  CHARSET_INFO *charset; /*!< charset used in the field */

  charset = get_charset(charset_no, MYF(MY_WME));

  CDE_ASSERT(charset != nullptr);
  CDE_ASSERT(charset->mbmaxlen);
  n_chars = prefix_len / charset->mbmaxlen;
  if (charset->mbmaxlen > 1) {
    char_length = my_charpos(charset, str, str + data_len, (int)n_chars);
    if (char_length > data_len) {
      char_length = data_len;
    }
  } else if (data_len < prefix_len) {
    char_length = data_len;

  } else {
    char_length = prefix_len;
  }

  return (char_length);
}

/** Determine how many bytes the first n characters of the given string occupy.
 If the string is shorter than n characters, returns the number of bytes
 the characters in the string occupy.
 @return length of the prefix, in bytes */
uint32_t GetAtMostNMbchars(
    uint32_t charset_no,      /*!< in: precise type */
    uint32_t mbminlen,        /*!< in: minimum and maximum length of
                              a multi-byte character */
    uint32_t mbmaxlen,        /*!< in: minimum and maximum length of
                               a multi-byte character */
    uint32_t prefix_len,      /*!< in: length of the requested
                           prefix, in characters, multiplied by
                           dtype_get_mbmaxlen(dtype) */
    uint32_t data_len,        /*!< in: length of str (in bytes) */
    const unsigned char *str) /*!< in: the string whose prefix
                     length is being determined */
{
  //   ut_a(rec_field_not_null_not_add_col_def(data_len));
  CDE_ASSERT(!mbmaxlen || !(prefix_len % mbmaxlen));

  if (mbminlen != mbmaxlen) {
    CDE_ASSERT(!(prefix_len % mbmaxlen));
    return (CdeGetAtMostNMbchars(charset_no, prefix_len, data_len,
                                 reinterpret_cast<const char *>(str)));
  }

  if (prefix_len < data_len) {
    return (prefix_len);
  }

  return (data_len);
}

bool CdeDatumHeapToIndex(Datum *values, bool *isNulls, dml_ctx *ctx) {
  session_index_info *index_info = ctx->get_index();
  cde_dict_index_t *idx_dict = index_info->shared_dict;
  Datum *index_values = ctx->index_tuple()->index_values;
  bool *index_is_nulls = ctx->index_tuple()->index_is_nulls;
  CdeVarlena *index_varlenas = ctx->index_tuple()->index_varlenas;

  cde_dict_field_t *field_dict;

  for (uint32_t i = 0; i < idx_dict->index_col_num; i++) {
    field_dict = &(idx_dict->fields[i]);
    CDE_ASSERT(field_dict != nullptr);
    index_is_nulls[i] = isNulls[field_dict->attId];
    if (index_is_nulls[i]) {
      continue;
    }

    if (((uint64_t)values[field_dict->attId] & DEREF_TAG_MASK) == 0) {
      // plain type
      if (field_dict->prefix_len == 0) {
        index_values[i] = values[field_dict->attId];
        continue;
      } else {
        index_values[i] =
            CdeSetVarlenaWithPrefix(false, index_info->rel->index->opcinType[i],
                                    values[field_dict->attId], field_dict,
                                    &(index_varlenas[field_dict->attId]), true);
      }
    } else {
      index_values[i] =
          CdeSetVarlenaWithPrefix(false, index_info->rel->index->opcinType[i],
                                  values[field_dict->attId], field_dict,
                                  &(index_varlenas[field_dict->attId]));
    }

    if (index_values[i] == INVALID_DATUM) {
      return CDE_FAIL;
    }
  }
  return CDE_SUCC;
}

int MysqlRecordToDstoreForInsert(const TABLE *table,
                                 const unsigned char *mysql_ptr,
                                 dml_tuple_t *tuple, dml_ctx *ctx,
                                 DSTORE::TupleDesc attr, bool skipVirtual) {
  uint32_t n_fields = ctx->get_table()->GetTotalCols();
  Field *mysql_field;
  uint32_t offset;
  int ret = CDE_OK;
  Datum *values = tuple->values;
  CdeVarlena *varlenas = tuple->varlenas;
  bool *is_nulls = tuple->is_nulls;
  bool *isBlob = tuple->is_blob;
  Oid type_oid;
  uint32_t field_nr = 0;
  auto &cols = ctx->get_table()->m_dictCols;

  uint32_t j = 0;  // index for non-virtual fields
                   // i is for indexing all columns

  for (uint32_t i = 0; i < n_fields; i++) {
    if (skipVirtual && cols[i].m_isVirtual) {
      ++field_nr;
      continue;
    }

    j = cols[i].m_phyPos;
    if (!cols[i].m_isVisible) {
      /* For instant dropped column, we just set it as null. */
      is_nulls[j] = true;
      continue;
    }

    mysql_field = table->field[field_nr++];
    is_nulls[j] = false;
    if (mysql_field->is_nullable()) {
      uint32_t mysql_null_byte_offset =
          mysql_field->null_offset(table->record[0]);
      if (mysql_ptr[mysql_null_byte_offset] & mysql_field->null_bit) {
        is_nulls[j] = true;
        continue;
      }
    }
    offset = static_cast<uint32_t>(mysql_field->offset(table->record[0]));
    type_oid = attr->attrs[j]->atttypid;
    ret = StoreMysqlFieldToDstoreFormat(mysql_ptr + offset, type_oid,
                                        mysql_field, false, values[j],
                                        varlenas[j], &isBlob[j]);
    if (ret != CDE_OK) {
      return ret;
    }
  }
  return ret;
}

int MysqlRecordToDstoreForUpd(const TABLE *table,
                              const unsigned char *mysql_ptr_old,
                              const unsigned char *mysql_ptr_new,
                              dml_upd_ctx *upd_ctx, dml_tuple_t *tuple,
                              dml_tuple_t *tupleOld, TupleDesc attr,
                              bool skipVirtual) {
  cde_dict_t *table_handler = upd_ctx->get_table();
  uint32_t n_fields = table_handler->GetTotalCols();
  Field *mysql_field;
  uint32_t offset;
  int ret = CDE_OK;
  Oid type_oid;

  Datum *values_old = tupleOld->values;
  Datum *values_new = tuple->values;
  CdeVarlena *varlenas_old = tupleOld->varlenas;
  CdeVarlena *varlenas_new = tuple->varlenas;
  bool *is_nulls_old = tupleOld->is_nulls;
  bool *is_nulls_new = tuple->is_nulls;
  bool *isBlobOld = tupleOld->is_blob;
  bool *isBlobNew = tuple->is_blob;
  bool *is_changed = upd_ctx->get_is_changed_vec();
  bool is_any_changed = false;
  uint32_t field_nr = 0;
  auto &cols = upd_ctx->get_table()->m_dictCols;
  uint32_t j = 0;

  CDE_ASSERT_DEBUG(nullptr != table_handler->attrFull);

  for (uint32_t i = 0; i < n_fields; i++) {
    if (skipVirtual && cols[i].m_isVirtual) {
      ++field_nr;
      continue;
    }

    j = cols[i].m_phyPos;
    if (!cols[i].m_isVisible) {
      /* For instant dropped column, we just set it as null. */
      is_nulls_old[j] = true;
      is_nulls_new[j] = true;
      continue;
    }

    mysql_field = table->field[field_nr++];
    is_nulls_old[j] = false;
    is_nulls_new[j] = false;
    if (mysql_field->is_nullable()) {
      uint32_t mysql_null_byte_offset =
          mysql_field->null_offset(table->record[0]);
      is_nulls_old[j] =
          mysql_ptr_old[mysql_null_byte_offset] & mysql_field->null_bit;
      is_nulls_new[j] =
          mysql_ptr_new[mysql_null_byte_offset] & mysql_field->null_bit;
    }

    offset = static_cast<uint32_t>(
        mysql_field->offset(table->record[0]));  // todo: 加入模板
    type_oid = attr->attrs[j]->atttypid;
    if (!is_nulls_old[j]) {
      ret = StoreMysqlFieldToDstoreFormat(mysql_ptr_old + offset, type_oid,
                                          mysql_field, false, values_old[j],
                                          varlenas_old[j], &isBlobOld[j]);
      if (ret != CDE_OK) {
        return ret;
      }
    }

    if (is_nulls_new[j]) {
      is_changed[j] = !(is_nulls_old[j]);
      is_any_changed |= is_changed[j];
      continue;
    }

    ret = StoreMysqlFieldToDstoreFormat(mysql_ptr_new + offset, type_oid,
                                        mysql_field, false, values_new[j],
                                        varlenas_new[j], &isBlobNew[j]);
    if (ret != CDE_OK) {
      return ret;
    }

    if (is_nulls_old[j]) {
      is_changed[j] = true;
      is_any_changed = true;
      continue;
    }

    /* can i compare mysql raw data directly here? */
    // todo: for immediate number data, no need to use memcmp
    // todo: confirm varchar is well defined
    is_changed[j] = (0 != memcmp(mysql_ptr_old + offset, mysql_ptr_new + offset,
                                 mysql_field->pack_length()));
    is_any_changed |= is_changed[j];
  }
  return is_any_changed ? CDE_OK : HA_ERR_RECORD_IS_THE_SAME;
}

/**
 * In the leftmost matching principle, mysql_key_ptr may only use some index
 * keys on the left, not all the key
 */
int key_mysql_to_dstore(const TABLE *table, session_index_info *idx_dict,
                        uint32_t index_col_num,
                        const unsigned char *mysql_key_ptr,
                        uint32_t mysql_key_len, Datum *index_values,
                        bool *index_is_nulls, TupleDesc attr) {
  Field *mysql_field;
  uint32_t col_len;
  uint32_t offset = 0;
  int ret = CDE_OK;
  CdeVarlena *varlenas = (CdeVarlena *)&index_values[index_col_num];
  Oid type_oid;
  cde_dict_field_t *field_dict;
  uint32_t *index_cols = idx_dict->shared_dict->index_cols;
  uint32_t *attr_cols = idx_dict->shared_dict->attr_cols;

  const unsigned char *original_key_ptr = mysql_key_ptr;
  const unsigned char *key_end;

  key_end = mysql_key_ptr + mysql_key_len;

  uint32_t i = 0;
  for (; (i < index_col_num) && (original_key_ptr < key_end); i++) {
    mysql_field = table->field[index_cols[i]];
    index_is_nulls[i] = false;
    if (mysql_field->is_nullable()) {
      CDE_ASSERT(offset < mysql_key_len);
      if (*(mysql_key_ptr + offset) != 0) {
        index_is_nulls[i] = true;
      }
      offset++;
    }

    field_dict = idx_dict->shared_dict->fields + i;
    uint32_t prefix_len = field_dict->prefix_len;
    if (prefix_len > 0) {
      col_len = prefix_len;
    } else {
      col_len = mysql_field->key_length();
    }

    type_oid = attr->attrs[attr_cols[i]]->atttypid;
    Oid base_type_oid = CdeConvertDatatypeOid(type_oid);

    if (mysql_field->type() == MYSQL_TYPE_VARCHAR) {
      col_len += MYSQL_STR_HEAD_LENTH2;
    }
    CDE_ASSERT(offset < mysql_key_len);
    /*
        For char type, whether in redundant or compact format, the storage
       format at the server layer is 'data + space'. The total length is the
       number of characters multiplied by the maximum number of characters. The
       length information is not saved.

        In compact format prefix index, the CDE layer obtains and saves the
       prefix length by trimming the space at the end. In redundant format
       prefix index, save data directly. For details, see
       StoreMysqlFieldToDstoreFormat.
    */
    if (prefix_len > 0 && base_type_oid == CDE_COMPACT_CHAR_OID) {
      CDE_ASSERT(field_dict->mbmaxlen > 0 &&
                 (prefix_len % field_dict->mbmaxlen) == 0);
      index_values[i] = CdeSetCmpctCharDatum(
          mysql_key_ptr + offset, prefix_len, prefix_len, field_dict->mbmaxlen,
          field_dict->mbminlen, &varlenas[i]);
    } else {
      ret = StoreMysqlFieldToDstoreFormat(mysql_key_ptr + offset, type_oid,
                                          mysql_field, true, index_values[i],
                                          varlenas[i], nullptr);
    }
    if (ret != CDE_OK) {
      return ret;
    }
    offset += col_len;
    original_key_ptr = mysql_key_ptr + offset;
  }

  // Set any columns we did not set the value for to NULL.
  for (; i < index_col_num; ++i) {
    index_is_nulls[i] = true;
  }

  return ret;
}

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
                           Datum *values, bool *is_nulls, MEM_ROOT *blobMemRoot,
                           bool allowBlobMemRootClearForReuse,
                           const MysqlAndDstoreIndex *columnsToDecode,
                           size_t numColumnsToDecode,
                           const TupleDesc heapTupleDesc) {
  Field *mysqlField;
  uint32_t offset;
  Oid typeOid;

  if (heapTupleDesc->tdhaslob > 0 && blobMemRoot &&
      allowBlobMemRootClearForReuse) {
    /* Clear BLOB data from the previous row */
    blobMemRoot->ClearForReuse();
  }
  for (size_t i = 0; i < numColumnsToDecode; ++i) {
    mysqlField = table->field[columnsToDecode[i].m_mysqlIndex];
    offset = static_cast<uint32_t>(mysqlField->offset(table->record[0]));
    if (is_nulls[columnsToDecode[i].m_dstoreIndex]) {
      uint32_t mysql_null_byte_offset =
          mysqlField->null_offset(table->record[0]);
      mysql_ptr[mysql_null_byte_offset] |= mysqlField->null_bit;
      /* When converting a record to MySQL format, copy the default column
      values for columns that are SQL NULL. This addresses failures in
      row-based replication (Bug #39648).
      Refer to InnoDB's row_sel_store_mysql_field. */
      memcpy_s(mysql_ptr + offset, mysqlField->pack_length(),
               table->s->default_values + offset, mysqlField->pack_length());
      continue;
    }

    typeOid = heapTupleDesc->attrs[columnsToDecode[i].m_dstoreIndex]->atttypid;
    DstoreColToMySQL(mysql_ptr + offset, typeOid, mysqlField,
                     values[columnsToDecode[i].m_dstoreIndex], blobMemRoot);
    if (mysqlField->null_bit) {
      uint32_t mysql_null_byte_offset =
          mysqlField->null_offset(table->record[0]);
      mysql_ptr[mysql_null_byte_offset] &= ~(unsigned char)mysqlField->null_bit;
    }
  }
}

/**
Convert a row in the DStore format to a row in the MySQL format.
Note this is used for covering index read, no need to read heap table.

@param[in]  table         the TABLE struct containing metadata
@param[in]  table_handler cde dict table struct
@param[out] mysql_ptr     mysql fields data
@param[in]  values        dstore data
@param[in]  is_nulls      columns is null or not
@param[in]  index_cols    field number in heap table of index fields
@param[in]  index_col_num index fields num

@return void.
*/
void DstoreIndexDataToMysql(const TABLE *table, unsigned char *mysql_ptr,
                            Datum *values, bool *is_nulls,
                            const uint32_t *index_cols,
                            const uint32_t *attr_cols, uint32_t index_col_num,
                            DSTORE::TupleDesc attr, MEM_ROOT *blobMemRoot,
                            bool onlyVirtualsFromReadSet) {
  Field *mysql_field;
  uint32_t offset;
  Oid type_oid;

  for (uint32_t i = 0; i < index_col_num; i++) {
    mysql_field = table->field[index_cols[i]];
    if (onlyVirtualsFromReadSet &&
        (false == mysql_field->is_virtual_gcol() ||
         false == bitmap_is_set(table->read_set, index_cols[i])))
      continue;
    if (is_nulls[i]) {
      uint32_t mysql_null_byte_offset =
          mysql_field->null_offset(table->record[0]);
      mysql_ptr[mysql_null_byte_offset] |= mysql_field->null_bit;
      continue;
    }
    offset = static_cast<uint32_t>(mysql_field->offset(table->record[0]));
    type_oid = attr->attrs[attr_cols[i]]->atttypid;
    DstoreColToMySQL(mysql_ptr + offset, type_oid, mysql_field, values[i],
                     blobMemRoot);

    if (mysql_field->null_bit) {
      uint32_t mysql_null_byte_offset =
          mysql_field->null_offset(table->record[0]);
      mysql_ptr[mysql_null_byte_offset] &=
          ~(unsigned char)mysql_field->null_bit;
    }
  }
}

/**
Convert the lock error of dstore to mysql error.

@param[in]  failureInfo    dstore lock error context
@param[in]  isoLevel       the transaction isolation level

@return CDE_OK if success, else other.
*/
int CdeConvertDstoreLockErrToMysql(DSTORE::FailureInfo &failureInfo,
                                   cde_isolation_level_t isoLevel) {
  int err = CDE_OK;
  switch (failureInfo.reason) {
    case DSTORE::HeapHandlerFailureReason::SELF_CREATED:
    case DSTORE::HeapHandlerFailureReason::SELF_MODIFIED:
    case DSTORE::HeapHandlerFailureReason::INVISIBLE_TO_SNAPSHOT: {
      /* For SELF_CREATED, SELF_MODIFIED and INVISIBLE_TO_SNAPSHOT, we treat it
      as deleted. SELF_CREATED means tuple was created in the same command of
      the current transaction, SELF_MODIFIED means tuple was modified in the
      same command of the current transaction, and INVISIBLE_TO_SNAPSHOT may be
      occurs that In a procedure that contains cursors, the cid of the affected
      tuple may be larger than the command of the current snapshot.
      To our best knowledge, we have not found any scenarios where these errors
      occur. */
      if (isoLevel == SERIALIZABLE) {
        err = static_cast<int>(
            HaDstoreErrE::HA_DSTORE_ERR_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY);
        break;
      }
      err = HA_ERR_RECORD_DELETED;
      CDE_LOG_DEBUG(
          "Tuple has created or modified by current transaction, ctid({%hu, "
          "%u}, %hu), reason=%u",
          failureInfo.ctid.GetFileId(), failureInfo.ctid.GetBlockNum(),
          failureInfo.ctid.GetOffset(),
          static_cast<uint8_t>(failureInfo.reason));
      break;
    }
    case DSTORE::HeapHandlerFailureReason::DELETED: {
      if (isoLevel == SERIALIZABLE) {
        err = static_cast<int>(
            HaDstoreErrE::HA_DSTORE_ERR_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY);
        break;
      }
      err = HA_ERR_RECORD_DELETED;
      CDE_LOG_DEBUG("Tuple is deleted, ctid({%hu, %u}, %hu)",
                    failureInfo.ctid.GetFileId(),
                    failureInfo.ctid.GetBlockNum(),
                    failureInfo.ctid.GetOffset());
      break;
    }
    case DSTORE::HeapHandlerFailureReason::UPDATED: {
      if (isoLevel == SERIALIZABLE) {
        err = static_cast<int>(
            HaDstoreErrE::HA_DSTORE_ERR_TRX_UNABLE_TO_ACCESS_CONTINUOUSLY);
        break;
      }
      err = HA_ERR_RECORD_CHANGED;
      break;
    }
    case DSTORE::HeapHandlerFailureReason::ALLOC_TD_FAILED: {
      err = HA_ERR_LOCK_WAIT_TIMEOUT;
      CDE_LOG_DEBUG("Allocate td failed, ctid({%hu, %u}, %hu)",
                    failureInfo.ctid.GetFileId(),
                    failureInfo.ctid.GetBlockNum(),
                    failureInfo.ctid.GetOffset());
      break;
    }
    case DSTORE::HeapHandlerFailureReason::DEADLOCK: {
      err = HA_ERR_LOCK_DEADLOCK;
      CDE_LOG_DEBUG(
          "Deadlock found, ctid({%hu, %u}, %hu)", failureInfo.ctid.GetFileId(),
          failureInfo.ctid.GetBlockNum(), failureInfo.ctid.GetOffset());
      break;
    }
    case DSTORE::HeapHandlerFailureReason::LOCK_WAIT_TIMEOUT: {
      err = HA_ERR_LOCK_WAIT_TIMEOUT;
      CDE_LOG_DEBUG("Lock tuple wait timeout, ctid({%hu, %u}, %hu)",
                    failureInfo.ctid.GetFileId(),
                    failureInfo.ctid.GetBlockNum(),
                    failureInfo.ctid.GetOffset());
      break;
    }
    case DSTORE::HeapHandlerFailureReason::LOCK_WAIT_CANCELED: {
      err = HA_ERR_QUERY_INTERRUPTED;
      CDE_LOG_DEBUG("Lock tuple wait canceled, ctid({%hu, %u}, %hu)",
                    failureInfo.ctid.GetFileId(),
                    failureInfo.ctid.GetBlockNum(),
                    failureInfo.ctid.GetOffset());
      break;
    }
    case DSTORE::HeapHandlerFailureReason::ALLOC_TRANS_SLOT_FAILED:
    case DSTORE::HeapHandlerFailureReason::CHECK_TUPLE_CHANGED_FAILED:
    case DSTORE::HeapHandlerFailureReason::GET_NEWEST_CTID_FAILED:
    case DSTORE::HeapHandlerFailureReason::READ_BUFFER_FAILED:
    case DSTORE::HeapHandlerFailureReason::LOCK_TUP_FAILED: {
      err = HA_ERR_INTERNAL_ERROR;
      CDE_LOG_ERROR(
          "DML statements fail to be executed in Dstore, reason=%u, "
          "ctid=({%hu, %u}, %hu)",
          static_cast<uint8_t>(failureInfo.reason),
          failureInfo.ctid.GetFileId(), failureInfo.ctid.GetBlockNum(),
          failureInfo.ctid.GetOffset());
      break;
    }
    case DSTORE::HeapHandlerFailureReason::LOCK_SKIP_WAIT: {
      err = static_cast<int>(HaDstoreErrE::HA_DSTORE_ERR_SKIP_WAIT);
      break;
    }
    default: {
      err = HA_ERR_GENERIC;
      CDE_LOG_ERROR(
          "Unrecognized lock failure reason, reason=%u, ctid=({%hu, %u}, %hu)",
          static_cast<uint8_t>(failureInfo.reason),
          failureInfo.ctid.GetFileId(), failureInfo.ctid.GetBlockNum(),
          failureInfo.ctid.GetOffset());
    }
  }

  return err;
}

/**
The first step to locking one tuple. The tuple located by ctid maybe deleted
or updated. The detail info see CdeConvertDstoreLockErrToMysql.

@param[in]      scanSnapshot   snapshot used in current scan
@param[in]      relation       dstore table handler
@param[in/out]  ctid           id of the tuple to lock and updated to new value
                               when error code is HA_ERR_RECORD_CHANGED
@param[out]     tuple          the locked tuple if success
@param[in]      isoLevel       the transaction isolation level

@return error code
*/
static int CdeLockUnchangedTuple(DSTORE::Snapshot scanSnapshot,
                                 DSTORE::StorageRelation relation,
                                 ItemPointer ctid, DSTORE::HeapTuple **tuple,
                                 cde_isolation_level_t isoLevel,
                                 bool needWait) {
  DSTORE::HeapLockTupleContext lockTupContext;
  lockTupContext.needRetTup = true;
  CDE_ASSERT(ctid != nullptr);
  lockTupContext.ctid = *ctid;
  lockTupContext.snapshot = *scanSnapshot;
  lockTupContext.allowLockSelf = false;
  lockTupContext.needWait = needWait;
  int ret = HeapInterface::LockUnchangedTuple(relation, &lockTupContext);

  DBUG_EXECUTE_IF("cde_lcktuple_updated_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::UPDATED;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_updated_deadlk_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::UPDATED;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_allocid_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::ALLOC_TD_FAILED;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_deadlk_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::DEADLOCK;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_selfmod_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::SELF_MODIFIED;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_invis_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::INVISIBLE_TO_SNAPSHOT;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_otherfail_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::UNKNOWN;
  });

  if (ret == CDE_OK) {
    *tuple = lockTupContext.retTup;
    return CDE_OK;
  }

  ret = CdeConvertDstoreLockErrToMysql(lockTupContext.failureInfo, isoLevel);
  if (ret == HA_ERR_RECORD_CHANGED) {
    *ctid = lockTupContext.failureInfo.ctid;
  } else if (CdeEnableDamagePageManager()) {
    HaErrorCode errcode = GetAndConvertDstoreErrcodeToMysql();
    if (errcode == (uint32_t)HaDstoreErrE::HA_DSTORE_ERR_SWAT_HIT_DAMAGE_PAGE) {
      ret = (int)errcode;
    }
  }

  return ret;
}

/**
The second step to locking one tuple when CdeLockUnchangedTuple return
HA_ERR_RECORD_CHANGED.

@param[in]      scanSnapshot   snapshot used in current scan
@param[in]      relation       dstore table handler
@param[in]      ctid           id of the tuple to lock
@param[out]     tuple          the locked tuple if success
@param[in]      isoLevel       the transaction isolation level

@return error code
*/
static int CdeLockNewestTuple(DSTORE::Snapshot scanSnapshot,
                              DSTORE::StorageRelation relation,
                              ItemPointer ctid, DSTORE::HeapTuple **new_tuple,
                              cde_isolation_level_t isoLevel, bool needWait) {
  DSTORE::HeapLockTupleContext lockTupContext;
  lockTupContext.ctid = *ctid;
  lockTupContext.needRetTup = true;
  lockTupContext.snapshot = *scanSnapshot;
  lockTupContext.allowLockSelf = false;
  lockTupContext.needWait = needWait;
  int ret = HeapInterface::LockNewestTuple(relation, &lockTupContext);

  DBUG_EXECUTE_IF("cde_lcktuple_updated_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::DELETED;
  });
  DBUG_EXECUTE_IF("cde_lcktuple_updated_deadlk_injection", {
    ret = CDE_ERROR;
    lockTupContext.failureInfo.reason =
        DSTORE::HeapHandlerFailureReason::DEADLOCK;
  });

  if (ret == CDE_OK) {
    *new_tuple = lockTupContext.retTup;
    return CDE_OK;
  }

  *new_tuple = nullptr;
  ret = CdeConvertDstoreLockErrToMysql(lockTupContext.failureInfo, isoLevel);

  return ret;
}

int CdeLockTuple(cde_isolation_level_t isoLevel, DSTORE::Snapshot scanSnapshot,
                 DSTORE::StorageRelation relation, ItemPointer ctid,
                 DSTORE::HeapTuple **tuple, bool needWait) {
  ItemPointerData oldctid = *ctid;
  DEBUG_SYNC(current_thd, "before_lock_tuple");
  int result = CdeLockUnchangedTuple(scanSnapshot, relation, ctid, tuple,
                                     isoLevel, needWait);
  CDE_LOG_DEBUG(
      "CdeLockUnchangedTuple, oldctid({%hu, %u}, %hu), newctid({%hu, "
      "%u}, %hu), result=%d, isoLevel=%d",
      oldctid.GetFileId(), oldctid.GetBlockNum(), oldctid.GetOffset(),
      ctid->GetFileId(), ctid->GetBlockNum(), ctid->GetOffset(), result,
      isoLevel);

  if (result == CDE_OK) {
    return result;
  }

  DEBUG_SYNC(current_thd, "before_lock_tuple");
  if (result == HA_ERR_RECORD_CHANGED) {
    CDE_LOG_DEBUG(
        "Tuple has changed, need to lock newest tuple, oldctid({%hu, %u}, "
        "%hu), newctid({%hu, "
        "%u}, %hu), result=%d, isoLevel=%d",
        oldctid.GetFileId(), oldctid.GetBlockNum(), oldctid.GetOffset(),
        ctid->GetFileId(), ctid->GetBlockNum(), ctid->GetOffset(), result,
        isoLevel);
    result = CdeLockNewestTuple(scanSnapshot, relation, ctid, tuple, isoLevel,
                                needWait);

    if (result == CDE_OK) {
      return CDE_OK;
    }

    CDE_LOG_WARN(
        "lock newest tuple failed, oldctid({%hu, %u}, %hu), newctid({%hu, "
        "%u}, %hu), error=%d",
        oldctid.GetFileId(), oldctid.GetBlockNum(), oldctid.GetOffset(),
        ctid->GetFileId(), ctid->GetBlockNum(), ctid->GetOffset(), result);
  }

  return result;
}

DSTORE::HeapTuple *FetchBlobFiled(StorageRelation relation,
                                  DSTORE::HeapTuple *tuple,
                                  DSTORE::Snapshot dstoreSnapshot,
                                  bool isConsistentRead) {
  DSTORE::SnapshotData snapshot = *dstoreSnapshot;
  if (!isConsistentRead) {
    CDESetSnapshotByCurrent(snapshot);
  }

  DSTORE::HeapTuple *tupleWithLob = HeapInterface::FetchTupleWithLob(
      relation, tuple, &snapshot, AllocMemZeroForDtuple);
  return tupleWithLob;
}

int StoreVirtualFieldValueToDstore(THD *currentThd, TABLE *mysqlTable,
                                   unsigned char *mysqlRec,
                                   DictVirtualCol *vCol, Oid typeOid,
                                   dml_tuple_t *tuple) {
  /** Bitmap for specifying which virtual columns the server
  should evaluate */
  MY_BITMAP columnMap;
  my_bitmap_map colMapStorage[bitmap_buffer_size(MAX_FIELDS)];
  bitmap_init(&columnMap, colMapStorage, MAX_FIELDS);

  /** Specify the column the server should evaluate */
  bitmap_set_bit(&columnMap, vCol->ind);

  bool ret = false;
  ret = handler::my_eval_gcolumn_expr(currentThd, mysqlTable, &columnMap,
                                      (uchar *)mysqlRec, nullptr, nullptr);
  if (ret) return HA_ERR_COMPUTE_FAILED;

  Field *mysqlField = mysqlTable->field[vCol->ind];
  bool *fullTupleIsNulls = tuple->is_nulls;
  if (mysqlField->is_nullable()) {
    uint32_t mysqlNullByteOffset =
        mysqlField->null_offset(mysqlTable->record[0]);
    *fullTupleIsNulls = mysqlRec[mysqlNullByteOffset] & mysqlField->null_bit;
    if (*fullTupleIsNulls) return CDE_OK;  // value is NULL => finish
  } else {
    *fullTupleIsNulls = false;
  }

  uint32_t offset =
      static_cast<uint32_t>(mysqlField->offset(mysqlTable->record[0]));

  return StoreMysqlFieldToDstoreFormat(mysqlRec + offset, typeOid, mysqlField,
                                       false, *(tuple->values),
                                       *(tuple->varlenas), tuple->is_blob);
}

} /* namespace CDE */
