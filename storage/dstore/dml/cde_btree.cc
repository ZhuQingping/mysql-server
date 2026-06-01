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

#include "dml/cde_btree.h"
#include "common/cde_alloc.h"
#include "common/cde_def.h"
#include "common/cde_typecache.h"
#include "securec.h"

// todo: remove

#include "common/dstore_datatype.h"
#include "common/memory/dstore_mctx.h"

using DSTORE::AttrNumber;
using DSTORE::DEFAULT_COLLATION_OID;
using DSTORE::DSTORE_INVALID_OID;
using DSTORE::FuncCache;
using DSTORE::g_dstoreCurrentMemoryContext;
using DSTORE::IndexBuildInfo;
using DSTORE::IndexInfo;
using DSTORE::SCAN_KEY_ISNULL;
using DSTORE::SCAN_KEY_ROW_MEMBER;
using DSTORE::SCAN_KEY_SEARCHNOTNULL;
using DSTORE::SCAN_KEY_SEARCHNULL;
using DSTORE::SCAN_ORDER_EQUAL;
using DSTORE::ScanKey;
using DSTORE::StorageRelation;
using DSTORE::SysIndexTupDef;

namespace CDE {

void cde_btree_api::SetSingleKeyInfo(DSTORE::ScanKey key_info,
                                     const DSTORE::Form_pg_attribute attr,
                                     Datum argument, bool isNull,
                                     uint32_t index_col_no, uint16_t strategy) {
  const FuncCache *cache =
      CdeGetDstoreFuncCache(attr->atttypid, attr->atttypid, strategy);
  if (cache->fnAddr == nullptr) {
    cache = CdeGetFuncCache(attr->atttypid, attr->atttypid, strategy);
    key_info->skSubtype = DSTORE_INVALID_OID;
  } else {
    key_info->skSubtype = attr->atttypid;
  }
  key_info->skFunc.fnAddr = cache->fnAddr;
  key_info->skFunc.fnOid = cache->fnOid;
  key_info->skFunc.fnNargs = 2;
  key_info->skFunc.fnStrict = true;
  key_info->skFunc.fnRetset = false;
  key_info->skFunc.fn_stats = 0;
  key_info->skFunc.fnExtra =
      DSTORE::FnExtraMake(attr->atttypid, attr->extAttrs);
  key_info->skFunc.fnMcxt = g_dstoreCurrentMemoryContext;
  key_info->skFunc.fnExpr = nullptr;
  key_info->skFlags = SCAN_KEY_ROW_MEMBER;
  if (isNull) {
    key_info->skFlags |= SCAN_KEY_ISNULL;
  }
  key_info->skCollation = attr->attcollation;
  key_info->skAttno = static_cast<AttrNumber>(index_col_no + 1);
  key_info->skStrategy = strategy;
  key_info->skArgument = argument;
}

void cde_btree_api::SetKeyInfos(DSTORE::ScanKey key_info,
                                const DSTORE::Form_pg_attribute *attrs,
                                Datum *argument, bool *isNull,
                                uint32_t index_col_num, uint16_t strategy) {
  for (uint32_t i = 0; i < index_col_num; i++) {
    SetSingleKeyInfo(key_info + i, attrs[i], argument[i], isNull[i], i,
                     strategy);
  }
}

void cde_btree_api::convert_search_mode_to_dstore(
    enum ha_rkey_function find_flag, DSTORE::ScanDirection &dir,
    cde_operatorStrategy &strategy, bool asc) {
  DBUG_TRACE;
  switch (find_flag) {
    case HA_READ_KEY_EXACT:
      /* this does not require the index to be UNIQUE */
      strategy = cde_operatorStrategy::CDE_SCAN_ORDER_EQUAL;
      /* For unique key, the dir can be optimized to NO_MOVEMENT_SCAN_DIRECTION.
      Todo: If a delete operation was previously executed, and then insert
      the same unique key, the corresponding deleted record will not be found
      when executing scan_next. so not use NO_MOVEMENT_SCAN_DIRECTION here,
      Temporarily change it to FORWARD_SCAN_DIRECTION, pending confirmation
      from 2012 dstore team. */
      dir = DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
      break;
    case HA_READ_KEY_OR_NEXT:
      strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL
                     : cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL;
      dir = DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
      break;
    case HA_READ_AFTER_KEY:
      strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_GREATER
                     : cde_operatorStrategy::CDE_SCAN_ORDER_LESS;
      dir = DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
      break;
    case HA_READ_BEFORE_KEY:
      strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_LESS
                     : cde_operatorStrategy::CDE_SCAN_ORDER_GREATER;
      dir = DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
      break;
    /* the prefix may contain an incomplete field, dstore support read
    incomplete fields for index read, different with innodb, choose strategy
    CDE_SCAN_ORDER_EQUAL. */
    case HA_READ_PREFIX_LAST:
      strategy = cde_operatorStrategy::CDE_SCAN_ORDER_EQUAL;
      dir = DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
      break;
    /* same logic with innodb, refer to convert_search_mode_to_innobase */
    case HA_READ_KEY_OR_PREV:
    case HA_READ_PREFIX_LAST_OR_PREV:
      strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL
                     : cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL;
      dir = DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
      break;
    case HA_READ_MBR_CONTAIN:
    case HA_READ_MBR_INTERSECT:
    case HA_READ_MBR_WITHIN:
    case HA_READ_MBR_DISJOINT:
    case HA_READ_MBR_EQUAL:
    case HA_READ_PREFIX:
    case HA_READ_INVALID:
      strategy = CDE_SCAN_ORDER_INVALID;
      dir = DSTORE::ScanDirection::NO_MOVEMENT_SCAN_DIRECTION;
      /* do not use "default:" in order to produce a gcc warning:
      enumeration value '...' not handled in switch
      (if -Wswitch or -Wall is used) */
  }
}

void CdeUpdateSkflagsForNull(enum ha_rkey_function find_flag,
                             uint32_t *skflags) {
  // only search null if is_null is true.
  CDE_ASSERT(skflags &&
             !(*skflags & (SCAN_KEY_SEARCHNOTNULL | SCAN_KEY_SEARCHNULL)));
  if (!(*skflags & SCAN_KEY_ISNULL)) {
    *skflags |= SCAN_KEY_SEARCHNOTNULL;
    return;
  }
  // if is_null and find_flag is HA_READ_KEY_EXACT, HA_READ_KEY_OR_NEXT,
  // HA_READ_KEY_OR_PREV and HA_READ_PREFIX_LAST_OR_PREV means search null.
  if (find_flag == HA_READ_KEY_EXACT || find_flag == HA_READ_KEY_OR_NEXT ||
      find_flag == HA_READ_KEY_OR_PREV ||
      find_flag == HA_READ_PREFIX_LAST_OR_PREV || find_flag == HA_READ_PREFIX ||
      find_flag == HA_READ_PREFIX_LAST) {
    *skflags |= SCAN_KEY_SEARCHNULL;
    return;
  }
  // if not is_null and find_flag is HA_READ_AFTER_KEY or HA_READ_BEFORE_KEY,
  // only search not null.
  if (find_flag == HA_READ_AFTER_KEY || find_flag == HA_READ_BEFORE_KEY) {
    *skflags |= SCAN_KEY_SEARCHNOTNULL;
    return;
  }
  // todo: other conditions have not met.
  CDE_ASSERT(false);
  return;
}

void cde_btree_api::BuildKeyInfos(DSTORE::ScanKey key_infos,
                                  const DSTORE::Form_pg_attribute *attrs,
                                  DSTORE::Datum *argument, bool *isNull,
                                  uint32_t index_col_num,
                                  ha_rkey_function find_flag, bool is_asc) {
  if (!index_col_num) {
    return;
  }

  if (index_col_num > 1) {
    SetKeyInfos(key_infos, attrs, argument, isNull, index_col_num - 1,
                (uint16_t)CDE_SCAN_ORDER_EQUAL);
    for (uint32_t i = 0; i < index_col_num - 1; ++i) {
      // set SCAN_KEY_SEARCHNULL and SCAN_KEY_SEARCHNOTNULL.
      // see CdeUpdateSkflagsForNull.
      CdeUpdateSkflagsForNull(HA_READ_KEY_EXACT, &key_infos[i].skFlags);
    }
  }

  cde_operatorStrategy strategy;
  DSTORE::ScanDirection direction;
  // not matter what is_unique is
  convert_search_mode_to_dstore(find_flag, direction, strategy, is_asc);

  // only set scankey of the last col in key as the strategy find_flag assigned
  SetSingleKeyInfo(key_infos + index_col_num - 1, attrs[index_col_num - 1],
                   argument[index_col_num - 1], isNull[index_col_num - 1],
                   index_col_num - 1, (uint16_t)strategy);

  CdeUpdateSkflagsForNull(find_flag, &key_infos[index_col_num - 1].skFlags);
}

bool cde_btree_api::IsValidNullCmp(const DSTORE::ScanKey keyInfos,
                                   uint32_t prefixCount) {
  DSTORE::ScanKeyData keyInfo = keyInfos[prefixCount - 1];
  /* Less than NULL always false, no need to read data from Dstore. */
  if ((keyInfo.skFlags & SCAN_KEY_ISNULL) &&
      (keyInfo.skFlags & SCAN_KEY_SEARCHNOTNULL)) {
    if (keyInfo.skStrategy == CDE_SCAN_ORDER_LESS) {
      return CDE_FAIL;
    }
  }
  return CDE_SUCC;
}

/**
Update the flag in skFlag indicating whether NULL is required.
The output parameter skFlag is comes from ScanKey.skFlag.
Handle according to the following:

EQUAL：means EXACT EQUAL, include HA_READ_KEY_EXACT and HA_READ_PREFIX_LAST
NOT EQUAL: other findFlags, such as
KEY_OR_NEXT/KEY_OR_PREV/AFTER_KEY/BEFORE_KEY/.....

ps: why HA_READ_PREFIX_LAST belongs to EQUAL refer to
`convert_search_mode_to_dstore()`。

---------------------------------------------------
|             |  key is NULL  |  key not NULL     |
---------------------------------------------------
|  EQUAL      |  NEED NULL    |  NO NEED NULL     |
---------------------------------------------------
|  NOT EQUAL  |  NEED NULL    |  MAYBE NEED NULL  |
---------------------------------------------------

Explain the last one, MAYBE NEED:

create table tt(a int, index(a));
insert into tt values(NULL),(NULL),(1),(2),(3),(4),(7),(10),(8);

Q1: select a from tt where a < 4 or a is null order by a desc;
Q2: select a from tt where a < 4 order by a desc;

For these two queries, the execution plans provided by the SQL layer are:
key_value=4, find_flag=HA_READ_BEFORE_KEY.
The meaning is to scan backward starting from 4 in the B-TREE.
In both cases, the key values are not NULL, and the find_flag belongs to NOT
EQUAL. However, the first query requires NULL value, while the second one does
not. Therefore, in this scenario, skFlags should be set to SCAN_KEY_SEARCHNULL.
Whether the final result actually needs NULL is determined and filtered by the
SQL layer.

In summary, NULL values are clearly not required only when the key value is not
NULL and the findFlag specifies an EQUAL; in all other cases, NULL values are
needed.
*/
void CdeUpdateSkflagsForNullForIndexRead(enum ha_rkey_function findFlag,
                                         uint32_t *skflags) {
  /* only search null if is_null is true. */
  CDE_ASSERT(skflags &&
             !(*skflags & (SCAN_KEY_SEARCHNOTNULL | SCAN_KEY_SEARCHNULL)));

  /* if search key value is not null, and strategy is CDE_SCAN_ORDER_EQUAL after
   findFlag converted, we don't need null values */
  if (!(*skflags & SCAN_KEY_ISNULL) &&
      (findFlag == HA_READ_KEY_EXACT || findFlag == HA_READ_PREFIX_LAST)) {
    *skflags |= SCAN_KEY_SEARCHNOTNULL;
    return;
  }

  /* other cases, NULL values are needed. */
  *skflags |= SCAN_KEY_SEARCHNULL;
  return;
}

/**
Build ScanKeys required for DStore B-tree scanning in index_read() scenario.
The difference between this function and the original BuildKeyInfos() is that
this function sets the corresponding strategy and SEARCH_NULL flag for each
index attribute based on whether it is ascending or descending,
whereas in the original BuildKeyInfos(), only the last attribute
was converted into the corresponding strategy according to the findFlag,
and all preceding attributes were considered EQUAL, which would lead to
incorrect results.

The last column of the composite index needs to be handled separately,
refer to the function ConvertFindFlagToStrategy().
*/
void cde_btree_api::BuildKeyInfosForIndexRead(
    DSTORE::ScanKey keyInfos, const DSTORE::Form_pg_attribute *attrs,
    DSTORE::Datum *argument, bool *isNull, uint32_t indexColNum,
    ha_rkey_function findFlag, const std::vector<bool> &isAsc) {
  CDE_ASSERT(isAsc.size() == indexColNum);

  if (!indexColNum) {
    return;
  }

  for (uint32_t i = 0; i < indexColNum; ++i) {
    bool lastCol = (i == indexColNum - 1);
    cde_operatorStrategy strategy;
    ConvertFindFlagToStrategy(findFlag, isAsc[i], lastCol, strategy);
    SetSingleKeyInfo(keyInfos + i, attrs[i], argument[i], isNull[i], i,
                     (uint16_t)strategy);
    CdeUpdateSkflagsForNullForIndexRead(findFlag, &keyInfos[i].skFlags);
  }
}

/**
Convert the findFlag at the SQL layer into the scan direction at the DSotre
layer. The scan direction applies to the entire index scan, so it only needs to
be converted once.
*/
DSTORE::ScanDirection cde_btree_api::ConvertFindFlagToDir(
    enum ha_rkey_function findFlag) {
  DBUG_TRACE;
  switch (findFlag) {
    case HA_READ_KEY_EXACT:
      /* For unique key, the dir can be optimized to NO_MOVEMENT_SCAN_DIRECTION.
      Todo: If a delete operation was previously executed, and then insert
      the same unique key, the corresponding deleted record will not be found
      when executing scan_next. so not use NO_MOVEMENT_SCAN_DIRECTION here,
      Temporarily change it to FORWARD_SCAN_DIRECTION, pending confirmation
      from 2012 dstore team. */
      return DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
    case HA_READ_KEY_OR_NEXT:
      return DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
    case HA_READ_AFTER_KEY:
      return DSTORE::ScanDirection::FORWARD_SCAN_DIRECTION;
    case HA_READ_BEFORE_KEY:
      return DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
    case HA_READ_PREFIX_LAST:
      return DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
    /* same logic with innodb, refer to convert_search_mode_to_innobase */
    case HA_READ_KEY_OR_PREV:
    case HA_READ_PREFIX_LAST_OR_PREV:
      return DSTORE::ScanDirection::BACKWARD_SCAN_DIRECTION;
    case HA_READ_MBR_CONTAIN:
    case HA_READ_MBR_INTERSECT:
    case HA_READ_MBR_WITHIN:
    case HA_READ_MBR_DISJOINT:
    case HA_READ_MBR_EQUAL:
    case HA_READ_PREFIX:
    case HA_READ_INVALID:
      return DSTORE::ScanDirection::NO_MOVEMENT_SCAN_DIRECTION;
      /* do not use "default:" in order to produce a gcc warning:
      enumeration value '...' not handled in switch
      (if -Wswitch or -Wall is used) */
  }

