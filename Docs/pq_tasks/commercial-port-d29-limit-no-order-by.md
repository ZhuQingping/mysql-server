# D29 parallel_limit_no_order_by behavior alignment

## Goal

Align the commercial `parallel_limit_no_order_by` session variable and its
eligibility behavior.

Commercial MySQL enables this variable by default. When it is ON, a
row-returning query with `LIMIT` and no `ORDER BY` may remain PQ eligible if all
other PQ gates pass. When it is OFF, the current conservative
`LIMIT_NO_ORDER_BY` fallback remains active.

## Scope

Allowed files:

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `sql/parallel_query/pq_optimizer.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_limit_no_order_by.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_limit_no_order_by.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `mysql-test/suite/parallel_query/t/pq_explain_fallback.test`
- `mysql-test/suite/parallel_query/r/pq_explain_fallback.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`
- `Docs/pq_tasks/commercial-port-d29-limit-no-order-by.md`

Forbidden in this task:

- Do not implement ORDER BY Gather Merge.
- Do not change worker/InnoDB/handler/MQ/range/ref/ICP paths.
- Do not bypass unrelated unsupported shape checks.
- Do not change LIMIT execution semantics outside the PQ eligibility decision.

## Design

Add `parallel_limit_no_order_by` as a commercial-compatible session variable
with default ON. Change the existing hard LIMIT-without-ORDER-BY rejection to
depend on this variable:

- ON: continue eligibility checks;
- OFF: reject with `LIMIT_NO_ORDER_BY`.

## Validation

Targeted validation:

- RED before code:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_limit_no_order_by`
- GREEN after code:
  `git diff --check`
  `cmake --build build-ninja --target mysqld -j 8`
  `cd build-ninja/mysql-test && ./mtr pq_commercial_limit_no_order_by pq_explain_fallback pq_vars`

## Completion Report

Status: completed.

Changed files:

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `sql/parallel_query/pq_optimizer.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_limit_no_order_by.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_limit_no_order_by.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `mysql-test/suite/parallel_query/t/pq_explain_fallback.test`
- `mysql-test/suite/parallel_query/r/pq_explain_fallback.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`
- `Docs/pq_tasks/commercial-port-d29-limit-no-order-by.md`

Implementation:

- Added `parallel_limit_no_order_by` to `System_variables`.
- Added commercial-compatible sysvar definition with default ON.
- Changed the existing LIMIT-without-ORDER-BY eligibility rejection to depend
  on `!thd->variables.parallel_limit_no_order_by`.
- Added a commercial behavior MTR covering default ON, explicit OFF, and
  DEFAULT reset.
- Added a `SET_VAR(parallel_limit_no_order_by=OFF)` coverage point for the
  commercial `HINT_UPDATEABLE` contract.
- Extended `pq_vars` and `pq_explain_fallback` for the new variable and
  explicit OFF fallback coverage.

Validation:

- RED before code:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_limit_no_order_by`
  failed with unknown variable `parallel_limit_no_order_by`.
- Build:
  `cmake --build build-ninja --target mysqld -j 8`
  passed.
- GREEN targeted regression:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_limit_no_order_by pq_explain_fallback pq_vars`
  passed.
- Final targeted regression after review suggestion:
  `cd build-ninja/mysql-test && ./mtr pq_commercial_limit_no_order_by pq_explain_fallback pq_vars`
  passed.

Review:

- Independent review Agent accepted the code/test boundary.
- Review suggested optional `SET_VAR` hint coverage; it was added before final
  verification.

Final status:

- Ready to commit as a D29 standalone change.
