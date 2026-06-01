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

#include "ha_cde_cond.h"
#include <cstdint>
#include "common/cde_compare_utils.h"
#include "common/cde_errorcode.h"
#include "dml/cde_btree.h"
#include "dml/cde_heap.h"
#include "libbinlogevents/export/binary_log_funcs.h"  // my_*_binary_length()
#include "my_securec.h"
#include "sql/sql_time.h"
#include "sql/tzfile.h"
#include "sql/tztime.h"

namespace CDE {
using DSTORE::Datum;
using DSTORE::ScanKey;
using DSTORE::ScanKeyData;
static constexpr int64_t TM_YEAR_MAX = TM_YEAR_BASE + 255;
static void ProcessDecimalOverUnderFlow(CondField *cond, my_decimal *dec);

/**
 * @brief Parse pushed condtion items.
 *
 * This function will parse the pushed condition items and output the pushed and
 * remaiend conditions.
 *
 * @param[in]  item          The item to be pushed to engine.
 * @param[out] pushedCond    A reference to the pushed condition.
 * @param[out] remainderCond A reference to the remaining condition.
 * @param[in]  parentType    The parent type of the function.
 * @param[out] condList      A list of conditional fields.
 * @param[in]  mem_root      A memory root for allocating memory.
 */
static void ParseConditionItems(Item *item, Item *&pushedCond,
                                Item *&remainderCond,
                                Item_func::Functype parentType,
                                List<CondField> &condList, MemHeap *mem_root);
/**
 * @brief Get the basic table from the given access path.
 *
 * @param[in]  path  the access path
 * @param[in|out]  pointer to the condition
 *
 * @return TABLE* Pointer to the basic table, or nullptr if not found.
 */
TABLE *CdeGetBasicTable(const AccessPath *path, Item **cond, bool valid) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      if (!valid) {
        return nullptr;
      }
      return GetBasicTable(path);
    case AccessPath::FILTER:
      *cond = path->filter().condition;
      return CdeGetBasicTable(path->filter().child, cond, true);
    case AccessPath::AGGREGATE:
      return CdeGetBasicTable(path->aggregate().child, cond);
    case AccessPath::SORT:
      return CdeGetBasicTable(path->sort().child, cond);
    case AccessPath::LIMIT_OFFSET:
      return CdeGetBasicTable(path->limit_offset().child, cond);
    case AccessPath::WINDOW:
      return CdeGetBasicTable(path->window().child, cond);
    case AccessPath::TEMPTABLE_AGGREGATE:
      return CdeGetBasicTable(path->temptable_aggregate().subquery_path, cond);
    case AccessPath::MATERIALIZE: {
      TABLE *table = nullptr;
      uint32_t blocksNum = path->materialize().param->query_blocks.size();
      for (uint32_t i = 0; i < blocksNum; i++) {
        if (path->materialize().param->query_blocks[i].subquery_path->type !=
            AccessPath::TABLE_SCAN) {
          table = CdeGetBasicTable(
              path->materialize().param->query_blocks[i].subquery_path, cond);
          break;
        }
      }  // for
      return table;
    }
    default:
      break;
  }
  return nullptr;
}

/**
 * @brief Create AND conditions from pushedList and remainderList.
 *
 * This function creates AND conditions based on the input parameters.
 *
 * @param[in] cond The condition item
 * @param[in,out] pushedList The list of pushed items
 * @param[in,out] remainderList The list of remaining items
 * @param[out] pushedCond The pushed condition item
 * @param[out] remainderCond The remaining condition item
 * @param[in] mem_root The memory root

 * @return int32_t The result of the operation
 */
static int32_t CreateAndConditions(Item_cond *cond, List<Item> &pushedList,
                                   List<Item> &remainderList, Item *&pushedCond,
                                   Item *&remainderCond, MemHeap *mem_root) {
  if (remainderList.is_empty()) {
    /*Entire cond pushed, no remainder*/
    pushedCond = cond;
    remainderCond = nullptr;
    return CDE_OK;
  }
  if (pushedList.is_empty()) {
    /*Nothing pushed, entire 'cond' is remainder*/
    pushedCond = nullptr;
    remainderCond = cond;
    return CDE_OK;
  }

  /*Condition was partly pushed, with some remainder*/
  if (pushedList.elements == 1) {
    /*Single boolean term pushed, return it*/
    pushedCond = pushedList.head();
  } else {
    /*Construct an 'AND' condition of pushed boolean terms*/
    pushedCond = new (mem_root) Item_cond_and(pushedList);
    if (pushedCond == nullptr) {
      return CDE_ERROR;
    }
  }

  if (remainderList.elements == 1) {
    /*A single boolean term as remainder, return it*/
    remainderCond = remainderList.head();
  } else {
    /*Construct a remainder as an 'AND' condition of the boolean terms*/
    remainderCond = new (mem_root) Item_cond_and(remainderList);
    if (remainderCond == nullptr) {
      return CDE_ERROR;
    }
  }
  return CDE_OK;
}

/**
 * @brief Check if the conditions are supported for a given item and function
 * type.
 *
 * @param[i] term The item to be checked.
 * @param[i] parentType The function type of the parent.
 *
 * @return True if the conditions are supported, false otherwise.
 */
static inline bool IsSupportedConds(Item *term,
                                    Item_func::Functype parentType) {
  if (parentType != Item_func::COND_AND_FUNC) {
    /*
      Unsupported push down condition,
        e.g. select * from tbl where ((col1 < 1) and (col1 > 0)) = 1;
      in which case the parent condition for
        ((col1 < 1) and (col1 > 0)) is not AND/OR operator
    */
    return false;
  }

  Item_func *itemFunc = dynamic_cast<Item_func *>(term);
  if (itemFunc == nullptr) {
    return false;
  }

  Item_func::Functype functype = itemFunc->functype();

  if (functype == Item_func::COND_AND_FUNC) {
    return true;
  }
  return false;
}
/**
 * @brief Check if the function arguments contain a reference item
 *
 * @param[in] args The function arguments to check.
 * @param[in] count The number of arguments.
 *
 * @return true If the function type contain a reference item, false otherwize.
 */
