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

#include "sql/recyclebin/recycle_table.h"
#include "sql/recyclebin/recycle.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <set>
#include "m_string.h"
#include "my_securec.h"
#include "mysql/components/services/log_builtins.h"
#include "securec.h"
#include "sql/auth/auth_acls.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/collection.h"
#include "sql/dd/dd_schema.h"
#include "sql/dd/dd_table.h"
#include "sql/dd/impl/types/check_constraint_impl.h"
#include "sql/dd/impl/types/column_impl.h"
#include "sql/dd/impl/types/foreign_key_impl.h"
#include "sql/dd/impl/types/index_impl.h"
#include "sql/dd/impl/types/partition_impl.h"
#include "sql/dd/impl/types/table_impl.h"
#include "sql/dd/impl/types/trigger_impl.h"
#include "sql/dd/impl/utils.h"  // dd::my_time_t_to_ull_datetime()
#include "sql/dd/object_id.h"
#include "sql/dd/properties.h"
#include "sql/dd/types/check_constraint.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/foreign_key.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/table.h"
#include "sql/dd/types/trigger.h"
#include "sql/handler.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_rename.h"
#include "sql/sql_table.h"
#include "sql/strfunc.h"
#include "sql/table.h"
#include "sql/thd_raii.h"

namespace im {

namespace recycle_bin {

LEX_CSTRING RECY_SCHEMA_NAME = {C_STRING_WITH_LEN("__recyclebin__")};

LEX_CSTRING ORIGIN_SCHEMA = {C_STRING_WITH_LEN("__origin_schema__")};

LEX_CSTRING ORIGIN_TABLE = {C_STRING_WITH_LEN("__origin_table__")};

/* Seconds before really purging the recycled table. */
ulonglong rds_recycle_bin_retention = 3 * 24 * 60 * 60;

/* The maximum characters of the table name is 64,
   64 - "@" - "_FFFFFFFF" = 54 */
static const size_t recyclebin_name_char_length = 54;
static const size_t recyclebin_invalid_table_id_str_len = 8;
static const size_t recyclebin_invalid_table_id = 0xFFFFFFFF;
/* The maximum length of the table name is 251 bytes,
   251 - len("@") - len("@FFFFFFFF") = 233 */
static const size_t recyclebin_schema_table_name_len = 233;
/* If the generated table name already exists,
   wait for 1s and generate a table id again.*/
static const uint32_t table_name_generate_retry_time = 1;
static const uint32_t table_name_generate_sleep_microseconds = 1000000;
static const std::string recyclebin_table_name_delimiter = "@";
static const uint32_t recyclebin_name_delimiter_count = 2;

/**
  Log warning message when recycle table

  @param[in]      db          Dropping table db
  @param[in]      table_name  Dropping table name
  @param[in]      reason      Why failed
*/
static void log_recycle_warning(const char *db, const char *table_name,
                                const char *reason) {
  std::stringstream ss;
  ss << "Fail to recycle table " << db << "." << table_name << " since "
     << reason;
  LogErr(WARNING_LEVEL, ER_RECYCLE_BIN, ss.str().c_str());
}

/* Constructor */
Timestamp_timezone_guard::Timestamp_timezone_guard(THD *thd) : m_thd(thd) {
  m_tz = m_thd->variables.time_zone;
  m_thd->variables.time_zone = my_tz_OFFSET0;
}

Timestamp_timezone_guard::~Timestamp_timezone_guard() {
  m_thd->variables.time_zone = m_tz;
}

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
                            const char *table, size_t table_len) {
  Table_ref *table_list = new (thd->mem_root) Table_ref;
  if (!table_list) return nullptr;
  table_list->db = thd->mem_strdup(db);
  table_list->db_length = db_len;
  table_list->table_name = thd->mem_strdup(table);
  table_list->table_name_length = table_len;
  table_list->next_local = table_list->next_global = nullptr;

  return table_list;
}

/**
  Whether the table was recycled into recycle-bin from normal table.

  Attention:
    Here didn't hold the table level MDL lock, so it's loose checking
*/
bool is_recycled_table(THD *thd, const char *table, bool *exists) {
  const dd::Schema *sch = nullptr;
  const char *schema = nullptr;
  DBUG_ENTER("is_recycled_table");
  dd::Schema_MDL_locker mdl_handler(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  schema = RECY_SCHEMA_NAME.str;
  if (mdl_handler.ensure_locked(schema) ||
      thd->dd_client()->acquire(schema, &sch)) {
    LogErr(WARNING_LEVEL, ER_RECYCLE_BIN, "acquire recycle schema");
    DBUG_RETURN(true);
  }
  if (sch == nullptr) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "recycle schema didn't exist");
    DBUG_RETURN(true);
  }

  const dd::Table *table_def = nullptr;

  if (thd->dd_client()->acquire(schema, table, &table_def)) DBUG_RETURN(true);

  if (table_def == nullptr) {
    DBUG_EXECUTE_IF("recyclebin_slave_purge_table_not_exist",
                    thd->slave_thread = true;);
    *exists = false;
    std::stringstream ss;
    ss << table << " didn't exist";
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0), ss.str().c_str());
    DBUG_RETURN(true);
  }
  if (!table_def->options().exists(ORIGIN_SCHEMA.str) ||
      !table_def->options().exists(ORIGIN_TABLE.str)) {
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_RECYBIN_NOT_RECYCLED_TABLE,
                        ER_THD(thd, ER_RECYBIN_NOT_RECYCLED_TABLE), table);
  }

  DBUG_RETURN(false);
}

