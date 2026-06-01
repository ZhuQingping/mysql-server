#ifndef PQ_CLONE_ITEM_H
#define PQ_CLONE_ITEM_H

/* Copyright (c) 2020, Huawei and/or its affiliates. All rights reserved.

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

/** @file sql/parallel_query/pq_clone_item.cc

    @brief
    This file defines the implementation of T::pq_clone for Item and its
    subclasses, along with its helper function T::pq_copy_from. To standardize
    the implementation logic of T::pq_clone and T::pq_copy_from, we provide
    corresponding template functions.

    The purpose of T::pq_clone is to create a new object of type T based on an
    existing original T instance. After constructing the new T object,
    T::pq_clone invokes the new object's pq_copy_from method to copy and assign
    all necessary member fields.

    The reason we need to define T::pq_copy_from separately, rather than
    implementing its logic directly inside pq_clone, is that pq_clone must not
    only copy the member variables of the current type T, but also those of T’s
    base class, and the base class of that base class, and so on. Therefore, we
    rely on the virtual function Item::pq_copy_from to ensure that all member
    variables associated with type T are properly copied.
*/

#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_geofunc.h"
#include "sql/item_gtid_func.h"
#include "sql/item_inetfunc.h"
#include "sql/item_pfs_func.h"
#include "sql/item_regexp_func.h"
#include "sql/item_sum.h"
#include "sql/item_timefunc.h"
#include "sql/log.h"
#include "sql/parallel_query/pq_clone.h"
#include "sql/parallel_query/pq_resolver.h"
#include "sql/parse_tree_items.h"
#include "sql/query_result.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_plan_cache.h"  // is_clone_for_plan_cache

namespace pq_def {
// Encapsulate the args[i]->pq_clone call from Item_func to preserves semantic
// clarity.
static inline bool clone_all_arguments(THD *thd, Query_block *select,
                                       Item **args, uint arg_count,
                                       mem_root_deque<Item *> &item_list) {
  item_list.clear();
  for (uint i = 0; i < arg_count; i++) {
    Item *arg = args[i]->pq_clone(thd, select);
    if (arg == nullptr) return true;
    if (item_list.push_back(arg)) return true;
  }
  return false;
}

static inline bool clone_all_arguments(THD *thd, Query_block *select,
                                       Item **src_args, uint arg_count,
                                       Item **dest_args) {
  assert(arg_count < 5);
  for (uint i = 0; i < arg_count; i++) {
    dest_args[i] = src_args[i]->pq_clone(thd, select);
    if (dest_args[i] == nullptr) {
      return true;
    }
  }
  return false;
}

template <typename T, typename C>
inline bool check_type_and_warn(THD *thd, Query_block *select, C *current) {
  if (typeid(*current) != typeid(T) ||
      DBUG_EVALUATE_IF("simulate_item_type_mismatch", true, false)) {
    if (select->pq_try_clone_item) return true;
    sql_print_warning(
        "Caller's type %s is not equals to this class type %s, "
        "will not use parallel query, SQL= %s",
        typeid(*current).name(), typeid(T).name(), thd->query().str);
    assert(DBUG_EVALUATE_IF("simulate_item_type_mismatch", true, false) ||
           false);
    return true;
  }
  return false;
}

inline bool copy_self_attributes(THD *thd, Query_block *select, Item *clone,
                                 Item *orig) {
  if (!clone || clone->pq_copy_from(thd, select, orig)) {
    return true;
  }
  return false;
}

/**
 * @brief Template function used to implement T::pq_clone.
 *
 * This function provides a generic cloning workflow for type T, including type,
 * checking clone object creation, and attribute copying. The
 * caller(T::pq_clone) injects a custom item_creator to define how the clone
 * object is constructed and initialized.
 *
 * @tparam T           The class type implementing pq_clone.
 * @tparam ItemCreator A callable (typically a lambda) that creates and
 *                     initializes the clone.
 * @param thd          Thread context.
 * @param select       Query block context.
 * @param self         The original object to be cloned.
 * @param item_creator User-injected logic for creating and initializing the
 *                     clone.
 * @return Item*       The cloned item, or nullptr if cloning fails.
 */
template <typename T, typename ItemCreator>
Item *pq_clone_template(THD *thd, Query_block *select, T *self,
                        ItemCreator item_creator) {
  // Check type compatibility and emit warnings if needed.
  if (check_type_and_warn<T>(thd, select, self)) return nullptr;

  Item *new_item = nullptr;
  // Invoke user-provided item_creator to construct and initialize the clone.
  // If item_creator returns true, cloning failed.
  if (item_creator(new_item)) return new_item;

  // Copy common attributes from the original item to the clone.
  if (copy_self_attributes(thd, select, new_item, self)) return nullptr;
  return new_item;
}

/**
 * @brief Template function used to implement D::pq_copy_from.
 *
 * This function provides a generic implementation for the pq_copy_from method
 * of type D, supporting inheritance from base type B.
 * The caller (D::pq_copy_from) injects the item_copier, which encapsulates the
 * logic for copying data from the original item to the current clone.
 *
 * @tparam D           The derived class implementing pq_copy_from.
 * @tparam B           The base class from which pq_copy_from is inherited.
 * @tparam ItemCopier  A callable (typically a lambda) that copies.
 *                     subclass-specific data
 * @param self         The current clone object (i.e., this).
 * @param thd          Thread context.
 * @param select       Query block context.
 * @param item         The original item to copy from.
 * @param item_copier  User-injected logic for copying subclass-specific
 *                     attributes.
 * @return true if copying failed, false if successful.
 */
template <typename D, typename B, typename ItemCopier>
bool pq_copy_from_template(D *self, THD *thd, Query_block *select, Item *item,
                           ItemCopier item_copier) {
  // First copy base class attributes
  if (self->B::pq_copy_from(thd, select, item)) {
    return true;
  }
  D *orig_item = down_cast<D *>(item);
  assert(orig_item);

  // Invoke user-defined copier for subclass-specific attributes
  return item_copier(orig_item);
}

/**
 * @brief Template function used to implement T::pq_rebuild_sum_func.
 *
 * This function provides a generic implementation for rebuilding a SUM function
 * item of type T.
 * The caller (T::pq_rebuild_sum_func) injects the sum_func_creator, which
 * encapsulates the logic for constructing and initializing the new SUM function
 * item.
 *
 * @tparam T               The class type implementing pq_rebuild_sum_func.
 * @tparam SumFuncCreator  A callable (typically a lambda) that creates and
 *                         initializes the SUM function item.
 * @param thd              Thread context.
 * @param select           Query block context.
 * @param self             The current SUM function item (i.e., this).
 * @param sum_func_creator User-injected logic for creating and initializing the
 *                         new SUM function item.
 * @return T*              The rebuilt SUM function item, or nullptr if the
 * rebuild fails.
 */
template <typename T, typename SumFuncCreator>
T *rebuild_sum_func_template(THD *thd, Query_block *select, T *self,
                             SumFuncCreator sum_func_creator) {
  // Check type compatibility and emit warnings if needed.
  if (check_type_and_warn<T>(thd, select, self)) return nullptr;
  assert(thd->is_pq_leader());

  T *new_item = nullptr;
  // Invoke user-provided sum_func_creator to construct and initialize the new
  // item. If it returns true, the rebuild failed.
  if (sum_func_creator(new_item)) return new_item;

  // Copy common attributes from the original item to the new one.
  if (copy_self_attributes(thd, select, new_item, self)) return nullptr;
  return new_item;
}

/**
 * @brief Template function used to implement T::pq_clone for Item_func
 *        subclasses.
 *
 * This function provides a generic cloning mechanism for function-type items
 * (i.e., subclasses of Item_func).
 *
 * The creator is injected via the caller(T::pq_clone) and is responsible for
 * constructing the clone using the copied arguments. This allows the clone
 * logic to be customized per subclass while reusing the common cloning
 * workflow.
 *
 * @tparam T        The subclass of Item_func implementing pq_clone.
 * @tparam Creator  A callable (typically a lambda) that creates the clone item.
 * @param thd       Thread context.
 * @param select    Query block context.
 * @param self      The original item to be cloned.
 * @param creator   User-injected logic for constructing the clone using copied
 *                  arguments.
 * @return Item*    The cloned item, or nullptr if cloning fails.
 */
template <typename T, typename Creator>
Item *func_item_clone_template(THD *thd, Query_block *select, T *self,
                               Creator creator) {
  // Check type compatibility and emit warnings if needed.
  if (check_type_and_warn<T>(thd, select, self)) return nullptr;

  // Clone all arguments of the function item.
  Item *copy_args[5];
  if (clone_all_arguments(thd, select, self->arguments(), self->arg_count,
                          copy_args)) {
    return nullptr;
  }

  // Use the injected creator to construct the new item from copied arguments.
  Item *new_item = creator(copy_args);
  if (!new_item) {
    return nullptr;
  }
  // Copy common attributes from the original item to the clone.
  if (copy_self_attributes(thd, select, new_item, self)) return nullptr;
  return new_item;
}

}  // namespace pq_def

Item *Item::pq_clone(THD *thd MY_ATTRIBUTE((unused)),
                     Query_block *select MY_ATTRIBUTE((unused))) {
  if (select->pq_try_clone_item) return nullptr;
  sql_print_warning(
      "Item type %s's deep copy method is not implemented, "
      "will not use parallel query, SQL= %s",
      typeid(*this).name(), thd->query().str);
  return nullptr;
}

static inline Item *no_need_copy() { return nullptr; }

bool Item::pq_copy_from(THD *thd MY_ATTRIBUTE((unused)),
                        Query_block *select MY_ATTRIBUTE((unused)),
                        Item *item) {
#ifndef NDEBUG
  assert(nullptr == cloned_origin_item);
#endif  // NDEBUG

  cmp_context = item->cmp_context;
  marker = item->marker;

  collation = item->collation;
  item_name.copy(item->item_name.ptr(), item->item_name.length(),
                 system_charset_info, item->item_name.is_autogenerated());
  orig_name.copy(item->orig_name.ptr(), item->orig_name.length(),
                 system_charset_info, item->orig_name.is_autogenerated());
  decimals = item->decimals;
  is_expensive_cache = item->is_expensive_cache;
  m_accum_properties = item->m_accum_properties;
  m_data_type = item->m_data_type;
  m_is_window_function = item->m_is_window_function;
  max_length = item->max_length;
  m_nullable = item->m_nullable;
  null_value = item->null_value;
  str_value = item->str_value;
  hidden = item->hidden;

#ifndef NDEBUG
  contextualized = item->contextualized;
#endif  // NDEBUG
  assert(!plan_cache::is_clone_for_plan_cache(select));

  cloned_origin_item = item;
  if ((cloned_origin_item = item)) {
    // find the oldest ancestor
    while (cloned_origin_item->cloned_origin_item)
      cloned_origin_item = cloned_origin_item->cloned_origin_item;
  }
  unsigned_flag = item->unsigned_flag;

  if (!pq_alloc_item && item->pq_alloc_item) thd->add_item(this);
  return false;
}

/* Item_basic_constant start */
bool Item_basic_constant::pq_copy_from(THD *thd, Query_block *select,
                                       Item *item) {
  return pq_def::pq_copy_from_template<Item_basic_constant, Item>(
      this, thd, select, item,
      ([this, thd, select, item](Item_basic_constant *orig_item) {
        used_table_map = orig_item->used_table_map;
        return false;
      }));
}

/* Item_cache start */
// TODO see more about cache_const_expr_transformer!!!
bool Item_cache::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_cache, Item_basic_constant>(
      this, thd, select, item,
      ([this, thd, select, item](Item_cache *orig_item) {
        used_table_map = orig_item->used_table_map;
        /* value_cached will be set in Item_cache::cache_value() */
        if (orig_item->example != nullptr) {
          Item *example_arg = orig_item->example->pq_clone(thd, select);
          if (!example_arg || (!example_arg->fixed &&
                               example_arg->refix_fields(thd, &example_arg))) {
            return true;
          }
          if (example_arg == nullptr) return true;
          setup(example_arg);
        }
        return false;
      }));
}

