/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include "common/cde_def.h"

#include "dd_helper.h"
#include "sql/dd/properties.h"
#include "sql/dd/types/partition.h"
#include "sql/sql_table.h"

void DDTypeToMysql(dd::enum_column_types ddType, enum_field_types &mysqlType,
                   enum_field_types &mysqlRealType) {
  switch (ddType) {
    case dd::enum_column_types::DECIMAL:
      mysqlType = MYSQL_TYPE_DECIMAL;
      mysqlRealType = MYSQL_TYPE_DECIMAL;
      return;

    case dd::enum_column_types::TINY:
      mysqlType = MYSQL_TYPE_TINY;
      mysqlRealType = MYSQL_TYPE_TINY;
      return;

    case dd::enum_column_types::SHORT:
      mysqlType = MYSQL_TYPE_SHORT;
      mysqlRealType = MYSQL_TYPE_SHORT;
      return;

    case dd::enum_column_types::LONG:
      mysqlType = MYSQL_TYPE_LONG;
      mysqlRealType = MYSQL_TYPE_LONG;
      return;

    case dd::enum_column_types::FLOAT:
      mysqlType = MYSQL_TYPE_FLOAT;
      mysqlRealType = MYSQL_TYPE_FLOAT;
      return;

    case dd::enum_column_types::DOUBLE:
      mysqlType = MYSQL_TYPE_DOUBLE;
      mysqlRealType = MYSQL_TYPE_DOUBLE;
      return;

    case dd::enum_column_types::TYPE_NULL:
      mysqlType = MYSQL_TYPE_NULL;
      mysqlRealType = MYSQL_TYPE_NULL;
      return;

    case dd::enum_column_types::TIMESTAMP:
      mysqlType = MYSQL_TYPE_TIMESTAMP;
      mysqlRealType = MYSQL_TYPE_TIMESTAMP;
      return;

    case dd::enum_column_types::LONGLONG:
      mysqlType = MYSQL_TYPE_LONGLONG;
      mysqlRealType = MYSQL_TYPE_LONGLONG;
      return;

    case dd::enum_column_types::INT24:
      mysqlType = MYSQL_TYPE_INT24;
      mysqlRealType = MYSQL_TYPE_INT24;
      return;

    case dd::enum_column_types::DATE:
      mysqlType = MYSQL_TYPE_DATE;
      mysqlRealType = MYSQL_TYPE_NEWDATE;
      return;

    case dd::enum_column_types::TIME:
      mysqlType = MYSQL_TYPE_TIME;
      mysqlRealType = MYSQL_TYPE_TIME;
      return;

    case dd::enum_column_types::DATETIME:
      mysqlType = MYSQL_TYPE_DATETIME;
      mysqlRealType = MYSQL_TYPE_DATETIME;
      return;

    case dd::enum_column_types::YEAR:
      mysqlType = MYSQL_TYPE_YEAR;
      mysqlRealType = MYSQL_TYPE_YEAR;
      return;

    case dd::enum_column_types::NEWDATE:
      mysqlType = MYSQL_TYPE_DATE;
      mysqlRealType = MYSQL_TYPE_NEWDATE;
      return;

    case dd::enum_column_types::VARCHAR:
      mysqlType = MYSQL_TYPE_VARCHAR;
      mysqlRealType = MYSQL_TYPE_VARCHAR;
      return;

    case dd::enum_column_types::BIT:
      mysqlType = MYSQL_TYPE_BIT;
      mysqlRealType = MYSQL_TYPE_BIT;
      return;

    case dd::enum_column_types::TIMESTAMP2:
      mysqlType = MYSQL_TYPE_TIMESTAMP;
      mysqlRealType = MYSQL_TYPE_TIMESTAMP2;
      return;

    case dd::enum_column_types::DATETIME2:
      mysqlType = MYSQL_TYPE_DATETIME;
      mysqlRealType = MYSQL_TYPE_DATETIME2;
      return;

    case dd::enum_column_types::TIME2:
      mysqlType = MYSQL_TYPE_TIME;
      mysqlRealType = MYSQL_TYPE_TIME2;
      return;

    case dd::enum_column_types::NEWDECIMAL:
      mysqlType = MYSQL_TYPE_NEWDECIMAL;
      mysqlRealType = MYSQL_TYPE_NEWDECIMAL;
      return;

    case dd::enum_column_types::ENUM:
      mysqlType = MYSQL_TYPE_STRING;
      mysqlRealType = MYSQL_TYPE_ENUM;
      return;

    case dd::enum_column_types::SET:
      mysqlType = MYSQL_TYPE_STRING;
      mysqlRealType = MYSQL_TYPE_SET;
      return;

    case dd::enum_column_types::STRING:
      mysqlType = MYSQL_TYPE_STRING;
      mysqlRealType = MYSQL_TYPE_STRING;
      return;
    case dd::enum_column_types::BLOB:
      mysqlType = MYSQL_TYPE_BLOB;
      mysqlRealType = MYSQL_TYPE_BLOB;
      return;
    case dd::enum_column_types::TINY_BLOB:
      mysqlType = MYSQL_TYPE_TINY_BLOB;
      mysqlRealType = MYSQL_TYPE_TINY_BLOB;
      return;
    case dd::enum_column_types::MEDIUM_BLOB:
      mysqlType = MYSQL_TYPE_MEDIUM_BLOB;
      mysqlRealType = MYSQL_TYPE_MEDIUM_BLOB;
      return;
    case dd::enum_column_types::LONG_BLOB:
      mysqlType = MYSQL_TYPE_LONG_BLOB;
      mysqlRealType = MYSQL_TYPE_LONG_BLOB;
      return;
    case dd::enum_column_types::JSON:
      mysqlType = MYSQL_TYPE_JSON;
      mysqlRealType = MYSQL_TYPE_JSON;
      return;
    /* Set default type to geometry, since we do not support geometry right
    now, and caller will return error for unsupported type */
    default:
      mysqlType = MYSQL_TYPE_GEOMETRY;
      mysqlRealType = MYSQL_TYPE_GEOMETRY;
      return;
  }
}

