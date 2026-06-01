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

#include "sql/sql_filter/sql_filter_proc.h"
#include <boost/algorithm/string.hpp>
#include "sql/auth/auth_acls.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"  // ER_THD
#include "sql/mysqld.h"
#include "sql/protocol.h"
#include "sql/sql_filter/sql_filter_cache.h"
#include "sql/sql_filter/sql_filter_table.h"
#include "sql/sql_filter/sql_filter_table_common.h"

LEX_CSTRING SQL_FILTER_PROC_SCHEMA = {C_STRING_WITH_LEN("dbms_sqlfilter")};

/* Singleton instance for add_sql_filter */
Proc *Sqlfilter_proc_add::instance() {
  static Proc *proc = new Sqlfilter_proc_add(key_memory_Sql_filter);

  return proc;
}

/* Singleton instance for delete_sql_filter */
Proc *Sqlfilter_proc_del::instance() {
  static Proc *proc = new Sqlfilter_proc_del(key_memory_Sql_filter);

  return proc;
}

/* Singleton instance for show_sql_filter */
Proc *Sqlfilter_proc_show::instance() {
  static Proc *proc = new Sqlfilter_proc_show(key_memory_Sql_filter);

  return proc;
}

/* Singleton instance for flush_sql_filter */
Proc *Sqlfilter_proc_flush::instance() {
  static Proc *proc = new Sqlfilter_proc_flush(key_memory_Sql_filter);

  return proc;
}

/* Singleton instance for update_sql_filter */
Proc *Sqlfilter_proc_update::instance() {
  static Proc *proc = new Sqlfilter_proc_update(key_memory_Sql_filter);

  return proc;
}

