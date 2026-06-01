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
#include "sql/ddl_info.h"
#include <mysql/plugin.h>
#include "my_stacktrace.h"
#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/binlog.h"
#include "sql/common/table.h"
#include "sql/dd/dd_table.h"
#include "sql/dd/types/table.h"
#include "sql/ddl_info_file.h"
#include "sql/mysqld.h"
#include "sql/protocol_classic.h"
#include "sql/sql_alter.h"
#include "sql/sql_base.h"
#include "sql/sql_gipk.h"
#include "sql/sql_lex.h"
#include "sql/sql_parse.h"
#include "sql/sql_partition.h"
#include "sql/sql_prepare.h"
#include "sql/sql_table.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/thd_raii.h"
#include "sql/transaction.h"

thread_local THD *replay_thd = nullptr;

/* ddl_info_replay_trace table alias */
const char *DDL_INFO_REPLAY_TABLE_ALIAS = "ddl_info_replay_trace";

/* ddl_info_replay_trace name */
LEX_CSTRING DDL_INFO_REPLAY_TRACE_NAME = {
    STRING_WITH_LEN(DDL_INFO_REPLAY_TABLE_ALIAS)};

/* Table "mysql.ddl_info_replay_trace" definition */
static const TABLE_FIELD_TYPE
    mysql_ddl_info_trace_table_fields[MYSQL_DDL_INFO_TRACE_FIELD_COUNT] = {
        {{STRING_WITH_LEN("lsn")}, {STRING_WITH_LEN("bigint")}, {NULL, 0}},
        {{STRING_WITH_LEN("ddl_info")}, {STRING_WITH_LEN("text")}, {NULL, 0}}};

static const TABLE_FIELD_DEF ddl_info_trace_table_def = {
    MYSQL_DDL_INFO_TRACE_FIELD_COUNT, mysql_ddl_info_trace_table_fields};

static std::mutex ddl_throttle_mutex;
static std::unordered_set<uint64_t> ongoing_ddl_threads;

int log_ddl_info_to_trace(THD *thd, uint64_t lsn, const char *ddl_info,
                          size_t ddl_info_len) {
  DBUG_TRACE;
  int error = 1;
  TABLE *table = nullptr;
  TABLE_REF_PTR table_list;
  Field **fields = nullptr;

  if (im::open_conf_table(thd, table_list, MYSQL_SCHEMA_NAME,
                          DDL_INFO_REPLAY_TRACE_NAME,
                          DDL_INFO_REPLAY_TABLE_ALIAS,
                          &ddl_info_trace_table_def, true /*write*/)) {
    return 1;
  }

  DBUG_ASSERT(table_list->table);
  table = table_list->table;

  // fill fields
  table->use_all_columns();
  fields = table->field;
  fields[0]->set_notnull();
  if (fields[0]->store(lsn, true /* unsigned = true*/)) {
    goto end;
  }
  if (fields[1]->store(ddl_info, ddl_info_len, &my_charset_bin)) {
    goto end;
  }

  // write row
  error = table->file->ha_write_row(table->record[0]);
  if (!error) return 0;

end:
  im::conf_end_trans(thd, error);

  return error;
}

/**
   Get the maximum LSN from the table "mysql.ddl_info_replay_trace".

   @param thd Pointer to the thread handle.
   @param max_lsn Reference to store the maximum LSN found in the table.

   @retval 0 if the operation was successful.
   @retval Non-zero error code otherwise.
 */
int get_max_ddl_query_trace_lsn(THD *thd, uint64_t &max_lsn) {
  TABLE *table = nullptr;
  int error = 0;
  bool ret = false;
  TABLE_REF_PTR table_list;

  if (im::open_conf_table(thd, table_list, MYSQL_SCHEMA_NAME,
                          DDL_INFO_REPLAY_TRACE_NAME,
                          DDL_INFO_REPLAY_TABLE_ALIAS,
                          &ddl_info_trace_table_def, false /*write*/)) {
    return 1;
  }
  DBUG_ASSERT(table_list->table);
  table = table_list->table;
  table->use_all_columns();

  // init index using primary key. Use sorted order
  error = table->file->ha_index_init(0, true);
  if (error) goto end;

  // move the position to last record
  error = table->file->ha_index_last(table->record[0]);
  if (error == 0) {
    // fetch the last LSN
    max_lsn = table->field[0]->val_int();
  }

end:
  table->file->ha_index_end();
  ret = (error != 0 && error != HA_ERR_END_OF_FILE);
  im::conf_end_trans(thd, ret);

  DBUG_EXECUTE_IF("ddl_replay_select_trace_error", {
    error = HA_ERR_INTERNAL_ERROR;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  });
  return error;
}

ddl_command_type get_ddl_command_type(THD *thd) {
  enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);
  switch (sql_command) {
    /* Schema object modification commands */
    case SQLCOM_ALTER_TABLE:
    case SQLCOM_ALTER_DB:
    case SQLCOM_ALTER_PROCEDURE:
    case SQLCOM_ALTER_FUNCTION:
    case SQLCOM_ALTER_TABLESPACE:
    case SQLCOM_ALTER_EVENT:
    case SQLCOM_ALTER_INSTANCE:
    case SQLCOM_ALTER_RESOURCE_GROUP:
    case SQLCOM_ANALYZE:
    case SQLCOM_CREATE_TABLE:
    case SQLCOM_CREATE_INDEX:
    case SQLCOM_CREATE_DB:
    case SQLCOM_CREATE_FUNCTION:
    case SQLCOM_CREATE_VIEW:
    case SQLCOM_CREATE_TRIGGER:
    case SQLCOM_CREATE_EVENT:
    case SQLCOM_CREATE_RESOURCE_GROUP:
    case SQLCOM_CREATE_SRS:
    case SQLCOM_DROP_TABLE:
    case SQLCOM_DROP_INDEX:
    case SQLCOM_DROP_DB:
    case SQLCOM_DROP_VIEW:
    case SQLCOM_DROP_TRIGGER:
    case SQLCOM_DROP_EVENT:
    case SQLCOM_DROP_RESOURCE_GROUP:
    case SQLCOM_DROP_SRS:
    case SQLCOM_OPTIMIZE:
    case SQLCOM_TRUNCATE:
    case SQLCOM_RENAME_TABLE:
      return DDLCOM_OTHER;

    /* User/privilege management commands */
    case SQLCOM_RENAME_USER:
    case SQLCOM_ALTER_USER:
    case SQLCOM_ALTER_USER_DEFAULT_ROLE:
    case SQLCOM_CREATE_USER:
    case SQLCOM_DROP_USER:
    case SQLCOM_REVOKE:
    case SQLCOM_REVOKE_ROLE:
    case SQLCOM_REVOKE_ALL:
    case SQLCOM_CREATE_ROLE:
    case SQLCOM_DROP_ROLE:
    case SQLCOM_GRANT:
    case SQLCOM_GRANT_ROLE:
    case SQLCOM_SET_PASSWORD:
    case SQLCOM_CREATE_SERVER:
    case SQLCOM_ALTER_SERVER:
    case SQLCOM_DROP_SERVER:
      return DDLCOM_PRIVI;

    /* Stored procedure/function ACL commands */
    case SQLCOM_CREATE_PROCEDURE:
    case SQLCOM_CREATE_SPFUNCTION:
    case SQLCOM_DROP_FUNCTION:
    case SQLCOM_DROP_PROCEDURE:
      return DDLCOM_ACL;

    default:
      break;
  }

  return DDLCOM_NONE;
}

