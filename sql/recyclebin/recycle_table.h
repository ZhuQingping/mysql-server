/* Copyright (c) 2023, 2024, Huawei Technologies Co., Ltd. All Rights Reserved.

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

#ifndef SQL_RECYCLE_BIN_RECYCLE_TABLE_INCLUDED
#define SQL_RECYCLE_BIN_RECYCLE_TABLE_INCLUDED

#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "lex_string.h"
#include "m_ctype.h"
#include "sql/error_handler.h"
#include "sql/mdl.h"
#include "sql/recyclebin/recycle.h"
#include "sql/sql_lex.h"
#include "sql/sql_table.h"
#include "sql/table.h"
#include "sql/tztime.h"

struct handlerton;
class THD;
class Foreign_key_parents_invalidator;

namespace dd {
class Table;
}
struct HA_CREATE_INFO;

namespace im {

namespace recycle_bin {

extern LEX_CSTRING RECY_SCHEMA_NAME;

extern LEX_CSTRING ORIGIN_SCHEMA;

extern LEX_CSTRING ORIGIN_TABLE;

/* Seconds before really purging the recycled table. */
extern ulonglong rds_recycle_bin_retention;

/* Whether the db is recycle schema */
inline bool is_recycle_db(const char *str) {
  return !my_strcasecmp(system_charset_info, RECY_SCHEMA_NAME.str, str);
}

/* Recycle operation result */
enum class Recycle_check_result {
  SUPPORTED = 0, /* All tables check success. */
  NOT_SUPPORTED, /* All tables do not support, DROP or TRUNCATE continue. */
  CHECK_ERROR    /* Recycle pre-check failed, report error. */
};

/**
  Fetch all tables should set timezone offset = 0.
*/
class Timestamp_timezone_guard {
 public:
  Timestamp_timezone_guard(THD *thd);

  ~Timestamp_timezone_guard();

 private:
  ::Time_zone *m_tz;
  THD *m_thd;
};

/**
  dbms_recycle.show_tables result
*/
struct Recycle_show_result {
  LEX_STRING schema;
  LEX_STRING table;
  LEX_STRING origin_schema;
  LEX_STRING origin_table;
  ulonglong recycled_time;

 public:
  Recycle_show_result() {
    schema = {nullptr, 0};
    table = {nullptr, 0};
    origin_schema = {nullptr, 0};
    origin_table = {nullptr, 0};
    recycled_time = 0;
  }
};

class Recycle_table_mdl_guard {
  THD *m_thd;
  MDL_request *m_mdl_request;

 public:
  Recycle_table_mdl_guard(THD *thd, MDL_request *mdl_request) {
    m_thd = thd;
    m_mdl_request = mdl_request;
  }

  ~Recycle_table_mdl_guard() {
    if (m_mdl_request->ticket)
      m_thd->mdl_context.release_lock(m_mdl_request->ticket);
  }
};

/**
   Lock the recycle schema

   @param[in]     thd       thread context

   @retval        true      Failure
   @retval        false     Success
*/
bool lock_recycle_schema(THD *thd);

/**
  Retrieve all recycle schema tables.

  @param[in]        thd         thread context
  @param[in]        mem_root    Memory pool
  @param[in/out]    container   show table result container

  @retval           true        Error
  @retval           false       success
*/
bool get_recycle_tables(THD *thd, MEM_ROOT *mem_root,
                        std::vector<Recycle_show_result *> *container);

/**
  Purge the table in recycle_bin

  @param[in]      thd       thread context
  @param[in]      table     Target table name

  @retval         false     success
  @retval         true      failure
*/
bool recycle_purge_table(THD *thd, const char *table);

/**
  drop the table in recycle_bin

  @param[in]      thd       thread context
  @param[in]      tables    Target Table_ref

  @retval         false     success
  @retval         true      failure
*/
bool drop_base_recycle_table(THD *thd, Table_ref *tables);

/**
  Build table_list object to check access conveniently.

  @param[in]      thd       thread context
  @param[in]      db        db string
  @param[in]      db_len    db string length
  @param[in]      table     table name string
  @param[in]      table_len table name string length

  @retval         table_list      single table list object
*/
Table_ref *build_table_list(THD *thd, const char *db, size_t db_len,
                            const char *table, size_t table_len);

bool is_recycled_table(THD *thd, const char *table, bool *exists);

/**
  Check the state whether it's suitable to recycle
  the table which is planned to drop.

  Log some warnings and Continue to drop it if check state fail.

  @param[in]      thd         thread context
  @param[in]      table_list  dropping table
  @param[out]     error       error code

  @retval         false       OK
  @retval         true        Not suitable
*/
bool is_support_logical_drop(THD *thd, Table_ref *table, int &error);

