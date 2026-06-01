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

#ifndef SQL_PACKAGE_RPL_WAL_PROC_INCLUDED
#define SQL_PACKAGE_RPL_WAL_PROC_INCLUDED

/**
  rpl_proc proc definition.

  Include following procs for rpl_wal:
  * rpl_wal.pdb_promote()
  * rpl_wal.pdb_demote()
*/
#include "sql/package/proc.h"

using im::Disable_copy_base;
using im::Proc;
using im::Sql_cmd_admin_proc;
using im::Sql_cmd_proc;

namespace rpl_wal {

extern LEX_CSTRING RPL_WAL_PROC_SCHEMA;

/**
  Procedure base for rpl_wal
  Uniform schema rpl_wal
*/
class Rpl_wal_proc_base : public Proc, public Disable_copy_base {
 public:
  explicit Rpl_wal_proc_base(PSI_memory_key key) : Proc(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;
  }

  /**
    Show the full package name and procedure name.

    @retval return the full package name and procedure name.
  */
  const std::string qname() const override {
    std::stringstream ss;
    ss << RPL_WAL_PROC_SCHEMA.str << "." << str();
    return ss.str();
  }
};

/**
  Procedure rpl_wal.pdb_promote() implementation
  Note need the SUPER_ACL privileges
*/
class Sql_cmd_proc_pdb_promote : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_proc_pdb_promote(THD *thd, mem_root_deque<Item *> *list,
                                    const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}
  /**
    Implementation of promote execution body on replica node.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  bool pc_execute(THD *thd) override;
};

/**
  call rpl_wal.pdb_promote();
    no parameters
*/
class Proc_pdb_promote : public Rpl_wal_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_pdb_promote;

 public:
  explicit Proc_pdb_promote(PSI_memory_key key) : Rpl_wal_proc_base(key) {}

  /**
    Singleton instance for Proc_pdb_promote.

    @retval  return the singleton instance for Proc_pdb_promote.
  */
  static Proc *instance() {
    static Proc *proc =
        new (std::nothrow) Proc_pdb_promote(im::key_memory_package);
    return proc;
  }

  /**
    Evoke the sql_cmd object for pdb_promote() procedure.

    @param[in] thd  the thread context.
    @param[in] list the list of expressions.

    @retval return the interface of SQL command for pdb promote.
  */
  Sql_cmd *evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const override {
    return new (thd->mem_root) Sql_cmd_type(thd, list, this);
  }

  virtual ~Proc_pdb_promote() {}

  /**
    Get the procedure name.

    @retval  return the procedure name.
  */
  const std::string str() const override { return std::string("pdb_promote"); }
};

/**
  Procedure rpl_wal.pdb_demote() implementation
  Note need the SUPER_ACL privileges
*/
class Sql_cmd_proc_pdb_demote : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_proc_pdb_demote(THD *thd, mem_root_deque<Item *> *list,
                                   const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}
  /**
    Implementation of demote execution body on source node.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  bool pc_execute(THD *thd) override;
};

/**
  call rpl_wal.pdb_demote();
    no parameters
*/
class Proc_pdb_demote : public Rpl_wal_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_pdb_demote;

 public:
  explicit Proc_pdb_demote(PSI_memory_key key) : Rpl_wal_proc_base(key) {}

  /**
    Singleton instance for Proc_pdb_demote.

    @retval  return the singleton instance for Proc_pdb_demote.
  */
  static Proc *instance() {
    static Proc *proc =
        new (std::nothrow) Proc_pdb_demote(im::key_memory_package);
    return proc;
  }

  /**
    Evoke the sql_cmd object for pdb_demote() procedure.

    @param[in] thd  the thread context.
    @param[in] list the list of expressions.

    @retval return the interface of SQL command for pdb demote.
  */
  Sql_cmd *evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const override {
    return new (thd->mem_root) Sql_cmd_type(thd, list, this);
  }

  virtual ~Proc_pdb_demote() {}

  /**
    Get the procedure name.

    @retval  return the procedure name.
  */
  const std::string str() const override { return std::string("pdb_demote"); }
};

}  // namespace rpl_wal

#endif /* SQL_PACKAGE_RPL_WAL_PROC_INCLUDED */