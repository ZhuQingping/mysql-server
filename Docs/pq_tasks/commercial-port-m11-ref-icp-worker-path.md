# M11-F Ref / ICP Worker Path Continuation Taskbook

## 状态

Status: M11-F0/F1a completed；M11-F2 Worker TABLE / Handler / Prebuilt
Ownership Contract design in progress。

M11-E 已收口：ORDER BY source work 停止，真实 ORDER BY 执行链路保持
blocked。M11-F 只处理 ref / ICP worker path，不与 M11-E ORDER BY、
`Exchange_sort`、handler-ref ORDER BY tie-break 或 visible ORDER BY gate 混合。

## 背景

当前分支已经打开一组 leader-local secondary/ref/ICP 子集：

- M9-B3d：strict covering integer secondary forward range；
- M9-C2：constant covering ref；
- M9-D3d：dependent covering ref leader-local gate；
- M9-E1c-2a：leader-local non-covering secondary range ICP + clustered lookup；
- M9-F0-F5：MVI、reverse、partition、secondary MIN、record buffer 的 guard
  与诊断。

这些能力都不是商用实现里的 worker-side ref / ICP path。当前实现仍应认为：

- `PQRefIterator` / `PQblockScanIterator` 的商用 worker/MQ 级 secondary/ref
  row production 未完成；
- worker-side ICP clone / refix / pushdown 未完成；
- `pq_worker_scan_next()` 相关真实 worker pull-row 路径仍不能默认启用；
- native `Record_buffer` / InnoDB prefetch cache 仍不是当前 PQ row sink；
- MVI positive unique filter、reverse positive scan、partition positive path
  仍 blocked。

## 商用参考入口

参考仓库：

`/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore`

关键文件和函数：

- `sql/parallel_query/pq_iterators.cc`
  - `PQblockScanIterator::Init() / Read() / End()`
  - `PQRefIterator::Init() / Read() / End()`
  - MVI unique filter: `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER` /
    `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER`
  - native `Record_buffer`: `set_record_buffer(...)`
- `sql/parallel_query/sql_parallel.cc`
  - `SetupPQTab()`
  - `InitPQTab()`
  - `pq_rewrite_full_access_path_cost()`
  - worker ICP pushdown: `make_cond_for_index()` / `idx_cond_push()` /
    `make_cond_remainder()`
- `sql/parallel_query/pq_clone.cc`
  - `QEP_TAB::pq_copy()` copies `m_reversed_access`
  - `TABLE::pq_copy()` copies `part_info` and deep-clones `pushed_idx_cond`
- `storage/innobase/handler/ha_innodb_pq.cc`
  - `ha_innobase::pq_index_scan_init()`
  - `ha_innobase::pq_range_scan_init()`
  - `ha_innobase::pq_ref_build_ranges()`
  - `ha_innobase::pq_ref_scan_init()`
  - `ha_innobase::pq_leader_scan_init()`
  - `ha_innopart::pq_leader_scan_init()`
  - `ha_innopart::pq_worker_scan_init()`
  - `ha_innobase::pq_worker_scan_next()`
- `storage/innobase/row/row0pread_pq.cc`
  - `PQ_Scan_ctx::partition()`
  - `PQ_Scan_ctx::find_visible_record()`
  - `PQ_Ctx::read_record()`
  - reverse boundary、ICP、clustered lookup、native record buffer、
    `n_fetch_cached` / `fetch_cache_first`

参考测试：

- `pq_multi_value.test`
- `pq_range_scan_reverse.test`
- `pq_reverse_index_scan.test`
- `pq_ref_reverse_scan.test`
- `pq_partition.test`
- `pq_sec_index_min.test`
- `pq_record_buffer.test`
- 现有当前仓 targeted guard: `pq_commercial_ref_icp`, `pq_stats`,
  `pq_not_support`

## M11-F0: Worker Path Continuation Contract

Status: design-only / read-only；Design Review accepted。

目标：

1. 梳理商用 worker-side ref / ICP 执行链路与当前 leader-local gates 的差异；
2. 定义后续 M11-F1+ 的最小安全切入点；
3. 明确哪些商用能力继续 blocked，哪些只允许 probe/guard；
4. 固化 review gate，防止直接复制商用 worker path 导致 handler/InnoDB
   生命周期、ICP Item、MQ row format 或 fallback 语义破坏。