Item *Item_cache_datetime::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_datetime>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cache_datetime(data_type());
        return false;
      }));
}

Item *Item_cache_decimal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_decimal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cache_decimal();
        return false;
      }));
}

Item *Item_cache_int::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_int>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new Item_cache_int();
        return false;
      }));
}

Item *Item_cache_bit::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_bit>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cache_bit(MYSQL_TYPE_BIT);
        return false;
      }));
}

Item *Item_cache_real::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_real>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cache_real();
        return false;
      }));
}

Item *Item_cache_row::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_row>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cache_row();
        return false;
      }));
}

Item *Item_cache_str::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cache_str>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        const Item *item = static_cast<const Item *>(this);
        new_item = new (thd->pq_mem_root) Item_cache_str(item);
        return false;
      }));
}

/* Item_cache end */

/* Item_hex_string start */
Item *Item_hex_string::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_hex_string>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->mem_root) Item_hex_string(POS());
        new_item->fixed = true;
        return false;
      }));
}

// TOOD str_value copyed twice
Item *Item_bin_string::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_bin_string>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_bin_string(str_value.ptr(), str_value.length());
        return false;
      }));
}

/* Item_hex_string end */

/* Item_null start */
Item *Item_null::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_null>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_null(POS());
        return false;
      }));
}

/* Item_null end */

/* Item_num start */  // TODO need copy? const value?
Item *Item_int_with_ref::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_int_with_ref>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *pq_ref = ref->pq_clone(thd, select);
        if (!pq_ref) return true;
        // We know the constructor for Item_int_with_ref will create a fixed
        // item We should make sure its child (pq_ref in that case) is therefore
        // fixed, could we consider doing something like that ?
        if (!pq_ref->fixed && pq_ref->refix_fields(thd, &pq_ref)) return true;
        new_item = new (thd->pq_mem_root) Item_int_with_ref(
            pq_ref->data_type(), value, pq_ref, unsigned_flag);
        return false;
      }));
}

Item *Item_datetime_with_ref::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_datetime_with_ref>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *pq_ref = ref->pq_clone(thd, select);
        if (!pq_ref) return true;

        new_item = new (thd->pq_mem_root) Item_datetime_with_ref(
            pq_ref->data_type(), decimals, value, pq_ref);
        return false;
      }));
}

Item *Item_time_with_ref::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_time_with_ref>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *pq_ref = ref->pq_clone(thd, select);
        if (!pq_ref) return true;

        new_item =
            new (thd->pq_mem_root) Item_time_with_ref(decimals, value, pq_ref);
        return false;
      }));
}

/* Item_num end */

/* Item_string start */  // TODO need copy? const value?
Item *Item_string::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_string>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_string(static_cast<Name_string>(item_name), str_value.ptr(),
                        str_value.length(), collation.collation,
                        collation.derivation, collation.repertoire);
        if (new_item) {
          down_cast<Item_string *>(new_item)->set_cs_specified(m_cs_specified);
        }
        return false;
      }));
}

Item *Item_static_string_func::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_static_string_func>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_static_string_func(
            func_name, str_value.ptr(), str_value.length(), collation.collation,
            collation.derivation);
        return false;
      }));
}

Item *PTI_literal_underscore_charset_bin_num::pq_clone(THD *thd,
                                                       Query_block *select) {
  return pq_def::pq_clone_template<PTI_literal_underscore_charset_bin_num>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(origin_item == nullptr);
        new_item =
            new (thd->pq_mem_root) PTI_literal_underscore_charset_bin_num(
                str_value.ptr(), str_value.length(), collation.collation,
                collation.derivation);
        return false;
      }));
}

/* Item_string end */
/* Item_basic_constant end */

/* Item_ident start */
bool Item_ident::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_ident, Item>(
      this, thd, select, item,
      ([this, thd, select, item]([[maybe_unused]] Item_ident *orig_item) {
        DBUG_EXECUTE_IF("simulate_item_clone_attr_copy_error", return true;);
        context = &select->context;
        return false;
      }));
}

Item *Item_field::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_field>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        DBUG_EXECUTE_IF("simulate_item_clone_error", return true;);
        DBUG_EXECUTE_IF("simulate_no_item_copy_function", {
          new_item = Item::pq_clone(thd, select);
          return true;
        });
        /**
         * For the case of "force" using tmp_table for aggregation (e.g.,
         * SQL_RESULT_BUFFER hint), item_sum's argument will be replaced with a
         * Item_field. Thus, we should copy the original item for prepare phase.
         *Note that, we cannot use the "origin_item" to copy item, because the
         *replaced item_field is the optimized-item after optimize phase,
         *especially when performing (semi)-join query.
         **/

        if (origin_sum_arg) {
          new_item = origin_sum_arg->pq_clone(thd, select);
          return true;
        }

        new_item = new (thd->pq_mem_root)
            Item_field(POS(), db_name, table_name,
                       m_orig_field_name ? m_orig_field_name : field_name);
        return false;
      }));
}

bool Item_field::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_field, Item_ident>(
      this, thd, select, item,
      ([this, thd, select, item](Item_field *orig_item) {
        field_index = orig_item->field_index;
        no_constant_propagation = orig_item->no_constant_propagation;
        can_use_prefix_key = orig_item->can_use_prefix_key;
        any_privileges = orig_item->any_privileges;
        have_privileges = orig_item->have_privileges;
        return false;
      }));
}

Item *Item_default_value::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_default_value>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *new_arg = nullptr;
        if (arg) {
          new_arg = arg->pq_clone(thd, select);
          if (nullptr == new_arg) return true;
        }
        new_item = new (thd->pq_mem_root) Item_default_value(POS(), new_arg);
        return false;
      }));
}

Item *Item_func_at_time_zone::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_at_time_zone>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg = args[0]->pq_clone(thd, select);
        if (arg == nullptr) return true;
        new_item = new (thd->pq_mem_root) Item_func_at_time_zone(
            POS(), arg, m_specifier_string, m_is_interval);
        return false;
      }));
}

Item *Item_ref::pq_clone(THD *thd, Query_block *select) {
  uint pos_in_ref;
  // find the equal field with "Item::eq()". As they are cloned from the
  // resolved item, this method should be more accurate.
  // Note that when this cloning function is called as part of building
  // Query_block::saved_where_cond (pq_try_clone_item==true), 'select' is the
  // original query block. While in the other case, 'select' is the new,
  // under-construction query block.
  auto source_select = depended_from ? (select->pq_try_clone_item
                                            ? depended_from
                                            : depended_from->pq_last_clone())
                                     : select;
  if (!source_select) {
    // Abnormal situation. Can happen if during resolution, some subquery Item
    // was removed, but not cleaned up (no call to clean_up_after_removal())
    // (likely due to containing a merged view's column) (f.ex. see
    // empty_order_list()): then the Query_expression remains reachable through
    // the chain of units, used by list_query_blocks_to_clone(), and we may
    // end up trying to clone this Query_expression while its content (incl.
    // this Item_ref) is irrelevant.
    return nullptr;
  }

  /*
    The principle: we have an original query block OQB and a new query block
    NQB. We have cloned the SELECT list of OQB into the SELECT list of NQB
    (new Items). Now we have an Item_ref of OQB, which references another Item
    located in the SELECT list of OQB. We search in the SELECT list of NQB for
    a new Item equal to that referenced item (the former being a clone of the
    latter, if everything is right).

    Note: in a subquery, outer references can be Item_ref, Item_outer_ref
    (unsupported in PQ), Item_field, depending on where we are in the subquery
    and where the subquery is in the top query. Here we handle Item_ref. For
    Item_field, there is no need to fiddle with dependend_from like above:
    this pointer is automatically set because Item_field is resolved, by the
    leader and workers, using the ordinary path of fix_fields() (which calls
    Item_field::fix_outer_field() which calls mark_as_dependent()). While
    Item_ref is not.
  */
  Item **select_item = find_item_in_base_items(source_select->base_ref_items,
                                               source_select->get_fields_list(),
                                               *m_ref_item, pos_in_ref);
  // So the expectation is that we found something:
  assert(select_item || select->pq_try_clone_item);
  // release-version will report PQ error rather than core-dump
  if (!select_item || !(*select_item)) return nullptr;
  Item_ref *new_item =
      new (thd->pq_mem_root) Item_ref(&select->context, select_item, db_name,
                                      table_name, field_name, m_alias_of_expr);
  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  if (depended_from)  // if original Item is an outer reference, so is the new
    new_item->depended_from = source_select;
  return new_item;
}

Item *Item_name_const::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_name_const>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *name_arg, *val_arg;
        if (name_item == nullptr) {
          name_arg = nullptr;
        } else {
          name_arg = name_item->pq_clone(thd, select);
          if (name_arg == nullptr) return true;
        }
        if (value_item == nullptr) {
          val_arg = nullptr;
        } else {
          val_arg = value_item->pq_clone(thd, select);
          if (val_arg == nullptr) return true;
        }
        new_item =
            new (thd->pq_mem_root) Item_name_const(POS(), name_arg, val_arg);
        return false;
      }));
}

bool Item_name_const::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_name_const, Item>(
      this, thd, select, item,
      ([this, thd, select, item](Item_name_const *orig_item) {
        valid_args = orig_item->valid_args;
        return false;
      }));
}

/* Item_result_field start */
/* Item_func start */
bool Item_func::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func, Item_result_field>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func *orig_item) {
        null_on_null = orig_item->null_on_null;
        // Do not copy allow_arg_cols, will calculate from constructor
        used_tables_cache = orig_item->used_tables_cache;
        not_null_tables_cache = orig_item->not_null_tables_cache;
        return false;
      }));
}

/* Item_func_bit start */
bool Item_func_bit::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_bit, Item_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_bit *orig_item) {
        hybrid_type = orig_item->hybrid_type;
        return false;
      }));
}

Item *PTI_literal_underscore_charset_hex_num::pq_clone(THD *thd,
                                                       Query_block *select) {
  return pq_def::pq_clone_template<PTI_literal_underscore_charset_hex_num>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        LEX_STRING str = {const_cast<char *>(str_value.ptr()),
                          str_value.length()};
        new_item =
            new (thd->pq_mem_root) PTI_literal_underscore_charset_hex_num(
                POS(), collation.collation, str);
        return false;
      }));
}

Item *Item_func_bit_neg::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_bit_neg(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_bit_neg>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_bit_and::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_bit_and(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_bit_and>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_bit_or::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_bit_or(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_bit_or>(thd, select, this,
                                                            item_creator);
}

Item *Item_func_bit_xor::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_bit_xor(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_bit_xor>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_shift_left::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_shift_left(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_shift_left>(
      thd, select, this, item_creator);
}

Item *Item_func_shift_right::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_shift_right(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_shift_right>(
      thd, select, this, item_creator);
}

/* Item_func_bit end */

Item *Item_func_case::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_case>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item = new (thd->pq_mem_root)
            Item_func_case(POS(), &item_list, nullptr, nullptr);
        return false;
      }));
}

bool Item_func_case::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_case, Item_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_case *orig_item) {
        first_expr_num = orig_item->first_expr_num;
        else_expr_num = orig_item->else_expr_num;
        cached_result_type = orig_item->cached_result_type;
        left_result_type = orig_item->left_result_type;
        ncases = orig_item->ncases;
        cmp_type = orig_item->cmp_type;
        case_item = orig_item->case_item;
        cmp_collation = orig_item->cmp_collation;
        for (uint i = 0; i <= (uint)DECIMAL_RESULT; i++) {
          if (orig_item->cmp_items[i]) {
            if (!(cmp_items[i] = cmp_item::new_comparator(
                      thd, (Item_result)i, orig_item->args[first_expr_num],
                      cmp_collation.collation)) ||
                DBUG_EVALUATE_IF("item_func_case_copy_error", true, false))
              return true;
          }
        }
        return false;
      }));
}

