/* Copyright (c) 2024, 2026, Oracle and/or its affiliates.

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

/**
  @file

  Implementation of the cost-based ICP adjustments declared in
  sql/icp_cost_based.h. See the header and Docs/icp_cost_based_*.md
  for design rationale.
*/

#include "sql/icp_cost_based.h"

#include <algorithm>

#include "my_base.h"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/key.h"
#include "sql/mysqld.h"  // innodb_hton
#include "sql/opt_costmodel.h"
#include "sql/opt_hints.h"
#include "sql/opt_trace.h"
#include "sql/range_optimizer/path_helpers.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_bitmap.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_opt_exec_shared.h"
#include "sql/sql_optimizer.h"  // JOIN
#include "sql/sql_planner.h"    // find_cost_for_ref
#include "sql/sql_select.h"     // JOIN_TAB, POSITION, LOWER_BITS
#include "sql/table.h"

namespace icp_cost_based {

/*
  Keep implementation-only helpers in an anonymous namespace nested inside
  icp_cost_based. The named namespace is the module boundary exposed by
  icp_cost_based.h; the anonymous namespace keeps constants, gate helpers, and
  small cost-model utilities local to this translation unit so other optimizer
  files can only use the documented entry points below.
*/
namespace {

/*
  Hard cap on the magnitude of any single ICP cost adjustment, expressed
  as a fraction of the pre-adjustment baseline (cost_if_no_icp /
  original_cost). Item::get_filtering_effect() can substantially
  under-estimate the surviving row count for composite predicates;
  without a cap, a bad selectivity estimate could wipe out most of a
  path's cost and flip the optimizer to a fundamentally different plan.
  50% leaves room for genuine wins (a well-filtered ICP typically saves
  30%~80% of row lookups) while ensuring no single reward alone can
  dominate the DP.
*/
constexpr double kIcpBenefitCapRatio = 0.5;

/*
  Engine-side ICP predicate evaluation is modeled as cheaper than the
  SQL-layer predicate evaluation it replaces. The factor is calibrated
  against the 30%~80% typical range of real ICP savings.
*/
constexpr double kIcpEvalCpuFactor = 0.25;

enum class GuardedCondState {
  kUnknown,
  kAbsent,
  kPresent,
};

/*
  Clamp an ICP benefit adjustment so that its magnitude does not exceed
  kIcpBenefitCapRatio * cost_if_no_icp. The adjustment is expected to be
  <= 0 (with-ICP cheaper than without). Returns the capped adjustment
  and writes an advisory flag through `capped_out` for optimizer-trace
  use.
*/
double cap_icp_benefit_adjustment(double adjustment, double cost_if_no_icp,
                                  bool *capped_out) {
  if (capped_out != nullptr) *capped_out = false;
  if (!(adjustment < 0.0) || !(cost_if_no_icp > 0.0)) return adjustment;
  const double max_savings = kIcpBenefitCapRatio * cost_if_no_icp;
  if (-adjustment > max_savings) {
    if (capped_out != nullptr) *capped_out = true;
    return -max_savings;
  }
  return adjustment;
}

/*
  Shared pre-check for whether a key may receive a cost-based ICP reward.

  This deliberately mirrors the stable, planning-visible subset of
  QEP_TAB::push_index_cond(). The reward is paid during planning, while the
  actual pushdown is still decided later by push_index_cond(); if this gate is
  looser than push_index_cond(), the optimizer can choose a path that only
  looks cheap because we assumed ICP would happen.

  Not every push_index_cond() check is available at every preview point:
    * ref and final scan/range previews have JOIN_TAB context, so they can
      pass the guarded-condition state;
    * range-candidate preview runs inside the range optimizer before it has a
      JOIN_TAB, so guarded conditions are unknown there;
    * late plan-refinement state such as reversed_access and BKA/BNL cache
      decisions is intentionally not checked here because it can be decided
      after the access-path tournaments.

  Maintenance rule: whenever QEP_TAB::push_index_cond() adds or changes a
  stable legality gate, update this function (or explicitly document why the
  gate is unavailable or too late for planning-time ICP costing).
*/
bool can_consider_icp_cost(THD *thd, TABLE *table, Table_ref *table_ref,
                           uint keyno, GuardedCondState guarded_conds) {
  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ICP_COST_BASED)) return false;

  if (keyno == MAX_KEY) return false;

  if (!(table->file->index_flags(keyno, 0, true) & HA_DO_INDEX_COND_PUSHDOWN))
    return false;

  if (!hint_key_state(thd, table_ref, keyno, ICP_HINT_ENUM,
                      OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN))
    return false;