size_t max_ddl_info_len() {
  return (mysql_bin_log.is_open() && rds_write_binlog_into_redo)
             ? MAX_DDL_INFO_LEN_WITH_BINLOG
             : MAX_DDL_INFO_STR_LEN;
}

/**
   Store create DDL's dstore specified DD info and generate a CREATE TABLE
   statement.
   TODO: future-proofing code for DD IN InnoDB, borrowed code from
   store_create_info(), to be consolidated with it.

   @param thd Pointer to the THD object.
   @param table_name The name of the table.
   @param packet The buffer to store the generated CREATE TABLE statement with
   dstore specified comments.
   @param create_info_arg Pointer to the HA_CREATE_INFO structure.
   @param show_database Whether to include the database name in the output.
   @param for_show_create_stmt Whether the output is for a SHOW CREATE TABLE
   statement.
   @param table Pointer to the TABLE object representing the table structure.
   @param table_obj Pointer to the data dictionary table object.
   @retval true Failure.
   @retval false Success.
 */
static bool store_create_ddl_info(THD *thd, LEX_CSTRING &table_name,
                                  String *packet,
                                  HA_CREATE_INFO *create_info_arg,
                                  bool show_database, bool for_show_create_stmt,
                                  TABLE *table, dd::Table *table_obj) {
  char tmp[MAX_FIELD_WIDTH], buff[128], def_value_buf[MAX_FIELD_WIDTH];
  String type(tmp, sizeof(tmp), system_charset_info);
  String def_value(def_value_buf, sizeof(def_value_buf), system_charset_info);
  Field **ptr, *field;
  uint primary_key;
  KEY *key_info;

  assert(table != nullptr);
  handler *file = table->file;
  TABLE_SHARE *share = table->s;
  if (file == nullptr || share == nullptr) return true;

  bool foreign_db_mode = (thd->variables.sql_mode & MODE_ANSI) != 0;
  my_bitmap_map *old_map;
  bool error = false;

  bool store_dstore_info = (DB_TYPE_DSTORE == file->ht->db_type);

  DBUG_TRACE;
  DBUG_PRINT("enter", ("table: %s", table->s->table_name.str));

  restore_record(table, s->default_values);  // Get empty record

  if (create_info_arg->options & HA_LEX_CREATE_TMP_TABLE)
    packet->append(STRING_WITH_LEN("CREATE TEMPORARY TABLE "));
  else
    packet->append(STRING_WITH_LEN("CREATE TABLE "));
  if (create_info_arg &&
      (create_info_arg->options & HA_LEX_CREATE_IF_NOT_EXISTS))
    packet->append(STRING_WITH_LEN("IF NOT EXISTS "));

  /*
    Print the database before the table name if told to do that. The
    database name is only printed in the event that it is different
    from the current database.  The main reason for doing this is to
    avoid having to update gazillions of tests and result files, but
    it also saves a few bytes of the binary log.
   */
  const LEX_CSTRING *const db = &table->s->db;
  if (show_database) {
    if (!thd->db().str || strcmp(db->str, thd->db().str)) {
      append_identifier(thd, packet, db->str, db->length);
      packet->append(STRING_WITH_LEN("."));
    }
  }

  append_identifier(thd, packet, table_name.str, table_name.length);
  packet->append(STRING_WITH_LEN(" ("));
  /*
    We need this to get default values from the table
    We have to restore the read_set if we are called from insert in case
    of row based replication.
  */
  old_map = tmp_use_all_columns(table, table->read_set);
  auto grd = create_scope_guard(
      [&]() { tmp_restore_column_map(table->read_set, old_map); });

  /*
    When building CREATE TABLE statement for the SHOW CREATE TABLE (i.e.
    for_show_create_stmt = true), skip generated invisible primary key
    if system variable 'show_gipk_in_create_table_and_information_schema' is set
    to OFF.
  */
  bool skip_gipk =
      (for_show_create_stmt &&
       table_has_generated_invisible_primary_key(table) &&
       !thd->variables.show_gipk_in_create_table_and_information_schema);

  Field **first_field = table->field;
  /*
    Generated invisible primary key column is placed at the first position.
    So skip first column when skip_gipk is set.
  */
  assert(!table_has_generated_invisible_primary_key(table) ||
         is_generated_invisible_primary_key_column_name(
             (*first_field)->field_name));
  if (skip_gipk) first_field++;

  for (ptr = first_field; (field = *ptr); ptr++) {
    // Skip hidden system fields.
    if (field->is_hidden_by_system()) continue;

    enum_field_types field_type = field->real_type();

    if (ptr != first_field) packet->append(STRING_WITH_LEN(","));

    packet->append(STRING_WITH_LEN("  "));
    append_identifier(thd, packet, field->field_name,
                      strlen(field->field_name));
    packet->append(' ');
    // check for surprises from the previous call to Field::sql_type()
    if (type.ptr() != tmp)
      type.set(tmp, sizeof(tmp), system_charset_info);
    else
      type.set_charset(system_charset_info);

    field->sql_type(type);
    /*
      If the session variable 'show_old_temporals' is enabled and the field
      is a temporal type of old format, add a comment to indicate the same.
    */
    if (thd->variables.show_old_temporals &&
        (field_type == MYSQL_TYPE_TIME || field_type == MYSQL_TYPE_DATETIME ||
         field_type == MYSQL_TYPE_TIMESTAMP))
      type.append(" /* 5.5 binary format */");
    packet->append(type.ptr(), type.length(), system_charset_info);

    bool column_has_explicit_collation = false;
    /* We may not have a table_obj for schema_tables. */
    if (table_obj)
      column_has_explicit_collation =
          table_obj->get_column(field->field_name)->is_explicit_collation();

    if (field->has_charset()) {
      /*
        For string types dump charset name only if field charset is same as
        table charset or was explicitly assigned.
      */
      if (field->charset() != share->table_charset ||
          column_has_explicit_collation) {
        packet->append(STRING_WITH_LEN(" CHARACTER SET "));
        packet->append(field->charset()->csname);
      }
      /*
        For string types dump collation name only if
        collation is not primary for the given charset
        or was explicitly assigned.
      */
      if (!(field->charset()->state & MY_CS_PRIMARY) ||
          column_has_explicit_collation ||
          (field->charset() == &my_charset_utf8mb4_0900_ai_ci &&
           share->table_charset != &my_charset_utf8mb4_0900_ai_ci)) {
        packet->append(STRING_WITH_LEN(" COLLATE "));
        packet->append(field->charset()->m_coll_name);
      }
    }

    if (field->gcol_info) {
      packet->append(STRING_WITH_LEN(" GENERATED ALWAYS"));
      packet->append(STRING_WITH_LEN(" AS ("));
      char buffer[128];
      String s(buffer, sizeof(buffer), system_charset_info);
      field->gcol_info->print_expr(thd, &s);
      packet->append(s);
      packet->append(STRING_WITH_LEN(")"));
      if (field->stored_in_db)
        packet->append(STRING_WITH_LEN(" STORED"));
      else
        packet->append(STRING_WITH_LEN(" VIRTUAL"));
    }

    if (field->is_flag_set(NOT_NULL_FLAG))
      packet->append(STRING_WITH_LEN(" NOT NULL"));
    else if (field->type() == MYSQL_TYPE_TIMESTAMP) {
      /*
        TIMESTAMP field require explicit NULL flag, because unlike
        all other fields they are treated as NOT NULL by default.
      */
      packet->append(STRING_WITH_LEN(" NULL"));
    }

    if (field->is_flag_set(NOT_SECONDARY_FLAG))
      packet->append(STRING_WITH_LEN(" NOT SECONDARY"));

    assert(field->type() != MYSQL_TYPE_GEOMETRY);

    switch (field->field_storage_type()) {
      case HA_SM_DEFAULT:
        break;
      case HA_SM_DISK:
        packet->append(STRING_WITH_LEN(" /*!50606 STORAGE DISK */"));
        break;
      case HA_SM_MEMORY:
        packet->append(STRING_WITH_LEN(" /*!50606 STORAGE MEMORY */"));
        break;
      default:
        assert(0);
        break;
    }

    switch (field->column_format()) {
      case COLUMN_FORMAT_TYPE_DEFAULT:
        break;
      case COLUMN_FORMAT_TYPE_FIXED:
        packet->append(STRING_WITH_LEN(" /*!50606 COLUMN_FORMAT FIXED */"));
        break;
      case COLUMN_FORMAT_TYPE_DYNAMIC:
        packet->append(STRING_WITH_LEN(" /*!50606 COLUMN_FORMAT DYNAMIC */"));
        break;
      default:
        assert(0);
        break;
    }

    if (print_default_clause(thd, field, &def_value, true)) {
      packet->append(STRING_WITH_LEN(" DEFAULT "));
      packet->append(def_value.ptr(), def_value.length(), system_charset_info);
    }

    if (print_on_update_clause(field, &def_value, false)) {
      packet->append(STRING_WITH_LEN(" "));
      packet->append(def_value);
    }

    if (field->auto_flags & Field::NEXT_NUMBER)
      packet->append(STRING_WITH_LEN(" AUTO_INCREMENT"));

    // Column visibility attribute
    if (field->is_hidden_by_user())
      packet->append(STRING_WITH_LEN(" /*!80023 INVISIBLE */"));

    if (field->comment.length) {
      packet->append(STRING_WITH_LEN(" COMMENT "));
      append_unescaped(packet, field->comment.str, field->comment.length);
    }

    // Storage engine specific json attributes
    if (field->m_engine_attribute.length) {
      packet->append(STRING_WITH_LEN(" /*!80021 ENGINE_ATTRIBUTE "));
      // append escaped JSON
      append_unescaped(packet, field->m_engine_attribute.str,
                       field->m_engine_attribute.length);
      packet->append(STRING_WITH_LEN(" */"));
    }
    if (field->m_secondary_engine_attribute.length) {
      packet->append(STRING_WITH_LEN(" /*!80021 SECONDARY_ENGINE_ATTRIBUTE "));
      // escape JSON
      append_unescaped(packet, field->m_secondary_engine_attribute.str,
                       field->m_secondary_engine_attribute.length);
      packet->append(STRING_WITH_LEN(" */"));
    }
  }

  key_info = table->key_info;
  /*
    Primary key is always at the first position in the keys list. Skip printing
    primary key definition when skip_gipk is set.
  */
  assert(!table_has_generated_invisible_primary_key(table) ||
         ((key_info->user_defined_key_parts == 1) &&
          is_generated_invisible_primary_key_column_name(
              key_info->key_part->field->field_name)));
  if (skip_gipk) key_info++;

  /* Allow update_create_info to update row type */
  primary_key = share->primary_key;

  for (uint i = skip_gipk ? 1 : 0; i < share->keys; i++, key_info++) {
    KEY_PART_INFO *key_part = key_info->key_part;
    bool found_primary = false;
    packet->append(STRING_WITH_LEN(",  "));

    if (i == primary_key && !strcmp(key_info->name, primary_key_name)) {
      found_primary = true;
      /*
        No space at end, because a space will be added after where the
        identifier would go, but that is not added for primary key.
      */
      packet->append(STRING_WITH_LEN("PRIMARY KEY"));
    } else if (key_info->flags & HA_NOSAME)
      packet->append(STRING_WITH_LEN("UNIQUE KEY "));
    else {
      assert(!(key_info->flags & (HA_FULLTEXT | HA_SPATIAL)));
      packet->append(STRING_WITH_LEN("KEY "));
    }

    if (!found_primary)
      append_identifier(thd, packet, key_info->name, strlen(key_info->name));

    packet->append(STRING_WITH_LEN(" ("));

    for (uint j = 0; j < key_info->user_defined_key_parts; j++, key_part++) {
      if (j) packet->append(',');

      if (key_part->field) {
        // If this fields represents a functional index, print the expression
        // instead of the column name.
        if (key_part->field->is_field_for_functional_index()) {
          assert(key_part->field->gcol_info);

          StringBuffer<STRING_BUFFER_USUAL_SIZE> s;
          s.set_charset(system_charset_info);
          key_part->field->gcol_info->print_expr(thd, &s);
          packet->append("(");
          packet->append(s);
          packet->append(")");
        } else {
          append_identifier(thd, packet, key_part->field->field_name,
                            strlen(key_part->field->field_name));
        }
      }

      if (key_part->field &&
          (key_part->length !=
               table->field[key_part->fieldnr - 1]->key_length() &&
           !(key_info->flags & (HA_FULLTEXT | HA_SPATIAL)))) {
        packet->append_parenthesized((long)key_part->length /
                                     key_part->field->charset()->mbmaxlen);
      }
      if (key_part->key_part_flag & HA_REVERSE_SORT)
        packet->append(STRING_WITH_LEN(" DESC"));
    }
    packet->append(')');
    store_key_options(thd, packet, table, key_info);
    /* FULLTEXT index is unsupported for cde table for now,  WITH PARSER only
     * work on FULLTEXT */
    assert(!key_info->parser);
    if (store_dstore_info && file->ht->get_index_dd_info) {
      file->ht->get_index_dd_info(packet, table_obj, i);
    }
  }

  // Append foreign key constraint definitions to the CREATE TABLE statement.
  print_foreign_key_info(thd, db, table_obj, packet);

  /*
    Append check constraints to the CREATE TABLE statement. All check
    constraints are listed in table check constraint form.
  */
  if (table->table_check_constraint_list != nullptr) {
    for (auto &cc : *table->table_check_constraint_list) {
      packet->append(STRING_WITH_LEN(",  CONSTRAINT "));
      append_identifier(thd, packet, cc.name().str, cc.name().length);

      packet->append(STRING_WITH_LEN(" CHECK ("));
      packet->append(cc.expr_str().str, cc.expr_str().length,
                     system_charset_info);
      packet->append(STRING_WITH_LEN(")"));

      /*
        If check constraint is not-enforced then it is listed with the comment
        "NOT ENFORCED".
      */
      if (!cc.is_enforced()) {
        packet->append(STRING_WITH_LEN(" /*!80016 NOT ENFORCED */"));
      }
    }
  }

  packet->append(STRING_WITH_LEN(")"));

  bool show_tablespace = false;
  if (!foreign_db_mode) {
    // Show tablespace name only if it is explicitly provided by user.
    if (share->tmp_table) {
      // Innodb allows temporary tables in be in system temporary tablespace.
      show_tablespace = share->tablespace;
    } else if (share->tablespace && table_obj) {
      show_tablespace = table_obj->is_explicit_tablespace();
    }

    /* TABLESPACE and STORAGE */
    if (show_tablespace || share->default_storage_media != HA_SM_DEFAULT) {
      packet->append(STRING_WITH_LEN(" /*!50100"));
      if (show_tablespace) {
        packet->append(STRING_WITH_LEN(" TABLESPACE "));
        append_identifier(thd, packet, share->tablespace,
                          strlen(share->tablespace));
      }

      if (share->default_storage_media == HA_SM_DISK)
        packet->append(STRING_WITH_LEN(" STORAGE DISK"));
      if (share->default_storage_media == HA_SM_MEMORY)
        packet->append(STRING_WITH_LEN(" STORAGE MEMORY"));

      packet->append(STRING_WITH_LEN(" */"));
    }

    /* Get Autoextend_size attribute for file_per_table tablespaces. */

    ulonglong autoextend_size{};

    if (create_info_arg != nullptr) {
      if ((create_info_arg->used_fields & HA_CREATE_USED_AUTOEXTEND_SIZE) !=
          0) {
        autoextend_size =
            create_info_arg->m_implicit_tablespace_autoextend_size;
      }
    } else if (!share->tmp_table && table_obj &&
               table_obj->engine() == "InnoDB") {
      /* Get the AUTOEXTEND_SIZE if the tablespace is an implicit tablespace. */
      dd::get_implicit_tablespace_options(thd, table_obj, &autoextend_size);
    }

    /* Print autoextend_size attribute if it is set to a non-zero value */
    if (autoextend_size > 0) {
      char buf[std::numeric_limits<decltype(autoextend_size)>::digits10 + 2];
      int len = my_safe_snprintf(buf, sizeof(buf), "%llu", autoextend_size);
      assert(len < static_cast<int>(sizeof(buf)));
      packet->append(STRING_WITH_LEN(" /*!80023 AUTOEXTEND_SIZE="));
      packet->append(buf, len);
      packet->append(STRING_WITH_LEN(" */"));
    }

    /*
      IF   check_create_info
      THEN add ENGINE only if it was used when creating the table
    */
    if (!create_info_arg ||
        (create_info_arg->used_fields & HA_CREATE_USED_ENGINE) ||
        store_dstore_info) {
      packet->append(STRING_WITH_LEN(" ENGINE="));

      /* partition table if not supported on cde table for now */
      assert(!table->part_info);
      packet->append(file->table_type());
    }

    /*
      Add AUTO_INCREMENT=... if there is an AUTO_INCREMENT column,
      and NEXT_ID > 1 (the default).  We must not print the clause
      for engines that do not support this as it would break the
      import of dumps, but as of this writing, the test for whether
      AUTO_INCREMENT columns are allowed and whether AUTO_INCREMENT=...
      is supported is identical, !(file->table_flags() & HA_NO_AUTO_INCREMENT))
      Because of that, we do not explicitly test for the feature,
      but may extrapolate its existence from that of an AUTO_INCREMENT column.

      If table has a generated invisible primary key and skip_gipk is set,
      then we should not print the AUTO_INCREMENT as AUTO_INCREMENT column
      (generated invisible primary key column) is skipped with this setting.
    */

    if (create_info_arg->auto_increment_value > 1 && !skip_gipk) {
      char *end;
      packet->append(STRING_WITH_LEN(" AUTO_INCREMENT="));
      end = longlong10_to_str(create_info_arg->auto_increment_value, buff, 10);
      packet->append(buff, (uint)(end - buff));
    }

    if (share->table_charset) {
      /*
        IF   check_create_info
        THEN add DEFAULT CHARSET only if it was used when creating the table
      */
      if (!create_info_arg ||
          (create_info_arg->used_fields & HA_CREATE_USED_DEFAULT_CHARSET)) {
        packet->append(STRING_WITH_LEN(" DEFAULT CHARSET="));
        packet->append(share->table_charset->csname);
        if (!(share->table_charset->state & MY_CS_PRIMARY) ||
            share->table_charset == &my_charset_utf8mb4_0900_ai_ci) {
          packet->append(STRING_WITH_LEN(" COLLATE="));
          packet->append(table->s->table_charset->m_coll_name);
        }
      }
    }

    if (share->min_rows) {
      char *end;
      packet->append(STRING_WITH_LEN(" MIN_ROWS="));
      end = longlong10_to_str(share->min_rows, buff, 10);
      packet->append(buff, (uint)(end - buff));
    }

    if (share->max_rows) {
      char *end;
      packet->append(STRING_WITH_LEN(" MAX_ROWS="));
      end = longlong10_to_str(share->max_rows, buff, 10);
      packet->append(buff, (uint)(end - buff));
    }

    if (share->avg_row_length) {
      char *end;
      packet->append(STRING_WITH_LEN(" AVG_ROW_LENGTH="));
      end = longlong10_to_str(share->avg_row_length, buff, 10);
      packet->append(buff, (uint)(end - buff));
    }

    if (share->db_create_options & HA_OPTION_PACK_KEYS)
      packet->append(STRING_WITH_LEN(" PACK_KEYS=1"));
    if (share->db_create_options & HA_OPTION_NO_PACK_KEYS)
      packet->append(STRING_WITH_LEN(" PACK_KEYS=0"));
    if (share->db_create_options & HA_OPTION_STATS_PERSISTENT)
      packet->append(STRING_WITH_LEN(" STATS_PERSISTENT=1"));
    if (share->db_create_options & HA_OPTION_NO_STATS_PERSISTENT)
      packet->append(STRING_WITH_LEN(" STATS_PERSISTENT=0"));
    if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON)
      packet->append(STRING_WITH_LEN(" STATS_AUTO_RECALC=1"));
    else if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF)
      packet->append(STRING_WITH_LEN(" STATS_AUTO_RECALC=0"));
    if (share->stats_sample_pages != 0) {
      char *end;
      packet->append(STRING_WITH_LEN(" STATS_SAMPLE_PAGES="));
      end = longlong10_to_str(share->stats_sample_pages, buff, 10);
      packet->append(buff, (uint)(end - buff));
    }
    /* We use CHECKSUM, instead of TABLE_CHECKSUM, for backward compatibility */
    if (share->db_create_options & HA_OPTION_CHECKSUM)
      packet->append(STRING_WITH_LEN(" CHECKSUM=1"));
    if (share->db_create_options & HA_OPTION_DELAY_KEY_WRITE)
      packet->append(STRING_WITH_LEN(" DELAY_KEY_WRITE=1"));

    /*
      If 'show_create_table_verbosity' is enabled, the row format would
      be displayed in the output of SHOW CREATE TABLE even if default
      row format is used. Otherwise only the explicitly mentioned
      row format would be displayed.
    */
    if (thd->variables.show_create_table_verbosity) {
      packet->append(STRING_WITH_LEN(" ROW_FORMAT="));
      packet->append(ha_row_type[(uint)share->real_row_type]);
    } else if (create_info_arg->row_type != ROW_TYPE_DEFAULT) {
      packet->append(STRING_WITH_LEN(" ROW_FORMAT="));
      packet->append(ha_row_type[(uint)create_info_arg->row_type]);
    }
    if (table->s->key_block_size) {
      char *end;
      packet->append(STRING_WITH_LEN(" KEY_BLOCK_SIZE="));
      end = longlong10_to_str(table->s->key_block_size, buff, 10);
      packet->append(buff, (uint)(end - buff));
    }
    if (table->s->compress.length) {
      packet->append(STRING_WITH_LEN(" COMPRESSION="));
      append_unescaped(packet, share->compress.str, share->compress.length);
    }
    bool print_encryption = false;
    if (should_print_encryption_clause(thd, share, &print_encryption))
      return true;
    if (print_encryption) {
      /*
        Add versioned comment when there is TABLESPACE clause displayed and
        the table uses general tablespace.
      */
      bool uses_general_tablespace = false;
      if (table_obj)
        uses_general_tablespace =
            show_tablespace && dd::uses_general_tablespace(*table_obj);
      if (uses_general_tablespace) packet->append(STRING_WITH_LEN(" /*!80016"));

      packet->append(STRING_WITH_LEN(" ENCRYPTION="));
      if (share->encrypt_type.length) {
        append_unescaped(packet, share->encrypt_type.str,
                         share->encrypt_type.length);
      } else {
        /*
          We print ENCRYPTION='N' only in case user did not explicitly
          provide ENCRYPTION clause and schema has default_encryption 'Y'.
          In other words, if there is no ENCRYPTION clause supplied, then
          it is always unencrypted table. Server always maintains
          ENCRYPTION clause for encrypted tables, even if user did not
          supply the clause explicitly.
        */
        packet->append(STRING_WITH_LEN("\'N\'"));
      }

      if (uses_general_tablespace) packet->append(STRING_WITH_LEN(" */"));
    }
    table->file->append_create_info(packet);
    if (share->comment.length) {
      packet->append(STRING_WITH_LEN(" COMMENT="));
      append_unescaped(packet, share->comment.str, share->comment.length);
    }
    if (share->connect_string.length) {
      packet->append(STRING_WITH_LEN(" CONNECTION="));
      append_unescaped(packet, share->connect_string.str,
                       share->connect_string.length);
    }
    if (share->has_secondary_engine() &&
        !thd->variables.show_create_table_skip_secondary_engine) {
      packet->append(" SECONDARY_ENGINE=");
      packet->append(share->secondary_engine.str,
                     share->secondary_engine.length);
    }

    if (share->engine_attribute.length) {
      packet->append(STRING_WITH_LEN(" /*!80021 ENGINE_ATTRIBUTE="));
      append_unescaped(packet, share->engine_attribute.str,
                       share->engine_attribute.length);
      packet->append(STRING_WITH_LEN(" */"));
    }
    if (share->secondary_engine_attribute.length) {
      packet->append(STRING_WITH_LEN(" /*!80021 SECONDARY_ENGINE_ATTRIBUTE="));
      // escape JSON
      append_unescaped(packet, share->secondary_engine_attribute.str,
                       share->secondary_engine_attribute.length);
      packet->append(STRING_WITH_LEN(" */"));
    }
    append_directory(thd, packet, "DATA", create_info_arg->data_file_name);
    append_directory(thd, packet, "INDEX", create_info_arg->index_file_name);

    if (store_dstore_info && file->ht->get_table_dd_info) {
      file->ht->get_table_dd_info(packet, table_obj);
    }
  }

  return error;
}

