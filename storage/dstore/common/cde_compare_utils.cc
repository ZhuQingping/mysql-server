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

#include "cde_compare_utils.h"
#include "catalog/dstore_function.h"
#include "common/cde_alloc.h"
#include "common/datatype/dstore_varlena_utils.h"
#include "my_base.h"

extern CHARSET_INFO *get_charset(uint cs_number, myf flags);
namespace CDE {
DSTORE::Datum CdeSetRdntCharDatum(const unsigned char *string_ptr, uint32_t len,
                                  uint32_t prefix_len, CdeVarlena *varlena) {
  varlena->len = static_cast<uint16_t>(len);
  varlena->prefix_n_chars = static_cast<uint16_t>(prefix_len);
  varlena->data = string_ptr;
  CDE_ASSERT(((uint64_t)varlena & DEREF_TAG_MASK) == 0);
  return static_cast<Datum>((uint64_t)varlena | DEREF_TAG);
}

/** Convert mysql compact string type data to CdeVarlena type.

@param[in]  str_ptr   MySQL record data buffer.
@param[in]  str_len   Valid data length in buffer.
@param[in]  max_len   Maximum length of buffer.
@param[in]  mbmaxlen  Maximum length of a character, in bytes.
@param[in]  mbminlen  Minimum length of a character, in bytes.
@param[out] varlena   Output to store the details of the compact string.
@return pointer to field of dstore tuple */
DSTORE::Datum CdeSetCmpctCharDatum(const unsigned char *str_ptr,
                                   uint32_t str_len, uint32_t max_len,
                                   uint32_t mbmaxlen, uint32_t mbminlen,
                                   CdeVarlena *varlena) {
  CDE_ASSERT((max_len < CDE_CHAR_TYPE_MAX_LEN) && (str_len <= max_len));
  varlena->max_len = static_cast<uint16_t>(max_len);

  /* In some cases we strip trailing spaces from UTF-8 and other
  multibyte charsets, from FIXED-length CHAR columns, to save
  space. UTF-8 would otherwise normally use 3 * the string length
  bytes to store an ASCII string! */

  /* We assume that this CHAR field is encoded in a
  variable-length character set where spaces have
  1:1 correspondence to 0x20 bytes, such as UTF-8.

  Consider a CHAR(n) field, a field of n characters.
  It will contain between n * mbminlen and n * mbmaxlen bytes.
  We will try to truncate it to n bytes by stripping
  space padding.      If the field contains single-byte
  characters only, it will be truncated to n characters.
  Consider a CHAR(5) field containing the string
  ".a   " where "." denotes a 3-byte character represented
  by the bytes "$%&". After our stripping, the string will
  be stored as "$%&a " (5 bytes). The string
  ".abc " will be stored as "$%&abc" (6 bytes).

  The space padding will be restored in cde_heap.cc, function
  CdeCmpctCharDstoreToMysql(). */
  static constexpr unsigned char SPACE_CHAR = 0x20;
  uint32_t n_chars = max_len / mbmaxlen;
  if (mbminlen == 1 && mbmaxlen > 1) {
    /* Strip space padding. */
    while (str_len > n_chars && str_ptr[str_len - 1] == SPACE_CHAR) {
      str_len--;
    }
  }

  varlena->len = static_cast<uint16_t>(str_len);
  varlena->prefix_n_chars = static_cast<uint16_t>(n_chars);
  varlena->data = str_ptr;
  CDE_ASSERT(((uint64_t)varlena & DEREF_TAG_MASK) == 0);
  return static_cast<Datum>((uint64_t)varlena | DEREF_TAG);
}

DSTORE::Datum CdeSetVarcharDatum(const unsigned char *mysql_ptr, uint32_t len,
                                 CdeVarlena *varlena) {
  varlena->len = len;
  varlena->data = mysql_ptr;
  CDE_ASSERT(((uint64_t)varlena & DEREF_TAG_MASK) == 0);
  return static_cast<Datum>((uint64_t)varlena | DEREF_TAG);
}

DSTORE::Datum CdeSetVarcharDatumFromMysql(const unsigned char *mysql_ptr,
                                          uint32_t length_bytes,
                                          CdeVarlena *varlena) {
  switch (length_bytes) {
    case MYSQL_STR_HEAD_LENTH1:
      varlena->len = static_cast<uint32_t>(*(const uint8_t *)(mysql_ptr));
      varlena->data = (mysql_ptr + MYSQL_STR_HEAD_LENTH1);
      break;
    case MYSQL_STR_HEAD_LENTH2:
      varlena->len = static_cast<uint32_t>(
          CdeReadFrom2LittleEndian((const uint8_t *)mysql_ptr));
      varlena->data = (mysql_ptr + MYSQL_STR_HEAD_LENTH2);
      break;
    default:
      CDE_ASSERT(0);
      break;
  }
  CDE_ASSERT(((uint64_t)varlena & DEREF_TAG_MASK) == 0);
  return static_cast<Datum>((uint64_t)varlena | DEREF_TAG);
}

int ConvertMysqlTimestampBinaryToDstore(const unsigned char *mysqlPtr,
                                        uint32_t len, Datum &dstorePtr) {
  dstorePtr = 0;
  int err = memcpy_s((unsigned char *)(&dstorePtr) + sizeof(Datum) - len, len,
                     mysqlPtr, len);
  if (unlikely(err)) {
    return err;
  }
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  dstorePtr = __builtin_bswap64(dstorePtr);
#endif
  return 0;
}

int ConvertMysqlDateBinaryToDstore(const unsigned char *mysqlPtr, uint32_t len,
                                   Datum &dstorePtr) {
  dstorePtr = 0;
  int err = memcpy_s(&dstorePtr, sizeof(Datum), mysqlPtr, len);
  if (unlikely(err)) {
    return err;
  }
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  dstorePtr = __builtin_bswap64(dstorePtr);
#endif
  return 0;
}

int CdeConvertDstoreTimestampBinaryToMysql(unsigned char *mysqlPtr,
                                           uint32_t len,
                                           const Datum &dstorePtr) {
  Datum tmp = dstorePtr;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  tmp = __builtin_bswap64(tmp);
#endif
  int err = memcpy_s(mysqlPtr, len,
                     (unsigned char *)(&tmp) + (sizeof(Datum) - len), len);
  if (unlikely(err)) {
    return err;
  }
  return 0;
}

int CdeConvertDstoreDateBinaryToMysql(unsigned char *mysqlPtr, uint32_t len,
                                      const Datum &dstorePtr) {
  Datum tmp = dstorePtr;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  tmp = __builtin_bswap64(tmp);
#endif
  int err = memcpy_s(mysqlPtr, len, &tmp, len);
  if (unlikely(err)) {
    return err;
  }
  return 0;
}

/**
Converts a MySQL BLOB/JSON field type to string representation.

@param[in]      fieldType the type of the column.
@param[in]      columnCharset the character set information of the column.

@return the string representation of the BLOB/JSON type.
*/
static std::string CdeBlobTypeNameToString(const enum_field_types fieldType,
                                           const CHARSET_INFO *columnCharset) {
  bool isBinary = (columnCharset == &my_charset_bin);
  switch (fieldType) {
    case MYSQL_TYPE_BLOB:
      return isBinary ? "LONGBLOB" : "LONGTEXT";
    case MYSQL_TYPE_JSON:
      return "JSON";
    default:
      CDE_ASSERT_DEBUG(0);
      return "UNKNOWN";
  }
}

DSTORE::Datum MysqlLobDataToDstore(const unsigned char *mysqlPtr,
                                   const Field *mysqlField, CdeVarlena *varlena,
                                   int32_t *errorCode, bool *isAllocBlobMem) {
  errno_t err = 0;
  *errorCode = CDE_OK;
  uint32_t lengthBytes = mysqlField->pack_length() - portable_sizeof_char_ptr;
  uint32_t dataLen = (uint32_t)CdeReadFromNLittleEndian(mysqlPtr, lengthBytes);
  uint32_t totalLenSize = dataLen + DSTORE_LOB_HEADER_BYTES;
  varlena->data = nullptr;

  if (dataLen > DSTORE_LOB_MAX_SIZE) {
    /* Exceeded the maximum length supported by the dstore. */
    enum_field_types mysqlType = mysqlField->type();
    std::string errorMessage =
        "the data length of " + std::to_string(dataLen) + " bytes for " +
        CdeBlobTypeNameToString(mysqlType, mysqlField->charset()) +
        " column. Maximum supported length is " +
        std::to_string(DSTORE_LOB_MAX_SIZE) + " bytes.";
    my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), errorMessage.c_str());
    *errorCode = CDE_ERROR;
    return DSTORE::Datum(nullptr);
  }
  varlena->len = dataLen;
  /* alloc dataBuf */
  unsigned char *dataBuf = (unsigned char *)CdeZalloc(totalLenSize);
  DBUG_EXECUTE_IF("databuf_null_pointer_injection", CdeFree(dataBuf);
                  dataBuf = nullptr;);
  if (dataBuf == nullptr) {
    CDE_LOG_ERROR("CdeZalloc fail, size is %d", totalLenSize);
    *errorCode = CDE_ERROR;
    return DSTORE::Datum(nullptr);
  }

  /* In the dstore format, using 30 bits to represent the total length allows a
  maximum storage of 0x3FFFFFFF bytes. */
  uint32_t vaHeader = (totalLenSize & 0x3FFFFFFF) << 2;
  err = memcpy_s(dataBuf, DSTORE_LOB_HEADER_BYTES, &vaHeader,
                 DSTORE_LOB_HEADER_BYTES);
  DBUG_EXECUTE_IF("memcpy_error_injection", err = 1;);
  if (unlikely(err != 0)) {
    CDE_LOG_ERROR("memcpy_s fail, ret: %d", err);
    *errorCode = CDE_ERROR;
    CdeFree(dataBuf);
    dataBuf = nullptr;
    return DSTORE::Datum(nullptr);
  }
  if (dataLen > 0) {
    /* copy data */
    std::byte *data;
    err = memcpy_s(&data, sizeof(data), mysqlPtr + lengthBytes, sizeof(data));
    if (unlikely(err != 0)) {
      CDE_LOG_ERROR("memcpy_s fail, ret: %d", err);
      *errorCode = CDE_ERROR;
      CdeFree(dataBuf);
      dataBuf = nullptr;
      return DSTORE::Datum(nullptr);
    }

    err = memcpy_s(dataBuf + DSTORE_LOB_HEADER_BYTES,
                   totalLenSize - DSTORE_LOB_HEADER_BYTES, data, dataLen);
    if (unlikely(err != 0)) {
      CDE_LOG_ERROR("memcpy_s fail, ret: %d", err);
      *errorCode = CDE_ERROR;
      CdeFree(dataBuf);
      dataBuf = nullptr;
      return DSTORE::Datum(nullptr);
    }
  }
  /* To facilitate memory release later in ~dml_ctx, mount the allocated
  dataBuf onto varlena->data. */
  varlena->data = const_cast<const unsigned char *>(dataBuf);
  *isAllocBlobMem = true;
  return static_cast<Datum>(reinterpret_cast<uintptr_t>(dataBuf));
}

