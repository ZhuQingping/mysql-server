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

#ifndef SQL_FILTER_TABLE_INCLUDED
#define SQL_FILTER_TABLE_INCLUDED

#include "sql/common/table.h"
#include "sql/common/table_common.h"
#include "sql/sql_filter/sql_filter_table_common.h"

/**
  Sqlfilter table reader helper.
*/
class Sql_filter_reader : public Conf_reader {
 public:
  explicit Sql_filter_reader(THD *thd, TABLE *table, MEM_ROOT *mem_root)
      : Conf_reader(thd, table, mem_root) {}

  virtual ~Sql_filter_reader() {}
  /**
    Push invalid sqlfilter record warning

    @param[in]      record        Sqlfilter record
    @param[in]      when          Operation
    @param[in]      msg           Error message
  */
  virtual void row_warning(Conf_record *record, const char *when,
                           const char *msg) override;
  /**
    Reconstruct and Report error by adding handler error.

    @param[in]      errcode     handler error.
  */
  virtual void print_ha_error(int errcode) override;
  /**
    Log the conf table error.

    @param[in]    err     Conf error type
  */
  virtual void log_error(Conf_error err) override;
  /**
    Save the row value into sqlfilter_record structure.

    @param[out]   record      Sqlfilter record
  */
  virtual void read_attributes(Conf_record *record) override;

  /* Create new sqlfilter record */
  virtual Conf_record *new_record() override;
};

class Sql_filter_writer : public Conf_writer {
 public:
  explicit Sql_filter_writer(THD *thd, TABLE *table, MEM_ROOT *mem_root,
                             Conf_table_op op_type)
      : Conf_writer(thd, table, mem_root, op_type) {}

  virtual void print_ha_error(int errcode) override;
  /**
    Store the sqlfilter attributes into table->field

    @param[in]      record        the table row object
  */
  virtual void store_attributes(const Conf_record *r) override;
  /**
    Whether has the auto increment column
  */
  virtual bool has_autoinc() override { return true; }
  /**
    Store the record sqlfilter id into table->field[ID].

    @param[in]      record        the table row
  */
  virtual void store_id(const Conf_record *r) override;
  /**
    Retrieve sqlfilter type from table->fields into record.

    @param[out]     record        the table row object
  */
  virtual void retrieve_attr(Conf_record *r) override;
  /**
    Push row not found warning to client.

    @param[in]      record        table table row object
  */
  virtual void row_not_found_warning(Conf_record *r) override;

  bool update_row_by_id(Conf_record *record);
};

/**
  Open the sqlfilter table, report error if failed.*

  Attention:
  It didn't open attached transaction, so it must commit
  current transaction context when close sql filter table.

  Make sure it launched within main thread booting
  or statement that cause implicit commit.

  Report client error if failed.

  @param[in]      thd           Thread context
  @param[in]      table_list    Sqlfilter table
  @param[in]      write         read or write

  @retval         Sqlfilter_error
*/
extern Conf_error open_sql_filter_table(THD *thd, TABLE_REF_PTR &table_list,
                                        bool write);

/**
  Add new sqlfilter into sqlfilter table and insert sqlfilter cache.
  Report client error if failed.

  @param[in]      thd         Thread context
  @param[in]      record      sqlfilter

  @retval         false       Success
  @retval         true        Failure
*/
bool add_sql_filter(THD *thd, Conf_record *r);

/**
  Delete a sql filter rule.

  Only report warning message if row or cache not found
*/
bool del_sql_filter(THD *thd, Conf_record *record);

Conf_error reload_sqlfilter_rules(THD *thd);

bool update_sql_filter(THD *thd, Conf_record *r);

#endif