/**
 * Generate the initial part of the DDL sql, "USE db_name; DROP TABLE IF
 * EXISTS"
 *
 * @param thd The thread handle (THD) containing the current database
 * information.
 * @param gen_query The string to which the prefix will be appended. It must be
 *                  empty at the start of this function.
 * @param form Pointer to the TABLE object representing the table structure.
 * default is nullptr.
 */
static void generate_prefix_ddl_sql(THD *thd, String &gen_query,
                                    TABLE *form = nullptr) {
  const char *db_name = nullptr;
  if (form && form->s && form->s->db.str) {
    db_name = form->s->db.str;
  } else if (thd->db().str) {
    db_name = thd->db().str;
  }

  if (db_name) {
    gen_query.append("USE ");
    gen_query.append(db_name);
    gen_query.append("; ");
  }

  gen_query.append("DROP TABLE IF EXISTS ");
}

bool generate_dstore_create_ddl_info(THD *thd, HA_CREATE_INFO *create_info,
                                     TABLE *form, dd::Table *table_def) {
  if (my_strcasecmp(system_charset_info, table_def->engine().c_str(),
                    DSTORE_ENGINE_NAME) != 0)
    return true;
  if (form == nullptr || form->s == nullptr) return true;

  bool result = false;
  String gen_query;
  gen_query.set_charset(system_charset_info);
  gen_query.length(0);
  generate_prefix_ddl_sql(thd, gen_query, form);

  /* confirm table name */
  LEX_CSTRING table_name;
  /* if internal tmp tables, we use the table name from thd table list */
  if (is_prefix(form->s->table_name.str, tmp_file_prefix)) {
    Table_ref *table_list = thd->lex->query_block->get_table_list();
    if (table_list) {
      table_name.str = table_list->table_name;
      table_name.length = table_list->table_name_length;
    }
  } else {
    table_name = form->s->table_name;
  }
  if (table_name.length == 0) return true;

  append_identifier(thd, &gen_query, table_name.str, table_name.length);
  gen_query.append(" /*!80041 dstore_ddl_comment='FOR_DD_REPLAY=1;' */; ");

  /* generate "CREATE TABLE ..." */
  result = store_create_ddl_info(thd, table_name, &gen_query, create_info, true,
                                 false, form, table_def);

  DBUG_EXECUTE_IF("dstore_create_set_ddl_sql_error", { gen_query.length(0); });
  if (!result && thd->set_ddl_sql(gen_query.lex_cstring(), true)) {
    result = true;
  }

  return result;
}