uint16_t CdeGetCmpctCharHeadLen(char *ptr) {
  return (ptr[0] & 0x80) ? MYSQL_STR_HEAD_LENTH2 : MYSQL_STR_HEAD_LENTH1;
}

uint16_t CdeGetCmpctCharHeadLen(uint16_t max_len) {
  return (max_len <= 0x7F) ? MYSQL_STR_HEAD_LENTH1 : MYSQL_STR_HEAD_LENTH2;
}

uint16_t CdeGetCmpctCharLen(char *ptr) {
  if (ptr[0] & 0x80) {
    return static_cast<uint16_t>((ptr[0] & 0x7F) << 8) +
           static_cast<uint16_t>(ptr[1] & 0xFF);
  }
  return static_cast<uint16_t>(ptr[0]);
}

uint16_t CdeParseDatatypeLen(Oid type_oid, Datum data) {
  const unsigned char *ptr;
  uint16_t len, head_byte;
  type_oid = CdeRmCollFromOid(type_oid);
  switch (type_oid) {
    case CDE_VARCHAR1B_OID:
    case CDE_VARCHAR2B_OID:
      CdeParseVarchar(data, type_oid, ptr, len, head_byte);
      return len + head_byte;
    case CDE_COMPACT_CHAR_OID:
      CdeParseCmpctChar(data, ptr, len, head_byte);
      return len + head_byte;
    default:
      if (CdeConvertDatatypeOid(type_oid) != CDE_CHAR_OID) {
        CDE_LOG_ERROR("invalid type oid: %u", type_oid);
        CDE_ASSERT(0);
      }
      return static_cast<uint16_t>(CdeGetCharLen(type_oid));
  }
}

