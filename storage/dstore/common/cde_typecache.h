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

#ifndef CDE_TYPECACHE_H
#define CDE_TYPECACHE_H

#include <iostream>
#include <unordered_map>

#include "common/cde_def.h"
#include "dml/cde_btree.h"

#include "catalog/dstore_function_struct.h"
#include "common/dstore_common_utils.h"

namespace CDE {

/* OIDS 1 - 99 */
#define CDE_INVALIDOID (static_cast<DSTORE::Oid>(0))

#define CDE_BIT_TYPE_MAX_LEN (8)

#define CDE_BIN_TYPE_MAX_LEN (1024)
/* todo: use mysql macro? for utf-8, max length is 255*4.
 * notice the max data length supported by dstore is 32767.
 */
#define CDE_TYPE_MAX_LEN (32767)
#define CDE_CHAR_TYPE_MAX_LEN CDE_TYPE_MAX_LEN
#define MAX_COLLATION_NUM (32767)

#define DEREF_TAG (0x1111000000000000ULL)
#define DEREF_TAG_MASK (0xFFFF000000000000ULL)
#define DEREF_PTR_MASK (0x0000FFFFFFFFFFFFULL)

#define MYSQL_STR_HEAD_LENTH1 (1)
#define MYSQL_STR_HEAD_LENTH2 (2)
#define MYSQL_STORE_BYTE3 (3)

/** Dstore uses the varattrib_4b structure (the structure definition can be
found in common/dstore_datatype.h) to receive LOB data from external sources.
Uint32 (4 bytes) is used to represent the HEADER length. */
#define DSTORE_LOB_HEADER_BYTES (sizeof(uint32_t))

/** Dstore constraints, specific code can refer to LZ4_compressBound and
VARHDRSZ_COMPRESSED in HeapLobHandler::Compress. The maximum supported value for
dstore is 0x3FBFFFE8. */
constexpr uint32_t DSTORE_LOB_MAX_SIZE(0x3FBFFFE8);

/* todo: use uint64 here
 * Currently, we use marco for start oid of multiple oid
 * and subtract it from the stored oid inefficiently,
 * we'd better use bit operation instead
 */
enum CdeDatatypeOid : DSTORE::Oid {
  CDE_DATATYPE_OID_START = 0x00001000UL, /* do not change this value */

  /* single oid */

  CDE_COMPACT_CHAR_OID,
  CDE_VARCHAR1B_OID,
  CDE_VARCHAR2B_OID,
  // CDE_ENUM_OID,       /* mysql use MYSQL_TYPE_STRING instead */
  CDE_BIT_OID,
  CDE_BIN_OID,
  CDE_CHAR_OID,

  /* add new single oid here */
  CDE_DATATYPE_OID_END,

  /* multiple oid */

  // char 1~32767
  CDE_CHAR_START_OID = 0x00002000UL, /* do not change this value */
  /* Oids between this range are used for mysql data type char(1~32767) */
  CDE_CHAR_END_OID = CDE_CHAR_START_OID + CDE_CHAR_TYPE_MAX_LEN + 1,

  // bin 1~255
  CDE_BIN_START_OID,
  /* Oids between this range are used for mysql binary data type like
     time(),datetime(5~8),timestamp */
  CDE_BIN_END_OID = CDE_BIN_START_OID + CDE_BIN_TYPE_MAX_LEN + 1,

  // bit 1~8
  CDE_BIT_START_OID,
  /* Oids between this range are used for mysql type bit(1),..,bit(8) */
  CDE_BIT_END_OID = CDE_BIT_START_OID + CDE_BIT_TYPE_MAX_LEN + 1,

