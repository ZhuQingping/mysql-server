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

#ifndef __CDE_INFORMATION_SCHEMA__H
#define __CDE_INFORMATION_SCHEMA__H

#include <cstdint>

namespace CDE {
extern struct st_mysql_plugin g_infomationSchemaDstoreTrx;
extern struct st_mysql_plugin g_infomationSchemaDstoreBufferPoolStats;
extern struct st_mysql_plugin g_infomationSchemaDstoreLocks;
extern struct st_mysql_plugin g_infomationSchemaDstoreMemStats;
extern struct st_mysql_plugin g_infomationSchemaDstoreSegStats;
extern struct st_mysql_plugin g_infomationSchemaDstoreUndo;
extern struct st_mysql_plugin g_infomationSchemaDstoreIndexes;
extern struct st_mysql_plugin g_infomationSchemaDstoreTableSpace;
extern struct st_mysql_plugin g_infomationSchemaDstoreOnlineDdlProgress;

int ThreadTransactionCallback(void *arg, void **output);

}  // namespace CDE
#endif