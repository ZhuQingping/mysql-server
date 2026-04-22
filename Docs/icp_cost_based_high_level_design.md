# Cost-based ICP High-level Design

> Chinese translation: [icp_cost_based_high_level_design.zh_CN.md](icp_cost_based_high_level_design.zh_CN.md)

## Background

In community MySQL behavior, ICP (Index Condition Pushdown) is primarily
decided late during plan refinement (`QEP_TAB::push_index_cond()`), not during
join/access-path planning. This may miss cases where a plan that can use ICP
would be faster than a plan that cannot.

## Goal

Enable optimizer to consider ICP benefit during planning, while keeping safety
and rollback guarantees:

- Improve plan quality when ICP-capable paths are genuinely cheaper.
- Avoid performance regression by using conservative cost checks.
- Keep runtime rollback via `optimizer_switch`.

## Design Principles

- Default behavior must remain compatible with community path.
- New behavior must be explicitly controlled by a switch.
- Execution-stage safety checks remain in place.
- **Strict improvement**: when the cost model cannot positively show that ICP
  helps, fall back to the community default pushdown. The feature may only
  reward beneficial plans; it must never silently disable a pushdown that
  the community code would have performed.

## Feature Gate

Add `optimizer_switch` option:

- `icp_cost_based=off` (default): fallback to legacy behavior.
- `icp_cost_based=on`: enable cost-based ICP decision in planner.

## Execution Flow

0. In `find_best_ref()`, **before** the access-method tournament in
   `best_access_path()` picks a ref candidate, preview each ref candidate's
   ICP benefit and feed it back into the candidate's own cost so the
   narrowest index no longer wins by accident (**issue-1 ref form**; see
   the "Cost-based ICP for ref candidates" section below).

1. In `best_access_path()`:
   - determine candidate key (`ref` key or chosen range key),
   - run eligibility prechecks,
   - build an ICP benefit baseline (see note below) and estimate
     `cost_if_disabled` vs `cost_if_enabled`,
   - if ICP is beneficial, persist an ON decision in `POSITION` and
     subtract the estimated benefit from `best_read_cost` so planning can
     prefer the better path;
   - if ICP is not shown to be beneficial, leave no decision in `POSITION`
     so that downstream stages fall back to community default pushdown.

   > **Baseline note.** For a range-scan-chosen path, the `rows_fetched` /
   > `filter_effect` values that the rest of `best_access_path()` uses are
   > already post-WHERE estimates (`rows_fetched = rows_after_filtering`,
   > `filter_effect ≈ 1.0`). Feeding those directly to the ICP model would
   > make `filtered_out_rows == 0` and the decision always "not beneficial",
   > even when ICP would save most of the row lookups. The model therefore
   > rebuilds an index-scan-output baseline from `tab->found_records` for
   > range-scan-chosen paths.

2. In `get_best_combination()`:
   - copy decision from `POSITION` into `JOIN_TAB`.

3. In `push_index_cond()`:
   - if a decision exists for this key, honor it;
   - otherwise (including the "no decision written" case above), preserve
     the legacy community ICP flow.

   Today the planner only ever writes ON decisions, so the "decision exists
   and is OFF" branch is effectively dormant. The branch is kept in the
   code so a future, better-calibrated cost model can safely opt in to
   cost-based OFF behavior without further refactoring.

## Cost-based ICP for ref candidates (issue-1 ref form)

**Motivation.** When several indexes share the same leading keypart
(e.g. `idx_a(a)`, `idx_ab(a, b)`, `idx_abc(a, b, c)`), the classic ref
cost loop in `find_best_ref()` compares candidates only by fanout and
index width. For `WHERE a = 4 AND c = 6` the three indexes all produce
the same fanout on `a = 4`, so the narrowest one (`idx_a`) wins --
losing the ICP filtering that only `idx_abc` could have performed on
`c = 6`. The `icp_cost_based=on` gate in step 1 above does not help,
because it runs *after* the ref winner has already been picked.

**Design.** Inside the per-candidate loop of `find_best_ref()`, after
`cur_ref_cost` has been computed but before it enters the tournament
comparison, preview the ICP benefit for this specific candidate:

- skip when `icp_cost_based=off` or `can_consider_icp_cost()` refuses;
- skip when the key has no unbound keyparts left (nothing for ICP to
  absorb);
- compute the set of WHERE-columns that are *not* bound by this ref;
  skip when that set is not a subset of the candidate's keyparts
  (columns not on this index cannot be pushed);
