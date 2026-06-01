/* Copyright (c) 2025, Huawei and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_FULL_LOCAL_BACKUP_INCLUDED
#define SQL_FULL_LOCAL_BACKUP_INCLUDED

#include "my_inttypes.h"

extern char *rds_full_local_backup_path_ptr;
extern ulong rds_full_local_backup_lock_timeout;
extern bool rds_full_local_backup_lock_optimize;

constexpr const char *full_lb_binlog_obs_obj_name = "binlog";
constexpr const char *full_lb_dd_obs_obj_name = "innodb";
constexpr const char *full_lb_schema_obs_obj_name = "mysql_schemas";

class THD;

/**
  Start full local backup command:
    call rds_backup.start_full_local_backup(flag);

  @param[in]  thd   THD
  @param[in]  flag  Whether consider using lsn point to catch wal recycle
                    0: Set none
                    1: Set LSN start point for incremental backup(wal archive)
                    2: Set LSN start point for replica
                    3: Set both 1 and 2

  @return return false on success
*/
bool exec_start_full_lb_cmd(THD *thd, longlong flag);

bool exec_stop_full_lb_cmd(THD *thd);

void init_full_local_backup();

/**
  Prepare for full backup:
  1. Stop events
  2. Acquire BACKUP MDL

  @param[in]  thd  THD

  @return return false on success
*/
bool prepare_for_full_backup(THD *thd);

void increase_current_lb_object_count(uint increase_count = 1);
uint get_current_lb_object_count();
void reset_current_lb_object_count(uint count = 0);
bool get_full_lb_running();
#endif  // SQL_FULL_LOCAL_BACKUP_INCLUDED
