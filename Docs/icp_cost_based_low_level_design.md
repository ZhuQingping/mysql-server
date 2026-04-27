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

#### 2.2.2 Hard cap on applied adjustment

File: `sql/sql_planner.cc`

Raw adjustments produced by `should_enable_icp_by_cost()` and
`range_icp_preview()` are fed through a shared hard cap before the
caller writes them back into any cost field:

- constant `kIcpBenefitCapRatio = 0.5` (file-local, `sql_planner.cc`);
- helper `cap_icp_benefit_adjustment(adjustment, cost_if_no_icp,
  capped_out)`:
  - expects `adjustment <= 0` (callers always pass
    `cost_if_with_icp - cost_if_no_icp`);
  - passes the adjustment through unchanged when
    `cost_if_no_icp <= 0` or `adjustment >= 0`;
  - otherwise clamps the magnitude so that
    `|adjustment| <= kIcpBenefitCapRatio * cost_if_no_icp`, sets
    `*capped_out = true`, and returns the clamped value;
  - otherwise leaves `*capped_out = false` and returns the original
    adjustment.

The cap is applied at three integration points so no reward pathway can
bypass it:

1. Scan/range path reward in `best_access_path()` -- see 2.3; the
   clamped value is what lands in `best_read_cost`.
2. Ref tournament reward in `find_best_ref()` -- see 2.4; the clamped
   value is what lands in `cur_ref_cost`.
3. Range-candidate preview in `range_icp_preview()` -- see 2.5;
   uses a mirror constant `kRangeIcpBenefitCapRatio = 0.5` and floors
   `effective_cost` at `(1 - kRangeIcpBenefitCapRatio) * original_cost`.

When the clamp fires, the corresponding trace point emits
`icp_cost_adjustment_capped = true` (see trace field lists in 2.3 /
2.4 / 2.5).

Why 50%: empirically well-filtered ICP predicates save 30%-80% of row
lookups; 50% is wide enough to preserve meaningful wins but narrow
enough that a single `get_filtering_effect()` miss cannot single-
handedly collapse a path to near-zero cost and reshape the join-order
DP. Tightening the cap in the future requires only changing the
two constants and rebaselining; loosening requires first showing that
the estimator is more trustworthy than it is today.

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
  - run the raw adjustment `cost_if_with_icp - cost_if_no_icp` through
    `cap_icp_benefit_adjustment(..., cost_if_no_icp, &capped)` (see
    2.2.2);
  - apply the (possibly clamped) reward to `best_read_cost`;
  - emit `icp_cost_adjustment` / `icp_adjusted_read_cost`, plus
    `icp_cost_adjustment_capped = true` when the cap fired;
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
   call returns true, pass the raw `with - no` through
   `cap_icp_benefit_adjustment(..., no, &capped)` (see 2.2.2) and apply
   the clamped value to `cur_ref_cost`. Emit per-candidate trace
   fields: `icp_cost_based`, `icp_rows_fetched`, `icp_filter_effect`,
   `icp_cost_if_disabled`, `icp_cost_if_enabled`, `icp_enabled`,
   `icp_cost_adjustment`, `icp_adjusted_ref_cost`; additionally emit
   `icp_cost_adjustment_capped = true` when the cap fired for this
   candidate.

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

**Out of scope for this cut.** Non-ref semi-join materializations and
the hypergraph optimizer are not touched. These are tracked as
separate follow-ups (see section 6). Range-scan candidates have their
own preview; see section 2.5.

### 2.5 Range-candidate ICP preview (issue-1 range form)

File: `sql/range_optimizer/index_range_scan_plan.cc`.

`get_key_scans_params()` picks the winning range candidate by
comparing each candidate's handler-reported `check_quick_select()`
cost against `cost_est` (the table-scan cost passed in from
`best_access_path()`). Handler cost is blind to "what the SQL layer
will push down later", so candidates that would be very cheap *with*
ICP get rejected with `cause: cost` and the entire range option is
collapsed to `nullptr`. The range-form fix adds a local ICP preview
that only influences tournament comparisons.

