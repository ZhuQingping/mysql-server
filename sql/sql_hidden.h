#ifndef SQL_HIDDEN_H_
#define SQL_HIDDEN_H_
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

#include <sys/types.h>

bool is_hidden_by_rds(const char *db_name, size_t db_name_length,
                      const char *table_name, size_t table_name_length);

#endif /* SQL_HIDDEN_H_ */
