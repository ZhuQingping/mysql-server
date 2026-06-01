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

#include "sql/sql_filter/sql_filter_cache.h"
#include "sql/sql_filter/sql_filter_table.h"

#include <stack>
#include "my_macros.h"
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_mutex.h"
#include "sql/sql_lex.h"

std::atomic<ulong> sql_filter_block_num = {0};

/* System sql filter rwlock psi */
PSI_rwlock_key key_rwlock_sql_filter;

/* Init the singleton instance opointer when load ELF */
System_sql_filter *System_sql_filter::m_system_sql_filter = nullptr;

static bool sqlfilter_inited = false;

#ifdef HAVE_PSI_INTERFACE

static PSI_rwlock_info sqlfilter_rwlocks[] = {
    {&key_rwlock_sql_filter, "RWLOCK_sqlfilter", 0, 0, PSI_DOCUMENT_ME}};
/**
  Init all the sqlfilter psi keys
*/
static void init_sqlfilter_psi_key() {
  const char *category = "sql";
  int count = static_cast<int>(array_elements(sqlfilter_rwlocks));
  mysql_rwlock_register(category, sqlfilter_rwlocks, count);
}
#endif

/* Initialize sqlfilter system. */
void sqlfilter_init() {
  DBUG_ENTER("sqlfilter_init");

#ifdef HAVE_PSI_INTERFACE
  init_sqlfilter_psi_key();
#endif
  assert(System_sql_filter::instance() == nullptr);

  System_sql_filter::m_system_sql_filter =
      allocate_sqlfilter_object<System_sql_filter>();

  sqlfilter_inited = true;
  DBUG_VOID_RETURN;
}

/* Destroy sql filter system. */
void sqlfilter_destroy() {
  DBUG_ENTER("sqlfilter_destroy");
  assert(System_sql_filter::instance() != nullptr);
  destroy_object<System_sql_filter>(System_sql_filter::instance());
  System_sql_filter::m_system_sql_filter = nullptr;
  DBUG_VOID_RETURN;
}

bool limit_query_by_sqlfilter(THD *thd) {
  return System_sql_filter::instance()->limit_query_by_sqlfilter(thd);
}

void dec_filter_item_conc(THD *thd) {
  return System_sql_filter::instance()->dec_filter_item_conc(thd);
}