  CDE_LOG_ERROR("findFlag %d doesn't support", findFlag);
  return DSTORE::ScanDirection::NO_MOVEMENT_SCAN_DIRECTION;
}

/**
Convert the `findFlag` at the SQL layer into a `strategy` at the DSotre layer.
The strategy will be set to each ScanKey.
The input parameter `lastCol` indicate whether the current attribute to be
processed is the last attribute.

The reason why the last attribute needs to be handled separately is as follows:
Generally speaking, `findFlag` being set to `HA_READ_AFTER_KEY` means that the
scan starts from the key and includes values greater than the key (excluding
those equal to the key). This corresponds to a strategy of "GREATER" (not
"GREATEREQUAL"). However, for a composite index, the first attributes are
actually equal to the values in the key, and only the last attribute is greater
than the value, which still satisfies the condition of being "AFTER_KEY." For
example, consider an index (a, b, c) with a key value of (2, 3, 4). If
`findFlag` is set to `HA_READ_AFTER_KEY`, then (2, 3, 5) also meets the
requirement, but for a and b, they are actually equal to the values in the key.
Therefore, in this case, for the same `findFlag`, the strategies converted from
a, b, and c are different: a and b use "GREATEREQUAL" while c uses "GREATER".

The same applies to `findFlag` value HA_READ_BEFORE_KEY.
*/
void cde_btree_api::ConvertFindFlagToStrategy(enum ha_rkey_function findFlag,
                                              bool asc, bool lastCol,
                                              cde_operatorStrategy &strategy) {
  DBUG_TRACE;
  switch (findFlag) {
    case HA_READ_KEY_EXACT:
      /* this does not require the index to be UNIQUE */
      strategy = cde_operatorStrategy::CDE_SCAN_ORDER_EQUAL;
      break;
    case HA_READ_KEY_OR_NEXT:
      strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL
                     : cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL;
      break;
    case HA_READ_AFTER_KEY:
      if (lastCol) {
        strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_GREATER
                       : cde_operatorStrategy::CDE_SCAN_ORDER_LESS;
      } else {
        strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL
                       : cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL;
      }
      break;
    case HA_READ_BEFORE_KEY:
      if (lastCol) {
        strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_LESS
                       : cde_operatorStrategy::CDE_SCAN_ORDER_GREATER;
      } else {
        strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL
                       : cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL;
      }
      break;
    /* the prefix may contain an incomplete field, dstore support read
    incomplete fields for index read, different with innodb, choose strategy
    CDE_SCAN_ORDER_EQUAL. */
    case HA_READ_PREFIX_LAST:
      strategy = cde_operatorStrategy::CDE_SCAN_ORDER_EQUAL;
      break;
    /* same logic with innodb, refer to convert_search_mode_to_innobase */
    case HA_READ_KEY_OR_PREV:
    case HA_READ_PREFIX_LAST_OR_PREV:
      strategy = asc ? cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL
                     : cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL;
      break;
    case HA_READ_MBR_CONTAIN:
    case HA_READ_MBR_INTERSECT:
    case HA_READ_MBR_WITHIN:
    case HA_READ_MBR_DISJOINT:
    case HA_READ_MBR_EQUAL:
    case HA_READ_PREFIX:
    case HA_READ_INVALID:
      strategy = CDE_SCAN_ORDER_INVALID;
      /* do not use "default:" in order to produce a gcc warning:
      enumeration value '...' not handled in switch
      (if -Wswitch or -Wall is used) */
  }
}

} /* namespace CDE */
