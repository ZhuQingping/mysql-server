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

#ifndef CDE_COMPARE_UTILS_H
#define CDE_COMPARE_UTILS_H

#include <type_traits>

#include "common/cde_def.h"
#include "common/cde_typecache.h"

#include "catalog/dstore_function_struct.h"
#include "common/dstore_common_utils.h"
#include "sql/field.h"

namespace CDE {
enum cde_cmp_func_type : uint8_t {
  CMP_TYPE_MO,
  CMP_TYPE_LT,
  CMP_TYPE_LE,
  CMP_TYPE_EQ,
  CMP_TYPE_GE,
  CMP_TYPE_GT,
};

template <bool is_char>
int CdeMysqlCmpText(uint32_t charset_no, const unsigned char *a, uint32_t len_a,
                    const unsigned char *b, uint32_t len_b);

static inline uint16_t CdeUint2Korr(const unsigned char *A) {
  return (uint16_t)((uint16_t)A[1]) + ((uint16_t)A[0] << 8);
}

static inline uint32_t CdeUint3Korr(const unsigned char *A) {
  return (uint32_t)((uint32_t)A[2] + ((uint32_t)A[1] << 8) +
                    ((uint32_t)A[0] << 16));
}

static inline uint32_t CdeUint4Korr(const unsigned char *A) {
  return (uint32_t)((uint32_t)A[3] + ((uint32_t)A[2] << 8) +
                    ((uint32_t)A[1] << 16) + ((uint32_t)A[0] << 24));
}

static inline uint64_t CdeUint5Korr(const unsigned char *A) {
  return (uint64_t)((uint32_t)A[4] + ((uint32_t)A[3] << 8) +
                    ((uint32_t)A[2] << 16) + ((uint32_t)A[1] << 24)) +
         ((uint64_t)A[0] << 32);
}

static inline uint64_t CdeUint6Korr(const unsigned char *A) {
  return (uint64_t)((uint32_t)A[5] + ((uint32_t)A[4] << 8) +
                    ((uint32_t)A[3] << 16) + ((uint32_t)A[2] << 24)) +
         (((uint64_t)((uint32_t)A[1] + ((uint32_t)A[0] << 8))) << 32);
}

static inline uint64_t CdeUint7Korr(const unsigned char *A) {
  return (uint64_t)((uint32_t)A[6] + ((uint32_t)A[5] << 8) +
                    ((uint32_t)A[4] << 16) + ((uint32_t)A[3] << 24)) +
         (((uint64_t)((uint32_t)A[2] + ((uint32_t)A[1] << 8) +
                      ((uint32_t)A[0] << 16)))
          << 32);
}

static inline uint64_t CdeUint8Korr(const unsigned char *A) {
  return (uint64_t)((uint32_t)A[7] + ((uint32_t)A[6] << 8) +
                    ((uint32_t)A[5] << 16) + ((uint32_t)A[4] << 24)) +
         (((uint64_t)((uint32_t)A[3] + ((uint32_t)A[2] << 8) +
                      ((uint32_t)A[1] << 16) + ((uint32_t)A[0] << 24)))
          << 32);
}

static inline int32_t CdeSint3Korr(const unsigned char *A) {
  return ((int32_t)(((A[2]) & 128)
                        ? (((uint32_t)255L << 24) | (((uint32_t)A[2]) << 16) |
                           (((uint32_t)A[1]) << 8) | ((uint32_t)A[0]))
                        : (((uint32_t)A[2]) << 16) | (((uint32_t)A[1]) << 8) |
                              ((uint32_t)A[0])));
}

static inline uint16_t CdeReadFrom2LittleEndian(const uint8_t *buf) {
  return ((uint16_t)(buf[0]) | ((uint16_t)(buf[1]) << 8));
}

static inline void CdeWriteTo2LittleEndian(uint8_t *buf, uint16_t n) {
  buf[0] = (uint8_t)(n & 0xFFUL);
  buf[1] = (uint8_t)((n >> 8) & 0xFFUL);
}

static inline uint64_t CdeReadFromNLittleEndian(const uint8_t *buf,
                                                uint64_t buf_size) {
  uint64_t n = 0;
  const uint8_t *ptr = buf + buf_size;
  for (;;) {
    if (ptr == buf) break;
    ptr--;
    n = n << 8;
    n += (uint64_t)(*ptr);
  }
  return (n);
}

static inline void CdeWriteToNLittleEndian(uint8_t *buf, uint64_t buf_size,
                                           uint64_t n) {
  uint8_t *end = buf + buf_size;
  for (;;) {
    if (buf == end) break;
    *buf = (uint8_t)(n & 0xFF);
    n = n >> 8;
    buf++;
  }
}

DSTORE::Datum CdeSetRdntCharDatum(const unsigned char *string_ptr, uint32_t len,
                                  uint32_t prefix_len, CdeVarlena *varlena);

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
                                   CdeVarlena *varlena);
