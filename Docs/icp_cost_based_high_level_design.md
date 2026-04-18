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

## Observability

With `icp_cost_based=on`, optimizer trace includes:

- `icp_cost_based`
- `icp_cost_keyno`
- `icp_rows_fetched`
- `icp_filter_effect`
- `icp_cost_if_disabled`
- `icp_cost_if_enabled`
- `icp_enabled`  -- cost model's own opinion about ICP benefit
- `icp_cost_adjustment` / `icp_adjusted_read_cost` (only when the ON path is
  taken and a cost reward has been applied)
- `icp_fallback_to_default` (only when the model could not show a benefit
  and the planner deliberately falls back to the community default path)

With `icp_cost_based=off`, none of these fields appear.

In `push_index_cond()` the trace still emits `not_pushed_due_to_icp_cost`
when a persisted OFF decision suppresses a pushdown. Because the current
planner never writes OFF decisions, this token does not appear in practice
today; its absence is used as a regression assertion in the MTR tests.

## Validation

Use `main.icp_cost_based` to verify:

- OFF path: legacy behavior (new fields absent, plan matches community
  baseline).
- ON path / ref access: cost-based fields present and the chosen plan still
  carries `with index condition: ...` in `EXPLAIN FORMAT=TREE`.
- ON path / range scan (issue-2 regression guard): `FORCE INDEX` + leading
  key range + equality on a trailing index column must still produce
  `Index range scan on ... with index condition: ...`. The assertion
  `icp_suppressed_by_cost_model = 0` guards against the planner silently
  disabling ICP for this shape.

Also keep the baseline `main.1st` green, plus the broader ICP suites
(`innodb_icp`, `innodb_icp_all`, `innodb_icp_none`, `range_icp`,
`func_in_icp`, `null_key_icp_innodb`) to guard against broad regression.