Item *Item_func_if::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_if(copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_if>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_month::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_month(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_month>(thd, select, this,
                                                           item_creator);
}

/* Item_func_coalesce start */
Item *Item_func_coalesce::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_coalesce>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count, list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = list;

        new_item =
            new (thd->pq_mem_root) Item_func_coalesce(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_any_value::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_any_value(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_any_value>(
      thd, select, this, item_creator);
}

Item *Item_func_ifnull::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_ifnull(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_ifnull>(thd, select, this,
                                                            item_creator);
}
/* Item_func_coalesce end */

/* Item_func_min_max start */
Item *Item_func_max::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_max>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;

        new_item = new (thd->mem_root) Item_func_max(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_min::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_min>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;

        new_item = new (thd->mem_root) Item_func_min(POS(), &pt_item_list);
        return false;
      }));
}

/* Item_func_min_max end */

/* Item_func_num1 start */
Item *Item_func_abs::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_abs(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_abs>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_ceiling::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ceiling(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_ceiling>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_neg::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_neg(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_neg>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_round::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_round(copy_args[0], copy_args[1], truncate);
  };
  return pq_def::func_item_clone_template<Item_func_round>(thd, select, this,
                                                           item_creator);
}

/* Item_func_num1 end */

/* Item_num_op start */
Item *Item_func_plus::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_plus(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_plus>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_minus::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_minus(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_minus>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_div::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_div(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_div>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_mod::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_mod(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_mod>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_mul::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_mul(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_mul>(thd, select, this,
                                                         item_creator);
}

/* Item_num_op end */

/* Item_func_regexp start */

bool Item_func_regexp::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_regexp, Item_func>(
      this, thd, select, item,
      ([this, thd, select, item]([[maybe_unused]] Item_func_regexp *orig_item) {
        m_facade =
            make_unique_destroy_only<regexp::Regexp_facade>(thd->mem_root);
        return false;
        return false;
      }));
}

Item *Item_func_regexp_instr::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_regexp_instr>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }

        PT_item_list pt_item_list;
        pt_item_list.value = item_list;
        new_item =
            new (thd->pq_mem_root) Item_func_regexp_instr(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_regexp_like::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_regexp_like>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;

        new_item =
            new (thd->mem_root) Item_func_regexp_like(POS(), &pt_item_list);
        return false;
      }));
}

/* Item_func_regexp end */

/* Item_func_weekday start */
Item *Item_func_weekday::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_weekday>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item = new (thd->mem_root)
            Item_func_weekday(POS(), item_list[0], this->odbc_type);
        return false;
      }));
}

Item *Item_func_dayname::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_dayname(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_dayname>(thd, select, this,
                                                             item_creator);
}

/* Item_func_weekday end */

/* Item_int_func start */
/* Item_bool_func2 start */
Item *Item_func_eq::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_eq(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_eq>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_equal::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_equal(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_equal>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_ge::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ge(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_ge>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_gt::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_gt(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_gt>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_le::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_le(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_le>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_lt::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_lt(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_lt>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_ne::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ne(copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_ne>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_pi::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_pi>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_func_pi(POS());
        return false;
      }));
}

Item *Item_func_like::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_like>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg0 = args[0]->pq_clone(thd, select);
        if (arg0 == nullptr) return true;

        Item *arg1 = args[1]->pq_clone(thd, select);
        if (arg1 == nullptr) return true;

        if (escape_was_used_in_parsing()) {
          Item *escape_item = nullptr;
          if (args[2]) {
            escape_item = args[2]->pq_clone(thd, select);
            if (escape_item == nullptr) return true;
          }
          new_item =
              new (thd->pq_mem_root) Item_func_like(arg0, arg1, escape_item);
        } else {
          new_item = new (thd->pq_mem_root) Item_func_like(arg0, arg1);
        }
        return false;
      }));
}

bool Item_func_like::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_like, Item_bool_func2>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_like *orig_item) {
        // In Item_func_like::eval_escape_clause function, it will be
        // called by Item_func_like::fix_fields and it defines
        // assert(!escape_evaluated) to ensure that escape_evaluated is not set.
        // So, the following copy action is applied to the PQ.
        assert((!plan_cache::is_clone_for_plan_cache(select)));
        escape_is_const = orig_item->escape_is_const;
        escape_evaluated = orig_item->escape_evaluated;
        m_escape = orig_item->m_escape;
        return false;
      }));
}

Item *Item_func_nullif::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_nullif(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_nullif>(thd, select, this,
                                                            item_creator);
}

bool Item_func_nullif::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_nullif, Item_bool_func2>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_nullif *orig_item) {
        cached_result_type = orig_item->cached_result_type;
        return false;
      }));
}

Item *Item_func_strcmp::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_strcmp(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_strcmp>(thd, select, this,
                                                            item_creator);
}

Item *Item_func_xor::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_xor(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_xor>(thd, select, this,
                                                         item_creator);
}

bool Item_bool_func2::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_bool_func2, Item_bool_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_bool_func2 *orig_item) {
        abort_on_null = orig_item->abort_on_null;
        return false;
      }));
}

/* Item_bool_func2 end */

/* Item_cond start */
bool Item_cond::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_cond, Item_bool_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_cond *orig_item) {
        Item *list_item;
        List_iterator_fast<Item> list_it(orig_item->list);
        while ((list_item = list_it++)) {
          // TODO if arg == null, pq_copy_from should also return null
          Item *arg = list_item->pq_clone(thd, select);
          if (arg == nullptr) return true;
          list.push_back(arg);
        }
        abort_on_null = orig_item->abort_on_null;
        return false;
      }));
}

Item *Item_cond_and::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cond_and>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cond_and();
        return false;
      }));
}

bool Item_cond_and::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_cond_and, Item_cond>(
      this, thd, select, item,
      ([this, thd, select, item](Item_cond_and *orig_item) {
        cond_equal.max_members = orig_item->cond_equal.max_members;
        // TODO upper_levels
        Item_equal *item_equal;
        List_iterator_fast<Item_equal> it(orig_item->cond_equal.current_level);
        for (size_t i = 0; (item_equal = it++); i++) {
          Item_equal *new_item_equal =
              dynamic_cast<Item_equal *>(item_equal->pq_clone(thd, select));
          if (new_item_equal == nullptr) return true;
          cond_equal.current_level.push_back(new_item_equal);
        }
        return false;
      }));
}

Item *Item_equal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_equal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_equal();
        return false;
      }));
}

bool Item_equal::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_equal, Item_bool_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_equal *orig_item) {
        Item_field *item_field;
        List_iterator_fast<Item_field> it(orig_item->fields);
        for (size_t i = 0; (item_field = it++); i++) {
          Item_field *new_field =
              dynamic_cast<Item_field *>(item_field->pq_clone(thd, select));
          if (new_field == nullptr) return true;
          fields.push_back(new_field);
        }
        if (orig_item->m_const_arg != nullptr) {
          m_const_arg = orig_item->m_const_arg->pq_clone(thd, select);
          if (m_const_arg == nullptr) return true;
        }
        // item_equal has been optimized away so shouldn't resolved again
        if (!fields.elements) {
          fixed = true;
        }
        cond_false = orig_item->cond_false;
        compare_as_dates = orig_item->compare_as_dates;
        return false;
      }));
}

Item *Item_func_true::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_true(POS());
  };
  return pq_def::func_item_clone_template<Item_func_true>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_false::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_false(POS());
  };
  return pq_def::func_item_clone_template<Item_func_false>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_isnotnull::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_isnotnull(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_isnotnull>(
      thd, select, this, item_creator);
}

Item *Item_func_isnull::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_isnull>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg = args[0]->pq_clone(thd, select);
        if (arg == nullptr) return true;
        new_item = new (thd->pq_mem_root) Item_func_isnull(POS(), arg);
        return false;
      }));
}

bool Item_func_isnull::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_isnull, Item_bool_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_isnull *orig_item) {
        cached_value = orig_item->cached_value;
        cache_used = orig_item->cache_used;
        return false;
      }));
}

Item *Item_func_not::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_not(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_not>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_truth::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_truth>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item = new Item_func_truth(POS(), item_list[0], truth_test);
        return false;
      }));
}

Item *Item_extract::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_extract>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item = new Item_extract(POS(), this->int_type, item_list[0]);
        return false;
      }));
}

bool Item_extract::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_extract, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_extract *orig_item) {
        date_value = orig_item->date_value;
        return false;
      }));
}

Item *Item_func_ascii::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ascii(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_ascii>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_bit_count::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_bit_count(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_bit_count>(
      thd, select, this, item_creator);
}

Item *Item_func_char_length::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_char_length>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count == 1);
        Item *arg = args[0]->pq_clone(thd, select);
        if (arg == nullptr) return true;
        new_item = new (thd->pq_mem_root) Item_func_char_length(POS(), arg);
        return false;
      }));
}

bool Item_func_char_length::pq_copy_from(THD *thd, Query_block *select,
                                         Item *item) {
  return pq_def::pq_copy_from_template<Item_func_char_length, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_char_length *orig_item) {
        value.copy(orig_item->value);
        return false;
      }));
}

Item *Item_func_coercibility::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_coercibility(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_coercibility>(
      thd, select, this, item_creator);
}

Item *Item_func_crc32::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_crc32(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_crc32>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_dayofmonth::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_dayofmonth(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_dayofmonth>(
      thd, select, this, item_creator);
}

Item *Item_func_dayofyear::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_dayofyear(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_dayofyear>(
      thd, select, this, item_creator);
}

Item *Item_func_field::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_field>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;

        new_item = new (thd->mem_root) Item_func_field(POS(), &pt_item_list);
        return false;
      }));
}

bool Item_func_field::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_field, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_field *orig_item) {
        cmp_type = orig_item->cmp_type;
        return false;
      }));
}

Item *Item_func_find_in_set::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_find_in_set(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_find_in_set>(
      thd, select, this, item_creator);
}

bool Item_func_find_in_set::pq_copy_from(THD *thd, Query_block *select,
                                         Item *item) {
  return pq_def::pq_copy_from_template<Item_func_find_in_set, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_find_in_set *orig_item) {
        m_enum_value = orig_item->m_enum_value;
        cmp_collation = orig_item->cmp_collation;
        return false;
      }));
}

Item *Item_func_hour::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_hour(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_hour>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_inet_aton::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_inet_aton(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_inet_aton>(
      thd, select, this, item_creator);
}

Item *Item_func_div_int::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_div_int(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_div_int>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_interval::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_interval>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count == 1 && args[0]->type() == Item::ROW_ITEM);
        Item *item = args[0]->pq_clone(thd, select);
        if (!item) return true;
        Item_row *new_row = down_cast<Item_row *>(item);
        new_item = new (thd->pq_mem_root) Item_func_interval(POS(), new_row);
        return false;
      }));
}

bool Item_func_interval::pq_copy_from(THD *thd, Query_block *select,
                                      Item *item) {
  return pq_def::pq_copy_from_template<Item_func_interval, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_interval *orig_item) {
        use_decimal_comparison = orig_item->use_decimal_comparison;
        intervals = orig_item->intervals;
        return false;
      }));
}

Item *Item_func_last_insert_id::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_last_insert_id>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        if (arg_count == 0) {
          new_item = new (thd->pq_mem_root) Item_func_last_insert_id(POS());
        } else if (arg_count == 1) {
          Item *item_arg = args[0]->pq_clone(thd, select);
          if (item_arg == nullptr) return true;
          new_item =
              new (thd->pq_mem_root) Item_func_last_insert_id(POS(), item_arg);
        }
        return false;
      }));
}