uint16_t CdeGetVarcharLenFromPtr(Oid type_oid, Datum data) {
  char *ptr = (char *)(data);
  switch (type_oid) {
    case CDE_VARCHAR1B_OID:
      return static_cast<uint16_t>(*(uint8_t *)(ptr));
    case CDE_VARCHAR2B_OID:
      return CdeReadFrom2LittleEndian((uint8_t *)ptr);
    default:
      CDE_LOG_ERROR("invalid varchar type oid: %d", type_oid);
      CDE_ASSERT(0);
      return 0;
  }
}

/* input oid should be true varchar oid */
void CdeParseVarchar(DSTORE::Datum data, DSTORE::Oid oid,
                     const unsigned char *&ptr, uint16_t &len,
                     uint16_t &head_byte) {
  head_byte = CdeGetVarcharHeadLen(oid);
  if (((uint64_t)data & DEREF_TAG_MASK) == 0) {
    len = CdeGetVarcharLenFromPtr(oid, data);
    ptr = (const unsigned char *)(data) + head_byte;
  } else {
    CDE_ASSERT(((uint64_t)data & DEREF_TAG_MASK) == DEREF_TAG);
    CdeVarlena *varlena =
        reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(data) &
                                       static_cast<uintptr_t>(DEREF_PTR_MASK));
    len = varlena->len;
    ptr = varlena->data;
  }
}