bool generate_dstore_drop_ddl_info(THD *thd) {
  String gen_query;
  gen_query.length(0);
  gen_query.set_charset(system_charset_info);

  if (thd->lex->sql_command == SQLCOM_DROP_TABLE) {
    generate_prefix_ddl_sql(thd, gen_query);

    Table_ref *tables = thd->lex->query_block->get_table_list();
    for (Table_ref *table = tables; table; table = table->next_local) {
      if (find_temporary_table(thd, table) != nullptr) {
        continue;
      }
      if (table->db) {
        gen_query.append(String(table->db, system_charset_info));
        gen_query.append('.');
      }
      gen_query.append(String(table->table_name, system_charset_info));
      gen_query.append(",");
    }
    gen_query.chop();
  } else if (thd->lex->sql_command == SQLCOM_DROP_DB) {
    gen_query.append(thd->query().str);
  } else {
    return true;
  }

  gen_query.append(" /*!80041 dstore_ddl_comment='FOR_DD_REPLAY=1;' */");
  DBUG_EXECUTE_IF("dstore_drop_set_ddl_sql_error", { gen_query.length(0); });
  if (thd->set_ddl_sql(gen_query.lex_cstring(), true)) {
    return true;
  }

  return false;
}

bool generate_dstore_alter_ddl_info(THD *thd, HA_CREATE_INFO *create_info,
                                    TABLE *old_table, dd::Table *old_table_def,
                                    TABLE *new_table, dd::Table *new_table_def,
                                    Alter_table_ctx *alter_ctx) {
  if (my_strcasecmp(system_charset_info, old_table_def->engine().c_str(),
                    DSTORE_ENGINE_NAME) != 0)
    return true;

  if (my_strcasecmp(system_charset_info, new_table_def->engine().c_str(),
                    DSTORE_ENGINE_NAME) != 0)
    return true;

  if (old_table == nullptr || old_table->s == nullptr) return true;

  if (new_table == nullptr || new_table->s == nullptr) return true;

  /* generate "DROP TABLE ..." for old table */
  String gen_query;
  gen_query.set_charset(system_charset_info);
  gen_query.length(0);
  generate_prefix_ddl_sql(thd, gen_query, old_table);

  LEX_CSTRING table_name;
  table_name = old_table->s->table_name;
  if (table_name.length == 0) return true;

  append_identifier(thd, &gen_query, table_name.str, table_name.length);
  gen_query.append(" /*!80041 dstore_ddl_comment='FOR_DD_REPLAY=1;' */; ");

  /* generate "CREATE TABLE ..." for new table */
  if (alter_ctx->is_table_renamed()) {
    /* If table has renamed, we need to change the db name and table name. */
    if (my_strcasecmp(system_charset_info, old_table->s->db.str,
                      new_table->s->db.str)) {
      gen_query.append("USE ");
      gen_query.append(new_table->s->db.str);
      gen_query.append("; ");
    }
    table_name.str = alter_ctx->new_alias;
    table_name.length = strlen(alter_ctx->new_alias);
    gen_query.append("DROP TABLE IF EXISTS ");
    append_identifier(thd, &gen_query, table_name.str, table_name.length);
    gen_query.append(" /*!80041 dstore_ddl_comment='FOR_DD_REPLAY=1;' */; ");
  }

  bool result = store_create_ddl_info(thd, table_name, &gen_query, create_info,
                                      true, false, new_table, new_table_def);

  DBUG_EXECUTE_IF("dstore_alter_set_ddl_sql_error", { gen_query.length(0); });
  if (!result && thd->set_ddl_sql(gen_query.lex_cstring(), true)) {
    result = true;
  }

  return result;
}

