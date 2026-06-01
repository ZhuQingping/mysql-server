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

#include <algorithm>
#include "sql/error_handler.h"
#include "sql/item.h"
#include "sql/item_func.h"
#include "sql/item_json_func.h"
#include "sql/item_row.h"
#include "sql/item_subselect.h"
#include "sql/item_sum.h"
#include "sql/sql_base.h"
#include "sql/sql_lex.h"
#include "sql/thd_raii.h"
using std::max;

static inline bool never_be_called() {
  assert(false);
  return true;
}

bool Item::refix_fields(THD *, Item **) {
  assert(is_contextualized());

  // We do not check fields which are fixed during construction
  assert(fixed == 0 || basic_const_item());
  fixed = true;
  return false;
}

void Item_field::set_field_for_pq(Field *field_par) {
  table_ref = field_par->table->pos_in_table_list;
  assert(table_ref == nullptr || table_ref->table == field_par->table);
  assert(field_par->field_index() != NO_FIELD_INDEX);
  field_index = field_par->field_index();

  field = result_field = field_par;  // for easy coding with fields
  field_name = field_par->field_name;
  if (!can_use_prefix_key)
    field->table->covering_keys.subtract(field->part_of_prefixkey);

  if (table_ref != nullptr) {
    m_orig_db_name = table_ref->db;
    m_orig_table_name = table_ref->table_name;
  } else {
    m_orig_db_name = field_par->orig_db_name;
    m_orig_table_name = field_par->orig_table_name;
  }
  fixed = true;
}

bool Item_sp_variable::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_name_const::refix_fields(THD *thd MY_ATTRIBUTE((unused)),
                                   Item **ref MY_ATTRIBUTE((unused))) {
  if (value_item->refix_fields(thd, &value_item) ||
      name_item->refix_fields(thd, &name_item) ||
      DBUG_EVALUATE_IF("name_const_refix_error", true, false)) {
    my_error(ER_RESERVED_SYNTAX, MYF(0), "NAME_CONST");
    return true;
  }
  fixed = true;
  return false;
}

bool Item_field::refix_fields(THD *thd, Item **reference) {
  assert(fixed == 0);
  Field *from_field = not_found_field;
  bool outer_fixed = false;

  Internal_error_handler_holder<View_error_handler, Table_ref> view_handler(
      thd, context->view_error_handler, context->view_error_handler_arg);

  if (!field) {
    from_field = find_field_in_tables(
        thd, this, context->first_name_resolution_table,
        context->last_name_resolution_table, reference,
        thd->lex->use_only_table_context ? REPORT_ALL_ERRORS
                                         : IGNORE_EXCEPT_NON_UNIQUE,
        any_privileges ? 0 : thd->want_privilege, true);
    if (thd->is_error()) goto error;
    if (from_field == not_found_field) {
      int ret;
      /* Look up in current select's item_list to find aliased fields */
      if (thd->lex->current_query_block()->is_item_list_lookup &&
          pq_can_resolve_Item_field_in(this, thd->lex->current_query_block())) {
        uint counter;
        enum_resolution_type resolution;
        Item **res;
        if (find_item_in_list(thd, this,
                              &thd->lex->current_query_block()->fields, &res,
                              &counter, &resolution))
          return true;
        if (resolution == RESOLVED_AGAINST_ALIAS) set_alias_of_expr();
        if (res != nullptr) {
          assert((*res)->type() == Item::FIELD_ITEM);
          /*
            It's an Item_field referencing another Item_field in the select
            list.
            Use the field from the Item_field in the select list and leave
            the Item_field instance in place.
          */
          Item_field *const item_field = (Item_field *)(*res);
          Field *const new_field = item_field->field;
          if (new_field == nullptr) {
            /* The column to which we link isn't valid. */
            my_error(ER_BAD_FIELD_ERROR, MYF(0), item_field->item_name.ptr(),
                     thd->where);
            return true;
          }

          set_field_for_pq(new_field);
          cached_table = table_ref;
          // The found column may be an outer reference
          if (item_field->depended_from)
            mark_as_dependent(thd, item_field->depended_from,
                              context->query_block, this, this);
          return false;
        }
      }
      if ((ret = fix_outer_field(thd, &from_field, reference)) < 0) goto error;
      outer_fixed = true;
      if (!ret) return false;
    } else if (!from_field)
      goto error;

    /*
      We should resolve this as an outer field reference if
      1. we haven't done it before, and
      2. the query_block of the table that contains this field is
         different from the query_block of the current name resolution
         context.
     */
    if (!outer_fixed &&  // 1
        cached_table && cached_table->query_block &&
        context->query_block &&  // 2
        cached_table->query_block != context->query_block) {
      int ret;
      if ((ret = fix_outer_field(thd, &from_field, reference)) < 0) goto error;
      outer_fixed = true;
      if (!ret) return false;
    }
    if (thd->lex->in_sum_func &&
        thd->lex->in_sum_func->base_query_block->nest_level ==
            context->query_block->nest_level)
      thd->lex->in_sum_func->max_aggr_level =
          max(thd->lex->in_sum_func->max_aggr_level,
              int8(context->query_block->nest_level));
    // Not view reference, not outer reference; need to set properties:
    set_field_for_pq(from_field);
  } else if (thd->mark_used_columns != MARK_COLUMNS_NONE) {
    TABLE *table = field->table;
    MY_BITMAP *current_bitmap;
    MY_BITMAP *other_bitmap MY_ATTRIBUTE((unused));
    if (thd->mark_used_columns == MARK_COLUMNS_READ) {
      current_bitmap = table->read_set;
      other_bitmap = table->write_set;
      bitmap_set_bit(&table->read_set_internal, field->field_index());
    } else {
      current_bitmap = table->write_set;
      other_bitmap = table->read_set;
    }
    if (!bitmap_test_and_set(current_bitmap, field->field_index()))
      assert(bitmap_is_set(other_bitmap, field->field_index()));
  }
  fixed = true;
  return false;

error:
  return true;
}

