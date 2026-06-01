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

#ifndef __DD_HELPER_H__
#define __DD_HELPER_H__

#include "sql/dd/types/column.h"
#include "sql/dd_table_share.h"
#include "sql/field.h"

namespace dd {
class Partition;
} /* namespace dd */

/** Postfix for a table name which is being altered. Since during
ALTER TABLE ... PARTITION, new partitions have to be created before
dropping existing partitions, so a postfix is appended to the name
to prevent name conflicts. This is also used for EXCHANGE PARTITION */
constexpr char TMP_POSTFIX[] = "#tmp";

/** Keys for dd column's private data */
enum ddColumnKeys {
  /** Default value when it was added instantly */
  DD_INSTANT_COLUMN_DEFAULT,
  /** Default value is null or not */
  DD_INSTANT_COLUMN_DEFAULT_NULL,
  /** Row version when this column was added instantly */
  DD_INSTANT_VERSION_ADDED,
  /** Row version when this column was dropped instantly */
  DD_INSTANT_VERSION_DROPPED,
  /** Column physical position on row when it was created */
  DD_INSTANT_PHYSICAL_POS,
  /** Sentinel */
  DD_COLUMN_LAST
};

const char *const ddColumnKeyStrings[DD_COLUMN_LAST] = {
    "default", "default_null", "version_added", "version_dropped",
    "physical_pos"};

/** Keys for dd column's private data */
enum ddSpaceKeys {
  /** Tablespace identifier */
  DD_SPACE_ID,
  /** Tablespace max size */
  DD_MAX_SIZE,
  /** Sentinel */
  DD_SPACE__LAST
};

/** DSTORE private key strings for dd::Tablespace. */
const char *const ddSpaceKeyStrings[DD_SPACE__LAST] = {"id", "max_size"};

/**
Get value by key from DD properties.

@tparam valType
@param[in]      prop  DD properties
@param[in]      key   Key to lookup
@param[in]      val   Value of this key

@return true    Key exists and get value success.
@return false   Key not exist or get value failed.
*/
template <typename valType>
bool GetDDPropVal(const dd::Properties &prop, const char *key, valType *val);

/**
Covert dd::Column type to MySQL field type, include realType and type,
return geometry for unsupported type right now.

@param[in]      ddType        Type of DD to covert.
@param[out]     mysqlType     MySQL datatype which is the same as field->type().
@param[out]     mysqlRealType MySQL realtype for some special datatype, like
                              enum, set.
*/
void DDTypeToMysql(dd::enum_column_types ddType, enum_field_types &mysqlType,
                   enum_field_types &mysqlRealType);
/**
Get column charset info, if it is not set, use default charset.

@param[in]      ddCol DD column.

@return charset info point
*/
CHARSET_INFO *GetColumnCharset(const dd::Column *ddCol);

/**
Get column charset no.

@param[in]      ddCol DD column.

@return No of charset
*/
uint32_t GetColumnCharsetNo(const dd::Column *ddCol);

/**
Get column data packed length

@param[in]      ddCol DD column.

@return length of this column data
*/
uint32_t DDGetColumnPackLength(const dd::Column *ddCol);
/**
Get varchar column length bytes

@param[in]      ddCol DD column.

@return bytes to specifiy the column datalen, maybe 1 or 2 right now.
*/
uint32_t GetVarcharColumnLengthBytes(const dd::Column *ddCol);

/**
Check whether column default value is null or not

@param[in]      ddCol DD column.

@return true for default value is null otherwise false for not null
*/
bool DDColDefaultIsNull(const dd::Column *ddCol);

void BuildPartition(const dd::Partition *dd_part, std::string &partition);

void BuildTable(const std::string &schema, const std::string &table,
                const std::string &partition, bool is_tmp, /*bool convert,*/
                std::string &dict_name);

bool IsPartition(const std::string &tableName);
void ParseTableName(const std::string &tableName, bool convert,
                    std::string &schema, std::string &table,
                    std::string &partition, bool &is_tmp);
void PartsAwareFileNameToTableName(const char *from, char *to, size_t toLenght);

#endif  // __DD_HELPER_H__