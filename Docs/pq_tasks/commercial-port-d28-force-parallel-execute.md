# D28 force_parallel_execute behavior alignment

## Goal

Align the first visible behavior of the commercial `force_parallel_execute`
session variable.

`force_parallel_execute=ON` should allow an otherwise supported full table scan
to become PQ eligible even when its estimated cost is below
`parallel_cost_threshold`.

## Scope

Allowed files:

- `sql/parallel_query/pq_optimizer.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_force_parallel_execute.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_force_parallel_execute.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`
- `Docs/pq_tasks/commercial-port-d28-force-parallel-execute.md`

Forbidden in this task:

- Do not bypass `parallel_query=ON`.
- Do not bypass unsupported-shape checks such as `ORDER BY` or `LIMIT` without
  `ORDER BY`.
- Do not bypass DOP/resource gates.
- Do not change worker, InnoDB, handler, MQ, aggregation, ORDER BY, range, ref,
  or ICP execution paths.

## Design

The current eligibility flow performs cost-threshold rejection after all
structural and safety checks. D28 changes only that final cost gate:

- when `force_parallel_execute=OFF`, preserve the existing
  `COST_BELOW_THRESHOLD` rejection;
- when `force_parallel_execute=ON`, skip only the cost-threshold rejection and
  allow later EXPLAIN/runtime DOP gates to make their existing decision.

## Validation

Targeted validation:

- RED before code:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_force_parallel_execute`
- GREEN after code:
  `git diff --check`
  `cmake --build build-ninja --target mysqld -j 8`
  `cd build-ninja/mysql-test && ./mtr pq_commercial_force_parallel_execute pq_explain_eligible pq_explain_fallback pq_commercial_max_threads`

## Completion Report

Status: completed.

Changed files:

- `sql/parallel_query/pq_optimizer.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_force_parallel_execute.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_force_parallel_execute.result`
- `Docs/pq_tasks/commercial-port-d28-force-parallel-execute.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

Implementation:

- Added a minimal eligibility guard so `force_parallel_execute=ON` skips only
  the final `parallel_cost_threshold` rejection.
- Added MTR coverage for:
  - force OFF still reports `COST_BELOW_THRESHOLD`;
  - force ON makes the same full scan PQ eligible;
  - force ON does not bypass the main `parallel_query` switch;
  - ORDER BY and LIMIT without ORDER BY remain rejected;
  - DOP/resource budget remains rejected and does not execute PQ workers.

Validation:

- RED before code:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_force_parallel_execute`
  failed because force ON still returned `COST_BELOW_THRESHOLD`.
- Build:
  `cmake --build build-ninja --target mysqld -j 8`
  passed.
- GREEN:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_force_parallel_execute`
  passed.
- Targeted regression:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_force_parallel_execute pq_explain_eligible pq_explain_fallback pq_commercial_max_threads`
  passed.
- Diff hygiene:
  `git diff --check`
  passed.

Review:

- Independent review Agent accepted the code/test boundary.
- Review suggested optional stronger MTR coverage for `parallel_query=OFF` and
  runtime DOP-cap status deltas; both were added before final verification.

Final status:

- Ready to commit as a D28 standalone change.
