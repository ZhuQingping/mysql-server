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

#include "sql/sql_filter/sql_filter_common.h"
#include "my_inttypes.h"
#include "my_murmur3.h"                 // my_murmur3_32
#include "my_sys.h"                     // MY_WME, MY_FATALERROR
#include "mysql/service_mysql_alloc.h"  // my_malloc

void *Sqlfilter_alloc::operator()(size_t s) const {
  return my_malloc(key_memory_Sql_filter, s, MYF(MY_WME | ME_FATALERROR));
}
