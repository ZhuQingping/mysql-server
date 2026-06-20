# M9 Secondary Index / Ref / ICP Taskbook

## 状态

M9-A Completed。M9-B0 Completed。M9-B1 Completed。M9-B2 Completed。M9-B3 Completed。M9-C0 Design Taskbook Created。M9-C1 Completed / Review Accepted。M9-C2 Completed / Review Accepted。M9-D0 Design Accepted。M9-D1 Dependent Ref Negative Guard completed / Review Accepted。M9-D2 Ref-key Dispatch Smoke completed / Review Accepted。M9-D3a User-visible Dependent Ref Gate design completed / Review Accepted。M9-D3b Iterator Scaffold completed / Review Accepted。M9-D3c Single-probe Buffering Smoke completed / Review Accepted。M9-D3d User-visible Leader-local Gate completed / Review Accepted。M9-E ICP Pushdown design accepted。M9-E0 ICP negative guard coding/validation/review completed。M9-E1a Leader-local ICP contract design accepted。M9-E1b coding taskbook accepted / coding blocked by missing stable covering ICP positive shape。M9-E1c non-covering ICP + clustered lookup contract design accepted。M9-E1c-0 detailed contract review accepted。M9-E1c-1 coding/review completed。M9-E1c-2 design accepted。M9-E1c-2a coding/validation/review completed。M9-F Planned。

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

M9-E 设计拆分详见 [m9-e-icp-pushdown.md](m9-e-icp-pushdown.md)：

- M9-E0: ICP negative guard and DBUG smoke；
- M9-E1a: leader-local covering secondary range ICP contract design；
- M9-E1b: leader-local covering secondary range ICP coding，只有 E1a review
  通过后才允许进入；
- M9-E1c: non-covering secondary range ICP + clustered lookup contract；
- M9-E2: constant covering ref ICP；
- M9-E3: dependent ref ICP contract；
- M9-E4: worker-side ICP clone/refix。

M9-E0 当前边界：

- 只新增 MTR negative guard，不改 SQL/InnoDB 执行路径；
- secondary range ICP 使用 `FORCE INDEX(k_idx)` +
  `WHERE k BETWEEN 20 AND 40 AND v > 150`，`EXPLAIN` 稳定显示
  `Using index condition`；
- constant ref / dependent ref 在当前测试表结构下不会稳定生成剩余
  pushed index condition，因此作为 adjacent boundary guard 保留，不声明
  已覆盖真实 ref ICP；
- negative counter window 验证
  `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced` 均不增长；
- `pq_commercial_ref_icp` record/replay、`mysqld` build、完整
  `parallel_query` suite 74/74 通过；Code/Task Review Agent 复审
  `ACCEPT`。

M9-E1a 当前边界：

- 两个只读 Explorer Agent 均建议 E1 不直接编码；
- 当前 SQL gate 与 InnoDB producer 都显式拒绝
  `pushed_idx_cond` / `prebuilt->idx_cond`；
- 现有 producer 写入 `handler/table->record[0]` 后由 SQL sink deep-copy，
  尚未建立 ICP template / `idx_cond_n_cols` / pushed key / end-range
  等价契约；
- E1b 若进入编码，必须先通过 E1a Design Review，且范围只能是
  leader-local、single-threaded、single-table、strict covering integer
  secondary forward range，不涉及 worker clone、ref/dependent ref、
  non-covering clustered lookup、reverse、partition。
- E1a Design Review Agent 返回 `ACCEPT`，无 blocking findings。

M9-E1b 当前边界：

- coding taskbook 已生成并通过 review；
- 只允许 leader-local covering secondary range ICP；
- 必须使用 InnoDB 窄 wrapper/helper 复用 serial ICP 语义，不允许裸调
  `pushed_idx_cond->val_int()`；
- 必须保持 public handler virtual signature 不变；
- 必须证明 ICP 过滤发生在 `row_sink->send_row()` 前，被过滤记录不增长
  `Parallel_secondary_rows_produced`；
- non-covering ICP、constant ref ICP、dependent ref ICP 必须用独立 counter
  window 证明继续 fallback。
- E1b Coding Taskbook Review Agent 首轮 `REVISE`，修正文件边界、
  unsupported fallback delta 和状态清单后最终 `ACCEPT`。
- 编码入口探测确认：覆盖候选 `k_v_idx` / `k_pad_idx` 只产生
  `Using where; Using index`，不产生 `Using index condition`；非覆盖
  `k_idx + v predicate` 才产生 ICP，但超出 E1b 范围。源码探测改动已移除，
  当前不提交 E1b 源码。