**Static helpers** (inside an anonymous namespace in
`index_range_scan_plan.cc`):

1. `range_icp_preview_gates(thd, table, keynr)` returns true iff:
   - `optimizer_switch=icp_cost_based=on`;
   - `keynr != MAX_KEY`;
   - engine reports `HA_DO_INDEX_COND_PUSHDOWN` for `keynr`;
   - the ICP hint/switch allows pushdown on this key
     (`hint_key_state(thd, table->pos_in_table_list, keynr,
     ICP_HINT_ENUM, OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN)`);
   - command is **not** `SQLCOM_UPDATE_MULTI` /
     `SQLCOM_DELETE_MULTI`;
   - the index does **not** contain virtual generated columns;
   - the key is **not** a clustered PK;
   - the key is **not** a covering index
     (`table->covering_keys.is_set(keynr) && !table->no_keyread` is
     false).

2. `range_icp_preview(thd, table, keynr, where_cond, found_records,
   original_cost, trace_idx)` returns the effective cost:
   - early return `original_cost` if
     `where_cond == nullptr`, `found_records` is 0 or
     `HA_POS_ERROR`, or the gates in (1) fail;
   - reads `bound_keyparts = table->quick_key_parts[keynr]`; if
     `bound_keyparts == 0` or `bound_keyparts >= total_keyparts`,
     return `original_cost` (nothing trailing to push into, or
     nothing bound by the range at all);
   - builds two bitmaps over `table->s->fields`:
     - `key_cols` = every field of every keypart of `keynr`;
     - `range_bound_cols` = fields backing the first
       `bound_keyparts` keyparts;
   - walks `where_cond` with
     `Item::add_field_to_cond_set_processor` to populate
     `table->cond_set`. The planner clears and re-walks `cond_set`
     later in `choose_table_order` before `best_access_path()`
     runs, so temporarily writing here is safe; the helper also
     restores `cond_set` on exit when it was initially empty;
   - computes `remaining_cond_cols = cond_set \ range_bound_cols`;
     returns `original_cost` when this set is empty (no predicates
     left to push) or not a subset of `key_cols` (some remaining
     predicate column is off this index);
   - estimates
     `remaining_filter = where_cond->get_filtering_effect(thd,
     table_map, read_tables=0, &range_bound_cols, found_records)`;
     requires `0 < remaining_filter < 1`, else returns
     `original_cost`;
   - computes
     `cost_if_with_icp = original_cost * remaining_filter +
     cost_model->row_evaluate_cost(found_records *
     kRangeIcpEvalCpuFactor)`
     where `kRangeIcpEvalCpuFactor = 0.25` (file-local constant
     that parallels the `kIcpEvalCpuFactor` used by
     `should_enable_icp_by_cost()`);
   - returns `cost_if_with_icp` only when it is strictly smaller
     than `original_cost`; otherwise returns `original_cost`;
   - enforces the hard cap (`kRangeIcpBenefitCapRatio = 0.5`, see
     2.2.2): if the candidate's `cost_if_with_icp` dips below
     `(1 - kRangeIcpBenefitCapRatio) * original_cost`, the helper
     floors the returned value at that floor (`effective_cost = min
     allowed value`) and sets an internal "capped" flag;
   - when a reward is applied, emits trace fields
     `icp_cost_based`, `icp_rows_fetched`,
     `icp_filter_effect`, `icp_cost_if_disabled`,
     `icp_cost_if_enabled`, `icp_enabled`,
     `icp_cost_adjustment`, `icp_adjusted_range_cost`; additionally
     emits `icp_cost_adjustment_capped = true` when the cap fired.

**Integration in `get_key_scans_params()`.** Inside the per-index
loop:

- track two running values:
  - `read_cost` -- current best "effective" cost used for
    comparisons (may be ICP-adjusted);
  - `best_original_cost` -- current winner's handler-reported cost
    used for `AccessPath::cost` and all downstream consumers;
