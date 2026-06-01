/*
  Copyright (c) 2026, Huawei and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef __UT_MYSQL_MOCK_H__
#define __UT_MYSQL_MOCK_H__

#include "sql/table.h"
#include "sql/field.h"
#include "dd/dd.h"
#include "dd/dictionary.h"
#include "dd/properties.h"
#include "dd/types/index.h"
#include "dd/types/object_table.h"
#include "dd/types/object_table_definition.h"
#include "dd/types/partition.h"
#include "dd/types/table.h"
#include "dd/types/tablespace.h"

class ut_mysql_table : public TABLE {
    static const int UT_TABLE_MAX_COL_NUM = 128;
public:
    ut_mysql_table(int id, const char* name);
    ut_mysql_table(int id_old, const char* name_old, int id_new, const char* name_new);
    ~ut_mysql_table();
};


dd::Table* ut_mysql_dd_init();
void ut_mysql_dd_release(dd::Table* table_def);




#endif // __UT_MYSQL_MOCK_H__
