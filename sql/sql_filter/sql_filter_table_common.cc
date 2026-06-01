/* Copyright (c) 2025, Huawei and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/sql_filter/sql_filter_table_common.h"
#include <boost/algorithm/string.hpp>

#include "errmsg.h"
#include "my_sys.h"
#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"
#include "mysys_err.h"
#include "sql/mysqld.h"
#include "sql/sql_filter/sql_filter_cache.h"
#include "sql/sql_parse.h"  // check_string_char_length

// max concurrency number which is same with DAS
const int SQL_MAX_CONCURRENCY_NUM = 1000000000;
/*
  max string length for single rule's key_str.
  (A rule contains key_str and concurrency).
*/
const int SQL_FILTER_KEY_STR_LEN = 1024;
/** max key words for each sql filter rule. */
const int MAX_KEY_WORDS = 128;

/* Currently we only support {SELECT, UPDATE, DELETE, INSERT} type's SQL filter
 */
const int NUM_SQL_FILTERS = 4;

const LEX_CSTRING FILTER_HEADER[NUM_SQL_FILTERS] = {
    {STRING_WITH_LEN("SELECT")},
    {STRING_WITH_LEN("UPDATE")},
    {STRING_WITH_LEN("DELETE")},
    {STRING_WITH_LEN("INSERT")}};

enum sql_filter_err : int {
  NONE = 0,
  TYPE_INVALID,
  MAX_CONCURRENCY_INVALID,
  KEYSTR_LEN_INVALID,
  KEYSTR_HEADER_INVALID,
  KEYSTR_COUNT_INVALID,
  KEYSTR_KEY_INVALID,
  NODE_ID_LEN_INVALID,
  SQL_FILTER_ERR_COUNT
};

const char *sql_filter_error_msg[] = {
    "unknow.",                                                    // NONE
    "the type is not any of select, update, delete, or insert.",  // TYPE_INVALID
    "the max_concurrency is out of range, the valid range is [0, "
    "1000000000].",  // MAX_CONCURRENCY_INVALID
    "the key_str's char length is out of range or collation is incompatible, "
    "the valid range is [1, 1024].",  // KEYSTR_LEN_INVALID
    "the header of key_str is not same with sql filter type.",  // KEYSTR_HEADER_INVALID
    "the number of keywords is out of range, the valid range is [2, "
    "128].",                            // KEYSTR_COUNT_INVALID
    "rule contain illegal empty key.",  // KEYSTR_KEY_INVALID
    "Parameter node_id's char length is invalid or collation is incompatible, "
    "range[0, 36]."  // NODE_ID_LEN_INVALID
};

/*
  Triming leading/trailing whitespace of the string.

  @param[in]  str         the input string
*/
void trim_str(std::string &str) {
  str.erase(0, str.find_first_not_of(" \n\t\r\f\v"));
  str.erase(str.find_last_not_of(" \n\t\r\f\v") + 1);
}

/*
  check whether if the header of sql filter rule is legal, ie the header of
  each rule must be consistent with the filter type. For example,
  rds_sql_select_filter = "SELECT~t1,1" is legal as the header of rule is
  SELECT; while rds_sql_select_filter = "UPDATE~t1,1" is illegal.

  @param[out] errmsg the error message when setting sql_filter
  @retval true if it is legal, otherwise return false.
*/
bool is_valid_filter_header(const std::string &str, SqlFilterType type,
                            std::string &errmsg) {
  std::string rule(str);
  trim_str(rule);
  errmsg = "header of rule inconsistent with filter type";
  auto str_len = rule.size();
  if (str_len > 0 && str_len < FILTER_HEADER[static_cast<int>(type)].length) {
    return false;
  }

  auto upperHeader =
      rule.substr(0, FILTER_HEADER[static_cast<int>(type)].length);
  std::transform(upperHeader.begin(), upperHeader.end(), upperHeader.begin(),
                 ::toupper);

  if (upperHeader == FILTER_HEADER[static_cast<int>(type)].str) {
    errmsg.clear();
    return true;
  }
  return false;
}

bool Sql_filter_record::check_record_valid(const char **msg) const {
  // type
  if (m_type == SqlFilterType::LAST) {
    *msg = sql_filter_error_msg[sql_filter_err::TYPE_INVALID];
    return false;
  }

  // max_concurrency, which is same with DAS
  if (m_max_concurrency > SQL_MAX_CONCURRENCY_NUM) {
    *msg = sql_filter_error_msg[sql_filter_err::MAX_CONCURRENCY_INVALID];
    return false;
  }

  // m_key_str char length <= 1024
  if (m_key_str.str == nullptr || m_key_str.length <= 0 ||
      check_string_char_length(m_key_str, "", SQL_FILTER_KEY_STR_LEN,
                               system_charset_info, true)) {
    *msg = sql_filter_error_msg[sql_filter_err::KEYSTR_LEN_INVALID];
    return false;
  } else {
    std::string errmsg;
    // header of keystr is one of insert/update/insert/delete
    if (!is_valid_filter_header(m_key_str.str, m_type, errmsg)) {
      *msg = sql_filter_error_msg[sql_filter_err::KEYSTR_HEADER_INVALID];
      return false;
    }

    std::vector<std::string> key_vec;
    boost::split(key_vec, m_key_str.str, boost::is_any_of("~"));
    // keystr keyword num : [2, 128]
    if (key_vec.size() < 2 || key_vec.size() > MAX_KEY_WORDS) {
      *msg = sql_filter_error_msg[sql_filter_err::KEYSTR_COUNT_INVALID];
      return false;
    }

    for (auto &key : key_vec) {
      trim_str(key);
      // the "" is an illegal key
      if (key.size() == 0) {
        *msg = sql_filter_error_msg[sql_filter_err::KEYSTR_KEY_INVALID];
        return false;
      }
    }
  }

  // node_id:  char length <= 36
  if (check_string_char_length(m_node_id, "", MAX_NODE_ID, system_charset_info,
                               true)) {
    *msg = sql_filter_error_msg[sql_filter_err::NODE_ID_LEN_INVALID];
    return false;
  }

  return true;
}

bool Sql_filter_record::check_valid([[maybe_unused]] const char **msg) const {
  return true;
}