- otherwise estimate the remaining filter effect via
  `Item::get_filtering_effect()` on everything except the already-bound
  ref columns, call `should_enable_icp_by_cost()` with
  (`rows_fetched = cur_fanout`, `filter_effect = remaining_filter`),
  and on a positive verdict subtract
  `cost_if_with_icp − cost_if_no_icp` (a negative value) from
  `cur_ref_cost`.

**Safety.** The preview can only *reduce* a candidate's cost, never
raise it. When any gate fails, `cur_ref_cost` stays identical to the
legacy value, so ref candidates that cannot benefit from ICP remain
bit-for-bit compatible with community behavior. `idx_a` / `idx_ab`
for the motivating shape fall into this "unchanged" path automatically
because `{c}` is not a subset of their keyparts.

**Scope.** This mechanism only lifts the ref-form subtree of issue 1.
The range-form subtree (Case 5 in `main.icp_cost_based`) is still a
known limitation and is tracked as a follow-up.

## Observability

With `icp_cost_based=on`, optimizer trace includes:

- Scan-path decision (from `best_access_path()`):
  - `icp_cost_based`
  - `icp_cost_keyno`
  - `icp_rows_fetched`
  - `icp_filter_effect`
  - `icp_cost_if_disabled`
  - `icp_cost_if_enabled`
  - `icp_enabled`  -- cost model's own opinion about ICP benefit
  - `icp_cost_adjustment` / `icp_adjusted_read_cost` (only when the ON
    path is taken and a cost reward has been applied)
  - `icp_fallback_to_default` (only when the model could not show a
    benefit and the planner deliberately falls back to community default)
- Ref-tournament preview (from `find_best_ref()`, per candidate key):
  - `icp_cost_based`
  - `icp_rows_fetched`
  - `icp_filter_effect`
  - `icp_cost_if_disabled`
  - `icp_cost_if_enabled`
  - `icp_enabled`
  - `icp_cost_adjustment` / `icp_adjusted_ref_cost` (only when the
    candidate received a cost reward)

With `icp_cost_based=off`, none of these fields appear. A ref candidate
that does not pass the preview gates (e.g. no unbound keyparts, or
remaining WHERE columns not on this index) simply does not emit the
per-candidate block.

In `push_index_cond()` the trace still emits `not_pushed_due_to_icp_cost`
when a persisted OFF decision suppresses a pushdown. Because the current
planner never writes OFF decisions, this token does not appear in practice
today; its absence is used as a regression assertion in the MTR tests.

## Validation

Use `main.icp_cost_based` to verify:

- **Case 1** -- OFF path, FORCE INDEX + ref: legacy behavior (new fields
  absent, plan matches community baseline).
- **Case 2** -- ON path, FORCE INDEX + ref: cost-based fields present and
  the chosen plan still carries `with index condition: ...` in
  `EXPLAIN FORMAT=TREE`.
- **Case 3** (issue-2 regression) -- ON path, FORCE INDEX + leading-key
  range + equality on a trailing index column: must still produce
  `Index range scan on ... with index condition: ...`. The assertion
  `icp_suppressed_by_cost_model = 0` guards against silent pushdown
  disabling on this shape.
- **Case 4** (issue-1 ref form) -- ON path, no hint, three indexes
  sharing the leading key `a`: the plan must switch from
  `Filter(c=6) + Index lookup on idx_a` (OFF baseline) to
  `Index lookup on idx_abc with index condition: (tt.c = 6)`. A
  `FORCE INDEX(idx_abc)` sanity probe documents that the execution
  layer has always supported this plan; the fix lives purely in
  access-method selection.
- **Case 4a** (feature-gate guardrail) -- same query, flipping
  `icp_cost_based` back to `off` must restore the legacy plan; this
  protects against accidental ON-by-default changes or stuck
  optimizer state.
- **Case 4b** (full-ref keyparts) -- `WHERE a = 4 AND b = 5 AND c = 6`:
  every keypart of `idx_abc` is bound by ref, so no unbound WHERE
  column remains; the preview gate must skip the reward and the plan
  must not switch to `idx_abc` merely because of the feature flag.
- **Case 4c** (remaining WHERE not on the index) -- `WHERE a = 4 AND
  d = 7`: column `d` is not a keypart of any index; the subset check
  must fail for all candidates so OFF and ON produce identical plans.
- **Case 5** (issue-1 range form, KNOWN LIMITATION) -- ON path, no
  hint, leading-key range + trailing-key equality: currently still
  falls back to a table scan (same as OFF). Deliberately kept as a
  red baseline so the follow-up range-form fix surfaces as a
  `.result` diff.

Also keep the baseline `main.1st` green, plus the broader ICP suites
(`innodb_icp`, `innodb_icp_all`, `innodb_icp_none`, `range_icp`,
`func_in_icp`, `null_key_icp_innodb`) to guard against broad regression.
