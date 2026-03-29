# Cost-based ICP Low-level Design

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

### 2.3 Planning integration

In `best_access_path()`:

- derive candidate key from chosen access path,
- run precheck + decision,
- write decision fields into `POSITION`,
- emit trace fields (`icp_cost_based`, `icp_enabled`, cost details),
- if enabled, apply cost delta:
  - `best_read_cost += (cost_if_with_icp - cost_if_no_icp)`

This lets planner account for ICP benefit when comparing paths.

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

### 3.2 `sql/sql_optimizer.cc`

In `JOIN::get_best_combination()`, copy decision from `POSITION` to `JOIN_TAB`.

## 4. Execution-stage Enforcement

File: `sql/sql_select.cc`

In `QEP_TAB::push_index_cond(...)`:

- if decision exists for `keyno` and is OFF:
  - add trace `not_pushed_due_to_icp_cost=true`
  - return without pushing ICP

If no decision exists, legacy path applies.

## 5. Test Coverage

`mysql-test/t/icp_cost_based.test`:

- setup indexed table and data
- run with:
  - `icp_cost_based=off` (expect new trace fields absent)
  - `icp_cost_based=on` (expect new trace fields present)

`mysql-test/r/icp_cost_based.result` stores expected output.

Regression sanity:

- `main.1st` remains pass.
