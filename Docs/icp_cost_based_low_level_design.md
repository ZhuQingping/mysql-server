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

### 2.4 Ref tournament ICP preview (issue-1 ref form)

Inside `find_best_ref()`'s per-candidate loop (`sql/sql_planner.cc`),
immediately after computing `cur_ref_cost` and before the tournament
comparison, preview each candidate's ICP benefit and feed it back into
`cur_ref_cost`:

1. **Gate 1 -- feature enabled and pushdown is legal.** Skip when
   `icp_cost_based=off`, when the key is FULLTEXT, or when
   `can_consider_icp_cost(tab, key)` returns false. Also skip when
   there is no WHERE clause (`tab->join()->where_cond == nullptr`) or
   `table->cond_set` is entirely clear -- there is nothing to filter.

2. **Gate 2 -- the key has unbound keyparts.** Compute
   `has_unbound_keyparts = (found_part != LOWER_BITS(actual_key_parts(keyinfo)))`.
   If every keypart is already bound by the current ref, there is no
   trailing key material for ICP to evaluate; skip the reward.

3. **Gate 3 -- remaining WHERE columns live on this key.** Build
   three column bitmaps over `table->s->fields`:
   - `key_columns` = every field of every keypart of this index
     (reused via `table->tmp_set`);
   - `ref_bound_cols` = fields backing the keyparts in `found_part`
     (built in a local `MY_BITMAP` to keep `tmp_set` clean);
   - `remaining_cond_cols = table->cond_set \ ref_bound_cols`.
   Skip the reward when `remaining_cond_cols` is empty (all predicates
   already absorbed by ref) or when
   `bitmap_is_subset(&remaining_cond_cols, &key_columns)` is false
   (some remaining predicate column is not on this key, so the engine
   cannot evaluate it).

4. **Selectivity estimate.** When all gates pass, estimate the
   selectivity of the remaining predicates via
   `Item::get_filtering_effect(thd, tab_map, /*read_tables=*/0,
   &ref_bound_cols, tab->records())`. Passing `ref_bound_cols` as the
   "ignore" set prevents double-counting predicates already consumed by
   ref. If the result is outside `(0, 1)` the estimate is not
   actionable -- skip without changes.

5. **Cost decision.** Call `should_enable_icp_by_cost(tab, key,
   prefix_rowcount, cur_fanout, remaining_filter, &no, &with)`. If the
   call returns true, apply
   `cur_ref_cost += (with - no)` (a negative adjustment that reduces
   cost), and emit per-candidate trace fields: `icp_cost_based`,
   `icp_rows_fetched`, `icp_filter_effect`, `icp_cost_if_disabled`,
   `icp_cost_if_enabled`, `icp_enabled`, `icp_cost_adjustment`,
   `icp_adjusted_ref_cost`.

Always clear `tmp_set` before leaving the block so later iterations and
other callers see it empty.

**Guarantees.**

- The preview only *reduces* cost; any failed gate leaves
  `cur_ref_cost` untouched, so ref candidates that cannot benefit
  from ICP stay bit-for-bit compatible with community behavior.
- Narrower indexes that do not cover the remaining WHERE columns
  (e.g. `idx_a` / `idx_ab` for a query like `WHERE a = 4 AND c = 6`)
  are excluded by Gate 3 and do not receive any adjustment.
- The preview is local to the candidate loop. No `POSITION` fields
  are written here -- that is still the responsibility of section
  2.3 on the eventual winner.

**Out of scope for this cut.** Range-scan candidates, non-ref
semi-join materializations, and the hypergraph optimizer are not
touched. These are tracked as separate follow-ups (see section 6).

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

- Case 4 (issue-1 ref form) -- `icp_cost_based=on`, no hint, three
  equality-eligible indexes sharing the leading key `a` (`idx_a`,
  `idx_ab`, `idx_abc`) on `WHERE a = 4 AND c = 6`:
  with `off` the baseline plan is
  `Filter: (tt.c = 6) -> Index lookup on tt using idx_a (a=4)`;
  with `on` the plan must switch to
  `Index lookup on tt using idx_abc (a=4), with index condition:
  (tt.c = 6)`. A paired `FORCE INDEX(idx_abc)` query documents that
  the execution layer already produced this plan when pointed at the
  right index -- the gap was purely in access-method selection.

- Case 4a (feature-gate guardrail) -- same query, flipping
  `icp_cost_based` back to `off` must restore the legacy plan. Pins
  the feature flag so an accidental ON-by-default regression is
  caught immediately.

- Case 4b (full-ref keyparts, reward must NOT fire) --
  `WHERE a = 4 AND b = 5 AND c = 6` with `icp_cost_based=on`: every
  keypart of `idx_abc` is bound by ref, so Gate 3 finds
  `remaining_cond_cols` empty and the preview skips the reward. The
  expected plan is the community `idx_ab` lookup + server-side
  Filter on `c`, matching baseline behavior.

- Case 4c (remaining WHERE not on the index, reward must NOT fire) --
  `WHERE a = 4 AND d = 7`: column `d` is not a keypart of any index,
  so Gate 3 (`remaining_cond_cols ⊆ key_columns`) fails for every
  candidate. Both `off` and `on` must produce the same plan.

- Case 5 (issue-1 range form, KNOWN LIMITATION) -- `WHERE a BETWEEN
  2 AND 4 AND c = 50` with no hint: currently still falls back to a
  table scan because the range path does not yet benefit from the
  ref-tournament preview. The baseline `.result` pins the broken
  plan intentionally: when the range-form fix lands it will surface
  as a `.result` diff and become the progress signal.

All `EXPLAIN FORMAT=TREE` output is normalized via `--replace_regex` to
strip `cost=...`, `rows=...` and `(actual time=...)` so the test is
stable across platforms and cost-model tweaks.

`mysql-test/r/icp_cost_based.result` stores the expected output.

Regression sanity:

- `main.1st` remains pass.
- Broader ICP suites pass: `innodb_icp`, `innodb_icp_all`,
  `innodb_icp_none`, `range_icp`, `func_in_icp`, `null_key_icp_innodb`.

## 6. Follow-up Work

- **Issue-1 range form.** Extend the preview to range-scan candidates
  so shapes like Case 5 can also prefer an ICP-capable wider index
  over a narrower range scan or table scan.
- **Hypergraph optimizer.** The current preview lives in the classic
  `find_best_ref()` path. The hypergraph path picks access methods
  through a different code flow and needs its own integration point.
- **Selectivity calibration.** `Item::get_filtering_effect()` is a
  conservative guesstimate. If telemetry shows systematic
  under/over-reward on specific predicate shapes, the preview can
  swap in a better estimator without reshaping the gate logic.
- **Cost-based OFF decisions.** Section 2.3 deliberately does not
  persist OFF decisions today. A future, better-calibrated cost
  model can opt in without further refactoring; the execution-side
  machinery in section 4 is already wired up.
