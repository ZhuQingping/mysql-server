# M9 Secondary Index / Ref / ICP Taskbook

## 状态

M9-A Completed。M9-B0 Completed。M9-B1 Completed。M9-B2/M9-B3/M9-C/M9-D/M9-E/M9-F Planned。

## 目标

迁移 `PQRefIterator`、secondary index、ICP 能力。该阶段依赖 M5/M6 的 handler/InnoDB execution gate 稳定。

设计调研后拆分：

- M9-A: secondary index/range/ref/ICP 仍默认 fallback，补负向 MTR，不改执行路径；
- M9-B0: secondary index range 正例设计与护栏，不改执行路径；
- M9-B1: secondary range candidate probe，synthetic/probe-only，不产生真实 secondary row；
- M9-B2: secondary range partition 最小前置，先限定 single-table InnoDB、非 partition、非 reverse、非 ref；
- M9-B3: secondary range callback row production 最小正例；
- M9-C: 常量 `JT_REF` 最小正例，含 `pq_ref_build_ranges`，不做 dependent ref；
- M9-D: dependent `PQRefIterator` / per-ref-key range dispatch；
- M9-E: ICP pushdown，迁移 worker 侧 `make_cond_for_index` / `idx_cond_push` / `make_cond_remainder`；
- M9-F: MVI unique filter、reverse scan、partition、secondary index MIN、record buffer/prefetch 等边角。

## 允许修改

- `sql/parallel_query/pq_iterators.*`
- `sql/handler.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/m9-b0-secondary-range-design.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

M9-A 只允许修改：

- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

M9-A 禁止修改：

- `sql/handler.h`
- `sql/join_optimizer/access_path.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/pq_iterators.*`
- `sql/parallel_query/pq_clone.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `sql/range_optimizer/*`

## 设计要求

- 先支持 ref/range 最小正例；
- ICP 打开前必须有负向 fallback 测试；
- `pushed_idx_cond` 存在但未验证时必须拒绝；
- MVI/unique filter 逻辑按商用实现迁移并单测；
- 不支持 partition/subquery 形态时必须 fallback。

M9-A 设计确认：

- 当前分支 SQL eligibility 仍只允许 `JT_ALL`，`JT_RANGE` / `JT_REF` / `JT_INDEX_SCAN` / `JT_CONST` 均应返回 `NON_FULL_TABLE_SCAN`；
- 当前 access path factory 只在 `TABLE_SCAN` 尝试创建 PQ iterator，M9-A 不新增 `INDEX_RANGE_SCAN` / `REF` factory；
- 当前 InnoDB PQ leader path 对非 clustered index 有 hard guard，M9-A 不放开；
- `CopyRangeScanAccessPath()` 仍 fail-closed，M9-A 不迁移 range clone；
- secondary ref/range/covering/ICP-on/off 的实际 SELECT 不应增加 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_ranges_built`、`Parallel_ranges_dispatched`。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_range_planning_dop pq_range_dispatch_dop1 pq_read_threaded_dop2_multirange pq_explain_fallback --parallel=1 --vardir=/tmp/pqv_m9 --tmpdir=/tmp/pqt_m9
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9_full --tmpdir=/tmp/pqt_m9_full
```

若完整 suite 因耗时或环境问题未执行，Completion Report 必须明确说明原因，并至少保留 ref/ICP targeted suite、build、review Agent 检视结论。

## Completion Report

M9-A completed by Codex Orchestrator.

Design notes:

- Commercial implementation uses `PQblockScanIterator`, `PQRefIterator`, handler `pq_ref_*` state, `pq_ref_build_ranges()`, worker-side ICP clone/refix and InnoDB secondary-index row production.
- Current branch does not have a safe secondary/ref/ICP execution path. M9-A therefore only adds negative guards before any positive migration.
- Two read-only Agents agreed M9-B+ must be split further and must not be mixed with M9-A tests.

Changed files:

- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- 新增 `pq_commercial_ref_icp` negative MTR；
- 覆盖 secondary ref、secondary range + ICP on、secondary range + ICP off、secondary covering index scan、multi-table ref；
- 实际 SELECT counter window 验证 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_ranges_built`、`Parallel_ranges_dispatched` 均不增加；
- EXPLAIN rows 估算列使用 `--replace_column 10 #` 屏蔽，避免统计估算在单跑/全套中波动；
- 未修改 `sql/` 或 `storage/innobase/` 执行路径。