  /* add new multiple oid here */
  CDE_DATATYPE_OID_MAX = 0x0000FFFFUL, /* do NOT change this value */
};

static_assert(CDE_DATATYPE_OID_END <= CDE_BIT_START_OID);
static_assert(CDE_CHAR_END_OID <= CDE_DATATYPE_OID_MAX);
static_assert(CDE_BIN_END_OID <= CDE_DATATYPE_OID_MAX);
static_assert(CDE_BIT_END_OID <= CDE_DATATYPE_OID_MAX);

constexpr uint32_t OID_COLL_SHIFT = 16;
constexpr uint32_t RM_OID_COLL_MASK = (0x1UL << OID_COLL_SHIFT) - 1;

#define CDE_DATATYPE_NUM (CDE_DATATYPE_OID_END - CDE_DATATYPE_OID_START - 1)
#define CDE_DATATYPE_SLOT_ID(type_oid) ((type_oid)-CDE_DATATYPE_OID_START - 1)
#define CHECK_DATATYPE_OID_VALID(type_oid) \
  (((type_oid) > CDE_DATATYPE_OID_START) && ((type_oid) < CDE_DATATYPE_OID_END))

#define MAINTAIN_ORDER_FN_OID(datatype) datatype##_MAINTAIN_ORDER_FN_OID
#define LESS_THAN_FN_OID(datatype) datatype##_LESS_THAN_FN_OID
#define LESS_EQUAL_FN_OID(datatype) datatype##_LESS_EQUAL_FN_OID
#define EQUAL_FN_OID(datatype) datatype##_EQUAL_FN_OID
#define GREATER_EQUAL_FN_OID(datatype) datatype##_GREATER_EQUAL_FN_OID
#define GREATER_THAN_FN_OID(datatype) datatype##_GREATER_THAN_FN_OID

#define DEF_CMP_FUNC_OIDS(datatype)                            \
  MAINTAIN_ORDER_FN_OID(datatype), LESS_THAN_FN_OID(datatype), \
      LESS_EQUAL_FN_OID(datatype), EQUAL_FN_OID(datatype),     \
      GREATER_EQUAL_FN_OID(datatype), GREATER_THAN_FN_OID(datatype)

enum CdePgFuncOid : DSTORE::Oid {
  CDE_PG_FUNC_OID_START = CDE_DATATYPE_OID_MAX,
  DEF_CMP_FUNC_OIDS(CDE_COMPACT_CHAR_OID),
  DEF_CMP_FUNC_OIDS(CDE_VARCHAR1B_OID),
  DEF_CMP_FUNC_OIDS(CDE_VARCHAR2B_OID),
  DEF_CMP_FUNC_OIDS(CDE_BIT_OID),
  DEF_CMP_FUNC_OIDS(CDE_BIN_OID),
  DEF_CMP_FUNC_OIDS(CDE_CHAR_OID),
  CDE_PG_FUNC_OID_END
};

#define CDE_PG_FUNC_NUM (CDE_PG_FUNC_OID_END - CDE_PG_FUNC_OID_START - 1)
#define CDE_PG_FUNC_SLOT_ID(fn_oid) ((fn_oid)-CDE_PG_FUNC_OID_START - 1)
#define CHECK_PG_FUNC_OID_VALID(fn_oid) \
  (((fn_oid) > CDE_PG_FUNC_OID_START) && ((fn_oid) < CDE_PG_FUNC_OID_END))

struct CdeVarlena {
  uint32_t len;
  uint16_t max_len;
  uint16_t prefix_n_chars;
  const unsigned char *data;
};

/**
Check whether the oid represent varlena type.

@param[in]  oid  Object ID of type.

@return true if is varlena type othewise false.
*/
inline bool CdeCheckIsVarlenaType(DSTORE::Oid oid) {
  return (oid == CDE_COMPACT_CHAR_OID) || (oid == CDE_VARCHAR1B_OID) ||
         (oid == CDE_VARCHAR2B_OID);
}

/**
Remove collation attribute of oid.

@param[in]  oid  Object ID of type.

@return oid of type removed collation attribute.
*/
inline DSTORE::Oid CdeRmCollFromOid(DSTORE::Oid oid) {
  return oid & RM_OID_COLL_MASK;
}

/**
Get the base type oid which remove the length attribute from dstore oid.

@param[in]  oid  Object ID of type.

@return base type oid.
*/
inline DSTORE::Oid CdeConvertDatatypeOid(DSTORE::Oid oid) {
  oid = CdeRmCollFromOid(oid);
  if ((oid > CDE_BIT_START_OID) && (oid < CDE_BIT_END_OID)) {
    return CDE_BIT_OID;
  } else if ((oid > CDE_BIN_START_OID) && (oid < CDE_BIN_END_OID)) {
    return CDE_BIN_OID;
  } else if ((oid > CDE_CHAR_START_OID) && (oid < CDE_CHAR_END_OID)) {
    return CDE_CHAR_OID;
  }
  return oid;
}

/**
Get the dstore oid from bin base type oid plus length attribute.

@param[in]  len Length of type.

@return dstore oid.
*/
inline DSTORE::Oid CdeGetBinOid(uint32_t len) {
  CDE_ASSERT((len > 0) && (len <= CDE_BIN_TYPE_MAX_LEN));
  return static_cast<DSTORE::Oid>(len +
                                  static_cast<uint32_t>(CDE_BIN_START_OID));
}

/**
Get the dstore oid from bit base type oid plus length attribute.

@param[in]  len Length of type.

@return dstore oid.
*/
inline DSTORE::Oid CdeGetBitOid(uint32_t len) {
  CDE_ASSERT((len > 0) && (len <= CDE_BIT_TYPE_MAX_LEN));
  return static_cast<DSTORE::Oid>(len +
                                  static_cast<uint32_t>(CDE_BIT_START_OID));
}

/**
Get the dstore oid from char base type oid plus length attribute.

@param[in]  len Length of type.

@return dstore oid.
*/
inline DSTORE::Oid CdeGetCharOid(uint32_t len) {
  CDE_ASSERT((len > 0) && (len <= CDE_CHAR_TYPE_MAX_LEN));
  return static_cast<DSTORE::Oid>(len +
                                  static_cast<uint32_t>(CDE_CHAR_START_OID));
}

/**
Get the length attribute of type from bin type oid.

@param[in]  oid Dstore oid of bin type.

@return the length attribute of type.
*/
inline uint32_t CdeGetBinLen(DSTORE::Oid oid) {
  CDE_ASSERT((oid > CDE_BIN_START_OID) && (oid < CDE_BIN_END_OID));
  return static_cast<uint32_t>(oid) - static_cast<uint32_t>(CDE_BIN_START_OID);
}

/**
Get the length attribute of type from bit type oid.

@param[in]  oid Dstore oid of bit type.

@return the length attribute of type.
*/
inline uint32_t CdeGetBitLen(DSTORE::Oid oid) {
  CDE_ASSERT((oid > CDE_BIT_START_OID) && (oid < CDE_BIT_END_OID));
  return static_cast<uint32_t>(oid) - static_cast<uint32_t>(CDE_BIT_START_OID);
}

/**
Get the length attribute of type from char type oid.

@param[in]  oid Dstore oid of char type.

@return the length attribute of type.
*/
inline uint32_t CdeGetCharLen(DSTORE::Oid oid) {
  CDE_ASSERT((oid > CDE_CHAR_START_OID) && (oid < CDE_CHAR_END_OID));
  return static_cast<uint32_t>(oid) - static_cast<uint32_t>(CDE_CHAR_START_OID);
}

/**
Get the header length of varchar type oid.

@param[in]  oid Dstore oid of varchar type.

@return the length of header.
*/
inline uint32_t CdeGetVarcharHeadLen(DSTORE::Oid oid) {
  switch (oid) {
    case CDE_VARCHAR1B_OID:
      return 1;
    case CDE_VARCHAR2B_OID:
      return 2;
    default:
      CDE_LOG_ERROR("invalid varchar typeoid: %d", oid);
      CDE_ASSERT(0);
      return 0;
  }
}

/**
Get varchar type oid by header's length.

@param[in]  headLen  The length of header.

@return Dstore oid of varchar type.
*/
inline DSTORE::Oid CdeGetVarcharOid(uint32_t headLen) {
  switch (headLen) {
    case 1:
      return CDE_VARCHAR1B_OID;
    case 2:
      return CDE_VARCHAR2B_OID;
    default:
      CDE_ASSERT(0);
      return CDE_INVALIDOID;
  }
}

struct CacheKey {
  DSTORE::Oid leftType;
  DSTORE::Oid rightType;
  uint16_t proc;

