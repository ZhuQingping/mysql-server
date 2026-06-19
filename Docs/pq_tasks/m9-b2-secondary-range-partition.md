# M9-B2 Secondary Range Partition Contract

## 状态

Design Ready，等待实现阶段。本文档只定义 M9-B2 的接口契约、任务边界和验收项；不修改源码。

## 目标

M9-B2 的目标是为 secondary `INDEX_RANGE_SCAN` 建立 InnoDB range partition 前置能力，让后续 M9-B3 可以在同一个 contract 上接入 callback row production。

本阶段不打开真实 secondary row production，不创建用户可见 PQ 正例，不放开默认 fallback。实现阶段即使能够构造 secondary ranges，也必须继续让查询串行执行，或者只在明确的 debug/smoke 入口里验证 partition counters。

## 当前分支与商用实现差异

当前分支：

- `pq_check_full_table_scan()` 在 `JT_RANGE` 上仍返回 `NON_FULL_TABLE_SCAN`；
- M9-B1 只在 `JT_RANGE && AccessPath::INDEX_RANGE_SCAN && secondary key` 时记录 candidate probe；
- `CopyRangeScanAccessPath()` 仍 fail-closed；
- `pq_leader_scan_init()` 选择 `m_prebuilt->index` 或 clustered first index，并拒绝非 clustered index；
- `InnoDB_pq_scan_ctx::partition()` 使用空 `Parallel_reader::Scan_range{}`，适配 clustered full scan；
- `produce_callback_rows_for_range()` 明确要求 `m_index->is_clustered()`。

商用实现：

- `sql_parallel.cc::SetupPQTab()` / `InitPQTab()` 根据 `QEP_TAB::range_scan()` 设置 `keyno`、`pq_range_type`、range iterator reverse state；
- `ha_innodb_pq.cc::pq_range_scan_init()` 遍历 `mrr_funcs.next()` 得到 `mrr_cur_range`；
- 通过 `start_key/end_key` 调用 `index_read()` 定位真实 secondary tuple 边界；
- 构造 `PQ_Borders{range_start, range_end}` 和 `PQ_Config`，再调用 `Parallel_leader::build_ranges()`；
- `row0pread_pq.cc::PQ_Scan_ctx::partition()` / `create_ranges()` 基于 B+tree page 边界拆出多个 scan ranges；
- secondary row production、ICP、回表和 visible version 处理在后续 read path 中完成。

## M9-B2 实现范围

允许实现的最小能力：

- 从 SQL 层保留 secondary `INDEX_RANGE_SCAN` 的 key number、range metadata 和 reverse flag；
- 只支持单表 InnoDB、单 secondary index、forward range；
- 只支持 non-partition table；
- 不支持 `JT_REF`、dependent ref、dynamic range、skip scan、group skip scan；
- 不支持 ICP pushdown；`pushed_idx_cond` / `idx_cond` 存在时继续 fallback；
- 不支持 reverse scan；
- 不支持 MVI、spatial、descending keypart、partition table；
- InnoDB 只构造 secondary range boundaries 和 exported partitions；
- 增长 `Parallel_secondary_ranges_built` 只能表示 partition 已构造，不表示执行了 secondary row；
- `Parallel_secondary_rows_produced` 必须保持 0。

禁止事项：

- 禁止把 `JT_RANGE` 查询直接改写为 PQ 正例；
- 禁止在 `access_path.cc` 为 `INDEX_RANGE_SCAN` 创建真实 PQ iterator；
- 禁止让 `PQblockScanIterator::Read()` / `PQRefIterator::Read()` 读取真实 row；
- 禁止在 M9-B2 接入 secondary record materialization、cluster lookup 或 ICP；
- 禁止复用 leader `row_prebuilt_t` 作为 worker mutable cursor；
- 禁止把 secondary range partition 和 callback row production 合并到同一个提交。

## 建议代码边界

实现阶段建议只修改：

- `sql/parallel_query/pq_clone.*` 或等价 metadata helper，用于记录/复制 secondary range scan metadata；
- `sql/parallel_query/pq_optimizer.cc`，继续保持 fallback，但为 M9-B2 smoke 提供更精确 diagnostics；
- `sql/handler.h` / `sql/parallel_query/pq_handler.h`，仅在确需 carrier 时新增最小 secondary range metadata；
- `storage/innobase/handler/ha_innodb_pq.cc`，新增 secondary range partition helper，默认不从真实执行入口调用；
- `storage/innobase/include/row0pread_pq.h`；
- `storage/innobase/row/row0pread_pq.cc`，新增带 explicit `Scan_range` / secondary boundary 的 partition helper；
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`；
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`；
- 本文档和总看板。