Review:

- 第一轮 Review Agent 发现 ICP-off secondary range 和 multi-table ref 只有 EXPLAIN，缺少 runtime counter guard；
- 已补充两类 actual SELECT，并重新 record；
- 第二轮 Review Agent 确认 blocker 已解决，未发现提交前必须修改项，建议可提交。

Validation:

```bash
cd build-ninja/mysql-test
./mtr --suite=parallel_query --record pq_commercial_ref_icp
./mtr --suite=parallel_query pq_commercial_ref_icp pq_not_support pq_stats
./mtr --suite=parallel_query
```

Result:

- M9-A targeted suite passed；
- full current `parallel_query` suite passed，74 tests successful。

Remaining:

- M9-B0 已确认 M9-B 需要拆成 candidate probe、secondary partition、callback row production 三段，不得一次性打开正例；
- M9-B1 前不得放开 `NON_FULL_TABLE_SCAN` fallback，也不得修改 InnoDB secondary row production；
- M9-C/M9-D/M9-E/M9-F 继续拆分 ref、dependent ref、ICP pushdown 和边角能力；
- 正例阶段前必须先明确 `AccessPath::INDEX_RANGE_SCAN`、range clone、InnoDB secondary range partition、record buffer/回表/可见性和 ICP Item 生命周期。

M9-B0 completed by Codex Orchestrator.

Design notes:

- 当前分支 `pq_check_full_table_scan()` 明确拒绝非 `JT_ALL`；
- `access_path.cc` 只在 `TABLE_SCAN` 分支尝试 PQ；
- `CopyRangeScanAccessPath()` 返回 `nullptr`；
- `PQblockScanIterator` / `PQRefIterator` 仍 fail-closed；
- InnoDB `pq_leader_scan_init()` 对非 clustered index 返回 unsupported；
- `InnoDB_pq_scan_ctx::partition()` 使用 empty `Parallel_reader::Scan_range{}`，只适合 clustered full scan。

Review:

- M9-B 设计检视 Agent 确认不建议直接进入 secondary range 正例；
- 建议先做 M9-B0 设计与护栏，再按 M9-B1/B2/B3 拆分推进。

Validation:

- Design-only；无源码改动；
- 复用 M9-A full suite 结果：完整当前 `parallel_query` suite 74 项通过。

M9-B1 completed by Codex Orchestrator.

Changed files:

- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/m9-b0-secondary-range-design.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- 新增 secondary range candidate probe counters；
- eligibility 仍在 `JT_RANGE` 返回 `NON_FULL_TABLE_SCAN`；
- 仅当 `AccessPath::INDEX_RANGE_SCAN` 的 key 是非 PRIMARY key 时记录 candidate；
- 本阶段只增长 `Parallel_secondary_range_probe_attempts` / `Parallel_secondary_range_probe_unsupported`；
- `Parallel_secondary_range_clone_attempts`、`Parallel_secondary_range_clone_failed`、`Parallel_secondary_ranges_built`、`Parallel_secondary_rows_produced` 保持 0，用于证明未进入 clone/partition/row production；
- 不创建 PQ iterator，不修改 `access_path.cc`，不修改 InnoDB secondary row production。
- Review 修正：probe 门控必须显式要求 `range_scan()->type == AccessPath::INDEX_RANGE_SCAN`，避免误计 skip scan / group skip scan；
- 新增 primary range 负向护栏，确认 PRIMARY range 不增长 secondary range probe。

Review:

- 第一轮 Review Agent 发现 `JT_RANGE` 不等价于 `INDEX_RANGE_SCAN`，原门控可能误计 skip scan / group skip scan；
- 已收紧为 `JT_RANGE && range_scan != nullptr && range_scan->type == AccessPath::INDEX_RANGE_SCAN && secondary key`；
- 第二轮 Review Agent 确认 blocker 已解决，建议可提交。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --record pq_commercial_ref_icp pq_stats
./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support
```

Result:

- `mysqld` build passed；
- M9-B1 targeted suite passed；
- full current `parallel_query` suite passed，74 tests successful。