- after `check_quick_select()` produces `(found_records, cost)`,
  pull `where_cond` from `param->query_block->where_cond()` (only
  when `!ror_only`, to keep index-merge construction completely
  untouched), then call
  `effective_cost = range_icp_preview(thd, param->table, keynr,
  where_cond, found_records, cost.total_cost(), &trace_idx)`;
- compare `read_cost > effective_cost` instead of
  `read_cost > cost.total_cost()`;
- on a new winner, update
  `read_cost = effective_cost` and
  `best_original_cost = cost.total_cost()`;
- on exit, set `path->cost = best_original_cost` (not `read_cost`).

**Guarantees.**

- `AccessPath::cost` is identical to community behavior for the
  winning candidate; only the *comparison* inside
  `get_key_scans_params()` may see an ICP-reduced value. As a
  result `calculate_scan_cost`, EXPLAIN, and join-order DP see the
  same numbers they would see today.
- Index-merge construction (`ror_only = true`) is skipped entirely
  inside the loop by forcing `where_cond = nullptr`; the classic
  "is this candidate rowid-ordered enough to participate in ROR
  intersect" check is untouched.
- Index shapes that cannot benefit from the preview (covering
  indexes, clustered PKs, fully-bound prefixes, off-index
  remainders, etc.) return `original_cost` from the helper and the
  legacy `read_cost > cost.total_cost()` comparison is preserved
  byte-for-byte.

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

- Case 5 (issue-1 range form) -- `WHERE a BETWEEN 2 AND 4 AND c = 50`
  with no hint. With `icp_cost_based=off`, every range candidate's
  handler cost exceeds the table-scan baseline, so the plan is a
  Table scan. With `icp_cost_based=on`, the range-candidate preview
  (see 2.5) rewards `idx_abc` because `c = 50` can be pushed down,
  and the plan becomes `Index range scan on tt using idx_abc over
  (2 <= a <= 4), with index condition: ((tt.c = 50) and (tt.a between
  2 and 4))`.

- Case 5a (full-prefix keyparts, reward must NOT fire) --
  `WHERE a BETWEEN 2 AND 4 AND b = 3 AND c = 50` with
  `icp_cost_based=on`: the range already binds every keypart of
  `idx_abc`, so `bound_keyparts == total_keyparts` and the preview
  gate returns `original_cost` unchanged. Plan must match community
  behavior.

- Case 5b (remaining column off any index, reward must NOT fire) --
  `WHERE a BETWEEN 2 AND 4 AND d = 7`: column `d` is not a keypart
  of any index, so the `remaining_cond_cols ⊆ key_cols` check fails
  for every candidate. Plan must be the same Table-scan baseline
  with ICP on and off.

- Case 6a (multi-table guardrail, STRAIGHT_JOIN): the pinned join
  order `tj_driver -> tt` holds on both sides of the toggle; ON
  legitimately upgrades `tt`'s access from `idx_a + SQL Filter`
  to `idx_abc + index condition: (tt.c = 50)`.

- Case 6b (multi-table guardrail, free join order): the optimizer
  must still choose `tj_driver` (5 rows) as the driver when ON,
  even though `tt` (10k rows) now looks cheaper due to the reward.
  This is ensured by NOT writing the reward into
  `pos->rows_fetched` / `pos->filter_effect` (line 1496-1497 of
  `sql_planner.cc`); join-order DP sees the same fanout estimate
  on both sides.

- Case 6c (multi-table guardrail, semijoin strategy change, EXPECTED):
  OFF -> `MaterializeLookup` on `tt`; ON -> `FirstMatch` on
  `idx_abc + ICP`. This is an EXPECTED consequence of
  `advance_sj_state()` / `fix_semijoin_strategies_for_picked_join_order`
  reading `pos->read_cost` (which DOES carry the reward). The
  guardrail records the exact plan strings so any unexpected
  strategy drift is surfaced the next time the baseline is
  recorded. Release notes / operator-facing docs should call out
  this potential strategy migration when turning the feature on.

