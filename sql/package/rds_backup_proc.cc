/*******************************************************************
 * Copyright (C) Huawei Technologies, 2024
 *
 * Separate Code and Hot Data Storage proc implementation
 *******************************************************************/
#include "sql/package/rds_backup_proc.h"

#include <stdio.h>
#include <string>
#include "sql/auth/auth_acls.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"  // ER_THD
#include "sql/local_backup/full_local_backup.h"
#include "sql/local_backup/log_archive.h"
#include "sql/mysqld.h"
#include "sql/protocol.h"

#include "sql/package/package_common.h"

#include "mysql/components/services/log_builtins.h"

namespace rds_backup {

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::this_thread::sleep_for;

LEX_CSTRING RDS_BACKUP_PROC_SCHEMA = {C_STRING_WITH_LEN("rds_backup")};

/* Singleton instance for start_full_local_backup */
Proc *Proc_start_full_local_backup::instance() {
  static Proc *proc =
      new (std::nothrow) Proc_start_full_local_backup(im::key_memory_package);

  return proc;
}

/**
  Evoke the sql_cmd object for start_full_local_backup() proc.
*/
Sql_cmd *Proc_start_full_local_backup::evoke_cmd(
    THD *thd, mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Sql cmd start_full_local_backup()

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_proc_start_full_backup::pc_execute(THD *thd) {
  bool error = exec_start_full_lb_cmd(thd, (*m_list)[0]->val_int());

  return error;
}

/* Singleton instance for stop_full_local_backup */
Proc *Proc_stop_full_local_backup::instance() {
  static Proc *proc =
      new (std::nothrow) Proc_stop_full_local_backup(im::key_memory_package);

  return proc;
}

/**
  Evoke the sql_cmd object for stop_full_local_backup() proc.
*/
Sql_cmd *Proc_stop_full_local_backup::evoke_cmd(
    THD *thd, mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Sql cmd stop_full_local_backup()

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_proc_stop_full_backup::pc_execute(THD *thd) {
  bool error = exec_stop_full_lb_cmd(thd);

  return error;
}

/* Singleton instance for start_log_archive */
Proc *Proc_start_log_archive::instance() {
  static Proc *proc =
      new (std::nothrow) Proc_start_log_archive(im::key_memory_package);

  return proc;
}

/**
  Evoke the sql_cmd object for start_log_archive() proc.
*/
Sql_cmd *Proc_start_log_archive::evoke_cmd(THD *thd,
                                           mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Sql cmd start_log_archive()

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_proc_start_log_archive::pc_execute(THD *thd) {
  bool error = exec_start_log_archive_cmd(thd);

  return error;
}

/* Singleton instance for stop_log_archive */
Proc *Proc_stop_log_archive::instance() {
  static Proc *proc =
      new (std::nothrow) Proc_stop_log_archive(im::key_memory_package);

  return proc;
}
/**
  Evoke the sql_cmd object for stop_log_archive() proc.
*/
Sql_cmd *Proc_stop_log_archive::evoke_cmd(THD *thd,
                                          mem_root_deque<Item *> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Sql cmd stop_log_archive()

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool Sql_cmd_proc_stop_log_archive::pc_execute(THD *thd) {
  bool error = exec_stop_log_archive_cmd(thd);

  return error;
}

} /* namespace rds_backup */
