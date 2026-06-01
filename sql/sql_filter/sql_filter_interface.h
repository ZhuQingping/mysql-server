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

#ifndef SQL_SQL_FILTER_INTERFACE_INCLUDED
#define SQL_SQL_FILTER_INTERFACE_INCLUDED

#include "my_inttypes.h"

class Parser_state;

/* count all filters(all rules)  */
extern std::atomic<ulong> sql_filter_block_num;

/* Initialize sql filter system. */
void sqlfilter_init();

/* Destroy sql filter system. */
void sqlfilter_destroy();

bool limit_query_by_sqlfilter(THD *thd);

void dec_filter_item_conc(THD *thd);

/**
  Init the sqlfilter rules when mysqld reboot

  It should log error message if failed, reported client error
  will be ingored.

  @param[in]      bootstrap     Whether initialize or restart.
*/
void statement_sqlfilter_init(bool bootstrap);

#endif