  if (thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
      thd->lex->sql_command == SQLCOM_DELETE_MULTI)
    return false;

  if (guarded_conds == GuardedCondState::kPresent) return false;

  // Disable ICP for InnoDB intrinsic temp tables, matching push_index_cond().
  if (table->s->db_type() == innodb_hton &&
      table->s->tmp_table != NO_TMP_TABLE &&
      table->s->tmp_table != TRANSACTIONAL_TMP_TABLE)
    return false;

  // Virtual generated columns are not supported for ICP.
  if (table->vfield && table->index_contains_some_virtual_gcol(keyno))
    return false;

  if (keyno == table->s->primary_key && table->file->primary_key_is_clustered())
    return false;

  // If key-only read is possible, make_join_readinfo() will skip ICP anyway.
  if (table->covering_keys.is_set(keyno) && !table->no_keyread) return false;

  return true;
}

bool can_consider(const JOIN_TAB *tab, uint keyno) {
  return can_consider_icp_cost(
      tab->join()->thd, tab->table(), tab->table_ref, keyno,
      tab->has_guarded_conds() ? GuardedCondState::kPresent
                               : GuardedCondState::kAbsent);
}

/*
  Estimate whether ICP should be enabled for the chosen access path.
  The estimate compares:
    - no ICP: row lookup + SQL-layer predicate evaluation on fetched rows
    - with ICP: fewer row lookups + SQL-layer eval on remaining rows +
      engine-side predicate eval on index entries.
*/
bool should_enable_icp_by_cost(const JOIN_TAB *tab, uint keyno,
                               double prefix_rowcount, double rows_fetched,
                               float filter_effect, double *cost_if_no_icp,
                               double *cost_if_with_icp) {
  const JOIN *const join = tab->join();
  const Cost_model_server *const cost_model = join->cost_model();

  const double bounded_filter =
      std::max(0.0, std::min(1.0, static_cast<double>(filter_effect)));
  const double rows_after_filter = rows_fetched * bounded_filter;
  const double filtered_out_rows = rows_fetched - rows_after_filter;

  if (filtered_out_rows <= 0.0) return false;

  const double row_lookup_cost =
      find_cost_for_ref(join->thd, tab->table(), keyno, 1.0, tab->worst_seeks);

  const double no_icp_lookup_cost =
      prefix_rowcount * rows_fetched * row_lookup_cost;
  const double no_icp_eval_cost =
      cost_model->row_evaluate_cost(prefix_rowcount * rows_fetched);

  const double with_icp_lookup_cost =
      prefix_rowcount * rows_after_filter * row_lookup_cost;
  const double with_icp_eval_cost =
      cost_model->row_evaluate_cost(prefix_rowcount * rows_after_filter) +
      cost_model->row_evaluate_cost(prefix_rowcount * rows_fetched *
                                    kIcpEvalCpuFactor);

  *cost_if_no_icp = no_icp_lookup_cost + no_icp_eval_cost;
  *cost_if_with_icp = with_icp_lookup_cost + with_icp_eval_cost;
  return *cost_if_with_icp < *cost_if_no_icp;
}

bool range_preview_gates(THD *thd, TABLE *table, uint keynr) {
  return can_consider_icp_cost(thd, table, table->pos_in_table_list, keynr,
                               GuardedCondState::kUnknown);
}

}  // namespace

void reset_position_decision(POSITION *pos) {
  // Defaults mean "no cost-based ICP override"; push_index_cond() decides.
  pos->icp_decision_made = false;
  pos->use_cost_based_icp = true;
  pos->icp_keyno = MAX_KEY;
  pos->cost_if_no_icp = 0.0;
  pos->cost_if_with_icp = 0.0;
}