CHARSET_INFO *GetColumnCharset(const dd::Column *ddCol) {
  CHARSET_INFO *charset = dd_get_mysql_charset(ddCol->collation_id());
  if (charset == nullptr) {
    charset = default_charset_info;
  }
  return charset;
}

uint32_t GetColumnCharsetNo(const dd::Column *ddCol) {
  return GetColumnCharset(ddCol)->number;
}

// todo: treat_bit_as_char always true???
uint32_t DDGetColumnPackLength(const dd::Column *ddCol) {
  return calc_pack_length(ddCol->type(), ddCol->char_length(),
                          ddCol->elements_count(), true, ddCol->numeric_scale(),
                          ddCol->is_unsigned());
}

uint32_t GetVarcharColumnLengthBytes(const dd::Column *ddCol) {
  CDE_ASSERT(ddCol->type() == dd::enum_column_types::VARCHAR);
  return ddCol->char_length() < 256 ? 1 : 2;
}

bool DDColDefaultIsNull(const dd::Column *ddCol) {
  const dd::Properties &props = ddCol->se_private_data();
  bool isNull;
  bool ret = GetDDPropVal<bool>(
      props, ddColumnKeyStrings[DD_INSTANT_COLUMN_DEFAULT_NULL], &isNull);
  return ret ? isNull : false;
}

template <typename valType>
bool GetDDPropVal(const dd::Properties &pro, const char *key, valType *val) {
  if (pro.exists(key)) {
    pro.get(key, val);
    return true;
  }
  return false;
}

template bool GetDDPropVal<uint32_t>(const dd::Properties &prop,
                                     const char *key, uint32_t *val);
template bool GetDDPropVal<bool>(const dd::Properties &prop, const char *key,
                                 bool *val);

/** Convert string to lower case.
@param[in,out]  name    name to convert */
static void toLower(std::string &name) {
  /* Skip empty string. */
  if (name.empty()) {
    return;
  }
  CDE_ASSERT_DEBUG(name.length() < FN_REFLEN);
  char conv_name[FN_REFLEN];
  auto len = name.copy(&conv_name[0], FN_REFLEN - 1);
  conv_name[len] = '\0';

  my_casedn_str(system_charset_info, &conv_name[0]);
  name.assign(&conv_name[0]);
}

/** Get partition and sub-partition name from DD. We convert the names to
lower case.
@param[in]      dd_part         partition object from DD
@param[in]      lower_case      convert to lower case name
@param[out]     part_name       partition name
@param[out]     sub_name        sub-partition name */
static void getPartFromDd(const dd::Partition *ddPart, bool lowerCase,
                          std::string &partName, std::string &subName) {
  /* Assume sub-partition and get the parent partition. */
  auto subPart = ddPart;
  auto part = subPart->parent();

  /* If parent is null then there is no sub-partition. */
  if (part == nullptr) {
    part = ddPart;
    subPart = nullptr;
  }

  CDE_ASSERT_DEBUG(part->name().length() < FN_REFLEN);

  partName.assign(part->name().c_str());
  /* Convert partition name to lower case. */
  if (lowerCase) {
    toLower(partName);
  }

  subName.clear();
  if (nullptr != subPart) {
    CDE_ASSERT_DEBUG(subPart->name().length() < FN_REFLEN);

    subName.assign(subPart->name().c_str());
    /* Convert sub-partition name to lower case. */
    if (lowerCase) {
      toLower(subName);
    }
  }
}