Item *Item_func_length::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_length(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_length>(thd, select, this,
                                                            item_creator);
}

Item *Item_func_bit_length::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_bit_length(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_bit_length>(
      thd, select, this, item_creator);
}

Item *Item_func_minute::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_minute(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_minute>(thd, select, this,
                                                            item_creator);
}

// TODO optimize this function
Item *Item_func_locate::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_locate>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 4);
        Item *new_args[4] = {nullptr};
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 2) {
          new_item = new (thd->pq_mem_root)
              Item_func_locate(POS(), new_args[0], new_args[1]);
        } else if (arg_count == 3) {
          new_item = new (thd->pq_mem_root)
              Item_func_locate(POS(), new_args[0], new_args[1], new_args[2]);
        }
        return false;
      }));
}

bool Item_func_locate::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_locate, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item]([[maybe_unused]] Item_func_locate *orig_item) {
        return false;
      }));
}

Item *Item_func_instr::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_instr(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_instr>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_microsecond::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_microsecond(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_microsecond>(
      thd, select, this, item_creator);
}

bool Item_func_opt_neg::pq_copy_from(THD *thd, Query_block *select,
                                     Item *item) {
  return pq_def::pq_copy_from_template<Item_func_opt_neg, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_opt_neg *orig_item) {
        negated = orig_item->negated;
        pred_level = orig_item->pred_level;
        return false;
      }));
}

Item *Item_func_between::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_between(
        POS(), copy_args[0], copy_args[1], copy_args[2], negated);
  };
  return pq_def::func_item_clone_template<Item_func_between>(thd, select, this,
                                                             item_creator);
}

bool Item_func_between::pq_copy_from(THD *thd, Query_block *select,
                                     Item *item) {
  return pq_def::pq_copy_from_template<Item_func_between, Item_func_opt_neg>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_between *orig_item) {
        compare_as_dates_with_strings =
            orig_item->compare_as_dates_with_strings;
        compare_as_temporal_dates = orig_item->compare_as_temporal_dates;
        compare_as_temporal_times = orig_item->compare_as_temporal_times;
        cmp_type = orig_item->cmp_type;
        cmp_collation = orig_item->cmp_collation;
        if (compare_as_dates_with_strings) {
          ge_cmp.set_datetime_cmp_func(this, args, args + 1);
          le_cmp.set_datetime_cmp_func(this, args, args + 2);
        }
        return false;
      }));
}

Item *Item_func_in::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_in>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        PT_item_list pt_item;
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        pt_item.value)) {
          return true;
        }
        new_item = new Item_func_in(POS(), &pt_item, negated);
        return false;
      }));
}

bool Item_func_in::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_in, Item_func_opt_neg>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_in *orig_item) {
        m_values_are_const = orig_item->m_values_are_const;
        dep_subq_in_list = orig_item->dep_subq_in_list;
        first_resolve_call = orig_item->first_resolve_call;
        left_result_type = orig_item->left_result_type;
        cmp_collation = orig_item->cmp_collation;
        for (uint i = 0; i <= (uint)DECIMAL_RESULT + 1; i++) {
          if (orig_item->cmp_items[i] &&
              !(cmp_items[i] = orig_item->cmp_items[i]->pq_clone(thd)))
            return true;
        }
        if (orig_item->m_const_array &&
            !(m_const_array = orig_item->m_const_array->pq_clone(thd)))
          return true;
        return false;
      }));
}

Item *Item_func_ord::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ord(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_ord>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_period_add::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_period_add(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_period_add>(
      thd, select, this, item_creator);
}

Item *Item_func_period_diff::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_period_diff(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_period_diff>(
      thd, select, this, item_creator);
}

Item *Item_func_quarter::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_quarter(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_quarter>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_second::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_second(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_second>(thd, select, this,
                                                            item_creator);
}

Item *Item_func_time_to_sec::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_time_to_sec(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_time_to_sec>(
      thd, select, this, item_creator);
}

Item *Item_func_timestamp_diff::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_timestamp_diff(POS(), copy_args[0], copy_args[1], int_type);
  };
  return pq_def::func_item_clone_template<Item_func_timestamp_diff>(
      thd, select, this, item_creator);
}

Item *Item_func_to_days::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_to_days(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_to_days>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_to_seconds::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_to_seconds(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_to_seconds>(
      thd, select, this, item_creator);
}

Item *Item_func_uncompressed_length::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_uncompressed_length(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_uncompressed_length>(
      thd, select, this, item_creator);
}

Item *Item_func_week::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_week(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_week>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_year::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_year(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_year>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_yearweek::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_yearweek(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_yearweek>(thd, select, this,
                                                              item_creator);
}

Item *Item_typecast_signed::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_typecast_signed(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_typecast_signed>(
      thd, select, this, item_creator);
}

Item *Item_typecast_unsigned::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_typecast_unsigned(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_typecast_unsigned>(
      thd, select, this, item_creator);
}

/* Item_int_func end */

/* Item_real_func start */
/* Item_dec_func start*/  // TODO add more dec functions
Item *Item_func_sin::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_sin(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_sin>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_sqrt::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_sqrt(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_sqrt>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_cos::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_cos(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_cos>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_tan::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_tan(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_tan>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_cot::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_cot(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_cot>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_pow::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_pow(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_pow>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_ln::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ln(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_ln>(thd, select, this,
                                                        item_creator);
}

Item *Item_func_log2::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_log2(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_log2>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_log10::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_log10(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_log10>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_asin::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_asin(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_asin>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_acos::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_acos(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_acos>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_exp::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_exp(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_exp>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_atan::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_atan>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *item_args[2];
        assert(arg_count < 3);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_args)) {
          return true;
        }

        if (arg_count == 1)
          new_item = new (thd->pq_mem_root) Item_func_atan(POS(), item_args[0]);
        else if (arg_count == 2)
          new_item = new (thd->pq_mem_root)
              Item_func_atan(POS(), item_args[0], item_args[1]);
        return false;
      }));
}

Item *Item_func_log::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_log>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *item_args[2];
        assert(arg_count < 3);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_args)) {
          return true;
        }

        if (arg_count == 1)
          new_item = new (thd->pq_mem_root) Item_func_log(POS(), item_args[0]);
        else if (arg_count == 2)
          new_item = new (thd->pq_mem_root)
              Item_func_log(POS(), item_args[0], item_args[1]);
        return false;
      }));
}

/* Item_str_func start */
Item *Item_func_aes_decrypt::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_aes_decrypt>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 4);
        Item *new_args[4] = {nullptr};
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 2) {
          new_item = new (thd->pq_mem_root)
              Item_func_aes_decrypt(POS(), new_args[0], new_args[1]);
        } else if (arg_count == 3) {
          new_item = new (thd->pq_mem_root) Item_func_aes_decrypt(
              POS(), new_args[0], new_args[1], new_args[2]);
        }
        return false;
      }));
}

Item *Item_func_aes_encrypt::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_aes_encrypt>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 4);
        Item *new_args[4] = {nullptr};
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 2) {
          new_item = new (thd->pq_mem_root)
              Item_func_aes_encrypt(POS(), new_args[0], new_args[1]);
        } else if (arg_count == 3) {
          new_item = new (thd->pq_mem_root) Item_func_aes_encrypt(
              POS(), new_args[0], new_args[1], new_args[2]);
        }
        return false;
      }));
}

Item *Item_func_char::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_char>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;
        new_item = new (thd->pq_mem_root) Item_func_char(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_charset::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_charset(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_charset>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_collation::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_collation(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_collation>(
      thd, select, this, item_creator);
}

Item *Item_func_compress::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_compress(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_compress>(thd, select, this,
                                                              item_creator);
}

Item *Item_func_concat::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_concat>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;
        new_item =
            new (thd->pq_mem_root) Item_func_concat(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_concat_ws::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_concat_ws>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;
        new_item =
            new (thd->pq_mem_root) Item_func_concat_ws(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_conv::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_conv(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_conv>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_conv_charset::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_conv_charset>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg0 = args[0]->pq_clone(thd, select);
        if (!arg0) return true;
        if (m_use_cached_value) {
          if (!arg0->fixed && arg0->refix_fields(thd, &arg0)) return true;
          new_item = new (thd->mem_root)
              Item_func_conv_charset(thd, arg0, m_cast_cs, m_use_cached_value);
        } else {
          new_item = new (thd->mem_root)
              Item_func_conv_charset(POS(), arg0, m_cast_cs);
        }
        return false;
      }));
}

Item *Item_func_date_format::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_date_format>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item = new (thd->mem_root) Item_func_date_format(
            POS(), item_list[0], item_list[1], this->is_time_format);
        return false;
      }));
}

bool Item_func_date_format::pq_copy_from(THD *thd, Query_block *select,
                                         Item *item) {
  return pq_def::pq_copy_from_template<Item_func_date_format, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_date_format *orig_item) {
        fixed_length = orig_item->fixed_length;
        return false;
      }));
}

Item *Item_func_elt::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_elt>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;

        new_item = new (thd->mem_root) Item_func_elt(POS(), &pt_item_list);
        return false;
      }));
}

Item *Item_func_export_set::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_export_set>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }

        if (arg_count == 3) {
          new_item = new (thd->pq_mem_root) Item_func_export_set(
              POS(), item_list[0], item_list[1], item_list[2]);
        } else if (arg_count == 4) {
          new_item = new (thd->pq_mem_root) Item_func_export_set(
              POS(), item_list[0], item_list[1], item_list[2], item_list[3]);
        } else if (arg_count == 5) {
          new_item = new (thd->pq_mem_root)
              Item_func_export_set(POS(), item_list[0], item_list[1],
                                   item_list[2], item_list[3], item_list[4]);
        }
        return false;
      }));
}

Item *Item_func_from_base64::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_from_base64(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_from_base64>(
      thd, select, this, item_creator);
}

Item *Item_func_inet_ntoa::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_inet_ntoa(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_inet_ntoa>(
      thd, select, this, item_creator);
}

Item *Item_func_insert::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_insert(
        POS(), copy_args[0], copy_args[1], copy_args[2], copy_args[3]);
  };
  return pq_def::func_item_clone_template<Item_func_insert>(thd, select, this,
                                                            item_creator);
}

Item *Item_func_left::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_left(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_left>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_lpad::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_lpad(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_lpad>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_make_set::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_make_set>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg_a = item->pq_clone(thd, select);
        if (arg_a == nullptr) return true;

        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        PT_item_list pt_item_list;
        pt_item_list.value = item_list;

        new_item = new (thd->pq_mem_root)
            Item_func_make_set(POS(), arg_a, &pt_item_list);
        return false;
      }));
}

Item *Item_func_monthname::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_monthname(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_monthname>(
      thd, select, this, item_creator);
}

Item *Item_func_pfs_format_bytes::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_pfs_format_bytes(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_pfs_format_bytes>(
      thd, select, this, item_creator);
}

bool Item_func_monthname::pq_copy_from(THD *thd, Query_block *select,
                                       Item *item) {
  return pq_def::pq_copy_from_template<Item_func_monthname, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select,
        item]([[maybe_unused]] Item_func_monthname *orig_item) {
        locale = thd->variables.lc_time_names;
        return false;
      }));
}

Item *Item_func_pfs_format_pico_time::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_pfs_format_pico_time(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_pfs_format_pico_time>(
      thd, select, this, item_creator);
}

Item *Item_func_quote::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_quote(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_quote>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_repeat::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_repeat(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_repeat>(thd, select, this,
                                                            item_creator);
}

Item *Item_func_replace::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_replace(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_replace>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_reverse::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_reverse(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_reverse>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_right::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_right>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item = new (thd->mem_root)
            Item_func_right(POS(), item_list[0], item_list[1]);
        return false;
      }));
}

