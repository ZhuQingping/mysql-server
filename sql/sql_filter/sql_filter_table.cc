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

#include "sql/sql_filter/sql_filter_table.h"
#include <boost/algorithm/string.hpp>
#include "lex_string.h"
#include "m_string.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/log_shared.h"
#include "scope_guard.h"  // create_scope_guard
#include "sql/auth/auth_acls.h"
#include "sql/common/table.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_filter/sql_filter_cache.h"
#include "sql/sql_filter/sql_filter_table_common.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "sql/thd_raii.h"
#include "sql/transaction.h"

/* sqlfilter table schema name */
LEX_CSTRING SQL_FILTER_SCHEMA_NAME = {STRING_WITH_LEN("mysql")};

/* sqlfilter table name */
LEX_CSTRING SQL_FILTER_TABLE_NAME = {STRING_WITH_LEN("rds_sql_filter_rules")};

/* sqlfilter table alias */
const char *SQL_FILTER_TABLE_ALIAS = "rds_sql_filter_rules";

const char *sql_filter_type_str[] = {"SELECT", "UPDATE", "DELETE", "INSERT"};

SqlFilterType to_sql_filter_type(const char *str) {
  for (size_t i = 0; i < static_cast<size_t>(SqlFilterType::LAST); i++) {
    if (my_strcasecmp(system_charset_info, sql_filter_type_str[i], str) == 0) {
      return static_cast<SqlFilterType>(i);
    }
  }
  return SqlFilterType::LAST;
}

/* Table "mysql.rds_sql_filter_rules" definition */
static const TABLE_FIELD_TYPE
    mysql_sql_filter_table_fields[MYSQL_SQL_FILTER_FIELD_COUNT] = {
        {{STRING_WITH_LEN("item_id")}, {STRING_WITH_LEN("bigint")}, {NULL, 0}},
        {{STRING_WITH_LEN("type")},
         {STRING_WITH_LEN(
             "enum('SELECT','UPDATE','DELETE','INSERT','REPLACE')")},
         {STRING_WITH_LEN("utf8mb3")}},
        {{STRING_WITH_LEN("max_concurrency")},
         {STRING_WITH_LEN("bigint")},
         {NULL, 0}},
        {{STRING_WITH_LEN("key_str")}, {STRING_WITH_LEN("text")}, {NULL, 0}},
        {{STRING_WITH_LEN("node_id")},
         {STRING_WITH_LEN("varchar(64)")},
         {NULL, 0}}};

static const TABLE_FIELD_DEF sql_filter_table_def = {
    MYSQL_SQL_FILTER_FIELD_COUNT, mysql_sql_filter_table_fields};

/**
  Reconstruct and Report error by adding handler error.

  @param[in]      errcode     handler error.
*/
void Sql_filter_reader::print_ha_error(int errcode) {
  ha_error(ER_SQLFILTER_TABLE_OP_FAILED, errcode);
}
/**
  Log the conf table error.

  @param[in]    err     Conf error type
*/
void Sql_filter_reader::log_error(Conf_error err) {
  log_conf_error(ER_APPLY_SQLFILTER, err);
}
/* Create new sqlfilter record */
Conf_record *Sql_filter_reader::new_record() {
  return new (m_mem_root) Sql_filter_record();
}
/**
  Push invalid sqlfilter record warning

  @param[in]      record        Sqlfilter record
  @param[in]      when          Operation
*/
void Sql_filter_reader::row_warning(Conf_record *, const char *when,
                                    const char *msg) {
  /* Report row warning if invalid rule */
  push_warning_printf(m_thd, Sql_condition::SL_WARNING, ER_SQLFILTER_INVALID,
                      ER_THD(m_thd, ER_SQLFILTER_INVALID), when, msg);
}

