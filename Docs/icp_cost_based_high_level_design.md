# Cost-based ICP High-level Design

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

## Feature Gate

Add `optimizer_switch` option:

- `icp_cost_based=off` (default): fallback to legacy behavior.
- `icp_cost_based=on`: enable cost-based ICP decision in planner.

## Execution Flow

1. In `best_access_path()`:
   - determine candidate key (`ref` key or chosen range key),
   - run eligibility prechecks,
   - estimate `cost_if_disabled` vs `cost_if_enabled`,
   - produce per-table/key ICP decision and store in `POSITION`,
   - when ICP is beneficial, adjust read cost by estimated delta so planning can
     prefer the better path.

2. In `get_best_combination()`:
   - copy decision from `POSITION` into `JOIN_TAB`.

3. In `push_index_cond()`:
   - if decision exists and says OFF, skip pushdown;
   - otherwise preserve legacy ICP flow.

## Observability

With `icp_cost_based=on`, optimizer trace includes:

- `icp_cost_based`
- `icp_enabled`
- `icp_cost_if_disabled`
- `icp_cost_if_enabled`
- `icp_cost_adjustment` (when enabled)

With `icp_cost_based=off`, those fields do not appear.

## Validation

Use `main.icp_cost_based` to verify:

- OFF path: legacy behavior (new fields absent).
- ON path: cost-based fields present.

Also keep baseline `main.1st` green to guard against broad regression.