M9-E1c 当前边界：

- design-only；
- 目标是 non-covering secondary range ICP + clustered lookup contract；
- 当前已有 clustered lookup visibility helper，但不支持 non-covering row
  materialization 后继续 secondary drain；
- 不直接打开用户可见支持，先审查 latch/mtr、clustered lookup
  continuation、materialization、ICP ordering、counter semantics。
- Design Review Agent 已返回 `ACCEPT`，确认 E1c 是 E1b blocked 后的合理
  下一步；后续继续拆为 E1c-0/E1c-1/E1c-2。
- E1c-0 detailed contract 已通过 Design Review；E1c-1 仍只能是 debug-only
  one-record smoke，E1c-2 才允许考虑 user-visible non-covering range gate。
- E1c-1 coding/review 已完成；该阶段未打开 user-visible ICP path。
- E1c-2 user-visible gate design 已通过 Design Review；编码前必须继续
  保持 positive non-covering-safe predicate 与 continued drain / cursor
  restore contract。
- E1c-2a coding 已完成 targeted validation：新增 leader-local
  non-covering secondary range ICP iterator、handler/InnoDB producer、positive
  clustered materialization test；正例 `e1c2_executed_delta=1`、
  `e1c2_secondary_rows_produced_delta=1`，`workers/ranges=0`；ICP-off 与
  unsafe read-set fallback windows 均为 0。Code/Task Review Agent 复审
  `ACCEPT`。

## 允许修改

以下为 M9 整体历史允许范围。具体子阶段以各自 taskbook 的
Allowed/Forbidden Files 为准；M9-E1b 必须遵守
`m9-e-icp-pushdown.md`，禁止修改 `sql/handler.h` 和
`storage/innobase/handler/ha_innodb.h`，保持 public handler virtual
signature 不变。

- `sql/parallel_query/pq_iterators.*`
- `sql/handler.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/m9-b0-secondary-range-design.md`
- `Docs/pq_tasks/m9-b2-secondary-range-partition.md`
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

M9-B2 design completed by Codex Orchestrator.

Design notes:

- 当前分支 `InnoDB_pq_scan_ctx::partition()` 只使用空 `Parallel_reader::Scan_range{}`，适合 clustered full scan，不具备 secondary range boundary carrier；
- 商用实现 `pq_range_scan_init()` 遍历 `mrr_funcs.next()`，用 `start_key/end_key` 调用 `index_read()` 定位 `range_start/range_end` tuple，再用 `PQ_Borders` / `PQ_Config` 构造 B+tree partitions；
- M9-B2 必须先定义 SQL range metadata、handler carrier、InnoDB boundary tuple deep-copy 和 partition counter contract；
- M9-B2 不允许创建 secondary PQ iterator、不允许让 `PQblockScanIterator::Read()` / `PQRefIterator::Read()` 读取真实 row、不允许接入 ICP/回表；
- 普通 secondary range SELECT 仍必须 fallback，`Parallel_secondary_rows_produced` 必须保持 0。

Changed files:

- `Docs/pq_tasks/m9-b2-secondary-range-partition.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Review:

- M9-B2 design Review Agent APPROVE；
- 未发现必须修改项；
- implementation residual risks 已写入 [m9-b2-secondary-range-partition.md](m9-b2-secondary-range-partition.md)。

Validation:

- Design-only；无源码改动；
- 未运行 build/MTR。

M9-B2 implementation completed by Codex Orchestrator.

Changed files:

- `sql/handler.h`
- `sql/parallel_query/pq_optimizer.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/m9-b2-secondary-range-partition.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- 新增 debug-only secondary range partition smoke；
- SQL 层从 `QUICK_RANGE` endpoint deep-copy key buffer，再调用 handler smoke API；
- InnoDB 层用 `row_sel_convert_mysql_key_to_innobase()` 构造临时 start/end tuple，并通过 `Parallel_reader::export_scan_ranges()` 生成 partitions；
- 当前只接受 forward half-open range；不能由 `Parallel_reader::Scan_range(start,end)` 精确表达的边界继续 fail-closed；
- 不修改 iterator factory，不打开 secondary row production；
- MTR 验证 clone attempts +1、clone failed 0、secondary ranges built +1、secondary rows produced 0；
- 普通 secondary range/ref/ICP SELECT 继续 fallback。

Review:

- 第一轮代码 Review Agent 发现 boundary blocker；
- 已修正为 only half-open forward range built，其它边界 fail-closed；
- 第二轮代码 Review Agent APPROVE。

Validation:

- `mysqld` build passed；
- M9-B2 record passed；
- M9-B2 targeted suite passed；
- full current `parallel_query` suite passed，74 tests successful。

M9-B3 design taskbook created by Codex Orchestrator.

Design boundary:

- 详见 [m9-b3-secondary-range-row-production.md](m9-b3-secondary-range-row-production.md)；
- M9-B3 默认先做 covering secondary range callback row production，不做 non-covering cluster lookup；
- 继续限定 half-open forward range：`k >= a AND k < b`；
- 不打开 `JT_REF`、dependent ref、ICP、reverse、partition table、MVI/spatial/descending keypart；
- B3 第一实现建议为 debug-only smoke，证明 `Parallel_secondary_rows_produced` 可控增长；
- 用户可见 secondary range PQ gate 必须在 B3a review 通过后单独进入 B3b。

Validation:

- Design-only；无源码改动；
- 未运行 build/MTR。

M9-B3a coding entry blocked by Codex Orchestrator.

Blocking reason:

- 当前 `Parallel_reader::check_visibility()` 对 secondary index under active
  read view 仍走 `ut_error`；
- 当前 `produce_callback_rows_for_range()` 保持 clustered-only；
- 按 M9-B3 design review 已接受的规则，无法证明 secondary callback
  visibility/delete-mark 前不得实现 row smoke。

Next:

- 先进入 M9-B3a-0 Secondary Visibility Helper；
- 对齐商用 `PQ_Scan_ctx::find_visible_record()` 的最小 secondary visibility
  语义；
- M9-B3a-0 review 通过后再重新评估 covering secondary row smoke。

M9-B3a-0 Secondary Visibility Helper fail-closed contract implemented by Codex
Orchestrator.

Changed files:

- `sql/handler.h`
- `sql/mysqld.cc`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/sql_parallel.h`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/m9-b3-secondary-range-row-production.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`

Implementation notes:

- 新增 handler debug-only `pq_secondary_visibility_smoke()`；
- 新增 `InnoDB_pq_scan_ctx::validate_secondary_visibility_contract()`；
- 当前 helper 对 secondary index under active read view 稳定 fail-closed，不读取 row；
- 新增 `Parallel_secondary_visibility_attempts` /
  `Parallel_secondary_visibility_unsupported`；
- optimizer 只在 `pq_secondary_visibility_smoke` DBUG flag 下触发；
- 普通 secondary range/ref/ICP SELECT 继续 fallback；
- 不修改 `access_path.cc`，不创建 secondary PQ iterator，不产生 row；
- `Parallel_secondary_rows_produced` 保持 0。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --record pq_commercial_ref_icp pq_stats` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a0 --tmpdir=/tmp/pqt_m9b3a0` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a0_full --tmpdir=/tmp/pqt_m9b3a0_full` 通过，完整 suite 74 项成功。

Review:

- Review Agent Dewey returned `APPROVE`；
- No blocker；
- Packaging reminder: include the new M9-B3 taskbook explicitly and exclude
  unrelated dirty/untracked files。

M9-B3a-1 Secondary Visibility Fast-path design created by Codex Orchestrator.

Design notes:

- 不直接迁移完整 commercial clustered lookup 分支；
- 当前分支缺少 commercial `pq_row_sel_get_clust_rec_for_mysql()` public
  wrapper，`row_search_idx_cond_check()` 仍是 static；
- B3a-1 只做 covering secondary visibility fast-path；
- `view->sees(page_get_max_trx_id(page_align(rec)))` 必须成立；
- uncertain page 必须 whole-smoke fail-closed，不能 skip row 后继续；
- clustered lookup for visibility 后置到 B3a-2；
- 用户可见 execution gate 仍禁止。

Review:

- Pending M9-B3a-1 design Review Agent。

M9-B3a-1 Secondary Visibility Fast-path helper implemented by Codex
Orchestrator.

Implementation notes:

- 新增 `InnoDB_pq_scan_ctx::validate_secondary_visibility_fast_path()`；
- 显式要求 active `m_trx->read_view`，不复用 `has_active_read_view()`；
- read-only mode without active read view 继续 unsupported；
- fast-path 只接受 no ICP、no clustered access、`LOCK_NONE`、matching
  secondary prebuilt index、page max trx id visible、record not delete-marked；
- uncertain page 返回 `DB_UNSUPPORTED`；
- 新增 `Parallel_secondary_visibility_supported`；
- 当前 smoke 没有安全 secondary record source，因此 supported delta 仍为 0，
  unsupported delta 仍为 1；
- 不产生 row，`Parallel_secondary_rows_produced` 保持 0。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --record pq_commercial_ref_icp pq_stats` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a1 --tmpdir=/tmp/pqt_m9b3a1` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a1_full --tmpdir=/tmp/pqt_m9b3a1_full` 通过，完整 suite 74 项成功。