DSTORE::Datum CdeSetVarcharDatum(const unsigned char *mysql_ptr, uint32_t len,
                                 CdeVarlena *varlena);
DSTORE::Datum CdeSetVarcharDatumFromMysql(const unsigned char *mysql_ptr,
                                          uint32_t length_bytes,
                                          CdeVarlena *varlena);
int ConvertMysqlTimestampBinaryToDstore(const unsigned char *mysqlPtr,
                                        uint32_t len, Datum &dstorePtr);
int ConvertMysqlDateBinaryToDstore(const unsigned char *mysqlPtr, uint32_t len,
                                   Datum &dstorePtr);
int CdeConvertDstoreTimestampBinaryToMysql(unsigned char *mysqlPtr,
                                           uint32_t len,
                                           const Datum &dstorePtr);
int CdeConvertDstoreDateBinaryToMysql(unsigned char *mysqlPtr, uint32_t len,
                                      const Datum &dstorePtr);
/**
Convert Data Format (MySQL to Dstore).
MySQL Format:
-------------------------------------
| data length bytes  | data pointer |
-------------------------------------
| TINYBLOB - 1 byte  |              |
| BLOB -2 byte       |              |
| MEDIUMBLOB -3 byte |              |
| LONGBLOB -4 byte   |              |
-------------------------------------
Dstore Format:
-----------------------------------------
| total length |  tag  |  tag  |  data  |
-----------------------------------------
|    30 bit    | 1 bit | 1 bit |        |
-----------------------------------------
data size(Bytes) = total length(Bytes) - 4(Bytes)

@param[in]      mysqlPtr         The buffer to store BLOB data in MySQL format.
@param[in]      mysqlBufferSize  Determines into how many bytes the blob length
is stored, the space for the length may vary from 1 to 4 bytes + 8
bytes(portable_sizeof_char_ptr)
@param[in,out]  varlena          Store the converted blob data information,
which is convenient for releasing the memory allocated by CdeZalloc.
@param[in,out]  errorMsg         0 on success, or an error code if an error
occurs (e.g., HA_ERR_TOO_BIG_ROW, CDE_ERROR).
@param[in,out]  isAllocBlobMem   Indicates whether the memory allocated by
CdeZalloc needs to be released in virtual ~dml_ctx().

@return BLOB data in Dstore format.
*/
DSTORE::Datum MysqlLobDataToDstore(const unsigned char *mysqlPtr,
                                   const Field *mysqlField, CdeVarlena *varlena,
                                   int32_t *errorMsg, bool *isAllocBlobMem);
uint16_t CdeGetCmpctCharHeadLen(char *ptr);
uint16_t CdeGetCmpctCharHeadLen(uint16_t max_len);
uint16_t CdeGetCmpctCharLen(char *ptr);
uint16_t CdeGetVarcharLenFromPtr(Oid type_oid, Datum data);
uint16_t CdeParseDatatypeLen(DSTORE::Oid type_oid, DSTORE::Datum data);
int CdeCopyDatumCb(char *tupleValues, size_t remainLength, DSTORE::Oid typeOid,
                   DSTORE::Datum data, size_t &data_len);
void CdeParseVarchar(DSTORE::Datum data, DSTORE::Oid oid,
                     const unsigned char *&ptr, uint16_t &len,
                     uint16_t &head_byte);
void CdeParseRdntChar(DSTORE::Datum data, uint32_t stored_len,
                      const unsigned char *&ptr, uint32_t &len);
void CdeParseCmpctChar(DSTORE::Datum data, const unsigned char *&ptr,
                       uint16_t &len, uint16_t &head_byte);

/**
Convert a datum which point to a 3 bytes buffer to an uint32 value,
used for datatype which have 3 bytes like mediumint/datetime.

@param[in]      datum  Data pointer to an address only have 3 bytes useful

@return converted value
*/
template <typename T>
static inline uint32_t CdeReadFrom3LittleEndian(T datum) {
  const uint8_t *b = (uint8_t *)datum;
  return ((static_cast<uint32_t>(b[0])) | (static_cast<uint32_t>(b[1]) << 8) |
          static_cast<uint32_t>(b[2]) << 16);
}

#define BYTE1_FROM_DSTORE_PTR(datum) \
  ((uint8_t)(*(uint64_t *)(datum)&0x00000000000000FF))
#define BYTE2_FROM_DSTORE_PTR(datum) \
  ((uint16_t)(*(uint64_t *)(datum)&0x000000000000FFFF))

#define INT24_TO_INT(x)                                                       \
  (((x) << 8) >> 8) /* if x is unsigned int, use Logical right shift, or, use \
                       Arithmetic right shift*/