static bool inline HaveRefItem(Item **args, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (args[i]->type() == Item::REF_ITEM) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Checks if a function type is supported.
 *
 * @param[in] itemFunc The function item to check.
 *
 * @return true If the function type is supported, false otherwize.
 */
static inline bool IsFuncTypeSupported(Item_func *itemFunc) {
  switch (itemFunc->functype()) {
    case Item_func::EQ_FUNC:
    case Item_func::EQUAL_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::GE_FUNC:
      if (itemFunc->argument_count() == 2) {
        Item **args = itemFunc->arguments();
        if (args[0]->type() == Item::FIELD_ITEM &&
            args[1]->type() == Item::FIELD_ITEM)
          return false;
        if (args[0]->type() != Item::FIELD_ITEM &&
            args[1]->type() != Item::FIELD_ITEM)
          return false;
        return !HaveRefItem(args, 2);
      }
      return false;
    case Item_func::BETWEEN: {
      Item_func_between *itemBetween = down_cast<Item_func_between *>(itemFunc);
      // Do not suppport 'NOT BETWEEN'
      if (itemBetween->negated) {
        return false;
      }

      if (itemFunc->argument_count() == 3) {
        Item **args = itemFunc->arguments();
        // Only support:
        // 1. val between col1 and col2
        // 2. col1 between val1 and val2
        bool supported = (args[0]->type() != Item::FIELD_ITEM &&
                          args[1]->type() == Item::FIELD_ITEM &&
                          args[2]->type() == Item::FIELD_ITEM) ||
                         (args[0]->type() == Item::FIELD_ITEM &&
                          args[1]->type() != Item::FIELD_ITEM &&
                          args[2]->type() != Item::FIELD_ITEM);
        if (supported) {
          return !HaveRefItem(args, 3);
        }
      }
      return false;
    }
    case Item_func::DATE_FUNC:
    case Item_func::DATETIME_LITERAL:
      return true;

    case Item_func::TRUE_FUNC:
    case Item_func::FALSE_FUNC:
      return true;

    case Item_func::UNKNOWN_FUNC:
      if (itemFunc->data_type() == MYSQL_TYPE_TIME ||
          itemFunc->data_type() == MYSQL_TYPE_TIME2) {
        return true;
      }
      return false;

    default:
      return false;
  }
}

/**
 * @brief Check if the given term supports the specified function type.
 *
 * @param[in] itemFunc The func item to be checked.
 * @param[in] parentType The function type to be checked against.
 *
 * @return True if the term supports the function type, false otherwise.
 */
static bool IsSupportedFuncs(Item_func *itemFunc,
                             Item_func::Functype parentType) {
  if (unlikely(itemFunc == nullptr)) {
    return false;
  }
  if (parentType != Item_func::COND_AND_FUNC) {
    /**
      Unsupported push down condition,
        eg. select * from tbl where ((col1 < 1) and (col1 > 0))= 1;
    */
    return false;
  }

  /**
  push down logic operation only for comparison condition, eg.
  'select * from tbl where (col1 < 1) and (col1 > 0)'
  'select * from tbl where col1 bewtween 0 and 1'
*/
  if (!IsFuncTypeSupported(itemFunc)) {
    return false;
  }

  Item **args = itemFunc->arguments();
  uint16_t size = itemFunc->argument_count();
  for (uint16_t i = 0; i < size; i++) {
    if (args[i]->type() == Item::FUNC_ITEM) {
      Item_func *subItem = dynamic_cast<Item_func *>(args[i]);
      if (!IsFuncTypeSupported(subItem)) {
        return false;
      }
    }
  }
  return true;
}
/**
 * @brief Checks if the given datatype is a supported field type.
 *
 * @param[in] datatype The enum_field_types to check.
 *
 * @return True if the datatype is supported, false otherwise.
 */
static bool IsSupportedDataTypeField(enum_field_types datatype) {
  switch (datatype) {
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY: {
      return false;
    }
    default:
      return true;
  }
}

/**
 * @brief Check if a field is supported based on its type and parent function
 * type.
 *
 * @param term The field to be checked.
 * @param parentType The type of the parent function.
 *
 * @return True if the field is supported, False otherwise.
 */
static bool IsSupportedField(Item *term, Item_func::Functype parentType) {
  if (parentType == Item_func::COND_AND_FUNC ||
      parentType == Item_func::COND_OR_FUNC) {
    return false;
  }

  Item::Type type = term->type();
  if (unlikely(type != Item::FIELD_ITEM)) {
    return false;
  }

  Item_field *itemField = dynamic_cast<Item_field *>(term);
  if (unlikely(itemField == nullptr)) {
    return false;
  }

  // filter virtual generated column
  if (itemField->field->is_virtual_gcol() ||
      !itemField->field->part_of_prefixkey.is_clear_all()) {
    return false;
  }

  // filter unsupport datatype
  if (!IsSupportedDataTypeField(itemField->field->real_type())) {
    return false;
  }

  if (itemField->data_type() == MYSQL_TYPE_STRING) {
    if (itemField->collation.collation->pad_char == '\0') {
      return false;
    }
  }

  if (parentType == Item_func::ISNULL_FUNC ||
      parentType == Item_func::ISNOTNULL_FUNC) {
    return false;
  }
  return true;
}

/**
 * This function processes the given item and updates the pushed and remainder
 * references accordingly.
 *
 * @param[in] term The item to be processed.
 * @param[out] pushed A reference to an item pointer that will be set to
 * nullptr.
 * @param[out] remainder A reference to an item pointer that will be set to
 * the given term.
 */
static inline void RemainAllCondItem(Item *term, Item *&pushed,
                                     Item *&remainder) {
  pushed = nullptr;
  remainder = term;
}

/**
 * This function is used to push all conditional items.
 *
 * @param[in] term The item to be processed.
 * @param[out] pushed The item that has been pushed.
 * @param[out] remainder The remaining items to be processed.
 */
static inline void PushAllCondItem(Item *term, Item *&pushed,
                                   Item *&remainder) {
  pushed = term;
  remainder = nullptr;
}

#define RETURN_IF_ERROR(result)                                        \
  do {                                                                 \
    int32_t res = result;                                              \
    DBUG_EXECUTE_IF("condition_pushdown_injection", res = CDE_ERROR;); \
    if ((res != CDE_OK)) {                                             \
      return CDE_ERROR;                                                \
    }                                                                  \
  } while (0)

#define RETURN_IF_NULL(ptr) \
  do {                      \
    if ((ptr) == nullptr) { \
      return CDE_ERROR;     \
    }                       \
  } while (0)
/**
 * @brief This function parses an expression tree and get the condition value.
 * Currently only supports pushdown of 'AND' conditions. It places
 * pushdown-supported Items into pushedCond for execution plan display,
 * unsupported conditions into remainderCond, and stores the supported
 * conditions into condList.
 *
 * @param[in] item The condition item to be parsed.
 * @param[out] pushedCond The condition pushed down the expression tree.
 * @param[out] remainderCond The remaining condition after pushing.
 * @param[in] parentType The type of the parent function influencing the
 * parsing.
 * @param[out] condList The list of conditions updated with the parsed
 * results.
 * @param[in] mem_root The memory root for memory allocation.
 */
static void ParseCondItem(Item *item, Item *&pushedCond, Item *&remainderCond,
                          Item_func::Functype parentType,
                          List<CondField> &condList, MemHeap *mem_root) {
  if (!IsSupportedConds(item, parentType)) {
    RemainAllCondItem(item, pushedCond, remainderCond);
    return;
  }
  List<Item> pushedList;
  List<Item> remainderList;

  Item_cond *condItem = down_cast<Item_cond *>(item);
  List<Item> *argumentList = condItem->argument_list();

  if (condItem->functype() != Item_func::COND_AND_FUNC) {
    RemainAllCondItem(item, pushedCond, remainderCond);
    return;
  }
  Item *booleanItem;
  List_iterator<Item> iter(*argumentList);

  while ((booleanItem = iter++)) {
    Item *pushed = nullptr;
    Item *remainder = nullptr;
    ParseConditionItems(booleanItem, pushed, remainder, condItem->functype(),
                        condList, mem_root);

    if (pushed != nullptr) pushedList.push_back(pushed);
    if (remainder != nullptr) remainderList.push_back(remainder);
  }

  if (CreateAndConditions(condItem, pushedList, remainderList, pushedCond,
                          remainderCond, mem_root) != CDE_OK) {
    RemainAllCondItem(condItem, pushedCond, remainderCond);
  }
}

/**
 * @brief Parses a field item and updates the conditions accordingly.
 *
 * @param[in] item The field item to be parsed.
 * @param[out] pushedCond The condition that has been pushed down.
 * @param[out] remainderCond The remaining condition after pushing.
 * @param[in] parentType The type of the parent function.
 * @param[in] cond The condition to be parsed.
 * @param[in] mem_root The memory root for allocating memory.
 *
 * @return CDE_OK if parse field item successful,  CDE_ERROR otherwise.
 */
static int32_t ParseFieldItem(Item *item, Item_func::Functype parentType,
                              CondField *cond) {
  if (item->type() != Item::FIELD_ITEM) {
    return CDE_ERROR;
  }
  Item_field *fieldItem = down_cast<Item_field *>(item->real_item());
  DBUG_EXECUTE_IF("condpush_injection_null_field", fieldItem = nullptr;);
  bool nullFiled = fieldItem == nullptr || fieldItem->field == nullptr;
  if (nullFiled) {
    return CDE_ERROR;
  }
  if (!IsSupportedField(fieldItem, parentType)) {
    return CDE_ERROR;
  }

  cond->m_fieldNo = fieldItem->field->field_index();
  cond->m_fieldType = fieldItem->field->real_type();
  cond->m_isUnsigned = fieldItem->field->is_unsigned();
  cond->m_isNull = fieldItem->field->is_null();
  cond->m_collateId = fieldItem->collation.collation->number;
  // Used in item parse, it's safe here to just keep the pointer
  cond->m_csetInfo = fieldItem->collation.collation;
  /*Record precision here, precision in Item_field is smaller than this,
    Will meet problem in decimal compare */
  cond->m_precision = (int32_t)fieldItem->decimal_precision();
  cond->m_decimals = fieldItem->decimals;
  return CDE_OK;
}

/**
 * @brief Checks if the given MYSQL_TIME is zero or not.
 *
 * This function takes a MYSQL_TIME as input and checks if all its fields
 * (hour, minute, second, microsecond) are zero. If all fields are zero,
 * the function returns true, indicating that the time is zero. Otherwise,
 * it returns false.

 * @param time The MYSQL_TIME to be checked.

 * @return True if the time is zero, false otherwise.
 */
static inline bool IsZeroTime(const MYSQL_TIME time) {
  if (!(time.year || time.month || time.day || time.hour || time.minute ||
        time.second || time.second_part)) {
    return true;
  }
  return false;
}

/**
 * @brief Fills a date/time value based on the given datatype and time.
 *
 * This function populates a CondField structure with a date/time value
 * based on the specified datatype and MYSQL_TIME.
 *
 * @param[in] datatype The type of the field to be filled (e.g., DATE,
 * DATETIME).
 * @param[in] time The MYSQL_TIME structure containing the date/time value.
 * @param[out] cond The CondField structure to be populated with the date/time
 * value.
 */
static int32_t FillDateTimeValue(MYSQL_TIME &time, CondField *cond) {
  if (cond->m_decimals > DATETIME_MAX_DECIMALS)
    cond->m_decimals = DATETIME_MAX_DECIMALS;
  if (check_time_mmssff_range(time)) return CDE_ERROR;
  my_time_adjust_frac(&time, cond->m_decimals, true);
  // after truncate non-significant digits, the value may became smaller, need
  // to adjust func type
  if (Item_func::LT_FUNC == cond->m_funcType) {
    cond->m_funcType = Item_func::LE_FUNC;
  } else if (Item_func::GT_FUNC == cond->m_funcType) {
    cond->m_funcType = Item_func::GE_FUNC;
  }

  Datum buffer = 0;
  static_assert(sizeof(Datum) == sizeof(int64_t));
  switch (cond->m_fieldType) {
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2: {
      cond->m_fieldSize = sizeof(int64_t);
      cond->m_fieldValue = 0x0;
      cond->m_resultType = Item_result::DECIMAL_RESULT;
      int64_t lltime = TIME_to_longlong_time_packed(time);
      my_time_packed_to_binary(lltime, (uchar *)(&buffer), cond->m_decimals);
      if (ConvertMysqlTimestampBinaryToDstore(
              (uchar *)(&buffer), my_time_binary_length(cond->m_decimals),
              *(Datum *)(&cond->m_fieldValue))) {
        return CDE_ERROR;
      }
      return CDE_OK;
    }
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      cond->m_fieldSize = sizeof(int64_t);
      cond->m_fieldValue = 0x0;
      cond->m_resultType = Item_result::DECIMAL_RESULT;
      my_date_to_binary(&time, (uchar *)(&buffer));
      if (ConvertMysqlDateBinaryToDstore((uchar *)(&buffer), 3,
                                         *(Datum *)(&cond->m_fieldValue))) {
        return CDE_ERROR;
      }
      return CDE_OK;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2: {
      int64_t lltime = TIME_to_longlong_datetime_packed(time);
      cond->m_fieldSize = sizeof(int64_t);
      cond->m_fieldValue = 0x0;
      cond->m_resultType = Item_result::DECIMAL_RESULT;
      my_datetime_packed_to_binary(lltime, (uchar *)(&buffer),
                                   cond->m_decimals);
      if (ConvertMysqlTimestampBinaryToDstore(
              (uchar *)(&buffer), my_datetime_binary_length(cond->m_decimals),
              *(Datum *)(&cond->m_fieldValue))) {
        return CDE_ERROR;
      }
      return CDE_OK;
    }
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      cond->m_fieldSize = sizeof(int64_t);
      cond->m_fieldValue = 0x0;
      cond->m_resultType = Item_result::DECIMAL_RESULT;
      struct my_timeval tm = {0, 0};
      if (!IsZeroTime(time)) {
        int warnings = 0;
        if (datetime_to_timeval(&time, *current_thd->time_zone(), &tm,
                                &warnings)) {
          return CDE_ERROR;
        }
      }
      my_timestamp_to_binary(&tm, (uchar *)(&buffer), cond->m_decimals);
      if (ConvertMysqlTimestampBinaryToDstore(
              (uchar *)(&buffer), my_timestamp_binary_length(cond->m_decimals),
              *(Datum *)(&cond->m_fieldValue))) {
        return CDE_ERROR;
      }
      return CDE_OK;
    }
    default:
      return CDE_ERROR;
  }
}

