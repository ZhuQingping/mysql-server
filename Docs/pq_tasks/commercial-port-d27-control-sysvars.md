# D9 Commercial Control Sysvars Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for behavior changes
> and request independent review before commit.

**Goal:** 补齐商用 `parallel_query` 测试套高频依赖的控制/队列变量合同。

**Architecture:** 本任务只增加 MySQL sysvar 和 `THD::variables` 存储字段，并用
MTR 验证变量存在、默认值、SESSION/GLOBAL 可见性、SET/DEFAULT 和边界值。
本任务不把变量接入 optimizer、cost model、fallback retry、message queue
allocation、InnoDB count fast path 或 hash join 行为。

**Tech Stack:** MySQL 8.0.46 sysvar framework、`System_variables`、MTR。

---

## 状态

Status: planned.

## 背景

`taurusdbondstore` 商用测试套大量直接引用以下变量。当前分支还没有这些变量，
导致后续迁移商用测试时会先失败在变量缺失，而不是失败在真实行为差异。

- `force_parallel_execute`
- `parallel_fail_retry`
- `pq_msg_queue_size`
- `pq_msg_queue_spin_lock`
- `innodb_parallel_select_count`
- `pq_hash_join_max_hash_table_refills`
- `op_over_pq_offset_threshold`

## 允许修改

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_control_sysvars.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_control_sysvars.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `Docs/pq_tasks/commercial-port-d27-control-sysvars.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `JOIN::optimize()`、AccessPath、iterator factory 或 EXPLAIN eligibility；
- PQ hint parser / `PQ()` / `NO_PQ` 行为；
- `Query_result_mq` queue sizing allocation；
- `parallel_fail_retry` 自动重试行为；
- `innodb_parallel_select_count` fast count 行为；
- hash join spill/refill 行为；
- InnoDB / handler worker scan 路径。

## 商用变量合同

参考：`/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/sys_vars.cc`。

- `force_parallel_execute`
  - scope: SESSION, global default visible through MySQL session sysvar rules
  - type: bool
  - default: `OFF`
  - `HINT_UPDATEABLE`
- `parallel_fail_retry`
  - scope: SESSION
  - type: bool
  - default: `ON`
- `pq_msg_queue_size`
  - scope: SESSION
  - type: unsigned integer
  - default: `1048576`
  - range: `1048576..1073741824`
- `pq_msg_queue_spin_lock`
  - scope: SESSION
  - type: unsigned integer
  - default: `1000`
  - range: `100..ULONG_MAX`
- `innodb_parallel_select_count`
  - scope: SESSION
  - type: bool
  - default: `ON`
  - `HINT_UPDATEABLE`
- `pq_hash_join_max_hash_table_refills`
  - scope: SESSION
  - type: unsigned integer
  - default: `1`
  - range: `1..ULONG_MAX`
- `op_over_pq_offset_threshold`
  - scope: SESSION
  - type: unsigned integer
  - default: `1000`
  - range: `0..ULONG_MAX`
  - `HINT_UPDATEABLE`

## TDD Steps

- [ ] Write `pq_commercial_control_sysvars` before production code.
  The first RED failure must be an unknown variable, expected at
  `@@session.force_parallel_execute`.
- [ ] Add `System_variables` fields and `Sys_var_*` declarations matching the
  商用 defaults/ranges above.
- [ ] Run GREEN MTR and adjust only exact sysvar output formatting.
- [ ] Extend `pq_vars` to include the new commercial variables in the common
  variable contract test.
- [ ] Run build, targeted MTR, and independent review.
- [ ] Fix Critical/Important review findings, commit, and push.

## RED Command

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_control_sysvars \
  --vardir=/tmp/pq-d27-red-vardir \
  --tmpdir=/tmp/pq-d27-red-tmpdir
```

Expected RED before production changes: unknown system variable
`force_parallel_execute`.

## Verification

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_control_sysvars pq_vars pq_commercial_resource_sysvars \
  --vardir=/tmp/pq-d27-final-vardir \
  --tmpdir=/tmp/pq-d27-final-tmpdir
```

## Completion Report

Status: pending.

Changed files: pending.

Implementation: pending.

Verification: pending.

Review: pending.

Residual risk: pending.
