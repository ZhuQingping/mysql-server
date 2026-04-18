# Cost-based ICP Low-level Design

> Chinese translation: [icp_cost_based_low_level_design.zh_CN.md](icp_cost_based_low_level_design.zh_CN.md)

## 1. Switch and Constants

### 1.1 `sql/sql_const.h`

Add a new optimizer switch bit:

- `OPTIMIZER_SWITCH_ICP_COST_BASED`

and move `OPTIMIZER_SWITCH_LAST` accordingly.

### 1.2 `sql/sys_vars.cc`

Extend `optimizer_switch` metadata:

- add switch name: `icp_cost_based`
- add switch description entry in `Sys_optimizer_switch`

Default is OFF (not in `OPTIMIZER_SWITCH_DEFAULT`).

## 2. Planner-side Cost Decision

File: `sql/sql_planner.cc`

### 2.1 Eligibility precheck

`can_consider_icp_cost(const JOIN_TAB *tab, uint keyno)`:

- requires `optimizer_switch=icp_cost_based` ON
- candidate key must exist (`keyno != MAX_KEY`)
- engine supports `HA_DO_INDEX_COND_PUSHDOWN`
- ICP switch/hints permit pushdown
- not multi-table update/delete
- not guarded-conditions case
- not virtual generated-column index
- not clustered PK
- not key-only-read case

### 2.2 Cost model

`should_enable_icp_by_cost(...)` computes:

- without ICP:
  - all row lookups + SQL predicate eval
- with ICP:
  - fewer row lookups + SQL eval on remaining rows +
  - cheaper engine-side eval proxy (`kIcpEvalCpuFactor`)

Decision:

- enable if `cost_if_with_icp < cost_if_no_icp`
- otherwise return false; the caller interprets this as "no ON decision"
  rather than "persist an OFF decision" (see 2.3 / section 4).

#### 2.2.1 Baseline for range-scan-chosen paths

`best_access_path()` overwrites `rows_fetched` with `rows_after_filtering`
when a range scan wins the scan-vs-ref comparison, and derives
`filter_effect = min(1, found_records * full_filter / rows_after_filtering)`.
On this code path `rows_after_filtering` is itself defined as
`found_records * full_filter` (when `COND_FANOUT_FILTER` is on), so
`filter_effect` collapses to ~1.0 by construction. Feeding those values
into the ICP model yields `filtered_out_rows == 0` and the decision is
permanently "not beneficial", causing a regression versus community
default pushdown.

To avoid that, the caller (section 2.3) rebuilds the baseline before
invoking `should_enable_icp_by_cost`:

- `icp_rows_fetched = tab->found_records` -- raw rows coming out of the
  index scan, which is what ICP actually filters on,
- `icp_filter_effect = min(1, rows_fetched / icp_rows_fetched)` -- an
  index-level filter ratio that is meaningful against the new baseline.

The adjustment only applies when a range scan is the chosen access method
(`best_ref == nullptr && tab->range_scan() != nullptr`) and the rebuilt
baseline is strictly larger than the existing one.

### 2.3 Planning integration

In `best_access_path()`:

- derive candidate key from chosen access path,
- run precheck,
- rebuild ICP baseline for range-scan-chosen paths (2.2.1), then call
  `should_enable_icp_by_cost`,
- emit trace fields (see section 3.3 of the high-level doc):
  `icp_cost_based`, `icp_cost_keyno`, `icp_rows_fetched`,
  `icp_filter_effect`, `icp_cost_if_disabled`, `icp_cost_if_enabled`,
  `icp_enabled`,
- if the model says ICP is beneficial:
  - set `POSITION::icp_decision_made = true`,
    `POSITION::use_cost_based_icp = true`,
    `POSITION::icp_keyno = icp_keyno`,
  - apply cost reward: `best_read_cost += (cost_if_with_icp - cost_if_no_icp)`,
  - emit `icp_cost_adjustment` / `icp_adjusted_read_cost`;
- if the model does not show a benefit:
  - leave `POSITION::icp_decision_made = false`,
  - emit `icp_fallback_to_default = true`,
  - do *not* change `best_read_cost`.

Rationale: the current cost model is not yet reliable enough to safely
override the community default pushdown (see the range-scan baseline
pathology in 2.2.1 and the open calibration items in the TODO). Only
writing ON decisions keeps the feature a strict improvement.

## 3. Data Propagation

### 3.1 `sql/sql_select.h`

`POSITION` carries:

- `icp_decision_made`
- `use_cost_based_icp`
- `icp_keyno`
- `cost_if_no_icp`
- `cost_if_with_icp`

`JOIN_TAB` carries copied runtime decision:

- `m_icp_decision_made`
- `m_use_cost_based_icp`
- `m_icp_keyno`

plus helper APIs:

- `has_cost_based_icp_decision_for(keyno)`
- `use_cost_based_icp()`
- `set_cost_based_icp_decision(...)`

Because the planner only writes ON decisions today (section 2.3),
`has_cost_based_icp_decision_for(keyno) == true` currently implies
`use_cost_based_icp() == true`. The separation is preserved so a future,
better-calibrated model can opt in to cost-based OFF without reshaping
the data structures.

### 3.2 `sql/sql_optimizer.cc`

In `JOIN::get_best_combination()`, copy decision from `POSITION` to `JOIN_TAB`.

## 4. Execution-stage Enforcement

File: `sql/sql_select.cc`

In `QEP_TAB::push_index_cond(...)`:

- if a decision exists for `keyno` and is OFF:
  - add trace `not_pushed_due_to_icp_cost=true`
  - return without pushing ICP
- otherwise preserve the legacy community pushdown path.

Given section 2.3, the OFF branch is inert for now. The MTR regression
guard asserts that `not_pushed_due_to_icp_cost` does *not* appear for the
range-scan shape that used to hit it.

## 5. Test Coverage

`mysql-test/t/icp_cost_based.test`:

- Case 1 -- `icp_cost_based=off`, FORCE INDEX + ref access:
  verifies that legacy behavior is unchanged and no new trace fields
  appear. The `EXPLAIN FORMAT=TREE` result asserts the plan shape:
  `Index lookup on t1 using idx_abc ..., with index condition: ...`.

- Case 2 -- `icp_cost_based=on`, FORCE INDEX + ref access:
  verifies that the new trace fields are emitted and that the plan still
  uses `Index lookup` with an `index condition: ...` annotation.

- Case 3 (issue-2 regression) -- `icp_cost_based=on`, FORCE INDEX + range
  on leading key + equality on a trailing index column:
  verifies that the planner does *not* silently disable ICP. The
  `EXPLAIN FORMAT=TREE` result asserts
  `Index range scan on t3 using idx_ab ..., with index condition: ...`
  and the trace assertion `icp_suppressed_by_cost_model = 0` confirms
  the absence of `not_pushed_due_to_icp_cost`.

All `EXPLAIN FORMAT=TREE` output is normalized via `--replace_regex` to
strip `cost=...`, `rows=...` and `(actual time=...)` so the test is
stable across platforms and cost-model tweaks.

`mysql-test/r/icp_cost_based.result` stores the expected output.

Regression sanity:

- `main.1st` remains pass.
- Broader ICP suites pass: `innodb_icp`, `innodb_icp_all`,
  `innodb_icp_none`, `range_icp`, `func_in_icp`, `null_key_icp_innodb`.
