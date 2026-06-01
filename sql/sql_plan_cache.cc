/* Copyright (c) 2022, Huawei and/or its affiliates. All rights reserved.

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

#include "sql/sql_plan_cache.h"
#include "sql/auth/auth_acls.h"
#include "sql/item_timefunc.h"  // Item_typecast
// For this file, there is a special -I instruction to the compiler in
// sql/CMakeLists.txt
#include "scope_guard.h"  // create_scope_guard
#include "securec.h"      // memcpy_s
#include "sql/join_optimizer/access_path.h"
#include "sql/opt_trace.h"  // Opt_trace_object
#include "sql/parallel_query/pq_clone.h"
#include "sql/range_optimizer/path_helpers.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_parse.h"
#include "sql/sql_resolver.h"
#include "sql/sql_select.h"
#include "sql/temp_table_param.h"
#include "sql/thd_raii.h"
#include "sql/window.h"

/*
  PLAN_CACHE_PORT 2025-05-xx

  This back-port used Taurus branch huawei/cbu/gaussdbformysql/develop as
  reference, at commit: f889b2664d6f807a0b4a798ca2a25b5e86efd700
  BUG2024030504391 Delete mysql and duckdb from the Hermes code library.
  Author: yangheng 00892479 <yangheng6@huawei.com>
  Date:   Thu May 8 19:10:59 2025 +0800

  Files sql/sql_plan_cache.* (at this revision) have been copied;
  the same for mysql-test/{t,r}/session_plan_cache.*. As to other files -
  those which plan cache (PC) didn't introduce but only modify (like
  sql_select.cc), the necessary changes were collected like this: grep in "git
  log" for the word "cache", spot all commits mentioning PC, study the
  modifications and port them.

  Adaptation was needed, due to changes in upstream MySQL from 8.0.22 to
  8.0.41:

  - some variables' names changed: like s/select_lex/query_block.

  - the range optimizer's main type, QUICK_SELECT, became an AccessPath.

  Then the main faced problem was that the PC code makes heavy use of the
  parallel query (PQ) code: it uses functions pq_clone() and pq_copy() to
  clone (a) QEP_TAB, Table_ref, etc, (b) Item-s. While (a) is rather isolated
  code (a few functions), (b) is more than a hundred functions (one per Item
  subclass), and brings special complexity wrt cloning of the various Item_ref
  subclasses.

  So (a) was ported and (b) was replaced with another solution, based on the
  observation that in a prepared statement, Items are persistent (re-used
  in each execution) except those which are created by the Optimizer, but
  those are of a restricted set of types. Thus we only need to clone this
  last, small group.

  When PQ is ported later, this design could be reconsidered.

  There are porting-related comments, all marked with "PLAN_CACHE_PORT".

  In order to help the reviewer :

  - all functions used solely by PC, scattered in other files (e.g.
  QEP_TAB::replace_cache_key()), have been put altogether in the present file,
  which has been saved to porting_notes/sql/plan_cache/ before any adaptation
  was done. The header file has been saved there too. These form the "raw"
  port which didn't even compile.

  - So the reviewer can just run a GUI diff tool on
  porting_notes/sql/plan_cache/sql_plan_cache* vs sql/sql_plan_cache.*
  , to see all adaptations done in the porting work. The other files (e.g.
  sql_select.cc) received no non-trivial adaptation, except in a few places
  marked with "PLAN_CACHE_PORT".

  - As to the mtr test, it has been adapted and extended with more queries.
  The reviewer could diff it (test and result) with the one in the Taurus
  branch. Note that the test file was moved into an '.inc' file, so it is run
  with InnoDB and DStore.

  Due to adaptations to the cloning code, in certain functions an argument is
  not used anymore.".
*/

/*
  Plan cache works as following process:
    Prepared_statement::execute_loop
       Prepared_statement::execute
       ...
       make_join_readinfo
       if (cache_plan)
         Query_block::cached_plan points to cached plan
       cached plan start its life

    =======================================
       Next Prepared_statement::execute
       ...
       exec_cached_plan
         if apply_cached_plan_if_suitable says it cannot
           if (cached plan needs to be invalidated)
             invalidate_cached_plan
             if (cached plan has been hit)
               executor has freed parts of cached plan (JOIN::destroy()) when
               previous execution ended, do nothing
             else
               call JOIN::destroy() on the cached plan
             call destructor on cached plan
         else
           apply_cached_plan (rebuild freed parts above)
             Query_block::join == Query_block::cached_plan
       execute cached plan
       Query_block::cleanup
         Query_block::join->destroy;
         if (using cached plan)
           reset cached plan status
         else
           call destructor on Query_block::join

    =======================================
       Life end time for cached plan
       Prepared_statement::~Prepared_statement
         Lex::destroy
          ...
          JOIN::~JOIN
          cached plan ends life

  Notes (PLAN_CACHE_PORT this is just precisions, not something changed by the
  porting):

  - when the optimizer thinks a plan cannot be cached, it marks it as
  UNCACHEABLE and this is a final decision which will apply to all future
  optimizations of it, even with different values of PS parameters: no plan
  will be cached. This is probably to avoid possibly-useless
  can-this-be-cached tests in next optimizations.

  - when the optimizer has a cached plan from before but fails to use it for a
  new execution, it invalidates this plan, makes a new optimization, and
  possibly makes a new cached plan.

  New features in the porting:

  - the cached plan is allocated in a dedicated MEM_ROOT, to stop memory
  growth in the permanent MEM_ROOT (that was an old problem).

  - When caching a plan (cache_plan()) we do not allocate and open a
  Table_ref anymore; this object was not much used (probably it was there
  because PQ functions needed it?), f.ex. when the plan was later used the
  JOIN's tables_list was (of course) re-pointed to the instance used by _that_
  execution. And it was using memory, also in the engine (as we did
  open_table()); it caused most of the memory usage of plan cache in simple
  queries.

  - We added a count of plan cache invalidations in SHOW GLOBAL STATUS.

  - subqueries are supported, in a very restricted scenario (which covers what
  is used in TPC-C): it has to be a non-correlated scalar subquery, plus some
  other restrictions.
*/

// PLAN_CACHE_PORT in .41 this is not exported by sql_select.h anymore, so:
store_key *get_store_key(THD *thd, Item *val, table_map used_tables,
                         table_map const_tables, const KEY_PART_INFO *key_part,
                         uchar *key_buff, uint maybe_null);

/// Minion for clone_if_transient(), to reduce code duplication when we make
/// clones of Item_func-s which have similar layout (example: equal,
/// greater-than, ...).
template <typename T>
T *make_cmp_op(THD *thd, Item_func *op) {
  assert(op->argument_count() <= 3);
  Item *args[3];
  for (uint i = 0; i < op->argument_count(); ++i) {
    args[i] = clone_if_transient(thd, op->get_arg(i));
    if (!args[i]) return nullptr;
  }
  if constexpr (std::is_base_of_v<Item_func_comparison, T>) {
    assert(op->argument_count() == 2);
    return new (thd->mem_root) T(args[0], args[1]);
  } else if constexpr (std::is_same_v<Item_func_if, T>) {
    assert(op->argument_count() == 3);
    return new (thd->mem_root) T(args[0], args[1], args[2]);
  } else {
    assert(op->argument_count() == 1);
    return new (thd->mem_root) T(args[0]);
  }
}