/**
 * @brief The key functionality involves checking if user-provided data values
 * are within the valid range of MySQL field types (TINY, SHORT, INT24, LONG,
 * LONGLONG) for both signed and unsigned variants.
 *
 * Dstore compare function only support comparing the same data type，and can
 * not upgrade to a larger data type. So can not compare if the user input
 * data out of the field type range. So can not pushdown the condition if the
 * user input data is out of the field type range.

 * @param[in] fieldType: the data type of the field
 * @param[in] isFieldUnsigned: the data type of the field is unsigned or not
 * @param[in] inputValue: the user input data
 * @param[in] isInputUnsigned: the user input data is unsigned or not

 * @return true if input data is in the field type range, otherwise false
 */
bool IsValueInFieldTypeRange(enum_field_types fieldType, bool isFieldUnsigned,
                             int64_t inputValue, bool isInputUnsigned) {
  int64_t minValue = 0;
  int64_t maxValue = 0;

  switch (fieldType) {
    case MYSQL_TYPE_TINY:
      minValue = isFieldUnsigned ? 0 : INT8_MIN;
      maxValue = isFieldUnsigned ? UINT8_MAX : INT8_MAX;
      break;

    case MYSQL_TYPE_SHORT:
      minValue = isFieldUnsigned ? 0 : INT16_MIN;
      maxValue = isFieldUnsigned ? UINT16_MAX : INT16_MAX;
      break;

    case MYSQL_TYPE_INT24:
      minValue = isFieldUnsigned ? 0 : -0x800000;
      maxValue = isFieldUnsigned ? 0xFFFFFF : 0x7FFFFF;
      break;

    case MYSQL_TYPE_LONG:
      minValue = isFieldUnsigned ? 0 : INT32_MIN;
      maxValue = isFieldUnsigned ? UINT32_MAX : INT32_MAX;
      break;

    case MYSQL_TYPE_LONGLONG:
      minValue = isFieldUnsigned ? 0 : INT64_MIN;
      maxValue = isFieldUnsigned ? UINT64_MAX : INT64_MAX;
      break;
    default:
      return false;
  }

  if (isFieldUnsigned) {
    // Field is unsigned
    if (isInputUnsigned) {
      // Both field and input are unsigned
      return static_cast<uint64_t>(inputValue) <=
             static_cast<uint64_t>(maxValue);
    } else {
      // Field is unsigned, input is signed
      return inputValue >= 0 && static_cast<uint64_t>(inputValue) <=
                                    static_cast<uint64_t>(maxValue);
    }
  } else {
    // Field is signed
    if (isInputUnsigned) {
      // Field is signed, input is unsigned
      return static_cast<int64_t>(inputValue) >= 0 &&
             static_cast<int64_t>(inputValue) <= static_cast<int64_t>(maxValue);
    } else {
      // Both field and input are signed
      return inputValue >= minValue && inputValue <= maxValue;
    }
  }
}

/**
 * @brief Process a integer constant.
 *
 * This function processes an integer constant and updates the condition field
 * accordingly.
 *
 * @param[in] item The item to process.
 * @param[out] cond The condition field to update.
 * @param[in] mem_root The memory root to use for allocations.
 *
 *@return CDE_OK if the integer constant is processed successfully, othewise
 *return CDE_ERROR.
 */