/**
  Retrieve all recycle schema tables.

  @param[in]        thd         thread context
  @param[in]        mem_root    Memory pool
  @param[in/out]    container   show table result container

  @retval           true        Error
  @retval           false       success
*/
bool get_recycle_tables(THD *thd, MEM_ROOT *mem_root,
                        std::vector<Recycle_show_result *> *container) {
  const dd::Schema *sch = nullptr;
  const char *schema = nullptr;
  std::vector<dd::String_type> sch_tables;
  DBUG_ENTER("get_recycle_tables");
  assert(container);

  /* Should set timezone offset = 0 or m_last_altered will be confused */
  Timestamp_timezone_guard timezone_guard(thd);

  dd::Schema_MDL_locker mdl_handler(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  schema = RECY_SCHEMA_NAME.str;
  if (mdl_handler.ensure_locked(schema) ||
      thd->dd_client()->acquire(schema, &sch) || sch == nullptr ||
      thd->dd_client()->fetch_schema_table_names_not_hidden_by_se(
          sch, &sch_tables)) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "fetch tables info from schema __recyclebin__ failed");
    DBUG_RETURN(true);
  }

  for (const dd::String_type &table_name : sch_tables) {
    MDL_request mdl_request;
    Recycle_table_mdl_guard table_mdl(thd, &mdl_request);
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, RECY_SCHEMA_NAME.str,
                     table_name.c_str(), MDL_SHARED, MDL_EXPLICIT);
    bool ret = thd->mdl_context.acquire_lock(&mdl_request,
                                             thd->variables.lock_wait_timeout);
    if (ret) DBUG_RETURN(true);

    dd::cache::Dictionary_client::Auto_releaser table_releaser(
        thd->dd_client());
    const dd::Table *table = nullptr;
    ret = thd->dd_client()->acquire(RECY_SCHEMA_NAME.str, table_name.c_str(),
                                    &table);
    if (ret) DBUG_RETURN(true);

    /* Do not purge tables that are not moved to __recyclebin__
       by the recycle bin statement. */
    if (table == nullptr || !table->options().exists(ORIGIN_SCHEMA.str) ||
        !table->options().exists(ORIGIN_TABLE.str)) {
      continue;
    }
    // check for the select access to the original table
    LEX_STRING origin_db;
    LEX_STRING origin_table;
    table->options().get(ORIGIN_SCHEMA.str, &origin_db, thd->mem_root);
    table->options().get(ORIGIN_TABLE.str, &origin_table, thd->mem_root);
    Table_ref *new_table_list =
        build_table_list(thd, RECY_SCHEMA_NAME.str, RECY_SCHEMA_NAME.length,
                         table->name().c_str(), table->name().length());
    // Delete tenant permission check judgment
    if (thd->system_thread == SYSTEM_THREAD_RECYCLE_SCHEDULER ||
        !check_table_access(thd, SELECT_ACL, new_table_list, false, 1, true)) {
      Recycle_show_result *show_table = new (mem_root) Recycle_show_result();

      if (show_table == nullptr) DBUG_RETURN(true);

      table->options().get(ORIGIN_SCHEMA.str, &(show_table->origin_schema),
                           mem_root);
      table->options().get(ORIGIN_TABLE.str, &(show_table->origin_table),
                           mem_root);

      lex_string_strmake(mem_root, &(show_table->schema), sch->name().c_str(),
                         sch->name().length());
      lex_string_strmake(mem_root, &(show_table->table), table->name().c_str(),
                         table->name().length());
      /* Recycled time come from last alter (recycle operation) */
      show_table->recycled_time = table->last_altered(true);

      container->push_back(show_table);
    }
  }
  DBUG_RETURN(false);
}