/**
  If rds_recycle_bin_mode is set to a value other than off,
  check whether the __recyclebin__ schema exists. If it does not exist,
  create it. *

  @param[in]      thd       thread context

  @retval         false       success
  @retval         true        failure
*/
bool recreate_recycle_bin_schema(THD *thd);

/**
  When rds_recycle_bin_skip_acl_check is set to ON, the recycle bin
  is disabled regardless of the value of rds_recycle_bin_retention. *

  @param[in]      thd       thread context

  @retval         false       not work
  @retval         true        work
*/
bool is_recycle_bin_work(THD *thd);

/* Recycle operation result */
enum class Recycle_result {
  OK,       /* Recycle success. */
  CONTINUE, /* Recycle pre-check failed, DROP or TRUNCATE continue. */
  ERROR     /* Recycle operation failed, report error */
};

/**
  Check whether the drop table supports the recycle bin.

  @param[in]      thd             thread context
  @param[in]      table           dropping table

  @retval         SUPPORTED       Support recycle bin
  @retval         NOT_SUPPORTED   Not support recycle bin
  @retval         CHECK_ERROR     Report client error
*/
Recycle_check_result check_support_logical_drop(THD *thd, Table_ref *table);

/**
  Recycle a table into __recyclebin__ schema.

  @param[in]      thd               thread context
  @param[in]      table             dropping table
  @param[in,out]  post_ddl_htons    Set of SEs supporting atomic DDL
                                    for which post-DDL hooks needs
                                    to be called.
  @param[in,out]  rb_fk_invalidator Object keeping track of which
                                    dd::Table objects to invalidate.

  @retval         ok              Success
  @retval         error           Report client error
*/
bool recycle_base_table(
    THD *thd, Table_ref *table,
    std::set<handlerton *> *post_ddl_htons = nullptr,
    Foreign_key_parents_invalidator *rb_fk_invalidator = nullptr);

/**
  restore a db into __recyclebin__ schema.

  @param[in]      thd               thread context
  @param[in]      origin_db         origin db
  @param[in]      dst_db            destination db
  @param[out]     restored_tables   restored table num
  @param[in,out]  post_ddl_htons    Set of SEs supporting atomic DDL
                                    for which post-DDL hooks needs
                                    to be called.
  @param[in,out]  rb_fk_invalidator Object keeping track of which
                                    dd::Table objects to invalidate.
  @retval         ok                Success
  @retval         error             Report client error
*/
bool restore_db_inner(THD *thd, const char *origin_db, const char *dst_db,
                      ulong *restored_tables,
                      std::set<handlerton *> *post_ddl_htons,
                      Foreign_key_parents_invalidator *rb_fk_invalidator);

void move_se_attributes(HA_CREATE_INFO *create_info,
                        HA_CREATE_INFO *original_create_info);

/**
  Recycle the table when truncate table.

  @param[in]      thd                 current thd
  @param[in]      path                table path
  @param[in]      table               table list
  @param[in]      create_info         temporary create info
  @param[in]      update_create_info  Whether update create info
  @param[in]      is_temp_table       Whether it's temporary table
  @param[in]      table_def           dd Table object

  @retval         ok                  Success
  @retval         drop_continue       Should continue to truncate table
  @retval         error               Report client error
*/
Recycle_result recycle_truncate_table(THD *thd, const char *path,
                                      Table_ref *table_list,
                                      HA_CREATE_INFO *create_info,
                                      bool update_create_info,
                                      bool is_temp_table, dd::Table *table_def);

/**
  clear table id map when drop __recyclebin__
*/
void recycle_table_id_map_clear(bool is_deinit);

/**
  Parse the original schema name, original table name and table id
  by table name.

  @param[in]      table_name        table name
  @param[out]     id                table id(timestamp)

  @retval         first             Schema name
  @retval         second            Table name
*/
std::pair<std::string, std::string> parse_origin_table_by_table_name(
    const char *table_name, uint64_t *id);

/**
  Copy a string based on specified character sets.

  @param[in]      mem_root          mem_root for space allocation.
  @param[in]      strfrom           input String
  @param[in]      csto              charset for the string data output
*/
const char *recycle_copy_str_with_cs(MEM_ROOT *mem_root, String *strfrom,
                                     const CHARSET_INFO *csto);

class Sql_command_backup {
 public:
  explicit Sql_command_backup(THD *thd, enum enum_sql_command command)
      : m_thd(thd) {
    m_sql_command = m_thd->lex->sql_command;
    m_thd->lex->sql_command = command;
  }

  ~Sql_command_backup() { m_thd->lex->sql_command = m_sql_command; }

 private:
  THD *m_thd;
  enum enum_sql_command m_sql_command;
};

} /* namespace recycle_bin */

} /* namespace im */
#endif
