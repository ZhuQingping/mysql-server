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

#include "sql/recyclebin/recycle_proc.h"
#include "securec.h"
#include "sql/auth/auth_acls.h"
#include "sql/recyclebin/recycle.h"
#include "sql/recyclebin/recycle_table.h"
#include "sql/sql_time.h"
#include "sql/tztime.h"

#include "mysql/components/services/log_builtins.h"
#include "sql/auth/auth_common.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dd_schema.h"
#include "sql/dd/properties.h"
#include "sql/dd/string_type.h"
#include "sql/mysqld.h"
#include "sql/protocol.h"
#include "sql/sql_rename.h"
#include "sql/sql_table.h"
#include "sql/thd_raii.h"
#include "sql/transaction.h"

namespace im {

namespace recycle_bin {

/* Uniform schema name for recycle bin */
LEX_CSTRING RECYCLE_BIN_PROC_SCHEMA = {STRING_WITH_LEN("dbms_recyclebin")};

/* Singleton instance for show_tables */
Proc *Recycle_proc_show::instance() {
  static Proc *proc = new Recycle_proc_show(key_memory_recycle);

  return proc;
}

/* Singleton instance for purge_table */
Proc *Recycle_proc_purge::instance() {
  static Proc *proc = new Recycle_proc_purge(key_memory_recycle);

  return proc;
}

/**
  Evoke the sql_cmd object for show_tables() proc.
*/
Sql_cmd *Recycle_proc_show::evoke_cmd(THD *thd,
                                      mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Evoke the sql_cmd object for purge_table() proc.
*/
Sql_cmd *Recycle_proc_purge::evoke_cmd(THD *thd,
                                       mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/* Override the default prepare. */
bool Sql_cmd_recycle_proc_show::prepare(THD *thd) {
  Sql_cmd_recycle_proc_base::prepare(thd);
  DBUG_ENTER("Sql_cmd_recycle_proc_show::prepare");
  if (!im::recycle_bin::is_recycle_bin_work(thd)) {
    my_error(ER_RECYCLEBIN_SWITCH_OFF, MYF(0));
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}

bool Sql_cmd_recycle_proc_show::pc_execute(THD *) {
  DBUG_ENTER("Sql_cmd_recycle_proc_show::pc_execute");
  DBUG_RETURN(false);
}

/**
  Convert the YYMMDDHHMMSS number to MYSQL_TIME.
*/
static void get_local_time(THD *thd, ulonglong nr, MYSQL_TIME *timestamp) {
  MYSQL_TIME t_mysql_time;
  bool not_used;
  Time_zone *tz = thd->variables.time_zone;

  /* Convert longlong to MYSQL_TIME (UTC) */
  my_longlong_to_datetime_with_warn(nr, &t_mysql_time, MYF(0));

  my_time_t t_time;
  /* Convert MYSQL_TIME to epoch time */
  t_time = my_tz_OFFSET0->TIME_to_gmt_sec(&t_mysql_time, &not_used);

  /* Convert epoch time to local time */
  tz->gmt_sec_to_TIME(timestamp, t_time);
}

/**
  Calculate future time by adding xxx seconds.
*/
static bool get_future_time(THD *thd, MYSQL_TIME *timestamp,
                            ulonglong seconds) {
  Interval interval;
  memset_s(&interval, sizeof(Interval), 0, sizeof(Interval));
  interval.second = seconds;
  return date_add_interval_with_warn(thd, timestamp, INTERVAL_SECOND, interval);
}

/**
  Query all recycle tables.
*/
void Sql_cmd_recycle_proc_show::send_result(THD *thd, bool error) {
  Protocol *protocol = thd->get_protocol();
  std::vector<Recycle_show_result *> container;
  DBUG_ENTER("Sql_cmd_recycle_proc_show::send_result");
  if (error) {
    assert(thd->is_error());
    DBUG_VOID_RETURN;
  }

  bool res = get_recycle_tables(thd, thd->mem_root, &container);
  if (res) DBUG_VOID_RETURN;

  if (m_proc->send_result_metadata(thd)) DBUG_VOID_RETURN;

  for (auto it = container.cbegin(); it != container.cend(); it++) {
    MYSQL_TIME timestamp;

    protocol->start_row();
    protocol->store_string((*it)->schema.str, (*it)->schema.length,
                           system_charset_info);
    protocol->store_string((*it)->table.str, (*it)->table.length,
                           system_charset_info);
    protocol->store_string((*it)->origin_schema.str,
                           (*it)->origin_schema.length, system_charset_info);
    protocol->store_string((*it)->origin_table.str, (*it)->origin_table.length,
                           system_charset_info);

    get_local_time(thd, (*it)->recycled_time, &timestamp);
    protocol->store_datetime(timestamp, 0);

    if (get_future_time(thd, &timestamp, rds_recycle_bin_retention))
      protocol->store_null();
    else
      protocol->store_datetime(timestamp, 0);

    if (protocol->end_row()) DBUG_VOID_RETURN;
  }
  my_eof(thd);
  DBUG_VOID_RETURN;
}

/* Override the default send result. */
void Sql_cmd_recycle_proc_purge::send_result(THD *thd, bool error) {
  DBUG_ENTER("Sql_cmd_recycle_proc_purge::send_result");
  thd->recycle_state->reset();
  if (error) {
    (void)(thd);
    assert(thd->is_error());
    DBUG_VOID_RETURN;
  }
  /** Ok protocal package has been set within mysql_rm_table(); */
  DBUG_VOID_RETURN;
}

/* Override the default prepare. */
bool Sql_cmd_recycle_proc_purge::prepare(THD *thd) {
  DBUG_ENTER("Sql_cmd_recycle_proc_purge::prepare");
  if (!thd->slave_thread && !im::recycle_bin::is_recycle_bin_work(thd)) {
    my_error(ER_RECYCLEBIN_SWITCH_OFF, MYF(0));
    DBUG_RETURN(true);
  }

  if (check_access(thd) || check_parameter()) DBUG_RETURN(true);

  if (thd->locked_tables_mode) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "unable to purge locked table");
    DBUG_RETURN(true);
  }

  char buff[128];
  String str(buff, sizeof(buff), system_charset_info);
  String *res;

  res = (*m_list)[0]->val_str(&str);
  const char *table =
      recycle_copy_str_with_cs(thd->mem_root, res, system_charset_info);
  if (table == NULL || strlen(table) == 0) {
    my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0), 1,
             m_proc->qname().c_str());
    DBUG_RETURN(true);
  }

