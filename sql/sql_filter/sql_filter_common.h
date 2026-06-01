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

#ifndef SQL_SQL_FILTER_COMMON_INCLUDED
#define SQL_SQL_FILTER_COMMON_INCLUDED

#include "mysql/components/services/bits/mysql_cond_bits.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "mysql/components/services/bits/mysql_rwlock_bits.h"
#include "mysql/components/services/bits/psi_cond_bits.h"
#include "mysql/components/services/bits/psi_memory_bits.h"
#include "mysql/components/services/bits/psi_mutex_bits.h"
#include "mysql/components/services/bits/psi_rwlock_bits.h"
#include "sql/common/component.h"

extern PSI_rwlock_key key_rwlock_sql_filter;

/* Sql filter object allocator */
template <typename T, typename... Args>
T *allocate_sqlfilter_object(Args &&... args) {
  return im::allocate_object<T, Args...>(key_memory_Sql_filter,
                                         std::forward<Args>(args)...);
}

struct Sqlfilter_alloc {
  void *operator()(size_t s) const;
};

/* Sql filter string */
using String_sqlfilter = im::String_template<Sqlfilter_alloc>;

namespace std {

template <>
struct hash<String_sqlfilter> {
  typedef String_sqlfilter argument_type;
  typedef size_t result_type;

  size_t operator()(const String_sqlfilter &s) const;
};

} /* namespace std */

#endif