允许修改：

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/commercial-port-m11-main-architecture-restart.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-m9-f-edge-cases.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/**`
- `storage/**`
- `mysql-test/**`
- 构建脚本或测试 result 文件

设计要求：

- 区分当前 leader-local 正例与商用 worker/MQ 正例；
- 列出 worker-side ref/ICP 所需的 TABLE/handler/prebuilt ownership；
- 列出 `pushed_idx_cond` clone / refix / pushdown 的生命周期；
- 列出 `pq_ref_build_ranges()`、per-ref-key dispatch、dependent ref key
  buffer 的所有权和重复 key 处理；
- 列出 worker ERROR / KILL / abort 后是否允许 serial fallback 的规则；
- 明确 MVI、reverse、partition、native `Record_buffer`、secondary MIN 不在
  M11-F0 编码范围。

验收：

- Design Review Agent 返回 `ACCEPT`；
- 文档明确 M11-F1 的第一个可执行任务；
- 文档明确 M11-F1 是 probe/guard-only 还是可编码正例；
- 不改变源码和 MTR。

Review:

- Design / Docs / Source Review Agent returned `ACCEPT`；
- confirmed current M9 capabilities are leader-local with `workers/ranges=0`；
- confirmed commercial worker-side ref/ICP is materially different because it
  depends on `PQRefIterator`, per-ref range building, `ha_pq_next()` /
  `pq_worker_scan_next()`, worker handler/prebuilt state, cloned
  `pushed_idx_cond`, and worker pull-row state；
- recommended next step is M11-F1 as probe/guard-only；
- explicitly rejected copying commercial `PQRefIterator::Read()`, calling real
  `pq_worker_scan_next()`, or introducing worker-side ICP clone/refix in F1。

## 初步任务拆分

### M11-F1: Worker-side Ref/ICP Shape Probe

Status: M11-F1a completed / Code-Docs-Test Review accepted / committed as
`ff201265db9`。

建议性质：probe/guard-only。F1 只能新增诊断、counter 或 fail-closed guard；
必须保持零真实 worker row production。

候选目标：

- 在不打开真实 worker row production 的前提下，观测 worker-side ref/ICP
  所需 shape；
- 对 `PQRefIterator`、secondary range/ref、`pushed_idx_cond`、dependent ref
  key buffer 建立 fail-closed counters；
- 证明当前 unsupported shape 不增长 `Parallel_queries_executed`、
  `Parallel_workers_launched`、`Parallel_ranges_built`、
  `Parallel_ranges_dispatched`、`Parallel_secondary_rows_produced`。

禁止：

- 不调用真实 `pq_worker_scan_next()`；
- 不复制商用 `PQRefIterator::Read()`；
- 不改 handler public virtual API；
- 不接真实 MQ worker-result path；
- 不引入 worker-side `Item` clone/refix；
- 不增长真实执行正例的 worker/MQ row production。

#### M11-F1a: Reverse Ref Reject Probe

目标：

- 给 secondary ref factory 中已有的 reverse / worker-ref 失败关闭分支增加
  直接可观测 counter；
- 证明当前 reverse ref 仍不进入 PQ execution、worker、range dispatch 或
  secondary row production；
- 对 reverse range/index 只保留现有 zero-counter guard，不在 F1a 试图从
  `TryCreatePQSecondaryCoveringRangeIterator()` 观测，因为 reverse
  `INDEX_RANGE_SCAN` / `INDEX_SCAN` 在 `access_path.cc` 会先选择串行
  reverse iterator；
- 为未来 ORDER BY / Gather Merge 打开后仍能识别 reverse rejection 留护栏；
- 不打开 reverse positive path。

允许修改：

- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `storage/innobase/**`
- `sql/handler.h`
- `sql/range_optimizer/**`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/pq_clone.*`
- `sql/parallel_query/pq_optimizer.*`
- `mysql-test/suite/parallel_query/t/pq_stats.test`，除非 status 列表查询
  本身必须更新；
- 任何 positive reverse/ref/ICP row production；
- 任何 `PQRefIterator::Read()`、`PQblockScanIterator::Read()` 或真实
  `pq_worker_scan_next()` 接线。

拟新增状态变量：

- `Parallel_secondary_reverse_reject_probes`
- `Parallel_secondary_reverse_ref_reject_probes`

实现要求：

- 在 `TryCreatePQSecondaryCoveringRefIterator()` 的 `param.reverse` /
  dependent-ref reverse fail-closed 分支记录 ref reverse reject；
- aggregate counter 与 ref 子 counter 同时增长；aggregate 不是
  MTR 派生值；
- counter 只允许在 ref factory 决定返回 `nullptr` 的 reject 分支增长；
- counter 增长不得改变 `pq_set_execution_state()`、iterator selection、
  fallback、handler/InnoDB 调用或 row materialization；
- 新字段必须加入 `PQ_global_stats::reset()`、`mysqld.cc` SHOW STATUS
  注册和 `pq_stats.result` status variable list；
- positive M9-B3/C2/D3d/E1c-2a windows 的 executed / worker / range /
  secondary row 语义必须保持不变。

MTR 验收：

- `pq_commercial_ref_icp` 新增 F1a counter window：
  - reverse ref 或 adjacent ref shape，必须能触达 ref factory reject
    branch；
  - reverse secondary range / reverse index 继续只断言 existing zero
    execution/worker/range/row counters，不要求新增 reverse reject counter；
- 断言：
  - `Parallel_secondary_reverse_reject_probes` delta `> 0`；
  - `Parallel_secondary_reverse_ref_reject_probes` delta `> 0`；
  - `Parallel_queries_executed` delta `= 0`；
  - `Parallel_workers_launched` delta `= 0`；
  - `Parallel_ranges_built` delta `= 0`；
  - `Parallel_ranges_dispatched` delta `= 0`；
  - `Parallel_secondary_rows_produced` delta `= 0`；
- 更新 `pq_stats.result` 的 status variable list；
- targeted record/replay、`pq_stats` record/replay、完整 `parallel_query`
  suite 通过。

Validation commands:

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_stats
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_stats
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Hard stop:

- 如果需要修改 InnoDB、handler API、`Query_result_mq`、worker thread task、
  `pq_clone` 或 `PQRefIterator::Read()`，立即停止并转为新的 design phase；
- 如果 reverse/ref SQL shape 无法稳定触达 factory reject branch，只提交
  docs-only blocked result，不用弱断言冒充 probe 成功。
- 如果要观测 reverse range/index 的 access-path-level rejection，必须另开
  phase 并重新 review 是否允许触碰 `sql/join_optimizer/access_path.cc`。

Taskbook Review:

- First review returned `REVISE` because reverse `INDEX_RANGE_SCAN` /
  `INDEX_SCAN` is handled in `access_path.cc` before current PQ secondary
  factories are reached；
- F1a was narrowed to reverse ref reject only；
- re-review returned `ACCEPT` and required:
  - counters only in `TryCreatePQSecondaryCoveringRefIterator()` reverse
    reject paths；
  - no counters for reverse range/index；
  - root reverse ref counted before ordered-query guard hides `param.reverse`；
  - fields added to `PQ_global_stats`, `reset()`, `mysqld.cc` SHOW STATUS,
    and `pq_stats.result`；
  - MTR proves reverse ref counter deltas `> 0` while execution/workers/ranges/
    secondary rows stay `0`。

Completion Report - M11-F1a Coding:

Changed files:

- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/t/pq_stats.test`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- Added `Parallel_secondary_reverse_reject_probes` and
  `Parallel_secondary_reverse_ref_reject_probes`；
- counters are registered in SHOW STATUS and reset with other
  `PQ_global_stats`；
- added `pq_secondary_reverse_ref_reject_probe()` and call it only from
  `TryCreatePQSecondaryCoveringRefIterator()` when `param.reverse` causes a
  fail-closed return；
- moved root reverse-ref counting before the simple-query / ordered-query guard；
- did not modify InnoDB, handler API, MQ, clone, optimizer, range optimizer,
  `PQRefIterator::Read()`, `PQblockScanIterator::Read()`, or
  `pq_worker_scan_next()`；
- `pq_stats.test` now masks EXPLAIN `rows` estimate with
  `--replace_column 10 ROWS` because the estimate varied between record/replay
  while the test only depends on PQ status and Extra diagnostics.

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_stats
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_stats
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- `mysqld` build passed；
- `pq_commercial_ref_icp` record/replay passed；
- F1a counter window produced
  `f2_reverse_executed_delta=0`、`f2_reverse_workers_delta=0`、
  `f2_reverse_ranges_built_delta=0`、
  `f2_reverse_ranges_dispatched_delta=0`、
  `f2_reverse_secondary_rows_delta=0`、
  `f1a_reverse_reject_probe_seen=1`、
  `f1a_reverse_ref_reject_probe_seen=1`；
- `pq_stats` record/replay passed；
- full `parallel_query` suite passed: 89/89。

Residual risks:

- reverse range/index still has no new counter because those paths are selected
  in `access_path.cc` before current PQ secondary factories；
- access-path-level reverse diagnostics require a separate reviewed phase；
- no worker-side ref/ICP row production is enabled by F1a。

### M11-F2: Worker TABLE / Handler / Prebuilt Ownership Contract

Status: Design Review accepted；next task is M11-F2a Worker Ownership Contract
Hardening。

建议性质：design-first / docs-only。F2 只固化 worker TABLE、handler、
`row_prebuilt_t`、read view、per-probe key 和 abort cleanup 的契约，不打开真实
worker ref/ICP row production。

背景差异：

- 当前分支已有 `PQ_Worker_open_context`、`pq_open_worker_table()`、
  `pq_close_worker_table()` 和 worker THD/TABLE smoke；该 helper 通过 SQL
  open path 打开独立 worker `TABLE`，并拒绝共享 leader `TABLE`、
  `handler`、`record[0]`。
- 当前 InnoDB `pq_worker_scan_init()` 只建立可观测 worker context 和 range
  dispatch；`pq_worker_scan_next()` 仍返回 unsupported，不产生真实行。
- 商用实现的 ref path 是 `PQRefIterator::Read()` 构造 per-probe key，
  调用 `pq_ref_build_ranges()`，再由 `ha_pq_next()` /
  `pq_worker_scan_next()` 拉取 worker 行；这依赖 worker handler/prebuilt、
  `PQ_ref_info`、`PQ_ref_key`、`pq_ref_depend` 和 worker-local context queue。
- 因此 F2 不能把商用 `PQRefIterator` 或 `ha_innobase::pq_ref_build_ranges()`
  整段平移；必须先约束当前 8.0.46 基座中的 ownership 和错误语义。
- 两个只读 Explorer 均确认：如果目标是“直接编码 worker ref/ICP”，应
  REVISE；如果目标改为“docs-only contract + 后续 debug-only smoke”，方向
  ACCEPT。

需要回答：

- worker TABLE 是否必须独立打开；
- worker handler/prebuilt 与 leader handler/prebuilt 是否完全隔离；
- read view / trx / mtr 生命周期如何复用 M11-D/M5/M6 已有 contract；
- dependent ref 的 per-probe key buffer 如何在 worker dispatch 期间保持有效；
- worker abort 后如何清理未消费 row 和 InnoDB cursor state。

#### M11-F2 Contract Requirements

1. Worker TABLE：
   - 必须通过 worker THD 独立打开；
   - 必须验证 `worker_table != leader_table`、
     `worker_table->file != leader_table->file`、
     `worker_table->record[0] != leader_table->record[0]`；
   - worker `read_set` / `write_set` 只能从 leader 当前需要列复制，不能反向修改
     leader bitmaps；
   - worker TABLE close 必须覆盖 success、ERROR、KILL、abort-after-start 和
     empty-range EOF。

2. Worker handler / prebuilt：
   - worker handler 与 leader handler 必须完全隔离；
   - worker handler 必须独占自己的 `ha_innobase::m_prebuilt`；不得保存、
     复用或间接引用 leader handler 的 `m_prebuilt` 指针；
   - 后续编码必须显式验证 worker `m_prebuilt->m_mysql_table`、handler 归属
     和 worker THD/TABLE 的关系；否则继续 fail-closed；
   - worker `row_prebuilt_t`、`pcur`、`clust_pcur`、`blob_heap`、
     `mysql_template`、`idx_cond`、`m_end_range`、`search_tuple`、
     `m_stop_tuple` 均不得共享 leader mutable state；
   - leader prebuilt 只允许用于 leader scan/range planning 或当前已有
     leader-local gated producer；worker row production 必须使用 worker-owned
     handler/prebuilt；
   - `pq_worker_scan_next()` 在上述隔离可验证前继续 unsupported。

3. Read view / trx / mtr：
   - 继续复用 M5/M6/M11-D 已确认的 leader-pinned read-view contract；
   - worker 不得创建独立、不一致的 statement read view；
   - worker handler/prebuilt 的 trx/read view 绑定方式必须在编码前明确：
     要么 clone/attach leader snapshot 并证明生命周期，要么继续 fail-closed；
   - worker-local `mtr_t` 只能在线程内创建、消费和提交/释放，不跨 worker、
     不跨 leader 线程传递。

4. Dependent ref per-probe key：
   - 每次 outer-row probe 的 key 必须在 dispatch 前 deep-copy 到 PQ-owned
     buffer；禁止 worker range/slice 保存指向 `QEP_TAB::ref().key_buff`、
     `Index_lookup` 临时 buffer 或 leader `TABLE::record[0]` 的裸指针；
   - repeated outer key 可以复用 range metadata，但不能跳过 probe 的输出
     语义；重复 key 不等价于“已输出过”；
   - empty probe 必须可观测，并且不得误增长 rows produced；
   - per-probe range build 失败若发生在 worker-start 前可 fail-closed；worker
     已启动后必须走 worker ERROR/abort，不允许 silent serial fallback。

5. Abort / KILL / ERROR：
   - worker-start 后出现 OOM、unsupported InnoDB state、ICP clone failure、
     clustered lookup failure、MQ detach 或 KILL，必须停止 worker、关闭 worker
     TABLE/handler/prebuilt、丢弃未消费 MQ row，并把错误传播给 leader；
   - 不允许在已经有 worker row production 或 worker range dispatch 后静默回退
     串行；
   - leader early EOF/abort 必须 drain 或 detach worker MQ，避免 worker 挂在
     send path。
   - cleanup 顺序必须固定为：stop workers/MQ -> worker scan end ->
     close worker TABLE/handler -> leader scan end -> read view/thread budget
     release。

允许修改：

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/**`
- `storage/**`
- `mysql-test/**`
- 构建脚本、result 文件或 patch 文件

明确继续 blocked：

- worker-side ICP clone / refix；
- `PQRefIterator::Read()` positive worker path；
- 真实 `pq_worker_scan_next()` row production；
- partition positive path；
- reverse positive ref/range/index；
- MVI positive unique filter；
- native `Record_buffer` / prefetch positive path；
- ORDER BY / `Exchange_sort` visible execution。

M11-F2 验收：

- 当前分支 Explorer 和商业实现 Explorer 均确认 F2 contract 覆盖关键 ownership
  风险；
- Review Agent 返回 `ACCEPT`；
- README 当前状态更新到 F2；
- 不产生源码 diff；
- `git diff --check` 通过。

#### Explorer Review Summary

Current-branch Explorer:

- 结论为 `REVISE`，原因是 F2 草案尚未明确后续 F2a 可编码 hardening 的
  文件范围；
- 确认当前已有 `PQ_Worker_open_context`、`pq_open_worker_table()`、
  `pq_close_worker_table()`、worker THD/TABLE smoke 和 InnoDB
  `pq_worker_scan_init()` context/range dispatch；
- 指出仍缺少 worker handler 的 `m_prebuilt` 归属强校验，且
  `pq_worker_scan_next()` 仍 disabled；
- 建议最小安全子任务为 M11-F2a worker ownership contract hardening：
  增加 worker TABLE/handler/prebuilt/record buffer 不共享的断言或 debug-only
  smoke，不启用新 row production。

Commercial Diff Explorer:

- 结论为：不建议直接编码 worker ref/ICP；
- 商用实现可参考 worker-owned TABLE/handler/prebuilt、per-ref-key dispatch、
  dependent ref deep-copy key、worker ctx ref-key rebinding 和 post-start
  no-serial-fallback 原则；
- 不可直接平移商用 `PQRefIterator::Read()`、`ha_pq_next()` pull-row 路径、
  `row_prebuilt_t` 内嵌 PQ mutable state、partition、reverse、MVI、
  native `Record_buffer` 或 worker-side ICP clone/refix；
- 建议下一步为 docs-only contract + debug-only smoke。

Design Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- non-blocking risks:
  - F2a 允许触碰 `sql/handler.h`，实现时必须保持 public virtual API 行为不变；
  - F2a 验收中 unsupported ref/ICP shape 的 counter 断言必须与现有 worker
    ownership smoke 的 range-dispatch 诊断区分开；
  - 工作区存在大量既有 untracked 文件，提交时只允许包含 F2 文档文件；
- recommended next task: M11-F2a Worker Ownership Contract Hardening。

#### Proposed M11-F2a: Worker Ownership Contract Hardening

Status: proposed next coding task after F2 Review ACCEPT。

目标：

- 在不打开 ref/ICP worker row production 的前提下，强化 worker TABLE、
  handler、`m_prebuilt`、record buffer 和 worker THD 的 ownership 断言；
- 增加 debug-only smoke 或状态诊断，证明 worker TABLE/handler/prebuilt 与
  leader 不共享；
- 修正文档/注释中“worker init disabled”与当前 `pq_worker_scan_init()` 已可
  建 context 的不一致；
- 保持 `pq_worker_scan_next()` unsupported。

F2a 允许修改：

- `sql/parallel_query/pq_handler.h`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/handler.h`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- 如确需 smoke 入口，可小范围修改 `sql/parallel_query/pq_iterator.cc`
- targeted MTR 仅限已有 worker ownership/smoke 测试窗口

F2a 禁止修改：

- `PQRefIterator::Read()` positive path；
- `PQblockScanIterator::Read()` positive ref/ICP path；
- `row0pread_pq.cc` 二级/ref/ICP worker row production；
- worker-side ICP clone / refix；
- optimizer eligibility 扩大；
- partition、reverse、MVI、native `Record_buffer` positive path；
- ORDER BY / `Exchange_sort` visible execution。

F2a 验收：

- `pq_worker_scan_next()` 仍返回 unsupported；
- unsupported worker ref/ICP shape 不增长
  `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced`；
- `mysqld` build 通过；
- targeted MTR 和完整 `parallel_query` suite 通过；
- Code/Task Review Agent 返回 `ACCEPT`。

#### M11-F2 Agent Task Prompt

请作为 M11-F2 Design / Source Review Agent，只读审查本任务书和相关源码：

1. F2 是否准确区分当前 `pq_open_worker_table()` scaffold 与商用 worker ref
   path；
2. worker TABLE/handler/prebuilt/read-view/per-probe-key/abort cleanup contract
   是否足够支撑后续编码；
3. 是否遗漏了 `row_prebuilt_t` mutable state、ICP state、record buffer 或
   MQ detach 风险；
4. F2 是否正确禁止直接打开 `PQRefIterator::Read()`、`pq_worker_scan_next()`
   和 worker-side ICP clone/refix；
5. 推荐下一个最小编码任务是什么。

输出：

- Verdict: ACCEPT 或 REVISE
- Blocking findings
- Non-blocking risks
- Recommended next task

### M11-F3: Worker-side ICP Clone / Refix Contract

建议性质：design-first。

需要回答：

- `TABLE::pq_copy()` deep-clone `pushed_idx_cond` 在当前 8.0 分支是否可行；
- `make_cond_for_index()` / `idx_cond_push()` / `make_cond_remainder()` 在
  worker THD/TABLE 中的所有权；
- `ICP_OUT_OF_RANGE`、filtered row、clustered lookup continuation 与
  PQ counters 的关系；
- ICP failure after partial row production 是否允许 fallback。

### M11-F4: Native Record_buffer / Prefetch Worker-owned Adapter

建议性质：design-first，性能优化，不是 correctness 前置。

首批仅允许讨论：

- non-partitioned；
- forward；
- covering secondary range；
- no ICP；
- no BLOB/fixed-point/read-set-hostile shape；
- 每个 worker 独占 `Record_buffer` / prebuilt fetch cache。

### M11-F5: Edge Positive Paths Backlog

继续 blocked，后续单独阶段：

- MVI positive unique filter；
- reverse positive range/ref/index scan；
- partition positive full/range/ref/dependent-ref；
- secondary index MIN / optimizer shortcut；
- ICP + native `Record_buffer` 组合。

## 必须保留的 Guard

- visible ORDER BY 继续由 `HAS_ORDER_BY` 串行拒绝；
- M9-B3/C2/D3d/E1c-2a 正例 counter window 不能回退；
- M9-F MVI/reverse/partition/secondary MIN/BLOB hostile shapes 继续不增长
  PQ execution / worker / range / secondary row counters；
- unsupported ICP/ref/dependent-ref shape 继续不增长
  `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced`；
- worker-start 后失败不得 silent serial fallback；
- no mixed commit：M11-F 不与 ORDER BY、partition positive、native
  record buffer positive 或 MVI positive 混合。

## M11-F0 Agent Review Prompt

请作为 M11-F0 Design / Source Review Agent，只读审查本任务书和相关资料：

1. M11-F0 是否正确区分当前 leader-local M9 能力与商用 worker-side ref/ICP；
2. 初步拆分 F1-F5 是否覆盖商用迁移所需关键链路；
3. F1 是否应先做 probe/guard-only，还是可以直接编码正例；
4. 是否存在比 F1 更小、更安全的首个任务；
5. Allowed/Forbidden files 和 guard 是否足够防止误开 ORDER BY、partition、
   MVI、reverse、native record buffer。

输出：

- Verdict: ACCEPT 或 REVISE
- Blocking findings
- Non-blocking risks
- 建议下一步