  char table_name_buff[NAME_LEN + 1];
  if (lower_case_table_names) {
    /* Convert database to lower case for comparison */
    strmake(table_name_buff, table, sizeof(table_name_buff) - 1);
    my_casedn_str(system_charset_info, table_name_buff);
    table = table_name_buff;
  }

  Table_ref *table_list = build_table_list(
      thd, RECY_SCHEMA_NAME.str, RECY_SCHEMA_NAME.length, table, strlen(table));
  thd->recycle_state->set_type(RECYCLE_BIN_PURGE);
  if (check_table_access(thd, DROP_ACL, table_list, false, 1, false))
    DBUG_RETURN(true);

  set_prepared();
  DBUG_RETURN(false);
}

/**
  Purge table in recycle_bin

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_recycle_proc_purge::pc_execute(THD *thd) {
  char buff[128];
  String str(buff, sizeof(buff), system_charset_info);
  String *res;
  DBUG_ENTER("Sql_cmd_recycle_proc_purge::pc_execute");
  res = (*m_list)[0]->val_str(&str);
  const char *table = strmake_root(thd->mem_root, res->ptr(), res->length());
  thd->lex->sql_command = SQLCOM_DROP_TABLE;

  DBUG_RETURN(recycle_purge_table(thd, table));
}

/* Singleton instance for restore_table */
Proc *Recycle_proc_restore::instance() {
  static Proc *proc = new Recycle_proc_restore(key_memory_recycle);

  return proc;
}

/**
  Evoke the sql_cmd object for restore_table() proc.
*/
Sql_cmd *Recycle_proc_restore::evoke_cmd(THD *thd,
                                         mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/* Override the default send result. */
void Sql_cmd_recycle_proc_restore::send_result(THD *thd, bool error) {
  DBUG_ENTER("Sql_cmd_recycle_proc_restore::restore_result");
  thd->recycle_state->reset();
  if (error) {
    (void)thd;
    assert(thd->is_error());
    DBUG_VOID_RETURN;
  }
  /** Ok protocal package has been set within mysql_rename_tables(); */
  DBUG_VOID_RETURN;
}

/* Override the default check_access. */
bool Sql_cmd_recycle_proc_restore::prepare(THD *thd) {
  DBUG_ENTER("Sql_cmd_recycle_proc_restore::prepare");

  if (!thd->slave_thread && !im::recycle_bin::is_recycle_bin_work(thd)) {
    my_error(ER_RECYCLEBIN_SWITCH_OFF, MYF(0));
    DBUG_RETURN(true);
  }

  if (check_access(thd) || check_parameter()) DBUG_RETURN(true);

  if (thd->locked_tables_mode) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "unable to restore locked table");
    DBUG_RETURN(true);
  }