bool Item_asterisk::refix_fields(THD *, Item **) { return never_be_called(); }

bool Item_ref::refix_fields(THD *thd, Item **) {
  assert(fixed == 0);
  assert(m_ref_item && (m_ref_item != nullptr));
  if (!(*m_ref_item)->fixed && (*m_ref_item)->refix_fields(thd, m_ref_item))
    return true;
  fixed = true;
  return false;
}

bool Item_view_ref::refix_fields(THD *thd, Item **reference) {
  assert(ref_item() != nullptr);  // view field reference must be defined
  if (ref_item()->fixed) {
    /*
      Underlying Item_field objects may be shared. Make sure that the use
      is marked regardless of how many ref items that point to this field.
    */
    Mark_field mf(thd->mark_used_columns);
    ref_item()->walk(&Item::mark_field_in_map, enum_walk::POSTFIX,
                     pointer_cast<uchar *>(&mf));
  } else {
    if (ref_item()->refix_fields(thd, reference)) {
      return true; /* purecov: inspected */
    }
  }
  if (super::refix_fields(thd, reference)) return true;

  if (cached_table && cached_table->is_inner_table_of_outer_join()) {
    first_inner_table = cached_table->any_outer_leaf_table();
  }
  return false;
}

bool Item_outer_ref::refix_fields(THD *, Item **) { return never_be_called(); }

bool Item_default_value::refix_fields(THD *thd, Item **) {
  assert(!fixed);

  Internal_error_handler_holder<View_error_handler, Table_ref> view_handler(
      thd, context->view_error_handler, context->view_error_handler_arg);
  if (arg == nullptr) {
    fixed = true;
    return false;
  }
  if (!arg->fixed && arg->refix_fields(thd, &arg)) return true;

  Item_field *const field_arg = down_cast<Item_field *>(arg->real_item());
  Field *const def_field = field_arg->field->clone(thd->mem_root);
  if (def_field == nullptr) return true;

  def_field->move_field_offset(def_field->table->default_values_offset());
  m_rowbuffer_saved = def_field->table->s->default_values;

  set_field_for_pq(def_field);
  cached_table = table_ref;
  return false;
}

bool Item_insert_value::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_trigger_field::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_row::refix_fields(THD *thd, Item **) {
  Item **arg, **arg_end;
  for (arg = items, arg_end = items + arg_count; arg != arg_end; arg++) {
    if ((!(*arg)->fixed && (*arg)->refix_fields(thd, arg))) return true;
  }
  fixed = true;
  return false;
}