/**
   Lock the recycle schema

   @param[in]     thd       thread context

   @retval        true      Failure
   @retval        false     Success
*/
bool lock_recycle_schema(THD *thd) {
  DBUG_ENTER("lock_recycle_schema");
  if (!thd->mdl_context.owns_equal_or_stronger_lock(
          MDL_key::SCHEMA, RECY_SCHEMA_NAME.str, "", MDL_INTENTION_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, RECY_SCHEMA_NAME.str, "",
                     MDL_INTENTION_EXCLUSIVE, MDL_TRANSACTION);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}

inline static bool have_secondary_engine(const dd::Table *dd_table) {
  return dd_table->options().exists("secondary_engine");
}

static inline bool is_recyclebin_support_db(const char *db) {
  if (db == nullptr) return false;

  if (is_perfschema_db(db) || is_mysql_db(db) || is_infoschema_db(db) ||
      is_sys_db(db)) {
    return false;
  }

  return true;
}

/**
   Check whether the table name and db name support the recycle bin.
   There are two check rules:
     1. The total length of db name and table name must be
        less than the value of recyclebin_name_char_length.
     2. The db name and table name and table name cannot contain "@".

   Recycle bin table name use "@" to separate the table name and db name.
   If the user's db name or table name of contains "@", the following
   situations may occur:
     table 1  table name: t@t, db name: db --> recycle bin table name: t@t@db_0
     table 2  table name: t, db name: t@db --> recycle bin table name: t@t@db_0

   To prevent tables with different db names and table names from generating
   the same recycle bin table name, recycle bin does not support the table
   name or db name containing "@"

   @param[in]     cs        character set
   @param[in]     db        db name
   @param[in]     table     table name

   @retval        true      Failure
   @retval        false     Success
*/
bool recycle_check_table_name(const CHARSET_INFO *cs, const char *db,
                              const char *table) {
  size_t character_num = 0;
  character_num += my_numchars_mb(cs, db, db + strlen(db));
  character_num += my_numchars_mb(cs, table, table + strlen(table));

  char tbbuff[FN_REFLEN], dbbuff[FN_REFLEN];

  size_t name_str_len = tablename_to_filename(db, tbbuff, sizeof(tbbuff)) +
                        tablename_to_filename(table, dbbuff, sizeof(dbbuff));

  if (character_num > recyclebin_name_char_length ||
      name_str_len > recyclebin_schema_table_name_len || strstr(db, "@") ||
      strstr(table, "@"))
    return true;
  return false;
}

bool is_support_logical_drop(THD *thd, Table_ref *table, int &error) {
  DBUG_ENTER("is_support_logical_drop");
  char path[FN_REFLEN + 1];
  handlerton *hton;
  handler *file;

  /* Error already */
  if (thd->is_error()) DBUG_RETURN(false);

  /* Recycle state within current thread context */
  if (!thd->recycle_state->is_recycle()) DBUG_RETURN(false);

  /* Check whether the total length of the table name and db name exceeds the
     limit or contains "@"*/
  if (recycle_check_table_name(system_charset_info, table->db,
                               table->table_name))
    DBUG_RETURN(false);

  /* 1. Check in Server layer. In the following case, recyclebin is not
   *    supported
   *
   * 1.1. If dropping table is in recycle schema, deny to recycle it
   * 1.2. Deny to recycle the table which restored from recycle
   * 1.3. The hidden table is not supported.
   * 1.4. foreign_key_checks is OFF.
   * 1.5. The table that is droped by trigger is not supported.
   * 1.6. The table that in system databases(mysql, information_schema,
   *      performance_schema, sys)  is not supported.
   * 1.7. The table with second storage engine is not supported.
   * 1.8. Deny to recycle and restore tables in lock table mode
   */

  if (my_strcasecmp(system_charset_info, table->db, RECY_SCHEMA_NAME.str) ==
      0) {
    log_recycle_warning(table->db, table->table_name, "schema is recycle bin.");
    DBUG_RETURN(false);
  }

  /*If the lock mode is supported, special processing needs to be performed in
  the rename scenario because the commit operation is not performed in the
  rename process.So consider temporarily blocking first.*/
  if (thd->locked_tables_mode) {
    log_recycle_warning(table->db, table->table_name, "within lock mode.");
    DBUG_RETURN(false);
  }

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Table *table_def = nullptr;
  if (thd->dd_client()->acquire(table->db, table->table_name, &table_def))
    DBUG_RETURN(false);

  DBUG_EXECUTE_IF("recyclebin_check_acquire_table_failed",
                  { table_def = nullptr; });
  /* Maybe SUPER_ACL user operated recycle schema directly */
  if (table_def == nullptr || table_def->options().exists(ORIGIN_SCHEMA.str) ||
      table_def->options().exists(ORIGIN_TABLE.str)) {
    log_recycle_warning(table->db, table->table_name,
                        "table doesn't exist or has been recycled.");
    DBUG_RETURN(false);
  }

  if (table_def->hidden() == dd::Abstract_table::HT_HIDDEN_SE) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table->db, table->table_name);
    DBUG_RETURN(false);
  }

  if (dd::table_storage_engine(thd, table_def, &hton)) DBUG_RETURN(false);

  (void)build_table_filename(path, sizeof(path) - 1, table->db,
                             table->table_name, "",
                             table->internal_tmp_table ? FN_IS_TMP : 0);

  bool no_fk_checks = static_cast<bool>(thd->variables.option_bits &
                                        OPTION_NO_FOREIGN_KEY_CHECKS);
  if (hton == nullptr ||
      (hton->db_type != DB_TYPE_INNODB && hton->db_type != DB_TYPE_DSTORE) ||
      no_fk_checks || thd->in_sp_trigger ||
      !is_recyclebin_support_db(table->db) || have_secondary_engine(table_def))
    DBUG_RETURN(false);

  /*
   * 2. Check in innodb engine layer.In the following case, recyclebin is not
   *    supported
   *
   * 2.1 The table with fts is not supported.
   * 2.2 The table with data directory is not supported.
   * 2.3 The table with discard tablespace is not supported.
   * */
  if (!(file =
            get_new_handler((TABLE_SHARE *)nullptr,
                            table_def->partition_type() != dd::Table::PT_NONE,
                            thd->mem_root, hton)) ||
      DBUG_EVALUATE_IF("recyclebin_check_support_recyclebin", 1, 0)) {
    error = true;
    DBUG_RETURN(false);
  }
  char tmp_path[FN_REFLEN + 1];
  const char *all_path = get_canonical_filename(file, path, tmp_path);
  bool is_supported = false;

  // Call the standard recyclebin check function - it will be properly
  // overridden in dstore
  error = file->ha_check_support_recyclebin(all_path, is_supported, table_def);
  destroy(file);

  if (error) {
    thd->clear_error();
    DBUG_RETURN(false);
  }
  DBUG_RETURN(is_supported);
}