  char buff[192];  // maximum length of the character set utf8mb3 is 64 * 3.
  String str(buff, sizeof(buff), system_charset_info);
  String *res;

  /* 1. Parse the input parameters */
  const char *old_table = nullptr;
  const char *new_db = nullptr;
  const char *new_table = nullptr;
  res = (*m_list)[0]->val_str(&str);
  old_table = recycle_copy_str_with_cs(thd->mem_root, res, system_charset_info);
  DBUG_EXECUTE_IF("recyclebin_copy_with_cs_fail_02", { old_table = nullptr; });
  if (old_table == NULL || strlen(old_table) == 0) {
    my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0), 1,
             m_proc->qname().c_str());
    DBUG_RETURN(true);
  } else if (m_list->size() == 3) {
    res = (*m_list)[1]->val_str(&str);
    new_db = recycle_copy_str_with_cs(thd->mem_root, res, system_charset_info);
    DBUG_EXECUTE_IF("recyclebin_copy_with_cs_fail_03", { new_db = nullptr; });

    if (new_db == NULL || strlen(new_db) == 0) {
      my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0), 2,
               m_proc->qname().c_str());
      DBUG_RETURN(true);
    }

    if (is_recycle_db(new_db)) {
      my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
               "unable to restore table to __recyclebin__");
      DBUG_RETURN(true);
    }

    res = (*m_list)[2]->val_str(&str);
    new_table =
        recycle_copy_str_with_cs(thd->mem_root, res, system_charset_info);
    DBUG_EXECUTE_IF("recyclebin_copy_with_cs_fail_04",
                    { new_table = nullptr; });

    if (new_table == NULL || strlen(new_table) == 0) {
      my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0), 3,
               m_proc->qname().c_str());
      DBUG_RETURN(true);
    }
  }

  if (lower_case_table_names) {
    /* Convert database and table to lower case for comparison */
    char *old_table_str = thd->strmake(old_table, strlen(old_table));
    DBUG_EXECUTE_IF("recyclebin_restore_alloc_fail_03",
                    { old_table_str = nullptr; });
    if (old_table_str == nullptr) {
      my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
               "memory allocation failed");
      DBUG_RETURN(true);
    }
    my_casedn_str(system_charset_info, old_table_str);
    old_table = old_table_str;

    if (new_db && new_table) {
      char *new_db_str = thd->strmake(new_db, strlen(new_db));
      char *new_table_str = thd->strmake(new_table, strlen(new_table));
      DBUG_EXECUTE_IF("recyclebin_restore_alloc_fail_04",
                      { new_db_str = nullptr; });
      if (new_db_str == nullptr || new_table_str == nullptr) {
        my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
                 "memory allocation failed");
        DBUG_RETURN(true);
      }
      my_casedn_str(system_charset_info, new_db_str);
      my_casedn_str(system_charset_info, new_table_str);
      new_db = new_db_str;
      new_table = new_table_str;
    }
  }

  /* 2. The old table must exist in recycle_bin */
  {
    if (!thd->mdl_context.owns_equal_or_stronger_lock(
            MDL_key::SCHEMA, RECY_SCHEMA_NAME.str, "",
            MDL_INTENTION_EXCLUSIVE)) {
      MDL_request schema_mdl_request;
      MDL_REQUEST_INIT(&schema_mdl_request, MDL_key::SCHEMA,
                       RECY_SCHEMA_NAME.str, "", MDL_INTENTION_EXCLUSIVE,
                       MDL_TRANSACTION);
      if (thd->mdl_context.acquire_lock(&schema_mdl_request,
                                        thd->variables.lock_wait_timeout))
        DBUG_RETURN(true);
    }

    if (!thd->mdl_context.owns_equal_or_stronger_lock(
            MDL_key::TABLE, RECY_SCHEMA_NAME.str, old_table, MDL_EXCLUSIVE)) {
      MDL_request table_mdl_request;
      MDL_REQUEST_INIT(&table_mdl_request, MDL_key::TABLE, RECY_SCHEMA_NAME.str,
                       old_table, MDL_EXCLUSIVE, MDL_TRANSACTION);
      if (thd->mdl_context.acquire_lock(&table_mdl_request,
                                        thd->variables.lock_wait_timeout))
        DBUG_RETURN(true);
    }
  }

  const dd::Schema *recycle_schema = nullptr;
  const dd::Table *old_table_def = nullptr;
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (thd->dd_client()->acquire(RECY_SCHEMA_NAME.str, &recycle_schema)) {
    LogErr(WARNING_LEVEL, ER_RECYCLE_BIN, "acquire recycle schema");
    DBUG_RETURN(true);
  }
  if (recycle_schema == nullptr) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "recycle schema didn't exist");
    DBUG_RETURN(true);
  }
  if (thd->dd_client()->acquire(RECY_SCHEMA_NAME.str, old_table,
                                &old_table_def)) {
    DBUG_RETURN(true);
  }

  if (old_table_def == nullptr) {
    std::stringstream ss;
    ss << old_table << " didn't exist";
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0), ss.str().c_str());
    DBUG_RETURN(true);
  }
  if (!old_table_def->options().exists(ORIGIN_SCHEMA.str) ||
      !old_table_def->options().exists(ORIGIN_TABLE.str)) {
    std::stringstream ss;
    ss << old_table << " isn't recycled table";
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0), ss.str().c_str());
    DBUG_RETURN(true);
  }

  /* 3. If the new_db and new_table is invalid, just restore to the original
        name */
  Table_ident *old_table_ident = nullptr;
  Table_ident *new_table_ident = nullptr;
  old_table_ident = new (thd->mem_root)
      Table_ident(to_lex_cstring(thd->mem_strdup(RECY_SCHEMA_NAME.str)),
                  to_lex_cstring(old_table));
  DBUG_EXECUTE_IF("recyclebin_restore_alloc_fail_01",
                  { old_table_ident = nullptr; });
  if (!old_table_ident) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "memory allocation failed");
    DBUG_RETURN(true);
  }

  LEX_STRING origin_db;
  LEX_STRING origin_table;
  old_table_def->options().get(ORIGIN_SCHEMA.str, &origin_db, thd->mem_root);
  old_table_def->options().get(ORIGIN_TABLE.str, &origin_table, thd->mem_root);

  if (new_db && new_table && strlen(new_db) && strlen(new_table)) {
    LEX_CSTRING new_db_cstring = to_lex_cstring(new_db);
    new_table_ident = new (thd->mem_root)
        Table_ident(new_db_cstring, to_lex_cstring(new_table));
  } else {
    new_table_ident = new (thd->mem_root)
        Table_ident(to_lex_cstring(origin_db), to_lex_cstring(origin_table));
  }
  DBUG_EXECUTE_IF("recyclebin_restore_alloc_fail_02",
                  { new_table_ident = nullptr; });
  if (!new_table_ident) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "memory allocation failed");
    DBUG_RETURN(true);
  }

  /* 4. Add table to table_list array */
  LEX *const lex = thd->lex;
  Query_block *const select_lex = lex->current_query_block();
  if (select_lex->add_table_to_list(thd, old_table_ident, NULL,
                                    TL_OPTION_UPDATING, TL_IGNORE,
                                    MDL_EXCLUSIVE) == NULL ||
      select_lex->add_table_to_list(thd, new_table_ident, NULL,
                                    TL_OPTION_UPDATING, TL_IGNORE,
                                    MDL_EXCLUSIVE) == NULL) {
    DBUG_RETURN(true);
  }

  /* 5. Check access, old table in recycle_bin need ALTER_ACL and DROP_ACL,
        and new table need CREATE_ACL and INSERT_ACL */
  Table_ref *const first_table = select_lex->get_table_list();
  Table_ref *table;
  thd->recycle_state->set_type(RECYCLE_BIN_RESTORE_TABLE);
  for (table = first_table; table; table = table->next_local->next_local) {
    if (check_table_access(thd, ALTER_ACL | DROP_ACL, table, false, 1, false) ||
        check_table_access(thd, INSERT_ACL | CREATE_ACL, table->next_local,
                           false, 1, false)) {
      DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}

/**
  Restore table in recycle_bin
  @param[in]    THD           Thread context
  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_recycle_proc_restore::pc_execute(THD *thd) {
  DBUG_ENTER("Sql_cmd_recycle_proc_restore::pc_execute");

  /*
    For statements which need this, prevent InnoDB from automatically
    committing InnoDB transaction each time data-dictionary tables are
    closed after being updated.
  */
  Disable_autocommit_guard autocommit_guard(thd);

  LEX *const lex = thd->lex;
  Query_block *const select_lex = lex->current_query_block();
  Table_ref *const first_table = select_lex->get_table_list();

  thd->lex->sql_command = SQLCOM_RENAME_TABLE;
  /* Reuse the rename process */
  DBUG_RETURN(mysql_rename_tables(thd, first_table));
}