/**
   Execute DDL information queries.
   TODO: future-proofing code for DD IN InnoDB

   This function parses and executes queries of DDL info stored in a buffer. It
   handles multiple statements separated by semicolons and executes them
   sequentially using a background connection.

   The function performs the following steps:
   1. Initializes a background connection for executing DDL queries.
   2. Parses the DDL information buffer into individual SQL statements.
   3. Executes each parsed statement using the background connection.
   4. Handles errors during execution and logs them appropriately.
   5. Cleans up resources and restores the original parser state.

   @param thd Pointer to the THD object.
   @param query_buffer Pointer to the buffer containing DDL info queries.
   @param buffer_len Length of the query buffer.
 */
static void execute_ddl_info_queries(THD *thd, const char *query_buffer,
                                     size_t buffer_len) {
  std::string ddl_info(query_buffer, buffer_len);
  Ed_connection bg_con(thd);

  Parser_state *old_ps = thd->m_parser_state;
  thd->m_parser_state = nullptr;

  Parser_state parser_state;
  if (parser_state.init(thd, ddl_info.c_str(), ddl_info.length())) {
    return;
  }
  parser_state.m_lip.multi_statements = true;

  Protocol_classic *protocol = thd->get_protocol_classic();

  auto save_client_capabilities = protocol->get_client_capabilities();
  protocol->add_client_capability(CLIENT_MULTI_QUERIES);

  const char *start = ddl_info.c_str();
  const char *end = ddl_info.c_str() + ddl_info.length();

  do {
    parser_state.reset(start, end - start);
    lex_start(thd);
    // parse one single statement
    if (parse_sql(thd, &parser_state, nullptr)) {
      break;
    }
    // extract current statement
    LEX_STRING parsed_stmt;
    parsed_stmt.str = const_cast<char *>(start);
    if (!parser_state.m_lip.found_semicolon) {
      parsed_stmt.length = end - start;
    } else {
      parsed_stmt.length = (parser_state.m_lip.found_semicolon - start) - 1;
      // update the pos for next statement
      start = parser_state.m_lip.found_semicolon + 1;
    }

    thd->set_query_id(next_query_id());
    Disable_binlog_guard disable_binlog(thd);
    Disable_sql_log_bin_guard disable_sql_log_bin(thd);

    /* executing last statement, need to insert trace table */
    if (!parser_state.m_lip.found_semicolon) {
      thd->set_write_to_ddl_info_replay_trace(true);
    }
    /* execte current statement */
    if (bg_con.execute_direct(parsed_stmt)) {
      std::string stmt(parsed_stmt.str, parsed_stmt.length);
      LogErr(ERROR_LEVEL, ER_DDL_INFO_REPLAY_FAILED, stmt.c_str());
    }

    lex_end(thd->lex);
    thd->lex->reset();
  } while (parser_state.m_lip.found_semicolon);

  thd->m_parser_state = old_ps;
  protocol->set_client_capabilities(save_client_capabilities);
}