static int32_t ParseIntConstant(Item *item, CondField *cond,
                                MemHeap *mem_root) {
  cond->m_itemType = item->type();
  cond->m_resultType = item->result_type();
  cond->m_isNull = item->null_value;
  cond->m_fieldSize = sizeof(int64_t);

  if (!cond->m_isNull) {
    switch (cond->m_fieldType) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24: {
        int64_t value = item->val_int();
        if (!IsValueInFieldTypeRange(cond->m_fieldType, cond->m_isUnsigned,
                                     value, item->unsigned_flag))
          return CDE_ERROR;
        cond->m_fieldValue =
            cond->m_isUnsigned ? (Datum)((uint64_t)value) : (Datum)value;
        return CDE_OK;
      }
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2: {
        MYSQL_TIME time;
        cond->m_fieldValue = (Datum)mem_root->Alloc(cond->m_fieldSize);
        cond->m_resultType = Item_result::DECIMAL_RESULT;
        int warnings = 0;
        switch (item->data_type()) {
          case MYSQL_TYPE_TIME:
            TIME_from_longlong_time_packed(&time, item->val_int());
            if (time.hour > TIME_MAX_HOUR || time.minute >= TIME_MAX_MINUTE ||
                time.second >= TIME_MAX_SECOND) {
              return CDE_ERROR;  // Invalid time value
            }
            break;
          case MYSQL_TYPE_LONGLONG:
            if (number_to_time(item->val_int(), &time, &warnings)) {
              return CDE_ERROR;
            }
            break;
          default:
            return CDE_ERROR;
        }
        return FillDateTimeValue(time, cond);
      }
      case MYSQL_TYPE_YEAR: {
        int64_t date = item->val_int();
        // the range of year is 1900-2155
        if (date > TM_YEAR_MAX) {
          return CDE_ERROR;
        }
        if (date <= TM_YEAR_BASE && date != 0) {
          if (cond->m_funcType == Item_func::LT_FUNC)
            cond->m_funcType = Item_func::LE_FUNC;
          if (cond->m_funcType == Item_func::GT_FUNC)
            cond->m_funcType = Item_func::GE_FUNC;
        }
        date =
            date <= TM_YEAR_BASE ? (uint8_t)0 : (uint8_t)(date - TM_YEAR_BASE);
        cond->m_fieldValue = (Datum)date;
        return CDE_OK;
      }
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal dec;
        int32_t ret = int2my_decimal(E_DEC_FATAL_ERROR, item->val_int(),
                                     item->unsigned_flag, &dec);
        ProcessDecimalOverUnderFlow(cond, &dec);
        cond->m_fieldSize =
            my_decimal_get_binary_size(cond->m_precision, cond->m_decimals);
        uchar *data = (uchar *)mem_root->Alloc(cond->m_fieldSize);
        my_decimal2binary(E_DEC_FATAL_ERROR, &dec, data, cond->m_precision,
                          cond->m_decimals);
        cond->m_fieldValue = (Datum)data;
        return ret;
      }
      default:
        return CDE_ERROR;
    }
  }
  return CDE_ERROR;
}

/**
 * @brief There is some inconsistency between the comparison functions for char
 * types at the storage layer and the server layer. The storage layer ignores
 * trailing spaces when comparing char types, whereas the server layer does not.
 * This leads to inconsistent results when filtering data at the storage layer
 * versus the server layer. Both pushing down and using indexes will encounter
 * this inconsistency issue.
 * For example, 'a\r' is originally less than 'a ', but after removing the
 * space, 'a\r' becomes greater than 'a'. This results in different query
 * outcomes.
 * Scenarios where errors may occur:
 * We assume col='a\r'
 * 1. const on right
 * col < 'a '
 * col <= 'a '
 * 2. const on left
 * 'a '> col
 * 'a '>=col
 * MySQL Index also has this issue
 * Bug report https://bugs.mysql.com/bug.php?id=118980&thanks=4
 * We simply reject such pushdown, aligning with MySQL's results.

 * @param[in] cond The condition field to check.
 *
 * @return CDE_OK if the condition is valid, CDE_ERROR otherwise.
 */

static inline int32_t AdjustCmpOpForChar(CondField *cond) {
  /*
  Combination Case    |  Display Behavior    | Comparison Behavior
  -----------------------------------------------------------------
  PAD_SPACE + PAD off |Removes padding spaces|Ignores trailing spaces
  PAD_SPACE + PAD  on |Shows padding spaces  |Ignores trailing spaces
  NO_PAD + PAD off    |Removes padding spaces|Does not ignore spaces
  NO_PAD + PAD on     |Shows padding spaces  |Does not ignore spaces
  */
  if (likely(cond->m_csetInfo->pad_attribute == NO_PAD)) {
    if (likely(!(current_thd->variables.sql_mode &
                 MODE_PAD_CHAR_TO_FULL_LENGTH))) {
      /*Without MODE_PAD_CHAR_TO_FULL_LENGTH, only const value has trailing
       * spaces*/
      bool needCheck = cond->m_isConstOnLeft == false &&
                       (cond->m_funcType == Item_func::LT_FUNC ||
                        cond->m_funcType == Item_func::LE_FUNC);
      needCheck = needCheck || (cond->m_isConstOnLeft == true &&
                                (cond->m_funcType == Item_func::GT_FUNC ||
                                 cond->m_funcType == Item_func::GE_FUNC));
      if (needCheck) {
        size_t fixedLen = cond->m_csetInfo->cset->lengthsp(
            cond->m_csetInfo, (const char *)cond->m_fieldValue,
            cond->m_fieldSize);
        if (fixedLen != cond->m_fieldSize) return CDE_ERROR;
      }
    } else {
      /* With MODE_PAD_CHAR_TO_FULL_LENGTH, both sides may have trailing spaces
        'a' = 'a ', after padding, 'a    ' > 'a '
        'a' = 'a        ', after padding, 'a   ' < 'a        '
      */
      if (cond->m_funcType != Item_func::EQ_FUNC &&
          cond->m_funcType != Item_func::EQUAL_FUNC) {
        return CDE_ERROR;
      }
    }
  }
  return CDE_OK;
}

/**
 * @brief Process an string constant.
 *
 * This function parses a string constant and updates the condition field
 * accordingly.
 *
 * @param[in] item The item to process.
 * @param[out] cond The condition field to update.
 * @param[in] mem_root The memory root to use for allocations.
 *
 * @retrun CDE_OK if the string constant is processed successfully, othewise
 * return CDE_ERROR
 */
template <typename T, typename U>
static int32_t ParseStringConstant(Item *item, CondField *cond,
                                   MemHeap *mem_root) {
  cond->m_itemType = item->type();
  cond->m_itemType = item->type();
  cond->m_resultType = item->result_type();
  cond->m_isNull = item->null_value;

  switch (item->data_type()) {
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      U *itemDateTime = dynamic_cast<U *>(item);
      if (unlikely(itemDateTime == nullptr)) return CDE_ERROR;
      MYSQL_TIME time;
      if (itemDateTime->get_date(&time, TIME_FUZZY_DATE)) return CDE_ERROR;
      return FillDateTimeValue(time, cond);
    }

    case MYSQL_TYPE_TIME: {
      U *itemDateTime = dynamic_cast<U *>(item);
      if (unlikely(itemDateTime == nullptr)) return CDE_ERROR;
      MYSQL_TIME time;
      if (itemDateTime->get_time(&time)) return CDE_ERROR;
      if (cond->m_fieldType == MYSQL_TYPE_DATETIME2 ||
          cond->m_fieldType == MYSQL_TYPE_DATETIME) {
        if (time.year == 0 && time.month == 0 && time.day == 0) {
          MYSQL_TIME now;
          THD *thd = current_thd;
          thd->variables.time_zone->gmt_sec_to_TIME(
              &now, thd->query_start_timeval_trunc(6));
          time.year = now.year;
          time.month = now.month;
          time.day = now.day;
        }
      }
      return FillDateTimeValue(time, cond);
    }
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING: {
      String strBuffer;
      T *itemString = dynamic_cast<T *>(item);
      if (unlikely(itemString == nullptr)) return CDE_ERROR;
      String *str = itemString->val_str(&strBuffer);
      RETURN_IF_NULL(str);
      switch (cond->m_fieldType) {
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VAR_STRING: {
          // need to convert column to condtion collation, don't support it.
          if (cond->m_collateId != item->collation.collation->number) {
            return CDE_ERROR;
          }
          cond->m_fieldSize = str->length();
          char *data =
              (char *)strmake_root(mem_root, str->ptr(), cond->m_fieldSize);
          if (data) {
            cond->m_fieldValue = (Datum)data;
          }
          if (cond->m_fieldType == MYSQL_TYPE_STRING)
            return AdjustCmpOpForChar(cond);
          return CDE_OK;
        }
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2: {
          MYSQL_TIME time;
          MYSQL_TIME_STATUS status;

          if (str_to_datetime(str->ptr(), str->length(), &time, TIME_FUZZY_DATE,
                              &status))
            return CDE_ERROR;
          DBUG_EXECUTE_IF("condition_pushdown_invalid_datetime",
                          status.warnings = 2;);
          if (status.warnings != 0) return CDE_ERROR;

          return FillDateTimeValue(time, cond);
        }
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2: {
          MYSQL_TIME time;
          MYSQL_TIME_STATUS status;
          if (str_to_time(str->ptr(), str->length(), &time, &status))
            return CDE_ERROR;
          DBUG_EXECUTE_IF("condition_pushdown_invalid_datetime",
                          status.warnings = 0;);
          if (status.warnings != 0) return CDE_ERROR;

          return FillDateTimeValue(time, cond);
        }
        default:
          return CDE_ERROR;
      }

    } break;
    default:
      return CDE_ERROR;
  }
}