void preview_ref_candidate(const JOIN_TAB *tab, uint keyno,
                           key_part_map found_part, double prefix_rowcount,
                           double cur_fanout,
                           Opt_trace_object *trace_access_idx,
                           double *cur_ref_cost) {
  if (!can_consider(tab, keyno) || cur_fanout <= 0.0) return;

  JOIN *const join = tab->join();
  if (join->where_cond == nullptr) return;

  TABLE *const table = tab->table();
  if (bitmap_is_clear_all(&table->cond_set)) return;

  const KEY *const keyinfo = table->key_info + keyno;
  const bool has_unbound_keyparts =
      found_part != LOWER_BITS(key_part_map, actual_key_parts(keyinfo));
  if (!has_unbound_keyparts) return;

  // Build the set of this key's keyparts (columns on index).
  assert(bitmap_is_clear_all(&table->tmp_set));
  for (uint i = 0; i < actual_key_parts(keyinfo); i++) {
    bitmap_set_bit(&table->tmp_set,
                   keyinfo->key_part[i].field->field_index());
  }

  // Columns bound by the ref condition.
  char refbuf[MAX_FIELDS / 8];
  my_bitmap_map *const refbits =
      static_cast<my_bitmap_map *>(static_cast<void *>(&refbuf));
  MY_BITMAP ref_bound_cols;
  bitmap_init(&ref_bound_cols, refbits, table->s->fields);
  for (uint kp = 0; kp < actual_key_parts(keyinfo); kp++) {
    if (found_part & (key_part_map{1} << kp)) {
      bitmap_set_bit(&ref_bound_cols,
                     keyinfo->key_part[kp].field->field_index());
    }
  }

  char rembuf[MAX_FIELDS / 8];
  my_bitmap_map *const rembits =
      static_cast<my_bitmap_map *>(static_cast<void *>(&rembuf));
  MY_BITMAP remaining_cond_cols;
  bitmap_init(&remaining_cond_cols, rembits, table->s->fields);
  bitmap_copy(&remaining_cond_cols, &table->cond_set);
  bitmap_subtract(&remaining_cond_cols, &ref_bound_cols);

  const bool remaining_on_index =
      !bitmap_is_clear_all(&remaining_cond_cols) &&
      bitmap_is_subset(&remaining_cond_cols, &table->tmp_set);

  if (remaining_on_index) {
    // Columns already consumed by the ref key must be excluded from
    // get_filtering_effect() to avoid double-counting them.
    const float remaining_filter = join->where_cond->get_filtering_effect(
        join->thd, tab->table_ref->map(),
        /*read_tables=*/0, &ref_bound_cols,
        static_cast<double>(tab->records()));

    if (remaining_filter > 0.0f && remaining_filter < 1.0f) {
      double ref_cost_if_no_icp = 0.0;
      double ref_cost_if_with_icp = 0.0;
      const bool icp_helps = should_enable_icp_by_cost(
          tab, keyno, prefix_rowcount, cur_fanout, remaining_filter,
          &ref_cost_if_no_icp, &ref_cost_if_with_icp);

      trace_access_idx->add("icp_cost_based", true);
      trace_access_idx->add("icp_rows_fetched", cur_fanout);
      trace_access_idx->add("icp_filter_effect", remaining_filter);
      trace_access_idx->add("icp_cost_if_disabled", ref_cost_if_no_icp);
      trace_access_idx->add("icp_cost_if_enabled", ref_cost_if_with_icp);
      trace_access_idx->add("icp_enabled", icp_helps);

      if (icp_helps) {
        bool icp_benefit_capped = false;
        const double icp_benefit_adjustment = cap_icp_benefit_adjustment(
            ref_cost_if_with_icp - ref_cost_if_no_icp, ref_cost_if_no_icp,
            &icp_benefit_capped);
        *cur_ref_cost += icp_benefit_adjustment;
        trace_access_idx->add("icp_cost_adjustment", icp_benefit_adjustment);
        if (icp_benefit_capped)
          trace_access_idx->add("icp_cost_adjustment_capped", true);
        trace_access_idx->add("icp_adjusted_ref_cost", *cur_ref_cost);
      }
    }
  }
  bitmap_clear_all(&table->tmp_set);
}