void CdeParseRdntChar(DSTORE::Datum data, uint32_t stored_len,
                      const unsigned char *&ptr, uint32_t &len) {
  if (((uint64_t)data & DEREF_TAG_MASK) == 0) {
    len = stored_len;
    ptr = (const unsigned char *)data;
  } else {
    CDE_ASSERT(((uint64_t)data & DEREF_TAG_MASK) == DEREF_TAG);
    CdeVarlena *varlena =
        reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(data) &
                                       static_cast<uintptr_t>(DEREF_PTR_MASK));
    len = varlena->len;
    ptr = varlena->data;
  }
}

void CdeParseCmpctChar(DSTORE::Datum data, const unsigned char *&ptr,
                       uint16_t &len, uint16_t &head_byte) {
  if (((uint64_t)data & DEREF_TAG_MASK) == 0) {
    head_byte = CdeGetCmpctCharHeadLen((char *)data);
    len = CdeGetCmpctCharLen((char *)data);
    ptr = (const unsigned char *)(data) + head_byte;
  } else {
    CDE_ASSERT(((uint64_t)data & DEREF_TAG_MASK) == DEREF_TAG);
    CdeVarlena *varlena =
        reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(data) &
                                       static_cast<uintptr_t>(DEREF_PTR_MASK));
    head_byte = CdeGetCmpctCharHeadLen(varlena->max_len);
    len = CDE_MAX(varlena->len, varlena->prefix_n_chars);
    ptr = varlena->data;
  }
}

