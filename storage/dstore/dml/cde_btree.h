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

#ifndef __CDE_BTREE_H__
#define __CDE_BTREE_H__

#include "index/dstore_index_struct.h"
#include "index/dstore_scankey.h"
#include "my_base.h"
#include "systable/dstore_relation.h"
namespace CDE {
/**
 *
 * should be consistent with Dstore ,below:
const StrategyNumber SCAN_ORDER_INVALID = 0;
const StrategyNumber SCAN_ORDER_LESS = 1;
const StrategyNumber SCAN_ORDER_LESSEQUAL = 2;
const StrategyNumber SCAN_ORDER_EQUAL = 3;
const StrategyNumber SCAN_ORDER_GREATEREQUAL = 4;
const StrategyNumber SCAN_ORDER_GREATER = 5;
const uint16 MAINTAIN_ORDER = 6;
const uint16 SORT_SUPPORT = 7;
 */
enum cde_operatorStrategy {
  CDE_SCAN_ORDER_INVALID = 0,
  CDE_SCAN_ORDER_LESS = 1,
  CDE_SCAN_ORDER_LESSEQUAL = 2,
  CDE_SCAN_ORDER_EQUAL = 3,
  CDE_SCAN_ORDER_GREATEREQUAL = 4,
  CDE_SCAN_ORDER_GREATER = 5,
  CDE_MAINTAIN_ORDER = 6,
  CDE_SORT_SUPPORT = 7,
  CDE_OPERATOR_STRATEGY_MAX,
};

#define CDE_OPERATOR_STRATEGY_NUM \
  (CDE_OPERATOR_STRATEGY_MAX - CDE_SCAN_ORDER_INVALID - 1)
#define CDE_OPERATOR_STRATEGY_SLOT_ID(strategy) \
  ((strategy)-CDE_SCAN_ORDER_INVALID - 1)
#define CHECK_OPERATOR_STRATEGY_VALID(strategy) \
  (((strategy) > CDE_SCAN_ORDER_INVALID) &&     \
   ((strategy) < CDE_OPERATOR_STRATEGY_MAX))

class cde_btree_api {
 public:
  static void SetSingleKeyInfo(DSTORE::ScanKey key_info,
                               const DSTORE::Form_pg_attribute attr,
                               DSTORE::Datum argument, bool isNull,
                               uint32_t index_col_no, uint16_t strategy);
  static void SetKeyInfos(DSTORE::ScanKey key_info,
                          const DSTORE::Form_pg_attribute *attrs,
                          DSTORE::Datum *argument, bool *isNull,
                          uint32_t index_col_num, uint16_t strategy);
  static void convert_search_mode_to_dstore(enum ha_rkey_function find_flag,
                                            DSTORE::ScanDirection &dir,
                                            cde_operatorStrategy &strategy,
                                            bool asc);
  static void BuildKeyInfos(DSTORE::ScanKey key_infos,
                            const DSTORE::Form_pg_attribute *attrs,
                            DSTORE::Datum *argument, bool *isNull,
                            uint32_t index_col_num, ha_rkey_function find_flag,
                            bool is_asc);

  static DSTORE::ScanDirection ConvertFindFlagToDir(
      enum ha_rkey_function findFlag);
  static void ConvertFindFlagToStrategy(enum ha_rkey_function findFlag,
                                        bool asc, bool last_col,
                                        cde_operatorStrategy &strategy);
  static void BuildKeyInfosForIndexRead(DSTORE::ScanKey keyInfos,
                                        const DSTORE::Form_pg_attribute *attrs,
                                        DSTORE::Datum *argument, bool *isNull,
                                        uint32_t indexColNum,
                                        ha_rkey_function findFlag,
                                        const std::vector<bool> &isAsc);

  /**
  Check null comparison is valid or not. For MySQL B+Tree(ascending or
  descending order), NULL value is always minimum value. So no data less than
  NULL value. But Dstore B+Tree return all not NULL data when get the
  data less than NULL which may leading to unexpected behavior.

  @param[in]  keyInfos scan key information
  @param[in]  prefixCount  prefix key column count

  @return bool Returns CDE_SUCC if null comparison is valid, else return
  CDE_FAIL.
  */
  static bool IsValidNullCmp(const DSTORE::ScanKey keyInfos,
                             uint32_t prefixCount);
};
} /* namespace CDE */
#endif  // __CDE_BTREE_H__
