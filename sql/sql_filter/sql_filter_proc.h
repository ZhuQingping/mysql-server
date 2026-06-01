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

#ifndef SQL_SQL_FILTER_PROC_INCLUDED
#define SQL_SQL_FILTER_PROC_INCLUDED

#include "sql/package/proc.h"
#include "sql/sql_filter/sql_filter_table_common.h"

/**
  Sql filter procedure (dbms_sqlfilter package)

  1) add_sql_filter
*/

extern LEX_CSTRING SQL_FILTER_PROC_SCHEMA;

/**
  Procedure base for dbms_sqlfilter

  1) Uniform schema : dbms_sqlfilter
*/

class Sqlfilter_proc_base : public Proc, public Disable_copy_base {
 public:
  explicit Sqlfilter_proc_base(PSI_memory_key key) : Proc(key) {}

  virtual const std::string qname() const override {
    std::stringstream ss;
    ss << SQL_FILTER_PROC_SCHEMA.str << "." << str();
    return ss.str();
  }
};

/**
  Sql command base for dbms_sqlfilter

  dbms_sqlfilter.show_sql_filter didn't require any privileges;
*/
class Sql_cmd_sqlfilter_proc_base : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_sqlfilter_proc_base(THD *thd, mem_root_deque<Item *> *list,
                                       const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {
    set_priv_type(Priv_type::PRIV_NONE_ACL);
  }
};

/**
  dbms_sqlfilter.add_sql_filter(...);

  It will add sql filter into mysql.rds_sql_filter_rules table,
  and the sql filter cache that take effect immediately.
*/
class Sql_cmd_sqlfilter_proc_add : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_sqlfilter_proc_add(THD *thd, mem_root_deque<Item *> *list,
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
    Create record from parameters.

    @param[in]      thd       Thread context
    @param[in]      list      Parameters

    @retval         record    Sqlfilter record
  */
  Sql_filter_record *get_record(THD *thd);
};

class Sqlfilter_proc_add : public Sqlfilter_proc_base {
  using Sql_cmd_type = Sql_cmd_sqlfilter_proc_add;

  /* All the parameters */
  enum enum_parameter {
    SQLFILTER_PARAM_TYPE = 0,
    SQLFILTER_PARAM_MAX_CONCURRENCY,
    SQLFILTER_PARAM_KEY_STR,
    SQLFILTER_PARAM_NODE_ID,
    SQLFILTER_PARAM_LAST
  };

  /* Corresponding field type */
  enum_field_types get_field_type(enum_parameter param) {
    DBUG_ASSERT(param < SQLFILTER_PARAM_LAST);
    if (param == SQLFILTER_PARAM_MAX_CONCURRENCY) return MYSQL_TYPE_LONGLONG;
    return MYSQL_TYPE_VARCHAR;
  }

 public:
  explicit Sqlfilter_proc_add(PSI_memory_key key) : Sqlfilter_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;

    /* Init parameters */
    for (size_t i = SQLFILTER_PARAM_TYPE; i < SQLFILTER_PARAM_LAST; i++) {
      m_parameters.assign_at(
          i, get_field_type(static_cast<enum enum_parameter>(i)));
    }
  }

  /* Singleton instance for add_sql_filter */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for add_sql_filter() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Sqlfilter_proc_add() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("add_sql_filter");
  }
};

/**
  2) dbms_sqlfilter.delete_sql_filter()

    Delete the sql filter rule from mysql.rds_sql_filter_rules and its cache.
*/
class Sql_cmd_sqlfilter_proc_del : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_sqlfilter_proc_del(THD *thd, mem_root_deque<Item *> *list,
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

class Sqlfilter_proc_del : public Sqlfilter_proc_base {
  using Sql_cmd_type = Sql_cmd_sqlfilter_proc_del;

  /* All the parameters */
  enum enum_parameter { SQLFILTER_PARAM_ID = 0, SQLFILTER_PARAM_LAST };

  /* Corresponding field type */
  enum_field_types get_field_type(enum_parameter param MY_ATTRIBUTE((unused))) {
    DBUG_ASSERT(param == SQLFILTER_PARAM_ID);
    return MYSQL_TYPE_LONGLONG;
  }

 public:
  explicit Sqlfilter_proc_del(PSI_memory_key key) : Sqlfilter_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;

    /* Init parameters */
    for (size_t i = SQLFILTER_PARAM_ID; i < SQLFILTER_PARAM_LAST; i++) {
      m_parameters.assign_at(
          i, get_field_type(static_cast<enum enum_parameter>(i)));
    }
  }
  /* Singleton instance for delete_sql_filter */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for delete_sql_filter() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Sqlfilter_proc_del() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("delete_sql_filter");
  }
};