/**
  Save the row value into Sql_filter_record structure.

  @param[out]   record      Sqlfilter record
*/
void Sql_filter_reader::read_attributes(Conf_record *r) {
  DBUG_ENTER("Sql_filter_reader::read_attributes");
  Sql_filter_record *record = dynamic_cast<Sql_filter_record *>(r);
  DBUG_ASSERT(record);
  record->reset();
  record->m_id = m_table->field[MYSQL_SQL_FILTER_FIELD_RULE_ID]->val_int();
  record->m_type = to_sql_filter_type(
      m_table->field[MYSQL_SQL_FILTER_FIELD_TYPE]->val_int());
  record->m_max_concurrency =
      m_table->field[MYSQL_SQL_FILTER_FIELD_MAX_CONCURRENCY]->val_int();
  record->m_key_str = to_lex_cstring(
      get_field(m_mem_root, m_table->field[MYSQL_SQL_FILTER_FIELD_KEYWORDS]));

  record->m_node_id = to_lex_cstring(
      get_field(m_mem_root, m_table->field[MYSQL_SQL_FILTER_FIELD_NODE_ID]));

  DBUG_VOID_RETURN;
}

/**
  Reconstruct and Report error by adding handler error.

  @param[in]      errcode     handler error.
*/
void Sql_filter_writer::print_ha_error(int errcode) {
  ha_error(ER_SQLFILTER_TABLE_OP_FAILED, errcode);
}

/**
  Store the sqlfilter attributes into table->field

  @param[in]      record        the table row object
*/
void Sql_filter_writer::store_attributes(const Conf_record *r) {
  DBUG_ENTER("Sql_filter_writer::store_attributes");
  const Sql_filter_record *record = dynamic_cast<const Sql_filter_record *>(r);

  if (m_op_type == Conf_table_op::OP_INSERT ||
      m_op_type == Conf_table_op::OP_UPDATE) {
    restore_record(m_table, s->default_values);

    // type
    longlong type_pos = static_cast<longlong>(record->m_type);
    m_table->field[MYSQL_SQL_FILTER_FIELD_TYPE]->store(
        sql_filter_type_str[type_pos], strlen(sql_filter_type_str[type_pos]),
        system_charset_info);
    m_table->field[MYSQL_SQL_FILTER_FIELD_TYPE]->set_notnull();

    // max_concurrency
    m_table->field[MYSQL_SQL_FILTER_FIELD_MAX_CONCURRENCY]->store(
        (longlong)record->m_max_concurrency, true);
    m_table->field[MYSQL_SQL_FILTER_FIELD_MAX_CONCURRENCY]->set_notnull();

    // key_str
    m_table->field[MYSQL_SQL_FILTER_FIELD_KEYWORDS]->store(
        record->m_key_str.str, record->m_key_str.length, system_charset_info);
    m_table->field[MYSQL_SQL_FILTER_FIELD_KEYWORDS]->set_notnull();

    // node_id
    m_table->field[MYSQL_SQL_FILTER_FIELD_NODE_ID]->store(
        record->m_node_id.str, record->m_node_id.length, system_charset_info);
    m_table->field[MYSQL_SQL_FILTER_FIELD_NODE_ID]->set_notnull();
  }
  DBUG_VOID_RETURN;
}

/**
  Store the record sqlfilter id into table->field[ID].

  @param[in]      record        the table row
*/
void Sql_filter_writer::store_id(const Conf_record *r) {
  const Sql_filter_record *record = dynamic_cast<const Sql_filter_record *>(r);
  m_table->field[MYSQL_SQL_FILTER_FIELD_RULE_ID]->store(record->m_id, true);
}

/**
  Retrieve some from table->fields into record.

  @param[out]     record        the table row object
*/
void Sql_filter_writer::retrieve_attr(Conf_record *) {}

/**
  Push row not found warning to client.

  @param[in]      record        table table row object
*/
void Sql_filter_writer::row_not_found_warning(Conf_record *r) {
  Sql_filter_record *record = dynamic_cast<Sql_filter_record *>(r);
  /* Not found rule, push warning here */
  push_warning_printf(m_thd, Sql_condition::SL_WARNING, ER_SQLFILTER_NOT_FOUND,
                      ER_THD(m_thd, ER_SQLFILTER_NOT_FOUND), record->get_id(),
                      "table");
}