bool replay_dd_query_info(uint64_t end_lsn, const unsigned char *buffer,
                          uint32_t buffer_size) {
  int error = 0;
  uint64_t max_lsn = 0;
  THD *thd = replay_thd;

  /* read the max lsn in table "ddl_info_replay_trace", check if end_lsn >
   * max_lsn; if no records in the table(means we are inserting the first
   * record), return HA_ERR_END_OF_FILE */
  error = get_max_ddl_query_trace_lsn(thd, max_lsn);
  if (error != 0 && error != HA_ERR_END_OF_FILE) return true;
  if (error == 0 && end_lsn <= max_lsn) {
    /* the end_lsn is already replayed, skip it. */
    DBUG_PRINT("debug",
               ("less than the max_lsn %ld in mysql.ddl_info_replay_trace, the "
                "end_lsn %ld is already replayed, skip it",
                max_lsn, end_lsn));
    return false;
  }

  /* to make sure records inserting in trace table and ddl_info executing in one
   * transation, we save the buffer and end_lsn in thd for now and perform
   * inserting during execute_ddl_info_queries. */
  thd->save_ddl_info_for_trace(end_lsn, buffer, buffer_size);

  /* execute ddl_info queries */
  execute_ddl_info_queries(thd, reinterpret_cast<const char *>(buffer),
                           buffer_size);

  return false;
}

