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

#ifndef SQL_SQL_FILTER_TABLE_COMMON_INCLUDED
#define SQL_SQL_FILTER_TABLE_COMMON_INCLUDED

#include <vector>
#include "m_ctype.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sqlcommand.h"
#include "prealloced_array.h"
#include "sql/common/table_common.h"
#include "sql/mysqld.h"  // lower_case_table_names
#include "sql/sql_filter/sql_filter_common.h"
#include "sql/sql_filter/sql_filter_interface.h"
#include "sql/sql_lex.h"

class THD;

using namespace im;

enum class SqlFilterType : int { SELECT = 0, UPDATE, DELETE, INSERT, LAST };

enum mysql_sql_filter_table_field {
  MYSQL_SQL_FILTER_FIELD_RULE_ID = 0,
  MYSQL_SQL_FILTER_FIELD_TYPE,
  MYSQL_SQL_FILTER_FIELD_MAX_CONCURRENCY,
  MYSQL_SQL_FILTER_FIELD_KEYWORDS,
  MYSQL_SQL_FILTER_FIELD_NODE_ID,
  MYSQL_SQL_FILTER_FIELD_COUNT
};

/* Convert string to sql filter tpe */
extern SqlFilterType to_sql_filter_type(const char *str);

extern const char *sql_filter_type_str[];

enum class Sqlfilter_update_error : int {
  UPDATE_OK = 0,
  TYPE_NOT_SAME,
  CACHE_NOT_FIND
};

/* Convert the val to sql filter enabled */
inline SqlFilterType to_sql_filter_type(longlong val) {
  longlong result_type = val - 1;
  if (result_type < static_cast<longlong>(SqlFilterType::SELECT) ||
      result_type >= static_cast<longlong>(SqlFilterType::LAST)) {
    return SqlFilterType::LAST;
  }
  return static_cast<SqlFilterType>(result_type);
}

struct Sql_filter_record : public Conf_record {
  ulonglong m_id;
  SqlFilterType m_type;
  ulonglong m_max_concurrency;
  LEX_CSTRING m_key_str;
  LEX_CSTRING m_node_id;

  std::atomic<ulong> m_cur_concur;
  std::atomic<ulong> m_block_query_num;

 public:
  Sql_filter_record() { reset(); }

  void reset() {
    m_id = 0;
    m_type = SqlFilterType::LAST;
    m_max_concurrency = 0;
    m_key_str = {nullptr, 0};
    m_node_id = {nullptr, 0};
    m_cur_concur = 0;
    m_block_query_num = 0;
  }
  virtual bool check_valid(const char **msg) const override;
  bool check_record_valid(const char **msg) const;

  virtual ulonglong get_id() const override { return m_id; }
  virtual void set_id(ulonglong value) override { m_id = value; }
  virtual bool check_active() const override { return true; }
};

void trim_str(std::string &str);

#endif