/**
 * @brief When storing float values in MySQL, the MySQL server call function
 * Field_float::store(double nr)function directly converts a double to float and
 * saves it to dstore, which may cause the float value to become larger. During
 * comparisons, the server layer's Arg_comparator::compare_real()converts the
 * float back to double for comparison. This means that when user input is
 * passed as a scan key to dstore, values that were originally greater or
 * smaller than the data stored may become equal after float conversion,
 * requiring corresponding adjustments in the comparison functions to handle
 * these cases properly.

 * Insert:
 *   User input double --> convert to float--> store in storage engine
 * Query:
 *   Get float data from storage --> convert to double --> compare with user
 *   input double

 * @param[in] cond The condition to convert.
 * @param[in] val The real value to check.
 */
static inline void AdjustCmpOpForFloat(CondField *cond, double val) {
  float f = (float)val;
  if (static_cast<double>(f) != val) {
    if (Item_func::LT_FUNC == cond->m_funcType) {
      cond->m_funcType = Item_func::LE_FUNC;
    } else if (Item_func::GT_FUNC == cond->m_funcType) {
      cond->m_funcType = Item_func::GE_FUNC;
    }
  }
}

/**
 * @brief Process a real constant.
 *
 * This function parses a real constant and updates the condition field
 * accordingly.
 *
 * @param[in] item The item to process.
 * @param[out] cond The condition field to update.
 *
 *@return CDE_OK if the real constant is processed successfully, othewise
 *return CDE_ERROR.
 */
template <typename T>
static int32_t ParseRealConstant(Item *item, CondField *cond,
                                 MemHeap *mem_root) {
  cond->m_itemType = item->type();
  cond->m_resultType = item->result_type();
  cond->m_isNull = item->null_value;

  if (!cond->m_isNull) {
    cond->m_fieldSize = sizeof(double);
    double value = item->val_real();
    switch (cond->m_fieldType) {
      case MYSQL_TYPE_FLOAT: {
        cond->m_fieldSize = sizeof(float);
        // Out of range for float
        if (value < -FLT_MAX || value > FLT_MAX) return CDE_ERROR;
        float floatValue = (float)value;
        my_memcpy(&cond->m_fieldValue, sizeof(Datum), &floatValue,
                  sizeof(float));
        AdjustCmpOpForFloat(cond, value);
        return CDE_OK;
      }
      case MYSQL_TYPE_DOUBLE:
        my_memcpy(&cond->m_fieldValue, sizeof(Datum), &value, sizeof(double));
        return CDE_OK;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2: {
        // MySQL's `BETWEEN` was originally designed for
        // numeric types, and time related types need to
        // convert to numeric types for comparison.
        if (cond->m_isBetween) {
          return CDE_ERROR;
        }
        T *itemTyped = static_cast<T *>(item);
        MYSQL_TIME time;
        my_decimal buffer;
        my_decimal *dec = itemTyped->val_decimal(&buffer);
        if (decimal_to_datetime(dec, &time, TIME_FUZZY_DATE)) {
          return CDE_ERROR;
        }
        return FillDateTimeValue(time, cond);
      }

      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal dec;
        int32_t ret =
            double2my_decimal(E_DEC_FATAL_ERROR, item->val_real(), &dec);
        ProcessDecimalOverUnderFlow(cond, &dec);
        cond->m_fieldSize =
            my_decimal_get_binary_size(cond->m_precision, cond->m_decimals);
        uchar *data = (uchar *)mem_root->Alloc(cond->m_fieldSize);
        my_decimal2binary(E_DEC_FATAL_ERROR, &dec, data, cond->m_precision,
                          cond->m_decimals);
        cond->m_fieldValue = (Datum)data;
        return ret;
      }
      default:
        return CDE_ERROR;
    }
  }
  return CDE_ERROR;
}

/**
 * @brief  Handles overflow and underflow conditions for DECIMAL types
 *
 * @note Comparison can only use memcmp() when precision and scale are
 * identical, otherwise field-by-field comparison is required. Since memcmp() is
 * 10x faster, we prefer using it whenever possible.
 *
 * @note For input DECIMAL values that exceed the Decimal(precision, scale)
 * range:
 *       - Replace with Decimal(precision, scale)'s min/max value for equivalent
 * comparison
 *       - This enables safe comparison while maintaining logical correctness
 * @param cond Condition field information, including constraints such as
 * precision and scale
 * @param dec Pointer to the DECIMAL value to be checked
 *
 * Comparison Strategy:
 * 1. If precisions and scales match exactly:
 *    - Use memcmp() for optimal performance (10x faster)
 * 2. If precisions/scales differ:
 *    a) For out-of-range values:
 *       - Values > max → substitute with max value
 *       - Values < min → substitute with min value
 *    b) use my_decimal_round to align the scale
 *
 * Performance Note:
 * - Always attempts memcmp() first when possible
 * - Falls back to field comparison only when necessary
 * - Maintains logical equivalence through range clamping
 */
static void ProcessDecimalOverUnderFlow(CondField *cond, my_decimal *decPtr) {
  // dec is in Decimal(presion, scale) range, no need to round
  if ((decPtr->frac <= cond->m_decimals) &&
      (my_decimal_intg(decPtr) <= (cond->m_precision - cond->m_decimals))) {
    return;
  }
  my_decimal_round(E_DEC_FATAL_ERROR, decPtr, cond->m_decimals, false, decPtr);
  bool sign = decPtr->sign();
  // use Decimal(presion, scale) max value instead if the input overflow or
  // underflow
  if (cond->m_precision - cond->m_decimals < my_decimal_intg(decPtr)) {
    max_my_decimal(decPtr, cond->m_precision, cond->m_decimals);
    decPtr->sign(sign);
  }

  if (cond->m_funcType == Item_func::GT_FUNC) {
    cond->m_funcType = Item_func::GE_FUNC;
  }

  if (cond->m_funcType == Item_func::LT_FUNC) {
    cond->m_funcType = Item_func::LE_FUNC;
  }
}

/**
 * @brief Process a Decimal constant.
 *
 * This function parses a decimal constant and updates the condition field
 * accordingly.
 *
 * @param[in] item The item to process.
 * @param[out] cond The condition field to update.
 * @param[in] mem_root The memory root to use for allocations.
 *
 *@return CDE_OK if the decimal constant is processed successfully, othewise
 *return CDE_ERROR.
 */
template <typename T>
static bool ParseDecimalConstant(Item *item, CondField *cond,
                                 MemHeap *mem_root) {
  cond->m_itemType = item->type();
  cond->m_resultType = item->result_type();
  cond->m_isNull = item->null_value;

  if (!cond->m_isNull) {
    T *itemTyped = static_cast<T *>(item);
    switch (cond->m_fieldType) {
      case MYSQL_TYPE_DOUBLE: {
        cond->m_resultType = Item_result::REAL_RESULT;
        cond->m_fieldSize = sizeof(double);
        double value = item->val_real();
        memcpy_s(&cond->m_fieldValue, sizeof(void *), &value, sizeof(double));
      }
        return CDE_OK;
      case MYSQL_TYPE_FLOAT: {
        cond->m_resultType = Item_result::REAL_RESULT;
        cond->m_fieldSize = sizeof(float);
        double dvalue = item->val_real();
        float value = (float)dvalue;
        memcpy_s(&cond->m_fieldValue, sizeof(void *), &value, sizeof(float));
        AdjustCmpOpForFloat(cond, dvalue);
      }
        return CDE_OK;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        const uint32_t scale = cond->m_decimals;
        my_decimal buffer;
        my_decimal *dec = itemTyped->val_decimal(&buffer);
        RETURN_IF_NULL(dec);
        // Can not modify dec directly, it will be used in
        // server layer compare when dec is a cached_decimal_item
        my_decimal decMod = *dec;
        ProcessDecimalOverUnderFlow(cond, &decMod);
        cond->m_fieldSize =
            my_decimal_get_binary_size(cond->m_precision, scale);
        uchar *data = (uchar *)mem_root->Alloc(cond->m_fieldSize);
        my_decimal2binary(E_DEC_FATAL_ERROR, &decMod, data, cond->m_precision,
                          scale);
        cond->m_fieldValue = (Datum)data;
        return CDE_OK;
      }
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2: {
        // MySQL's `BETWEEN` was originally designed for
        // numeric types, and time related types need to
        // convert to numeric types for comparison.
        if (cond->m_isBetween) {
          return CDE_ERROR;
        }
        MYSQL_TIME time;
        my_decimal buffer;
        my_decimal *dec = itemTyped->val_decimal(&buffer);
        if (decimal_to_datetime(dec, &time, TIME_FUZZY_DATE)) {
          return CDE_ERROR;
        }
        return FillDateTimeValue(time, cond);
      }

      default:
        return CDE_ERROR;
    }
  }
  return CDE_ERROR;
}

