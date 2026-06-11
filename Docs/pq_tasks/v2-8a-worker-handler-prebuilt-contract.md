# V2-8A Worker Handler/Prebuilt Contract Design 任务书

## Goal

V2-8A 是 V2-8 真实 full scan 闭环前的设计 gate。目标是明确 worker handler / `row_prebuilt_t` / read view / `PQ_Worker_context` 的 ownership 和 cleanup contract，避免在未解决 InnoDB mutable scan state 前直接打开真实 row scan。

本阶段默认不改源码，除非发现必须先修复的文档或测试边界。输出应服务于后续 V2-8B/C 编码。

## Current Baseline

- Baseline commit: `56e9b3bc1ad Plan PQ V2-8 full scan closure`
- V2-4: `Parallel_reader::export_scan_ranges()` 和 range planning observable 已完成。
- V2-5: worker lifecycle smoke 已完成，但不创建真实 OS worker THD。
- V2-6: synthetic MQ ROW/FINISH smoke 已完成，但不 materialize row。
- V2-7: predicate/projection boundary 已完成，worker 不 clone Item/JOIN。
- InnoDB `pq_worker_scan_init()` / `pq_worker_scan_next()` 仍返回 unsupported。

## Questions To Resolve

1. SQL 层是否需要真实 worker THD / worker handler；如果需要，谁创建、谁销毁？
2. worker 是否必须拥有独立 `ha_innobase` / `row_prebuilt_t`，还是可以采用 InnoDB-internal cursor 绕过 handler clone？
3. read view 由 leader 创建并 pin，还是 worker 各自创建；RR/RC 语义如何保持？
4. `PQ_Worker_context` 如何建立真实类型层次，替代当前潜在 `reinterpret_cast` 风险？
5. DOP=1 real full scan 可以接受哪些简化；DOP=2/4 前必须补哪些 boundary？
6. fatal-after-start、kill、early EOF、OOM 的 cleanup 顺序如何定义？

## Active Dispatch

### Design Explorer A - SQL/Handler Boundary

- Agent: `019eb54a-ffe4-7331-954c-bd0371ce4523` (`Kuhn`)
- Mode: read-only
- Scope:
  - `sql/parallel_query/pq_iterator.*`
  - `sql/parallel_query/sql_parallel.*`
  - `sql/handler.h`
  - `storage/innobase/handler/ha_innodb.*` pq API surface
- Expected output:
  - SQL ownership design；
  - iterator state machine；
  - handler API gap；
  - allowed/forbidden files；
  - tests.

### Design Explorer B - InnoDB Prebuilt / Read View / Trx

- Agent: `019eb54b-2e6a-75b2-9682-85fc9cff12fe` (`Boole`)
- Mode: read-only
- Scope:
  - `storage/innobase/handler/ha_innodb.cc`
  - `storage/innobase/handler/ha_innodb.h`
  - `storage/innobase/include/row0pread_pq.h`
  - `storage/innobase/row/row0pread_pq.cc`
  - `row_prebuilt_t` / `row_search_mvcc` related definitions
- Expected output:
  - prebuilt/trx/read-view ownership；
  - worker independent scan state options；
  - cleanup/error/kill order；
  - cast/type hierarchy fix.

### Local Orchestrator Task - Range Dispatch / Boundary

第三个 range dispatch explorer 因 subagent 数量限制暂未启动，由 Codex 主控本地调研：

- `storage/innobase/include/row0pread.h`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`

输出：

- start/end boundary 是否真正被 worker scan 使用；
- DOP=1 简化范围；
- DOP=2/4 前必须补的 no-duplicate/no-missing gate；
- row image 生成位置建议。

### Local Range Dispatch Findings

- `storage/innobase/row/row0pread_pq.cc:217`
  `InnoDB_pq_ctx::read_record()` 当前只在首次读取时调用
  `row_search_mvcc(..., ROW_SEL_EXACT, 0)` 定位到 clustered index 起点，
  后续调用 `ROW_SEL_NEXT`。它没有使用 `InnoDB_pq_range::m_start` /
  `m_end` boundary。
- `storage/innobase/row/row0pread_pq.cc:366`
  `InnoDB_pq_leader_ctx::dispatch_next_range()` 通过普通
  `m_next_range_id++` 分配 range；当前没有 mutex/atomic。真实并发 worker
  下不能直接使用。
- `storage/innobase/row/row0pread_pq.cc:138`
  `InnoDB_pq_scan_ctx::partition()` 已将
  `Parallel_reader::export_scan_ranges()` 的 boundary 深拷贝进
  `InnoDB_pq_iter`，所以 boundary metadata 已存在，缺的是 scan 使用它。
- V2-8C 若只做 DOP=1 real full scan，可暂时接受 whole-index scan；
  但必须显式 gate：`actual_dop == 1` 且只 dispatch 一个 range，否则结果会
  重复或漏读。
- V2-8D 前必须补：
  - range start seek；
  - end boundary 截断；
  - dispatch_next_range 线程安全；
  - no duplicate / no missing MTR。
- Row image 生成建议先沿用 `row_search_mvcc()` 写入 worker-side
  MySQL record buffer，然后通过 MQ copy record image 给 leader；
  不建议 V2-8A 引入低层 InnoDB rec 转 MySQL record 的新转换路径。

## Allowed Files For V2-8A

- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8-single-table-fullscan-closure.md`
- `Docs/pq_tasks/v2-8a-worker-handler-prebuilt-contract.md`
- 可选：`Docs/pq_tasks/v2-execution-path-roadmap.md`
- 可选：`Docs/pq_tasks/v2-test-matrix.md`

## Forbidden Files For V2-8A

- `sql/**` 源码行为修改；
- `storage/innobase/**` 源码行为修改；
- 新增真实 execution MTR；
- 启动真实 worker row scan；
- 设置 `PQ_execution_state::EXECUTED` 或真实 execution counters。

## Acceptance Checklist

- [ ] SQL/handler boundary explorer 完成；
- [ ] InnoDB prebuilt/read-view explorer 完成；
- [ ] range dispatch 本地调研完成；
- [ ] 形成 V2-8A design conclusion；
- [ ] 明确 V2-8B/C 的 implementation gates；
- [ ] 更新 README 当前状态。

## Current Status

- Status: In Progress
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Active agents:
  - `Kuhn`: SQL/handler boundary
  - `Boole`: InnoDB prebuilt/read-view/trx

## Completion Report

待 explorer 返回后补充。