bool Item_subselect::refix_fields(THD *thd, Item **) {
  if (!unit->outer_query_block()->parallel_exec) {
    assert(false);
    return true;
  }
  if (unit->uncacheable) {
    // resolve_type can not be removed, it will store value to rows in
    // resolve_type.
    auto rc = resolve_type(thd);
    assert(!rc);
    if (rc) return true;
  }
  fixed = true;
  return false;
}

bool Item_in_subselect::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_func::refix_fields(THD *thd, Item **) {
  if (arg_count) {
    Item **arg, **arg_end;
    for (arg = args, arg_end = args + arg_count; arg != arg_end; arg++) {
      if ((!(*arg)->fixed && (*arg)->refix_fields(thd, arg))) return true;
      // m_accum_properties must be reset because it would be reset to initial
      // value and the values in pq clone is not correct value.
      // See Issue1 in refactor_fix_fields.test
      add_accum_properties(*arg);
    }
  }
  fixed = true;
  return false;
}

bool Item_func_min_max::refix_fields(THD *thd, Item **ref) {
  if (Item_func_numhybrid::refix_fields(thd, ref)) return true;

  for (uint i = 0; i < arg_count; i++) {
    if (args[i]->is_temporal()) {
      /*
        Bug #33996054: Result mismatch with input column re-order for
        GREATEST(). The community bug fix modified the function
        Item_func_min_max::resolve_type_inner, and we need to synchronize the
        changes.
      */
      if (!temporal_item || (temporal_rank(args[i]->data_type()) >
                             temporal_rank(temporal_item->data_type())))
        temporal_item = args[i];
    }
  }
  fixed = true;
  return false;
}

bool Item_udf_func::refix_fields(THD *, Item **) { return never_be_called(); }

bool Item_func_set_user_var::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_func_json_schema_valid::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_func_bool_const::refix_fields(THD *, Item **) { return false; }

bool Item_in_optimizer::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_bool_func2::refix_fields(THD *thd,
                                   Item **ref MY_ATTRIBUTE((unused))) {
  if (arg_count) {
    Item **arg, **arg_end;
    for (arg = args, arg_end = args + arg_count; arg != arg_end; arg++) {
      if ((!(*arg)->fixed && (*arg)->refix_fields(thd, arg))) return true;
      if (!allowed_arg_cols) allowed_arg_cols = (*arg)->cols();
    }
  }
  // In Arg_comparator::clone, it will set left or right of comparators[i] with
  // (*left)->addr(i) or (*right)->addr(i). And in
  // Item_signlerow_subselect::addr, row will be set in resolve_type().
  // So, cmp.clone must be called after all args are fixed and not in pq clone.
  // Test case can see Issue2 in refactor_fix_fields.test.
  if (cloned_origin_item) {
    Item_bool_func2 *orig_func2 =
        static_cast<Item_bool_func2 *>(cloned_origin_item);
    if (cmp.clone(&orig_func2->cmp, this)) return true;
  } else {
    if (thd->lex->sql_command != SQLCOM_SHOW_CREATE) set_cmp_func();
  }
  fixed = true;
  return false;
}

bool Item_func_between::refix_fields(THD *thd, Item **ref) {
  if (Item_func_opt_neg::refix_fields(thd, ref)) return true;
  thd->lex->current_query_block()->between_count++;
  fixed = true;
  return false;
}

bool Item_func_in::refix_fields(THD *thd, Item **ref) {
  if (Item_func_opt_neg::refix_fields(thd, ref)) return true;
  // fill need that args has been fixed.
  // array rename to m_const_array
  // values_are_const rename to m_values_are_const
  if (m_const_array && m_values_are_const) {
    if (m_const_array) {
      Item_result cmp_type = STRING_RESULT;
      uint found_types = collect_cmp_types(args, arg_count, true);
      if (found_types == 0) return true;
      uint type_cnt = 0;
      for (uint i = 0; i <= (uint)DECIMAL_RESULT; i++) {
        if (found_types & (1U << i)) {
          (type_cnt)++;
          cmp_type = (Item_result)i;
        }
      }
      if (cmp_type == ROW_RESULT) {
        if (down_cast<in_row *>(m_const_array)
                ->allocate(thd->mem_root, args[0], arg_count - 1)) {
          return true;
        }
      }
    }
    have_null = m_const_array->fill(args + 1, arg_count - 1);
    m_populated = true;
  }
  fixed = true;
  return false;
}