/* Singleton instance for restore_database */
Proc *Recycle_proc_restore_db::instance() {
  static Proc *proc = new Recycle_proc_restore_db(key_memory_recycle);

  return proc;
}

/**
  Evoke the sql_cmd object for restore_database() proc.
*/
Sql_cmd *Recycle_proc_restore_db::evoke_cmd(
    THD *thd, mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/* Override the default send result. */
void Sql_cmd_recycle_proc_restore_db::send_result(THD *thd, bool error) {
  DBUG_ENTER("Sql_cmd_recycle_proc_restore_db::restore_result");
  thd->recycle_state->reset();
  if (error) {
    assert(thd->is_error());
  }
  /** Ok protocal package has been set within mysql_rename_tables(); */
  DBUG_VOID_RETURN;
}

/* Override the default check_access. */
bool Sql_cmd_recycle_proc_restore_db::prepare(THD *thd) {
  DBUG_ENTER("Sql_cmd_recycle_proc_restore_db::prepare");

  if (!thd->slave_thread && !im::recycle_bin::is_recycle_bin_work(thd)) {
    my_error(ER_RECYCLEBIN_SWITCH_OFF, MYF(0));
    DBUG_RETURN(true);
  }

  if (check_access(thd) || check_parameter()) DBUG_RETURN(true);

  if (thd->locked_tables_mode) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "unable to restore locked table");
    DBUG_RETURN(true);
  }

  char buff[128];
  String str(buff, sizeof(buff), system_charset_info);
  String *res;

  /* 1. Parse the input parameters */
  const char *old_db = nullptr;
  const char *new_db = nullptr;
  res = (*m_list)[0]->val_str(&str);
  old_db = recycle_copy_str_with_cs(thd->mem_root, res, system_charset_info);
  DBUG_EXECUTE_IF("recyclebin_restore_table_old_db_null", { old_db = NULL; });

  if (old_db == NULL || strlen(old_db) == 0) {
    my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0), 1,
             m_proc->qname().c_str());
    DBUG_RETURN(true);
  } else if (m_list->size() == 2) {
    res = (*m_list)[1]->val_str(&str);
    new_db = recycle_copy_str_with_cs(thd->mem_root, res, system_charset_info);
    DBUG_EXECUTE_IF("recyclebin_copy_with_cs_fail_05", { new_db = nullptr; });
    if (new_db == NULL || strlen(new_db) == 0) {
      my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0), 2,
               m_proc->qname().c_str());
      DBUG_RETURN(true);
    }
    if (is_recycle_db(new_db)) {
      my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
               "unable to restore table to __recyclebin__");
      DBUG_RETURN(true);
    }
  } else {
    new_db = old_db;
  }

  m_origin_db = old_db;
  m_new_db = new_db;

  /* Lock recycle schema's logic here should check */
  if (lock_schema_name(thd, RECY_SCHEMA_NAME.str)) DBUG_RETURN(true);

  const dd::Schema *recycle_schema = nullptr;
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (thd->dd_client()->acquire(RECY_SCHEMA_NAME.str, &recycle_schema)) {
    LogErr(WARNING_LEVEL, ER_RECYCLE_BIN, "acquire recycle schema");
    DBUG_RETURN(true);
  }

  if (recycle_schema == nullptr) {
    my_error(ER_PREPARE_RECYCLE_TABLE_ERROR, MYF(0),
             "recycle schema didn't exist");
    DBUG_RETURN(true);
  }

  thd->lex->sql_command = SQLCOM_RENAME_TABLE;
  thd->recycle_state->set_type(RECYCLE_BIN_RESTORE_DB);

  DBUG_RETURN(false);
}