/**
  generate recycle table name;

  @param[in]      thd         thread context
  @param[in]      table_list  dropping table
  @param[out]     new_table   renamed new table name

  @retval         false       success
  @retval         true        failure
*/
bool generate_recycle_table_name(THD *thd, Table_ref *table_list,
                                 const char **new_table) {
  DBUG_ENTER("generate_recycle_table_name");
  char
      buff[NAME_LEN];  // maximum length of the character set utf8mb3 is 64 * 3.

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  const dd::Table *new_table_def = nullptr;
  for (uint32_t retry_time = 0; retry_time <= table_name_generate_retry_time;
       retry_time++) {
    /*
      If the generated table name already exists,
      wait for 1s and generate a table id again.
    */
    if (retry_time) {
      my_sleep(table_name_generate_sleep_microseconds);
      thd->set_time();
    }
    /* Saves the last 32 bits of the timestamp seconds as the table id. */
    ulong table_id =
        (ulong)thd->query_start_in_secs() & recyclebin_invalid_table_id;

    int ret = 0;
    ret = snprintf_s(buff, sizeof(buff), sizeof(buff), "%s%s%s%s%lx",
                     table_list->db, recyclebin_table_name_delimiter.c_str(),
                     table_list->table_name,
                     recyclebin_table_name_delimiter.c_str(), table_id);
    if (ret <= 0) DBUG_RETURN(true);

    *new_table = thd->strmake(buff, strlen(buff));
    if (!*new_table) DBUG_RETURN(true);

    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, RECY_SCHEMA_NAME.str,
                     *new_table, MDL_EXCLUSIVE, MDL_TRANSACTION);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
      DBUG_RETURN(true);
    /* check the new table exist */
    if (thd->dd_client()->acquire(RECY_SCHEMA_NAME.str, *new_table,
                                  &new_table_def)) {
      DBUG_RETURN(true);
    } else {
      /*
        The current timestamp of the slave node is replicated from the master
        node. Do not need to generate table id again.
      */
      if (thd->system_thread == SYSTEM_THREAD_SLAVE_SQL ||
          thd->system_thread == SYSTEM_THREAD_SLAVE_WORKER ||
          new_table_def == nullptr)
        break;
    }
  }

  if (new_table_def != nullptr) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "table already exists in recycle_bin");
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}

bool rebuild_table_list(THD *thd, Table_ref ***next_local,
                        Table_ref ***next_global, const char *db_name,
                        size_t db_length, const char *table_name,
                        size_t table_name_length) {
  Table_ref *table_list = new (thd->mem_root) Table_ref;
  if (table_list == nullptr) return true;

  table_list->db = thd->mem_strdup(db_name);
  table_list->db_length = db_length;
  table_list->table_name = thd->mem_strdup(table_name);
  table_list->table_name_length = table_name_length;
  table_list->open_type = OT_BASE_ONLY;

  /* To be able to correctly look up the table in the table cache */
  if (lower_case_table_names)
    my_casedn_str(files_charset_info,
                  const_cast<char *>(table_list->table_name));
  table_list->alias = table_list->table_name;

  MDL_REQUEST_INIT(&table_list->mdl_request, MDL_key::TABLE, table_list->db,
                   table_list->table_name, MDL_EXCLUSIVE, MDL_TRANSACTION);
  /* Link into list */
  (**next_local) = table_list;
  (**next_global) = table_list;
  (*next_local) = &table_list->next_local;
  (*next_global) = &table_list->next_global;

  if (thd->recycle_state->get_type() ==
          im::recycle_bin::RECYCLE_BIN_RECYCLE_DB ||
      thd->recycle_state->get_type() ==
          im::recycle_bin::RECYCLE_BIN_RESTORE_DB) {
    Table_ident *table_ident = nullptr;
    table_ident = new (thd->mem_root) Table_ident(
        to_lex_cstring(table_list->db), to_lex_cstring(table_list->table_name));
    DBUG_EXECUTE_IF("recyclebin_recycle_database_alloc_fail_01",
                    { table_ident = nullptr; });

    if (!table_ident) {
      my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
               "memory allocation failed");
      return true;
    }

    LEX *const lex = thd->lex;
    Query_block *const select_lex = lex->current_query_block();
    if (select_lex->add_table_to_list(thd, table_ident, NULL,
                                      TL_OPTION_UPDATING, TL_IGNORE,
                                      MDL_EXCLUSIVE) == NULL) {
      return true;
    }
  }

  return false;
}