Review:

- Review Agent Curie returned `APPROVE`；
- No blocker；
- Open question for B3a-2: define explicit `mtr_t *` / latch contract for
  secondary visibility helpers before row smoke work。

M9-B3a-2 clustered lookup for visibility design created by Codex Orchestrator.

Design notes:

- 需要新增窄 `pq_row_sel_get_clust_rec_for_mysql()` wrapper；
- 允许触碰 `storage/innobase/include/row0sel.h` 与
  `storage/innobase/row/row0sel.cc`，但只新增 wrapper；
- wrapper 不改变现有 `row_search_mvcc()` 行为；
- caller 提供 active `mtr_t *`，secondary record 和返回 clustered record 的
  lifetime 都受该 mtr 保护；
- B3a-2 只允许 one-record-and-stop，不允许 clustered lookup 后继续推进
  secondary cursor；
- fast-path success 必须保留 secondary delete-mark check；
- helper 必须硬门控 `prebuilt->index == sec_index/m_index`；
- intrinsic table explicitly unsupported；
- clustered lookup 只用于 visibility/delete-mark；
- 不 materialize MySQL record，不调用 row sink，不打开 execution gate。

Review:

- Review Agent Hooke returned `REVISE`；
- Design has been revised；
- Review Agent Hooke returned `ACCEPT` on re-review。

M9-B3a-2 clustered lookup visibility helper implemented by Codex
Orchestrator.

Implementation notes:

- 新增窄 `pq_row_sel_get_clust_rec_for_mysql()` wrapper；
- 新增 `InnoDB_pq_scan_ctx::validate_secondary_visibility_with_cluster_lookup()`；
- helper 要求 active read view、secondary index、`LOCK_NONE`、no ICP、
  matching prebuilt index、matching prebuilt trx、non-intrinsic table、
  non-null `prebuilt->clust_pcur`、active `mtr_t *`；
- helper 只做 visibility/delete-mark validation，不 materialize MySQL row，
  不 enqueue row，不打开 execution gate；
- `pq_secondary_visibility_smoke()` 对 intrinsic table fail closed。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a2_r2 --tmpdir=/tmp/pqt_m9b3a2_r2` 通过；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a2_full_r2 --tmpdir=/tmp/pqt_m9b3a2_full_r2` 通过，完整 suite 74 项成功。

Review:

- Review Agent Parfit returned `APPROVE`；
- No blocker；
- Minor documentation gap for `prebuilt->clust_pcur != nullptr` has been fixed。

M9-B3 secondary range row production completed by Codex Orchestrator.

Summary:

- 详见 [m9-b3-secondary-range-row-production.md](m9-b3-secondary-range-row-production.md)；
- 已完成 secondary visibility fail-closed contract、fast-path helper、
  clustered lookup visibility helper、one-record materialization smoke、
  range materialization smoke 和用户可见 covering secondary range gate；
- 用户可见 gate 只允许 strict covering integer secondary forward range；
- runtime `HA_ERR_UNSUPPORTED` 回退串行；
- `SELECT k ... WHERE k >= 20 AND k < 40` 返回 3 行并使
  `Parallel_secondary_rows_produced` 增长 3；
- composite child hook 和 unsafe non-projected keypart 风险已由 Review Agent
  提出并修复；