uint64_t CdeBitBufToUlonglong(const unsigned char *buf, int16_t buf_len);

static inline DSTORE::Oid CdeGetTypeOidFromExtra(
    DSTORE::FunctionCallInfo fcinfo) {
  CDE_ASSERT(fcinfo->flinfo->fnExtra != nullptr);
  return CdeRmCollFromOid(DSTORE::FnExtraGetOid(fcinfo->flinfo->fnExtra));
}

static inline DSTORE::Oid CdeGetTypeOidFromExtra(
    DSTORE::FunctionCallInfo fcinfo, uint32_t &charset_no) {
  CDE_ASSERT(fcinfo->flinfo->fnExtra != nullptr);
  DSTORE::Oid oid = DSTORE::FnExtraGetOid(fcinfo->flinfo->fnExtra);
  charset_no = (oid >> 16) & MAX_COLLATION_NUM;
  return CdeRmCollFromOid(oid);
}

template <typename T, cde_cmp_func_type func_type>
static DSTORE::Datum CdeGetCmpRes(T a, T b) {
  switch (func_type) {
    case CMP_TYPE_MO:
      return (a > b) ? ((DSTORE::Datum)1)
                     : ((a < b) ? ((DSTORE::Datum)-1) : ((DSTORE::Datum)0));
    case CMP_TYPE_LT:
      return (a < b) ? ((DSTORE::Datum)1) : ((DSTORE::Datum)0);
    case CMP_TYPE_LE:
      return (a <= b) ? ((DSTORE::Datum)1) : ((DSTORE::Datum)0);
    case CMP_TYPE_EQ:
      return (a == b) ? ((DSTORE::Datum)1) : ((DSTORE::Datum)0);
    case CMP_TYPE_GE:
      return (a >= b) ? ((DSTORE::Datum)1) : ((DSTORE::Datum)0);
    case CMP_TYPE_GT:
      return (a > b) ? ((DSTORE::Datum)1) : ((DSTORE::Datum)0);
    default:
      CDE_ASSERT(0);
      break;
  }
  return (DSTORE::Datum)(0);
}

/**
Get value from datum which is a pass-by-value type of integer.
Template type T can only be int8_t or uint8_t or int16_t or
uint16_t or int32_t or uint32_t or int64_t or uint64_t.

@param[in]  datum    A datum contains a value of a pass-by-value
                     of integer type.

@return The integer value parsed from the datum.
*/
template <typename T>
static inline T DatumToInt(DSTORE::Datum datum) {
  return static_cast<T>(datum & (T)(~0));
}

/**
Get value from datum which is a pass-by-value type of MEDIUMINT.
Template type T can only be int32_t or uint32_t.

@param[in]  datum    A datum contains a value of a pass-by-value
                     of MEDIUMINT type.

@return The integer value parsed from the datum.
*/
template <typename T, typename U>
static inline T DatumToInt24(U datum) {
  T val = static_cast<T>(CdeReadFrom3LittleEndian<U>(datum));
  return INT24_TO_INT(val);
}

/**
Get value from datum which is a pass-by-value type of float.

@param[in]  datum    A datum contains a value of a pass-by-value
                     of float type.

@return The float value parsed from the datum.
*/
static inline float DatumToFloat(DSTORE::Datum datum) {
  union {
    uint32_t u;
    float f;
  } val;
  val.u = DatumToInt<uint32_t>(datum);
  return val.f;
}

/**
Get value from datum which is a pass-by-value type of double.

@param[in]  datum    A datum contains a value of a pass-by-value
                     of double type.

@return The double value parsed from the datum.
*/
static inline double DatumToDouble(DSTORE::Datum datum) {
  union {
    uint64_t u;
    double f;
  } val;
  val.u = DatumToInt<uint64_t>(datum);
  return val.f;
}

/* operaters "<,<=,=,>=,>" of this type must be defined.
 */
template <typename T>
struct cde_cmp_int {
  /* T can only be int8_t or uint8_t or int16_t or uint16_t
  or int32_t or uint32_t or int64_t or uint64_t. */
  template <cde_cmp_func_type func_type>
  static DSTORE::Datum cmp(DSTORE::FunctionCallInfo fcinfo) {
    T a = DatumToInt<T>(fcinfo->arg[0]);
    T b = DatumToInt<T>(fcinfo->arg[1]);
    return CdeGetCmpRes<T, func_type>(a, b);
  }
};

template <typename T>
struct cde_cmp_int24 {
  /* T can only be int32_t or uint32_t */
  template <cde_cmp_func_type func_type>
  static DSTORE::Datum cmp(DSTORE::FunctionCallInfo fcinfo) {
    T a = DatumToInt24<T, DSTORE::Datum>(fcinfo->arg[0]);
    T b = DatumToInt24<T, DSTORE::Datum>(fcinfo->arg[1]);
    return CdeGetCmpRes<T, func_type>(a, b);
  }
};