Table_ref *prepare_recycle_table(THD *thd, Table_ref *table, bool *res) {
  Table_ref *rename_table_list = nullptr;
  Table_ref **list_next_local, **list_next_global;
  list_next_local = list_next_global = &rename_table_list;

  /* Build the old table of renaming */
  const char *old_db = table->db;
  const char *old_table = table->table_name;
  const char *new_db = im::recycle_bin::RECY_SCHEMA_NAME.str;
  const char *new_table = nullptr;

  DEBUG_SYNC(thd, "recyclebin_recycle_after_mdl_lock_old_table");

  if (rebuild_table_list(thd, &list_next_local, &list_next_global, old_db,
                         strlen(old_db), old_table, strlen(old_table)))
    goto err;

  if (generate_recycle_table_name(thd, table, &new_table)) goto err;

  if (rebuild_table_list(thd, &list_next_local, &list_next_global, new_db,
                         strlen(new_db), new_table, strlen(new_table)))
    goto err;

  DEBUG_SYNC(thd, "recyclebin_recycle_after_mdl_lock_new_table");

  thd->lex->sql_command = SQLCOM_RENAME_TABLE;
  *res = false;
  return rename_table_list;

err:
  *res = true;
  return rename_table_list;
}

/**
  drop the table in recycle_bin

  @param[in]      thd       thread context
  @param[in]      table     Target table name

  @retval         false     success
  @retval         true      failure
*/
bool drop_base_recycle_table(THD *thd, Table_ref *tables) {
  LEX *lex;
  DBUG_ENTER("drop_base_recycle_table");
  lex = thd->lex;
  lex->sql_command = SQLCOM_DROP_TABLE;
  lex->drop_temporary = false;
  lex->drop_if_exists = false;
  thd->recycle_state->set_type(RECYCLE_BIN_PURGE);
  Table_ref *table_list;

  for (table_list = tables; table_list != nullptr;
       table_list = table_list->next_global) {
    table_list->open_type = OT_BASE_ONLY;
    MDL_REQUEST_INIT(&table_list->mdl_request, MDL_key::TABLE, table_list->db,
                     table_list->table_name, MDL_EXCLUSIVE, MDL_TRANSACTION);
  }

  /* Didn't check any access here */
  bool res =
      mysql_rm_table(thd, tables, lex->drop_if_exists, lex->drop_temporary);
  DBUG_RETURN(res);
}

/**
  Purge the table in recycle_bin

  @param[in]      thd       thread context
  @param[in]      table     Target table name

  @retval         false     success
  @retval         true      failure
*/
bool recycle_purge_table(THD *thd, const char *table) {
  DBUG_ENTER("recycle_purge_table");
  Recycle_lex recycle_lex(thd);
  Disable_autocommit_guard autocommit_guard(thd);
  /**
    Purge proc will be blocked if readonly, but recycle scheduler will pass.

    It's not absolutely safe when set read only, since here didn't hold global
    read lock, But tolerate it.
  */
  if (check_readonly(thd, true)) DBUG_RETURN(true);

  if (lock_recycle_schema(thd)) DBUG_RETURN(true);

  {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, RECY_SCHEMA_NAME.str, table,
                     MDL_EXCLUSIVE, MDL_TRANSACTION);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
      DBUG_RETURN(true);
  }

  bool exists = true;

  if (is_recycled_table(thd, table, &exists)) {
    if (!exists && thd->slave_thread) {
      DBUG_EXECUTE_IF("recyclebin_slave_purge_table_not_exist",
                      thd->slave_thread = false;);
      thd->clear_error();
      my_ok(thd);
      DBUG_RETURN(false);
    }
    DBUG_RETURN(true);
  }

  Table_ref *tables = build_table_list(
      thd, RECY_SCHEMA_NAME.str, RECY_SCHEMA_NAME.length, table, strlen(table));
  DBUG_RETURN(drop_base_recycle_table(thd, tables));
}

bool recreate_recycle_bin_schema(THD *thd) {
  thd->variables.transaction_read_only = false;
  thd->tx_read_only = false;
  bool error = false;

  Disable_autocommit_guard autocommit_guard(thd);

  /*
    Use Auto_releaser to keep uncommitted object for database until
    trans_commit() call.
  */
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  bool exists = false;
  if (dd::schema_exists(thd, RECY_SCHEMA_NAME.str, &exists)) return true;

  if (!exists) {
    if (lock_schema_name(thd, RECY_SCHEMA_NAME.str)) return true;
    DEBUG_SYNC(thd, "create_recyclebin_schema_after_lock_schema");
    error = dd::execute_query(
        thd, dd::String_type("CREATE DATABASE IF NOT EXISTS ") +
                 dd::String_type(RECY_SCHEMA_NAME.str) +
                 dd::String_type(" CHARACTER SET utf8mb4"));
  }

  return dd::end_transaction(thd, error);
}

bool is_recycle_bin_work(THD *thd) {
  return (!rds_recycle_bin_skip_acl_check) &&
         thd->variables.rds_recycle_bin_mode;
}

/**
  Reset the dd::Table entity id value, and the reference entity id value.
*/
template <typename Item, typename Item_impl>
static void reset_primary_key_id(const dd::Collection<Item *> &items) {
  for (const Item *item : items) {
    Item_impl *impl = dynamic_cast<Item_impl *>(const_cast<Item *>(item));
    if (impl) impl->set_id(dd::INVALID_OBJECT_ID);
  }
}