void preview_scan_or_range(const JOIN_TAB *tab, Key_use *best_ref,
                           POSITION *pos, double prefix_rowcount,
                           double rows_fetched, float filter_effect,
                           Opt_trace_object *trace_access_scan,
                           double *best_read_cost) {
  /*
    Ensure pos's ICP fields end up in a well-defined state no matter
    which branch below the function exits through. This mirrors the
    pre-refactor behavior in best_access_path() where the local
    `icp_decision_made/use_cost_based_icp/cost_if_*` defaults were
    unconditionally written back to pos.
  */
  reset_position_decision(pos);

  /*
    Determine the tentative keyno for the access method that
    best_access_path() has picked. A ref win is signaled by @a best_ref
    being non-null; otherwise a range pick is detected via
    tab->range_scan() with JT_RANGE.
  */
  uint icp_keyno = MAX_KEY;
  const bool ref_chosen = (best_ref != nullptr);
  if (ref_chosen) {
    icp_keyno = best_ref->key;
  } else if (tab->range_scan() != nullptr &&
             calc_join_type(tab->range_scan()) == JT_RANGE) {
    icp_keyno = used_index(tab->range_scan());
  }

  pos->icp_keyno = icp_keyno;

  if (!can_consider(tab, icp_keyno)) return;

  // Build a reliable baseline for ICP benefit estimation.
  double icp_rows_fetched = rows_fetched;
  float icp_filter_effect = filter_effect;
  const bool range_scan_chosen = (!ref_chosen && tab->range_scan() != nullptr);
  if (range_scan_chosen && tab->found_records > 0 &&
      rows_fetched < static_cast<double>(tab->found_records)) {
    // `rows_fetched` here was set to `rows_after_filtering`, i.e. the
    // post-WHERE estimate. ICP actually operates on the raw rows
    // produced by the index scan, so use `tab->found_records`
    // (index-output rows) as the baseline and derive an index-level
    // filter effect.
    icp_rows_fetched = static_cast<double>(tab->found_records);
    icp_filter_effect =
        static_cast<float>(std::min(1.0, rows_fetched / icp_rows_fetched));
  }

  double cost_if_no_icp = 0.0;
  double cost_if_with_icp = 0.0;
  const bool icp_favored_by_cost = should_enable_icp_by_cost(
      tab, icp_keyno, prefix_rowcount, icp_rows_fetched, icp_filter_effect,
      &cost_if_no_icp, &cost_if_with_icp);

  trace_access_scan->add("icp_cost_based", true);
  trace_access_scan->add("icp_cost_keyno", icp_keyno);
  trace_access_scan->add("icp_rows_fetched", icp_rows_fetched);
  trace_access_scan->add("icp_filter_effect", icp_filter_effect);
  trace_access_scan->add("icp_cost_if_disabled", cost_if_no_icp);
  trace_access_scan->add("icp_cost_if_enabled", cost_if_with_icp);
  trace_access_scan->add("icp_enabled", icp_favored_by_cost);

  if (icp_favored_by_cost) {
    /*
      Feed the estimated ICP benefit back into access-path cost so
      join order planning can prefer plans where ICP is expected to
      help. The adjustment is hard-capped at kIcpBenefitCapRatio of
      the no-ICP baseline to guard against selectivity mis-estimation.
    */
    pos->icp_decision_made = true;
    pos->use_cost_based_icp = true;
    pos->cost_if_no_icp = cost_if_no_icp;
    pos->cost_if_with_icp = cost_if_with_icp;

    bool icp_benefit_capped = false;
    const double icp_benefit_adjustment = cap_icp_benefit_adjustment(
        cost_if_with_icp - cost_if_no_icp, cost_if_no_icp,
        &icp_benefit_capped);
    *best_read_cost += icp_benefit_adjustment;
    trace_access_scan->add("icp_cost_adjustment", icp_benefit_adjustment);
    if (icp_benefit_capped)
      trace_access_scan->add("icp_cost_adjustment_capped", true);
    trace_access_scan->add("icp_adjusted_read_cost", *best_read_cost);
  } else {
    /*
      Do NOT record an OFF decision. The cost model is not yet reliable
      enough to safely override the community default ICP pushdown.
      Falling back here avoids regressions like the FORCE INDEX + range
      scan case where `filter_effect` is estimated as ~1.0 by
      construction.
    */
    trace_access_scan->add("icp_fallback_to_default", true);
  }
}

