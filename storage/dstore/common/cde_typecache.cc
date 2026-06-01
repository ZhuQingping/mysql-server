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

#include "common/cde_typecache.h"
#include "catalog/dstore_typecache.h"
#include "common/cde_compare_utils.h"

using DSTORE::DSTORE_INVALID_OID;
using DSTORE::MAINTAIN_ORDER;
using DSTORE::SCAN_ORDER_EQUAL;
using DSTORE::SCAN_ORDER_GREATER;
using DSTORE::SCAN_ORDER_GREATEREQUAL;
using DSTORE::SCAN_ORDER_INVALID;
using DSTORE::SCAN_ORDER_LESS;
using DSTORE::SCAN_ORDER_LESSEQUAL;

namespace CDE {

const DSTORE::FuncCache INVALID_CDE_FUNC_CACHE = {
    DSTORE_INVALID_OID, DSTORE_INVALID_OID, DSTORE_INVALID_OID,
    SCAN_ORDER_INVALID, nullptr};

#define MAINTAIN_ORDER_FN_OID(datatype) datatype##_MAINTAIN_ORDER_FN_OID
#define LESS_THAN_FN_OID(datatype) datatype##_LESS_THAN_FN_OID
#define LESS_EQUAL_FN_OID(datatype) datatype##_LESS_EQUAL_FN_OID
#define EQUAL_FN_OID(datatype) datatype##_EQUAL_FN_OID
#define GREATER_EQUAL_FN_OID(datatype) datatype##_GREATER_EQUAL_FN_OID
#define GREATER_THAN_FN_OID(datatype) datatype##_GREATER_THAN_FN_OID

#define DEF_FUNC_CACHE(datatype, func_templ)                                   \
  {datatype##_MAINTAIN_ORDER_FN_OID, datatype, datatype, MAINTAIN_ORDER,       \
   func_templ<CMP_TYPE_MO>},                                                   \
      {datatype##_LESS_THAN_FN_OID, datatype, datatype, SCAN_ORDER_LESS,       \
       func_templ<CMP_TYPE_LT>},                                               \
      {datatype##_LESS_EQUAL_FN_OID, datatype, datatype, SCAN_ORDER_LESSEQUAL, \
       func_templ<CMP_TYPE_LE>},                                               \
      {datatype##_EQUAL_FN_OID, datatype, datatype, SCAN_ORDER_EQUAL,          \
       func_templ<CMP_TYPE_EQ>},                                               \
      {datatype##_GREATER_EQUAL_FN_OID, datatype, datatype,                    \
       SCAN_ORDER_GREATEREQUAL, func_templ<CMP_TYPE_GE>},                      \
  {                                                                            \
    datatype##_GREATER_THAN_FN_OID, datatype, datatype, SCAN_ORDER_GREATER,    \
        func_templ<CMP_TYPE_GT>                                                \
  }

static const DSTORE::FuncCache CDE_FUNC_CACHE_TABLE[] = {

    /* comparator for btree scan */

    DEF_FUNC_CACHE(CDE_BIT_OID, CdeCmpBit),
    DEF_FUNC_CACHE(CDE_BIN_OID, CdeCmpBin),
    DEF_FUNC_CACHE(CDE_CHAR_OID, CdeCmpChar),
    DEF_FUNC_CACHE(CDE_COMPACT_CHAR_OID, CdeCmpCmpctChar),
    DEF_FUNC_CACHE(CDE_VARCHAR1B_OID, CdeCmpVarchar),
    DEF_FUNC_CACHE(CDE_VARCHAR2B_OID, CdeCmpVarchar),
};

const DSTORE::TypeCache INVALID_CDE_TYPE_CACHE = {
    DSTORE_INVALID_OID, "invalid_type", 0, false, '0', DSTORE_INVALID_OID};

static const DSTORE::TypeCache CDE_TYPE_CACHE_TABLE[] = {
    {CDE_BIT_OID, "column_bit", -1, false, 'i', 43},
    {CDE_BIN_OID, "column_bin", -1, false, 'i', 43},
    {CDE_CHAR_OID, "column_char", -3, false, 'i', 43},
    {CDE_COMPACT_CHAR_OID, "column_cmpct_char", -3, false, 'i', 43},
    {CDE_VARCHAR1B_OID, "column_varchar1b", -3, false, 'i', 43},
    {CDE_VARCHAR2B_OID, "column_varchar2b", -3, false, 'i', 43},
};

cache_hash_manager *cache_hash_manager::get_ins() {
  static cache_hash_manager mgr;
  return &mgr;
}

const DSTORE::TypeCache *cache_hash_manager::get_type_cache(
    DSTORE::Oid type_oid) {
  type_oid = CdeConvertDatatypeOid(type_oid);
  if (!CHECK_DATATYPE_OID_VALID(type_oid)) {
    auto it = dstoreTypeCache.find(type_oid);
    if (it != dstoreTypeCache.end()) {
      return it->second;
    }
    return &INVALID_CDE_TYPE_CACHE;
  }
  return type_cache_slot[CDE_DATATYPE_SLOT_ID(type_oid)];
}

const DSTORE::FuncCache *cache_hash_manager::get_func_cache(Oid function_oid) {
  if (!CHECK_PG_FUNC_OID_VALID(function_oid)) {
    return &INVALID_CDE_FUNC_CACHE;
  }
  return func_cache_oid_slot[CDE_PG_FUNC_SLOT_ID(function_oid)];
}

const DSTORE::FuncCache *cache_hash_manager::get_func_cache(
    DSTORE::Oid left_type, DSTORE::Oid right_type, uint16_t proc) {
  CDE_ASSERT(left_type == right_type);
  left_type = CdeConvertDatatypeOid(left_type);
  if (!CHECK_DATATYPE_OID_VALID(left_type) ||
      !CHECK_OPERATOR_STRATEGY_VALID(proc)) {
    return &INVALID_CDE_FUNC_CACHE;
  }
  return func_cache_type_slot[CDE_DATATYPE_SLOT_ID(left_type)]
                             [CDE_OPERATOR_STRATEGY_SLOT_ID(proc)];
}

Oid cache_hash_manager::get_fn_oid(Oid left_type, Oid right_type,
                                   uint16_t proc) {
  return get_func_cache(left_type, right_type, proc)->fnOid;
}

const DSTORE::FuncCache *cache_hash_manager::FindDstoreFuncCache(
    DSTORE::Oid leftType, DSTORE::Oid rightType, uint16_t proc) {
  CacheKey key{leftType, rightType, proc};
  auto it = dstoreFuncCache.find(key);
  return (it != dstoreFuncCache.end()) ? it->second : &INVALID_CDE_FUNC_CACHE;
  ;
}

void cache_hash_manager::init() {
  if (func_cache_is_init) {
    return;
  }
  for (uint32_t i = 0;
       i < sizeof(CDE_TYPE_CACHE_TABLE) / sizeof(CDE_TYPE_CACHE_TABLE[0]);
       i++) {
    CDE_ASSERT(CHECK_DATATYPE_OID_VALID(CDE_TYPE_CACHE_TABLE[i].type));

    int type_slotid = CDE_DATATYPE_SLOT_ID(CDE_TYPE_CACHE_TABLE[i].type);
    CDE_ASSERT(type_cache_slot[type_slotid] == nullptr);
    type_cache_slot[type_slotid] = &CDE_TYPE_CACHE_TABLE[i];
  }

  /* set invalid on empty slot */
  for (uint32_t i = 0; i < sizeof(type_cache_slot) / sizeof(type_cache_slot[0]);
       i++) {
    if (type_cache_slot[i] == nullptr) {
      type_cache_slot[i] = &INVALID_CDE_TYPE_CACHE;
    }
  }

  /* Initialize types from DStore. */
  for (uint32_t i = 0; i < sizeof(DSTORE::TYPE_CACHE_TABLE) /
                               sizeof(DSTORE::TYPE_CACHE_TABLE[0]);
       i++) {
    dstoreTypeCache[DSTORE::TYPE_CACHE_TABLE[i].type] =
        &(DSTORE::TYPE_CACHE_TABLE[i]);
  }

  for (uint32_t i = 0;
       i < sizeof(CDE_FUNC_CACHE_TABLE) / sizeof(CDE_FUNC_CACHE_TABLE[0]);
       i++) {
    CDE_ASSERT(
        (CDE_FUNC_CACHE_TABLE[i].leftTypeOid ==
         CDE_FUNC_CACHE_TABLE[i].rightTypeOid) &&
        CHECK_PG_FUNC_OID_VALID(CDE_FUNC_CACHE_TABLE[i].fnOid) &&
        CHECK_DATATYPE_OID_VALID(CDE_FUNC_CACHE_TABLE[i].leftTypeOid) &&
        CHECK_OPERATOR_STRATEGY_VALID(CDE_FUNC_CACHE_TABLE[i].funcStrategy));

    int fn_oid_slotid = CDE_PG_FUNC_SLOT_ID(CDE_FUNC_CACHE_TABLE[i].fnOid);
    CDE_ASSERT(func_cache_oid_slot[fn_oid_slotid] == nullptr);
    func_cache_oid_slot[fn_oid_slotid] = &CDE_FUNC_CACHE_TABLE[i];

    int type_slotid = CDE_DATATYPE_SLOT_ID(CDE_FUNC_CACHE_TABLE[i].leftTypeOid);
    int strategy_slotid =
        CDE_OPERATOR_STRATEGY_SLOT_ID(CDE_FUNC_CACHE_TABLE[i].funcStrategy);
    CDE_ASSERT(func_cache_type_slot[type_slotid][strategy_slotid] == nullptr);
    func_cache_type_slot[type_slotid][strategy_slotid] =
        &CDE_FUNC_CACHE_TABLE[i];
  }

  /* set invalid on empty slot */
  for (uint32_t i = 0;
       i < sizeof(func_cache_oid_slot) / sizeof(func_cache_oid_slot[0]); i++) {
    if (func_cache_oid_slot[i] == nullptr) {
      func_cache_oid_slot[i] = &INVALID_CDE_FUNC_CACHE;
    }
  }

  for (uint32_t i = 0;
       i < sizeof(func_cache_type_slot) / sizeof(func_cache_type_slot[0]);
       i++) {
    for (uint32_t j = 0; j < sizeof(func_cache_type_slot[0]) /
                                 sizeof(func_cache_type_slot[0][0]);
         j++) {
      if (func_cache_type_slot[i][j] == nullptr) {
        func_cache_type_slot[i][j] = &INVALID_CDE_FUNC_CACHE;
      }
    }
  }

  uint32_t size =
      sizeof(DSTORE::FUNC_CACHE_TABLE) / sizeof(DSTORE::FUNC_CACHE_TABLE[0]);
  for (uint32_t i = 0; i < size; ++i) {
    CacheKey key{DSTORE::FUNC_CACHE_TABLE[i].leftTypeOid,
                 DSTORE::FUNC_CACHE_TABLE[i].rightTypeOid,
                 DSTORE::FUNC_CACHE_TABLE[i].funcStrategy};
    dstoreFuncCache[key] = &DSTORE::FUNC_CACHE_TABLE[i];
  }
  func_cache_is_init = true;
}
} /* namespace CDE */