/*
  PLAN_CACHE_PORT

  Here an attempt to make a simple Item-cloning function capable to handle
  single-table queries. In order to not bring in the complexity of PQ Item
  cloning. This is specific of the new implementation.

  Why does plan cache need to clone Items?

  Because the Optimizer modifies WHERE/HAVING conditions (and builds more: ICP
  condition, per-table condition) (modifications can be: equality propagation,
  trivial condition removal, change of expressions to generated columns, etc),
  and these modified conditions are intentionally lost after execution (as a
  next execution is supposed to re-do the optimization). As the goal of plan
  cache is to not re-do an optimization, it needs to save the modified,
  optimized conditions before they disappear. Hence, the need to clone them.

  Some background about Item's life when there is a prepared statement (to be
  used like this: PREPARE/EXECUTE/EXECUTE/etc, with EXECUTE re-doing a fresh
  optimization each time): Items which already exist when optimization starts,
  which are post-resolution, live in the statement's (permanent) MEM_ROOT, so
  are long-lived ; items created during optimization live in the execution
  (transient) MEM_ROOT. The latter MEM_ROOT is freed at the end of one
  execution, so they are short-lived. When an optimization starts, it starts
  on the long-lived items. It duplicates any AND/OR node into short-lived
  items (see copy_andor_structure()). For example, "a=1 AND a=a" becomes "a=1
  NEW_AND a=a" (the '=' are still the original ones, NEW_AND is a new AND i.e.
  a clone). The advantage of this is that, when the optimizer then sees that
  "a=a" is always true, it can just disconnect it from NEW_AND by editing the
  argument list of NEW_AND. At end of execution, the WHERE clause can simply
  be restored to be the original AND, which still has its two components. But
  this is not the only mechanism: starting with "a=1 AND b>a", then "a=1
  NEW_AND b>a", the optimizer makes "a=1 NEW_AND b>1": here we have a
  modification done to the argument list of the long-lived item ">". To make
  sure this is undone at the end of execution, the change "a -> 1" in ">" is
  recorded (change_item_tree()) when it's done, and undone with
  rollback_item_tree_changes().

  Summary: changes to short-lived items can be done without precaution,
  changes to long-lived items use change_item_tree().

  How to avoid using the PQ cloning code?

  The idea is that the optimizer phase up to until we cache the plan (i.e.
  JOIN::optimize() up to after calling make_join_readinfo()) creates only few
  types of Item-s [1]; so if we meet those types we must clone them if they
  are short-lived (allocated in the execution MEM_ROOT), OTOH if we meet other
  types they must be existing from before, and so they are in a persistent,
  permanent MEM_ROOT, and so we can just re-use them without cloning (contrast
  this with PQ, where multiple workers may want to evaluate the Item at the
  same time and on different input rows, so really need to duplicate it for
  thread safety).

  So far we say:
  - long-lived item: return unchanged.
  - short-lived item: return a clone.

  Abbreviations for the rest: long-lived = LL, short-lived = SL.

  But what about Items with arguments: if the LL item contains (as
  argument) a SL item (call this "situation SL-in-LL"), or the
  SL item contains a LL item ("LL-in-SL") ?

  LL-in-SL is no problem: clone the SL, give the clone the LL item as
  argument. Example: "a=1 NEW_AND b=a", changed to "NEW=(1,a,b)" then to "a
  NEW= 1 NEW_AND b NEW= 1" where both "=" are SL and "1" is LL (cf
  eliminate_item_equal()).

  Now to SL-in-LL: in this case the change of argument has been tracked by a
  call to change_item_tree(). Example: "a=1 NEW_AND FUNC(a)>2". In FUNC, "a"
  is replaced with "1". "1" is sometimes just a pointer to the "1" in "a=1"
  (LL), sometimes a copy (SL) (see Item_int::clone_item()), and in the latter
  case we have SL-in-LL.

  This case poses a hard problem: if, when making the plan cache, we simply
  store the LL FUNC in our "copy of the Item tree for later", later when we
  want to use this copy in the next execution, the replacement "a-to-1" has
  been undone by rollback_item_tree_changes() meanwhile, and we are back to
  the un-optimized, less efficient "FUNC(a)>2". OTOH if we rather decide to
  store FUNC(1) in the plan cache (efficient), how do we store it so that it
  is not rolled back to FUNC(a)? We cannot, unless we clone FUNC (the clone
  will not be affected by rollback), but then it requires us to implement
  cloning for ALL SQL functions (not only those created by the optimizer -
  FUNC could be anything).

  We cannot simply ban such case from plan cache, as it happens in simple
  queries like:
  - "a=1 AND b>a", changed to "a=1 NEW_AND b> NEW1" (cf
  substitute_for_best_equal_field()) (using '>' here, not '=' which triggers
  another quite different code path),
  - "time_col IN ('2002-02-02 01:01:01',...)", changed to
  "time_col IN (NEW<cache>('2002-02-02 01:01:01'),...)" (the <cache>
  represents a one-time-calculated temporal value ; cf
  cache_const_expr_transformer()).

  The chosen solution is as follows.

  First, when caching the plan (cf cache_plan()).

  (A) We create a map of all SL items (scanning THD::item_list), which
  maps the address to nullptr (for now).

  (B) We scan the changes registered by change_item_tree() (which are not yet
  rolled back), and we build a list of all such changes if they modify a LL
  item ; if the new value is a SL item then we clone such SL item, so the list
  contains pairs of type {LL place to change, LL item or clone of SL item to
  put in the place}.

  When cloning a SL item, the address of the clone is put as second value in
  the map (replacing nullptr).

  Then when a call to clone_if_transient() is made, check the map: if found,
  then the item is SL, return the address of its clone, or if nullptr (= not yet
  cloned), make a clone and return it (and update the map). If not found, then
  it is LL, return it.

  Next, when applying the cached plan.

  We get conditions saved in the previous phase (no cloning happens, anymore).
  For the case of "a=1 AND b>a", it means we get "a=1 CLONED_AND b>a". But we
  also replay changes which we had recorded in a list in (B), which gives us
  "a=1 CLONED_AND b>CLONED_1", as desired. The replaying is tracked with
  change_item_tree(), so it is undone when execution ends (due to the usual
  mechanism of PS).

  If we give up on applying this cached plan due to errors, we rollback the
  changes above, with an explicit call to rollback_item_tree_changes(). Then
  we fall through a fresh new optimization.

  [1] which are:
  - AND, OR
  - FIELD (only for substitution of expression with a generated column?)
  - CACHE (to replace 1+2 with <cache>(1+2), addition done only once)
  - TRIG_COND (only for left join, so with >=2 tables, so can be ignored for
  - plan cache)
  - TRUE,FALSE (for always-true/false conditions)
  - Constants (made by Item::clone_item() implemented only for Item_int and the
  like)
  - CAST (from cast_incompatible_args())
  - EQUAL (from conversion of multiple-equality to plain equality)
  - IS NOT NULL (for NULL filtering) (only for >=2 tables, so can be ignored for
  plan cache).
  - sql_const_folding.cc adds those:
  - Constants which do not have clone_item(): Item_{date,time,datetime}_literal;
  - tests IS NULL, IS NOT NULL, >, <, >=, <=, <>;
  - IF.

  The list above has been compiled by code inspection (which proved to be
  insufficient) and by running the main mtr suite on {vanilla 8.0.41 with this
  port of plan cache} with plan cache globally enabled (it is impossible to
  run the main mtr suite on the mysql+dstore branch currently), and by running
  it again with --ps-protocol which makes much more use of prepared
  statements, thus exercises plan cache a lot. Any non-handled item led to an
  assertion failure at the end of clone_if_transient(). That revealed the case
  of Items made by sql_const_folding.cc.
*/
Item *clone_if_transient(THD *thd, Item *i) {
  // transient = short-lived. vs permanent = long-lived.
  auto &transient_items =
      thd->lex->current_query_block()
          ->cached_plan->plan_cache_exec_context->transient_items;

  auto transient = transient_items.find(i);
  if (transient == transient_items.end()) {
    // A permanent Item, will continue to exist in next executions, so just use
    // it.
    return i;
  }

  // 'c' stands for " (C)lone of 'i' ".
  auto c = transient->second;
  // If a clone already exists, avoid double cloning, use it. Can happen if we
  // clone where_cond() then the QEP_TAB's condition then the ICP condition
  // (they may all be equal!).
  if (c != nullptr) return c;

  // Make a clone. Code depends on the type.
  switch (i->type()) {
    case Item::FUNC_ITEM:
    case Item::COND_ITEM: {
      auto i_f = down_cast<Item_func *>(i);
      auto f_t = i_f->functype();
      switch (f_t) {
        case Item_func::EQ_FUNC: {
          c = make_cmp_op<Item_func_eq>(thd, i_f);
          // need a call to update_used_tables() for proper creation of Filter
          // access paths containing this item; fix_fields() will do that below.
          break;
        }
        case Item_func::LT_FUNC: {
          c = make_cmp_op<Item_func_lt>(thd, i_f);
          break;
        }
        case Item_func::GT_FUNC: {
          c = make_cmp_op<Item_func_gt>(thd, i_f);
          break;
        }
        case Item_func::LE_FUNC: {
          c = make_cmp_op<Item_func_le>(thd, i_f);
          break;
        }
        case Item_func::GE_FUNC: {
          c = make_cmp_op<Item_func_ge>(thd, i_f);
          break;
        }
        case Item_func::NE_FUNC: {
          c = make_cmp_op<Item_func_ne>(thd, i_f);
          break;
        }
        case Item_func::ISNULL_FUNC: {
          c = make_cmp_op<Item_func_isnull>(thd, i_f);
          break;
        }
        case Item_func::ISNOTNULL_FUNC: {
          c = make_cmp_op<Item_func_isnotnull>(thd, i_f);
          break;
        }
        case Item_func::IF_FUNC: {
          c = make_cmp_op<Item_func_if>(thd, i_f);
          break;
        }
        case Item_func::TRUE_FUNC:
          c = new Item_func_true();
          break;
        case Item_func::FALSE_FUNC:
          c = new Item_func_false();
          break;
        case Item_func::COND_AND_FUNC: {
          // inspired by copy_andor_structure()
          auto i_cond = down_cast<Item_cond_and *>(i);
          auto c_cond = new Item_cond_and(thd, i_cond);
          if (c_cond == nullptr) return nullptr;
          for (auto &it : *i_cond->argument_list()) {
            assert(it.real_item());  // Sanity check (no dangling 'ref')
            auto arg = clone_if_transient(thd, &it);
            if (arg == nullptr) return nullptr;
            c_cond->argument_list()->push_back(arg);
          }
          c = c_cond;
          break;
        }
        case Item_func::COND_OR_FUNC: {
          auto i_cond = down_cast<Item_cond_or *>(i);
          auto c_cond = new Item_cond_or(thd, i_cond);
          if (c_cond == nullptr) return nullptr;
          for (auto &it : *i_cond->argument_list()) {
            assert(it.real_item());  // Sanity check (no dangling 'ref')
            auto arg = clone_if_transient(thd, &it);
            if (arg == nullptr) return nullptr;
            c_cond->argument_list()->push_back(arg);
          }
          c = c_cond;
          break;
        }
        case Item_func::TYPECAST_FUNC: {
          // cf wrap_in_cast()
          switch (i_f->data_type()) {
            case MYSQL_TYPE_DATETIME:
              c = new Item_typecast_datetime(i_f->get_arg(0), false);
              break;
            case MYSQL_TYPE_DATE:
              c = new Item_typecast_date(i_f->get_arg(0), false);
              break;
            case MYSQL_TYPE_TIME:
              c = new Item_typecast_time(i_f->get_arg(0));
              break;
            case MYSQL_TYPE_DOUBLE:
              c = new Item_typecast_real(i_f->get_arg(0));
              break;
            default:
              break;
          }
          break;
        }
        case Item_func::DATETIME_LITERAL: {
          auto i_dl = down_cast<Item_datetime_literal *>(i);
          MYSQL_TIME ltime;
          i_dl->get_date(&ltime, 0);
          c = new (thd->mem_root) Item_datetime_literal(
              &ltime, i_dl->decimals, thd->variables.time_zone);
          break;
        }
        default:
          break;
      }
      break;
    }
    case Item::CACHE_ITEM: {
      // cf Item::cache_const_expr_transformer
      auto i_cache = down_cast<Item_cache *>(i);
      auto c_cache =
          Item_cache::get_cache(i_cache->get_example(), i->result_type());
      c_cache->setup(i_cache->get_example());
      c_cache->store(i_cache->get_example());
      c = c_cache;
      break;
    }
    case Item::FIELD_ITEM: {
      // Could originate from gc_subst_transformer().
      auto i_field = down_cast<Item_field *>(i);
      assert(i_field->field->is_gcol());
      c = new Item_field(i_field->field);
      break;
    }
    default:
      break;
  }
  if (!c) {
    // Some constants have a working clone_item().
    c = i->clone_item();
  }
  if (!c) {
    // Unlike other items above, these two temporal literals have no type() or
    // functype() to identify them, so we dynamically cast to see:
    auto i_time = dynamic_cast<Item_time_literal *>(i);
    if (i_time) {
      // PLAN_CACHE_PORT inspired by pq_clone() for such item; though no need to
      // alloc 'ltime' on MEM_ROOT, it's used only temporarily.
      MYSQL_TIME ltime;
      i_time->get_time(&ltime);
      c = new (thd->mem_root) Item_time_literal(&ltime, i_time->decimals);
    } else {
      auto i_datetime = dynamic_cast<Item_date_literal *>(i);
      if (i_datetime) {
        MYSQL_TIME ltime;
        i_datetime->get_date(&ltime, 0);
        c = new (thd->mem_root) Item_date_literal(&ltime);
      }
    }
  }
  if (!c) {
    // Still unknown Item, not able to copy this type. We have an assertion
    // here, because we are interested in discovering uncovered types when
    // testing internally.
    assert(false);
    return nullptr;
  }
  Item *dummy = c;
  // Some ctors above have set 'fixed=true', some not. Run fix_fields().
  if (!c->fixed) {
    // fix_fields() on a comparison operator (like "=") may evaluate its
    // arguments, which is:
    // - undesirable here
    // - causing trouble if one argument is a subquery (this subquery's
    // evaluation may trigger its optimization, with optimize-time Items-s
    // getting created inside the persistent MEM_ROOT currently active!).
    // So we turn this evaluation off.
    auto flag_on = thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW;
    thd->lex->context_analysis_only |= CONTEXT_ANALYSIS_ONLY_VIEW;
    bool rc = c->fix_fields(thd, &dummy);
    if (!flag_on)
      thd->lex->context_analysis_only &= ~CONTEXT_ANALYSIS_ONLY_VIEW;
    if (rc) return nullptr;
    assert(dummy == c);
  }
  transient_items[i] = c;
  return c;
}