Item *Item_func_rpad::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_rpad(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_rpad>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_set_collation::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_set_collation(POS(), copy_args[0], collation_string);
  };
  return pq_def::func_item_clone_template<Item_func_set_collation>(
      thd, select, this, item_creator);
}

// TODO args[1] copyed twice
// This function can not delete, because does not rewrite
// pq_copy_from and it will call Item_str_func::pq_copy_from
// and it has 2 args but only copy arg[0], so it will crash
// when args[1].copy
bool Item_func_set_collation::pq_copy_from(THD *thd, Query_block *select,
                                           Item *item) {
  return pq_def::pq_copy_from_template<Item_func_set_collation, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_set_collation *orig_item) {
        if (orig_item->args[1] != nullptr) {
          args[1] = orig_item->args[1]->pq_clone(thd, select);
          if (args[1] == nullptr) return true;
        }
        return false;
      }));
}

Item *Item_func_soundex::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_soundex(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_soundex>(thd, select, this,
                                                             item_creator);
}

bool Item_func_soundex::pq_copy_from(THD *thd, Query_block *select,
                                     Item *item) {
  return pq_def::pq_copy_from_template<Item_func_soundex, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select,
        item]([[maybe_unused]] Item_func_soundex *orig_item) {
        tmp_value.set_charset(collation.collation);
        return false;
      }));
}

Item *Item_func_space::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_space(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_space>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_substr::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_substr>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 4);
        Item *new_args[4] = {nullptr};
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 2) {
          new_item = new (thd->pq_mem_root)
              Item_func_substr(POS(), new_args[0], new_args[1]);
        } else if (arg_count == 3) {
          new_item = new (thd->pq_mem_root)
              Item_func_substr(POS(), new_args[0], new_args[1], new_args[2]);
        }
        return false;
      }));
}

Item *Item_func_substr_index::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_substr_index(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_substr_index>(
      thd, select, this, item_creator);
}

Item *Item_func_database::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_database(POS());
  };
  return pq_def::func_item_clone_template<Item_func_database>(thd, select, this,
                                                              item_creator);
}

Item *Item_func_trim::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_trim>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }

        if (arg_count > 1)
          new_item = new (thd->mem_root)
              Item_func_trim(POS(), item_list[0], item_list[1], m_trim_mode);
        else
          new_item = new (thd->mem_root)
              Item_func_trim(POS(), item_list[0], m_trim_mode);
        return false;
      }));
}

bool Item_func_trim::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_trim, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_trim *orig_item) {
        remove.copy(orig_item->remove);
        return false;
      }));
}

Item *Item_func_ltrim::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_ltrim(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_ltrim>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_rtrim::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_rtrim(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_rtrim>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_unhex::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_unhex(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_unhex>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_uuid::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_uuid(POS());
  };
  return pq_def::func_item_clone_template<Item_func_uuid>(thd, select, this,
                                                          item_creator);
}

Item *Item_func_uuid_to_bin::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_uuid_to_bin>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 3);
        Item *new_args[4] = {nullptr};

        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 1) {
          new_item =
              new (thd->pq_mem_root) Item_func_uuid_to_bin(POS(), new_args[0]);
        } else if (arg_count == 2) {
          new_item = new (thd->pq_mem_root)
              Item_func_uuid_to_bin(POS(), new_args[0], new_args[1]);
        }
        return false;
      }));
}

Item *Item_func_bin_to_uuid::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_bin_to_uuid>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 3);
        Item *new_args[4] = {nullptr};

        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 1) {
          new_item =
              new (thd->pq_mem_root) Item_func_bin_to_uuid(POS(), new_args[0]);
        } else if (arg_count == 2) {
          new_item = new (thd->pq_mem_root)
              Item_func_bin_to_uuid(POS(), new_args[0], new_args[1]);
        }
        return false;
      }));
}

Item *Item_func_format::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_format>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count < 4);
        Item *new_args[4] = {nullptr};
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        new_args)) {
          return true;
        }

        if (arg_count == 2)
          new_item = new (thd->pq_mem_root)
              Item_func_format(POS(), new_args[0], new_args[1]);
        else if (arg_count == 3)
          new_item = new (thd->pq_mem_root)
              Item_func_format(POS(), new_args[0], new_args[1], new_args[2]);
        return false;
      }));
}

bool Item_func_format::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_format, Item_str_ascii_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_format *orig_item) {
        locale = orig_item->locale;
        return false;
      }));
}

Item *Item_func_get_format::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_get_format>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count == 1);
        Item *arg = args[0]->pq_clone(thd, select);
        if (arg == nullptr) return true;

        new_item =
            new (thd->pq_mem_root) Item_func_get_format(POS(), type, arg);
        return false;
      }));
}

Item *Item_func_hex::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_hex(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_hex>(thd, select, this,
                                                         item_creator);
}

Item *Item_func_inet6_aton::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_inet6_aton(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_inet6_aton>(
      thd, select, this, item_creator);
}

Item *Item_func_inet6_ntoa::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_inet6_ntoa(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_inet6_ntoa>(
      thd, select, this, item_creator);
}

Item *Item_func_to_base64::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_to_base64(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_to_base64>(
      thd, select, this, item_creator);
}

bool Item_str_conv::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_str_conv, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_str_conv *orig_item) {
        multiply = orig_item->multiply;
        converter = orig_item->converter;
        return false;
      }));
}

Item *Item_func_upper::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_upper(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_upper>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_lower::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_lower(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_lower>(thd, select, this,
                                                           item_creator);
}

bool Item_temporal_hybrid_func::pq_copy_from(THD *thd, Query_block *select,
                                             Item *item) {
  return pq_def::pq_copy_from_template<Item_temporal_hybrid_func,
                                       Item_str_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_temporal_hybrid_func *orig_item) {
        sql_mode = orig_item->sql_mode;
        ascii_buf.copy(orig_item->ascii_buf);
        return false;
      }));
}

Item *Item_date_add_interval::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_date_add_interval>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        // TODO only support two args;
        Item *arg_a = args[0]->pq_clone(thd, select);
        Item *arg_b = args[1]->pq_clone(thd, select);
        if (arg_a == nullptr || arg_b == nullptr) return true;
        new_item = new (thd->pq_mem_root) Item_date_add_interval(
            arg_a, arg_b, get_interval_type(), is_subtract());
        if (new_item) {
          new_item->set_data_type(data_type());
        }
        return false;
      }));
}

bool Item_date_add_interval::pq_copy_from(THD *thd, Query_block *select,
                                          Item *item) {
  return pq_def::pq_copy_from_template<Item_date_add_interval,
                                       Item_temporal_hybrid_func>(
      this, thd, select, item,
      ([this, thd, select,
        item]([[maybe_unused]] Item_date_add_interval *orig_item) {
        value.alloc(max_length);
        return false;
      }));
}

Item *Item_func_add_time::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_add_time(POS(), copy_args[0], copy_args[1], m_datetime,
                           sign() == -1 ? true : false);
  };
  return pq_def::func_item_clone_template<Item_func_add_time>(thd, select, this,
                                                              item_creator);
}

Item *Item_func_str_to_date::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_str_to_date(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_str_to_date>(
      thd, select, this, item_creator);
}

bool Item_func_str_to_date::pq_copy_from(THD *thd, Query_block *select,
                                         Item *item) {
  return pq_def::pq_copy_from_template<Item_func_str_to_date,
                                       Item_temporal_hybrid_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_str_to_date *orig_item) {
        cached_timestamp_type = orig_item->cached_timestamp_type;
        return false;
      }));
}

bool Item_charset_conversion::pq_copy_from(THD *thd, Query_block *select,
                                           Item *item) {
  return pq_def::pq_copy_from_template<Item_charset_conversion, Item_str_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_charset_conversion *orig_item) {
        m_from_cs = orig_item->m_from_cs;
        m_cast_cs = orig_item->m_cast_cs;
        m_charset_conversion = orig_item->m_charset_conversion;
        m_use_cached_value = orig_item->m_use_cached_value;
        m_cast_length = orig_item->m_cast_length;
        m_safe = orig_item->m_safe;
        return false;
      }));
}

Item *Item_typecast_char::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_typecast_char(POS(), copy_args[0], m_cast_length, m_cast_cs);
  };
  return pq_def::func_item_clone_template<Item_typecast_char>(thd, select, this,
                                                              item_creator);
}

Item *Item_date_literal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_date_literal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        MYSQL_TIME ltime;
        cached_time.get_time(&ltime);
        new_item = new (thd->pq_mem_root) Item_date_literal(&ltime);
        return false;
      }));
}

/* Item_str_func end */

Item *Item_func_curdate_utc::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_curdate_utc(POS());
  };
  return pq_def::func_item_clone_template<Item_func_curdate_utc>(
      thd, select, this, item_creator);
}

Item *Item_func_curdate_local::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_curdate_local(POS());
  };
  return pq_def::func_item_clone_template<Item_func_curdate_local>(
      thd, select, this, item_creator);
}

Item *Item_func_from_days::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_from_days(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_from_days>(
      thd, select, this, item_creator);
}

Item *Item_func_makedate::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_makedate(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_makedate>(thd, select, this,
                                                              item_creator);
}

Item *Item_typecast_date::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_typecast_date(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_typecast_date>(thd, select, this,
                                                              item_creator);
}

bool Item_typecast_date::pq_copy_from(THD *thd, Query_block *select,
                                      Item *item) {
  return pq_def::pq_copy_from_template<Item_typecast_date, Item_date_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_typecast_date *orig_item) {
        m_explicit_cast = orig_item->m_explicit_cast;
        return false;
      }));
}

Item *Item_datetime_literal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_datetime_literal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        MYSQL_TIME *ltime = new (thd->pq_mem_root) MYSQL_TIME();
        this->get_date(ltime, 0);
        new_item = new (thd->pq_mem_root) Item_datetime_literal(
            ltime, this->cached_time.decimals(), thd->variables.time_zone);
        return false;
      }));
}

Item *Item_func_convert_tz::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_convert_tz(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_convert_tz>(
      thd, select, this, item_creator);
}

Item *Item_func_from_unixtime::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_from_unixtime(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_from_unixtime>(
      thd, select, this, item_creator);
}

Item *Item_func_sysdate_local::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_sysdate_local>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_func_sysdate_local(decimals);
        return false;
      }));
}

Item *Item_typecast_datetime::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_typecast_datetime>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg_item = args[0]->pq_clone(thd, select);
        if (!arg_item) return true;

        new_item =
            new (thd->pq_mem_root) Item_typecast_datetime(POS(), arg_item);
        return false;
      }));
}

bool Item_typecast_datetime::pq_copy_from(THD *thd, Query_block *select,
                                          Item *item) {
  return pq_def::pq_copy_from_template<Item_typecast_datetime,
                                       Item_datetime_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_typecast_datetime *orig_item) {
        detect_precision_from_arg = orig_item->detect_precision_from_arg;
        decimals = orig_item->decimals;
        m_explicit_cast = orig_item->m_explicit_cast;
        return false;
      }));
}

Item *Item_func_curtime_local::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_curtime_local>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        mem_root_deque<Item *> item_list(thd->pq_mem_root);
        if (pq_def::clone_all_arguments(thd, select, args, arg_count,
                                        item_list)) {
          return true;
        }
        new_item =
            new (thd->mem_root) Item_func_curtime_local(POS(), this->decimals);
        return false;
      }));
}

Item *Item_func_curtime_utc::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_curtime_utc(POS(), decimals);
  };
  return pq_def::func_item_clone_template<Item_func_curtime_utc>(
      thd, select, this, item_creator);
}

Item *Item_func_maketime::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_maketime(POS(), copy_args[0], copy_args[1], copy_args[2]);
  };
  return pq_def::func_item_clone_template<Item_func_maketime>(thd, select, this,
                                                              item_creator);
}