bool Sql_filter_writer::update_row_by_id(Conf_record *record) {
  DBUG_ENTER("Sql_filter_writer::update_row_by_id");
  int err = 0;
  uchar user_key[MAX_KEY_LENGTH];
  setup_table();
  /* Save record key into table->field */
  store_id(record);
  /* Use user key */
  int user_key_idx = 0;
  /* Promise the id column has first key */
  key_copy(user_key, m_table->record[0], m_table->key_info,
           m_table->key_info->key_length);

  err = m_table->file->ha_index_read_idx_map(m_table->record[0], user_key_idx,
                                             user_key, HA_WHOLE_KEY,
                                             HA_READ_KEY_EXACT);
  if (err) {
    if (err != HA_ERR_KEY_NOT_FOUND && err != HA_ERR_END_OF_FILE) {
      print_ha_error(err);
      DBUG_RETURN(true);
    }
    row_not_found_warning(record);
    DBUG_RETURN(false);
  }

  /* old row found */
  store_record(m_table, record[1]);
  // Update again for new columns
  store_attributes(record);
  store_id(record);

  err = m_table->file->ha_update_row(m_table->record[1], m_table->record[0]);
  if (err && err != HA_ERR_RECORD_IS_THE_SAME) {
    print_ha_error(err);
    DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}

/**
  Open the sqlfilter table, report error if failed.*

  Attention:
  It didn't open attached transaction, so it must commit
  current transaction context when close sqlfilter table.

  Make sure it launched within main thread booting
  or statement that cause implicit commit.

  Report client error if failed.

  @param[in]      thd           Thread context
  @param[in]      table_list    rds_sql_filter_rules table
  @param[in]      write         read or write

  @retval         Conf_error
*/
Conf_error open_sql_filter_table(THD *thd, TABLE_REF_PTR &table_list,
                                 bool write) {
  DBUG_ENTER("open_sql_filter_table");

  if (open_conf_table(thd, table_list, SQL_FILTER_SCHEMA_NAME,
                      SQL_FILTER_TABLE_NAME, SQL_FILTER_TABLE_ALIAS,
                      &sql_filter_table_def, write))

    DBUG_RETURN(Conf_error::CONF_ER_TABLE_OP_ERROR);

  DBUG_RETURN(Conf_error::CONF_OK);
}

/**
  Reload sql filter rules to memory structure from mysql.rds_sql_filter_rules.

  @retval         Conf_error
*/
Conf_error reload_sqlfilter_rules(THD *thd) {
  Conf_error error;
  TABLE_REF_PTR table_list;
  Conf_records *records;
  DBUG_ENTER("reload_sqlfilter_rules");

  if ((error = open_sql_filter_table(thd, table_list, false)) !=
      Conf_error::CONF_OK)
    DBUG_RETURN(error);
  DEBUG_SYNC_C("flush_sql_filter_after_open_table");
  DBUG_ASSERT(table_list->table);

  Sql_filter_reader reader(thd, table_list->table, thd->mem_root);
  records = reader.read_all_rows(&error);

  if (error != Conf_error::CONF_OK) goto err_and_close;
  DBUG_ASSERT(records);

  // refresh sqlfilter cache
  System_sql_filter::instance()->flush_records(records);

err_and_close:
  commit_and_close_conf_table(thd);
  DBUG_RETURN(error);
}

/**
  Commit the sqlfilter transaction.
  Call reload_sqlfilter_rules() if commit failed.

  @param[in]      thd           Thread context
  @param[in]      rollback      Rollback request
*/
bool sql_filter_end_trans(THD *thd, bool rollback) {
  bool result;
  if ((result = conf_end_trans(thd, rollback))) reload_sqlfilter_rules(thd);
  return result;
}

/**
  Add new sqlfilter into sqlfilter table and insert sqlfilter cache.
  Report client error if failed.

  @param[in]      thd         Thread context
  @param[in]      record      sqlfilter

  @retval         false       Success
  @retval         true        Failure
*/
bool add_sql_filter(THD *thd, Conf_record *r) {
  Conf_error error;
  TABLE_REF_PTR table_list;
  Conf_records records(key_memory_Sql_filter);
  DBUG_ENTER("add_sql_filter");
  if (System_sql_filter::instance()->map_size() >=
      rds_sqlfilter_rules_max_count) {
    my_error(ER_SQLFILTER_PRIVILEGE_REQUIRED, MYF(0),
             "Sql filter rule's count exceed rds_sqlfilter_rules_max_count.");
    DBUG_RETURN(true);
  }

  Disable_binlog_guard binlog_guard(thd);
  if ((error = open_sql_filter_table(thd, table_list, true)) !=
      Conf_error::CONF_OK)
    DBUG_RETURN(true);

  DEBUG_SYNC_C("add_sql_filter_after_open_table");

  DBUG_ASSERT(table_list->table);

  Sql_filter_writer writer(thd, table_list->table, thd->mem_root,
                           Conf_table_op::OP_INSERT);

  if (writer.write_row(r)) goto err;

  records.push_back(r);
  System_sql_filter::instance()->add_records(&records);

  sql_filter_end_trans(thd, false);
  DBUG_RETURN(false);

err:
  sql_filter_end_trans(thd, true);
  DBUG_RETURN(true);
}

/**
  Delete a sql filter rule.

  Only report warning message if row or cache not found
*/
bool del_sql_filter(THD *thd, Conf_record *record) {
  Conf_error error;
  TABLE_REF_PTR table_list;

  DBUG_ENTER("del_sql_filter");
  Disable_binlog_guard binlog_guard(thd);

  if ((error = open_sql_filter_table(thd, table_list, true)) !=
      Conf_error::CONF_OK)
    DBUG_RETURN(true);

  DEBUG_SYNC_C("del_sql_filter_after_open_table");

  Sql_filter_writer writer(thd, table_list->table, thd->mem_root,
                           Conf_table_op::OP_DELETE);

  if (writer.delete_row_by_id(record)) goto err;

  if (System_sql_filter::instance()->delete_sql_filter(record->get_id())) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SQLFILTER_NOT_FOUND,
                        ER_THD(thd, ER_SQLFILTER_NOT_FOUND), record->get_id(),
                        "cache");
  }

  sql_filter_end_trans(thd, false);
  DBUG_RETURN(false);