  bool operator==(const CacheKey &other) const {
    return leftType == other.leftType && rightType == other.rightType &&
           proc == other.proc;
  }
};

struct CacheKeyHash {
  uint64_t operator()(const CacheKey &key) const {
    return ((uint64_t)key.leftType << 32) ^ ((uint64_t)key.rightType << 16) ^
           key.proc;
  }
};

class cache_hash_manager {
 public:
  static cache_hash_manager *get_ins();
  void init();

  const DSTORE::TypeCache *get_type_cache(DSTORE::Oid type_oid);
  DSTORE::Oid get_fn_oid(DSTORE::Oid left_type, DSTORE::Oid right_type,
                         uint16_t proc);
  const DSTORE::FuncCache *get_func_cache(DSTORE::Oid function_oid);
  const DSTORE::FuncCache *get_func_cache(DSTORE::Oid left_type,
                                          DSTORE::Oid right_type,
                                          uint16_t proc);
  /**
  Look up the comparison function in the cache for the specified types and
  strategy.
  @param leftType[in]  Oid of the left type.
  @param rightType[in] Oid of the right type.
  @param proc[in]      Comparison strategy.

  @return Returns the function cache entry if found, otherwise returns nullptr.
  */
  const DSTORE::FuncCache *FindDstoreFuncCache(DSTORE::Oid leftType,
                                               DSTORE::Oid rightType,
                                               uint16_t proc);