/**
 * @brief Parse a cache item
 *
 * This function parse a cache item and updates the conditions
 * accordingly.
 *
 * @param[in] item The cache item to process
 * @param[out] cond The condition field to update
 * @param[in] mem_root The memory root for allocating memory
 * @param[out] pushedCond The condition that was pushed down
 * @param[out] remainderCond The remaining condition after pushing
 *
 *@return CDE_OK if the cache item is processed successfully, otherwise return
 *CDE_ERROR.
 */
int32_t ParseCacheItem(Item *item, CondField *cond, MemHeap *mem_root) {
  Item_cache *itemCache = down_cast<Item_cache *>(item);
  cond->m_itemType = item->type();
  cond->m_resultType = itemCache->result_type();
  cond->m_isNull = !itemCache->has_value();
  int32_t result = CDE_ERROR;

  if (!cond->m_isNull) {
    switch (cond->m_resultType) {
      case Item_result::INT_RESULT:
        result = ParseIntConstant(itemCache, cond, mem_root);
        break;
      case Item_result::REAL_RESULT:
        result = ParseRealConstant<Item_cache_real>(itemCache, cond, mem_root);
        break;
      case Item_result::DECIMAL_RESULT:
        result =
            ParseDecimalConstant<Item_cache_decimal>(itemCache, cond, mem_root);
        break;
      case Item_result::STRING_RESULT:
        result = ParseStringConstant<Item_cache_str, Item_cache_datetime>(
            itemCache, cond, mem_root);
        break;
      default:
        break;
    }
  }

  cond->m_isNull = !itemCache->has_value();
  return result;
}

static inline bool IsStringType(const enum_field_types fieldType) {
  switch (fieldType) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
      return true;
    default:
      return false;
  }
}
/**
 * @brief Process a func constant.
 *
 * This function parses a func constant and updates the condition field
 * currently only support date/datetime/timestamp/time type function constant
 * accordingly.
 *
 * @param[in] item The item to process.
 * @param[out] cond The condition field to update.
 * @param[in] mem_root The memory root to use for allocations.
 *
 * @return CDE_OK if the func constant is processed successfully, othewise
 * return CDE_ERROR.
 */
static int32_t ParseFuncConstant(Item *item, CondField *cond,
                                 MemHeap *mem_root) {
  Item_func *funcItem = down_cast<Item_func *>(item);
  cond->m_itemType = item->type();
  cond->m_resultType = item->result_type();
  cond->m_isNull = item->null_value;
  if (IsStringType(cond->m_fieldType) || !item->const_item()) {
    return CDE_ERROR;
  }
  switch (funcItem->functype()) {
    case Item_func::DATE_FUNC:
    case Item_func::DATETIME_LITERAL: {
      MYSQL_TIME time = {0, 0, 0, 0, 0, 0, 0, 0, MYSQL_TIMESTAMP_ERROR, 0};
      enum_field_types datatype = item->data_type();
      if (datatype == MYSQL_TYPE_YEAR) {
        Item_int *item_int = dynamic_cast<Item_int *>(item);
        time.year = (uint32_t)item_int->value;
        time.month = 1;
        time.day = 1;
        time.hour = 0;
        time.minute = 0;
        time.second = 0;
        time.second_part = 0;
        time.neg = false;
      } else {
        Item_func *item_date_func = dynamic_cast<Item_func *>(item);
        Item_date_literal *item_date_literal =
            (Item_date_literal *)(item_date_func);
        if (item_date_literal->get_date(&time, TIME_FUZZY_DATE)) {
          return CDE_ERROR;
        }
      }
      if (cond->m_decimals < item->decimals &&
          cond->m_funcType == Item_func::LT_FUNC) {
        my_time_adjust_frac(&time, cond->m_decimals, false);
        cond->m_funcType = Item_func::LE_FUNC;
      }
      if ((cond->m_fieldType == MYSQL_TYPE_NEWDATE ||
           cond->m_fieldType == MYSQL_TYPE_DATE) &&
          cond->m_funcType == Item_func::LT_FUNC &&
          (time.hour | time.minute | time.second | time.second_part)) {
        cond->m_funcType = Item_func::LE_FUNC;
      }
      return FillDateTimeValue(time, cond);
    }

    case Item_func::UNKNOWN_FUNC:
      if (funcItem->data_type() == MYSQL_TYPE_TIME ||
          funcItem->data_type() == MYSQL_TYPE_TIME2) {
        if (cond->m_fieldType == MYSQL_TYPE_TIME ||
            cond->m_fieldType == MYSQL_TYPE_TIME2) {
          MYSQL_TIME time;
          Item_func *item_date_func = dynamic_cast<Item_func *>(item);
          Item_time_literal *item_date_literal =
              (Item_time_literal *)(item_date_func);
          if (item_date_literal->get_time(&time)) {
            return CDE_ERROR;
          }
          return FillDateTimeValue(time, cond);
        }
      }
      return CDE_ERROR;

    case Item_func::TRUE_FUNC:
    case Item_func::FALSE_FUNC:
      return ParseIntConstant(item, cond, mem_root);

    default:
      return CDE_ERROR;
  }
}

/**
 * @brief Parse a cache item
 *
 * This function parse a cache item and updates the conditions
 * accordingly.
 *
 * @param[in] item The cache item to process
 * @param[out] cond The condition field to update
 * @param[in] mem_root The memory root for allocating memory
 * @param[out] pushedCond The condition that was pushed down
 * @param[out] remainderCond The remaining condition after pushing
 *
 *@return CDE_OK if the cache item is processed successfully, otherwise return
 *CDE_ERROR.
 */
int32_t ParseParamItem(Item *item, CondField *cond, MemHeap *mem_root) {
  Item_param *paramItem = down_cast<Item_param *>(item);
  cond->m_itemType = item->type();
  cond->m_resultType = paramItem->result_type();
  cond->m_isNull = paramItem->is_null();
  int32_t result = CDE_ERROR;

  if (!cond->m_isNull) {
    switch (cond->m_resultType) {
      case Item_result::INT_RESULT:
        result = ParseIntConstant(paramItem, cond, mem_root);
        break;
      case Item_result::REAL_RESULT:
        result = ParseRealConstant<Item_param>(paramItem, cond, mem_root);
        break;
      case Item_result::DECIMAL_RESULT:
        result = ParseDecimalConstant<Item_param>(paramItem, cond, mem_root);
        break;
      case Item_result::STRING_RESULT:
        result = ParseStringConstant<Item_param, Item_param>(paramItem, cond,
                                                             mem_root);
        break;
      default:
        break;
    }
  }

  cond->m_isNull = paramItem->is_null();
  return result;
}

/**
 * @brief Parse a function parameter item.
 *
 * This function is responsible for parsing a function parameter item and
 * updating the conditions accordingly.
 *
 * @param[in] item The item to be parsed.
 * @param[out] pushedCond The condition to be pushed to engine.
 * @param[out] remainderCond The remaining condition to be kept in server.
 * @param[in] parentType The type of the parent function.
 * @param[in,out] cond The condition field to be updated.
 * @param[in] mem_root The memory root for allocating memory.
 *
 * @return CDE_OK if the item is parsed successfully, otherwise CDE_ERROR.
 */