/**
  dbms_sqlfilter.show_sql_filter();

  It will show sql filter in cache.
*/
class Sql_cmd_sqlfilter_proc_show : public Sql_cmd_sqlfilter_proc_base {
 public:
  explicit Sql_cmd_sqlfilter_proc_show(THD *thd, mem_root_deque<Item *> *list,
                                       const Proc *proc)
      : Sql_cmd_sqlfilter_proc_base(thd, list, proc) {}

  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;

  /* Override default send_result */
  virtual void send_result(THD *thd, bool error) override;
};

class Sqlfilter_proc_show : public Sqlfilter_proc_base {
  using Sql_cmd_type = Sql_cmd_sqlfilter_proc_show;

  enum enum_column {
    Item_id = 0,
    Type,
    Cur_concur,
    Max_concur,
    Key_num,
    Block_query_num,
    Key_str,
    Node_id,
    COLUMN_LAST
  };

 public:
  explicit Sqlfilter_proc_show(PSI_memory_key key) : Sqlfilter_proc_base(key) {
    /* Result set protocol packet */
    m_result_type = Result_type::RESULT_SET;

    Column_element elements[COLUMN_LAST] = {
        {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("item_id"), 0},
        {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("type"), 32},
        {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("cur_concur"), 0},
        {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("max_concurrency"), 0},
        {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("key_num"), 0},
        {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("cur_reject"), 0},
        {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("key_str"), 1024},
        {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("node_id"), 64}};
    for (size_t i = 0; i < COLUMN_LAST; i++) {
      m_columns.assign_at(i, elements[i]);
    }
  }
  /* Singleton instance for show_sql_filter */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for show_sql_filter proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Sqlfilter_proc_show() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("show_sql_filter");
  }
};

/**
  dbms_sqlfilter.flush_sql_filter();

  It will flush all sqlfilter cache and read all rules from
  mysql.rds_sql_filter_rules and add into cache.
*/
class Sql_cmd_sqlfilter_proc_flush : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_sqlfilter_proc_flush(THD *thd, mem_root_deque<Item *> *list,
                                        const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}

  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;

  /* Inherit the default send_result */
};

class Sqlfilter_proc_flush : public Sqlfilter_proc_base {
  using Sql_cmd_type = Sql_cmd_sqlfilter_proc_flush;

 public:
  explicit Sqlfilter_proc_flush(PSI_memory_key key) : Sqlfilter_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;
  }
  /* Singleton instance for show_sql_filter */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for flush_sql_filter proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Sqlfilter_proc_flush() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("flush_sql_filter");
  }
};

/**
  dbms_sqlfilter.update_sql_filter(...);

  It will update a sql filter rule in mysql.rds_sql_filter_rules table,
  and the sql filter cache will take effect immediately.
*/
class Sql_cmd_sqlfilter_proc_update : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_sqlfilter_proc_update(THD *thd, mem_root_deque<Item *> *list,
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
    Create record from parameters.

    @param[in]      thd       Thread context
    @param[in]      list      Parameters

    @retval         record    sql filter record
  */
  Sql_filter_record *get_record(THD *thd);
};

class Sqlfilter_proc_update : public Sqlfilter_proc_base {
  using Sql_cmd_type = Sql_cmd_sqlfilter_proc_update;

  enum enum_parameter {
    SQLFILTER_PARAM_ID = 0,
    SQLFILTER_PARAM_TYPE,
    SQLFILTER_PARAM_MAX_CONCURRENCY,
    SQLFILTER_PARAM_KEY_STR,
    SQLFILTER_PARAM_NODE_ID,
    SQLFILTER_PARAM_LAST
  };

  /* Corresponding field type */
  enum_field_types get_field_type(enum_parameter param) {
    DBUG_ASSERT(param < SQLFILTER_PARAM_LAST);
    if (param == SQLFILTER_PARAM_ID || param == SQLFILTER_PARAM_MAX_CONCURRENCY)
      return MYSQL_TYPE_LONGLONG;
    return MYSQL_TYPE_VARCHAR;
  }

 public:
  explicit Sqlfilter_proc_update(PSI_memory_key key)
      : Sqlfilter_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;

    /* Init parameters */
    for (size_t i = SQLFILTER_PARAM_ID; i < SQLFILTER_PARAM_LAST; i++) {
      m_parameters.assign_at(
          i, get_field_type(static_cast<enum enum_parameter>(i)));
    }
  }

  /* Singleton instance for update_sql_filter */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for update_sql_filter() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Sqlfilter_proc_update() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("update_sql_filter");
  }
};

#endif