constexpr char PART_SEPARATOR[] = "#p#";
constexpr char SUB_PART_SEPARATOR[] = "#sp#";
/** Partition separator length excluding terminating NULL */
constexpr size_t PART_SEPARATOR_LEN = sizeof(PART_SEPARATOR) - 1;
constexpr char SCHEMA_SEPARATOR[] = "/";
/** Sub-Partition separator length excluding terminating NULL */
constexpr size_t SUB_PART_SEPARATOR_LEN = sizeof(SUB_PART_SEPARATOR) - 1;
constexpr size_t TMP_POSTFIX_LEN = sizeof(TMP_POSTFIX) - 1;

/** Convert table name to filename
@param[in,out] name - table name */
static void TableToFile(std::string &name) {
  CDE_ASSERT_DEBUG(name.length() < FN_REFLEN);
  char conv_name[FN_REFLEN + 1];

  /* Convert to system character set from file name character set. */
  static_cast<void>(tablename_to_filename(name.c_str(), conv_name, FN_REFLEN));
  name.assign(conv_name);
}

/** Build partition string from partition and sub-partition name
@param[in]      part            partition name
@param[in]      sub_part        sub-partition name
@param[out]     partition       partition string for dictionary table name */
static void BuildPartitionLow(const std::string part,
                              const std::string sub_part,
                              std::string &partition) {
  partition.clear();
  std::string conv_str;

  CDE_ASSERT_DEBUG(false == part.empty());

  /* Get partition separator strings */
  std::string part_sep{PART_SEPARATOR};
  std::string sub_part_sep{SUB_PART_SEPARATOR};

  /* Append separator and partition. */
  partition.append(part_sep);

  conv_str.assign(part);
  TableToFile(conv_str);
  partition.append(conv_str);

  if (sub_part.empty()) {
    return;
  }

  /* Append separator and sub-partition. */
  partition.append(sub_part_sep);

  conv_str.assign(sub_part);
  TableToFile(conv_str);
  partition.append(conv_str);
}

void BuildPartition(const dd::Partition *ddPart, std::string &partition) {
  std::string partName;
  std::string subName;

  /* Extract partition and sub-partition name from DD. */
  getPartFromDd(ddPart, true, partName, subName);

  /* Build partition string after converting names. */
  BuildPartitionLow(partName, subName, partition);
}

static bool CheckPartition(const std::string &dict_name, bool sub_part,
                           size_t &position) {
  std::string part_sep = sub_part ? SUB_PART_SEPARATOR : PART_SEPARATOR;

  /* Check for partition separator string. */
  position = dict_name.find(part_sep);

  return std::string::npos != position;
}

bool IsPartition(const std::string &tableName) {
  size_t position;
  return CheckPartition(tableName, false, position);
}

void FileToTable(std::string &name, bool quiet) {
  CDE_ASSERT_DEBUG(name.length() < FN_REFLEN);
  char conv_name[FN_REFLEN + 1];

  /* Convert to system character set from file name character set. */
  filename_to_tablename(name.c_str(), conv_name, FN_REFLEN, quiet);
  name.assign(conv_name);
}