/* Evoke the sql_cmd object for add_sql_filter() proc. */
Sql_cmd *Sqlfilter_proc_add::evoke_cmd(THD *thd,
                                       mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Evoke the sql_cmd object for delete_sql_filter() proc.
*/
Sql_cmd *Sqlfilter_proc_del::evoke_cmd(THD *thd,
                                       mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Evoke the sql_cmd object for show_sql_filter() proc.
*/
Sql_cmd *Sqlfilter_proc_show::evoke_cmd(THD *thd,
                                        mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Evoke the sql_cmd object for flush_sql_filter() proc.
*/
Sql_cmd *Sqlfilter_proc_flush::evoke_cmd(THD *thd,
                                         mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Evoke the sql_cmd object for update_sql_filter() proc.
*/
Sql_cmd *Sqlfilter_proc_update::evoke_cmd(THD *thd,
                                          mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Create record from parameters.

  @param[in]      thd       Thread context
  @param[in]      list      Parameters

  @retval         record    sqlfilter record
*/
Sql_filter_record *Sql_cmd_sqlfilter_proc_add::get_record(THD *thd) {
  char buff[1024];
  String str(buff, sizeof(buff), system_charset_info);
  String *res;
  Sql_filter_record *record = new (thd->mem_root) Sql_filter_record();
  if (!record) return nullptr;

  // type
  res = (*m_list)[0]->val_str(&str);
  const char *type_str = strmake_root(thd->mem_root, res->ptr(), res->length());
  record->m_type = to_sql_filter_type(type_str);

  // max_concurrency
  record->m_max_concurrency = (*m_list)[1]->val_int();

  // key_str
  res = (*m_list)[2]->val_str(&str);
  record->m_key_str =
      to_lex_cstring(strmake_root(thd->mem_root, res->ptr(), res->length()));

  // node_id
  res = (*m_list)[3]->val_str(&str);
  record->m_node_id =
      to_lex_cstring(strmake_root(thd->mem_root, res->ptr(), res->length()));

  return record;
}

/**
  Add the sqlfilter into the mysql.rds_sql_filter_rules table,
  and copy into sqlfilter cache.

  First write into table, then update the sqlfilter cache,
  report my_error if failed.

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_sqlfilter_proc_add::pc_execute(THD *thd) {
  bool error = false;
  Sql_filter_record *record;
  DBUG_ENTER("Sql_cmd_sqlfilter_proc_add::pc_execute");
  record = get_record(thd);
  DBUG_EXECUTE_IF("inject_record_is_null", record = NULL;);
  if (!record) {
    my_error(ER_SQLFILTER_INVALID, MYF(0), "add a sql filter rule",
             "record is null");
    DBUG_RETURN(true);
  }

  const char *msg = "unknown";
  if (!record->check_record_valid(&msg)) {
    my_error(ER_SQLFILTER_INVALID, MYF(0), "add sql filter", msg);
    DBUG_RETURN(true);
  }

  if ((error = add_sql_filter(thd, (Conf_record *)(record)))) DBUG_RETURN(true);

  DBUG_RETURN(false);
}

/**
  Delete the sql filter rule from cache and table mysql.rds_sql_filter_rules.

  report my_error if failed.

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_sqlfilter_proc_del::pc_execute(THD *thd) {
  bool error;
  Conf_record *record;
  DBUG_ENTER("Sql_cmd_sqlfilter_proc_del::pc_execute");
  record = new (thd->mem_root) Sql_filter_record();
  DBUG_EXECUTE_IF("inject_record_is_null", record = NULL;);
  if (!record) {
    my_error(ER_SQLFILTER_INVALID, MYF(0), "delete a sql filter rule",
             "record is null");
    DBUG_RETURN(true);
  }
  record->set_id((*m_list)[0]->val_int());
  if ((error = del_sql_filter(thd, record))) DBUG_RETURN(true);

  DBUG_RETURN(false);
}

/**
  Show the sql filter in cache

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_sqlfilter_proc_show::pc_execute(THD *) {
  DBUG_ENTER("Sql_cmd_sqlfilter_proc_show::pc_execute");
  DBUG_RETURN(false);
}

void Sql_cmd_sqlfilter_proc_show::send_result(THD *thd, bool error) {
  Protocol *protocol = thd->get_protocol();
  Sqlfilter_show_result_container results;
  DBUG_ENTER("Sql_cmd_sqlfilter_proc_show::send_result");
  if (error) {
    assert(thd->is_error());
    DBUG_VOID_RETURN;
  }

  System_sql_filter::instance()->aggregate_sql_filters(&results);

  if (m_proc->send_result_metadata(thd)) DBUG_VOID_RETURN;

  for (const auto &result : results) {
    protocol->start_row();
    // Item_id
    protocol->store(result.m_id);

    // Type
    protocol->store(sql_filter_type_str[static_cast<size_t>(result.m_type)],
                    system_charset_info);

    // Cur_concur
    protocol->store((ulonglong)result.m_cur_concur);

    // Max_concur
    protocol->store((ulonglong)result.m_max_concurrency);

    // Key_num
    protocol->store((ulonglong)result.m_key_array.size());

    // Block_query_num
    protocol->store((ulonglong)result.m_block_query_num);

    // Key_str
    protocol->store_string(result.m_key_str.c_str(), result.m_key_str.length(),
                           system_charset_info);

    // Node_id
    protocol->store_string(result.m_node_id.c_str(), result.m_node_id.length(),
                           system_charset_info);

    if (protocol->end_row()) DBUG_VOID_RETURN;
  }

  my_eof(thd);
  DBUG_VOID_RETURN;
}

/**
  Clear the cache and read sql filter rules from mysql.rds_sql_filter_rules
  table, and copy into cache.

  report my_error if failed.

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_sqlfilter_proc_flush::pc_execute(THD *thd) {
  Conf_error error;
  DBUG_ENTER("Sql_cmd_sqlfilter_proc_flush::pc_execute");

  if ((error = reload_sqlfilter_rules(thd)) != Conf_error::CONF_OK) {
    my_error(ER_LOAD_SQL_FILTER_RULE, MYF(0));
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}

/**
  Create record from parameters of update_sql_filter.

  @param[in]      thd       Thread context
  @param[in]      list      Parameters

  @retval         record    Sql filter record
*/
Sql_filter_record *Sql_cmd_sqlfilter_proc_update::get_record(THD *thd) {
  char buff[1024];
  String str(buff, sizeof(buff), system_charset_info);
  String *res;
  Sql_filter_record *record = new (thd->mem_root) Sql_filter_record();
  if (!record) return nullptr;

  // id
  record->set_id((*m_list)[0]->val_int());

  // type
  res = (*m_list)[1]->val_str(&str);
  const char *type_str = strmake_root(thd->mem_root, res->ptr(), res->length());
  record->m_type = to_sql_filter_type(type_str);

  // max_concurrency
  record->m_max_concurrency = (*m_list)[2]->val_int();

  // key_str
  res = (*m_list)[3]->val_str(&str);
  record->m_key_str =
      to_lex_cstring(strmake_root(thd->mem_root, res->ptr(), res->length()));

  // node_id
  res = (*m_list)[4]->val_str(&str);
  record->m_node_id =
      to_lex_cstring(strmake_root(thd->mem_root, res->ptr(), res->length()));

  return record;
}

/**
  update one sql filter rule into the mysql.rds_sql_filter_rules table,
  and udpate into sql filter cache.

  First update into table, then update the sql filter cache,
  report my_error if failed.

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_sqlfilter_proc_update::pc_execute(THD *thd) {
  bool error = false;
  Sql_filter_record *record;
  DBUG_ENTER("Sql_cmd_sqlfilter_proc_update::pc_execute");
  record = get_record(thd);
  DBUG_EXECUTE_IF("inject_record_is_null", record = NULL;);
  if (!record) {
    my_error(ER_SQLFILTER_INVALID, MYF(0), "update a sql filter rule",
             "record is null");
    DBUG_RETURN(true);
  }

  const char *msg = "unknown";
  if (!record->check_record_valid(&msg)) {
    my_error(ER_SQLFILTER_INVALID, MYF(0), "update a sql filter rule", msg);
    DBUG_RETURN(true);
  }

  if ((error = update_sql_filter(thd, (Conf_record *)(record))))
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}