/**
  Restore db in recycle_bin
  @param[in]    THD           Thread context
  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_recycle_proc_restore_db::pc_execute(THD *thd) {
  DBUG_ENTER("Sql_cmd_recycle_proc_restore_db::pc_execute");

  /*
    For statements which need this, prevent InnoDB from automatically
    committing InnoDB transaction each time data-dictionary tables are
    closed after being updated.
  */
  Disable_autocommit_guard autocommit_guard(thd);

  std::set<handlerton *> post_ddl_htons;
  Foreign_key_parents_invalidator fk_invalidator;
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  ulong restored_tables = 0;
  bool error = restore_db_inner(thd, m_origin_db, m_new_db, &restored_tables,
                                &post_ddl_htons, &fk_invalidator);

  DBUG_EXECUTE_IF("recyclebin_restore_db_fail_before_commit", {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    error = true;
  });
  DBUG_EXECUTE_IF("recyclebin_restore_db_crash_before_commit", DBUG_SUICIDE(););

  if (!error) error = trans_commit_stmt(thd) || trans_commit(thd);

  DBUG_EXECUTE_IF("recyclebin_restore_db_fail_after_commit", {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    error = true;
  });
  DBUG_EXECUTE_IF("recyclebin_restore_db_crash_after_commit", DBUG_SUICIDE(););

  /*
    In case of error rollback the transaction in order to revert
    changes which are possible to rollback (e.g. removal of tables
    in SEs supporting atomic DDL, events and routines).
  */
  if (error) {
    trans_rollback_stmt(thd);
    /*
      Play safe to be sure that THD::transaction_rollback_request is
      cleared before work-around code below is run. This also necessary
      to synchronize state of data-dicitionary on disk and in cache (to
      clear cache of uncommitted objects).
    */
    trans_rollback_implicit(thd);
  }

  /*
    Call post-DDL handlerton hook. For engines supporting atomic DDL
    tables' files are removed from disk on this step.
  */
  for (handlerton *hton : post_ddl_htons) hton->post_ddl(thd);

  fk_invalidator.invalidate(thd);

  if (error) DBUG_RETURN(true);

  my_ok(thd, restored_tables);
  DBUG_RETURN(false);
}

} /* namespace recycle_bin */

} /* namespace im */