double preview_range_candidate(THD *thd, TABLE *table, uint keynr,
                               Item *where_cond, ha_rows found_records,
                               double original_cost,
                               Opt_trace_object *trace_idx) {
  if (where_cond == nullptr) return original_cost;
  if (found_records == 0 || found_records == HA_POS_ERROR)
    return original_cost;
  if (!range_preview_gates(thd, table, keynr)) return original_cost;

  const KEY &key_info = table->key_info[keynr];
  const uint total_keyparts = key_info.user_defined_key_parts;

  // The range optimizer already told us how many leading keyparts the
  // scan exercises.
  const uint bound_keyparts = table->quick_key_parts[keynr];
  if (bound_keyparts == 0 || bound_keyparts >= total_keyparts)
    return original_cost;

  // Build this key's full column set (for subset check) and the
  // "bound by range" column set (used as ignore set for selectivity).
  char keybuf[MAX_FIELDS / 8];
  my_bitmap_map *const kbits =
      static_cast<my_bitmap_map *>(static_cast<void *>(&keybuf));
  MY_BITMAP key_cols;
  bitmap_init(&key_cols, kbits, table->s->fields);
  for (uint i = 0; i < total_keyparts; i++) {
    bitmap_set_bit(&key_cols, key_info.key_part[i].field->field_index());
  }

  char rbndbuf[MAX_FIELDS / 8];
  my_bitmap_map *const rbndbits =
      static_cast<my_bitmap_map *>(static_cast<void *>(&rbndbuf));
  MY_BITMAP range_bound_cols;
  bitmap_init(&range_bound_cols, rbndbits, table->s->fields);
  for (uint i = 0; i < bound_keyparts; i++) {
    bitmap_set_bit(&range_bound_cols,
                   key_info.key_part[i].field->field_index());
  }

  // Collect WHERE columns for `table`. `add_field_to_cond_set_processor`
  // writes into `field->table->cond_set`, which is exactly the semantic
  // we want. At this point in optimization the planner has not yet
  // filled cond_set (that happens in choose_table_order() right before
  // best_access_path); the planner also unconditionally clears and
  // re-walks cond_set there, so writing into it here is safe. We
  // restore the original contents on function exit to avoid leaking
  // bits into other tables' cond_set (the processor only touches this
  // table's cond_set, so the restore is trivially scoped).
  const bool cond_set_was_empty = bitmap_is_clear_all(&table->cond_set);
  if (cond_set_was_empty) {
    where_cond->walk(&Item::add_field_to_cond_set_processor,
                     enum_walk::POSTFIX, nullptr);
  }

  auto cond_set_guard = [&]() {
    if (cond_set_was_empty) bitmap_clear_all(&table->cond_set);
  };

  // "Remaining WHERE columns" must live on this key's trailing
  // keyparts. We also require non-empty remaining (otherwise ICP has
  // nothing to filter).
  char rembuf[MAX_FIELDS / 8];
  my_bitmap_map *const rembits =
      static_cast<my_bitmap_map *>(static_cast<void *>(&rembuf));
  MY_BITMAP remaining_cond_cols;
  bitmap_init(&remaining_cond_cols, rembits, table->s->fields);
  bitmap_copy(&remaining_cond_cols, &table->cond_set);
  bitmap_subtract(&remaining_cond_cols, &range_bound_cols);

  if (bitmap_is_clear_all(&remaining_cond_cols)) {
    cond_set_guard();
    return original_cost;
  }
  if (!bitmap_is_subset(&remaining_cond_cols, &key_cols)) {
    cond_set_guard();
    return original_cost;
  }

  // Selectivity of predicates not bound by the range.
  const double full_rows = static_cast<double>(found_records);
  const float remaining_filter = where_cond->get_filtering_effect(
      thd, table->pos_in_table_list->map(), /*read_tables=*/0,
      &range_bound_cols, full_rows);
  if (!(remaining_filter > 0.0f) || !(remaining_filter < 1.0f)) {
    cond_set_guard();
    return original_cost;
  }

  // Cost model (mirrors should_enable_icp_by_cost() at the range level):
  //   - without ICP: pay original_cost to fetch all `found_records` rows.
  //   - with ICP:    pay original_cost * filter to fetch the survivors,
  //                  plus a per-index-entry eval overhead.
  const Cost_model_server *const cost_model = thd->cost_model();
  const double cost_if_no_icp = original_cost;
  const double cost_if_with_icp =
      original_cost * static_cast<double>(remaining_filter) +
      cost_model->row_evaluate_cost(full_rows * kIcpEvalCpuFactor);

  if (!(cost_if_with_icp < cost_if_no_icp)) {
    cond_set_guard();
    return original_cost;
  }

  /*
    Hard-cap the range-level ICP reward: the effective_cost cannot drop
    below (1 - kIcpBenefitCapRatio) * original_cost. Mirrors the
    best_access_path()/find_best_ref() cap so that get_filtering_effect()
    mis-estimates cannot single-handedly reshape range-scan selection.
  */
  const double min_effective_cost = (1.0 - kIcpBenefitCapRatio) * original_cost;
  double effective_cost = cost_if_with_icp;
  bool icp_capped = false;
  if (effective_cost < min_effective_cost) {
    effective_cost = min_effective_cost;
    icp_capped = true;
  }

  trace_idx->add("icp_cost_based", true);
  trace_idx->add("icp_rows_fetched", full_rows);
  trace_idx->add("icp_filter_effect", remaining_filter);
  trace_idx->add("icp_cost_if_disabled", cost_if_no_icp);
  trace_idx->add("icp_cost_if_enabled", cost_if_with_icp);
  trace_idx->add("icp_enabled", true);
  trace_idx->add("icp_cost_adjustment", effective_cost - cost_if_no_icp);
  if (icp_capped) trace_idx->add("icp_cost_adjustment_capped", true);
  trace_idx->add("icp_adjusted_range_cost", effective_cost);

  cond_set_guard();
  return effective_cost;
}

}  // namespace icp_cost_based
