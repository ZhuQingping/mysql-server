/* Copyright (c) 2026, Oracle and/or its affiliates.

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

#include "sql/parallel_query/pq_resolver.h"

#include "sql/sql_array.h"

bool find_order_in_list_for_pq(THD *, Ref_item_array &, ORDER *,
                               mem_root_deque<Item *> *) {
  return true;
}

Item **resolve_item_in_base_ref_items(THD *, Ref_item_array,
                                      mem_root_deque<Item *> *, uint &,
                                      Item *) {
  return nullptr;
}

Item **find_item_in_base_items(Ref_item_array, mem_root_deque<Item *> *,
                               Item *, uint &) {
  return nullptr;
}

bool items_equal_after_resolve(Item *, Item *, bool &need_alias_item) {
  need_alias_item = false;
  return false;
}
