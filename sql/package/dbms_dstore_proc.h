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

#ifndef SQL_PACKAGE_DBMS_DSTORE_PROC_INCLUDED
#define SQL_PACKAGE_DBMS_DSTORE_PROC_INCLUDED

/**
  rpl_proc proc definition.

  Include following procs for dbms_dstore_maintenance:
  * dbms_dstore_maintenance.show_dstore_info()
*/
#include "sql/package/proc.h"

using im::Disable_copy_base;
using im::Proc;
using im::Sql_cmd_admin_proc;
using im::Sql_cmd_proc;

namespace dbms_dstore_maintenance {

extern LEX_CSTRING DBMS_DSTORE_MAINTENANCE_PROC_SCHEMA;

struct DstoreInfoParams {
  const char *func_name;  // IN param
  const char *func_arg;   // IN param
  String *res;            // OUT param
};
/**
  Procedure base for dbms_dstore_maintenance
  Uniform schema dbms_dstore_maintenance
*/
class Dbms_dstore_proc_base : public Proc, public Disable_copy_base {
 public:
  explicit Dbms_dstore_proc_base(PSI_memory_key key) : Proc(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;
  }

  /**
    Show the full package name and procedure name.

    @retval return the full package name and procedure name.
  */
  const std::string qname() const override {
    std::stringstream ss;
    ss << DBMS_DSTORE_MAINTENANCE_PROC_SCHEMA.str << "." << str();
    return ss.str();
  }
};

/**
  Procedure dbms_dstore_maintenance.show_dstore_info() implementation
  Note need the SUPER_ACL privileges
*/
class Sql_cmd_proc_show_dstore_info : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_proc_show_dstore_info(THD *thd, mem_root_deque<Item *> *list,
                                         const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}

  /**
    Implementation of promote execution body on replica node.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  bool pc_execute(THD *) override;

  /* Override default send_result */
  void send_result(THD *thd, bool error) override;

 private:
  /** Check availability of user input parameters
  @param[out] tmp_value  Command execution result
  @param[in]  name       the pointer to buffer that Item object store func name
  @param[in]  arg       the pointer to buffer that Item object store func arg
  */
  bool check_user_param_avaliable(String *name, String *arg);
};

/**
  call dbms_dstore_maintenance.show_dstore_info("func_name", "func_arg");
*/
class Proc_show_dstore_info : public Dbms_dstore_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_show_dstore_info;
  enum enum_parameter {
    SHOW_DSTORE_INFO_FUNC_NAME_ID = 0,
    SHOW_DSTORE_INFO_FUNC_ARGS_ID,
    SHOW_DSTORE_INFO_LAST
  };
  enum enum_column { COLUMN_SCHEMA_OUTPUT = 0, COLUMN_LAST };
  /* Corresponding field type */
  enum_field_types get_field_type(enum_parameter param MY_ATTRIBUTE((unused))) {
    DBUG_ASSERT(param < SHOW_DSTORE_INFO_LAST);
    return MYSQL_TYPE_VARCHAR;
  }

 public:
  explicit Proc_show_dstore_info(PSI_memory_key key)
      : Dbms_dstore_proc_base(key) {
    /* Result set protocol packet */
    m_result_type = Result_type::RESULT_SET;
    /* Init parameters */
    for (size_t i = SHOW_DSTORE_INFO_FUNC_NAME_ID; i < SHOW_DSTORE_INFO_LAST;
         i++) {
      m_parameters.assign_at(
          i, get_field_type(static_cast<enum enum_parameter>(i)));
    }

    Column_element elements[COLUMN_LAST] = {
        {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("show_dstore_info output"), 4096}};
    for (size_t i = 0; i < COLUMN_LAST; i++) {
      m_columns.assign_at(i, elements[i]);
    }
  }
  /**
    Singleton instance for Proc_show_dstore_info.

    @retval  return the singleton instance for Proc_show_dstore_info.
  */
  static Proc *instance() {
    static Proc *proc =
        new (std::nothrow) Proc_show_dstore_info(im::key_memory_package);
    return proc;
  }

  /**
    Evoke the sql_cmd object for show_dstore_info() procedure.

    @param[in] thd  the thread context.
    @param[in] list the list of expressions.

    @retval return the interface of SQL command for show_dstore_indo.
  */
  Sql_cmd *evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const override {
    return new (thd->mem_root) Sql_cmd_type(thd, list, this);
  }

  virtual ~Proc_show_dstore_info() {}

  /**
    Get the procedure name.

    @retval  return the procedure name.
  */
  const std::string str() const override {
    return std::string("show_dstore_info");
  }
};
}  // namespace dbms_dstore_maintenance

#endif /* SQL_PACKAGE_DBMS_DSTOR_PROC_INCLUDED */