/**
 PLAN_CACHE_PORT this is needed in the new plan cache implementation, where
 we sometimes re-use the permanent items instead of cloning everything.

 This function stores 'src' into 'dst' and refreshes 'used_tables'. Also, for
 a condition, it makes a copy of the AND-OR structure ; because the condition
 may be modified later in the process (*) so we need the modification to be done
 on a copy, so the original is preserved for the next execution.

 (*) details: after we have applied the cached plan, make_tmp_tables_info()
 runs. Which calls JOIN::add_having_as_tmp_table_cond(). This function may
 move HAVING into the QEP_TAB's condition, like this: qep_tab condition =
 qep_tab condition AND having condition. If before this, qep_tab condition was
 an AND then we now have qep_tab condition = (A AND B) AND having condition.
 then Item_cond::fix_fields() on this new AND will dissolve the nested AND:
 qep_tab condition = A AND B AND having condition. And this dissolving has
 the side effect that the old AND Item_cond_and becomes empty (in
 cond->list.clear() in Item_cond::fix_fields()). If we didn't make a copy
 here, then this would make the cached condition empty, so a next execution
 (next applying of the cached plan) would be wrong.

 @param dst store the new item here
 @param src source item
 @param is_cond if true, this is used as condition.
*/
static inline void get_item_and_refresh(Item *&dst, Item *src, bool is_cond) {
  dst = src;
  if (!src) return;
  if (is_cond) dst = dst->copy_andor_structure(current_thd);
  // It is important to update Item-s after we rolled forward
  // Exec_context::new_change_list, so they know they are constant (e.g. due
  // to rolling forward a constant propagation in an argument). You may search
  // for "update_used_tables" in session_plan_cache.test for an example.
  dst->update_used_tables();
}

/**
  This function is used for plan cache. When reuse a cached plan for a ref
  scan, if referenced value is changed, this function is responsible to change
  the values of store_keys of Index_lookup.

  This function was made by copying create_ref_for_key() and simplifying.
  Future changes to create_ref_for_key() probably must be propagated here. So:
  HUAWEI_ASSERT_MATCH_CANON create_ref_for_key
*/
bool QEP_TAB::replace_cache_key() {
  Index_lookup *t_ref = &ref();
  THD *const thd = join()->thd;
  Key_use *keyuse = NULL;
  KEY *const keyinfo = table()->key_info + t_ref->key;
  uchar *key_buff = t_ref->key_buff;

  auto set_size = ALIGN_SIZE(t_ref->key_length);
  memset_s(key_buff, set_size, 0, set_size);
  memset_s(t_ref->key_buff2, set_size, 0, set_size);

  for (uint part_no = 0; part_no < t_ref->key_parts; part_no++) {
    keyuse = &position()->key[part_no];
    bool maybe_null = keyinfo->key_part[part_no].null_bit;

    get_item_and_refresh(keyuse->val, keyuse->val, false);

    store_key *s_key =
        get_store_key(thd,
                      // PLAN_CACHE_PORT signature changed in .41: arg changed
                      // from Keyuse* to Item*
                      keyuse->val, join()->const_table_map,
                      // PLAN_CACHE_PORT new arg in .41
                      join()->const_table_map, &keyinfo->key_part[part_no],
                      key_buff, maybe_null);
    if (unlikely(!s_key || thd->is_fatal_error())) return true;

    /* PLAN_CACHE_PORT the original plan cache does not test the return value,
       which is like in create_ref_for_key(). However I think it's better to
       test it. Consider that we have "WHERE idx_col=?", and we're in
       optimization, an index lookup is chosen, and '?' has a value which is in
       the range of idx_col. reduce_cond_for_table() runs, using test_if_ref(),
       and decides that the post-lookup filter (Item_func_eq) is redundant (it
       is, as the value of '?' is in-bounds), and eliminates it. We cache this
       plan. Then we re-execute, but with a value of '?' out of bounds of
       idx_col. copy() below truncates it to a bound, the lookup is done, and
       may return a wrong row. And there's no post-lookup filter to eliminate
       this row, anymore. Thus, if truncation happens, we bail out. The effect
       will be that plan cache will not be used for this execution. */
    if (s_key->copy() != store_key::STORE_KEY_OK) return true;

    if (s_key->null_key)
      t_ref->key_copy[part_no] = s_key;  // Reevaluate in JOIN::exec()
    else
      t_ref->key_copy[part_no] = NULL;

    key_buff += keyinfo->key_part[part_no].store_length;
  }
  return false;
}

/// This function is a very simplified, sort of copy constructor for JOIN, for
/// the use of session-plan-cache.
///
/// @todo see if we can merge it with JOIN::pq_copy_from
/// @returns true if error

bool JOIN::shallow_clone(JOIN *orig) {
  // If, in the future, new members are added to class JOIN, they probably
  // must be copied here. So:
  // HUAWEI_ASSERT_MATCH_CANON sql_class_JOIN

  tables_list = query_block->leaf_tables;
  const_table_map = orig->const_table_map;
  where_cond = orig->where_cond;
  having_cond = orig->having_cond;
  having_for_explain = orig->having_for_explain;
  tables = orig->tables;
  explain_flags = orig->explain_flags;
  calc_found_rows = orig->calc_found_rows;
  m_select_limit = orig->m_select_limit;
  send_group_parts = orig->send_group_parts;
  grouped = orig->grouped;
  group_optimized_away = orig->group_optimized_away;
  implicit_grouping = orig->implicit_grouping;
  need_tmp_before_win = orig->need_tmp_before_win;
  simple_group = orig->simple_group;
  simple_order = orig->simple_order;
  streaming_aggregation = orig->streaming_aggregation;
  m_ordered_index_usage = orig->m_ordered_index_usage;
  skip_sort_order = orig->skip_sort_order;
  select_distinct = orig->select_distinct;
  group_list = orig->group_list;
  order = orig->order;
  rollup_state = orig->rollup_state;
  best_read = orig->best_read;
  // PLAN_CACHE_PORT this is used in Query_expression::optimize() so let's try
  // to not let it stay at 0.
  best_rowcount = orig->best_rowcount;
  m_windows_sort = orig->m_windows_sort;

  return false;
}

/**
   When a plan is cached, QEP_TABs for tmp tables are allocated into this plan
   (e.g. tmp table for GROUP BY). They remain empty, because the intention
   that when the plan is applied, make_tmp_tables_info() will fill them.
   However, when the plan is applied a second time, this 'filling' needs to be
   cleared, so that make_tmp_tables_info() can run properly again. Overall, it
   means we have to re-run the constructor.

   Note that at the end of the first execution of the cached plan,
   QEP_TAB::cleanup() has been called on this QEP_TAB. All that remains after
   that in QEP_TAB, is dangling pointers like condition(), integer or boolean
   members describing plan details... Thus this function's job is to clear
   these.
 */
void QEP_TAB::rerun_constructor_for_tmp_table() {
  // This function is only for optimizer-internal tmp tables
  assert(table_ref == nullptr);
  auto save_join = join();  // save ptr before losing it
  auto save_idx = idx();
  // Using placement-new to overwrite existing object with empty object.
  auto *new_qs = new (m_qs) QEP_shared;
  assert(new_qs == m_qs);
  auto new_tab [[maybe_unused]] = new (this) QEP_TAB;
  assert(new_tab == this);
  set_qs(new_qs);
  set_join(save_join);
  set_idx(save_idx);
  // The primary motivation for this function is to clear the short-lived
  // condition created by JOIN::add_having_as_tmp_table_cond(); so:
  assert(condition() == nullptr);
}

/////////////////// End of dependencies

namespace plan_cache {

std::atomic<ulong> cached_plan_count{0};
std::atomic<ulong> cached_plan_invalidations{0};

class Clone_plan_RAII {
 public:
  explicit Clone_plan_RAII(THD *thd)
      : m_thd(thd),
        save_want_privilege(m_thd->want_privilege),
        save_allow_sum_func(m_thd->lex->allow_sum_func) {
    m_thd->want_privilege = SELECT_ACL;
    m_thd->lex->allow_sum_func |= ((nesting_map)1)
                                  << m_thd->lex->query_block->nest_level;
  }
  ~Clone_plan_RAII() {
    m_thd->want_privilege = save_want_privilege;
    m_thd->lex->allow_sum_func = save_allow_sum_func;
  }