int CdeCopyDatumCbDeref(char *tupleValues, size_t remainLength, Oid typeOid,
                        Datum data, size_t &data_len) {
  errno_t err = 0;
  CdeVarlena *varlena =
      reinterpret_cast<CdeVarlena *>(reinterpret_cast<uintptr_t>(data) &
                                     static_cast<uintptr_t>(DEREF_PTR_MASK));
  const unsigned char *str_ptr = varlena->data;
  uint32_t str_len = varlena->len;
  uint16_t full_str_len = str_len;
  uint16_t prefix_n_chars = 0, n_pad_space = 0;
  uint32_t head_len = 0;

  switch (typeOid) {
    case CDE_VARCHAR1B_OID:
      CDE_ASSERT(str_len <= MAX_U8);
      *((uint8_t *)tupleValues) = str_len;
      head_len = MYSQL_STR_HEAD_LENTH1;
      break;
    case CDE_VARCHAR2B_OID:
      CDE_ASSERT(str_len <= MAX_U16);
      CdeWriteTo2LittleEndian((uint8_t *)tupleValues, str_len);
      head_len = MYSQL_STR_HEAD_LENTH2;
      break;
    case CDE_COMPACT_CHAR_OID:
      prefix_n_chars = varlena->prefix_n_chars;
      if (str_len < prefix_n_chars) {
        n_pad_space = prefix_n_chars - str_len;
        full_str_len = prefix_n_chars;
      }
      if (varlena->max_len <= 0x7F) {
        CDE_ASSERT(full_str_len <= 0x7F);
        *((uint8_t *)tupleValues) = full_str_len;
        head_len = MYSQL_STR_HEAD_LENTH1;
      } else {
        CDE_ASSERT(full_str_len <= CDE_CHAR_TYPE_MAX_LEN);
        *((uint8_t *)tupleValues) = ((full_str_len >> 8) & 0xFF) | 0x80;
        *((uint8_t *)tupleValues + 1) = full_str_len & 0xFF;
        head_len = MYSQL_STR_HEAD_LENTH2;
      }
      break;
    default:
      if (CdeConvertDatatypeOid(typeOid) != CDE_CHAR_OID) {
        CDE_LOG_ERROR("invalid type oid: %u", typeOid);
        return CDE_ERROR;
      }
      break;
  }

  data_len = full_str_len + head_len;

  tupleValues += head_len;
  remainLength -= head_len;
  CDE_ASSERT(remainLength >= str_len);
  if (str_len > 0) {
    err = memcpy_s(tupleValues, remainLength, str_ptr, str_len);
  }
  if (unlikely(err != 0)) {
    CDE_LOG_ERROR("memcpy_s fail, ret: %d", err);
    return CDE_ERROR;
  }

  if (n_pad_space == 0) {
    return CDE_OK;
  }

  tupleValues += str_len;
  remainLength -= str_len;
  CDE_ASSERT(remainLength >= n_pad_space);
  err = memset_s(tupleValues, remainLength, 0x20, n_pad_space);
  if (unlikely(err != 0)) {
    CDE_LOG_ERROR("memset_s fail, ret: %d", err);
    return CDE_ERROR;
  }
  return CDE_OK;
}