如实现需要修改 `sql/join_optimizer/access_path.cc` 或 `pq_iterators.*`，必须先把任务拆成 M9-B2.1 设计补充；M9-B2 默认不允许打开 iterator factory。

## 关键设计约束

### Range Metadata

M9-B2 必须明确 SQL 层保存的是 optimizer/range iterator 可重建信息，而不是悬空指针：

- `AccessPath::index_range_scan().ranges` 生命周期只在当前 plan 内可靠，不能跨 worker 直接裸用；
- `QUICK_RANGE` / `KEY_MULTI_RANGE` 的 key buffer 所有权必须明确；
- 如果无法安全 clone，就只记录 `Parallel_secondary_range_clone_attempts` / `Parallel_secondary_range_clone_failed`，继续 fallback。

### Boundary Semantics

商用实现使用 `index_read()` 将 SQL key range 转换为 InnoDB tuple 边界。M9-B2 必须保留以下语义：

- forward start: `HA_READ_AFTER_KEY` 保持 open lower bound，否则使用 `HA_READ_KEY_OR_NEXT`；
- forward end: `HA_READ_BEFORE_KEY` 使用 `HA_READ_KEY_OR_NEXT`，否则使用 `HA_READ_AFTER_KEY`；
- `HA_ERR_KEY_NOT_FOUND` 对 start/end 的含义不同，不能统一当作错误；
- 当前阶段不实现 reverse scan，所有 reverse range 必须 fallback。

### Partition Semantics

M9-B2 只证明 partition 可构造：

- partition range 的 start/end tuple 必须 deep-copy 到 `InnoDB_pq_iter` 或等价内存所有者；
- empty range 可以成功返回 0 ranges，但不能触发执行；
- range split counters 只说明 partition 建立，不说明 rows produced；
- M9-B3 前 `produce_callback_rows_for_range()` 对 secondary index 仍返回 unsupported。

## 验收标准

设计验收：

- 本文档说明 SQL metadata、handler carrier、InnoDB boundary、partition 和 row production 的分界；
- Review Agent 同意 M9-B2 不会提前打开 secondary execution；
- 总看板和 M9 taskbook 状态同步。

实现阶段验收建议：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b2 --tmpdir=/tmp/pqt_m9b2
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b2_full --tmpdir=/tmp/pqt_m9b2_full
```

预期：

- secondary range candidate probe 仍增长；
- `Parallel_secondary_ranges_built` 只在明确 smoke/debug 入口增长；
- 普通 secondary range SELECT 仍不增长 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_secondary_rows_produced`；
- 完整 `parallel_query` suite 继续通过。

## Agent 任务书

角色：Code Agent

主控：Codex

任务：M9-B2 Secondary Range Partition Contract Implementation

请先阅读：

- `AGENTS.md`
- `CLAUDE.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/m9-b0-secondary-range-design.md`
- `Docs/pq_tasks/m9-b2-secondary-range-partition.md`
- 当前分支 `storage/innobase/handler/ha_innodb_pq.cc`
- 当前分支 `storage/innobase/row/row0pread_pq.cc`
- 商用参考 `taurusdbondstore/storage/innobase/handler/ha_innodb_pq.cc`
- 商用参考 `taurusdbondstore/storage/innobase/row/row0pread_pq.cc`

任务要求：

1. 只做 secondary range partition 前置，不打开 row production；
2. 保持普通 secondary range 查询 fallback；
3. 如新增 smoke，必须证明 rows produced 为 0；
4. 如发现需要修改 iterator factory，停止并回报，不继续编码；
5. 完成后更新 Completion Report，不要提交。

## Completion Report

Design-only stage completed by Codex Orchestrator.

Review:

- M9-B2 design Review Agent APPROVE；
- Review 确认当前分支和商用实现描述准确；
- Review 确认文档覆盖 metadata 生命周期、start/end boundary flag、reverse fallback、ICP fallback、回表/row production 延后、partition table 禁止、iterator factory 禁止修改、worker prebuilt/cursor 不复用 leader mutable cursor 等核心风险。

Residual risks for implementation:

- `AccessPath::index_range_scan().ranges` / `QUICK_RANGE` / `KEY_MULTI_RANGE` key buffer 必须 deep-copy 或可重建，不能保存裸指针；
- forward boundary 的 `HA_ERR_KEY_NOT_FOUND` 分支需要覆盖：start after all 表示 empty range，end after all 表示 `+infinity`；
- `Parallel_secondary_ranges_built` 只能由明确 smoke/debug 入口增长；
- 普通 SELECT 仍必须保持 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_secondary_rows_produced` 不增长；
- ICP、reverse、partition table、descending keypart、MVI/spatial fallback 必须在实现门控中显式可见。

Validation:

- Design-only；无源码改动；
- 未运行 build/MTR。
