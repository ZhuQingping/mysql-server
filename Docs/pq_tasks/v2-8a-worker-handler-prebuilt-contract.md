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

## Explorer Results

### SQL/Handler Boundary Conclusions

- `PQTableScanIterator` owns `Gather_operator` and borrows leader `TABLE::file` from
  executor；iterator cleanup 必须先停止/释放 gather，再调用 leader handler 的
  `pq_leader_scan_end()`。
- `PQ_Leader_context` 是 handler API 返回的 opaque SQL-visible context。SQL 层不应
  缓存或解释 InnoDB internal ctx，只能把它传回同一个 handler API。
- worker context 必须通过 worker handler close API 释放，不能由 SQL 层直接
  `delete` InnoDB internal ctx。
- V2-8A 不应实现真实 OS worker 或真实 row scan。当前
  `PQ_worker_manager::start()` 仍是 smoke stub，InnoDB worker scan 仍返回
  unsupported；本阶段只收敛 contract。
- iterator 状态机应显式区分：
  - `NEW`
  - `LEADER_PROBED`：仍可安全 fallback
  - `FALLBACK_SERIAL`
  - `PQ_STARTING`：execution-only init，准备 read view / gather / worker ctx / MQ
  - `PQ_STARTED`
  - `PQ_READING`
  - `PQ_EOF` / `PQ_ERROR` / `CLOSED`
- commit point 必须明确：read view 已绑定、worker start 已越过、Exchange row
  protocol 已准备好。commit point 前 unsupported/OOM 可以 fallback；commit
  point 后所有错误都必须作为 PQ execution error 处理，不能再透明串行 fallback。
- handler API 需要补齐：
  - `pq_leader_scan_init()` 增加 probe/execute mode 或等价 flags，避免 probe
    阶段提前绑定 read view；
  - `pq_worker_scan_init()` 增加 worker open carrier，例如
    `PQ_Worker_open_context { THD*, TABLE*, handler*, worker_id }`；
  - `PQ_Worker_context` 建立真实 polymorphic type hierarchy，禁止
    `reinterpret_cast`；
  - row image ownership 由 V2-8B 单独定义：handler 写 worker-local MySQL
    record buffer，MQ copy fixed record image 给 leader。

### InnoDB Prebuilt / Read View / Trx Conclusions

- `ha_innobase::m_prebuilt` 是 handler 私有状态，由 `ha_innobase::open()` 创建，
  由 `ha_innobase::close()` 释放；`row_prebuilt_t` 明确禁止 copy/move，不是可共享
  scan descriptor。
- `row_prebuilt_t` 中 cursor、fetch cache、mysql template、BLOB/undo heap、
  lock/read mode、`m_mysql_table`、`m_mysql_handler` 等都是 mutable state，不能
  在 leader/worker 或多个 worker 间共享。
- `row_search_mvcc()` 每次读取都会修改 `row_prebuilt_t`，并访问
  `prebuilt->m_mysql_handler`；直接复用 leader `m_prebuilt` 不可行。
- `trx_t` 也不能作为 worker execution mutable state 盲目共享；
  `row_search_mvcc()` 会修改 `trx->op_info`，`trx_assign_read_view()` 也依赖当前
  线程可处理该 trx 的前提。
- read view 应由 leader 在 no-fallback execution boundary 创建并 pin，worker 使用
  同一个 statement snapshot 或等价受控 snapshot；worker 各自创建 read view 在
  RR/RC 下都可能偏离单条串行 SELECT 语义。
- 最小可行方案是 worker 拥有独立 `TABLE` row buffer、独立 `ha_innobase`
  handler、独立 `row_prebuilt_t`，但不 clone SQL `Item` / `JOIN`。仅 clone
  handler 不够，因为通用 `handler::clone()` 会复用同一个 `TABLE*`，导致
  `TABLE::record[0]`、row status、read_set 等 SQL mutable state 共享。
- cleanup 顺序必须是：
  1. 通知/kill workers，停止 MQ production；
  2. join/确认 worker 不再执行 `row_search_mvcc()`；
  3. 释放 worker cursor/prebuilt/handler/TABLE buffer；
  4. 释放 leader scan ctx/ranges；
  5. 最后 close/unpin read view 并释放 thread budget。
- 当前 `ha_innobase::pq_worker_scan_end()` 对 `PQ_Worker_context*` 做
  `reinterpret_cast<InnoDB_pq_worker_ctx*>` 是类型契约硬 gate。后续应引入
  `InnoDB_pq_sql_worker_context final : public PQ_Worker_context` wrapper，内部
  持有 InnoDB worker ctx、worker handler/TABLE owner、range id、EOF/error 状态。

## V2-8A Design Conclusion

V2-8A 结论是先固化 worker handler/prebuilt/read-view contract，不直接打开真实
worker row stream。V2-8 后续编码必须按以下顺序推进：

1. 先补 `PQ_Worker_context` 真实类型层次和 worker open context，消除
   `reinterpret_cast` 和 ownership 分裂。
2. 再补 V2-8B row image protocol：worker handler 写 worker-local MySQL record
   buffer，Exchange/MQ copy record image，leader 从 MQ materialize 到当前
   `TABLE::record[0]`。
3. V2-8C 只允许 DOP=1 real full scan MVP，且必须显式 gate
   `actual_dop == 1` 和 single range；此时可暂不使用 range start/end boundary。
4. V2-8D 才打开 DOP>1：必须实现 range start seek、end boundary 截断、
   thread-safe range dispatch，以及 no-duplicate/no-missing MTR。
5. 一旦越过 commit point 或 `Read()` 已经返回 PQ row，后续错误、kill、OOM
   都不能再 fallback 到 serial path。

## V2-8B/C Implementation Gates

- V2-8B 必须先定义并测试 row image ownership：
  - worker-local record buffer；
  - MQ payload copy；
  - leader-side materialization；
  - EOF/error token cleanup。
- V2-8C 必须先补 handler API contract：
  - probe vs execute mode；
  - worker open context；
  - typed worker wrapper；
  - leader read view commit point。
- 建议测试项：
  - `pq_worker_contract_probe`：eligible query 仍 probe/fallback，executed=0，
    fallback counter 增加；
  - `pq_no_fallback_after_start`：debug hook 模拟 post-start error，返回错误且
    不串行 fallback；
  - `pq_worker_ctx_cleanup`：partial worker init failure 清理 ctx/leader/thread
    budget；
  - `pq_read_view_boundary`：probe 不绑定 read view，execute commit 才绑定并清理；
  - `pq_fullscan_real_dop1`：后续 V2-8C DOP=1 real full scan smoke。

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

- [x] SQL/handler boundary explorer 完成；
- [x] InnoDB prebuilt/read-view explorer 完成；
- [x] range dispatch 本地调研完成；
- [x] 形成 V2-8A design conclusion；
- [x] 明确 V2-8B/C 的 implementation gates；
- [x] 更新 README 当前状态。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11
- Agents:
  - `Kuhn`: SQL/handler boundary completed
  - `Boole`: InnoDB prebuilt/read-view/trx completed

## Completion Report

V2-8A 已完成设计收敛。后续不应直接进入真实并行 full scan，而应先执行 V2-8B
row image protocol，再执行 V2-8C DOP=1 real full scan MVP，最后在 V2-8D 打开
DOP>1 range-bound scan。