Item *Item_func_sec_to_time::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_sec_to_time(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_sec_to_time>(
      thd, select, this, item_creator);
}

Item *Item_func_timediff::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_timediff(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_timediff>(thd, select, this,
                                                              item_creator);
}

Item *Item_typecast_time::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_typecast_time(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_typecast_time>(thd, select, this,
                                                              item_creator);
}

Item *Item_func_now_local::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_now_local(POS(), decimals);
  };
  return pq_def::func_item_clone_template<Item_func_now_local>(
      thd, select, this, item_creator);
}

Item *Item_func_now_utc::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_now_utc(POS(), decimals);
  };
  return pq_def::func_item_clone_template<Item_func_now_utc>(thd, select, this,
                                                             item_creator);
}

bool Item_typecast_time::pq_copy_from(THD *thd, Query_block *select,
                                      Item *item) {
  return pq_def::pq_copy_from_template<Item_typecast_time, Item_time_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_typecast_time *orig_item) {
        detect_precision_from_arg = orig_item->detect_precision_from_arg;
        decimals = orig_item->decimals;
        m_explicit_cast = orig_item->m_explicit_cast;
        return false;
      }));
}

Item *Item_time_literal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_time_literal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        MYSQL_TIME *ltime = new (thd->pq_mem_root) MYSQL_TIME();
        if (ltime == nullptr) return true;
        cached_time.get_time(ltime);
        new_item =
            new (thd->pq_mem_root) Item_time_literal(ltime, saved_dec_arg);
        return false;
      }));
}

Item *Item_typecast_decimal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_typecast_decimal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *item_arg = args[0]->pq_clone(thd, select);
        if (item_arg == nullptr) return true;

        new_item = new (thd->pq_mem_root)
            Item_typecast_decimal(POS(), item_arg, saved_presion, decimals);
        return false;
      }));
}

Item *Item_typecast_real::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_typecast_real>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *item_arg = args[0]->pq_clone(thd, select);
        if (!item_arg) return true;

        new_item = new (thd->pq_mem_root)
            Item_typecast_real(POS(), item_arg, double_type);
        return false;
      }));
}

Item *Item_func_get_system_var::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_get_system_var>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_func_get_system_var(var_tracker, var_scope);
        return false;
      }));
}

bool Item_func_get_system_var::pq_copy_from(THD *thd, Query_block *select,
                                            Item *item) {
  return pq_def::pq_copy_from_template<Item_func_get_system_var, Item_var_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_get_system_var *orig_item) {
        cached_llval = orig_item->cached_llval;
        cached_dval = orig_item->cached_dval;
        cached_strval.copy(orig_item->cached_strval);
        cached_null_value = orig_item->cached_null_value;
        used_query_id = orig_item->used_query_id;
        cache_present = orig_item->cache_present;
        return false;
      }));
}

/* Item_func end */

/* Item sum start */
bool Item_sum::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_sum, Item_result_field>(
      this, thd, select, item, ([this, thd, select, item](Item_sum *orig_item) {
        // TODO need copy
        // aggr
        // m_window
        // m_window_resolved
        m_window = orig_item->m_window;
        m_window_resolved = orig_item->m_window_resolved;
        force_copy_fields = orig_item->force_copy_fields;
        with_distinct = orig_item->with_distinct;
        // ref_by
        // next_sum
        // in_sum_func
        // base_select
        // aggr_select
        base_query_block = select;
        max_aggr_level = orig_item->max_aggr_level;
        max_sum_func_level = orig_item->max_sum_func_level;
        allow_group_via_temp_table = orig_item->allow_group_via_temp_table;
        save_deny_window_func = orig_item->save_deny_window_func;
        used_tables_cache = orig_item->used_tables_cache;
        forced_const = orig_item->forced_const;
        orig_func = orig_item;
        return false;
      }));
}

Item_sum *Item_sum::pq_rebuild_sum_func(
    THD *thd MY_ATTRIBUTE((unused)), Query_block *select MY_ATTRIBUTE((unused)),
    Item *item MY_ATTRIBUTE((unused))) {
  sql_print_warning(
      "Item type %s's rebuild sum method is not implemented, "
      "will not use parallel query, SQL= %s",
      typeid(*this).name(), thd->query().str);
  assert(DBUG_EVALUATE_IF("simulate_no_item_rebuild_function", true, false) ||
         false);
  return nullptr;
}

bool Item_sum_hybrid::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_sum_hybrid, Item_sum>(
      this, thd, select, item,
      ([this, thd, select, item](Item_sum_hybrid *orig_item) {
        if (orig_item->value != nullptr) {
          value = dynamic_cast<Item_cache *>(
              orig_item->value->pq_clone(thd, select));
          if (value == nullptr) return true;
        }
        // arg_cache is only involved in window function as the cache
        // to store previous row.
        if (orig_item->arg_cache != nullptr &&
            orig_item->m_is_window_function) {
          arg_cache = dynamic_cast<Item_cache *>(
              orig_item->arg_cache->pq_clone(thd, select));
          if (arg_cache == nullptr) return true;
        }
        was_values = orig_item->was_values;
        hybrid_type = orig_item->hybrid_type;
        m_nulls_first = orig_item->m_nulls_first;
        m_optimize = orig_item->m_optimize;
        m_want_first = orig_item->m_want_first;
        m_cnt = orig_item->m_cnt;
        m_saved_last_value_at = orig_item->m_saved_last_value_at;
        return false;
      }));
}

Item *Item_sum_max::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_sum_max(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_sum_max>(thd, select, this,
                                                        item_creator);
}

Item_sum *Item_sum_max::pq_rebuild_sum_func(THD *thd, Query_block *select,
                                            Item *item) {
  return pq_def::rebuild_sum_func_template<Item_sum_max>(
      thd, select, this, ([this, thd, select, item](Item_sum_max *&new_item) {
        new_item = new (thd->pq_mem_root) Item_sum_max(POS(), item, nullptr);
        new_item->hidden = item->hidden;
        return false;
      }));
}

Item *Item_sum_min::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_sum_min(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_sum_min>(thd, select, this,
                                                        item_creator);
}

Item_sum *Item_sum_min::pq_rebuild_sum_func(THD *thd, Query_block *select,
                                            Item *item) {
  return pq_def::rebuild_sum_func_template<Item_sum_min>(
      thd, select, this, ([this, thd, select, item](Item_sum_min *&new_item) {
        new_item = new (thd->pq_mem_root) Item_sum_min(POS(), item, nullptr);
        new_item->hidden = item->hidden;
        return false;
      }));
}

bool Item_sum_num::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_sum_num, Item_sum>(
      this, thd, select, item,
      ([this, thd, select, item](Item_sum_num *orig_item) {
        DBUG_EXECUTE_IF("simulate_item_rebuild_attr_copy_error", return true;);

        is_evaluated = orig_item->is_evaluated;
        return false;
      }));
}

Item *Item_sum_count::pq_clone(THD *thd, Query_block *select) {
  if (pq_def::check_type_and_warn<Item_sum_count>(thd, select, this)) {
    return nullptr;
  }
  Item *new_item = nullptr;
  if (!has_with_distinct()) {
    assert(arg_count == 1);
    Item *arg = args[0]->pq_clone(thd, select);
    if (arg == nullptr) return nullptr;
    new_item = new (thd->pq_mem_root) Item_sum_count(POS(), arg, nullptr);
  } else {
    PT_item_list *list = new (thd->pq_mem_root) PT_item_list();
    for (uint i = 0; i < arg_count; i++) {
      Item *arg = args[i]->pq_clone(thd, select);
      if (arg == nullptr) {
        return nullptr;
      }
      list->push_back(arg);
    }
    new_item = new (thd->pq_mem_root) Item_sum_count(POS(), list, nullptr);
  }
  if (pq_def::copy_self_attributes(thd, select, new_item, this)) return nullptr;
  return new_item;
}

Item_sum *Item_sum_count::pq_rebuild_sum_func(THD *thd, Query_block *select,
                                              Item *item) {
  DBUG_EXECUTE_IF("simulate_item_rebuild_error", return nullptr;);
  DBUG_EXECUTE_IF("simulate_no_item_rebuild_function",
                  return Item_sum::pq_rebuild_sum_func(thd, select, item););

  Item_sum_count *new_item_sum = nullptr;
  if (has_with_distinct())
    new_item_sum = new (thd->pq_mem_root) Item_sum_count(POS(), item, nullptr);
  else
    new_item_sum =
        new (thd->pq_mem_root) Item_sum_count(POS(), item, nullptr, true);
  if (new_item_sum == nullptr ||
      new_item_sum->Item_sum_num::pq_copy_from(thd, select, this))
    return nullptr;
  return new_item_sum;
}

// TODO we copy PIT_count_sym to item_sum_count, anything wrong?
Item *PTI_count_sym::pq_clone(THD *thd, Query_block *select) {
  if (pq_def::check_type_and_warn<PTI_count_sym>(thd, select, this)) {
    return nullptr;
  }
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;
  Item_sum_count *new_count =
      new (thd->pq_mem_root) Item_sum_count(POS(), arg, nullptr);
  if (new_count == nullptr || new_count->pq_copy_from(thd, select, this))
    return nullptr;
  return new_count;
}

Item *Item_sum_sum::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_sum_sum(POS(), copy_args[0], has_with_distinct(), nullptr);
  };
  return pq_def::func_item_clone_template<Item_sum_sum>(thd, select, this,
                                                        item_creator);
}

bool Item_sum_sum::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_sum_sum, Item_sum_num>(
      this, thd, select, item,
      ([this, thd, select, item](Item_sum_sum *orig_item) {
        hybrid_type = orig_item->hybrid_type;
        curr_dec_buff = orig_item->curr_dec_buff;
        sum = orig_item->sum;
        return false;
      }));
}

Item_sum *Item_sum_sum::pq_rebuild_sum_func(THD *thd, Query_block *select,
                                            Item *item) {
  return pq_def::rebuild_sum_func_template<Item_sum_sum>(
      thd, select, this, ([this, thd, select, item](Item_sum_sum *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_sum_sum(POS(), item, has_with_distinct(), nullptr);
        new_item->hidden = item->hidden;
        return false;
      }));
}

Item *Item_sum_avg::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_sum_avg>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count == 1);
        Item *arg = args[0]->pq_clone(thd, select);
        if (arg == nullptr) return true;
        new_item = new (thd->pq_mem_root)
            Item_sum_avg(POS(), arg, has_with_distinct(), nullptr);
        if (new_item &&
            // for avg(distinct) we transfor raw items, do not need enlarge
            // field pack_length
            dynamic_cast<Item_sum_avg *>(new_item)->sum_func() !=
                Item_sum::AVG_DISTINCT_FUNC &&
            /*
              This mark PQ_WORKER is only if we have a leader and workers which
              split the AVG calculation (workers sum their rows, leader sums
              these sums and divides. If, instead, we have a AVG aggregated
              inside a correlated subquery then we need the normal calculation.
              Related to this topic: note that in the latter case
              pq_build_sum_funcs() is not called as make_leader_tmp_table() is
              not called either, which is expected.
            */
            select->parallel_exec) {
          dynamic_cast<Item_sum_avg *>(new_item)->pq_avg_type = PQ_WORKER;
        }
        return false;
      }));
}

Item_sum *Item_sum_avg::pq_rebuild_sum_func(THD *thd, Query_block *select,
                                            Item *item) {
  return pq_def::rebuild_sum_func_template<Item_sum_avg>(
      thd, select, this, ([this, thd, select, item](Item_sum_avg *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_sum_avg(POS(), item, has_with_distinct(), nullptr);
        if (new_item &&
            // for avg(distinct) we transfor raw items, do not need enlarge
            // field pack_length
            dynamic_cast<Item_sum_avg *>(new_item)->sum_func() !=
                Item_sum::AVG_DISTINCT_FUNC) {
          new_item->pq_avg_type = PQ_REBUILD;
        }
        new_item->hidden = item->hidden;
        return false;
      }));
}

