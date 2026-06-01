/* Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

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

#include "sql/item.h"
#include "sql/item_func.h"
#include "sql/item_strfunc.h"
#include "sql/parallel_query/pq_resolver.h"
#include "sql/sql_lex.h"

bool Item::replace_with_base_item(uchar *) { return false; }

bool Item::replace_with_base_item2(uchar *info, Item **ref) {
  Query_block *select = (Query_block *)info;
  auto item_type = type();
  // Recursively find and replace argument items with base items.
  if (item_type == FUNC_ITEM || item_type == ROW_ITEM ||
      item_type == COND_ITEM) {
    if (replace_with_base_item(info)) return true;
  }

  return select->find_base_ref_array(ref);
}

bool Item_func::replace_with_base_item(uchar *info) {
  Item **arg, **arg_end;
  for (arg = args, arg_end = args + arg_count; arg != arg_end; arg++) {
    if ((*arg)->replace_with_base_item2(info, arg)) return true;
  }
  return false;
}

bool Item_row::replace_with_base_item(uchar *info) {
  Item **arg, **arg_end;
  for (arg = items, arg_end = items + arg_count; arg != arg_end; arg++) {
    if ((*arg)->replace_with_base_item2(info, arg)) return true;
  }
  return false;
}

bool Item_cond::replace_with_base_item(uchar *info) {
  List_iterator<Item> li(list);
  Item *item;
  while ((item = li++)) {
    if (item->replace_with_base_item2(info, li.ref())) return true;
  }
  return false;
}

bool Item_func_make_set::replace_with_base_item(uchar *info) {
  if (item->replace_with_base_item2(info, &item)) return true;
  return Item_str_func::replace_with_base_item(info);
}

bool Query_block::find_base_ref_array(Item **found, Item_ref *ref,
                                      bool is_ref) {
  // make ref[0] points to the base item. note that, Item_view_ref can be
  // filled into visible part of fields.
  if (found[0]->type() == Item::REF_ITEM &&
      (down_cast<Item_ref *>(found[0])->ref_type() == Item_ref::REF ||
       down_cast<Item_ref *>(found[0])->ref_type() ==
           Item_ref::AGGREGATE_REF)) {
    Item_ref *item_ref = static_cast<Item_ref *>(found[0]);
    assert(!pq_try_clone_item);
    // Unlike in Item_ref::pq_clone, this is called only if we have already
    // cloned query blocks (see assertion above), thus the computation of
    // source_select is simpler:
    auto source_select =
        item_ref->depended_from ? item_ref->depended_from : this;
    return source_select->find_base_ref_array(item_ref->ref_pointer(), item_ref,
                                              true);
  }

  uint pos_in_ref;
  Item **item_found =
      find_item_in_base_items(base_ref_items, &fields, found[0], pos_in_ref);
  // We must find an item in fields equal to found[0]
  if (is_ref || (found[0]->type() == Item::SUM_FUNC_ITEM)) {
    assert(item_found);
    // PQ will report error rather than core-dump in release version
    if (!item_found) return true;
  }

  if (item_found) {
    if (is_ref) {
      ref->set_ref_pointer(item_found);
    } else {
      found[0] = item_found[0];
    }
  }

  return false;
}