All `EXPLAIN FORMAT=TREE` output is normalized via `--replace_regex` to
strip `cost=...`, `rows=...` and `(actual time=...)` so the test is
stable across platforms and cost-model tweaks.

`mysql-test/r/icp_cost_based.result` stores the expected output.

Regression sanity:

- `main.1st` remains pass.
- Broader ICP suites pass: `innodb_icp`, `innodb_icp_all`,
  `innodb_icp_none`, `range_icp`, `func_in_icp`, `null_key_icp_innodb`.
- Semijoin strategy suites pass with default
  `optimizer_switch`: `subquery_sj_firstmatch`, `subquery_sj_mat`,
  `subquery_sj_loosescan` (these confirm that with
  `icp_cost_based=off`, the default, no strategy migration happens
  to the existing baselines).

## 6. Current Scope: Classic Optimizer Only

The implementation in this document is deliberately limited to the classic
optimizer path. All write points and cost previews are anchored in classic
optimizer data structures and call sites:

- `find_best_ref()` adjusts a ref candidate's tournament cost before the
  classic ref winner is picked.
- `get_key_scans_params()` adjusts the range optimizer's per-index effective
  tournament cost before a range candidate is selected.
- `best_access_path()` writes the final cost-based ON decision into
  `POSITION`; `JOIN::get_best_combination()` copies it into `JOIN_TAB`.
- `QEP_TAB::push_index_cond()` consumes the copied decision during classic
  plan refinement.

The Hypergraph optimizer does not use this exact planning flow for access-path
enumeration and costing. In particular, adding hooks to `find_best_ref()` and
`get_key_scans_params()` does not make Hypergraph access paths compare ref or
range alternatives with the ICP-adjusted effective cost described here.
Therefore, for this cut, the supported scenario is:

- classic optimizer enabled / Hypergraph optimizer not selected for the query;
- InnoDB or another engine that exposes `HA_DO_INDEX_COND_PUSHDOWN`;
- `optimizer_switch='icp_cost_based=on'` explicitly enabled.

When Hypergraph optimization is selected, this feature should be considered
out of scope: plan selection is not guaranteed to include the cost-based ICP
reward, and the query falls back to the ICP behavior already implemented by
that optimizer path. A future extension can add equivalent preview and
decision propagation hooks to the Hypergraph access-path costing layer.

## 7. Follow-up Work

- **Hypergraph optimizer.** The current previews live in the classic
  `find_best_ref()` and `get_key_scans_params()` paths. The
  hypergraph path picks access methods through a different code flow
  and needs its own integration point for both ref and range forms.
- **Selectivity calibration.** `Item::get_filtering_effect()` is a
  conservative guesstimate. If telemetry shows systematic
  under/over-reward on specific predicate shapes, both previews can
  swap in a better estimator without reshaping the gate logic. The
  range-form helper also uses a standalone `kRangeIcpEvalCpuFactor`;
  a future unification with the planner-side `kIcpEvalCpuFactor`
  would be cleaner once both have soaked long enough. Tighten /
  loosen `kIcpBenefitCapRatio` / `kRangeIcpBenefitCapRatio` (2.2.2)
  in lockstep once `icp_cost_adjustment_capped` telemetry suggests
  the current 50% budget is too tight or too loose.
- **Cost-based OFF decisions.** Section 2.3 deliberately does not
  persist OFF decisions today. A future, better-calibrated cost
  model can opt in without further refactoring; the execution-side
  machinery in section 4 is already wired up.
- **Trace for off-tournament skips.** The range-form helper emits
  trace only when a reward is applied. Adding a concise skip
  reason (`icp_preview_skipped = "covering"`, `"fully_bound"`,
  `"not_on_index"`, etc.) would make the MTR gate cases easier to
  diagnose without reading the C++ code.
