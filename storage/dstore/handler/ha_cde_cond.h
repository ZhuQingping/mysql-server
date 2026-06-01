/*
  Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

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

#ifndef HA_CDE_COND_H
#define HA_CDE_COND_H

#include <sys/types.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <variant>
#include "common/cde_alloc.h"
#include "common/cde_error.h"
#include "common/cde_session.h"
#include "common/cde_srv.h"
#include "common/cde_typecache.h"
#include "m_ctype.h"
#include "sql/item.h"
#include "sql/item_timefunc.h"
#include "sql/join_optimizer/access_path.h"

namespace CDE {
using Memory::MemHeap;

/**
 * @struct CondField
 * @brief Structure representing a condition field.
 * They are extracted from MySQL Item and used to generate dstore scan key
 */
struct CondField {
  uint32_t m_collateId = 0;        // Collation ID of the string values.
  uint16_t m_fieldNo;              // Field index in the table.
  uint16_t m_fieldSize;            // Size of the field.
  Datum m_fieldValue;              // Pointer to the field value.
  const CHARSET_INFO *m_csetInfo;  // Pointer to charset info.
  Item::Type m_itemType;           // Type of the item.
  Item_result m_resultType;        // Result type of the item.
  enum_field_types m_fieldType = MYSQL_TYPE_INVALID;  // Type of the field.
  Item_func::Functype m_funcType;  // Function type of the item.
  int32_t m_precision;             // Precision of the decimal value.
  int8_t m_decimals;               // Number of decimal places.
  bool m_isNull;                   // IsNull flag of the field.
  bool m_isUnsigned;               // Unsigned flag of the field.
  bool m_isConstOnLeft =
      false;  // Is the constant on the left side of the condition
  bool m_isBetween = false;  // MySQL's `BETWEEN` was originally designed for
                             // numeric types, and many other types are
                             // converted to numeric types for comparison.
};

/**
 * Get table from the type. It's a union, so it is private
 * to force all access to be through the type-checking
 *
 * @param[in]  path  the access path
 * @param[in|out]  pointer to the condition
 * @param[in] flag for cond is valid
 *
 * @return pointer to the table or nullptr
 */
TABLE *CdeGetBasicTable(const AccessPath *path, Item **cond,
                        bool valid = false);

class CdeCondHandler {
 public:
  /**
   * Prepares the condition push.
   * Paser the input condition and save the pushed and remainder conditions.
   *
   * @param[in] cond The condition to be pushed.
   * @param[in] mem_root The memory root to be used for memory allocation.
   */
  void PrepareCondPush(Item *cond, MemHeap *mem_root);

  /**
   * @brief Generate scan key from condition.
   *
   * This function generates a dstore scan key based on the given condition.
   *
   * @param[out] scanKey The generated scan key.
   * @param[out] keyNum The number of keys generated.
   * @param[in] dstoreHandler The dstore handler for accessing table metadata.
   * @param[in] table The table for which the scan key is generated.
   * @param[in] mem_root The memory root for allocating memory.
   */
  void GenScankeyFromCond(ScanKey &scanKey, uint32_t &keyNum,
                          const dstore_handler_t *dstoreHandler,
                          const TABLE *table, MemHeap *mem_root);

  /**
   * @brief Clean up and release condition resources.
   */
  void CleanUpResources();

  /**
   * @brief Get the pushed conditions.
   *
   * @return The pushed conditions.
   */
  Item *&GetPushedConds() { return m_pushedConds; }

  /**
   * Get the remainder conditions.
   *
   * @return The reference to the remainder conditions.
   */
  Item *&GetRemainderConds() { return m_remainderConds; }

  /**
   * @brief Get the condition list.

   * @return The condition list.
   */
  List<CondField> &GetCondList() { return m_condList; }

 private:
  /**
   * @brief Get the strategy from MySQL function type.
   *
   * This function takes a MySQL function type as input and returns the
   * corresponding scan strategy for the Dstore Engine.
   *
   * @param[in] type The MySQL function type.

   * @return The corresponding Dstore scan strategy.
   */
  uint16_t GetStrategyFromMySQLFunc(Item_func::Functype type,
                                    bool isConstOnLeft);

  /**
   * @brief Convert MySQL condition to Dstore scan argument
   *
   * This function converts a MySQL condition to the Dstore scan argument.
   *
   * @param[in,out] argument store dstore format condition
   * @param[in,out] var Variable to store the String value condition
   * @param[in] cond Condition to be converted

   * @retrun convet result, CDE_OK means convert success,
   *         CDE_ERROR means convert failed
   */
  int32_t MySQLCondToDstore(Datum &argument, CdeVarlena &var,
                            const CondField *cond);

  /*conditon will be pushed to storage engine*/
  Item *m_pushedConds = nullptr;
  /*condition still need to be kept in server side*/
  Item *m_remainderConds = nullptr;
  /* The list of condition fields extracted from MySQL Item */
  List<CondField> m_condList;
};
} /* namespace CDE */
#endif /* HA_CDE_COND_H */
