/* Copyright (C) 2025, Huawei Technologies Co., Ltd. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
   USA
*/

#include "rds_comment.h"
#include "field.h"
#include "log.h"
#include "m_string.h"
#include "rpl_table_access.h"
#include "sql_base.h"
#include "sql_parse.h"

constexpr const char *DATABASE_TYPE = "DB";

enum RDS_COMMENT_FIELDS {
  OBJECT_TYPE,
  OBJECT_NAME,
  COMMENT,
  STATUS,
  CREATE_AT,
  UPDATE_AT,
  RDS_COMMENT_FIELDS_COUNT
};

struct rds_comment_table_record {
  LEX_CSTRING object_type;
  LEX_CSTRING object_name;
  LEX_CSTRING comment;
  explicit rds_comment_table_record(const char *type) {
    object_type.str = type;
    object_type.length = strlen(type);
    object_name.str = nullptr;
    object_name.length = 0;
    comment.str = nullptr;
    comment.length = 0;
  }
};

class rds_comment_table_access : public System_table_access {
 public:
  static const LEX_CSTRING TABLE_SCHEMA;
  static const LEX_CSTRING TABLE_NAME;

  explicit rds_comment_table_access(THD *thd)
      : m_thd(nullptr),
        m_origin_thd(thd),
        m_table(NULL),
        m_need_commit(false),
        m_error(0) {}
  virtual ~rds_comment_table_access() {}

  bool open_comment_table();
  bool close_comment_table();

  int delete_row(const rds_comment_table_record &record);

 private:
  // thd used only for operating rds_comment.
  THD *m_thd;

  // origin user thd, restore to this thd after operation.
  THD *m_origin_thd;

  TABLE *m_table;

  // mark whether any records changed.
  bool m_need_commit;

  int m_error;
  Open_tables_backup m_backup;
  void before_open(THD *thd) override;
  THD *create_thd();

  bool fill_field(Field *field, const LEX_CSTRING &value);
};

const LEX_CSTRING rds_comment_table_access::TABLE_SCHEMA = {
    STRING_WITH_LEN("mysql")};
const LEX_CSTRING rds_comment_table_access::TABLE_NAME = {
    STRING_WITH_LEN("rds_comment")};

void rds_comment_table_access::before_open(THD *thd MY_ATTRIBUTE((unused))) {
  DBUG_ENTER("rds_comment_table_access::before_open");

  /**
    Allow to operate the rds_comment table while disconnecting
    the session.
  */
  m_flags = (MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
             MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH |
             MYSQL_LOCK_IGNORE_TIMEOUT | MYSQL_OPEN_IGNORE_KILLED);

  DBUG_VOID_RETURN;
}

/**
 * open comment table, we disable binlog here.
 * @return false OK
 *         true Error
 */
bool rds_comment_table_access::open_comment_table() {
  DBUG_ENTER("rds_comment_table_access::open_comment_table");
  m_thd = create_thd();

  bool err =
      open_table(m_thd, TABLE_SCHEMA, TABLE_NAME, RDS_COMMENT_FIELDS_COUNT,
                 TL_WRITE, &m_table, &m_backup);

  DBUG_EXECUTE_IF("rds_comment_table_access_open_failed", err = true;);

  if (err) {
    sql_print_error("Failed to open rds_comment table.");
  }

  DBUG_RETURN(err);
}

/**
 * close comment table, we didn't restore binlog here,
 * because this thd is be dropping.
 * @return false OK
 *         true Error
 */
bool rds_comment_table_access::close_comment_table() {
  DBUG_ENTER("rds_comment_table_access::close_comment_table");

  bool err = close_table(m_thd, m_table, &m_backup, m_error, m_need_commit);

  /**
    If err is true this means that there was some problem during
    FLUSH LOGS commit phase.
  */

  DBUG_EXECUTE_IF("rds_comment_table_access_close_failed", err = true;);

  if (err) {
    sql_print_error("Failed to close rds_comment table.");
  }

  drop_thd(m_thd);
  m_origin_thd->store_globals();
  DBUG_RETURN(err);
}