bool Item_func_like::refix_fields(THD *thd, Item **ref) {
  assert(fixed == 0);

  args[0]->real_item()->set_can_use_prefix_key();

  if (Item_bool_func2::refix_fields(thd, ref) ||
      DBUG_EVALUATE_IF("item_func_like_refix_error", true, false))
    goto err;

  if (!escape_was_used_in_parsing() || args[2]->const_item()) {
    escape_is_const = true;
    if (!(thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW)) {
      assert(escape_evaluated);
      if (check_covering_prefix_keys(thd)) return true;
    }
  }
  return false;

err:
  fixed = false;
  return true;
}

bool Item_cond::refix_fields(THD *thd, Item **) {
  assert(fixed == 0);
  List_iterator<Item> li(list);
  Item *item;
  Query_block *select = thd->lex->current_query_block();
  auto func_type = functype();
  while ((item = li++)) {
    Item_cond *cond;
    while (item->type() == Item::COND_ITEM &&
           (cond = down_cast<Item_cond *>(item)) &&
           cond->functype() == func_type &&
           !cond->list.is_empty()) {  // Identical function
      li.replace(cond->list);
      cond->list.clear();
      item = *li.ref();  // new current item
    }
    if (ignore_unknown()) item->apply_is_true();
    if ((!item->fixed && item->refix_fields(thd, li.ref())) ||
        (item = *li.ref())->check_cols(1) ||
        DBUG_EVALUATE_IF("item_cond_refix_fields_error", true, false))
      return true; /* purecov: inspected */
  }
  select->cond_count += list.elements;
  fixed = true;
  return false;
}

bool Item_equal::refix_fields(THD *thd, Item **) {
  List_iterator_fast<Item_field> li(fields);
  Item *item;
  while ((item = li++)) {
    if (!item->fixed && item->refix_fields(thd, &item)) return true;
  }
  if (resolve_type(thd)) return true;
  fixed = true;
  return false;
}

bool Item_func_make_set::refix_fields(THD *thd, Item **ref) {
  assert(fixed == 0);
  bool res = ((!item->fixed && item->refix_fields(thd, &item)) ||
              item->check_cols(1) || Item_func::refix_fields(thd, ref));
  return res;
}

bool Item_func_weight_string::refix_fields(THD *, Item **) { return false; }

bool Item_sum::refix_fields(THD *, Item **) {
  assert(fixed == 0);
  // PQ does not support window functions and m_window is nullptr.
  assert(!m_window || m_window_resolved);
  return false;
}

bool Item_sum_num::refix_fields(THD *thd, Item **ref) {
  if (super::refix_fields(thd, ref)) return true; /* purecov: inspected */
  if (init_sum_func_check(thd)) return true;
  Query_block *curr_select = thd->lex->current_query_block();
  Condition_context CCT(curr_select);

  for (uint i = 0; i < arg_count; i++) {
    if (!args[i]->fixed && args[i]->refix_fields(thd, args + i)) return true;
  }

  if (check_sum_func(thd, ref)) return true;
  fixed = true;
  return false;
}

bool Item_sum_json::refix_fields(THD *, Item **) { return never_be_called(); }

bool Item_sum_avg::refix_fields(THD *thd, Item **ref) {
  // resolve_type can not be removed because max_length/item_field's precision
  // and scale are different in PQ_REBUILD and PQ_WORKER/PQ_LEADER.
  if (super::refix_fields(thd, ref) || resolve_type(thd)) return true;
  fixed = true;
  return false;
}

bool Item_sum_hybrid::refix_fields(THD *thd, Item **ref) {
  if (super::refix_fields(thd, ref)) return true; /* purecov: inspected */

  Item *item = args[0];
  if (init_sum_func_check(thd)) return true;
  Condition_context CCT(thd->lex->current_query_block());

  if (!item->fixed && item->refix_fields(thd, args)) return true;

  if (setup_hybrid(args[0], nullptr) || check_sum_func(thd, ref)) return true;
  fixed = true;
  return false;
}

bool Item_func_group_concat::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_non_framing_wf::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_first_last_value::refix_fields(THD *, Item **) {
  return never_be_called();
}

bool Item_nth_value::refix_fields(THD *, Item **) { return never_be_called(); }