err:
  sql_filter_end_trans(thd, true);
  DBUG_RETURN(true);
}

static void sqlfilter_log_error(Conf_error err) {
  log_conf_error(ER_APPLY_SQLFILTER, err);
}

/**
  Init the sqlfilter rules when mysqld reboot

  It should log error message if failed, reported client error
  will be ingored.

  @param[in]      bootstrap     Whether initialize or restart.
*/
void statement_sqlfilter_init(bool bootstrap) {
  Conf_error error;
  DBUG_ENTER("statement_sqlfilter_init");

  if (bootstrap) DBUG_VOID_RETURN;

  /* When reboot, we need to create new THD to read rules */
  THD *orig_thd = current_thd;

  THD *thd = new THD;
  thd->thread_stack = pointer_cast<char *>(&thd);
  thd->store_globals();
  lex_start(thd);

  error = reload_sqlfilter_rules(thd);

  lex_end(thd->lex);
  delete thd;
  if (orig_thd) orig_thd->store_globals();
  /* Log error message if any */
  sqlfilter_log_error(error);
  DBUG_VOID_RETURN;
}

/**
  update a sql filter rule in the mysql.rds_sql_filter_rules table and update
  rules cache. Report client error if failed.

  @param[in]      thd         Thread context
  @param[in]      record      sql filter

  @retval         false       Success
  @retval         true        Failure
*/
bool update_sql_filter(THD *thd, Conf_record *r) {
  Conf_error error;
  TABLE_REF_PTR table_list;

  DBUG_ENTER("update_sql_filter");
  Disable_binlog_guard binlog_guard(thd);

  // update the cache
  Sqlfilter_update_error update_error =
      System_sql_filter::instance()->update_sql_filter(r->get_id(), r);
  if (update_error == Sqlfilter_update_error::TYPE_NOT_SAME) {
    my_error(ER_SQLFILTER_INVALID, MYF(0), "update a sql filter rule",
             "updating the type is not allowed.");
    DBUG_RETURN(true);
  }

  if (update_error == Sqlfilter_update_error::CACHE_NOT_FIND) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SQLFILTER_NOT_FOUND,
                        ER_THD(thd, ER_SQLFILTER_NOT_FOUND), r->get_id(),
                        "cache");
  }

  if ((error = open_sql_filter_table(thd, table_list, true)) !=
      Conf_error::CONF_OK)
    DBUG_RETURN(true);

  DEBUG_SYNC_C("update_sql_filter_after_open_table");
  DBUG_ASSERT(table_list->table);

  Sql_filter_writer writer(thd, table_list->table, thd->mem_root,
                           Conf_table_op::OP_UPDATE);

  bool ret = writer.update_row_by_id(r);
  sql_filter_end_trans(thd, ret);
  DBUG_RETURN(ret);
}
