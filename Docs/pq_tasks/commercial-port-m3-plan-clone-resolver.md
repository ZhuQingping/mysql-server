# M3 Commercial Plan Clone And Resolver Activation Taskbook

## 状态

Completed。

## 目标

迁移商用 plan clone / resolver 的最小 activation probe 和 diagnostic。M3 不直接搬商用 `pq_make_join()` 全链路，不生成可执行 worker plan；真正 worker plan 只在 Item/JOIN/QEP_TAB clone contract 完整后打开。

## 允许修改

- `sql/sql_class.h`
- `sql/sql_lex.h`
- `sql/sql_optimizer.h`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_clone.*`
- `sql/parallel_query/pq_clone_item.cc`
- `sql/parallel_query/pq_resolver.*`
- `sql/parallel_query/pq_refix_fields_item.cc`
- `sql/parallel_query/pq_replace_base_item.cc`
- `sql/parallel_query/pq_optimizer.*`
- `Docs/pq_tasks/commercial-port-m3-plan-clone-resolver.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 禁止修改

- `storage/innobase/**`
- `sql/handler.h`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/exchange.*`
- `mysql-test/suite/parallel_query/t/*`，除非只新增 M3 fallback/diagnostic 用例
- `mysql-test/suite/parallel_query/r/*`，除非只新增 M3 fallback/diagnostic 预期

## 设计要求

- M3 流程是 `eligible -> clone_preflight_attempt -> pq_make_join probe -> diagnostic -> cleanup -> serial plan`；
- clone 失败必须 worker-start 前 fallback；
- worker THD 不允许递归开启 PQ；
- 不打开 subquery / UNION / derived / semijoin / partition / secondary index；
- 不把 `PARALLEL_SCAN` 接入真实执行；
- 不迁移大面积 `Item::pq_clone()/refix_fields()/pq_restore()` virtual contract；
- `pq_dup_tabs()`、`pq_dup_order()`、`pq_replace_base_item()`、resolver/base-ref helpers 继续 fail-closed；
- 不创建 `Gather_operator`、不分配 worker、不调用 handler/InnoDB `pq_leader_scan_init()`；
- 新增 diagnostic 必须区分 clone attempts/fallback/unsupported shape；
- review Agent 达成一致后才提交。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m3 --tmpdir=/tmp/pqt_m3
```

## Completion Report

Completed by Codex Orchestrator.

Changed files:

- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/pq_clone.h`
- `sql/parallel_query/pq_clone.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_clone_diagnostics.test`
- `mysql-test/suite/parallel_query/r/pq_clone_diagnostics.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m3-plan-clone-resolver.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- 新增 `pq_clone_activation_probe()`，只在 guarded `TryCreatePQTableScanIterator()` 中、非 EXPLAIN、PQ eligible、InnoDB、非 worker 的边界触发；
- probe 只记录 `Parallel_clone_probe_attempts` / `fallback` / `success` / `unsupported`，不调用 `pq_make_join()`，不保存 cloned JOIN；
- 当前 `pq_make_join()`、`pq_dup_tabs()`、`pq_dup_order()`、`pq_replace_base_item()`、resolver/base-ref helper 继续 fail-closed；
- M3 没有创建 `Gather_operator`、没有启动 worker、没有新增 handler/InnoDB 调用；
- 为保留当前分支 V2 smoke/回归护栏，clone probe 后继续进入既有 `PQTableScanIterator` fallback/smoke 路径；M3 不移除已有 worker/exchange/read-view smoke 行为；
- `Parallel_queries_fallback` 仍由现有 iterator fallback 路径计数，clone probe 不重复计数；
- EXPLAIN 在 clone probe 前返回，不污染 clone probe counters。

Review:

- Explorer Agent 确认商用 `pq_make_join()` 依赖大量当前分支尚未完整迁移的 `Item::pq_clone()` / `refix_fields()` / `pq_restore()`、`Query_block::pq_backup()/pq_restore()`、`JOIN::pq_restore()` 等契约；
- Review Agent 未发现 blocker；
- Review Agent 建议修正 `pq_clone.h` 注释，已完成；
- 已确认 M3 当前目标是 activation probe + diagnostics，不是完整 worker plan clone。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_clone_diagnostics --parallel=1 --vardir=/tmp/pqv_m3_clone --tmpdir=/tmp/pqt_m3_clone
TMPDIR=/tmp ./mtr --suite=parallel_query pq_stats --parallel=1 --vardir=/tmp/pqv_m3_stats --tmpdir=/tmp/pqt_m3_stats
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m3_full --tmpdir=/tmp/pqt_m3_full
```

Result:

- `mysqld` build passed；
- `pq_clone_diagnostics` passed；
- `pq_stats` passed；
- full current `parallel_query` suite passed，70 tests successful。

Risk / next:

- `Parallel_clone_probe_success` 当前预期为 0，等后续完整 clone contract 接入后再新增正向断言；
- M4 必须在此基础上先明确 worker result protocol，不应假设 M3 已生成可执行 worker plan。