void standby_replay_dd_query_info(uint64_t end_lsn, const unsigned char *buffer,
                                  uint32_t buffer_size) {
  int error = 0;
  uint64_t max_lsn = 0;
  /* prepare thd */
  my_thread_init();
  THD *thd = new THD(true);
  thd->set_new_thread_id();
  thd->security_context()->skip_grants();
  thd->thread_stack = reinterpret_cast<char *>(&thd);
  thd->store_globals();
  thd->for_ddl_info_replay = true;

  /* read the max lsn in table "ddl_info_replay_trace", check if end_lsn >
   * max_lsn; if no records in the table(means we are inserting the first
   * record), return HA_ERR_END_OF_FILE */
  error = get_max_ddl_query_trace_lsn(thd, max_lsn);
  // LCOV_EXCL_START
  if (error != 0 && error != HA_ERR_END_OF_FILE) {
    LogErr(ERROR_LEVEL, ER_WAL_RPL_WALLOG_DDL_REPLAY_SELECT_TRACE_TALBE_ERROR,
           error, buffer_size, buffer);
    my_abort();
  }
  // LCOV_EXCL_STOP

  if ((error == 0 && end_lsn <= max_lsn) ||
      DBUG_EVALUATE_IF("standby_ddl_already_replayed", true, false)) {
    /* the end_lsn is already replayed, skip it. */
    DBUG_PRINT("debug",
               ("less than the max_lsn %ld in mysql.ddl_info_replay_trace, the "
                "end_lsn %ld is already replayed, skip it",
                max_lsn, end_lsn));
    thd->release_resources();
    delete thd;
    thd = nullptr;
    my_thread_end();
    return;
  }

  /* to make sure records inserting in trace table and ddl_info executing in one
   * transation, we save the buffer and end_lsn in thd for now and perform
   * inserting during execute_ddl_info_queries. */
  thd->save_ddl_info_for_trace(end_lsn, buffer, buffer_size);

  /* execute ddl_info queries */
  execute_ddl_info_queries(thd, reinterpret_cast<const char *>(buffer),
                           buffer_size);
  /* destroy the thd */
  thd->release_resources();
  delete thd;
  thd = nullptr;
  my_thread_end();
}

static bool should_skip_ddl_info_op(THD *thd) {
  enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);
  switch (sql_command) {
    case SQLCOM_ALTER_RESOURCE_GROUP:
    case SQLCOM_ANALYZE:
    case SQLCOM_CREATE_RESOURCE_GROUP:
    case SQLCOM_DROP_RESOURCE_GROUP:
      return true;
    default:
      break;
  }
  return false;
}

bool need_ddl_info_xid(THD *thd) {
  return need_ddl_info(thd) && rds_dstore_enable_atomic_ddl;
}

bool need_ddl_info_sql(THD *thd) {
  return need_ddl_info(thd) && rds_dstore_write_ddl_sql_into_redo;
}

