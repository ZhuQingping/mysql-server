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

#include <cstring>

#include "sql/item.h"
#include "sql/sql_array.h"

bool find_order_in_list_for_pq(THD *, Ref_item_array &ref_item_array,
                               ORDER *order, mem_root_deque<Item *> *fields) {
  if (order == nullptr || order->item == nullptr || *order->item == nullptr)
    return true;

  uint pos_in_ref = 0;
  Item **resolved = find_item_in_base_items(ref_item_array, fields, *order->item,
                                            pos_in_ref);
  if (resolved == nullptr) return true;

  order->item = resolved;
  return false;
}

Item **resolve_item_in_base_ref_items(THD *, Ref_item_array ref_item_array,
                                      mem_root_deque<Item *> *fields,
                                      uint &pos_in_ref, Item *item) {
  return find_item_in_base_items(ref_item_array, fields, item, pos_in_ref);
}

Item **find_item_in_base_items(Ref_item_array ref_item_array,
                               mem_root_deque<Item *> *fields, Item *item,
                               uint &pos_in_ref) {
  if (ref_item_array.is_null() || fields == nullptr || item == nullptr)
    return nullptr;

  uint field_pos = 0;
  bool need_alias_item = true;
  for (Item *candidate : *fields) {
    if (candidate == nullptr) {
      ++field_pos;
      continue;
    }

    if (candidate->hidden || item->hidden) return nullptr;

    need_alias_item = true;
    if (items_equal_after_resolve(item, candidate, need_alias_item)) {
      if (field_pos >= ref_item_array.size()) return nullptr;
      if (ref_item_array[field_pos] == nullptr ||
          ref_item_array[field_pos] != candidate)
        return nullptr;
      pos_in_ref = field_pos;
      return &ref_item_array[pos_in_ref];
    }
    ++field_pos;
  }

  return nullptr;
}

bool items_equal_after_resolve(Item *find, Item *item, bool &need_alias_item) {
  need_alias_item = true;
  if (find == nullptr || item == nullptr) return false;
  if (!find->eq(item, false)) return false;

  if (!find->item_name.is_set() || !item->item_name.is_set()) {
    need_alias_item = false;
    return true;
  }

  if (find->item_name.length() == item->item_name.length() &&
      !strcmp(find->item_name.ptr(), item->item_name.ptr())) {
    need_alias_item = false;
    return true;
  }

  return false;
}
