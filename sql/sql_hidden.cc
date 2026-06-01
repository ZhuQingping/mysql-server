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

#include "sql_hidden.h"
#include <string.h>
#include <array>
#include <string>
#include <utility>
#include "sql/recyclebin/recycle_table.h"

#include "mysqld.h"

/* If more hidden tables added, change it to map. */
const static std::array<std::pair<std::string, std::string>, 1> HIDDEN_TABLES =
    {{{"mysql", "dstore_ddl_log"}}};

bool is_hidden_by_rds(const char *db_name, size_t db_name_length,
                      const char *table_name, size_t table_name_length) {
  if (rds_show_hidden_table) {
    return false;
  }
  for (const auto &pair : HIDDEN_TABLES) {
    const std::string &hidden_db = pair.first;
    const std::string &hidden_table = pair.second;
    if (db_name_length == hidden_db.size() &&
        table_name_length == hidden_table.size() &&
        strncmp(db_name, hidden_db.c_str(), hidden_db.size()) == 0 &&
        strncmp(table_name, hidden_table.c_str(), hidden_table.size()) == 0) {
      return true;
    }
  }
  return false;
}