/* Item sum end */
/* Item_result_field end */

Item *Item_row::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_row>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        assert(arg_count > 0);
        Item *arg_head = items[0]->pq_clone(thd, select);
        if (arg_head == nullptr) return true;

        mem_root_deque<Item *> tail(thd->pq_mem_root);
        for (uint i = 1; i < arg_count; i++) {
          Item *arg_tail = items[i]->pq_clone(thd, select);
          if (arg_tail == nullptr) return true;
          tail.push_back(arg_tail);
        }
        new_item = new (thd->pq_mem_root) Item_row(arg_head, tail);
        return false;
      }));
}

bool Item_row::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_row, Item>(
      this, thd, select, item, ([this, thd, select, item](Item_row *orig_item) {
        // generated a random item_name for item_row
        if (orig_item->item_name.length() == 0) {
          assert(orig_item->item_name.ptr() == nullptr);
          // assign the same item_name for cloned Item_row to ensure
          // that when they are equal, they can captured by Item::eq().
          std::string std_addr = "ITEM_ROW";
          item_name.copy(std_addr.c_str(), std_addr.length(),
                         system_charset_info, true);
        }

        used_tables_cache = orig_item->used_tables_cache;
        not_null_tables_cache = orig_item->not_null_tables_cache;
        with_null = orig_item->with_null;
        return false;
      }));
}

Item *Item_float::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_float>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_float(item_name, value, decimals, max_length);
        return false;
      }));
}

Item *Item_int::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_int>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_int(this);
        return false;
      }));
}

bool Item_int::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_int, Item_num>(
      this, thd, select, item, ([this, thd, select, item](Item_int *orig_item) {
        value = orig_item->value;
        return false;
      }));
}

Item *Item_uint::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_uint>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item =
            new (thd->pq_mem_root) Item_uint(item_name, value, max_length);
        return false;
      }));
}

Item *Item_decimal::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_decimal>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root)
            Item_decimal(item_name, &decimal_value, decimals, max_length);
        return false;
      }));
}

Item *Item_func_version::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_version>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_func_version(POS());
        return false;
      }));
}

Item *Item_func_icu_version::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_icu_version>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_func_icu_version(POS());
        return false;
      }));
}

Item *PTI_function_call_nonkeyword_now::pq_clone(THD *thd,
                                                 Query_block *select) {
  return pq_def::pq_clone_template<PTI_function_call_nonkeyword_now>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root)
            PTI_function_call_nonkeyword_now(POS(), decimals);
        return false;
      }));
}

Item *PTI_text_literal_text_string::pq_clone(THD *thd, Query_block *select) {
  Item *new_item = new (thd->pq_mem_root)
      PTI_text_literal_text_string(POS(), is_7bit, literal);
  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  // As it is constant, we can directly fix it without fix_fields.
  new_item->fixed = true;
  return new_item;
}

Item *PTI_text_literal_nchar_string::pq_clone(THD *thd, Query_block *select) {
  Item *new_item = new (thd->pq_mem_root)
      PTI_text_literal_nchar_string(POS(), is_7bit, literal);

  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  // it is constant => can be used without fix_fields (and frequently used)
  new_item->fixed = true;
  return new_item;
}

Item *PTI_text_literal_underscore_charset::pq_clone(THD *thd,
                                                    Query_block *select) {
  Item *new_item = new (thd->pq_mem_root)
      PTI_text_literal_underscore_charset(POS(), is_7bit, cs, literal);

  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  // it is constant => can be used without fix_fields (and frequently used)
  new_item->fixed = true;
  return new_item;
}

Item *Item_func_get_user_var::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_get_user_var>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_func_get_user_var(POS(), name);
        return false;
      }));
}

Item *PTI_variable_aux_set_var::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<PTI_variable_aux_set_var>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *expr = args[0] ? args[0]->pq_clone(thd, select) : nullptr;
        if (args[0] && !expr) return true;

        new_item = new (thd->pq_mem_root)
            PTI_variable_aux_set_var(POS(), saved_var, expr);
        return false;
      }));
}

Item *PTI_user_variable::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<PTI_user_variable>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) PTI_user_variable(POS(), saved_var);
        return false;
      }));
}

Item *Item_func_connection_id::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_connection_id>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_func_connection_id(POS());
        return false;
      }));
}

bool Item_func_connection_id::pq_copy_from(THD *thd, Query_block *select,
                                           Item *item) {
  return pq_def::pq_copy_from_template<Item_func_connection_id, Item_int_func>(
      this, thd, select, item,
      ([this, thd, select,
        item]([[maybe_unused]] Item_func_connection_id *orig_item) {
        return false;
      }));
}

Item *Item_func_unix_timestamp::pq_clone(THD *thd, Query_block *select) {
  Item *arg_item = nullptr;
  if (arg_count > 0) {
    arg_item = args[0]->pq_clone(thd, select);
    if (!arg_item) return nullptr;
  }

  Item_func_unix_timestamp *new_item = nullptr;
  if (arg_count) {
    new_item = new (thd->pq_mem_root) Item_func_unix_timestamp(POS(), arg_item);
  } else {
    new_item = new (thd->pq_mem_root) Item_func_unix_timestamp(POS());
  }

  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  return new_item;
}

Item *Item_func_benchmark::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_benchmark(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_benchmark>(
      thd, select, this, item_creator);
}

Item *Item_func_found_rows::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_found_rows(POS());
  };
  return pq_def::func_item_clone_template<Item_func_found_rows>(
      thd, select, this, item_creator);
}

Item *Item_func_validate_password_strength::pq_clone(THD *thd,
                                                     Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_validate_password_strength(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_validate_password_strength>(
      thd, select, this, item_creator);
}

Item *Item_func_rand::pq_clone(THD *thd, Query_block *select) {
  Item *cloned_item = nullptr;
  if (arg_count == 0) {
    cloned_item = new (thd->pq_mem_root) Item_func_rand(POS());
  } else if (arg_count == 1) {
    Item *copy_args = args[0]->pq_clone(thd, select);
    if (copy_args == nullptr) return nullptr;

    cloned_item = new (thd->pq_mem_root) Item_func_rand(POS(), copy_args);
  }

  if (!cloned_item || cloned_item->pq_copy_from(thd, select, this))
    return nullptr;

  return cloned_item;
}

Item *Item_func_is_uuid::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_is_uuid(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_is_uuid>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_uuid_short::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd]([[maybe_unused]] Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_uuid_short(POS());
  };
  return pq_def::func_item_clone_template<Item_func_uuid_short>(
      thd, select, this, item_creator);
}

Item *Item_func_floor::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_floor(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_floor>(thd, select, this,
                                                           item_creator);
}

Item *Item_func_regexp_substr::pq_clone(THD *thd, Query_block *select) {
  mem_root_deque<Item *> item_list(thd->pq_mem_root);
  for (uint i = 0; i < arg_count; i++) {
    Item *arg = args[i]->pq_clone(thd, select);
    if (arg == nullptr) return nullptr;
    item_list.push_back(arg);
  }
  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  Item_func_regexp_substr *new_item =
      new (thd->pq_mem_root) Item_func_regexp_substr(POS(), &pt_item_list);

  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  return new_item;
}

Item *Item_func_statement_digest::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_statement_digest(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_statement_digest>(
      thd, select, this, item_creator);
}

bool Item_func_statement_digest::pq_copy_from(THD *thd, Query_block *select,
                                              Item *item) {
  return pq_def::pq_copy_from_template<Item_func_statement_digest,
                                       Item_str_ascii_func>(
      this, thd, select, item,
      ([this, thd, select,
        item]([[maybe_unused]] Item_func_statement_digest *orig_item) {
        m_token_buffer =
            static_cast<uchar *>(thd->alloc(get_max_digest_length()));
        return false;
      }));
}

Item *Item_func_random_bytes::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_random_bytes(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_random_bytes>(
      thd, select, this, item_creator);
}

Item *Item_func_last_day::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_last_day(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_last_day>(thd, select, this,
                                                              item_creator);
}

Item *Item_typecast_year::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_typecast_year(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_typecast_year>(thd, select, this,
                                                              item_creator);
}

Item *PTI_in_sum_expr::pq_clone(THD *thd, Query_block *select) {
  Item *new_expr = expr ? expr->pq_clone(thd, select) : nullptr;
  if (expr && !new_expr) return nullptr;

  PTI_in_sum_expr *new_item =
      new (thd->pq_mem_root) PTI_in_sum_expr(POS(), new_expr);
  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  return new_item;
}

Item *Item_func_trig_cond::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_func_trig_cond>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        Item *arg = args[0]->pq_clone(thd, select);
        if (arg == nullptr) return true;
        assert(trig_var == nullptr);
        new_item = new (thd->pq_mem_root)
            Item_func_trig_cond(arg, nullptr, select->join, m_idx, trig_type);
        return false;
      }));
}

Item *Item_func_radians::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_radians(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_radians>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_degrees::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_degrees(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_degrees>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_sign::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_sign(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_sign>(thd, select, this,
                                                          item_creator);
}

Item *Item_aggregate_ref::pq_clone(THD *thd, Query_block *select) {
  auto source_select = depended_from ? (select->pq_try_clone_item
                                            ? depended_from
                                            : depended_from->pq_last_clone())
                                     : nullptr;

  Item **cloned_ref = (Item **)thd->pq_mem_root->Alloc(sizeof(Item **));
  if (!cloned_ref) return nullptr;
  *cloned_ref = (*m_ref_item)->pq_clone(thd, select);
  if (!(*cloned_ref)) return nullptr;

  Item_aggregate_ref *ref = new (thd->pq_mem_root)
      Item_aggregate_ref(&select->context, cloned_ref, db_name, table_name,
                         field_name, source_select);
  if (!ref || ref->pq_copy_from(thd, select, this)) return nullptr;
  return ref;
}

Item *Item_view_ref::pq_clone(THD *thd, Query_block *select) {
  Name_resolution_context *new_context = &select->context;
  Item **new_ref = (Item **)thd->pq_mem_root->Alloc(sizeof(Item **));
  if (!new_ref) return nullptr;

  if ((*m_ref_item)->type() == Item::REF_ITEM &&
      down_cast<Item_ref *>(*m_ref_item)->ref_type() == Item_ref::REF) {
    Item *real_item = (*m_ref_item)->real_item();
    Item **cloned_ref = nullptr;
    {
      *new_ref = new (thd->pq_mem_root)
          Item_ref(&select->context, db_name, table_name, field_name);
      if (!(*new_ref) || (*new_ref)->pq_copy_from(thd, select, (*m_ref_item))) {
        return nullptr;
      }
      cloned_ref = (Item **)thd->pq_mem_root->Alloc(sizeof(Item **));
      if (!cloned_ref) return nullptr;

      *cloned_ref = real_item->pq_clone(thd, select);
      if (!(*cloned_ref)) return nullptr;
    }

    auto ref_item = down_cast<Item_ref *>(*new_ref);
    ref_item->set_ref_pointer(cloned_ref);
    // set Item_ref's hidden as its referred item.
    ref_item->hidden = real_item->hidden;
  } else {
    *new_ref = (*m_ref_item)->pq_clone(thd, select);
    if (!(*new_ref)) return nullptr;
  }

  Table_ref *inner_table_list =
      first_inner_table
          ? find_table_in_local_list(select->leaf_tables, first_inner_table->db,
                                     first_inner_table->table_name,
                                     first_inner_table->alias)
          : nullptr;

  // cannot find inner table of outer join
  if (first_inner_table && !inner_table_list) return nullptr;

  Item_view_ref *view_ref = new (thd->pq_mem_root)
      Item_view_ref(new_context, new_ref, db_name, table_name,
                    m_orig_table_name, field_name, nullptr, inner_table_list);
  if (!view_ref || view_ref->pq_copy_from(thd, select, this)) {
    return nullptr;
  }

  return view_ref;
}

Item *Item_outer_ref::pq_clone(THD *, Query_block *) {
  /*
    OUTER_REF is listed in array not_supported_type[] (sql_optimizer.cc),
    which is checked after optimization. But cloning for PQ starts earlier
    than that, at the end of preparation, to fill
    Query_block::saved_where_cond. So we can come here. Return nullptr, means
    "unsupported".
  */
  return nullptr;
}

Item *Item_func_gtid_subset::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root)
        Item_func_gtid_subset(POS(), copy_args[0], copy_args[1]);
  };
  return pq_def::func_item_clone_template<Item_func_gtid_subset>(
      thd, select, this, item_creator);
}