int CdeCopyDatumCbNoDeref(char *tupleValues, size_t remainLength, Oid typeOid,
                          Datum data, size_t &data_len) {
  errno_t err;
  char *buf = (char *)data;
  switch (typeOid) {
    case CDE_VARCHAR1B_OID:
      data_len = *((uint8_t *)buf) + MYSQL_STR_HEAD_LENTH1;
      break;
    case CDE_VARCHAR2B_OID:
      data_len =
          CdeReadFrom2LittleEndian((uint8_t *)buf) + MYSQL_STR_HEAD_LENTH2;
      break;
    case CDE_COMPACT_CHAR_OID:
      data_len = CdeGetCmpctCharHeadLen(buf) + CdeGetCmpctCharLen(buf);
      break;
    default:
      if (CdeConvertDatatypeOid(typeOid) != CDE_CHAR_OID) {
        CDE_LOG_ERROR("invalid type oid: %u", typeOid);
        return CDE_ERROR;
      }
      data_len = CdeGetCharLen(typeOid);
      break;
  }
  if (remainLength < data_len) {
    CDE_LOG_ERROR("remainLength not enough(%lu,%lu)", remainLength, data_len);
    return CDE_ERROR;
  }
  err = memcpy_s(tupleValues, remainLength, buf, data_len);
  if (unlikely(err != 0)) {
    CDE_LOG_ERROR("memcpy_s fail, ret: %d", err);
    return CDE_ERROR;
  }
  return CDE_OK;
}

int CdeCopyDatumCb(char *tupleValues, size_t remainLength, Oid typeOid,
                   Datum data, size_t &data_len) {
  typeOid = CdeRmCollFromOid(typeOid);
  if (((uint64_t)data & DEREF_TAG_MASK) == DEREF_TAG) {
    return CdeCopyDatumCbDeref(tupleValues, remainLength, typeOid, data,
                               data_len);
  } else {
    return CdeCopyDatumCbNoDeref(tupleValues, remainLength, typeOid, data,
                                 data_len);
  }
}

uint64_t CdeBitBufToUlonglong(const unsigned char *buf, int16_t buf_len) {
  switch (buf_len) {
    case 0:
      return (uint64_t)0;
    case 1:
      return (uint64_t)buf[0];
    case 2:
      return (uint64_t)CdeUint2Korr(buf);
    case 3:
      return (uint64_t)CdeUint3Korr(buf);
    case 4:
      return (uint64_t)CdeUint4Korr(buf);
    case 5:
      return (uint64_t)CdeUint5Korr(buf);
    case 6:
      return (uint64_t)CdeUint6Korr(buf);
    case 7:
      return (uint64_t)CdeUint7Korr(buf);
    default:
      return (uint64_t)CdeUint8Korr(buf + buf_len - sizeof(uint64_t));
  }
  return (uint64_t)0;
}

template <bool is_char>
int CdeMysqlCmpText(uint32_t charset_no, const unsigned char *a, uint32_t len_a,
                    const unsigned char *b, uint32_t len_b) {
  CHARSET_INFO *cs = get_charset(charset_no, MYF(MY_WME));
  if (!cs) {
    CDE_LOG_ERROR("get CHARSET_INFO from server fail, charset_no=%u",
                  charset_no);
    CDE_ASSERT(0);
    return 0;
  }

  if (is_char && (cs->pad_attribute == NO_PAD)) {
    len_a = cs->cset->lengthsp(cs, (const char *)a, len_a);
    len_b = cs->cset->lengthsp(cs, (const char *)b, len_b);
  }
  return (cs->coll->strnncollsp(cs, a, len_a, b, len_b));
}

template int CdeMysqlCmpText<true>(uint32_t charset_no, const unsigned char *a,
                                   uint32_t len_a, const unsigned char *b,
                                   uint32_t len_b);
template int CdeMysqlCmpText<false>(uint32_t charset_no, const unsigned char *a,
                                    uint32_t len_a, const unsigned char *b,
                                    uint32_t len_b);

int CdeStrcasecmp(const char *a, const char *b) {
  if (!a) {
    if (!b) {
      return (0);
    } else {
      return (-1);
    }
  } else if (!b) {
    return (1);
  }

  return (my_strcasecmp(system_charset_info, a, b));
}
} /* namespace CDE */