/* Init SE attributes. */
static void init_se_attributes(HA_CREATE_INFO *create_info) {
  create_info->data_file_name = nullptr;
  create_info->tablespace = nullptr;
}
/* Copy SE attributes. */
void move_se_attributes(HA_CREATE_INFO *create_info,
                        HA_CREATE_INFO *original_create_info) {
  create_info->data_file_name = original_create_info->data_file_name;
  create_info->tablespace = original_create_info->tablespace;
}

/**
  Recycle the table when truncate table.

  @param[in]      thd                 current thd
  @param[in]      path                table path
  @param[in]      table               table list
  @param[in]      create_info         temporary create info
  @param[in]      update_create_info  Whether update create info
  @param[in]      is_temp_table       Whether it's temporary table
  @param[in]      table_def           dd Table object

  @retval         ok              Success
  @retval         drop_continue   Should continue to truncate table
  @retval         error           Report client error
*/
Recycle_result recycle_truncate_table(THD *thd, const char *path,
                                      Table_ref *table_list,
                                      HA_CREATE_INFO *create_info,
                                      bool update_create_info,
                                      bool is_temp_table,
                                      dd::Table *table_def) {
  Thd_recycle_state_guard state_guard(thd);
  int error = 0;
  std::set<handlerton *> dummy_ddl_htons;
  bool res = false;
  HA_CREATE_INFO original_create_info;
  DBUG_ENTER("recycle_truncate_table");
  thd->recycle_state->set_type(im::recycle_bin::RECYCLE_BIN_TRUNCATE_TABLE);

  if (im::recycle_bin::check_support_logical_drop(thd, table_list) !=
      im::recycle_bin::Recycle_check_result::SUPPORTED) {
    thd->recycle_state->reset();
    DBUG_RETURN(Recycle_result::CONTINUE);
  }

  init_se_attributes(&original_create_info);

  handlerton *hton;
  if (dd::table_storage_engine(thd, table_def, &hton))
    DBUG_RETURN(Recycle_result::ERROR);

  // Get the handler for the table, and issue an error if we cannot load it.
  handler *file =
      (hton == NULL
           ? 0
           : get_new_handler((TABLE_SHARE *)0,
                             table_def->partition_type() != dd::Table::PT_NONE,
                             thd->mem_root, hton));
  if (!file) DBUG_RETURN(Recycle_result::ERROR);
  file->get_create_info(path, table_def, &original_create_info);

  res |= recycle_base_table(thd, table_list);

  DBUG_EXECUTE_IF("recyclebin_truncate_crash_after_rename", DBUG_SUICIDE(););
  DBUG_EXECUTE_IF("recyclebin_truncate_fail_after_rename", {
    res = true;
    file->print_error(HA_ERR_GENERIC, MYF(0));
  });

  if (!res) {
    /* 1. Reset all the primary key of DD table. */

    /* 1.1 Reset tablespace id (Unnecessary right now.)*/

    /* 1.2 Reset dd::Table id */
    dd::Table_impl *table_impl = dynamic_cast<dd::Table_impl *>(table_def);
    if (table_impl) table_impl->set_id(dd::INVALID_OBJECT_ID);

    /* 1.3 Reset dd::Column id */
    reset_primary_key_id<dd::Column, dd::Column_impl>(
        static_cast<const dd::Table *>(table_def)->columns());

    /* 1.4 Reset dd::Index id */
    reset_primary_key_id<dd::Index, dd::Index_impl>(
        static_cast<const dd::Table *>(table_def)->indexes());

    /* 1.5 Reset dd::foreign key id */
    reset_primary_key_id<dd::Foreign_key, dd::Foreign_key_impl>(
        static_cast<const dd::Table *>(table_def)->foreign_keys());

    /* 1.6 Reset dd::triggers */
    reset_primary_key_id<dd::Trigger, dd::Trigger_impl>(
        static_cast<const dd::Table *>(table_def)->triggers());

    /* 1.7 Reset dd::check_constraints */
    reset_primary_key_id<dd::Check_constraint, dd::Check_constraint_impl>(
        static_cast<const dd::Table *>(table_def)->check_constraints());

    /* 1.8 Reset dd::partition */
    for (const dd::Partition *item :
         static_cast<const dd::Table *>(table_def)->partitions()) {
      dd::Partition_impl *impl =
          dynamic_cast<dd::Partition_impl *>(const_cast<dd::Partition *>(item));
      if (impl) impl->set_id(dd::INVALID_OBJECT_ID);
      /* Reset sub partition */
      for (const dd::Partition *sub_item : item->subpartitions()) {
        dd::Partition_impl *sub_impl = dynamic_cast<dd::Partition_impl *>(
            const_cast<dd::Partition *>(sub_item));
        if (sub_impl) sub_impl->set_id(dd::INVALID_OBJECT_ID);
      }
    }
    /* 2. modify the sql command temporary */
    Sql_command_backup backup(thd, SQLCOM_CREATE_TABLE);

    /* 3. create new table */
    if ((error =
             ha_create_table(thd, path, table_list->db, table_list->table_name,
                             create_info, update_create_info, is_temp_table,
                             table_def, true, &original_create_info)) ||
        DBUG_EVALUATE_IF("recyclebin_truncate_fail_after_create_new_table", 1,
                         0)) {
      file->print_error(error, MYF(0));
      DBUG_RETURN(Recycle_result::ERROR);
    }
  } else {
    DBUG_RETURN(Recycle_result::ERROR);
  }

  DBUG_EXECUTE_IF("recyclebin_truncate_crash_after_create_new_table",
                  DBUG_SUICIDE(););

  DBUG_RETURN(Recycle_result::OK);
}