Item *Item_func_is_ipv4::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_is_ipv4(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_is_ipv4>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_is_ipv6::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_is_ipv6(POS(), copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_is_ipv6>(thd, select, this,
                                                             item_creator);
}

Item *Item_func_reject_if::pq_clone(THD *thd, Query_block *select) {
  auto item_creator = [this, thd](Item **copy_args) -> Item * {
    return new (thd->pq_mem_root) Item_func_reject_if(copy_args[0]);
  };
  return pq_def::func_item_clone_template<Item_func_reject_if>(
      thd, select, this, item_creator);
}

Item *Item_cond_or::pq_clone(THD *thd, Query_block *select) {
  return pq_def::pq_clone_template<Item_cond_or>(
      thd, select, this, ([this, thd, select](Item *&new_item) {
        new_item = new (thd->pq_mem_root) Item_cond_or();
        return false;
      }));
}

/* Item_cond end */

SubqueryWithResult *SubqueryWithResult::pq_clone(THD *thd, Item_subselect *si) {
  auto subquery =
      new (thd->pq_mem_root) SubqueryWithResult(si->unit, nullptr, si);
  if (!subquery) return nullptr;

  subquery->res_type = res_type;
  subquery->res_field_type = res_field_type;
  subquery->maybe_null = maybe_null;
  return subquery;
}

bool Item_subselect::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_subselect, Item>(
      this, thd, select, item,
      ([this, thd, select, item](Item_subselect *orig_item) {
        // Item_subselect and its children are large classes. When we move to a
        // newer MySQL version, we should check if they got new members, in
        // which case we should consider if these must be copied:
        // HUAWEI_ASSERT_MATCH_CANON item_subselect_Item_subselect

        // The following members are shared by each worker
        unit = orig_item->unit;
        used_tables_cache = orig_item->used_tables_cache;
        m_subquery_used_tables = orig_item->m_subquery_used_tables;

        max_columns = orig_item->max_columns;
        parsing_place = orig_item->parsing_place;
        have_to_be_excluded = orig_item->have_to_be_excluded;
        value_assigned = orig_item->value_assigned;
        traced_before = orig_item->traced_before;
        substitution = orig_item->substitution;
        // 'substitution' is nullptr as we are not inside fix_fields()
        assert(!substitution);
        in_cond_of_tab = orig_item->in_cond_of_tab;
        // The 'subquery' member is not copied here, this is left to children
        // classes, either before or after this function
        indexsubquery_engine = orig_item->indexsubquery_engine;
        // IN(subquery) is not yet supported in PQ, if there was such engine we
        // would need to clone it.
        assert(!indexsubquery_engine);
        changed = orig_item->changed;
        return false;
      }));
}

Item *Item_singlerow_subselect::pq_clone(THD *thd, Query_block *select) {
  // shallow clone it when try to clone where/having condition
  if (select->pq_try_clone_item) return this;

  if (m_pq_last_clone.first == thd) {
    /*
      An Item_subselect may be pointed to by, say, the WHERE clause and a
      QEP_TAB's condition. But it's still one single item, together with one
      single underlying query block. This underlying query block will be
      cloned once only (once for the leader, once per worker). So the item
      must be cloned once only. So if we already cloned it for this THD, we
      return the same clone again.
    */
    return m_pq_last_clone.second;
  }

  // This is not item_maxmin_subselect, because:
  // - if uncorrelated: it's excluded by subquery_suite_for_parallel_query().
  // - if correlated: a subquery is never transformed to such item.
  assert(!is_maxmin());

  auto new_item_base = pq_clone_common(thd, select);

  if (!new_item_base) return nullptr;

  auto *new_item = down_cast<Item_singlerow_subselect *>(new_item_base);

  // HUAWEI_ASSERT_MATCH_CANON item_subselect_Item_singlerow_subselect

  // copy no_rows
  new_item->no_rows = no_rows;

  if (!unit->uncacheable) {
    /*
      In the first-phase parallel execution, we execute the inner subquery and
      the result is stored into "row". In the second-phase parallel execution
      of outer query, each worker just reads these cached value and will never
      change it. Thus, we can directly share the cached value without cloning.
    */
    assert(!new_item->row && !new_item->value);
    new_item->row = thd->pq_mem_root->ArrayAlloc<Item_cache *>(max_columns);
    if (!new_item->row) return nullptr;
    for (uint i = 0; i < unit_cols(); i++) {
      new_item->row[i] = row[i];
    }
    new_item->value = *row;
  } else {
    /*
      As this is correlated, we cannot share the value&row of the source item.
      We need to set up separate value&row. This will be done by a call to
      resolve_type() later. We cannot call it now, as it requires our unit to
      be prepared, which is not yet.
    */
  }
  m_pq_last_clone.first = thd;
  m_pq_last_clone.second = new_item;
  return new_item;
}

bool Item_exists_subselect::pq_copy_from(THD *thd, Query_block *select,
                                         Item *item) {
  return pq_def::pq_copy_from_template<Item_exists_subselect, Item_subselect>(
      this, thd, select, item,
      ([this, thd, select, item](Item_exists_subselect *orig_item) {
        // HUAWEI_ASSERT_MATCH_CANON item_subselect_Item_exists_subselect
        sj_convert_priority = orig_item->sj_convert_priority;
        strategy = orig_item->strategy;
        outer_condition_context = orig_item->outer_condition_context;
        // This pointer is useful only for semi-join conversion, which PQ
        // cloning does not. So we don't need to copy the pointer and can safely
        // use nullptr instead; Good thing, as we don't clone join nests in
        // general.
        embedding_join_nest = nullptr;
        value_transform = orig_item->value_transform;
        implicit_is_op = orig_item->implicit_is_op;
        can_do_aj = orig_item->can_do_aj;
        return false;
      }));
}

bool Item_func_rand::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_rand, Item_real_func>(
      this, thd, select, item,
      ([this, thd, select, item]([[maybe_unused]] Item_func_rand *orig_item) {
        if (arg_count) {
          if (!m_rand &&
              !(m_rand = (struct rand_struct *)thd->stmt_arena->alloc(
                    sizeof(*m_rand))))
            return true;
        } else {
          if (!thd->rand_used) {
            thd->rand_used = true;
            thd->rand_saved_seed1 = thd->rand.seed1;
            thd->rand_saved_seed2 = thd->rand.seed2;
          }
          m_rand = &thd->rand;
        }
        return false;
      }));
}

bool Item_func_numhybrid::pq_copy_from(THD *thd, Query_block *select,
                                       Item *item) {
  return pq_def::pq_copy_from_template<Item_func_numhybrid, Item_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_numhybrid *orig_item) {
        hybrid_type = orig_item->hybrid_type;
        return false;
      }));
}

bool Item_func_dayname::pq_copy_from(THD *thd, Query_block *select,
                                     Item *item) {
  return pq_def::pq_copy_from_template<Item_func_dayname, Item_func_weekday>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_dayname *orig_item) {
        locale = orig_item->locale;
        return false;
      }));
}

bool Item_func_if::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_if, Item_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_if *orig_item) {
        cached_result_type = orig_item->cached_result_type;
        return false;
      }));
}

bool Item_func_from_unixtime::pq_copy_from(THD *thd, Query_block *select,
                                           Item *item) {
  return pq_def::pq_copy_from_template<Item_func_from_unixtime,
                                       Item_datetime_func>(
      this, thd, select, item,
      ([this, thd, select,
        item]([[maybe_unused]] Item_func_from_unixtime *orig_item) {
        thd->time_zone_used = true;
        return false;
      }));
}

bool Item_func_at_time_zone::pq_copy_from(THD *thd, Query_block *select,
                                          Item *item) {
  return pq_def::pq_copy_from_template<Item_func_at_time_zone,
                                       Item_datetime_func>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_at_time_zone *orig_item) {
        m_tz = orig_item->m_tz;
        return false;
      }));
}

bool Item_func_div::pq_copy_from(THD *thd, Query_block *select, Item *item) {
  return pq_def::pq_copy_from_template<Item_func_div, Item_num_op>(
      this, thd, select, item,
      ([this, thd, select, item](Item_func_div *orig_item) {
        m_prec_increment = orig_item->m_prec_increment;
        return false;
      }));
}

Item *Item_exists_subselect::pq_clone(THD *thd, Query_block *select) {
  // shallow clone it when try to clone where/having condition
  if (select->pq_try_clone_item) return this;

  if (m_pq_last_clone.first == thd) return m_pq_last_clone.second;

  auto new_item = pq_clone_common(thd, select);

  if (!new_item) return nullptr;

  m_pq_last_clone.first = thd;
  m_pq_last_clone.second = new_item;

  return new_item;
}

bool Arg_comparator::clone(Arg_comparator *origin_cmp,
                           Item_result_field *result) {
  if (!origin_cmp) return false;
  func = origin_cmp->func;
  get_value_a_func = origin_cmp->get_value_a_func;
  get_value_b_func = origin_cmp->get_value_b_func;
  comparator_count = origin_cmp->comparator_count;
  precision = origin_cmp->precision;
  set_null = origin_cmp->set_null;
  cmp_collation = origin_cmp->cmp_collation;
  m_compare_type = origin_cmp->m_compare_type;
  owner = result;
  if (origin_cmp->comparators) {
    assert(origin_cmp->comparator_count);
    if (!(comparators = new (*THR_MALLOC) Arg_comparator[comparator_count]) ||
        DBUG_EVALUATE_IF("arg_comparator_clone_error", true, false))
      return true;
    for (int i = 0; i < comparator_count; i++) {
      comparators[i].left = (*left)->addr(i);
      comparators[i].right = (*right)->addr(i);
      comparators[i].clone(&origin_cmp->comparators[i], result);
    }
  }

  assert(left || right);
  // This place can not use pq_clone function to set left_cache/right_cache
  // because in Item_cache::pq_copy_from, it will copy example and refix_fields
  // it. left_cache/right_cache's example is *left/*right and it does not need
  // to regenerate a new example.
  bool cmp_as_dates = can_compare_as_dates(*left, *right);
  auto set_cache_arg = [&](Item **opt_arg, Item **arg_cache, Item *orig_cache) {
    if (cmp_as_dates) {
      auto *cache = new Item_cache_datetime(MYSQL_TYPE_DATETIME);
      cache->store_value(static_cast<Item_cache_datetime *>(orig_cache));
      cache->set_used_tables(1);
      *arg_cache = cache;
      return arg_cache;
    } else {
      THD *thd = current_thd;
      return cache_converted_constant(thd, opt_arg, arg_cache, m_compare_type);
    }
  };

  if (origin_cmp->left_cache)
    left = set_cache_arg(left, &left_cache, origin_cmp->left_cache);
  if (origin_cmp->right_cache)
    right = set_cache_arg(right, &right_cache, origin_cmp->right_cache);
  return false;
}
#endif
