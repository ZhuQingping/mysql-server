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

#ifndef SQL_ICP_COST_BASED_INCLUDED
#define SQL_ICP_COST_BASED_INCLUDED

/**
  @file icp_cost_based.h

  Cost-based Index Condition Pushdown (ICP) adjustments for the classic
  optimizer.

  Responsibilities centralized here:
    * The "ICP benefit hard cap" rule that bounds the magnitude of any
      single ICP-driven cost adjustment, guarding against inaccurate
      Item::get_filtering_effect() estimates.
    * Ref-access tournament reward when a wider index can push a
      surviving WHERE predicate into the engine.
    * Scan/range finalization of the cost-based ICP decision, persisted
      into POSITION (and later copied to JOIN_TAB).
    * Range-optimizer preview so a range candidate's ICP benefit can
      participate in its internal tournament against the table-scan
      baseline.
    * Suppression of the classic pushdown path when a cost-based OFF
      decision has been made.

  Core optimizer files should stay independent of this module's
  internals and only hold thin call sites into the entry points below.
  Design rationale and caller placement are documented in
  Docs/icp_cost_based_*.md.
*/

#include "my_base.h"  // key_part_map, ha_rows
#include "my_inttypes.h"

class Item;
class JOIN_TAB;
class Key_use;
class Opt_trace_object;
class THD;
struct POSITION;
struct TABLE;

namespace icp_cost_based {

/**
  Reset the cost-based ICP decision fields of @a pos to their defaults
  ("no decision"). Called wherever the optimizer (re-)initializes a
  POSITION that must start without any ICP override.
*/
void reset_position_decision(POSITION *pos);

/**
  Let the current ref candidate reflect its expected ICP benefit in the
  ref-access tournament cost. Safe no-op when the feature is off, when
  the ref candidate cannot push any predicate, or when get_filtering_effect()
  cannot produce a usable estimate.

  Any benefit is hard-capped: the adjustment magnitude may not exceed
  50% of the pre-adjustment baseline, guarding against get_filtering_effect()
  under-estimates.

  @param tab                       Join table whose ref candidates are
                                   being evaluated.
  @param keyno                     Candidate index.
  @param found_part                Bitmap of keyparts bound by the ref
                                   condition.
  @param prefix_rowcount           Expected rowcount of the join prefix.
  @param cur_fanout                Candidate ref access fanout.
  @param[in,out] trace_access_idx  Trace object for this candidate.
  @param[in,out] cur_ref_cost      Candidate cost; may be decreased
                                   (capped) when ICP is expected to help.
*/
void preview_ref_candidate(const JOIN_TAB *tab, uint keyno,
                           key_part_map found_part, double prefix_rowcount,
                           double cur_fanout,
                           Opt_trace_object *trace_access_idx,
                           double *cur_ref_cost);

/**
  Finalize the cost-based ICP decision for the access method that
  best_access_path() has just chosen (ref, range, or full scan) and
  persist it into @a pos. May also reduce @a best_read_cost (subject
  to the hard cap) so the decision feeds back into join-order DP.

  Only persists an ON decision; OFF is deliberately left as "no
  decision" so push_index_cond() can fall back to the classic pushdown.

  @param tab                        Join table being finalized.
  @param pos                        POSITION receiving the decision and
                                    the tentative keyno.
  @param prefix_rowcount            Expected rowcount of the join prefix.
  @param rows_fetched               Rows expected from the chosen access
                                    method (post-WHERE for ranges).
  @param filter_effect              Condition-filter effect already
                                    associated with the chosen access.
  @param[in,out] trace_access_scan  Trace for the scan/range block.
  @param[in,out] best_read_cost     Running best access cost; may be
                                    decreased (capped) on ON.

  Note: the function takes @a best_ref explicitly (rather than reading
  @a pos->key) so that best_access_path() does not have to reorder its
  post-decision pos writes around the ICP helper invocation.
*/
void preview_scan_or_range(const JOIN_TAB *tab, Key_use *best_ref,
                           POSITION *pos, double prefix_rowcount,
                           double rows_fetched, float filter_effect,
                           Opt_trace_object *trace_access_scan,
                           double *best_read_cost);

/**
  Range-optimizer preview. Returns an "effective" cost used only for
  the candidate-vs-baseline tournament in get_key_scans_params(). When
  no preview applies, returns @a original_cost verbatim so the legacy
  comparison path is preserved bit-for-bit. On a successful reward,
  writes trace fields mirroring those produced by preview_scan_or_range()
  so observability stays consistent between the two entry points.
*/
double preview_range_candidate(THD *thd, TABLE *table, uint keynr,
                               Item *where_cond, ha_rows found_records,
                               double original_cost,
                               Opt_trace_object *trace_idx);

/**
  Suppress the classic pushdown path when a cost-based decision has
  concluded "ICP off" for @a keyno. Returns true iff the caller should
  skip its legacy pushdown work. Writes the `not_pushed_due_to_icp_cost`
  trace flag on true.
*/
bool legacy_pushdown_suppressed(const JOIN_TAB *tab, uint keyno,
                                Opt_trace_object *trace_obj);

}  // namespace icp_cost_based

#endif  // SQL_ICP_COST_BASED_INCLUDED