template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpFloat(DSTORE::FunctionCallInfo fcinfo) {
  float a = DatumToFloat(fcinfo->arg[0]);
  float b = DatumToFloat(fcinfo->arg[1]);
  return CdeGetCmpRes<float, func_type>(a, b);
}

template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpDouble(DSTORE::FunctionCallInfo fcinfo) {
  double a = DatumToDouble(fcinfo->arg[0]);
  double b = DatumToDouble(fcinfo->arg[1]);
  return CdeGetCmpRes<double, func_type>(a, b);
}

template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpBit(DSTORE::FunctionCallInfo fcinfo) {
  DSTORE::Oid oid = CdeGetTypeOidFromExtra(fcinfo);
  uint32_t buf_len = CdeGetBitLen(oid);
  /* Is it really faster then memcmp? */
  uint64_t a = CdeBitBufToUlonglong((unsigned char *)(fcinfo->arg[0]), buf_len);
  uint64_t b = CdeBitBufToUlonglong((unsigned char *)(fcinfo->arg[1]), buf_len);
  return CdeGetCmpRes<uint64_t, func_type>(a, b);
}

// todo: optimize len <= 8
template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpBin(DSTORE::FunctionCallInfo fcinfo) {
  DSTORE::Oid oid = CdeGetTypeOidFromExtra(fcinfo);
  uint32_t buf_len = CdeGetBinLen(oid);
  int cmp_res =
      memcmp((char *)(fcinfo->arg[0]), (char *)(fcinfo->arg[1]), buf_len);
  return CdeGetCmpRes<int, func_type>(cmp_res, 0);
}

template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpChar(DSTORE::FunctionCallInfo fcinfo) {
  DSTORE::Datum a = (DSTORE::Datum)fcinfo->arg[0];
  DSTORE::Datum b = (DSTORE::Datum)fcinfo->arg[1];

  uint32_t charset_no;
  DSTORE::Oid oid = CdeGetTypeOidFromExtra(fcinfo, charset_no);
  uint32_t stored_len = CdeGetCharLen(oid);

  const unsigned char *ptr_a, *ptr_b;
  uint32_t len_a, len_b;
  CdeParseRdntChar(a, stored_len, ptr_a, len_a);
  CdeParseRdntChar(b, stored_len, ptr_b, len_b);

  // SEE: innobase_mysql_cmp
  int cmp_res = CdeMysqlCmpText<true>(charset_no, ptr_a, len_a, ptr_b, len_b);
  return CdeGetCmpRes<int, func_type>(cmp_res, 0);
}

template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpCmpctChar(DSTORE::FunctionCallInfo fcinfo) {
  DSTORE::Datum a = (DSTORE::Datum)fcinfo->arg[0];
  DSTORE::Datum b = (DSTORE::Datum)fcinfo->arg[1];

  uint32_t charset_no;
  (void)CdeGetTypeOidFromExtra(fcinfo, charset_no);

  const unsigned char *ptr_a, *ptr_b;
  uint16_t len_a, len_b;
  uint16_t head_byte;  // not care

  CdeParseCmpctChar(a, ptr_a, len_a, head_byte);
  CdeParseCmpctChar(b, ptr_b, len_b, head_byte);

  int cmp_res = CdeMysqlCmpText<true>(charset_no, ptr_a, len_a, ptr_b, len_b);
  return CdeGetCmpRes<int, func_type>(cmp_res, 0);
}

template <cde_cmp_func_type func_type>
DSTORE::Datum CdeCmpVarchar(DSTORE::FunctionCallInfo fcinfo) {
  DSTORE::Datum a = (DSTORE::Datum)fcinfo->arg[0];
  DSTORE::Datum b = (DSTORE::Datum)fcinfo->arg[1];
  uint32_t charset_no;
  DSTORE::Oid oid = CdeGetTypeOidFromExtra(fcinfo, charset_no);

  const unsigned char *ptr_a, *ptr_b;
  uint16_t len_a, len_b;
  uint16_t head_byte;  // not care

  CdeParseVarchar(a, oid, ptr_a, len_a, head_byte);
  CdeParseVarchar(b, oid, ptr_b, len_b, head_byte);

  int cmp_res = CdeMysqlCmpText<false>(charset_no, ptr_a, len_a, ptr_b, len_b);
  return CdeGetCmpRes<int, func_type>(cmp_res, 0);
}

/**
 * @brief Compares NUL-terminated UTF-8 strings case insensitively.
 *
 * @param[in] a first string to compare
 * @param[in] b second string to compare
 *
 * @return 0 if a=b, <0 if a<b, >0 if a>b
 */
int CdeStrcasecmp(const char *a, const char *b);
} /* namespace CDE */
#endif
