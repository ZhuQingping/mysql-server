# M6 Commercial Full Scan Execution Gate Taskbook

## 状态

Completed。

## 目标

用商用主路径跑通单表 clustered full scan 的 DOP2/DOP4 查询。DOP2 clustered full scan 已作为首个用户可见 gate 开启；DOP4 仍需要显式实验变量开启。

## 允许修改

- `sql/parallel_query/pq_optimizer.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/parallel_query/pq_iterators.*`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `Docs/pq_tasks/commercial-port-m6-fullscan-execution-gate.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- DOP2 clustered full scan 在 `parallel_query=ON`、`parallel_default_dop=2`、查询满足 `pq_eligible` 时可用户可见执行；
- DOP4 仍保持默认 OFF，只能通过显式实验变量开启；
- 支持 `SELECT * FROM t` 和简单 projection；
- WHERE 可随 worker plan 执行；
- LIMIT 无 ORDER BY 暂 fallback，除非 worker abort、MQ detach 和 result determinism 都有 targeted MTR；
- BLOB/TEXT/JSON/GEOMETRY、ORDER/GROUP/ref/ICP/partition 必须 fallback；
- worker started 后不允许 silent serial fallback。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_fullscan --parallel=1 --vardir=/tmp/pqv_m6 --tmpdir=/tmp/pqt_m6
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m6_full --tmpdir=/tmp/pqt_m6_full
```

## Completion Report

### 2026-06-19 Codex Orchestrator

Changed files:

- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`

实现说明:

- 当时保留默认 OFF 的执行 gate：DOP2 由 `parallel_query_experimental_threaded_dop` 显式开启，DOP4 由 `parallel_query_experimental_threaded_dop4` 显式开启；
- 新增 `pq_commercial_fullscan` MTR，集中覆盖 commercial fullscan gate：
  - 默认 OFF 时串行 fallback；
  - DOP2 `SELECT *` clustered full scan；
  - DOP2 简单 projection + WHERE；
  - DOP4 projection + WHERE；
  - TEXT/BLOB 表在 worker 启动前串行 fallback，`workers_delta=0`；
- 在 `PQTableScanIterator::Init()` safe fallback window 增加 BLOB/TEXT early guard，避免 PROBE/smoke 或 worker open 触碰当前不支持的 row image 类型；
- 未打开 ORDER/GROUP/ref/ICP/partition 路径。

验证:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_fullscan --parallel=1 --vardir=/tmp/pqv_m6 --tmpdir=/tmp/pqt_m6
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=4 --vardir=/tmp/pqv_m6_full2 --tmpdir=/tmp/pqt_m6_full2
```

结果:

- `mysqld` build 通过；
- `pq_commercial_fullscan` 通过；
- 完整 `parallel_query` suite 72 项成功。

Review:

- Review Agent 指出首版 BLOB 分支只查 `id`，不能证明 BLOB/TEXT fallback；
- 已改为 `SELECT * FROM t_blob`，并断言 `executed_delta=0`、`fallback_delta=1`、`workers_delta=0`；
- 复跑目标测试和完整 suite 均通过。

### 2026-06-24 Codex Orchestrator

Changed files:

- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `Docs/pq_tasks/commercial-port-m6-fullscan-execution-gate.md`

实现说明:

- 将 DOP2 clustered full scan 从实验变量 gate 提升为首个用户可见 commercial fullscan gate；
- gate 条件保持保守：`parallel_query=ON`、`parallel_default_dop=2`、`JOIN::pq_eligible=true`；
- DOP1/DOP4 不随本次改动放开，仍沿用各自实验变量或 DBUG 路径；
- 更新 `pq_commercial_fullscan`，直接覆盖无实验变量的 DOP2 `SELECT *` 正向执行，并保留 legacy DOP2 实验变量兼容性块；
- 同步更新历史 MTR 中“DOP2 默认 fallback”的旧断言：DOP1/DOP4 仍 fallback，DOP2 fullscan/COUNT/projection 按用户可见 row-stream 执行计数。