 private:
  THD *m_thd;
  ulonglong save_want_privilege;
  nesting_map save_allow_sum_func;
};

void set_uncacheable(Query_block *sl) {
  sl->plan_cache_state = plan_cache_state::UNCACHEABLE;
  // This state is final. After that, 'sl' will never do plan caching.
}

/// Throws away an already cached plan.
void invalidate_cached_plan(Query_block *select) {
  JOIN *plan = select->cached_plan;
  if (!plan) return;

  Opt_trace_context *const trace = &plan->thd->opt_trace;
  Opt_trace_object trace_plan_cache(trace, "invalidate_cached_plan");
  trace_plan_cache.add("invalidate", true);
  // If already hit, plan->destroy() has been called already (when a previous
  // execution ended, which had join==plan). Avoid double call:

  // PLAN_CACHE_PORT let me explain more. Independently of plan caching, after
  // execution of a plan, JOIN::destroy() is called and this function does not
  // support being called a 2nd time (the QEP_TAB::cleanup() function may have
  // destroyed some members in table() especially if this was a materialized
  // tmp table, so on 2nd time there are unreadable pointers). Let's bring
  // caching in. In the normal situation of "1 cache a plan, 2 re-use cached
  // plan, 3 re-use cached plan once more", in step 2 the JOIN is the cached
  // plan, and thus at end of step 2 there is a call to JOIN::destroy() on it,
  // in step 3 we re-fill it properly (apply_cached_plan() etc), so at the end
  // of step 3 it can go into another JOIN::destroy() fine. Now to
  // invalidation. In the situation of "1 cache a plan, 2 re-use cached plan,
  // 3 try to re-use it once more but see it cannot so invalidate it", the
  // invalidation in 3 operates on a plan which has just gone out of
  // JOIN::destroy(), so we mustn't do it again. While in the situation of "1
  // cache a plan, 2 try to re-use it once more but see it cannot so
  // invalidate it", the invalidation in 2 operates on a plan which has not
  // just gone through JOIN::destroy(), so we must do it. Hence this if(),
  // which interprets "this plan has been used at least once" to "so
  // invalidation is occurring after a recent call to JOIN::destroy()".
  // Personally I think that this interpretation, though generally true as I
  // tried to explain, is possibly not fool-proof in all scenarios of
  // failures.

  if (!plan->is_cached_plan_hit()) plan->destroy();
  destroy_cached_plan(plan);
  select->cached_plan = nullptr;
  select->plan_cache_state = plan_cache_state::NONE;
  cached_plan_invalidations++;
}

void destroy_cached_plan(JOIN *join) {
  cached_plan_count--;
  auto arena = (join->plan_cache_exec_context != nullptr)
                   ? join->plan_cache_exec_context->arena
                   : nullptr;
  destroy(join);  // Note: this is a call to ~JOIN(), not to JOIN::destroy(),
  // and it calls ~Exec_context() too. Which is why we saved
  // Exec_context::arena first.
  if (arena) {
    cleanup_items(arena->item_list());
    // This is necessary, or we may leak memory. F.ex. we cloned an Item_json
    // and Item_json::clone_item() allocated a Json_dom on the heap (not on a
    // MEM_ROOT ; look at all the calls to 'new' in json_dom.cc) ; this thing
    // remains reachable only from Item_json. We must free it by calling the
    // destructor of Item_json below.
    arena->free_items();
    // This is where memory allocated in a MEM_ROOT to hold the cached plan
    // (Exec_context, JOIN, QEP_TAB, clones of Items...), is finally freed. We
    // have to be cautious: the MEM_ROOT itself is allocated in its own
    // buffers (see at the end of cache_plan(), the variable mem_root_heap),
    // so if we destroy the MEM_ROOT it risks becoming unreadable by its
    // own destructor. So we first move this MEM_ROOT to a stack object, which
    // takes ownership of the buffers, and whose destructor is then implicitly
    // called when it goes out of scope. This is the reverse operation
    // (move-to-stack) of what was done at the end of cache_plan()
    // (move-from-stack).
    auto root(std::move(*arena->mem_root));
    // Make sure the compiler moved it, not copied it.
    assert(arena->mem_root->allocated_size() == 0 &&
           root.allocated_size() != 0);
    ::destroy(arena);
  }
}

bool is_ready(Query_block *sl) {
  return sl->plan_cache_state == plan_cache_state::READY;
}

void set_ready(Query_block *sl) {
  sl->plan_cache_state = plan_cache_state::READY;
}

static Key_map get_quick_key_map(AccessPath *quick) {
  Key_map keys_map{0};
  auto idx = used_index(quick);
  if (idx != MAX_KEY)
    keys_map.set_bit(idx);
  else {
    Mem_root_array<AccessPath *> *children = nullptr;
    switch (quick->type) {
      case AccessPath::ROWID_UNION:
        children = quick->rowid_union().children;
        break;
      case AccessPath::INDEX_MERGE:
        children = quick->index_merge().children;
        break;
      case AccessPath::ROWID_INTERSECTION:
        children = quick->rowid_intersection().children;
        break;
      default:
        assert(0);
        break;
    }
    if (children) {
      for (auto inner_path : *children)
        keys_map.merge(get_quick_key_map(inner_path));
    }
  }

  return keys_map;
}

/*
 * Check whether current plan can be cached.
 *
 * @param[in] join pointer of current plan object
 *
 * @reval FALSE plan can't be cached. Otherwise, it can be.
 */
bool check_query_plan_cachable(JOIN *join) {
  Query_block *select = join->query_block;
  auto leaf_table_count = select->leaf_table_count;
  // Only one table
  if (leaf_table_count != 1) return false;

  Query_expression *unit = select->master_query_expression();
  THD *thd = join->thd;
  if (!thd->lex->m_sql_cmd) return false;
  // "WHERE col=@a" may be optimized to WHERE FALSE or WHERE TRUE, which
  // cannot be cached in a plan as @a may change.
  if (thd->lex->has_user_or_system_var()) return false;
  // For WITH ROLLUP, items in query_block::fields will be transformed into
  // Item_rollup**, which can't be found in query_block::fields, making
  // make_group_order_list fail and plan not be cached. We short-cut this
  // case.
  if (select->olap == ROLLUP_TYPE) return false;
  // Don't support stored procedure.
  if (thd->sp_runtime_ctx) return false;
  // Don't support stored function.
  if (thd->lex->sroutines_list.elements > 0) return false;

  // Don't support query under lock tables because clone plan needs open a new
  // table which causes a mess under lock tables.
  if (thd->locked_tables_mode == LTM_LOCK_TABLES ||
      thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES)
    return false;

  // Here we need check UNION from root Query_expression.
  // PLAN_CACHE_PORT: also blocking EXCEPT,INTERSECT (possible in .41).
  if (unit->is_set_operation()) return false;

  if (!(  // user asked to cache plans
          thd->variables.rds_plan_cache &&
          // must be a prepared statement
          thd->stmt_arena->is_stmt_prepared_or_executed() &&
          // must be a SELECT command
          select->parent_lex->sql_command == SQLCOM_SELECT))
    return false;

  if (unit->outer_query_block()) {
    // If this is a subquery, it should be scalar and not correlated. This is
    // to ensure that :
    // - it undergoes no transformation,
    // - its value is not parameterized on the outer query block's current row
    // (which would complicate plan caching).
    // Last, max-min is the product of a subquery transformation, we do not
    // want to tackle the complexity of these yet.
    if (!(unit->item &&
          unit->item->substype() == Item_subselect::SINGLEROW_SUBS &&
          !down_cast<Item_singlerow_subselect *>(unit->item)->is_maxmin() &&
          !unit->item->is_uncacheable()))
      return false;
    // If Item tree changes have been done, given that THD::change_list is
    // common to all query blocks, this makes it impossible for us to identify
    // which changes must be replayed later in apply_cached_plan().
    if (!thd->change_list.is_empty()) return false;
  }

  if (select->first_inner_query_expression()) {
    // If this contains a subquery, see above
    auto inner_unit = select->first_inner_query_expression();
    if (!(inner_unit->item &&
          inner_unit->item->substype() == Item_subselect::SINGLEROW_SUBS &&
          !down_cast<Item_singlerow_subselect *>(inner_unit->item)
               ->is_maxmin() &&
          !inner_unit->item->is_uncacheable()))
      return false;
    if (!thd->change_list.is_empty()) return false;
  }

  for (uint i = 0; i < leaf_table_count; ++i) {
    QEP_TAB *qep_tab = &join->qep_tab[i];
    TABLE *table = qep_tab->table();
    // Fulltext is not supported
    if (qep_tab->type() == JT_FT) return false;
    if (table->s->is_secondary_engine()) return false;
  }

  if (is_temporary_table(select->leaf_tables) ||
      // Don't support information_schema table
      select->leaf_tables->schema_table ||
      select->leaf_tables->is_system_view ||
      is_infoschema_db(select->leaf_tables->db) ||
      is_perfschema_db(select->leaf_tables->db))
    return false;

  if (select->hidden_items_from_optimization > 0) {
    // Some fields were added to Query_block::fields during optimization, and
    // as we do not repeat this addition when applying a cached plan, we are
    // not able to re-create a correct execution. So, no plan caching. Grep
    // for this counter for more details. The known case is: ORDER BY <expr>
    // where <expr> is replaced by the Optimizer with a generated column. This
    // is a new limitation in the new implementation.
    return false;
  }

  // Considering where cache_plan() is called, this holds. See related
  // comment in reinit_qep_tab_properties().
  assert(join->zero_result_cause == nullptr);

  // Note that when using the hypergraph optimizer, JOIN::optimize() does not
  // call cache_plan(). So the plan cache does not work with such optimizer;
  // and to solve this, it would need a design change, as it needs a QEP_TAB
  // whereas the hypergraph optimizer does not create any.

  return true;
}

/*
 * During clone plan, we need clone ORDER/GROUP BY list. This is because some
 * optimization might occur for ORDER/GROUP BY statement. For example
 * ORDER/GROUP BY '1', col1: optimizer will remove const expression '1' in
 * remove_const(). Thus plan cache should clone such list so as to reuse such
 * an optimization. The ORDER BY clause of WINDOW does not undergo the same
 * optimization so we needn't clone it.
 *
 * @skip_fix is used to mark whether ORDER object needs a refix. When cache
 * a plan, it's not necessary. There is one case for virtual column in ORDER
 * object. Optimizer will create a Item_field to replace an expression with
 * related virtual column. Such a Item_field is created in current memory,
 * which can't be referenced by cached plan because such a Item_field will be
 * destroyed after query ends. So it should not be fixed during cached plan.
 */
static bool make_group_order_list(THD *thd, Query_block *sl, ORDER **dst,
                                  ORDER *orig, bool skip_fix [[maybe_unused]]) {
  assert(dst && !*dst);
  ORDER *ptr = nullptr;
  for (ORDER *group = orig; group; group = group->next) {
    ORDER *group_new = pq_dup_order(thd, sl, group);
    if (!group_new) return true;
    if (ptr) {
      ptr->next = group_new;
      ptr = ptr->next;
    } else {
      ptr = group_new;
    }
    // Point to the first cloned ORDER item.
    if (!*dst) *dst = ptr;
  }
  return false;
}

bool Exec_context::fill(THD *thd, JOIN *join, QEP_TAB *cached_qep_tab) {
  optimizer_switch =
      (thd->variables.optimizer_switch & interested_optimizer_switch_flags);

  // Do deep copy for tmp_table_param, see cache_plan() for details
  // PLAN_CACHE_PORT signature of copy ctor changed
  if (!(tmp_table_param = new (thd->mem_root)
            Temp_table_param(thd->mem_root, join->tmp_table_param)))
    return true;

  character_set_client = thd->variables.character_set_client;
  auto orig_tab = &join->qep_tab[0];
  TABLE *table = orig_tab->table();
  // PLAN_CACHE_PORT: using new clone_if_transient() instead of pq_clone()
  if (table->file->pushed_idx_cond &&
      (!(pushed_idx_cond =
             clone_if_transient(thd, table->file->pushed_idx_cond))))
    return true;
  pushed_idx_cond_keyno = table->file->pushed_idx_cond_keyno;
  key_read = table->key_read;
  // Note that we are after partition pruning, so the number below is that of
  // remaining partitions. Later when we want to reuse this plan, we'll be
  // before pruning, thus is_table_stats_changed_sharply() will compare apple
  // to oranges which may lead to invalidation of the plan, alas.
  table_records = table->file->stats.records;
  table_version = table->s->get_table_ref_version();
  covering_keys = table->covering_keys;
  // Used to check whether secondary engine optimization is needed.
  current_query_cost = join->best_read;
  Query_block *sl = join->query_block;
  if (join->group_list.order) {
    ORDER *order = nullptr;
    if (make_group_order_list(thd, sl, &order, join->group_list.order, true))
      return true;
    distinct_group_list =
        new (thd->mem_root) ORDER_with_src(order, join->group_list.src);
    if (!distinct_group_list) return true;
  }
  if (join->order.order) {
    ORDER *order = nullptr;
    if (make_group_order_list(thd, sl, &order, join->order.order, true))
      return true;
    order_list = new (thd->mem_root) ORDER_with_src(order, join->order.src);
    if (!order_list) return true;
  }
  // PLAN_CACHE_PORT: problems below do not apply as we use the new
  // clone_if_transient() instead of pq_clone(); so we avoid this complexity.
  // clone_if_transient() clones a limited set of item types, so has an easier
  // job. This place need to call refix_fields function to resolve conditions
  // because cloned_condition returned by pq_clone is not complete(Cloned_1) and
  // many attributes are not set. When the second execution, new
  // cloned_condition is created through incomplete condition(Cloned_1). As a
  // result, resolving conditions using refix_fields function will be
  // incorrect.
  if (join->where_cond &&
      (!(where_cond = clone_if_transient(thd, join->where_cond))))
    return true;
  if (join->having_cond &&
      (!(having_cond = clone_if_transient(thd, join->having_cond))))
    return true;

  auto *quick = orig_tab->range_scan();

  if (quick) {
    // pq_dup_tabs() has cloned the QUICK_SELECT_I.
    // We do not store this clone into the being-cached
    // plan, because when we later reuse the plan, we would have to update
    // values in the intervals of ranges to match the new values of prepared
    // statements' parameters for the new execution, which is complex.
    // Instead, when this plan is later reused, its QUICK_SELECT_I will be
    // regenerated by a call to test_quick_select().
    quick_type = quick->type;
    quick_keys_map.merge(get_quick_key_map(quick));
    if (quick->type == AccessPath::INDEX_RANGE_SCAN)
      index_range_scan_props = quick->index_range_scan();
    quick_used_key_parts = get_used_key_parts(quick);
  }

  tab_condition = cached_qep_tab->condition();

  /* Here we have some members of JOIN which have already been copied from the
  original JOIN to our JOIN by JOIN::shallow_clone(). However, after this
  copying happened, their values in the original JOIN will be modified (most
  often in JOIN::make_tmp_tables_info() and callees). You may think this is
  ok, as our JOIN is already made at this time. But now imagine we apply the
  cached plan. Now it is our JOIN (the cached JOIN) which is going to go
  through make_tmp_tables_info() and its members will be modified. So when we
  apply the cached plan a second time (i.e. when we are starting the third
  execution of the prepared statement), the members in our cached JOIN will
  not be what they used to be: the third execution will not start on the same
  state as the second did, leading to bugs. This is why we back up these
  members in the context. */
  explain_flags = join->explain_flags;
  calc_found_rows = join->calc_found_rows;
  m_select_limit = join->m_select_limit;
  grouped = join->grouped;
  streaming_aggregation = join->streaming_aggregation;
  select_distinct = join->select_distinct;
  // rollup_state should also be copied as it's modified, but is not needed as
  // plan cache does not support ROLLUP.
  assert(join->rollup_state == JOIN::RollupState::NONE);
  best_read = join->best_read;

  return false;
}

static bool has_same_type_and_item_params(Item *old_val, Item *new_val);

/**
 * Entry to cache exection plan
 *
 * Check whether current plan is applicable to cache plan. If it is,
 * plan will be cloned and stored.
 * @returns true if error
 */
bool cache_plan(JOIN *orig_join) {
  THD *thd = orig_join->thd;
  Query_block *sl = orig_join->query_block;
  if (is_ready(sl)) return false;  // already cached
  // If optimizer thinks that current plan can't be cached, then skip.
  if (sl->plan_cache_state == plan_cache_state::UNCACHEABLE) return false;
  // Check further whether the query is applicable to be cached.
  if (!check_query_plan_cachable(orig_join)) return false;

  Opt_trace_context *const trace = &thd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object trace_plan_cache(trace, "cache_plan");
  sl->plan_cache_state = plan_cache_state::START;
  assert(thd->stmt_arena);

  // Before switching to a new MEM_ROOT, save pointers to transient items:
  auto transient_mem_root = thd->mem_root;
  auto transient_item_list = thd->item_list();

  // We here switch to a MEM_ROOT which is dedicated to plan cache. It makes
  // sense, as the current MEM_ROOT (the execution MEM_ROOT), will be freed at
  // the end of execution, and our cached plan (JOIN, Item-s etc) must survive
  // longer. The original implementation instead used the statement's
  // permanent MEM_ROOT (thd->stmt_arena->mem_root). But this had a bad
  // consequence: if we later invalidated this plan, and then made a new
  // cached plan, invalidated it, etc, we used to grow the permanent MEM_ROOT
  // indefinitely as we made new clones (yes this can happen in real life,
  // grep for GROUP_INDEX_SKIP_SCAN in session_plan_cache.test; it can also be
  // provoked at will, if you look at is_environment_changed(): prepare,
  // execute, change optimizer_switch flag to "mrr=off", execute, change to
  // "mrr=on", execute, repeat forever).

  MEM_ROOT mem_root{key_memory_plan_cache_mem_root, 1024};

  // When a plan is invalidated, the MEM_ROOT above is freed, and so are any
  // Items which were allocated into it. Therefore, these Items must not be
  // left in the item_list() of some Query_arena which would outlive the plan.
  // We thus have a dedicated arena too:
  Query_arena arena(&mem_root, Query_arena::STMT_PREPARED), arena_backup;
  // With this, all new Items created by the plan cache will be in the
  // item_list() of 'arena', and thd->mem_root is updated.
  thd->swap_query_arena(arena, &arena_backup);

  Clone_plan_RAII cpr(thd);
  JOIN *join_local = new (thd->mem_root) JOIN(thd, sl);

  auto error_guard =
      create_scope_guard([&trace_plan_cache, orig_join, join_local, sl, thd,
                          &arena, &arena_backup]() {
        trace_plan_cache.add("cached", false);
        orig_join->query_block->cached_plan = nullptr;
        if (join_local) {
          join_local->destroy();
          ::destroy(join_local);
        }
        set_uncacheable(sl);
        // return to the previous arena and destroy unneeded Items
        thd->swap_query_arena(arena_backup, &arena);
        cleanup_items(arena.item_list());
        arena.free_items();
        // MEM_ROOT is on stack, will be destroyed implicitly.
      });

  auto some_item_param_are_gone = [thd]() {
    // Check whether Item_param objects have been optimized away or transformed
    // into other type. If so, cached plan must be invalidated.
    I_List_iterator<Item_change_record> it(thd->change_list);
    Item_change_record *change;
    while ((change = it++)) {
      if (!has_same_type_and_item_params(change->old_value,
                                         change->new_value)) {
        return true;
      }
    }
    return false;
  };

  // We run the check a first time now ; if it fails, it saves us from doing
  // useless allocations; it also prevents clone_if_transient() from calling
  // clone_item() on the special case of an Item_int_with_ref wrapping a
  // (non-constant) Item_param, a case where clone_item() fails with assertion
  // failure.
  if (some_item_param_are_gone()) return true;

  if (!(join_local->plan_cache_exec_context =
            new (thd->mem_root) Exec_context(thd->mem_root)))
    return true;

  orig_join->query_block->cached_plan = join_local;
  join_local->shallow_clone(orig_join);
  // No Table_ref is stored in the cached plan.
  join_local->tables_list = nullptr;

  {
    auto &transient_items_map =  // shorter synonym
        join_local->plan_cache_exec_context->transient_items;

    for (auto i = transient_item_list; i != nullptr; i = i->next_free) {
      if (transient_mem_root->Contains(i)) {
        assert(transient_items_map.count(i) ==
               0);                         // There is uniqueness in list.
        transient_items_map[i] = nullptr;  // No clone exists yet.
      } else {
        // As we are in execution, when it started the execution Query_arena
        // has been activated, which (cf set_query_arena()) has changed
        // THD::item_list(). So all items we meet in transient_item_list
        // should be in the transient MEM_ROOT.
        assert(false);
      }
    }

    auto &new_change_list =
        join_local->plan_cache_exec_context->new_change_list;

    I_List_iterator<Item_change_record> it(thd->change_list);
    Item_change_record *change;
    while ((change = it++)) {
      assert(!change->m_cancel);  // A grep in code shows it's always false
      // Track only changes to permanent items. Transient items will
      // disappear.
      if (!transient_mem_root->Contains(change->place))
        new_change_list.push_back({change->place, change->new_value});
    }
    // Vector is in order last-change-is-first. Reverse it, because we want
    // new_change_list to start with older changes first, as we will replay
    // them in such order when we apply the cached plan.
    std::reverse(new_change_list.begin(), new_change_list.end());
    // Clone transient items and record the clone's address.
    for (auto &c : new_change_list) {
      if (transient_mem_root->Contains(c.second)) {
        auto n = clone_if_transient(thd, c.second);
        if (!n) return true;
        c.second = n;
      }
    }
  }

  if (pq_dup_tabs(join_local, orig_join, false)) return true;
  // During clone, fix_fields might do a transformation, which might cause
  // the plan to not be cacheable
  if (sl->plan_cache_state == plan_cache_state::UNCACHEABLE) return true;
  // Do deep copy for tmp_table_param. Following execution on join_local may
  // allocate memory when create temp table, if allocate memory on
  // thd->stmt_arena may result in increased memory usage even OOM for long
  // session, need to reinitialize mem root allocator for copy_fields and
  // grouped_expressions to thd->mem_root, instead of use permanent memory
  // of prepared statement(thd->stmt_arena). Note "new (&...)" operator is
  // placement-new which doesn't allocate any memory, no need to delete it
  // manually.
  new (&join_local->tmp_table_param)
      Temp_table_param(thd->mem_root, orig_join->tmp_table_param);

  if (join_local->plan_cache_exec_context->fill(thd, orig_join,
                                                join_local->qep_tab))
    return true;

  // Repeat the check done when the function started, because since then, we
  // cloned and fixed more Items, which may have changed types again (usually
  // happens in the fixing of equalities).
  if (some_item_param_are_gone()) return true;

  // Here we reset m_qs->m_table to be nullptr in order to free such a cached
  // plan correctly because TABLE::file might be closed when such a cached
  // plan tries to free itself, such as this cached plan is never hit.
  join_local->qep_tab[0].set_table(nullptr);

  set_ready(sl);

  if (join_local->alloc_func_list()) return true;

  // Now we have to protect the plan's MEM_ROOT (an object on the stack) from
  // being deleted when leaving the function. For that, we save (move) it to
  // heap memory and keep a pointer in the Exec_context. And do the same for
  // Query_arena.
  auto root_storage = mem_root.Alloc(sizeof(MEM_ROOT));
  if (root_storage == nullptr) return true;
  auto arena_storage = mem_root.Alloc(sizeof(Query_arena));
  if (arena_storage == nullptr) return true;

  thd->swap_query_arena(arena_backup, &arena);
  error_guard.release();
  // From here, no call must fail until the function ends.
  auto mem_root_heap = new (root_storage) MEM_ROOT(std::move(mem_root));
  // Make sure the compiler moved it, not copied it.
  assert(mem_root.allocated_size() == 0 &&
         mem_root_heap->allocated_size() != 0);
  // Query_arena has no move constructor so we do a swap.
  auto arena_heap = new (arena_storage) Query_arena();
  std::swap(arena, *arena_heap);
  assert(arena.mem_root == nullptr && arena_heap->mem_root == &mem_root);
  arena_heap->mem_root = mem_root_heap;
  // Items which have just been created for this plan, are not going to be
  // used in this execution, so we clean up them up, as is commonly done when
  // an execution ends. They will be re-bound (to tables) with bind_fields().
  cleanup_items(arena_heap->item_list());
  join_local->plan_cache_exec_context->arena = arena_heap;
  cached_plan_count++;
  trace_plan_cache.add("cached", true);
  return false;
}

/// @returns true if Plan Cache wants the optimizer to assume that a certain
/// hint has been used. @See comment in Exec_context::fill().
bool emulates_hint(const THD *thd, opt_hints_enum hint) {
  auto qb = thd->lex->current_query_block();
  JOIN *plan = qb ? qb->cached_plan : nullptr;
  if (!plan) return false;
  auto quick_type = plan->plan_cache_exec_context->quick_type;
  switch (hint) {
    case INDEX_MERGE_HINT_ENUM:
      if (quick_type == AccessPath::ROWID_UNION ||
          quick_type == AccessPath::ROWID_INTERSECTION ||
          quick_type == AccessPath::INDEX_MERGE)
        return true;
      break;
    case SKIP_SCAN_HINT_ENUM:
      if (quick_type == AccessPath::INDEX_SKIP_SCAN) return true;
      break;
    default:
      break;
  }
  return false;
}

static int evaluate_const_cond(THD *thd, Item *const_cond) {
  bool const_cond_result = true;
  if (const_cond) {
    const_cond_result = const_cond->val_int() != 0;
    if (thd->is_error()) return 1;
  }
  if (!const_cond_result) return -1;

  return 0;
}

/**
   Initialize properties for QEP_TAB according to scan method.

   @param qt pointer of QEP_TAB

   @retval 0 success
           1 error
          -1 nullable table is returned.
*/
static int reinit_qep_tab_properties(QEP_TAB *qt) {
  JOIN *join = qt->join();
  THD *thd = join->thd;
  Item *qep_cond = nullptr;
  get_item_and_refresh(qep_cond, join->plan_cache_exec_context->tab_condition,
                       true);
  qt->set_condition(qep_cond);
  // PLAN_CACHE_PORT This line below was introduced in Taurus by
  // "BUG2023110302419 Fix the crash caused by the interaction between
  // features plan cache and NDP" but the line makes sense even without NDP so
  // is included here. The NDP-specific mtr test is not, however.
  qt->reset_condition_as_pushed_to_sort();
  Item *where_cond = nullptr;
  get_item_and_refresh(where_cond, join->query_block->where_cond(), true);
  // Approximate number of found rows to read them, ndp need this information
  // Only one matching row for JT_SYSTEM or JT_CONST, otherwise use file->stats
  ha_rows read_rows = 1;
  if (qt->type() != JT_SYSTEM && qt->type() != JT_CONST) {
    read_rows = qt->table()->file->stats.records;
  }
  qt->set_records(read_rows);

  // Only JT_SYSTEM/JT_CONST can set zero_result_cause to "Impossible WHERE".
  // For JT_SYSTEM, the table has only one row and table'storage is heap such as
  // MEMORY, but taurus only supports innodb storage, so reset zero_result_cache
  // in this place for coverage. And other impossible where_cond situations suce
  // as where 1=@a , plan_cache_state is set UNCACHEABLE and the plan is not
  // cached.

  // PLAN_CACHE_PORT the comment above is becoming out of date, as we're
  // supporting Dstore on top of InnoDB. Also, code reading suggests that if
  // JOIN::optimize() sets zero_result_cause, the plan is never cached. So we
  // could think we can simplify the assert below to:
  // assert(!join->zero_result_cause); But in the present function, we then do
  // set zero_result_cause in some "case" labels below (btw, why do we do this:
  // so that the execution shortcuts). Then, if we come to execute this cached
  // plan again, we understand why the assertion is relaxed. But actually no:
  // reset_cached_plan() has set zero_result_cause to 0 before coming here. So I
  // simplified:
  // - assert, when we cache a plan, that zero_result_cause is 0.
  // - assert that it is 0 here
  // - continue setting it in the switch below.
  assert(!join->zero_result_cause);  // per reset_cached_plan()

  switch (qt->type()) {
    case JT_SYSTEM: {
      TABLE *table = qt->table();
      // Verify that this is still a system table.
      if (!((table->s->system || table->file->stats.records <= 1 ||
             table->all_partitions_pruned_away) &&
            (table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT))) {
        // PLAN_CACHE_PORT invalidate_cached_plan(join->query_block) removed;
        // instead, doing this at the end of the caller (exec_cached_plan()), to
        // "centralize" this.
        return 1;
      }
      int status = read_system(qt->table());
      if (status == 0) {
        // Imagine that the query is SELECT FROM const_table WHERE cond; Then,
        // Then, in the first optimization, 'cond' was evaluated, let's assume
        // it was TRUE (if it was FALSE, then we got a zero_result_cause and
        // the plan was not cached). 'cond' was not recorded in QEP_TABs.
        // Maybe now the table's single row's value has changed and we thus
        // must evaluate 'cond' again, in case it is FALSE now.  As the plan
        // cache is only about one table, we can test the whole WHERE
        // condition.
        status = evaluate_const_cond(thd, where_cond);
      }
      if (status == -1) join->zero_result_cause = "Impossible WHERE";
      return status;
    }
    case JT_CONST: {
      // This table still has the right indexes which make it JT_CONST, unless
      // there was a DDL to change indexes, but in that case plan cache
      // detected the change in metadata version number and invalidated the
      // plan.
      if (qt->replace_cache_key()) return 1;
      int status = read_const_maybe_key_read(qt);
      if (status == 0) status = evaluate_const_cond(thd, where_cond);
      if (status == -1) join->zero_result_cause = "Impossible WHERE";
      return status;
    }
    case JT_EQ_REF:
    case JT_REF_OR_NULL:
    case JT_REF:
      if (qt->replace_cache_key()) return 1;
      break;
    case JT_INDEX_MERGE:
    case JT_RANGE: {
      AccessPath *qck;
      qt->set_skip_records_in_range(true);
      Key_map needed_reg_dummy;
      // During test_quick_select, needs cost information.
      qt->table()->init_cost_model(join->cost_model());
      auto interesting_order =
          join->order.order ? join->order.order->direction : ORDER_NOT_RELEVANT;
      auto &cached_props =
          join->plan_cache_exec_context->index_range_scan_props;
      if (join->plan_cache_exec_context->quick_type ==
              AccessPath::INDEX_RANGE_SCAN &&
          !(cached_props.mrr_flags & HA_MRR_SORTED)) {
        // It means that the original range access was created without
        // consideration of ORDER BY (see 'order_direction' usage in
        // check_quick_select()); it can happen if the caller was
        // get_quick_record_count(). So, we should do the same.
        interesting_order = ORDER_NOT_RELEVANT;
      }
      // PLAN_CACHE_PORT the signature of test_quick_select has changed
      // significantly since .22.
      MEM_ROOT temp_mem_root(key_memory_test_quick_select_exec,
                             thd->variables.range_alloc_block_size);
      int rc = test_quick_select(
          thd, thd->mem_root, &temp_mem_root,
          join->plan_cache_exec_context->quick_keys_map,  // keys_to_use
          0,                                              // prev_tables
          0,                                              // read_tables
          join->calc_found_rows ? HA_POS_ERROR
                                : join->query_expression()->select_limit_cnt,
          true,  // force quick range
          interesting_order, qt->table(),
          true,  // skip_records_in_range
          join->query_block->where_cond(), &needed_reg_dummy,
          true,  // ignore_table_scan,
          join->query_block, &qck);
      DBUG_EXECUTE_IF("plan_cache_debug_test_quick_select_fail", rc = 0;);
      if (!rc) {
        // PLAN_CACHE_PORT invalidate_cached_plan(join->query_block); removed;
        // instead, doing this at the end of the caller (exec_cached_plan()),
        // to "centralize" this.
        return 1;
      } else if (rc == -1) {
        join->zero_result_cause = "Impossible range";
        return -1;  // impossible range
      }
      if (thd->is_error())  // @todo consolidate error reporting of
        // test_quick_select
        return 1;
      // Verify that the new QUICK_SELECT_I is of the same type and keys as the
      // cached plan had. Imagine the chosen index has changed from A to B:
      // maybe with A some ORDER BY was superfluous and removed by the
      // optimizer, while with B it is not superfluous and we would miss the
      // removed ORDER BY. The same is true for the type, if it has changed
      // from RANGE to INDEX MERGE.
      if (qck && qck->type == join->plan_cache_exec_context->quick_type) {
        if (get_quick_key_map(qck) !=
            join->plan_cache_exec_context->quick_keys_map)
          return 1;
        if (qck->type == AccessPath::INDEX_RANGE_SCAN) {
          if (cached_props.reverse &&
              make_reverse(get_used_key_parts(qck), qck))
            return 1;
          if (cached_props.mrr_flags & HA_MRR_SORTED) {
            // Setting this flag is not done by test_quick_select() but by
            // test_if_skip_sort_order() which we have not called. So:
            set_need_sorted_output(qck);
          }
          // If DS-MRR (disk-sweep multi-range-read) is used : as we skipped the
          // call to records_in_range() in test_quick_select() above (to save
          // time), the estimated number of records is different, and so is the
          // buffer size. If the new one is smaller, it can be inefficient, so
          // we increase it to become as big as the old one. If, instead, it is
          // bigger, this is suspicious, we do not want to allocate a big buffer
          // based on missing cost calculations.
          auto &new_props = qck->index_range_scan();
          if (new_props.mrr_buf_size < cached_props.mrr_buf_size)
            new_props.mrr_buf_size = cached_props.mrr_buf_size;
          // Check that other non-pointer properties are equal
          if (new_props.num_ranges != cached_props.num_ranges ||
              new_props.mrr_flags != cached_props.mrr_flags ||
              new_props.mrr_buf_size != cached_props.mrr_buf_size ||
              new_props.index != cached_props.index ||
              new_props.num_used_key_parts != cached_props.num_used_key_parts ||
              new_props.can_be_used_for_ror !=
                  cached_props.can_be_used_for_ror ||
              new_props.need_rows_in_rowid_order !=
                  cached_props.need_rows_in_rowid_order ||
              new_props.can_be_used_for_imerge !=
                  cached_props.can_be_used_for_imerge ||
              new_props.reuse_handler != cached_props.reuse_handler ||
              new_props.geometry != cached_props.geometry ||
              new_props.reverse != cached_props.reverse ||
              new_props.using_extended_key_parts !=
                  cached_props.using_extended_key_parts)
            return 1;
        }
        if (join->plan_cache_exec_context->quick_used_key_parts !=
            get_used_key_parts(qck))
          return 1;
        qt->set_range_scan(qck);
        qt->set_type(calc_join_type(qck));
      } else
        return 1;
    } break;
    case JT_INDEX_SCAN:
      // Item_param will make sure key_part type to be compatible with value
      // result type. Here we skip to do such a check.
    case JT_ALL:
      break;
    case JT_FT:
    default:
      assert(0);
  }
  return 0;
}

static bool apply_cached_plan(JOIN *join);

/** Check cached plan and see whether it's ready to be used. If it can be
 * used, then apply cached plan.
 *
 * This function will check constraints to see whether current cached plan is
 * adapted to be used. For example, if the table related is altered during
 * applying, plan cache won't be reused and will be invalidated.
 *
 * @retval True means cached plan is not ready or some thing wrong happens.
 * Otherwise false.
 */
static bool apply_cached_plan_if_suitable(THD *thd, Query_block *query_block,
                                          Opt_trace_object &trace) {
  JOIN *cached_plan = query_block->cached_plan;
  assert(cached_plan);
  Exec_context *context = cached_plan->plan_cache_exec_context;
  Table_ref *tl = query_block->leaf_tables;

  auto error_guard = create_scope_guard(
      [query_block]() { invalidate_cached_plan(query_block); });

  if (!thd->change_list.is_empty()) {
    // We expect the change list to be empty, because we are in execution,
    // resolution has not been re-done, and we are at the very start of
    // optimization.But if there are multiple query blocks (like subqueries)
    // another query block may have started its optimization and filled the
    // changed list. If not empty, we would not know how to undo (in case of
    // error) only the changes which apply_cached_plan() is about to do.
    return true;
  }

  // Checked when we cached plan, checked again when we reuse cached plan:
  if (tl->table->s->is_secondary_engine()) {
    trace.add("used", false);
    trace.add_utf8("cause", "Table is using secondary engine");
    return true;
  }
  if (tl->fetch_number_of_rows()) return true;

  /*
    PLAN_CACHE_PORT in .22, ANALYZE causes a re-preparation of the prepared
    statement and thus an invalidation of the cached plan (this was visible in
    the mtr test); but in .41 this changed due to
    3a32cf15f02c065dbe429625731659c095068663 BUG#32224917: ANALYZE TABLE TAKES
    TABLE LOCK DURING INDEX STATS UPDATE, CAUSES QUERY PILEUP, and they don't
    invalidate TABLE_SHARE any more, they just invalidate TABLE, and
    table_ref_version() does not change. So a cached plan is not anymore
    invalidated by ANALYZE TABLE.
  */
  if (tl->table->s->get_table_ref_version() != context->table_version) {
    // PLAN_CACHE_PORT note that this is not covered in the test (even in the
    // original patch in .22): if version changed, statement repreparation
    // occurred which invalidated the cached plan immediately, so we do not come
    // here.
    trace.add("used", false);
    trace.add_utf8("cause", "DDL was done on cached table");
    return true;
  }
  if (context->is_environment_changed(thd)) {
    trace.add("used", false);
    trace.add_utf8("cause", "Optimizer_switch or character set was changed");
    return true;
  }
  if (context->is_table_stats_changed_sharply(
          tl->table->file->stats.records,
          thd->variables.rds_plan_cache_allow_change_ratio)) {
    trace.add("used", false);
    trace.add("cached table records", context->table_records);
    trace.add("current table records", tl->table->file->stats.records);
    trace.add_utf8("cause", "Number of table records was changed sharply");
    return true;
  }

  if (apply_cached_plan(cached_plan)) {
    trace.add("used", false);
    trace.add_utf8("cause", "Error happened during apply cached plan");
    return true;
  }

  thd->lock_query_plan();
  query_block->join = cached_plan;
  thd->unlock_query_plan();

  cached_plan->set_cached_plan_hit();
  error_guard.release();
  return false;
}

static bool refix_table(JOIN *join, TABLE *table) {
  table->covering_keys = join->plan_cache_exec_context->covering_keys;
  // PLAN_CACHE_PORT: key difference between original and new impl: when
  // applying the plan, we do not clone any Item: clones, if they are needed,
  // were already made during plan caching, and they were made long-lived.
  Item *cond;
  get_item_and_refresh(cond, join->plan_cache_exec_context->pushed_idx_cond,
                       true);
  if (cond != nullptr) {
    auto keyno = join->plan_cache_exec_context->pushed_idx_cond_keyno;
    auto not_pushed [[maybe_unused]] = table->file->idx_cond_push(keyno, cond);
    // Verify that the engine accepted our request
    assert(not_pushed == nullptr &&
           keyno == table->file->pushed_idx_cond_keyno &&
           cond == table->file->pushed_idx_cond);
  } else {
    table->file->cancel_pushed_idx_cond();
  }

  if (join->plan_cache_exec_context->key_read) table->set_keyread(true);

  return false;
}

/**
  Entrance to take use of cached plan.

  @param sl Pointer of current query block.

  This function will check whether cached plan can be used any more. If does,
  cached plan will be restored and apply it to current query block.

  @retval True means some error happens. Optimizer will re-optimize currennt
  qury. Otherwise, False means cached plan applies.
*/
bool exec_cached_plan(Query_block *sl) {
  if (!sl) return true;
  THD *thd = sl->parent_lex->thd;
  // user asked to use cached plans or plan is not cached
  if (!thd->variables.rds_plan_cache || !is_ready(sl)) return true;
  Opt_trace_context *const trace = &thd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object trace_plan_cache(trace, "apply_cached_plan");
  // LIMIT is required for optimization
  if (sl->master_query_expression()->set_limit(thd, sl)) return true;

  if (apply_cached_plan_if_suitable(thd, sl, trace_plan_cache)) return true;

  // From now on, the cached plan is registered in sl->join, we must be
  // careful in error paths.

  // Execute some final steps of optimization.
  assert(sl->plan_cache_state == plan_cache_state::READY);
  JOIN *join = sl->join;
  Table_ref *leaf_tables = sl->leaf_tables;
  uint leaf_table_count = sl->leaf_table_count;
  int status = 0;
  // Now we only support query with one table
  assert(leaf_table_count == 1);

  auto hints_table_saved = leaf_tables->opt_hints_table;

  auto error_guard = create_scope_guard([&trace_plan_cache, sl, thd, join,
                                         leaf_tables, hints_table_saved]() {
    trace_plan_cache.add("used", false);
    trace_plan_cache.add_utf8("cause",
                              "Error happened during apply cached plan");
    // cancel what refix_table() has done on the table, to return the table to
    // a state similar to what is expected when JOIN::optimize() starts
    auto table = leaf_tables->table;
    table->covering_keys = leaf_tables->get_covering_keys_saved();
    table->file->cancel_pushed_idx_cond();
    table->set_keyread(false);
    // Cancel the hint which was added for the range optimizer
    leaf_tables->opt_hints_table = hints_table_saved;
    thd->rollback_item_tree_changes();  // if failure after apply_cached_plan().
    // It was ready earlier in this function (see some assertion above), and
    // invalidate_cached_plan() has not been called yet:
    assert(is_ready(sl));
    join->destroy();
    // PLAN_CACHE_PORT this is to replace other invalidations scattered in
    // callees. It makes sense to say: if it failed to apply, no matter why,
    // invalidate it.
    invalidate_cached_plan(sl);
  });

  const bool has_windows = join->m_windows.elements != 0;
  if (has_windows && Window::setup_windows2(thd, &sl->join->m_windows))
    return true;

  if (join->alloc_indirection_slices()) return true;
  // The base ref items from query block are assigned as JOIN's ref items
  join->ref_items[REF_SLICE_ACTIVE] = sl->base_ref_items;

  for (uint i = 0; i < leaf_table_count; ++i) {
    QEP_TAB *qep_tab = &join->qep_tab[i];
    TABLE *table = leaf_tables->table;

    assert(table);
    Key_use *keyuse = qep_tab->position()->key;
    if (keyuse) keyuse->table_ref = leaf_tables;
    if (refix_table(join, table)) return true;
    // If multiple tables, map2qep_tab should be used to set correct
    // table_ref for qep_tab.
    qep_tab->set_table(table);
    if (join->plan_cache_exec_context->quick_type != -1) {
      // reinit_qep_tab_properties() will soon call the range optimizer. To
      // ensure that such function will pick the exact same type of range
      // access path as we had in the cached plan, we have modified
      // hint_table_state() and also create a table hint (used by
      // idx_merge_hint_state()). It is unfortunately not respecting the
      // layout of hint classes (it is not "resolved", it has no
      // PT_key_level_hint, etc), due to the complexity of such layout.
      auto opt_hints_table =
          new (thd->mem_root) Opt_hints_table(nullptr, nullptr, thd->mem_root);
      if (opt_hints_table == nullptr) return true;
      opt_hints_table->index_merge.get_key_map()->merge(
          join->plan_cache_exec_context->quick_keys_map);
      leaf_tables->opt_hints_table = opt_hints_table;
    }
    qep_tab->table_ref = leaf_tables;
    auto old_leaf = leaf_tables;
    leaf_tables = leaf_tables->next_leaf;
    status = reinit_qep_tab_properties(qep_tab);
    // The hint is not needed anymore, and is on transient MEM_ROOT, so do not
    // keep a pointer to it.
    old_leaf->opt_hints_table = hints_table_saved;
    if (status > 0) return true;
  }

  // Now handle the other QEP_TABs - that of optimizer-internal tmp tables
  for (uint i = 0; i < join->tables; ++i) {
    QEP_TAB *qep_tab = &join->qep_tab[i];
    if (qep_tab->table_ref == nullptr)
      qep_tab->rerun_constructor_for_tmp_table();
  }

  // PLAN_CACHE_PORT clone_if_transient() does not clone Item_param, then no
  // need to bother below. Item_param-s created by the calls to pq_clone() in
  // the lines above, may be clones of clones (see comment in
  // Item_param::sync_clones() which explains how this happens); the original
  // items (lex->param_list) have got their value updated by
  // insert_params_...(), and so have their clones, here we must update the
  // just-created clones of clones.
  if (join->query_block->partitioned_table_count &&
      join->prune_table_partitions())  // PLAN_CACHE_PORT I changed from "return
                                       // true", sounds safer
    return true;

  join->set_optimized();
  thd->status_var.cached_plan_hits++;
  trace_plan_cache.add("used", true);
  error_guard.release();
  return false;
}

/**
  Reset the state of cached_plan object so that it be reused normally.
*/
static void reset_cached_plan(JOIN *cached_plan) {
  assert(cached_plan);

  cached_plan->zero_result_cause = nullptr;
  // Reset quick() to nullptr because in QEP_shared_owner::qs_cleanup, it only
  // delete quick() and it must be set to nullptr to avoid repeated memory
  // release.
  QEP_TAB *qep_tab = cached_plan->qep_tab;
  if (qep_tab) {
    for (uint i = 0; i < cached_plan->tables; i++) {
      qep_tab[i].set_range_scan(nullptr);
    }
  }
}

/**
  Applied recorded plan properties to restored plan.

  After we cached a plan, some optimized plan information has been cleaned up
  (because it was in some data structures which have been freed or reset). In
  order to restore cached plan, this information needs to be recovered from
  plan cache context. This function helps do these things.
*/
static bool apply_cached_plan(JOIN *join) {
  Exec_context *context = join->plan_cache_exec_context;
  Query_block *sl = join->query_block;
  assert(sl->leaf_table_count == 1);
  bind_fields(context->arena->item_list());
  reset_cached_plan(join);
  // PLAN_CACHE_PORT not needed, we do not clone Item_param anymore
  assert(context);
  if (context->tmp_table_param) {
    // Since cached plan's join->tmp_table_param was freed by MEM_ROOT::Clear
    // for last execution, we need to reinitialize tmp_table_param for each
    // apply cached plan, see cache_plan() for details.
    new (&join->tmp_table_param) Temp_table_param();
    join->tmp_table_param.pq_copy(context->tmp_table_param);
  }

  DBUG_EXECUTE_IF("apply_cached_plan_fail", { return true; });
  join->ref_items = nullptr;
  // Here we need reset lock information.
  join->lock = join->thd->lock;
  join->tmp_tables = 0;
  join->thd->m_current_query_cost = context->current_query_cost;
  // Need reset such a fields otherwise join->fields is reset to slice. The
  // following alloc_indirect_slice will reset slices. This will result in
  // a crash.
  join->fields = &sl->fields;
  join->tables_list = sl->leaf_tables;

  THD *thd = join->thd;
  Clone_plan_RAII cpr(thd);
  auto error_guard = create_scope_guard([thd]() {
    // We must undo the changes we did above, as a normal optimization is now
    // going to start (we do not want it to start on half-modified cached
    // objects).
    thd->rollback_item_tree_changes();
  });

  // Replay the Item tree changes done during optimization (only those
  // affecting permanent items).
  for (auto &c : context->new_change_list) {
    if (*c.first != c.second) {  // skip a no-op
      thd->change_item_tree(c.first, c.second);
    }
    // If it is a generated column created during optimization by
    // get_gc_for_expr(), it got added to read_set then. Later got
    // removed from read_set at start of next execution, by
    // Table_ref::restore_properties(). We need to add it back again.
    auto i_f = dynamic_cast<Item_field *>(c.second);
    if (i_f && i_f->field->is_gcol()) {
      i_f->field->table->mark_column_used(i_f->field, MARK_COLUMNS_READ);
    }
  }

  join->group_list.clean();
  join->order.clean();

  // Need replace with virtual column so that cached plan can find related
  // column.
  // PLAN_CACHE_PORT not needed, because the new impl does not support the
  // case of a substituted virtual column in ORDER BY or GROUP BY (grep for
  // hidden_items_from_optimization in this file for more).
  // substitute_gc(thd, sl, nullptr, sl->group_list.first,
  // sl->order_list.first);
  if (context->distinct_group_list && context->distinct_group_list->order) {
    // PLAN_CACHE_PORT here is a minor bugfix which is not in Taurus: when
    // doing PREPARE EXPLAIN, on second execution the displayed plan sometimes
    // lacked "Using temporary", even though the tmp table was properly
    // created (so it was just a display issue). This was because the 'src' of
    // ORDER_with_src was not copied from 'context' to the JOIN's GROUP and
    // ORDER BY. This bug was visible when running main.ps or cde.ps-dstore.
    ORDER *order = nullptr;
    if (make_group_order_list(thd, sl, &order,
                              context->distinct_group_list->order, false))
      return true;
    join->group_list = *context->distinct_group_list;
    join->group_list.order = order;
  }
  if (context->order_list && context->order_list->order) {
    ORDER *order = nullptr;
    if (make_group_order_list(thd, sl, &order, context->order_list->order,
                              false))
      return true;
    join->order = *context->order_list;
    join->order.order = order;
  }

  for (auto list : {join->group_list.order, join->order.order}) {
    for (auto group = list; group; group = group->next) {
      get_item_and_refresh(*group->item, *group->item, false);
    }
  }

  // JOIN::make_tmp_tables_info() fills these two lists with possibly
  // short-lived Items, for use with tmp tables, so empty them for our fresh
  // start, they will be recreated.
  if (join->sum_funcs) *join->sum_funcs = nullptr;
  join->group_fields_cache.clear();

  // Non-pointer members of JOIN which JOIN:make_tmp_tables_info() changes,
  // need to be restored from their value at the time of caching.
  join->explain_flags = context->explain_flags;
  join->calc_found_rows = context->calc_found_rows;
  join->m_select_limit = context->m_select_limit;
  join->grouped = context->grouped;
  join->streaming_aggregation = context->streaming_aggregation;
  join->select_distinct = context->select_distinct;
  join->best_read = context->best_read;

  // PLAN_CACHE_PORT Two things:
  // - no cloning is needed here: when we built the cached plan, we already
  // cloned what was needed, now we can just use that, these are all
  // long-lived items.
  // - Item_field-s in these conditions are properly pointing into the TABLE
  // currently used by this execution, because they went through bind_fields()
  // called by Sql_cmd_dml::restore_cmd_properties() when this execution
  // started.
  get_item_and_refresh(join->where_cond, context->where_cond, true);
  get_item_and_refresh(join->having_cond, context->having_cond, true);

  set_ready(sl);
  count_field_types(sl, &join->tmp_table_param, *join->fields, false, false);
  error_guard.release();
  return false;
}

bool Exec_context::is_environment_changed(THD *thd) {
  return ((thd->variables.optimizer_switch &
           interested_optimizer_switch_flags) ^
          optimizer_switch) ||
         (character_set_client != thd->variables.character_set_client);
}

Exec_context::~Exec_context() {
  if (tmp_table_param) tmp_table_param->cleanup();
  destroy(tmp_table_param);
  destroy(distinct_group_list);
  destroy(order_list);
}

void collect_item_params(Item *item, std::set<Item *> &params) {
  assert(current_thd);
  if (!current_thd->variables.rds_plan_cache) return;

  bool save_walk_const_item = current_thd->walk_const_item;
  current_thd->walk_const_item = true;
  WalkItem(item, enum_walk::POSTFIX, [&params](Item *sub_item) {
    if (sub_item->type() == Item::PARAM_ITEM) {
      params.insert(sub_item);
    }
    return false;
  });
  current_thd->walk_const_item = save_walk_const_item;
}

/**
  After executing reduce_cond_for_table or remove_eq_conds function,
  some conditon might be reduced. If any Item_param is optimized away,
  plan cache is not used. This function Imagine there was "WHERE ?=1",
  and '?' is 1: WHERE is removed. This plan, without any WHERE, would be
  unusable for a next execution with '?' being 2.
  No need to compare types (as in plan_cache::has_same_type_and_item_params),
  because condition optimization didn't change them.
*/
void cmp_item_params_after_reduce_cond(THD *thd,
                                       const std::set<Item *> &old_params,
                                       Item *condition) {
  if (!thd->variables.rds_plan_cache || old_params.empty()) return;

  if (!condition)
    plan_cache::set_uncacheable(thd->lex->current_query_block());
  else {
    std::set<Item *> params_new;
    plan_cache::collect_item_params(condition, params_new);
    if (old_params != params_new)
      plan_cache::set_uncacheable(thd->lex->current_query_block());
  }
}

/// This function will check whether Item type has changed during optimizaion.
/// e.g col_int <= ? might be transformed into col_int = ? . Pessimistically,
/// such transformation may be dependent on the value of some Item_param
/// inside it (like if '?' is INT_MIN, in the example above).
static bool is_item_type_transformed(Item *old_val, Item *new_val) {
  // Here we need compare actual Item cached by Item_cache.
  if (old_val->type() == Item::CACHE_ITEM)
    old_val = down_cast<Item_cache *>(old_val)->get_example();
  if (new_val->type() == Item::CACHE_ITEM)
    new_val = down_cast<Item_cache *>(new_val)->get_example();

  if (old_val->type() != new_val->type()) return true;

  if (old_val->type() == Item::FUNC_ITEM &&
      down_cast<Item_func *>(old_val)->functype() !=
          down_cast<Item_func *>(new_val)->functype())
    return true;

  return false;
}

/// Check whether Item_param objects in two Item objects are the same. In
/// order to make sure Item_param objects are not optimized away or
/// transformed, we need such a function. It also verifies that the item's
/// type didn't change.
static bool has_same_type_and_item_params(Item *old_val, Item *new_val) {
  std::set<Item *> old_item_params;
  collect_item_params(old_val, old_item_params);
  // This item is not what we are interested in. We only care Item with
  // Item_param object.
  if (old_item_params.empty()) return true;
  // Here we need to compare if Item type has been transformed.
  if (is_item_type_transformed(old_val, new_val)) return false;
  // Check whether actual Item_params are the same.
  std::set<Item *> new_item_params;
  collect_item_params(new_val, new_item_params);
  return (old_item_params == new_item_params);
}

/// @Returns true if we are in the process of cloning objects for the purpose
/// of plan caching. pq_clone() functions use this to vary their behaviour
/// depending on if they're called for PQ or plan caching. Use this as little
/// as possible.
bool is_clone_for_plan_cache(const Query_block *sl) {
  return sl->plan_cache_state != plan_cache_state::NONE &&
         sl->plan_cache_state != plan_cache_state::UNCACHEABLE;
}
}  // namespace plan_cache