- Review Agent 最终复核 `ACCEPT`；
- commit: `233e8f039df Add PQ M9B3 secondary range row production gate`。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query` passed, 74/74。

M9-C0 constant JT_REF minimal design created by Codex Orchestrator.

Summary:

- 任务书：[m9-c-jt-ref-minimal.md](m9-c-jt-ref-minimal.md)；
- 商用实现使用 `PQRefIterator -> pq_ref_build_ranges() -> ha_pq_next()`；
- 当前分支 worker pull-row 仍显式 disabled，因此 C0 不建议直接搬商用
  worker ref 路线；
- C1 建议先做 debug-only ref-to-range endpoint bridge，复用 B3d
  `pq_secondary_covering_range_produce()`；
- C2 再接用户可见 constant covering ref gate；
- dependent ref、ICP、non-covering、multi-table、worker/MQ 全部后置。

M9-D3d user-visible leader-local dependent ref gate implemented by Codex
Orchestrator.

Summary:

- 详见 [m9-d3-dependent-ref-gate-plan.md](m9-d3-dependent-ref-gate-plan.md)；
- 新增 `PQSecondaryDependentRefIterator`；
- 仅允许 two-table、simple query、no group/having、no reverse、InnoDB
  non-partitioned covering secondary dependent `REF`；
- D3c DBUG hook 优先保留，hook 开启时仍由原生 `RefIterator<false>`
  返回用户可见 rows；
- 默认用户可见路径按 probe 执行 `impossible_null_ref()`、
  `construct_lookup()`、deep-copy `key_buff`、leader-local
  `pq_secondary_covering_ref_produce()`；
- 当前 probe rows 缓存在 SQL-owned buffer；
- empty probe 返回 EOF；
- `HA_ERR_UNSUPPORTED` 只在当前 probe 未对外返回 row 前回退原生
  `RefIterator<false>`；若 producer 已写入内部 buffer 但尚未返回 visible
  row，则丢弃 buffer 后 serial fallback；
- statement-level buffered row cap overflow 视为 pre-visible unsupported
  fallback；
- `Parallel_queries_executed` 每 statement 最多 +1；
- `Parallel_secondary_rows_produced` /
  `Parallel_secondary_ref_rows_produced` 按真实 inner rows 增长；
- `Parallel_workers_launched`、`Parallel_ranges_built`、
  `Parallel_ranges_dispatched` 保持 0。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query` passed, 74/74。

Observed MTR counters:

- `d3d_ref_probe_attempts_delta = 5`；
- `d3d_ref_probe_unsupported_delta = 0`；
- `d3d_ref_empty_probes_delta = 1`；
- `d3d_ref_fallback_probes_delta = 0`；
- `d3d_ref_rows_produced_delta = 7`；
- `d3d_executed_delta = 1`；
- `d3d_workers_delta = 0`；
- `d3d_ranges_built_delta = 0`；
- `d3d_ranges_dispatched_delta = 0`；
- `d3d_secondary_rows_produced_delta = 7`。
- D3d fallback-after-buffer debug block:
  - `d3d_fallback_ref_probe_unsupported_delta = 1`；
  - `d3d_fallback_ref_fallback_probes_delta = 1`；
  - `d3d_fallback_executed_delta = 0`；
  - `d3d_fallback_secondary_rows_produced_delta = 0`。

Review:

- First Code/Task Review Agent returned `REVISE`；
- Fixed pre-visible partially-buffered unsupported fallback；
- Fixed statement buffer cap overflow fallback；
- Added serial baseline / unsorted D3d order comparison；
- Added debug-only fallback-after-buffer MTR；
- Re-review Agent returned `ACCEPT`；
- Non-blocking residual risks:
  - 1024 statement cap fallback is code-reviewed but not directly forced by
    MTR；
  - per-probe fallback after earlier PQ-visible probes remains allowed；
  - join shape remains protected by current two-table/simple/nested-loop REF
    gates rather than an explicit join-type enum check。

M9-E design taskbook created by Codex Orchestrator.

Summary:

- 任务书：[m9-e-icp-pushdown.md](m9-e-icp-pushdown.md)；
- 两个只读调研 Agent 均确认当前分支不适合直接打开用户可见 ICP；
- 商用 worker-side ICP 依赖 `pq_cond`、`pushed_idx_cond` clone/refix、
  worker `idx_cond_push()`、InnoDB ICP template、`row_search_idx_cond_check()`
  和真实 `PQblockScanIterator` / `PQRefIterator`；
- 当前分支 secondary range/ref/dependent-ref 仍是 leader-local covering gate，
  且 SQL iterator/InnoDB producer 都显式拒绝 `pushed_idx_cond` /
  `prebuilt->idx_cond`；
- M9-E0 先做 negative guard + DBUG smoke，不迁移
  `make_cond_for_index()` / `make_cond_remainder()`，不调用
  `pushed_idx_cond->val_int()`，不打开 worker/MQ。

Review:

- First Design Review Agent returned `REVISE`；
- Taskbook tightened:
  - E0 ICP SQL shapes are explicit；
  - ICP-on `EXPLAIN` must show `Using index condition`；
  - ICP-off baseline must match ICP-on results；
  - negative window must report `executed=0`、
    `secondary_rows_produced=0`、`workers=0`、`ranges_built=0`、
    `ranges_dispatched=0`；
  - Forbidden Files include iterator factory、eligibility、handler/InnoDB
    headers and executor entry points；
  - optional DBUG smoke must not add API or alter runtime gate。
- Design Re-review Agent returned `ACCEPT`。
