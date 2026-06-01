/* Copyright (c) 2023, Huawei and/or its affiliates.

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

#ifndef SQL_PACKAGE_PROC_DUMMY_INCLUDED
#define SQL_PACKAGE_PROC_DUMMY_INCLUDED

/**
  Dummy proc definition.

  It was used to demostrate how to define a native procedure,
  so it only take effect on DBUG mode.
*/
namespace im {

/* The schema of dummy and dummy_3 proc */
extern const LEX_CSTRING PROC_DUMMY_SCHEMA;

/**
  Dummy proc sql command class.
*/
class Sql_cmd_proc_dummy : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_proc_dummy(THD *thd, mem_root_deque<Item *> *list,
                              const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}

  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;
};

/**
  Dummy proc definition.
*/
class Proc_dummy : public Proc {
  typedef Sql_cmd_proc_dummy Sql_cmd_type;

 public:
  explicit Proc_dummy(PSI_memory_key key) : Proc(key) {
    m_result_type = Result_type::RESULT_OK;
  }

  virtual ~Proc_dummy() {}

  static Proc *instance();

  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual const std::string str() const override {
    return std::string("dummy");
  }
};

/**
  Dummy proc sql command class.
*/
class Sql_cmd_proc_dummy_3 : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_proc_dummy_3(THD *thd, mem_root_deque<Item *> *list,
                                const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}

  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;

  /**
    Send the result set.
  */
  virtual void send_result(THD *thd, bool error) override;
};

/**
  Dummy_3 proc definition.
*/
class Proc_dummy_3 : public Proc {
 public:
  typedef Sql_cmd_proc_dummy_3 Sql_cmd_type;

  /**
    call dummy_3(id bigint, name varchar(100), num decimal);
     1) id : MYSQL_TYPE_LONGLONG
     2) name : MYSQL_TYPE_VARCHAR
     3) num  : MYSQL_TYPE_DECIMAL
  */
  enum enum_parameter {
    PARAMETER_ID_ID = 0,
    PARAMETER_NAME_ID = 1,
    PARAMETER_NUM_ID = 2
  };

  /**
    dummy_3 result columns list
  */
  enum enum_column { COLUMN_NAME = 0, COLUMN_ID, COLUMN_NUM };

  static constexpr const char *column_name = "NAME";
  static constexpr const char *column_id = "ID";
  static constexpr const char *column_num = "NUM";

 public:
  explicit Proc_dummy_3(PSI_memory_key key) : Proc(key) {
    m_result_type = Result_type::RESULT_SET;

    /* Parameter definition. */
    m_parameters.assign_at(PARAMETER_ID_ID, MYSQL_TYPE_LONGLONG);
    m_parameters.assign_at(PARAMETER_NAME_ID, MYSQL_TYPE_VARCHAR);
    m_parameters.assign_at(PARAMETER_NUM_ID, MYSQL_TYPE_NEWDECIMAL);

    /* Column definition. */

    Column_element element;

    /* Column name */
    element = {MYSQL_TYPE_VARCHAR, column_name, strlen(column_name), 256};
    m_columns.assign_at(COLUMN_NAME, element);

    /* Column id */
    element = {MYSQL_TYPE_LONGLONG, column_id, strlen(column_id), 0};
    m_columns.assign_at(COLUMN_ID, element);

    /* Column num */
    element = {MYSQL_TYPE_NEWDECIMAL, column_num, strlen(column_num), 0};
    m_columns.assign_at(COLUMN_NUM, element);
  }

  virtual ~Proc_dummy_3() {}

  static Proc *instance();

  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual const std::string str() const override {
    return std::string("dummy_3");
  }
};
} /* namespace im */

#endif