bool need_ddl_info(THD *thd) {
  if (!rds_dstore_write_ddl_info_into_redo ||
      storage_engine_mode != static_cast<ulong>(ONLY_DSTORE) ||
      thd->is_bootstrap_system_thread() ||
      thd->system_thread == SYSTEM_THREAD_BACKGROUND ||
      thd->for_ddl_info_replay) {
    return false;
  }
  DBUG_EXECUTE_IF("check_ddl_info_written_state", {
    static bool first_check = true;
    if (first_check) {
      assert(thd->is_ddl_info_written == false);
      first_check = false;
    }
  });
  if (get_ddl_command_type(thd) == DDLCOM_NONE ||
      should_skip_ddl_info_op(thd) || thd->is_ddl_info_written) {
    return false;
  }
  return true;
}

void write_only_ddl_info_into_redo(THD *thd) {
  uint32_t position = 0;
  uint32_t sql_size = need_ddl_info_sql(thd) ? thd->ddl_info_sql().length() : 0;
  /* flag(size 4) + trx_id(size 8) + ddl_sql_len(size 2) + ddl_sql(size) */
  size_t buff_size = BUF_SIZE_FOR_FLAG + BUF_SIZE_FOR_TRX_ID +
                     BUF_SIZE_FOR_DDL_SQL_LEN + sql_size;
  uchar *buff = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, buff_size, MYF(0));
  if (buff == nullptr) {
    sql_print_error(
        "write_only_ddl_info_into_redo: Couldn't allocate memory!\n");
    return;
  }
  uint32_t flags = 0;
  flags |= HAS_DDL_INFO;
  int4store(buff + position, flags);
  // write trx_id
  position += BUF_SIZE_FOR_FLAG;
  int8store(buff + position,
            need_ddl_info_xid(thd)
                ? thd->get_transaction()->xid_state()->get_xid()->get_my_xid()
                : 0);
  position += BUF_SIZE_FOR_TRX_ID;
  // write ddl_sql_len
  int2store(buff + position, sql_size);
  position += BUF_SIZE_FOR_DDL_SQL_LEN;
  // write ddl_sql
  if (sql_size != 0 && (memcpy_s(buff + position, sql_size,
                                 thd->ddl_info_sql().ptr(), sql_size) != 0)) {
    sql_print_error("write_only_ddl_info_into_redo: memory copy error!\n");
    return;
  }
  sql_print_information("write ddl info into wal ddl sql_size %u.", sql_size);
  ha_commit_wals(thd, buff, buff_size);
  my_free(buff);
}

void restore_replay_ddl_info() {
  using record_reader = BasicRecordFileStorage<DDLInfoRecord>::Reader;
  int error = 0;
  /* in normal startup process, g_ddl_info_store_file has not ever opened. skip
   * replay process */
  if (!g_ddl_info_store_file.isOpen()) return;

  /* read the ddl_info file */
  std::unique_ptr<record_reader> reader(
      g_ddl_info_store_file.createReaderPtr());
  if (reader.get() == nullptr) {
    sql_print_error(
        "restore_replay_ddl_info: failed to create ddl_info_record_reader!\n");
    return;
  }

  DBUG_PRINT("debug", ("%d records in total in ddl_info store file",
                       reader->get_total_records()));

  /* prepare thd */
  replay_thd = create_thd(true, true, true, 0, 0);
  replay_thd->set_new_thread_id();
  replay_thd->for_ddl_info_replay = true;

  reader->init();
  while (reader->has_more_records()) {
    DDLInfoRecord record;
    error = reader->get_next(record);

    if (error != RECORD_OP_SUCCESS) {
      sql_print_error("read ddl info failed, error=%d\n", error);
      break;
    }

    /* replay this item of ddl info */
    if (replay_dd_query_info(record.lsn(), record.data(), record.dataSize())) {
      sql_print_error("replay ddl info failed. ddl info statement is %s:\n",
                      record.data());
    }
  }
  /* reset/destroy the thd */
  destroy_thd(replay_thd);
}

bool ddl_concurrency_throttle(THD *thd) {
  uint64_t thread_id = thd->thread_id();
  constexpr uint64_t DDL_CONCURRENCY_CHECK_INTERVAL_MS = 10;
  constexpr uint64_t DDL_CONCURRENCY_WARN_LOOP_INTERVAL = 100 * 60;
  uint64_t loop_cnt = 0;
  const uint64_t timeout_ms =
      static_cast<uint64_t>(rds_dstore_ddl_throttle_timeout) * 1000ULL;
  const auto wait_begin = std::chrono::steady_clock::now();
  while (true) {
    if (thd->is_killed()) {
      if (!thd->is_error()) {
        thd->send_kill_message();
      }
      return false;
    }
    {
      std::lock_guard<std::mutex> mutexGuard(ddl_throttle_mutex);
      auto iter = ongoing_ddl_threads.find(thread_id);
      if (iter != ongoing_ddl_threads.end()) {
        /* Already ongoing. DDL query may not re-entry this func which
        lead to this branch. But we reserve this for safty. */
        return false;
      }
      if (ongoing_ddl_threads.size() < rds_dstore_ddl_max_concurrency) {
        ongoing_ddl_threads.emplace(thread_id);
        return true;
      }
    }
    const uint64_t waited_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - wait_begin)
            .count());
    if (waited_ms >= timeout_ms) {
      my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      sql_print_warning(
          "DDL concurrency throttle timeout for %lu ms, thread_id:%lu.",
          waited_ms, thread_id);
      return false;
    }
    DEBUG_SYNC_C("ddl_concurrency_throttle_before_sleep");
    std::this_thread::sleep_for(
        std::chrono::milliseconds(DDL_CONCURRENCY_CHECK_INTERVAL_MS));
    ++loop_cnt;
    if (loop_cnt != 0 && loop_cnt % DDL_CONCURRENCY_WARN_LOOP_INTERVAL == 0) {
      sql_print_warning("DDL concurrency throttled for %lu s.\n",
                        loop_cnt * DDL_CONCURRENCY_CHECK_INTERVAL_MS / 1000);
    }
  }
  return false;
}

void ddl_concurrency_release(uint64_t thread_id) {
  std::lock_guard<std::mutex> mutexGuard(ddl_throttle_mutex);
  uint32_t ret = ongoing_ddl_threads.erase(thread_id);
  if (unlikely(ret != 1)) {
    sql_print_warning(
        "Not found ddl thread:%lu to release concurrency, ret:%u.\n", thread_id,
        ret);
  }
}
