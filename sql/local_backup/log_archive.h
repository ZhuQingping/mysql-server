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

#ifndef SQL_LOG_ARCHIVE_INCLUDED
#define SQL_LOG_ARCHIVE_INCLUDED

extern char *rds_log_archive_path_ptr;

class THD;

/**
  Logic of sql cmd start_log_archive

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool exec_start_log_archive_cmd(THD *thd);

/**
  Logic of sql cmd stop_log_archive

  @param[in]    THD           Thread context

  @retval       true          Failure
  @retval       false         Success
*/
bool exec_stop_log_archive_cmd(THD *thd);

#endif  // SQL_LOG_ARCHIVE_INCLUDED