Recycle_check_result check_support_logical_drop(THD *thd, Table_ref *table) {
  if (!is_recycle_bin_work(thd) || !thd->recycle_state->is_recycle())
    return Recycle_check_result::NOT_SUPPORTED;

  int error = 0;
  bool ret = im::recycle_bin::is_support_logical_drop(thd, table, error);

  if (error) return Recycle_check_result::CHECK_ERROR;

  if (!ret) return Recycle_check_result::NOT_SUPPORTED;

  return Recycle_check_result::SUPPORTED;
}

bool recycle_base_table(THD *thd, Table_ref *table,
                        std::set<handlerton *> *post_ddl_htons,
                        Foreign_key_parents_invalidator *rb_fk_invalidator) {
  bool res = false;
  Table_ref *rename_table_list = prepare_recycle_table(thd, table, &res);

  DEBUG_SYNC_C("before_mysql_rename_tables_in_drop");

  if (res || DBUG_EVALUATE_IF("logical_drop_table_error_01", 1, 0) ||
      mysql_rename_tables(thd, rename_table_list, post_ddl_htons,
                          rb_fk_invalidator) ||
      DBUG_EVALUATE_IF("logical_drop_table_error_02", 1, 0)) {
    res = true;
  }

  return res;
}

Table_ref *prepare_restore_table(THD *thd, Recycle_show_result *table,
                                 const char *dst_db, bool *res) {
  Table_ref *rename_table_list = nullptr;
  Table_ref **list_next_local, **list_next_global;
  list_next_local = list_next_global = &rename_table_list;

  /* Build the old table of renaming */
  const char *old_db = table->schema.str;
  const char *old_table = table->table.str;
  const char *new_db = dst_db;
  const char *new_table = table->origin_table.str;

  if (rebuild_table_list(thd, &list_next_local, &list_next_global, old_db,
                         strlen(old_db), old_table, strlen(old_table)))
    goto err;

  if (rebuild_table_list(thd, &list_next_local, &list_next_global, new_db,
                         strlen(new_db), new_table, strlen(new_table)))
    goto err;

  thd->lex->sql_command = SQLCOM_RENAME_TABLE;
  *res = false;
  return rename_table_list;

err:
  *res = true;
  return rename_table_list;
}

bool restore_db_inner(THD *thd, const char *origin_db, const char *dst_db,
                      ulong *restored_tables,
                      std::set<handlerton *> *post_ddl_htons,
                      Foreign_key_parents_invalidator *rb_fk_invalidator) {
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  if (lock_recycle_schema(thd)) return true;

  std::vector<Recycle_show_result *> container;
  /* Retrieve all the recycled tables */
  if (get_recycle_tables(thd, thd->mem_root, &container)) return true;

  const char *origin_db_name;
  const char *dst_db_name;
  if (lower_case_table_names) {
    /* Convert database to lower case for comparison */
    char *origin_db_buff = thd->strmake(origin_db, strlen(origin_db));
    char *dst_db_buff = thd->strmake(dst_db, strlen(dst_db));
    if (origin_db_buff == nullptr || dst_db_buff == nullptr) {
      my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
               "memory allocation failed");
      return true;
    }
    my_casedn_str(system_charset_info, origin_db_buff);
    my_casedn_str(system_charset_info, dst_db_buff);
    origin_db_name = origin_db_buff;
    dst_db_name = dst_db_buff;
  } else {
    origin_db_name = origin_db;
    dst_db_name = dst_db;
  }

  for (auto it = container.cbegin(); it != container.cend(); it++) {
    if (!strcmp(origin_db_name, (*it)->origin_schema.str)) {
      if (!thd->mdl_context.owns_equal_or_stronger_lock(
              MDL_key::TABLE, RECY_SCHEMA_NAME.str, (*it)->table.str,
              MDL_EXCLUSIVE)) {
        MDL_request table_mdl_request;
        MDL_REQUEST_INIT(&table_mdl_request, MDL_key::TABLE,
                         RECY_SCHEMA_NAME.str, (*it)->table.str, MDL_EXCLUSIVE,
                         MDL_TRANSACTION);
        if (thd->mdl_context.acquire_lock(&table_mdl_request,
                                          thd->variables.lock_wait_timeout))
          return true;
      }

      bool res = false;
      Table_ref *rename_table_list =
          prepare_restore_table(thd, *it, dst_db_name, &res);

      if (res || !rename_table_list) {
        my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
                 "prepare restore table list failed");
        return true;
      }

      /* Check access, old table in recycle_bin need ALTER_ACL and DROP_ACL,
         and new table need CREATE_ACL and INSERT_ACL */
      if (check_table_access(thd, ALTER_ACL | DROP_ACL, rename_table_list,
                             false, 1, false) ||
          check_table_access(thd, INSERT_ACL | CREATE_ACL,
                             rename_table_list->next_local, false, 1, false)) {
        return true;
      }

      if (mysql_rename_tables(thd, rename_table_list, post_ddl_htons,
                              rb_fk_invalidator)) {
        my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
                 "restore db from __recyclebin__ failed");
        return true;
      }
      (*restored_tables)++;
    }
  }

  container.clear();

  if ((*restored_tables) == 0) {
    my_error(ER_RECYBIN_NO_MATCH, MYF(0), origin_db_name);
    return true;
  }

  return write_bin_log(thd, true, thd->query().str, thd->query().length, true);
}

