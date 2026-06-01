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

#ifndef SQL_PACKAGE_RDS_BACKUP_PROC_INCLUDED
#define SQL_PACKAGE_RDS_BACKUP_PROC_INCLUDED

/**
  rds_backup proc definition.

  There are 4 procs for rds_backup:
  * a) call rds_backup.start_full_local_backup()
  * b) call rds_backup.stop_full_local_backup()
  * c) call rds_backup.start_log_archive()
  * d) call rds_backup.stop_log_archive()
*/
#include "sql/package/proc.h"

using im::Disable_copy_base;
using im::Proc;
using im::Sql_cmd_admin_proc;

namespace rds_backup {

extern LEX_CSTRING RDS_BACKUP_PROC_SCHEMA;

/**
  Procedure base for rds_backup

  1) Uniform schema : rds_backup
*/
class Backup_proc_base : public Proc, public Disable_copy_base {
 public:
  explicit Backup_proc_base(PSI_memory_key key) : Proc(key) {}

  virtual const std::string qname() const override {
    std::stringstream ss;
    ss << RDS_BACKUP_PROC_SCHEMA.str << "." << str();
    return ss.str();
  }
};

/**
  Sql command base for rds_backup

  Note the priviledges
*/

class Sql_cmd_rds_backup_proc_base : public Sql_cmd_admin_proc {
 public:
  explicit Sql_cmd_rds_backup_proc_base(THD *thd, mem_root_deque<Item *> *list,
                                        const Proc *proc)
      : Sql_cmd_admin_proc(thd, list, proc) {}
};

/**
  1) rds_backup.start_full_local_backup()
*/
class Sql_cmd_proc_start_full_backup : public Sql_cmd_rds_backup_proc_base {
 public:
  explicit Sql_cmd_proc_start_full_backup(THD *thd,
                                          mem_root_deque<Item *> *list,
                                          const Proc *proc)
      : Sql_cmd_rds_backup_proc_base(thd, list, proc) {}
  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;
};

class Proc_start_full_local_backup : public Backup_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_start_full_backup;

  /**
    call rds_backup.start_full_local_backup(full_backup_flag);
      full_backup_flag : MYSQL_TYPE_LONGLONG
        0: Set none
        1: Set LSN start point for incremental backup(wal archive)
        2: Set LSN start point for replica
        3: Set both 1 and 2
  */
  enum enum_parameter { PARAMETER_FULL_BACKUP_FLAG_ID = 0 };

 public:
  explicit Proc_start_full_local_backup(PSI_memory_key key)
      : Backup_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;

    m_parameters.assign_at(PARAMETER_FULL_BACKUP_FLAG_ID, MYSQL_TYPE_LONGLONG);
  }

  /* Singleton instance */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for start_full_local_backup() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Proc_start_full_local_backup() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("start_full_local_backup");
  }
};

/**
  2) rds_backup.stop_full_local_backup()
*/
class Sql_cmd_proc_stop_full_backup : public Sql_cmd_rds_backup_proc_base {
 public:
  explicit Sql_cmd_proc_stop_full_backup(THD *thd, mem_root_deque<Item *> *list,
                                         const Proc *proc)
      : Sql_cmd_rds_backup_proc_base(thd, list, proc) {}
  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;
};

class Proc_stop_full_local_backup : public Backup_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_stop_full_backup;

 public:
  explicit Proc_stop_full_local_backup(PSI_memory_key key)
      : Backup_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;
  }

  /* Singleton instance */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for stop_full_local_backup() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Proc_stop_full_local_backup() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("stop_full_local_backup");
  }
};

/**
  3) rds_backup.start_log_archive()
*/
class Sql_cmd_proc_start_log_archive : public Sql_cmd_rds_backup_proc_base {
 public:
  explicit Sql_cmd_proc_start_log_archive(THD *thd,
                                          mem_root_deque<Item *> *list,
                                          const Proc *proc)
      : Sql_cmd_rds_backup_proc_base(thd, list, proc) {}
  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;
};

class Proc_start_log_archive : public Backup_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_start_log_archive;

 public:
  explicit Proc_start_log_archive(PSI_memory_key key) : Backup_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;
  }

  /* Singleton instance */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for start_log_archive() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Proc_start_log_archive() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("start_log_archive");
  }
};

/**
  4) rds_backup.stop_log_archive()
*/
class Sql_cmd_proc_stop_log_archive : public Sql_cmd_rds_backup_proc_base {
 public:
  explicit Sql_cmd_proc_stop_log_archive(THD *thd, mem_root_deque<Item *> *list,
                                         const Proc *proc)
      : Sql_cmd_rds_backup_proc_base(thd, list, proc) {}
  /**
    Implementation of Proc execution body.

    @param[in]    THD           Thread context

    @retval       true          Failure
    @retval       false         Success
  */
  virtual bool pc_execute(THD *thd) override;
};

class Proc_stop_log_archive : public Backup_proc_base {
  using Sql_cmd_type = Sql_cmd_proc_stop_log_archive;

 public:
  explicit Proc_stop_log_archive(PSI_memory_key key) : Backup_proc_base(key) {
    /* Only OK or ERROR protocol packet */
    m_result_type = Result_type::RESULT_OK;
  }

  /* Singleton instance */
  static Proc *instance();

  /**
    Evoke the sql_cmd object for stop_log_archive() proc.
  */
  virtual Sql_cmd *evoke_cmd(THD *thd,
                             mem_root_deque<Item *> *list) const override;

  virtual ~Proc_stop_log_archive() {}

  /* Proc name */
  virtual const std::string str() const override {
    return std::string("stop_log_archive");
  }
};

}  // namespace rds_backup

#endif
