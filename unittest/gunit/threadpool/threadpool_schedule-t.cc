/*
   Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

/* See http://code.google.com/p/googletest/wiki/Primer */
#include <gtest/gtest.h>
#include "plugin/threadpool/threadpool.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_class.h"
#include "sql/system_variables.h"

extern ulong storage_engine_mode;
extern bool can_schedule_out(THD *thd);

TEST(ThreadPoolSchedule, ThreadPoolScheduleTest) {
  THD *thd = new (std::nothrow) THD(false);

  /*For non-dstore engine*/
  storage_engine_mode = static_cast<int>(ONLY_INNODB);

  threadpool_schedule_mode = TP_SCHEDULE_MODE_STATEMENT;
  ASSERT_TRUE(can_schedule_out(thd));

  threadpool_schedule_mode = TP_SCHEDULE_MODE_STATEMENT;
  thd->server_status &= ~SERVER_STATUS_IN_TRANS;
  ASSERT_TRUE(can_schedule_out(thd));

  thd->server_status |= SERVER_STATUS_IN_TRANS;
  threadpool_schedule_mode = TP_SCHEDULE_MODE_STATEMENT;
  ASSERT_TRUE(can_schedule_out(thd));

  threadpool_schedule_mode = TP_SCHEDULE_MODE_TRANSACTION;
  thd->server_status &= ~SERVER_STATUS_IN_TRANS;
  ASSERT_TRUE(can_schedule_out(thd));

  thd->server_status |= SERVER_STATUS_IN_TRANS;
  threadpool_schedule_mode = TP_SCHEDULE_MODE_TRANSACTION;
  ASSERT_FALSE(can_schedule_out(thd));

  /*For dstore engine*/
  storage_engine_mode = static_cast<int>(ONLY_DSTORE);
  thd->server_status &= ~SERVER_STATUS_IN_TRANS;
  ASSERT_TRUE(can_schedule_out(thd));

  thd->server_status |= SERVER_STATUS_IN_TRANS;
  ASSERT_FALSE(can_schedule_out(thd));
  thd->server_status &= ~SERVER_STATUS_IN_TRANS;

  Table_ref *hash_tables = nullptr;
  char table[16] = "test";
  thd->handler_tables_hash.emplace(table,
                                   unique_ptr_my_free<Table_ref>(hash_tables));
  ASSERT_FALSE(can_schedule_out(thd));
  thd->handler_tables_hash.clear();
}