bool get_next_char_pos(const CHARSET_INFO *cs, const char *start,
                       const char *end, char target, size_t *pos) {
  uint next_char_len;
  auto next_char = start;
  while (next_char < end) {
    next_char_len = my_mbcharlen_ptr(cs, next_char, end);
    if (next_char_len == 0) {
      break;
    }
    if (next_char_len == 1 && *next_char == target) {
      *pos = next_char - start;
      return false;
    }
    next_char += next_char_len;
  }
  return true;
}

size_t get_char_count(const CHARSET_INFO *cs, const char *start, size_t len,
                      char target) {
  size_t count = 0;
  size_t pos = 0;
  auto next_char = start;
  auto end = start + len;
  while (!get_next_char_pos(cs, next_char, end, target, &pos)) {
    count++;
    next_char += (pos + 1);
  }
  return count;
}

bool get_key_id_delimiter_pos(const CHARSET_INFO *cs, const char *start,
                              size_t len, size_t *offset) {
  size_t count = 0;
  size_t pos = 0;
  auto next_char = start;
  auto end = start + len;
  while (!get_next_char_pos(cs, next_char, end,
                            recyclebin_table_name_delimiter[0], &pos)) {
    count++;
    if (count == recyclebin_name_delimiter_count) {
      *offset = next_char + pos - start;
      return false;
    }
    next_char += (pos + 1);
  }
  return true;
}

std::string parse_key_by_table_name(const char *table_name, uint64_t *id) {
  DBUG_ENTER("parse_key_by_table_name");
  std::string key(table_name);
  std::string id_str;
  /*
    1. For 20240930 version, table name could be __innodb_dbname_tablename_id
       or be __innodb_dbname_tablename_id_
    2. For versions greater than or equal to 20241230, table name will be
       dbname@tablename@id
  */
  if (get_char_count(system_charset_info, key.c_str(), key.length(),
                     recyclebin_table_name_delimiter[0]) !=
      recyclebin_name_delimiter_count)
    DBUG_RETURN("");

  size_t pos = 0;
  if (get_key_id_delimiter_pos(system_charset_info, key.c_str(), key.length(),
                               &pos))
    DBUG_RETURN("");

  /*
    The split key and id are invalid. For example:
    123
    1@@123
    123231@mmm
  */
  if (pos > key.length() - 1 - recyclebin_table_name_delimiter.length() ||
      pos == 0)
    DBUG_RETURN("");
  id_str = key.substr(pos + recyclebin_table_name_delimiter.length());
  key = key.substr(0, pos);

  if (id_str.length() > recyclebin_invalid_table_id_str_len) DBUG_RETURN("");
  for (char c : id_str) {
    if (!std::isxdigit(c)) DBUG_RETURN("");
  }

  /* Parse the table id in hexadecimal format. */
  if (id) *id = std::strtoul(table_name + pos + 1, nullptr, 16);

  if (*id >= recyclebin_invalid_table_id) DBUG_RETURN("");
  DBUG_RETURN(key);
}

std::pair<std::string, std::string> parse_origin_table_by_table_name(
    const char *table_name, uint64_t *id) {
  DBUG_ENTER("parse_origin_table_by_table_name");
  /* dbname@tablename@id, dbname@tablename is key. */
  std::string key = parse_key_by_table_name(table_name, id);
  if (key.length() == 0) DBUG_RETURN(std::make_pair("", ""));

  size_t pos = 0;
  char target = recyclebin_table_name_delimiter[0];
  bool ret = get_next_char_pos(system_charset_info, key.c_str(),
                               key.c_str() + key.length(), target, &pos);
  if (ret ||
      pos > key.length() - 1 - recyclebin_table_name_delimiter.length() ||
      pos == 0)
    DBUG_RETURN(std::make_pair("", ""));

  /* schema name */
  std::string first = key.substr(0, pos);
  /* table name */
  std::string second =
      key.substr(pos + recyclebin_table_name_delimiter.length());

  if (first.length() == 0 || second.length() == 0)
    DBUG_RETURN(std::make_pair("", ""));

  DBUG_RETURN(std::make_pair(first, second));
}

const char *recycle_copy_str_with_cs(MEM_ROOT *mem_root, String *strfrom,
                                     const CHARSET_INFO *csto) {
  String new_db_utf8_str;
  uint dummy_errors;
  if (!new_db_utf8_str.copy(strfrom->ptr(), strfrom->length(),
                            strfrom->charset(), csto, &dummy_errors)) {
    return strmake_root(mem_root, new_db_utf8_str.ptr(),
                        new_db_utf8_str.length());
  }
  return nullptr;
}

} /* namespace recycle_bin */

} /* namespace im */
