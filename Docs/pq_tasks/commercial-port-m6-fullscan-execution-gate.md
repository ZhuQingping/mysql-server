# M6 Commercial Full Scan Execution Gate Taskbook

## 状态

Planned。

## 目标

用商用主路径跑通单表 clustered full scan 的 DOP2/DOP4 查询。新执行 gate 默认 OFF，显式开启后结果必须与串行一致。

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

- 新 gate 默认 OFF；
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

Pending.