static int32_t ParseFuncParams(Item *item, Item_func::Functype parentType,
                               CondField *cond, MemHeap *mem_root) {
  switch (item->type()) {
    case Item::FIELD_ITEM:
      return ParseFieldItem(item, parentType, cond);

    case Item::INT_ITEM:
      return ParseIntConstant(item, cond, mem_root);

    case Item::CACHE_ITEM:
      return ParseCacheItem(item, cond, mem_root);

    case Item::STRING_ITEM:
      return ParseStringConstant<Item_string, Item_string>(item, cond,
                                                           mem_root);

    case Item::REAL_ITEM:
      return ParseRealConstant<Item_float>(item, cond, mem_root);

    case Item::DECIMAL_ITEM:
      return ParseDecimalConstant<Item_decimal>(item, cond, mem_root);

    case Item::FUNC_ITEM:
      return ParseFuncConstant(item, cond, mem_root);

    case Item::PARAM_ITEM:
      return ParseParamItem(item, cond, mem_root);

    default:
      return CDE_ERROR;
  }
}

/**
 * @brief Parse single parameter function item.
 *
 * Parse the function item, keep supported func to pushedCond and remain
 * unsupported func to ramainderCond.
 *
 * @param[in] item The function item.
 * @param[out] pushedCond A reference to the pushed condition.
 * @param[out] remainderCond A reference to the remainder condition.
 * @param[in] parentType The parent type of the condition.
 * @param[in,out] condList A list of condition fields.
 * @param[in] mem_root A memory root for allocating memory.
 *
 * @return CDE_OK if the function item is parsed successfully, otherwise
 * CDE_ERROR
 */
static int32_t ParseSingleParamFunc(Item *item, List<CondField> &condList,
                                    MemHeap *mem_root) {
  Item_func *funcItem = down_cast<Item_func *>(item);
  unique_ptr_destroy_only<CondField> condField(new (mem_root) CondField());
  RETURN_IF_NULL(condField);
  condField->m_funcType = funcItem->functype();
  size_t argCount = funcItem->argument_count();
  if (argCount < 2) return CDE_ERROR;

  /** should parse field item first, then parse value item.
   SELECT * FROM t1 WHERE '100000000000000000000002' = value;
   SELECT * FROM t1 WHERE value = '100000000000000000000002';
   have different field item and value item order.
  */
  int32_t result = CDE_OK;
  Item **args = funcItem->arguments();
  if (args[0]->type() == Item::FIELD_ITEM) {
    condField->m_isConstOnLeft = false;
  } else {
    condField->m_isConstOnLeft = true;
  }
  for (size_t i = 0; i < argCount; ++i) {
    Item *argItem = args[i];
    if (argItem->type() == Item::FIELD_ITEM) {  // field item
      result = ParseFieldItem(args[i], funcItem->functype(), condField.get());
      RETURN_IF_ERROR(result);
      break;
    }
  }

  // Parse const item after field item
  for (size_t i = 0; i < argCount; ++i) {
    if (args[i]->type() == Item::FIELD_ITEM) {
      continue;  // skip field item, already parsed
    }
    result = ParseFuncParams(args[i], funcItem->functype(), condField.get(),
                             mem_root);
    RETURN_IF_ERROR(result);
  }
  condList.push_back(condField.release());
  return CDE_OK;
}

/**
 * @brief Parse between function item.
 *
 * Parse the function item, keep supported func to pushedCond and remain
 * unsupported func to ramainderCond.
 *
 * @param[in] item The function item.
 * @param[out] pushedCond A reference to the pushed condition.
 * @param[out] remainderCond A reference to the remainder condition.
 * @param[in] parentType The parent type of the condition.
 * @param[in,out] condList A list of condition fields.
 * @param[in] mem_root A memory root for allocating memory.
 *
 * @return CDE_OK if the function item is parsed successfully, otherwise
 * CDE_ERROR
 */
static int32 ParseBetweenFunc(Item *item, List<CondField> &condList,
                              MemHeap *mem_root) {
  Item_func *funcItem = down_cast<Item_func *>(item);

  /**
   SELECT * FROM t1 WHERE age between 18 and 20;
   SELECT * FROM t1 WHERE 18 between start_age_col and end_age_col;
   have different field item and value item order.
  */
  int32_t result = CDE_OK;
  Item **args = funcItem->arguments();
  RETURN_IF_NULL(args);
  if (args[0]->type() == Item::FIELD_ITEM) {
    // Parameter order is FIELD_ITEM VALUE  VALUE
    unique_ptr_destroy_only<CondField> condMin(new (mem_root) CondField());
    RETURN_IF_NULL(condMin);
    condMin->m_funcType = Item_func::GE_FUNC;
    condMin->m_isBetween = true;

    result = ParseFieldItem(args[0], funcItem->functype(), condMin.get());
    RETURN_IF_ERROR(result);

    result =
        ParseFuncParams(args[1], funcItem->functype(), condMin.get(), mem_root);
    RETURN_IF_ERROR(result);

    unique_ptr_destroy_only<CondField> condMax(new (mem_root)
                                                   CondField(*condMin));
    RETURN_IF_NULL(condMax);
    condMax->m_funcType = Item_func::LE_FUNC;
    condMax->m_isBetween = true;
    result =
        ParseFuncParams(args[2], funcItem->functype(), condMax.get(), mem_root);
    RETURN_IF_ERROR(result);
    condList.push_back(condMin.release());
    condList.push_back(condMax.release());
  } else {
    // Parameter order is VALUE FIELD_ITEM FIELD_ITEM,
    // condMin and condMax may have different type, so need
    // to parse the value twice and convert to corressponding type.
    unique_ptr_destroy_only<CondField> condMax(new (mem_root) CondField());
    RETURN_IF_NULL(condMax);
    condMax->m_funcType = Item_func::LE_FUNC;
    condMax->m_isBetween = true;
    result = ParseFieldItem(args[1], funcItem->functype(), condMax.get());
    RETURN_IF_ERROR(result);

    result =
        ParseFuncParams(args[0], funcItem->functype(), condMax.get(), mem_root);
    RETURN_IF_ERROR(result);

    unique_ptr_destroy_only<CondField> condMin(new (mem_root) CondField());
    RETURN_IF_NULL(condMin);
    condMin->m_funcType = Item_func::GE_FUNC;
    condMax->m_isBetween = true;
    result = ParseFieldItem(args[2], funcItem->functype(), condMin.get());
    RETURN_IF_ERROR(result);

    result =
        ParseFuncParams(args[0], funcItem->functype(), condMin.get(), mem_root);
    RETURN_IF_ERROR(result);
    /*Converting `val BETWEEN col1 AND col2` to `val >= col1 AND val <= col2` is
     * not entirely equivalent. The conditions `>=` and `<=` are completely
     * independent, but `BETWEEN` also requires that `col1` and `col2` be
     * converted to the same type for comparison. However, we do not support
     * type conversion for columns, so in this case, we cannot push down the
     * operation.*/
    if (condMin->m_fieldType != condMax->m_fieldType ||
        condMin->m_collateId != condMax->m_collateId ||
        condMin->m_decimals != condMax->m_decimals) {
      return CDE_ERROR;
    }
    condList.push_back(condMin.release());
    condList.push_back(condMax.release());
  }
  return CDE_OK;
}

/**
 * @brief Parse function item.
 *
 * Parse the function item, keep supported func to pushedCond and remain
 * unsupported func to ramainderCond.
 *
 * @param[in] item The function item.
 * @param[out] pushedCond A reference to the pushed condition.
 * @param[out] remainderCond A reference to the remainder condition.
 * @param[in] parentType The parent type of the condition.
 * @param[in,out] condList A list of condition fields.
 * @param[in] mem_root A memory root for allocating memory.
 */
static void ParseFuncItem(Item *item, Item *&pushedCond, Item *&remainderCond,
                          Item_func::Functype parentType,
                          List<CondField> &condList, MemHeap *mem_root) {
  Item_func *funcItem = down_cast<Item_func *>(item);
  if (!IsSupportedFuncs(funcItem, parentType)) {
    RemainAllCondItem(item, pushedCond, remainderCond);
    return;
  }

  if (item->has_subquery()) {
    RemainAllCondItem(item, pushedCond, remainderCond);
    return;
  }
  int32_t result = CDE_OK;
  if (funcItem->functype() == Item_func::BETWEEN) {
    result = ParseBetweenFunc(item, condList, mem_root);
  } else {
    result = ParseSingleParamFunc(item, condList, mem_root);
  }
  if (result == CDE_OK) {
    PushAllCondItem(item, pushedCond, remainderCond);
  } else {
    RemainAllCondItem(item, pushedCond, remainderCond);
  }
}

