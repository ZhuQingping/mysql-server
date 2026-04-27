# ICP Cost-Based Planning Commit Notes

## Overview

This note captures the key context for the ICP cost-based planning work:
why it was done, how it was implemented, how it was tested, and what should
be optimized next.

## Commit

- Commit ID: `882c7537748`
- Title: `Add cost-based ICP planning with optimizer switch`
- Branch: `optimize_icp`

## Why This Change

MySQL traditionally decides ICP late in plan refinement (`refine_plan`),
after join order and access path choices are already fixed. This can miss
plans that become better when ICP filtering benefit is included in cost.

The goal is to let the optimizer consider ICP benefit during access path
selection while keeping rollback easy and behavior safe.

## Design Summary

1. Add a dynamic optimizer switch gate:
   - `optimizer_switch=icp_cost_based`
   - Default behavior remains compatible when disabled.
2. During planning (`best_access_path`):
   - Estimate cost if ICP is disabled.
   - Estimate cost if ICP is enabled.
   - Decide whether to enable ICP by comparing those estimates.
3. Persist the decision:
   - Store on `POSITION`.
   - Copy to `JOIN_TAB` when the final combination is built.
4. Enforce decision at execution-plan refinement:
   - In `QEP_TAB::push_index_cond`, skip pushdown when planner decided off.
5. Improve observability:
   - Emit optimizer trace fields to debug decision inputs and outputs.

## Main Files Touched

- `sql/sql_const.h`
- `sql/sys_vars.cc`
- `sql/sql_planner.cc`
- `sql/sql_select.h`
- `sql/sql_optimizer.cc`
- `sql/sql_select.cc`
- `Docs/icp_cost_based_high_level_design.md`
- `Docs/icp_cost_based_low_level_design.md`
- `mysql-test/t/icp_cost_based.test`
- `mysql-test/r/icp_cost_based.result`

## Test Coverage

- Added MTR test: `main.icp_cost_based`
- Validates both:
  - `optimizer_switch='icp_cost_based=off'`
  - `optimizer_switch='icp_cost_based=on'`
- Checks optimizer trace output differences and key fields.

Typical run command (from build directory):

```bash
cd build-ninja/mysql-test
./mtr main.icp_cost_based
```

## TODO

- Documented current scope: this cut supports the classic optimizer path only.
  Hypergraph optimizer access-path selection is out of scope until equivalent
  ref/range preview hooks are added under its costing flow.
- Calibrate engine-side ICP CPU factor with broader benchmark data.
- Add optimizer_switch combination tests for `default/on/off` transitions.
- Add more corner-case tests (reverse scan, temp-table-related paths).

## Future Optimization Ideas

- Replace fixed ICP CPU factor with engine-aware dynamic calibration.
- Integrate ICP selectivity with histogram/fanout estimation.
- Refine interaction with range optimizer for borderline plans.
- Add Hypergraph optimizer integration once its access-path costing layer can
  consume equivalent ICP benefit previews.