 private:
  bool func_cache_is_init{false};
  cache_hash_manager() {}
  ~cache_hash_manager() {}
  const DSTORE::TypeCache *type_cache_slot[CDE_DATATYPE_NUM]{nullptr};
  std::unordered_map<DSTORE::Oid, const DSTORE::TypeCache *> dstoreTypeCache;
  const DSTORE::FuncCache *func_cache_oid_slot[CDE_PG_FUNC_NUM]{nullptr};
  const DSTORE::FuncCache
      *func_cache_type_slot[CDE_DATATYPE_NUM][CDE_OPERATOR_STRATEGY_NUM]{
          nullptr};
  std::unordered_map<CacheKey, const DSTORE::FuncCache *, CacheKeyHash>
      dstoreFuncCache;
};

static inline const DSTORE::TypeCache *CdeGetTypeCache(DSTORE::Oid type_oid) {
  return cache_hash_manager::get_ins()->get_type_cache(type_oid);
}

static inline DSTORE::Oid CdeGetFnOid(DSTORE::Oid left_type,
                                      DSTORE::Oid right_type, uint16_t proc) {
  return cache_hash_manager::get_ins()->get_fn_oid(left_type, right_type, proc);
}

static inline const DSTORE::FuncCache *CdeGetFuncCache(
    DSTORE::Oid function_oid) {
  return cache_hash_manager::get_ins()->get_func_cache(function_oid);
}

static inline const DSTORE::FuncCache *CdeGetFuncCache(DSTORE::Oid left_type,
                                                       DSTORE::Oid right_type,
                                                       uint16_t proc) {
  return cache_hash_manager::get_ins()->get_func_cache(left_type, right_type,
                                                       proc);
}

static inline const DSTORE::FuncCache *CdeGetDstoreFuncCache(
    DSTORE::Oid leftType, DSTORE::Oid rightType, uint16_t proc) {
  return cache_hash_manager::get_ins()->FindDstoreFuncCache(leftType, rightType,
                                                            proc);
}
} /* namespace CDE */
#endif