/**
MySQL Condition Expression Parsing Framework
The structure you described represents a typical condition expression
parsing mechanism, similar to how MySQL internally processes conditional
expressions. Below is a detailed explanation of how this structure works
along with implementation examples.

COND_ITEM: Logical relation node representing AND/OR operations
          example: AND(age>18, gender='M')
FUNC_ITEM: Condition function noderepresenting comparison operators
           example: =, >, >=, <, <=
FIELD_ITEM: Field node representing column information
            example: age, name, salary

STRING_ITEM/INT_ITEM: Value node representing constant values
             example:'John', 18, 5000


Expression Tree Structure
This structure typically forms an expression tree:

            COND_ITEM(AND)
            /           \
      FUNC_ITEM(>)    FUNC_ITEM(=)
      /      \        /        \
FIELD_ITEM  INT_ITEM FIELD_ITEM STRING_ITEM
(age)       (18)     (gender)   ('M')
*/

/**
 * @brief Parse pushed condtion items.
 *
 * This function will parse the pushed condition items and output the
 * pushed and remaiend conditions.
 *
 * @param[in]  item          The item to be pushed to engine.
 * @param[out] pushedCond    A reference to the pushed condition.
 * @param[out] remainderCond A reference to the remaining condition.
 * @param[in]  parentType    The parent type of the function.
 * @param[out] condList      A list of conditional fields.
 * @param[in]  mem_root      A memory root for allocating memory.
 */
static void ParseConditionItems(Item *item, Item *&pushedCond,
                                Item *&remainderCond,
                                Item_func::Functype parentType,
                                List<CondField> &condList, MemHeap *mem_root) {
  switch (item->type()) {
    case Item::COND_ITEM: {
      ParseCondItem(item, pushedCond, remainderCond, parentType, condList,
                    mem_root);
      break;
    }
    case Item::FUNC_ITEM: {
      ParseFuncItem(item, pushedCond, remainderCond, parentType, condList,
                    mem_root);
      break;
    }
    default:
      RemainAllCondItem(item, pushedCond, remainderCond);
      break;
  }
}

/**
 * @brief Prepares the condition push.

 * This function parses the input condition and saves the pushed and
 remainder conditions.
 *
 * @param[in] cond The condition to be pushed.
 * @param[in] mem_root The memory root to be used for memory allocation.
 */
void CdeCondHandler::PrepareCondPush(Item *cond, MemHeap *mem_root) {
  ParseConditionItems(cond, m_pushedConds, m_remainderConds,
                      Item_func::COND_AND_FUNC, m_condList, mem_root);
}

/**
 * @brief Get the strategy from MySQL function type.
 *
 * This function takes a MySQL function type as input and returns the
 * corresponding scan strategy for the Dstore Engine.
 *
 * @param[in] type The MySQL function type.

 * @return The corresponding Dstore scan strategy.
 */
uint16_t CdeCondHandler::GetStrategyFromMySQLFunc(Item_func::Functype type,
                                                  bool isConstOnLeft) {
  switch (type) {
    case Item_func::EQ_FUNC:
      return cde_operatorStrategy::CDE_SCAN_ORDER_EQUAL;
    case Item_func::EQUAL_FUNC:  // NULL-safe equal
      return cde_operatorStrategy::CDE_SCAN_ORDER_EQUAL;

    case Item_func::LT_FUNC:
      if (unlikely(isConstOnLeft))
        return cde_operatorStrategy::CDE_SCAN_ORDER_GREATER;
      else
        return cde_operatorStrategy::CDE_SCAN_ORDER_LESS;

    case Item_func::LE_FUNC:
      if (unlikely(isConstOnLeft))
        return cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL;
      else
        return cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL;

    case Item_func::GT_FUNC:
      if (unlikely(isConstOnLeft))
        return cde_operatorStrategy::CDE_SCAN_ORDER_LESS;
      else
        return cde_operatorStrategy::CDE_SCAN_ORDER_GREATER;

    case Item_func::GE_FUNC:
      if (unlikely(isConstOnLeft))
        return cde_operatorStrategy::CDE_SCAN_ORDER_LESSEQUAL;
      else
        return cde_operatorStrategy::CDE_SCAN_ORDER_GREATEREQUAL;

    default:
      return CDE_SCAN_ORDER_INVALID;
  }
}

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
int32_t CdeCondHandler::MySQLCondToDstore(Datum &argument, CdeVarlena &var,
                                          const CondField *cond) {
  switch (cond->m_resultType) {
    case Item_result::INT_RESULT:
      argument = cond->m_fieldValue;
      break;

    case Item_result::STRING_RESULT: {
      CdeVarlena *varlena = (CdeVarlena *)&var;
      varlena->len = static_cast<uint16_t>(cond->m_fieldSize);
      varlena->prefix_n_chars = 0;
      varlena->max_len = 0;
      varlena->data = (const unsigned char *)(cond->m_fieldValue);
      argument = static_cast<Datum>((Datum)varlena | DEREF_TAG);
    } break;

    case Item_result::REAL_RESULT: {
      if (cond->m_fieldType == MYSQL_TYPE_FLOAT) {
        my_memcpy(&argument, sizeof(Datum), &cond->m_fieldValue, sizeof(float));
      } else {
        my_memcpy(&argument, sizeof(Datum), &cond->m_fieldValue,
                  sizeof(double));
      }
    } break;
    case Item_result::DECIMAL_RESULT:
      argument = cond->m_fieldValue;
      break;
    default:
      CDE_LOG_INFO("unkown result type %d", cond->m_resultType);
      return CDE_ERROR;
  }
  return CDE_OK;
}

/**
 * @brief Generate scan key from condition.
 *
 * This function generates a dstore scan key based on the given
 * condition.
 *
 * @param[out] scanKey The generated scan key.
 * @param[out] keyNum The number of keys generated.
 * @param[in] dstoreHandler The dstore handler for accessing table
 * metadata.
 * @param[in] table The table for which the scan key is generated.
 * @param[in] mem_root The memory root for allocating memory.
 */
void CdeCondHandler::GenScankeyFromCond(ScanKey &scanKey, uint32_t &keyNum,
                                        const dstore_handler_t *dstoreHandler,
                                        const TABLE *table [[maybe_unused]],
                                        MemHeap *mem_root) {
  uint32_t colNum = m_condList.size();
  uint32_t columnIndex;
  uint16_t strategy = 0;
  keyNum = 0;
  scanKey = nullptr;
  if (likely(colNum > 0)) {
    ScanKey keyInfo = HeapInterface::CreateScanKey(colNum);
    Datum *arguments = static_cast<Datum *>(
        mem_root->Alloc(colNum * (sizeof(Datum) + sizeof(CdeVarlena))));
    if (arguments == nullptr) {
      return;
    }
    CdeVarlena *varlena = (CdeVarlena *)(arguments + colNum);
    uint32_t index = 0;

    CondField *cond;
    List_iterator<CondField> iter(m_condList);
    DSTORE::ScanKey currentScanKey = nullptr;
    while ((cond = iter++)) {
      if (cond->m_isNull) {
        continue;
      }

      columnIndex =
          dstoreHandler->table_handler->m_dictCols[cond->m_fieldNo].m_phyPos;

      DBUG_EXECUTE_IF("condpush_injection_invalid_column",
                      columnIndex = (uint32_t)dstoreHandler->table_handler
                                        ->dstore_relation->attr->natts;);
      if (columnIndex >= (uint32_t)dstoreHandler->table_handler->dstore_relation
                             ->attr->natts) {
        continue;
      }
      currentScanKey = keyInfo + index;
      strategy =
          GetStrategyFromMySQLFunc(cond->m_funcType, cond->m_isConstOnLeft);
      if (MySQLCondToDstore(arguments[index], varlena[index], cond) != CDE_OK)
        continue;

      cde_btree_api::SetSingleKeyInfo(
          currentScanKey,
          dstoreHandler->table_handler->dstore_relation->attr
              ->attrs[columnIndex],
          arguments[index], cond->m_isNull, columnIndex, strategy);
      ++index;
    }

    keyNum = index;
    if (keyNum > 0) {
      scanKey = keyInfo;
    }
  }
}

/**
 * @brief Clean up and release condition resources.
 */
void CdeCondHandler::CleanUpResources() {
  m_condList.destroy_elements();
  m_pushedConds = nullptr;
  m_remainderConds = nullptr;
}
} /* namespace CDE */