/** Check for TMP extension name.
@param[in]      dict_name       name from innodb dictionary
@param[out]     position        position of TMP extension in string
@return true, iff TMP extension exists. */
static bool CheckTmp(const std::string &tableName, size_t &position) {
  std::string checkName(tableName);
  position = std::string::npos;

  /* For partitioned or sub partitioned table we need to search the
  temp postfix within the partition, sub-partition string. The temp
  extension looks as follows in different cases.

  1. Non partitioned table : table_name#TMP
  2. Table Partition       : table_name#p#part_name#TMP
  3. Table Sub Partition   : table_name#p#part_name#sp#sub_part_name#TMP

  The issue with checking only #TMP at the end of string is that the partition
  or sub partition could be named as 'TMP' and in following cases we could
  wrongly classify it as name with temporary extension.

  1. Table Partition       : table_name#p#TMP
  3. Table Sub Partition   : table_name#p#part_name#sp#TMP */

  size_t partBegin = std::string::npos;

  if (CheckPartition(tableName, false, partBegin)) {
    partBegin += PART_SEPARATOR_LEN;
    auto partString = checkName.substr(partBegin);
    /* Modify the name to start from the beginning of partition string
    excluding the partition separator '#p#'. */
    checkName.assign(partString);

    size_t subPartBegin = std::string::npos;

    if (CheckPartition(partString, true, subPartBegin)) {
      subPartBegin += SUB_PART_SEPARATOR_LEN;
      auto subPartString = checkName.substr(subPartBegin);
      /* Modify the name to start from the beginning of sub-partition string
      excluding the sub-partition separator '#sp#'. */
      checkName.assign(subPartString);
    }
  }

  auto length = checkName.size();

  if (length < TMP_POSTFIX_LEN) {
    return false;
  }

  auto postfix_pos = length - TMP_POSTFIX_LEN;
  auto ret = checkName.compare(postfix_pos, TMP_POSTFIX_LEN, TMP_POSTFIX);

  if (ret == 0) {
    auto length = tableName.size();
    CDE_ASSERT_DEBUG(length >= TMP_POSTFIX_LEN);

    position = length - TMP_POSTFIX_LEN;
    CDE_ASSERT_DEBUG(0 ==
                     tableName.compare(position, TMP_POSTFIX_LEN, TMP_POSTFIX));
    return true;
  }
  return false;
}

void ParseTableName(const std::string &tableName, bool convert,
                    std::string &schema, std::string &table,
                    std::string &partition, bool &is_tmp) {
  size_t tableBegin = tableName.find(SCHEMA_SEPARATOR);

  /* Check if schema is specified. */
  if (tableBegin == std::string::npos) {
    tableBegin = 0;
    schema.clear();
  } else {
    schema.assign(tableName.substr(0, tableBegin));
    if (convert) {
      /* Perform conversion if requested. Allow invalid conversion
      in schema name. For temp table server passes directory name
      instead of schema name which might contain "." resulting in
      conversion to "?". For temp table this schema name is never used. */
      FileToTable(schema, true);
    }
    ++tableBegin;
  }

  table.assign(tableName.substr(tableBegin));
  partition.clear();

  /* Check if partitioned table. */
  size_t partBegin = std::string::npos;
  bool isPart = CheckPartition(table, false, partBegin);

  /* Check if temp extension. */
  size_t tmpBegin = std::string::npos;
  is_tmp = CheckTmp(table, tmpBegin);

  if (isPart) {
    CDE_ASSERT_DEBUG(partBegin > 0);
    size_t partLen = std::string::npos;

    if (is_tmp && tmpBegin > partBegin) {
      partLen = tmpBegin - partBegin;
    } else if (is_tmp) {
      /* TMP extension must follow partition. */
      CDE_ASSERT_DEBUG(false);
    }
    partition.assign(table.substr(partBegin, partLen));
    table.assign(table.substr(0, partBegin));

  } else if (is_tmp) {
    CDE_ASSERT_DEBUG(tmpBegin > 0);
    table.assign(table.substr(0, tmpBegin));
  }

  /* Perform conversion if requested. */
  if (convert) {
    FileToTable(table, false);
  }
}

void PartsAwareFileNameToTableName(const char *from, char *to,
                                   size_t toLength) {
  const std::string tableNameWithParts(from);
  std::string tableNameWithPartsConv, schema, partition;
  bool isTmp{false};

  ParseTableName(tableNameWithParts, true, schema, tableNameWithPartsConv,
                 partition, isTmp);

  if (false == partition.empty()) {
    tableNameWithPartsConv += partition;
  } else if (isTmp) {
    tableNameWithPartsConv.append(TMP_POSTFIX);
  }

  if (tableNameWithPartsConv.size() >= toLength)
    strncpy_s(to, toLength, tableNameWithPartsConv.c_str(), toLength - 1);
  else {
    strncpy_s(to, toLength, tableNameWithPartsConv.c_str(),
              tableNameWithPartsConv.length());
  }
}

void BuildTable(const std::string &schema, const std::string &table,
                const std::string &partition, bool is_tmp, /*bool convert,*/
                std::string &dict_name) {
  dict_name.clear();
  std::string conv_str;

  /* Check and append schema name. */
  if (!schema.empty()) {
    conv_str.assign(schema);
    dict_name.append(conv_str);
    dict_name.append(SCHEMA_SEPARATOR);
  }

  conv_str.assign(table);
  dict_name.append(conv_str);

  /* Check and assign partition string. Any conversion for partition
  and sub-partition is already done while building partition string. */
  if (!partition.empty()) {
    dict_name.append(partition);
  }

  /* Check and append temporary extension. */
  if (is_tmp) {
    dict_name.append(TMP_POSTFIX);
  }
}