/**
 * File fields
 * @param field  field to fill
 * @param value  field value
 * @return false: OK
 *         true: error
 */
bool rds_comment_table_access::fill_field(Field *field,
                                          const LEX_CSTRING &value) {
  field->set_notnull();
  if (field->store(value.str, value.length, system_charset_info) ||
      DBUG_EVALUATE_IF("rds_comment_table_fill_fields_failed", true, false)) {
    sql_print_error("Fill column '%s.%s' failed.", *field->table_name,
                    field->field_name);
    return true;
  }
  return false;
}

/**
 * delete a row from rds_comment according to the given record.
 * @param record: record to delete.
 * @return 0: OK
 *         !=0 Error
 */
int rds_comment_table_access::delete_row(
    const rds_comment_table_record &record) {
  Field **fields = m_table->field;
  uchar user_key[MAX_KEY_LENGTH];

  empty_record(m_table);
  if (fill_field(fields[OBJECT_TYPE], record.object_type) ||
      fill_field(fields[OBJECT_NAME], record.object_name)) {
    m_error = 1;
    return m_error;
  }

  key_copy(user_key, m_table->record[0], m_table->key_info,
           m_table->key_info->key_length);

  m_error = m_table->file->ha_index_init(0, true);
  DBUG_EXECUTE_IF("rds_comment_index_init_fail", m_error = 1;);
  if (m_error) {
    sql_print_error("Init rds_comment table index failed.");
    goto end;
  }

  m_error = m_table->file->ha_index_read_map(m_table->record[0], user_key,
                                             HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  DBUG_EXECUTE_IF("rds_comment_table_key_not_found", m_error = 1;);
  if (m_error) {
    /**
     * There's no record exist, that means database or user doesn't have a
     * comment, it's not an error, so swallow it.
     */
    m_error = 0;
  } else {
    m_error = m_table->file->ha_delete_row(m_table->record[0]);
    DBUG_EXECUTE_IF("rds_comment_table_delete_failed", m_error = 1;);
    if (m_error) {
      sql_print_error("Delete from rds_comment table failed.");
    } else {
      /*
       * Mark we already modified some records, will commit transaction when
       * close table.
       */
      m_need_commit = true;
    }
  }

end:
  if (m_table->file->inited != handler::NONE) {
    if (m_table->file->ha_index_end()) {
      sql_print_error("End rds_comment table index failed.");
    }
  }
  return m_error;
}

/**
 * create a new thd for operating rds_comment.
 * we don't use user thd because we may change something in thd.
 * @return THD: new thd for operating rds_comment.
 */
THD *rds_comment_table_access::create_thd() {
  THD *thd = System_table_access::create_thd();

  if (thd && (!thd->is_cmd_skip_readonly())) {
    thd->set_skip_readonly_check();
  }

  if (thd) {
    thd->tx_read_only = false;
    // Do not record in binlog
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
  }
  return (thd);
}

/**
 * wrapper for delete_row, open table before deleting and
 * close table after it.
 * @param thd: user origin thd(for restore).
 * @param record: record to delete.
 * @return false: OK
 *         true: Error
 */
bool do_delete(THD *thd, const rds_comment_table_record &record) {
  rds_comment_table_access rds_comment_table(thd);
  bool err = rds_comment_table.open_comment_table();
  if (!err) {
    err = (rds_comment_table.delete_row(record) != 0);
  }
  err |= rds_comment_table.close_comment_table();
  return err;
}

/**
 * Delete a database comment, for caller outside.
 * @param thd: user origin thd(for restore).
 * @param db: db name.
 * @return false: OK
 *         true: Error
 */
bool delete_db_comment(THD *thd, const LEX_CSTRING &db) {
  // if drop mysql database, no need to delete this comment
  // all table will gone anyway.
  if (!my_strcasecmp(system_charset_info, db.str,
                     rds_comment_table_access::TABLE_SCHEMA.str)) {
    return false;
  }
  rds_comment_table_record record(DATABASE_TYPE);
  record.object_name = db;
  return do_delete(thd, record);
}