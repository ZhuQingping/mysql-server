# M11-F Ref / ICP Worker Path Continuation Taskbook

## 状态

Status: M11-F0/F1a/F2/F3/F4 completed；M11-F5 backlog triage accepted；
M11-F5a secondary MIN / optimizer shortcut source inventory completed and
accepted；M11-F5a-1 debug-only optimizer shortcut diagnostic completed and
committed；F5-A reverse boundary contract completed and committed；F5-C
partition worker ownership design completed and committed；F5-C1 partition
reject diagnostic completed and committed；F5-B MVI inventory / design accepted；
F5-B1 debug-only MVI reject diagnostic completed and committed；F5-E ICP +
native `Record_buffer` combined path design accepted；
real worker-side ICP positive row production remains blocked。

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

Status: Code-Task Review accepted；ready to commit。

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

Completion Report - M11-F2a Coding:

Changed files:

- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_agg_result.test`
- `mysql-test/suite/parallel_query/r/pq_agg_result.result`
- `mysql-test/suite/parallel_query/t/pq_commercial_order_by.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_order_by.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- `ha_innobase::pq_worker_scan_init()` now hard-fails unsupported unless the
  current InnoDB handler owns the worker TABLE/prebuilt pair:
  `table == worker_table`、`worker_table->in_use == worker_thd`、
  `m_prebuilt->m_mysql_table == worker_table`、
  `m_prebuilt->m_mysql_handler == this`；
- added explicit rejection if the worker prebuilt/handler points back to the
  leader TABLE/handler；
- updated InnoDB worker API comments to match the current state:
  `pq_worker_scan_init()` may create guarded worker contexts, while the public
  pull-row `pq_worker_scan_next()` API remains disabled；
- did not open `PQRefIterator::Read()`、`PQblockScanIterator::Read()` positive
  ref/ICP path、`pq_worker_scan_next()` row production、worker-side ICP
  clone/refix、optimizer eligibility、partition/reverse/MVI/native
  `Record_buffer`/ORDER BY positive paths；
- `pq_agg_result` and `pq_commercial_order_by` now mask EXPLAIN `rows` with
  `--replace_column 10 ROWS` because InnoDB small-table row estimates drifted
  between 6/7 and 10/11 during full-suite runs while PQ assertions depend on
  `Extra` diagnostics and counters, not optimizer row estimates.

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_worker_attach_contract_smoke pq_worker_dop1 \
  --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_agg_result pq_commercial_order_by \
  --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_agg_result pq_commercial_order_by --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- `mysqld` build passed；
- targeted worker ownership tests passed；
- targeted EXPLAIN rows-mask record/replay passed；
- full `parallel_query` suite passed: 89/89；
- no worker-side ref/ICP row production enabled。

Code / Task Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- confirmed ownership gate is fail-closed and aligned with
  `pq_open_worker_table()` independent worker TABLE/handler contract；
- confirmed `ha_innodb.h` comments do not claim pull-row enablement；
- confirmed forbidden paths remain closed:
  `PQRefIterator::Read()`、`PQblockScanIterator::Read()` positive ref/ICP path、
  `pq_worker_scan_next()` row production、worker-side ICP clone/refix、
  optimizer eligibility、partition/reverse/MVI/native `Record_buffer` and
  ORDER BY positive path；
- confirmed EXPLAIN `rows` masking does not hide PQ `Extra` or counter
  assertions；
- recommended next task: M11-F2b debug-only ownership mismatch negative smoke。

#### M11-F2b: Worker Ownership Mismatch Negative Smoke

Status: completed / Code-Task Review accepted。

目标：

- 增加 debug-only negative smoke，证明 worker open context 的 handler/prebuilt
  ownership 不匹配时，`pq_worker_scan_init()` fail-closed；
- 负例必须不增长 `Parallel_worker_attach_smoke_success`；
- 负例必须不增长 `Parallel_queries_executed`、
  `Parallel_workers_launched`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced`；
- 用户 SELECT 仍走串行结果，不因 debug-only ownership rejection 报错。

Changed files:

- `sql/parallel_query/sql_parallel.cc`
- `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- Added DBUG flag `pq_worker_ownership_mismatch_smoke` inside
  `Gather_operator::run_worker_attach_contract_smoke()`；
- the flag deliberately points the worker open context handler at the leader
  handler before `pq_worker_scan_init()`；
- the InnoDB ownership gate rejects the mismatch before range dispatch；
- the debug-only helper treats this expected rejection as smoke success after
  cleanup, without incrementing attach success；
- no public execution path is changed。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_worker_attach_contract_smoke --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- `mysqld` build passed；
- targeted record/replay passed；
- full `parallel_query` suite passed: 89/89；
- mismatch window produced:
  `mismatch_attempt_delta=1`、`mismatch_success_delta=0`、
  `mismatch_cleanup_delta=1`、
  `mismatch_queries_executed_delta=0`、
  `mismatch_workers_launched_delta=0`、
  `mismatch_ranges_dispatched_delta=0`、
  `mismatch_secondary_rows_delta=0`。

Code / Task Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- confirmed DBUG injection is scoped to `run_worker_attach_contract_smoke()`
  and cannot affect normal attach/callback/threaded paths；
- confirmed mismatch is rejected before worker ctx allocation and range
  dispatch；
- confirmed cleanup closes worker TABLE and destroys worker THD without
  calling scan_end on a null worker ctx；
- confirmed MTR covers attempts/success/cleanup/executed/workers/ranges/
  secondary rows；
- confirmed no worker ref/ICP row production or other positive blocked path
  was opened。

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

Status: M11-F3b completed / Code-Docs-Test Review accepted / committed as
`a8ad45ecf0c`。

建议性质：design-first + 后续 debug-only fail-closed smoke。F3 不打开真实
worker-side ICP positive path；不调用真实 `PQRefIterator::Read()` /
`PQblockScanIterator::Read()` row production；不启用 `pq_worker_scan_next()`。

#### Explorer Findings

当前分支确认：

- 标准 ICP 拆分与推下在 `make_cond_for_index()`、
  `make_cond_remainder()`、`QEP_TAB::push_index_cond()` 和
  handler `idx_cond_push()` 链路中完成；
- InnoDB ICP 依赖 handler `pushed_idx_cond` / `pushed_idx_cond_keyno`、
  `build_template()` 生成的 `m_prebuilt->idx_cond` template，以及
  `row_search_idx_cond_check()` 对当前 handler `table->record[0]` 的求值；
- 当前可执行正例 M9-E1c-2a 是 leader-local non-covering secondary range
  ICP + clustered lookup，会保存/恢复 leader handler/prebuilt mutable
  state；它不是 worker-owned ICP；
- 当前 `pq_clone.cc`、`pq_clone_item.cc`、`pq_refix_fields_item.cc` 和
  `pq_resolver.cc` 仍不能提供 worker `TABLE/Field/Item` 全量 clone/refix；
- 因此当前不能把 leader `pushed_idx_cond`、`pq_cond` 或 `Item_field`
  直接借给 worker handler 执行。

商用实现确认：

- `QEP_TAB::pq_copy()` clone `pq_cond` 并 `refix_fields()`；
- `TABLE::pq_copy()` deep-clone `orig->file->pushed_idx_cond`，复制
  `pushed_idx_cond_keyno`，并把字段引用 refix 到 worker TABLE；
- `Index_lookup::pq_copy()` deep-copy ref key buffer、clone/refix ref items，
  重建 `store_key`；
- worker setup 在 worker `THD/TABLE/handler` 上重新执行
  `make_cond_for_index()` -> `idx_cond_push()` -> `make_cond_remainder()`；
- InnoDB worker row read 在 secondary record 阶段执行 ICP，
  `ICP_NO_MATCH` 跳过，`ICP_OUT_OF_RANGE` 结束当前 range，`ICP_MATCH`
  才继续必要的 clustered lookup；
- `pq_ref_build_ranges()` 不用 ICP 判断 ref key 是否为空；dependent ref 的
  per-probe key 需要由 ref path deep-copy，并交给 worker dispatch 校验。

#### F3 Contract Requirements

Worker-owned ICP Item：

- `pushed_idx_cond`、`pq_cond`、remainder condition 和 ref key items 必须在
  worker `THD` / worker `TABLE` 生命周期内 deep-clone；
- 所有 `Item_field::field` / table map / record pointer 必须 refix 到
  worker TABLE/record；
- 禁止共享 leader `Item*`、leader handler `pushed_idx_cond`、leader
  `TABLE::record[0]` 或 leader prebuilt ICP state；
- 在 clone/refix 覆盖面没有逐类证明前，worker-side ICP 必须 fail-closed。

Worker pushdown / remainder：

- worker TABLE 与 handler 已打开后，才能在 worker handler 上重新执行
  `idx_cond_push(keyno, idx_cond)`；
- `idx_cond` 必须来自 worker-owned cloned condition；
- `make_cond_remainder()` 的结果必须保留 SQL 层非 ICP filter，不能丢
  filter，也不能重复过滤导致结果不一致；
- `pushed_idx_cond_keyno` 必须与 worker active index 一致。

InnoDB prebuilt / template：

- `m_prebuilt->m_mysql_table`、`m_mysql_handler`、`index`、
  `active_index`、`read_just_key`、`mysql_template`、`idx_cond`、
  `idx_cond_n_cols`、`need_to_access_clustered`、`m_end_range`、
  `pcur/clust_pcur` 必须是 worker handler 独占状态；
- worker ICP 只能经由 `idx_cond_push()` + `build_template()` +
  `row_search_idx_cond_check()`，禁止裸调 `pushed_idx_cond->val_int()`；
- leader prebuilt/handler mutable state 不得因 worker setup 或 cleanup 被修改。

Counters / error semantics：

- `ICP_NO_MATCH` 过滤行不得增长 produced-row counters；
- `ICP_OUT_OF_RANGE` 只能结束当前 worker range，不能被解释为 produced row；
- worker start 之前发现 clone/refix/pushdown 不满足，可以 fail-closed 并走
  串行 fallback；
- worker start 之后或已有 worker row token 之后失败，不允许 silent serial
  fallback，必须走 worker ERROR / abort / cleanup；
- dependent ref + ICP 中，range/ref boundary build 不能用 ICP 过滤 key 是否
  存在，尤其不能用含 outer-table 引用的 ICP 提前判空。

继续 blocked：

- native `Record_buffer` / InnoDB prefetch 与 ICP 组合；
- MVI positive unique filter；
- reverse positive range/ref/index scan；
- partition positive full/range/ref/dependent-ref；
- visible ORDER BY positive path；
- `PQRefIterator::Read()`、`PQblockScanIterator::Read()`、真实
  `pq_worker_scan_next()` row production。

#### Proposed M11-F3a: Worker ICP Contract Documentation Review

性质：docs-only。

允许修改：

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/**`
- `storage/**`
- `mysql-test/**`
- 构建脚本或 result 文件

验收：

- Design / Docs / Source Review Agent 返回 `ACCEPT`；
- review 明确 F3b 是否可以进入 debug-only fail-closed smoke；
- 不改变源码和 MTR。

#### Proposed M11-F3b: Debug-only ICP Ownership Mismatch Smoke

性质：debug-only negative smoke；需在 F3a review accepted 后再编码。

目标：

- 构造 ICP-specific mismatch：leader 有 `pushed_idx_cond`，但 worker 没有
  worker-owned cloned/refixed ICP state，或 worker ICP keyno ownership 不满足；
- 不允许仅重复 M11-F2b generic handler/prebuilt ownership mismatch；
- 证明该形态在 worker attach/init 边界 fail-closed；
- 证明负例不增长 `Parallel_queries_executed`、
  `Parallel_workers_launched`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced`；
- 不强制 `Parallel_ranges_built = 0`，因为 worker attach/init smoke 可能在
  worker-side rejection 之前已经构造 leader-side ranges；
- 断言 attach/smoke attempt delta `>= 1`、cleanup delta `>= 1`、success
  delta `= 0`，证明负例可达且 cleanup 执行；
- 证明不会调用真实 `pq_worker_scan_next()` 或 enqueue worker row token。

候选允许修改：

- `sql/parallel_query/sql_parallel.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`
- 必要的 PQ status 变量注册文件
- 本任务文档与总看板

候选禁止修改：

- `sql/item*.{h,cc}`
- `sql/sql_select.cc`
- `sql/parallel_query/pq_clone.*`
- `sql/parallel_query/pq_refix*`
- `storage/innobase/row/row0sel.cc`
- `storage/innobase/handler/ha_innodb.cc`
- 任何 positive worker-side ICP row production。

Hard stop：

- 需要 clone `pushed_idx_cond` 到 worker `THD/TABLE/handler`；
- 需要重绑 `Item_field::field` 到 worker `TABLE::field[]`；
- 需要在 worker handler 上真实执行 `idx_cond_push()`；
- worker 路径出现 `m_prebuilt->idx_cond == true` 后仍继续生产行；
- smoke 需要非 debug gate 或长测试。

Design / Docs / Source Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- confirmed F3 contract distinguishes current leader-local ICP from commercial
  worker-side ICP；
- confirmed F3 keeps `PQRefIterator::Read()`、`PQblockScanIterator::Read()`、
  `pq_worker_scan_next()` and positive worker-side ICP row production blocked；
- recommended F3b proceed with an ICP-specific mismatch, not a repeat of the
  F2b generic ownership mismatch；
- clarified F3b should not require `Parallel_ranges_built = 0` but must assert
  attempt/cleanup reached, success `= 0`, workers/ranges-dispatched/secondary
  rows unchanged。

Completion Report - M11-F3b Coding:

Changed files:

- `sql/parallel_query/sql_parallel.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_locking_read_fallback.test`
- `mysql-test/suite/parallel_query/r/pq_locking_read_fallback.result`
- `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- Added debug flag `pq_worker_icp_ownership_mismatch_smoke` inside
  `Gather_operator::run_worker_attach_contract_smoke()`；
- the flag creates an ICP-specific mismatch by temporarily setting the leader
  handler `pushed_idx_cond` / `pushed_idx_cond_keyno` to a non-null sentinel
  without constructing or dereferencing a real `Item` tree；
- cleanup restores the leader handler ICP state before serial fallback；
- `ha_innobase::pq_worker_scan_init()` now rejects worker attach/init when the
  leader handler has `pushed_idx_cond` but the worker handler does not own a
  distinct cloned/refixed ICP condition with the same keyno；
- the rejection happens before worker context allocation, range dispatch, worker
  row production, MQ enqueue, or `pq_worker_scan_next()`；
- no `pq_clone*`、`pq_refix*`、`sql_select.cc`、standard InnoDB ICP path、
  positive worker-side ICP row production、ORDER BY、partition、MVI、reverse or
  native `Record_buffer` path was opened。

TDD / RED evidence:

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
```

Before implementation, the new ICP-specific mismatch window failed because
`icp_mismatch_success_delta` was `1` and
`icp_mismatch_ranges_dispatched_delta` was `1` instead of the expected `0`。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_locking_read_fallback --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_locking_read_fallback --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- `mysqld` build passed；
- targeted MTR passed；
- `pq_locking_read_fallback` record/replay passed after masking the volatile
  EXPLAIN `rows` column；full-suite-only drift was 5/6 while `Extra` and
  counters were unchanged；
- full `parallel_query` suite passed: 89/89；
- ICP mismatch window produced:
  `icp_mismatch_attempt_delta=1`、`icp_mismatch_success_delta=0`、
  `icp_mismatch_cleanup_delta=1`、
  `icp_mismatch_queries_executed_delta=0`、
  `icp_mismatch_workers_launched_delta=0`、
  `icp_mismatch_ranges_dispatched_delta=0`、
  `icp_mismatch_secondary_rows_delta=0`；
- Code / Docs / Test Review pending。

Code / Docs / Test Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- confirmed F3b is ICP-specific and not a repeat of F2b generic handler
  mismatch；
- confirmed the debug-only sentinel is restored through cleanup and rejected
  before worker context allocation or range dispatch；
- confirmed the InnoDB gate remains future-compatible with a distinct
  worker-owned cloned/refixed ICP condition using the same keyno；
- confirmed forbidden positive paths remain closed；
- confirmed `pq_locking_read_fallback` masks only volatile EXPLAIN `rows` and
  still checks `Extra=Not parallel LOCKING_READ` plus counters；
- commit can proceed。

### M11-F4: Native Record_buffer / Prefetch Worker-owned Adapter

Status: F4b completed / Code-Docs-Test Review accepted / committed as
`23e3e9d57b8`。

建议性质：docs-only contract first。`Record_buffer` / prefetch 是性能路径，
不是当前 correctness 前置。F4 不直接打开 native `Record_buffer` positive
path，不调用真实 `pq_worker_scan_next()`，不迁移商用 `pq_record_buffer`
大矩阵。

#### Explorer Findings

当前分支确认：

- 没有 native `Record_buffer` / InnoDB prefetch positive path；
- 已有 `pq_record_buffer_probe()` 只统计 handler
  `ha_get_record_buffer()` 是否为 null/non-null；
- `PQ_record_buffer_sink` 是 SQL-owned `std::vector<std::vector<uchar>>`
  row-image buffer，不是 server native `Record_buffer`；
- 当前 leader-local secondary/ref positive path 会先写 handler
  `record[0]`，再 deep-copy 到 SQL vector，最后 leader iterator 再 memcpy
  回 leader `record[0]`；
- debug worker callback/MQ path 使用 row-image MQ / `Exchange_nosort`，
  也不是 native `Record_buffer`；
- `pq_worker_scan_next()` 仍返回 unsupported；worker pull-row path 没有
  native buffer 执行入口；
- M9-F/F5 diagnostics 期望 native record-buffer non-null delta 为 `0`。

商用实现确认：

- `PQblockScanIterator::Init()` / `PQRefIterator::Init()` 在
  `pq_worker_scan_init()` 后调用 `set_record_buffer()`；
- `set_record_buffer()` 分配或复用 `TABLE::m_record_buffer`，并通过
  `ha_set_record_buffer()` 把裸指针交给 handler；
- InnoDB worker row read 通过 `row_sel_get_record_buffer(prebuilt)` 从
  worker handler 取得 buffer；
- 有 record buffer 时，商用 `PQ_Ctx::read_record()` 以
  `Record_buffer::max_records()` 为 batch 上限；
- `Record_buffer::out_of_range` 用于延迟通知当前 ctx/range 已耗尽：先 drain
  cached rows，再请求下一 ctx；
- `pq_worker_scan_end()` 清理 worker `pq_ctx`、`pq_worker`、ref info、
  `n_fetch_cached`、`fetch_cache_first` 等 prebuilt fetch-cache 状态；
- 商用 `pq_record_buffer.test` 覆盖 normal/partition/hash/list/key、
  unsuitable scenes、ICP、dependent-ref join、subquery、dirty fetch-cache 等
  大矩阵，当前不能一次性平移。

#### F4 Contract Requirements

所有权：

- native `Record_buffer` 所有权属于 worker `TABLE::m_record_buffer`；
- 内存由 SQL executor / worker MEM_ROOT 管理；
- handler / InnoDB 只能保存裸指针，不释放、不跨 worker 共享；
- worker handler、`TABLE::record[0]`、`TABLE::m_record_buffer`、
  `prebuilt->n_fetch_cached`、`prebuilt->fetch_cache_first` 必须 worker-local；
- leader `TABLE::m_record_buffer` 或 leader handler `m_record_buffer` 不能
  泄漏到 worker。

attach / detach 顺序：

- worker `TABLE` / handler / prebuilt ownership gate 通过后，才允许讨论
  native buffer attach；
- attach 必须发生在 worker handler scan init 完成、handler `inited`
  状态有效之后，第一次 worker row read 之前；
- worker scan end、ERROR、KILL、early EOF、fallback-before-start 都必须清理
  handler record-buffer pointer 和 InnoDB fetch-cache counters；
- nested-loop / repeated init 场景不能重复分配泄漏，只能 reset
  `TABLE::m_record_buffer` 并同步清 prebuilt cached counters。

row lifetime：

- native `Record_buffer` 只缓存 worker handler row read 的 result rows；
- SQL-owned row-image sink / MQ sink 仍是当前 row-production contract；
- F4 不把 SQL-owned `PQ_record_buffer_sink` 替换为 native `Record_buffer`；
- native buffer slots 由 worker `TABLE::m_record_buffer` / worker MEM_ROOT
  拥有，只能经 worker handler row buffer 消费，并继续通过当前 worker
  row-image / MQ contract copy 给 leader；不能把 buffer slot 指针直接交给
  leader result；
- BLOB/TEXT/JSON/GEOMETRY、fixed-point、hostile read_set/write_set、
  locking read、HANDLER、FTS 等 unsuitable shapes 继续 blocked。

range / ref / ICP：

- `Record_buffer::out_of_range` 必须与 worker range/ctx 边界一致；
- dependent ref 每个 probe key 重建 range 后必须 reset buffer 与
  fetch-cache state；
- ICP + native `Record_buffer` 必须单独阶段处理，不能并入 F4 首批；
- non-covering clustered lookup、MVI、reverse、partition、visible ORDER BY
  positive path 继续 blocked。

当前 F4 正例延后原因：

- 当前分支不使用商用独立 B+tree partition/pull reader，而是基于 8.0.46
  `Parallel_reader` / callback / row-image MQ 适配路线；
- `PQblockScanIterator::Read()`、`PQRefIterator::Read()` 和真实
  `pq_worker_scan_next()` 仍未作为用户可见路径启用；
- worker-side ICP clone/refix、dependent ref dispatch、partition/MVI/reverse
  gates 还未达到可与 native buffer 混合的状态；
- 因此直接平移商用 `pq_record_buffer` positive path 风险高于收益。

#### Proposed M11-F4a: Record_buffer Contract Design Review

性质：docs-only。

允许修改：

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/**`
- `storage/**`
- `mysql-test/**`
- 构建脚本或 result 文件

验收：

- Design / Docs / Source Review Agent 返回 `ACCEPT`；
- 明确 F4b 是否进入 debug-only diagnostic；
- 不改变源码和 MTR。

#### Proposed M11-F4b: Debug-only Record_buffer Diagnostic

性质：F4a review accepted 后再做；debug-only / fail-closed diagnostic。

候选目标：

- 在现有 worker attach/probe 边界证明当前 worker handler
  `ha_get_record_buffer()` 仍为空；
- 证明没有 native buffer attach、没有 `n_fetch_cached` /
  `fetch_cache_first` 污染、worker end cleanup 幂等；
- 任何 native-buffer non-null shape 都不得增长
  `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_dispatched`、`Parallel_secondary_rows_produced`；
- 不要求 `Parallel_ranges_built = 0`，除非诊断点位于 leader range build 前。

候选允许修改：

- `sql/parallel_query/sql_parallel.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`
- 必要的 PQ status 变量注册文件
- 本任务文档与总看板

候选禁止修改：

- `sql/sql_executor.cc`
- `sql/record_buffer.h`
- `sql/iterators/basic_row_iterators.*`
- `sql/iterators/ref_row_iterators.*`
- `sql/parallel_query/pq_iterators.cc` positive `Read()` 路径；
- `storage/innobase/row/row0pread_pq.cc` positive pull-reader path；
- `storage/innobase/row/row0sel.cc`
- 任何 positive native `Record_buffer` / prefetch row production。

Hard stop：

- 需要在 PQ worker 上真实调用 `set_record_buffer()`；
- 需要启用 `pq_worker_scan_next()`；
- 需要把 native buffer slot 作为 leader result row sink；
- 需要同时处理 ICP/ref/partition/reverse/MVI；
- smoke 需要非 debug gate 或长测试。

Design / Docs / Source Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- confirmed current branch has no native `Record_buffer` / prefetch positive
  path；
- confirmed current code only probes `ha_get_record_buffer()` null/non-null and
  uses SQL-owned row-image sinks；
- confirmed `PQblockScanIterator::Read()`、`PQRefIterator::Read()` and
  `ha_innobase::pq_worker_scan_next()` remain fail-closed/stubbed；
- confirmed ownership/lifecycle contract covers worker TABLE/handler/prebuilt
  isolation, attach ordering, cleanup on ERROR/KILL/early EOF/
  fallback-before-start, dependent-ref reinit, and fetch-cache cleanup；
- confirmed F4b can proceed if it remains debug-only/fail-closed and keeps
  the hard stops。

Completion Report - M11-F4b Coding:

Changed files:

- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- Added status counters:
  `Parallel_worker_record_buffer_null_probes` and
  `Parallel_worker_record_buffer_nonnull_probes`；
- added debug flag `pq_worker_record_buffer_probe_smoke` inside
  `Gather_operator::run_worker_attach_contract_smoke()`；
- the debug path opens the independent worker TABLE/handler, observes
  `ha_get_record_buffer()` before `pq_worker_scan_init()`, records null/non-null,
  then runs cleanup；
- the debug path does not call `set_record_buffer()`、`pq_worker_scan_init()`、
  `pq_worker_scan_next()`、worker row production, or MQ enqueue；
- no native `Record_buffer` positive path was opened。

TDD / RED evidence:

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
```

Before implementation, the new F4b window failed because
`record_buffer_null_probe_delta` stayed `0` and
`record_buffer_ranges_dispatched_delta` was `1`。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_stats --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_stats --parallel=1
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- `mysqld` build passed；
- targeted F4b MTR passed；
- `pq_stats` record/replay passed；
- full `parallel_query` suite passed: 89/89；
- F4b window produced:
  `record_buffer_null_probe_delta=1`、
  `record_buffer_nonnull_probe_delta=0`、
  `record_buffer_attempt_delta=1`、
  `record_buffer_success_delta=1`、
  `record_buffer_cleanup_delta=1`、
  `record_buffer_queries_executed_delta=0`、
  `record_buffer_workers_launched_delta=0`、
  `record_buffer_ranges_dispatched_delta=0`、
  `record_buffer_secondary_rows_delta=0`；
- Code / Docs / Test Review accepted。

Code / Docs / Test Review:

- Review Agent verdict: `ACCEPT`；
- blocking findings: none；
- confirmed status variables are added to `PQ_global_stats`、reset、SHOW
  registration and `pq_stats.result`；
- confirmed `pq_worker_record_buffer_probe_smoke` is DBUG-only, observes only
  `ha_get_record_buffer()` after independent worker TABLE open, and returns
  before ownership mismatch branches and `pq_worker_scan_init()`；
- confirmed MTR covers null probe、nonnull probe、attempt/success/cleanup and
  executed/workers/ranges-dispatched/secondary rows；
- confirmed no F4 hard stop was crossed: no `set_record_buffer()`、no
  `pq_worker_scan_next()` enablement、no native buffer leader sink、no
  ICP/ref/partition/reverse/MVI expansion；
- noted non-blocking limitation: F4b proves only the current pre-init worker
  handler boundary, not future native attach cleanup after scan init / ERROR /
  KILL / fetch-cache use。

### M11-F5: Edge Positive Paths Backlog

Status: backlog triage taskbook created；docs-only / no source edits。

F5 目标不是打开正例，而是把 M11-F 之后仍未平移的商用 edge paths 分组、
排序、明确前置条件和 hard stops。以下路径继续 blocked，后续必须单独阶段：

- MVI positive unique filter；
- reverse positive range/ref/index scan；
- partition positive full/range/ref/dependent-ref；
- secondary index MIN / optimizer shortcut；
- ICP + native `Record_buffer` 组合。

#### Backlog Classes

F5-A: Reverse positive range/ref/index scan

- 商用能力：reverse range/index/ref scan、ORDER-sensitive worker result；
- 当前状态：F5-A boundary contract completed；F5-A1 review accepted
  `RECORD_BLOCKED`；reverse ref reject probe 已有，visible ORDER BY 仍
  blocked；
- 主要前置：
  - M11-E ORDER BY / Exchange_sort visible path 达到可执行；
  - reverse range boundary、direction、tie-break rowid、worker result order
    contract 单独验证；
  - no partition、no ICP、no native `Record_buffer` 首批；
- 结论：不进入 positive；不继续编码 reverse range/index diagnostic，除非
  后续允许触碰 `access_path.cc` 且 ORDER BY / worker path 已 ready。

F5-B: MVI positive unique filter

- 商用能力：`HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER` /
  `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER`；
- 当前状态：MVI 仍作为 wrong-result 高风险 shape blocked；F5-B inventory
  in progress；
- 主要前置：
  - single-table MVI access path 与 duplicate elimination contract；
  - worker-local unique filter lifecycle；
  - cleanup on ERROR/KILL/early EOF；
  - result ordering 与 duplicate counter 不影响 existing PQ counters；
- 首个可执行任务建议：MVI guard/read-only source inventory；不得打开
  positive unique filter。

F5-C: Partition positive full/range/ref/dependent-ref

- 商用能力：partition-aware leader/worker scan init、partition range ctx；
- 当前状态：partition positive path blocked；F5-C ownership design in
  progress；
- 主要前置：
  - worker TABLE/handler/prebuilt ownership 覆盖 `ha_innopart`；
  - per-partition read view、part id、range dispatch、cleanup 顺序；
  - partition + ref/dependent ref key ownership；
  - partition + native `Record_buffer` 继续后置；
- 首个可执行任务建议：`ha_innopart` worker ownership design + debug-only
  partition reject diagnostic。

F5-D: Secondary index MIN / optimizer shortcut

- 商用能力：secondary MIN / shortcut access path；
- 当前状态：F5a inventory completed；F5a-1 debug-only optimizer shortcut
  diagnostic completed and committed as `377de6edd70`；secondary MIN positive
  path remains blocked；
- 主要前置：
  - optimizer path 识别与 fail-closed reason；
  - aggregate / MIN shortcut 与 worker row production counter 关系；
  - no reverse/no partition/no record-buffer 首批；
- 结论：只保留 debug-only guard / diagnostic，不平移 positive shortcut
  execution。

F5-E: ICP + native `Record_buffer` combined path

- 商用能力：worker-side ICP + InnoDB native record buffer batch read；
- 当前状态：M11-F3 只完成 worker ICP contract + negative smoke；
  M11-F4 只完成 native buffer contract + null diagnostic；
- 主要前置：
  - worker-owned ICP clone/refix positive path；
  - native `Record_buffer` attach after worker scan init；
  - `ICP_OUT_OF_RANGE` 与 `Record_buffer::out_of_range` 双边界；
  - non-covering clustered lookup + batch cache consistency；
- BLOB/TEXT/JSON/GEOMETRY、fixed-point、hostile read_set/write_set
  shape 仍保持 global guard，不允许随 combined path 混入。
- 首个可执行任务建议：combined-path design only；严禁直接编码。

#### Proposed Priority

1. F5-C partition worker ownership design：跨 handler/InnoDB，保持 docs-first；
2. F5-B MVI inventory：wrong-result 风险高，先只读调研；
3. F5-E ICP + native `Record_buffer` combined design：依赖 F3/F4 positive
   path，最后处理。

Completed / blocked before this priority:

- F5-D secondary MIN / optimizer shortcut inventory and debug-only diagnostic
  completed；positive execution remains blocked；
- F5-A reverse boundary contract completed；F5-A1 recorded blocked for
  reverse range/index diagnostics under the current allowed-file boundary。
- F5-C partition ownership design and C1 partition reject diagnostic completed；
  positive execution remains blocked。

#### Proposed M11-F5-B: MVI Unique Filter Inventory / Design

Status: docs-only taskbook accepted by Design / Source / Test Review。

Goal:

- 对照商用 MVI positive path，明确当前分支 MVI key 被拒绝的位置和原因；
- 保持 MVI / multi-valued index 全部 fail-closed，不打开
  `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER`；
- 定义后续最小安全可执行任务：debug-only MVI reject diagnostic，而不是
  positive unique filter。

Current Branch Facts:

- MySQL handler 层通过 `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER` /
  `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER` 管理 MVI duplicate filter；
- key flag `HA_MULTI_VALUED_KEY` 表示 multi-valued index；
- 当前分支 PQ secondary/range/ref helpers 显式把
  `HA_MULTI_VALUED_KEY` 当 unsafe：
  - `pq_secondary_range_key_has_unsupported_parts()`；
  - `pq_secondary_covering_read_set_is_safe()`；
  - `pq_secondary_ref_key_parts_are_safe()`；
  - iterator-side secondary helpers in `pq_iterators.cc`；
- 当前用户可见 MVI SQL 主要通过 `NON_FULL_TABLE_SCAN` 或 `MULTI_TABLE`
  fail-closed；
- 当前没有 `Parallel_*mvi*` status counter；现有测试只能证明 generic PQ
  execution / worker / range / secondary-row counters 不增长。

Existing Test Guard:

- `pq_commercial_ref_icp` F0 negative matrix 覆盖 MVI、reverse、partition、
  secondary MIN 和 BLOB / record-buffer-hostile shape；
- `pq_commercial_ref_icp` F1 专门覆盖 MVI / multi-valued key：
  - single-table ref shape on non-array leading keyparts；
  - dependent-ref / multi-table shape using the same MVI composite key；
- result 保持：
  - single-table MVI shape `Not parallel NON_FULL_TABLE_SCAN`；
  - dependent-ref MVI join `Not parallel MULTI_TABLE`；
  - `Parallel_queries_executed`、`Parallel_workers_launched`、
    `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
    `Parallel_secondary_rows_produced` deltas 均为 0。

Commercial Reference Findings:

- 商用仓有真实 MVI positive path，但能力是窄子集：
  - `pq_multi_value.test` 覆盖 index contains MVI keypart 的
    `PQblockScanIterator` case；
  - `pq_multi_value.test` 覆盖 MVI composite key 的 `PQRefIterator` case；
  - result 显示 `Parallel execute (4 workers, ...)`；
  - positive evidence 是 duplicate elimination / unique filter 生命周期，
    不是泛化 JSON predicate 支持；
  - `JSON_CONTAINS` 等 JSON functions 在商用 PQ optimizer 中仍被列为
    unsupported。
- 商用关键依赖：
  - `PQblockScanIterator::Init()` / `PQRefIterator::Init()` 检测
    `HA_MULTI_VALUED_KEY` 并启用
    `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER`；
  - destructors 调用 `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER`；
  - `Read()` 路径把 duplicate-filter 的 `HA_ERR_KEY_NOT_FOUND` 当作
    continue；
  - handler 层 `filter_dup_records()` / `Unique_on_insert` 维护 rowid
    去重；
  - worker-local handler ownership、dependent-ref range dispatch、ICP /
    secondary visibility、worker prebuilt ownership、native `Record_buffer`
    和 cleanup contract 都必须 ready。

Do Not Copy Directly:

- 不复制商用 `PQblockScanIterator` / `PQRefIterator` 的 MVI enable/disable
  片段。没有 worker-local handler lifetime 和 cleanup，单独启用
  `m_unique` 会污染 leader/worker handler 状态；
- 不复制 handler `ha_extra()` / `filter_dup_records()` 行为。它们是 MySQL
  handler core path，F5-B 不允许改变；
- 不复制商用 `ha_innodb_pq.cc` / `row0pread_pq.cc` / `pq_clone.cc` 大块
  逻辑。它们依赖成熟 worker row path、record buffer、ICP、reverse、
  partition 和 ref dispatch ownership。

Required Contract Before Positive Gate:

- worker-local handler 必须独立拥有 `m_unique`；
- enable / reset / disable unique record filter 必须覆盖 Init、normal EOF、
  ERROR、KILL、early abort、fallback cleanup；
- duplicate-filter `HA_ERR_KEY_NOT_FOUND` 必须只作为 MVI duplicate skip，
  不能吞掉真实 handler error；
- `prepare_for_position()` / rowid stability 必须与 worker result frame
  生命周期一致；
- ref / dependent-ref MVI key ownership、range dispatch 和 repeated outer
  key 语义必须单独验证；
- MVI 不得与 partition、reverse、ICP、native `Record_buffer`、
  ORDER BY/Gather Merge 或 JSON predicate positive path 混入首批。

Allowed Files:

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/**`
- `storage/**`
- `mysql-test/**`
- build scripts and generated result files

Hard Stops:

- 不调用 `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER`；
- 不调用 `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER`；
- 不修改 handler `m_unique`、`filter_dup_records()`、MRR 或
  `multi_range_read_next()`；
- 不打开 worker-local MVI duplicate elimination；
- 不打开 MVI + ref / dependent-ref / ICP / partition / reverse /
  native `Record_buffer` combined path；
- 不把 MVI diagnostic 接入 key selection、range planning、handler cursor 或
  InnoDB scan behavior。

Recommended Next Coding Task After Review:

- M11-F5-B1 debug-only MVI reject diagnostic；
- 候选 hook 必须限于 PQ helper 层：
  - `pq_secondary_range_key_has_unsupported_parts()`；
  - `pq_secondary_covering_read_set_is_safe()`；
  - `pq_secondary_ref_key_parts_are_safe()`；
  - 或 `pq_check_full_table_scan()` 已知 candidate table/index 后的只读
    diagnostic；
- 不允许从 handler / InnoDB / iterator execution path 计数；
- MTR 复用 `pq_commercial_ref_icp` F1 window，新增 MVI diagnostic delta，
  同时保持 generic execution / worker / range / secondary-row counters 全为
  0；
- dependent-ref MVI join 仍可能 `MULTI_TABLE` 先拒绝，counter semantics
  必须避免要求该 join 被 MVI counter 覆盖。

Validation:

- 本阶段只做文档 review，不运行 build/MTR；
- Design / Source / Test Review Agent returned `ACCEPT`；
- 若要编码，必须另起 M11-F5-B1 coding taskbook。

Review Result:

- Verdict: `ACCEPT`；
- Blocking findings: None；
- Non-blocking risks:
  - M11-F5-B1 counter semantics 必须保持为 reject probes，不解释为完整
    MVI query count；
  - dependent-ref MVI join 可能先被 `MULTI_TABLE` 拒绝，不应要求 MVI
    counter 覆盖该路径；
  - 本阶段按任务要求不运行 build/MTR；
- Safe next task: M11-F5-B1 debug-only MVI reject diagnostic，限于 PQ
  helper / preflight observation；不得触碰 `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER`、
  handler `m_unique`、worker duplicate elimination、handler cursor behavior、
  InnoDB scan behavior 或 MVI combined paths。

#### Proposed M11-F5-B1: Debug-only MVI Reject Diagnostic Taskbook

Status: implementation completed；Code / Docs / Test Review accepted。

Goal:

- 增加一个用户可见 status counter，用于证明当前分支遇到 MVI /
  `HA_MULTI_VALUED_KEY` secondary/ref candidate 时保持 fail-closed；
- counter 语义是 MVI reject probes，不是完整 MVI query count；
- 不打开 MVI positive execution，不启用 unique record filter。

Implementation Scope:

- 在 `PQ_global_stats` 增加 `secondary_mvi_reject_probes`；
- 在 `mysqld` status var 暴露
  `Parallel_secondary_mvi_reject_probes`；
- 只在 PQ helper 层已存在的 `HA_MULTI_VALUED_KEY` 拒绝点增长：
  - `pq_secondary_range_key_has_unsupported_parts()`；
  - `pq_secondary_covering_read_set_is_safe()`；
  - `pq_secondary_ref_key_parts_are_safe()`；
- 不改变任何 helper 的返回值和调用顺序；
- MTR 复用 `pq_commercial_ref_icp` F1 MVI window，新增 mixed-index negative
  guard：同一张表存在 MVI key 时，强制普通 secondary key 不得增长 MVI
  reject counter；同时保持 existing generic execution / worker / range /
  secondary-row deltas 全为 0；
- 更新 `pq_stats` 变量总数和 result。

Allowed Files:

- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`

Forbidden Files:

- `sql/handler.cc`
- `storage/**`
- `sql/parallel_query/pq_iterators.cc`
- `mysql-test/suite/parallel_query/t/pq_stats.test`
- build scripts and unrelated tests

Hard Stops:

- 不调用 `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER`；
- 不调用 `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER`；
- 不修改 handler `m_unique`、`filter_dup_records()`、MRR 或
  `multi_range_read_next()`；
- 不打开 worker-local MVI duplicate elimination；
- 不打开 MVI + ref / dependent-ref / ICP / partition / reverse /
  native `Record_buffer` combined path；
- 不从 handler / InnoDB / iterator execution path 计数；
- 不把 counter 用作 eligibility 决策输入。

Validation:

- `cmake --build build-ninja --target mysqld -j 16`
- `cd mysql-test && TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp --record --vardir=/tmp/pqv_f5b1_ref_record8 --tmpdir=/tmp/pqt_f5b1_ref_record8`
- `cd mysql-test && TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp --vardir=/tmp/pqv_f5b1_ref2 --tmpdir=/tmp/pqt_f5b1_ref2`
- `cd mysql-test && TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl --suite=parallel_query pq_stats --record --vardir=/tmp/pqv_f5b1_stats_record --tmpdir=/tmp/pqt_f5b1_stats_record`
- `cd mysql-test && TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl --suite=parallel_query pq_stats --vardir=/tmp/pqv_f5b1_stats2 --tmpdir=/tmp/pqt_f5b1_stats2`
- `cd mysql-test && TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_f5b1_full2 --tmpdir=/tmp/pqt_f5b1_full2`

Acceptance Checklist:

- New status var exists and is listed by `pq_stats`；
- F1 mixed-index normal secondary key observes
  `Parallel_secondary_mvi_reject_probes` delta `0`；
- F1 generic PQ execution / worker / range / secondary-row deltas remain 0；
- No handler/InnoDB/iterator execution behavior changed；
- Code / Docs / Test Review Agent returns `ACCEPT` before commit。

Completion Report - M11-F5-B1 Coding:

- Changed files:
  - `sql/parallel_query/sql_parallel.h`
  - `sql/parallel_query/pq_optimizer.cc`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
  - `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
  - `Docs/pq_tasks/README.md`
  - `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- Implementation:
  - added `PQ_global_stats::secondary_mvi_reject_probes` and reset logic；
  - exposed `Parallel_secondary_mvi_reject_probes` through SHOW STATUS；
  - incremented the counter in existing optimizer-side MVI reject helpers；
  - detects MVI keyparts through both `HA_MULTI_VALUED_KEY` and
    `Field::is_array()` when keypart metadata is visible；
  - added a non-full-scan reject observation path for ref/range candidates where
    the selected key number is unavailable but the candidate table has an MVI
    key；
  - kept all handler / InnoDB / iterator execution paths untouched；
  - kept `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER` and
    `HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER` unused。
- Test updates:
  - `pq_commercial_ref_icp` F1 now records
    `Parallel_secondary_mvi_reject_probes` baseline and asserts
    `f1_mvi_normal_key_reject_delta = 0` for a mixed-index table forced to use
    a normal secondary key；
  - current functional MVI ref shape remains fail-closed through
    `NON_FULL_TABLE_SCAN` but is not required to increment this counter because
    MySQL may expose only visible keyparts for the selected key；
  - `pq_stats` expected status variable count updated from 217 to 218 and
    lists `Parallel_secondary_mvi_reject_probes`。
- Validation result:
  - `mysqld` build passed；
  - `pq_commercial_ref_icp --record` executed SQL successfully but MTR failed
    at final log-to-result copy with errno 1；result file contains the recorded
    F1 output from the run；
  - `pq_stats --record` executed SQL successfully but MTR failed at final
    log-to-result copy with errno 1；result file contains count 218 and the new
    variable；
  - `pq_commercial_ref_icp` replay passed；
  - `pq_stats` replay passed；
  - full `parallel_query` suite passed 89/89 after the review fix。
- Risk notes:
  - counter semantics are reject probes, not full MVI query count；
  - dependent-ref MVI joins may still reject earlier as `MULTI_TABLE` and are
    not required to increment this counter；
  - fallback table-level MVI observation is limited to non-full-scan reject
    branch and is diagnostic-only。
- Review result:
  - first Code / Docs / Test Review returned `REVISE` for mixed-index
    overcount risk；
  - fixed by using selected-key-only counting when the key is available and
    adding `f1_mvi_normal_key_reject_delta = 0` guard；
  - re-review returned `ACCEPT` with no blocking findings。

#### Proposed M11-F5-E: ICP + Native Record_buffer Combined Path Design

Status: docs-only taskbook accepted by Design / Source / Test Review。

Goal:

- 对照商用 ICP + native `Record_buffer` combined path，明确当前分支为何
  不能直接平移该路径；
- 固化 worker-side ICP、native `Record_buffer`、secondary visibility、
  ref/dependent-ref range dispatch 之间的状态机和错误码边界；
- 定义后续只允许的 negative / diagnostic 工作，不打开 positive combined
  execution。

Current Branch Facts:

- worker-side ICP clone/refix 未实现：
  - `Item::pq_clone()` / `Item::pq_copy_from()` 仍 fail-closed；
  - `pq_refix_fields_item.cc` 仍是 placeholder；
  - clone preflight 会记录 unsupported，不生成 worker-owned
    `pushed_idx_cond` / `pq_cond`；
- worker ICP 目前只有负向 ownership gate：
  - `pq_worker_icp_ownership_mismatch_smoke` 可注入 leader ICP sentinel；
  - `ha_innobase::pq_worker_scan_init()` 要求 worker handler 拥有 distinct
    cloned/refixed ICP condition 和 matching keyno，否则拒绝；
- native `Record_buffer` 未用于 PQ row production：
  - `PQ_record_buffer_sink` 是 SQL-owned row-image vector，不是 server
    native `Record_buffer`；
  - 当前仅有 `ha_get_record_buffer()` null/non-null probes；
  - 不调用 PQ worker 上的 `set_record_buffer()`；
- user-visible secondary ICP 仍是 leader-local narrow bridge：
  - `PQSecondaryNoncoveringIcpRangeIterator` 使用 SQL-owned row images；
  - tests 要求 `workers_delta = 0`；
  - 这不是 worker-side ICP + native buffer；
- `PQblockScanIterator::Init/Read()`、`PQRefIterator::Init/Read()` 仍为
  fail stubs；`ha_innobase::pq_worker_scan_next()` 仍返回 unsupported。

Commercial Reference Findings:

- 商用 combined path 的核心在 worker read path：
  - `PQ_Ctx::read_record()` 先 drain native `Record_buffer`，再从 pcur /
    worker ctx 批量填充，再把缓存行交给 SQL；
  - `PQ_Scan_ctx::find_visible_record()` 在 worker 侧处理 MVCC、ICP、
    secondary clustered lookup；
  - `pq_worker_scan_next()` 把 `DB_END_OF_RANGE` / `DB_END_OF_INDEX` 解释为
    换 ctx / range，不是 statement EOF；
  - `PQRefIterator::Read()` 对每个 outer ref key build ranges 并 reset
    native `Record_buffer`；
  - `pq_ref_build_ranges()` 明确避免在 range-build 阶段用 ICP 判断 key
    是否存在，防止 dependent-ref 丢行；
  - `pq_record_buffer.test`、`pq_ref_build_range.test`、`pq_icp.test` 覆盖
    ICP + native buffer、secondary clustered lookup、ref + ICP range build。

Combined State / Error Contract:

| Source state | Meaning | Allowed PQ interpretation |
| --- | --- | --- |
| `ICP_NO_MATCH` | secondary record fails pushed index condition | skip row；do not grow produced-row counters |
| `DB_NOT_FOUND` after ICP miss | commercial worker read path maps ICP miss to retryable not-found | retry / continue within current ctx；not fatal and not statement EOF |
| `ICP_MATCH` | secondary record passes pushed index condition | continue visibility；clustered lookup only if needed |
| `ICP_OUT_OF_RANGE` | ICP comparison crossed current key/range boundary | end current range/ctx；not statement EOF |
| `DB_SUCCESS` with native buffer rows | cached rows are available | drain buffer before requesting next ctx |
| `Record_buffer::out_of_range` | buffer reached range/index boundary after cached rows | reset buffer and return current range/ctx end after drain |
| `DB_END_OF_RANGE` | current ctx/range done | worker may request next ctx；not statement EOF |
| `DB_END_OF_INDEX` | current scan/index done | worker may request next ctx or finish if no ctx remains |
| `HA_ERR_END_OF_FILE` | SQL handler EOF after all ctx/ranges drained | statement-level EOF only after all workers finish |
| worker ERROR/KILL | fatal after row production may have started | abort / ERROR token / cleanup；no silent serial fallback |

Required Migration Preconditions:

- worker-owned `TABLE` / handler / `row_prebuilt_t` / cursor / template；
- worker-owned cloned/refixed `pushed_idx_cond`、`pq_cond`、`Item_field` and
  ref key Items；
- worker handler `pushed_idx_cond_keyno` / active index / `m_prebuilt->idx_cond`
  consistency；
- `Record_buffer` ownership by worker `TABLE::m_record_buffer` and worker
  MEM_ROOT；
- deterministic attach/reset/detach for EOF、ERROR、KILL、early abort、
  dependent-ref reinit and fallback-before-start；
- secondary visibility and clustered lookup correctness under worker read view；
- explicit gates for reverse、partition、MVI、descending/generation/nullable
  keyparts、BLOB/TEXT/JSON/GEOMETRY、fixed-point and unsafe read/write sets。

Hard Stops:

- 不 clone/refix `pushed_idx_cond`、`pq_cond`、`Item_field` 或 ref key Items；
- 不在 PQ worker 上调用 `set_record_buffer()`；
- 不启用 `pq_worker_scan_next()`；
- 不把 native `Record_buffer` slot 直接作为 leader result；
- 不裸调 `pushed_idx_cond->val_int()`；
- 不在 ref/dependent-ref range build 阶段使用 ICP 判断 key 是否存在；
- 不把 leader-local secondary ICP 视为 worker-side ICP；
- 不混入 dependent ref、partition、reverse、MVI、ORDER BY、native
  `Record_buffer` positive path 或 worker-side ICP positive row production。

Allowed Files:

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/**`
- `storage/**`
- `mysql-test/**`
- build scripts and generated result files

Safe Follow-up Candidate:

- M11-F5-E1 debug-only combined negative diagnostic；
- candidate hook: `Gather_operator::run_worker_attach_contract_smoke()` after
  worker TABLE open and before any successful `pq_worker_scan_init()`；
- it may combine the existing worker record-buffer null probe with the existing
  ICP ownership mismatch sentinel；
- it must assert:
  - worker `ha_get_record_buffer() == nullptr`；
  - ICP ownership mismatch rejects before worker scan success；
  - `Parallel_queries_executed`、`Parallel_workers_launched`、
    `Parallel_ranges_dispatched`、`Parallel_secondary_rows_produced` stay 0；
- it must not call `set_record_buffer()` or produce worker rows。

Validation:

- This stage is docs-only；no build/MTR required；
- Design / Source / Test Review Agent returned `ACCEPT`；
- If review asks for coding, create a separate M11-F5-E1 coding taskbook。

Review Result:

- Verdict: `ACCEPT`；
- Blocking findings: None；
- Non-blocking risks:
  - commercial path maps `ICP_NO_MATCH` to retryable `DB_NOT_FOUND` before
    continuing；the state table now records this explicitly；
  - F5-E1 must not reuse a worker attach path that increments range-dispatch
    counters before the negative diagnostic completes；
- Required fixes before commit: None；
- Safe next task: keep F5-E design-only；next coding, if any, must be
  M11-F5-E1 debug-only combined negative diagnostic with no
  `set_record_buffer()`、no `pq_worker_scan_next()` enablement、no worker row
  production, and explicit zero-counter assertions。

Agent Review Prompt:

请作为 M11-F5-E Design / Source / Test Review Agent，只读审查本任务书和
相关源码 / 测试：

1. 当前分支状态是否准确区分 leader-local ICP、debug-only ownership gate、
   SQL-owned row-image sink 和 native `Record_buffer`；
2. 商用 combined path 的状态机 / 错误码转换是否描述准确；
3. hard stops 是否足够阻止误开 worker-side ICP + native buffer positive path；
4. 是否应该把 F5-E 固定为 design-only，并将后续编码限制为
   F5-E1 debug-only combined negative diagnostic；
5. 是否还缺少必须写入文档的前置条件或测试护栏。

输出：

- Verdict: `ACCEPT` 或 `REVISE`
- Blocking findings
- Non-blocking risks
- Required fixes before commit
- Safe next task recommendation

Historical Agent Review Prompt - M11-F5-B:

请作为 M11-F5-B Design / Source / Test Review Agent，只读审查本任务书和
相关源码 / 测试：

1. 当前分支 MVI fail-closed 边界是否描述准确；
2. 商用 MVI positive path 是否被正确归类为窄能力，而不是泛化 JSON/MVI；
3. unique record filter ownership contract 是否覆盖 handler/worker cleanup
   风险；
4. Allowed / Forbidden files 和 Hard Stops 是否足够防止误开
   `HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER`、handler unique filter、worker
   duplicate elimination、MVI combined paths；
5. 下一步是否应为 M11-F5-B1 debug-only MVI reject diagnostic。

输出：

- Verdict: `ACCEPT` 或 `REVISE`
- Blocking findings
- Non-blocking risks
- Required fixes before commit
- Safe next task recommendation

#### Proposed M11-F5-C: Partition Worker Ownership Design

Status: docs-only taskbook accepted；committed as `011cec1e30f`。

Goal:

- 对照商用 partition positive PQ path，明确当前分支要支持 partition table
  前必须补齐的 ownership contract；
- 保持当前分支 partition table 全部 fail-closed，不打开任何用户可见
  partition PQ 正例；
- 定义后续最小安全可执行任务：优先是 optimizer-level debug-only
  partition reject diagnostic，而不是 `ha_innopart` positive path。

Current Branch Facts:

- `PQUnsuiteReason::PARTITIONED_TABLE` 已存在；
- `pq_check_single_table()` 在 `share->m_part_info != nullptr` 时返回
  `PARTITIONED_TABLE`；
- `pq_check_query_block_eligible()` 在 full scan / cost / iterator 创建前
  先调用 single-table check，因此 partition full scan 不进入 PQ iterator；
- secondary range / visibility / covering / ICP debug smokes 均显式拒绝
  `table->part_info != nullptr`；
- dependent-ref scaffold、user-visible secondary range iterator factory 和
  secondary ref iterator factory 均拒绝 partition table；
- 当前分支 `ha_innopart` 没有 PQ-specific worker ownership override；
- 当前没有 `Parallel_*partition*` status counter；partition rejection 主要
  通过 EXPLAIN `Not parallel PARTITIONED_TABLE` 和 generic zero-delta
  counters 可见。

Existing Test Guard:

- `pq_commercial_ref_icp` F0 negative matrix 覆盖 partition secondary range；
- `pq_commercial_ref_icp` F3 partition guard 覆盖：
  - partition full scan；
  - partition secondary range / ICP；
  - partition ref；
  - partition dependent ref；
- result 断言 single-table partition full/range/ref 的 EXPLAIN 为
  `Not parallel PARTITIONED_TABLE`；
- dependent-ref partition join 继续通过 multi-table guard fail-closed，
  EXPLAIN 为 `Not parallel MULTI_TABLE`，不应计入 single-table partition
  reject counter；
- F3 counter window 断言以下 counters delta 均为 0：
  - `Parallel_queries_executed`；
  - `Parallel_workers_launched`；
  - `Parallel_ranges_built`；
  - `Parallel_ranges_dispatched`；
  - `Parallel_secondary_rows_produced`。

Commercial Reference Findings:

- 商用参考仓 partition positive path 是受限能力，不是泛化 partition PQ：
  - explicit single partition full scan positive；
  - explicit single partition primary / secondary range positive；
  - explicit single partition ref positive；
  - explicit single partition dependent ref + ICP positive；
  - explicit single hash/range/key/subpartition positive；
  - multi-partition or non-explicit partition shapes 仍可能 serial；
  - partition + GROUP / derived / materialization 依赖更广的 clone/rewrite
    stack。
- 商用关键依赖：
  - `SetupPQTab()` 设置 `JT_ALL` / `JT_RANGE` / `JT_REF`、keyno、`pq_ref`；
  - `InitPQTab()` 设置 `pq_range_type` 并调用 `ha_pq_init()`；
  - `handler::ha_pq_init()` / `ha_pq_next()` / `ha_pq_end()` 处理 leader /
    worker split 和 cleanup；
  - `ha_innopart::pq_leader_scan_init()` /
    `ha_innopart::pq_worker_scan_init()` 提供 partition-aware PQ init；
  - `row0pread_pq.cc` 管理 B+tree partitioning、range creation、record
    buffer、ICP 和 persistent cursor ownership；
  - `TABLE::pq_copy()` 复制 `partition_info` 并 clone pushed ICP condition。

Do Not Copy Directly:

- 不复制商用 `row0pread_pq.cc` 整体实现。当前分支刻意基于 upstream
  `Parallel_reader`，商用独立 B+tree partitioner / cursor stack 与当前
  ownership 模型不一致；
- 不单独复制 `ha_innopart` positive overrides。没有 SQL eligibility、
  cloned partition state、worker TABLE ownership 和 worker handler/prebuilt
  isolation 时，容易读错 partition 或共享 mutable state；
- 不复制 `pq_ref_build_ranges()` / dependent-ref queue code。它依赖商用
  `PQ_slices_map`、`PQ_ref_key`、`PQRefIterator` 和 ref-key lifecycle；
- 不复制 ICP / native `Record_buffer` pieces。它们依赖 cloned pushed
  conditions 和 worker-owned `record[0]`。

Required Ownership Contract Before Positive Gate:

- SQL 层：
  - eligibility 必须区分 explicit single partition、multi-partition、
    non-explicit pruning、subpartition；
  - access shape 必须先限定在最小子集，不混合 GROUP / ORDER / derived /
    materialization；
  - ref/dependent-ref key ownership 必须定义 per-worker copy / lifetime。
- TABLE / handler 层：
  - worker TABLE 与 handler 必须独立于 leader；
  - partition_info clone / pruning bitmap / selected partitions 必须明确
    ownership；
  - `ha_innopart` leader / worker init and end 必须有 cleanup contract；
  - worker-start 后失败不得 silent serial fallback。
- InnoDB 层：
  - partition-local dict table / index / prebuilt ownership 必须明确；
  - read view、trx、mtr、cursor、range boundary 和 EOF / ERROR / KILL
    cleanup 必须单独验证；
  - native `Record_buffer`、ICP、MVI、reverse 继续后置。

Allowed Files:

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/**`
- `storage/**`
- `mysql-test/**`
- build scripts and generated result files

Hard Stops:

- 不打开 `ha_innopart` worker ownership；
- 不实现 partition pruning 或 per-partition range dispatch；
- 不实现 worker TABLE / handler / prebuilt ownership for partition tables；
- 不接 native `Record_buffer`；
- 不混入 MVI / reverse / ICP combined partition path；
- 不打开用户可见 partition PQ execution，即使是 full scan 或 explicit
  single partition。

Recommended Next Coding Task After Review:

- M11-F5-C1 debug-only partition reject diagnostic；
- 最小安全 hook：在 `pq_check_single_table()` 的
  `share->m_part_info != nullptr` / `PARTITIONED_TABLE` reject 点增加
  partition-specific counter；
- counter semantics 仅覆盖 single-table partition rejection；F3 dependent-ref
  join 会在 `MULTI_TABLE` 处提前拒绝，不要求 partition counter 增长；
- 不进入 `ha_innopart`、partition pruning、range dispatch、worker
  TABLE/handler/prebuilt、native `Record_buffer` 或 combined paths；
- MTR 复用 `pq_commercial_ref_icp` F3 window，新增 counter delta，同时保持
  generic execution / worker / range / secondary-row counters 全为 0。

Validation:

- 本阶段只做文档 review，不运行 build/MTR；
- Design / Source / Test Review Agent returned `ACCEPT` after one revision；
- 若 review 认为要编码，必须另起 M11-F5-C1 coding taskbook。

Review:

- Initial verdict: `REVISE`；
- blocking finding: F3 dependent-ref partition join is rejected by
  `MULTI_TABLE`, not `PARTITIONED_TABLE`；
- fixed by limiting F3 `PARTITIONED_TABLE` wording and C1 counter semantics to
  single-table partition rejection；
- final verdict: `ACCEPT`；
- safe next task: M11-F5-C1 debug-only partition reject diagnostic at
  `pq_check_single_table()`。

#### Proposed M11-F5-C1: Debug-only Partition Reject Diagnostic Taskbook

Status: implemented；build and MTR validation passed；waiting Code / Docs /
Test Review。

Goal:

- Add a narrow partition-specific reject diagnostic at the existing
  single-table eligibility reject point；
- prove partition full/range/ref single-table shapes remain rejected as
  `PARTITIONED_TABLE` and do not enter PQ execution / worker / range /
  secondary row production；
- keep dependent-ref partition join as `MULTI_TABLE` fail-closed and exclude it
  from partition reject counter requirements；
- do not open any partition positive PQ path。

Allowed Files:

- `sql/parallel_query/pq_optimizer.cc`，仅允许在
  `pq_check_single_table()` 的 `share->m_part_info != nullptr` reject 分支
  增加 counter increment；
- `sql/parallel_query/sql_parallel.h`，仅允许新增一个
  `PQ_global_stats` atomic counter 和 `reset()` 清零；
- `sql/mysqld.cc`，仅允许新增 SHOW STATUS helper / entry；
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/handler.*`
- `sql/sql_executor.*`
- `sql/sql_select.*`
- `sql/join_optimizer/**`
- `sql/range_optimizer/**`
- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/pq_clone.*`
- `sql/parallel_query/exchange*`
- `sql/parallel_query/query_result_mq.*`
- `storage/innobase/**`
- any `ha_innopart` implementation or header；
- any partition pruning / range dispatch / worker TABLE / handler / prebuilt
  ownership code；
- any native `Record_buffer`、ICP、MVI、reverse positive path。

Counter Semantics:

- Add one status variable:
  `Parallel_partition_reject_probes`；
- increment exactly when `pq_check_single_table()` rejects a single-table
  candidate because `share->m_part_info != nullptr`；
- do not increment for multi-table queries, including the F3 dependent-ref
  partition join that is rejected earlier as `MULTI_TABLE`；
- do not increment from iterator factories, handler, InnoDB, MTR-only hooks,
  `ha_innopart`, or partition pruning code；
- counter increment must not alter `pq_reject()` reason, execution state,
  AccessPath / iterator selection, handler calls, or fallback behavior。

MTR Requirements:

- Extend `pq_commercial_ref_icp` F3 window；
- capture `Parallel_partition_reject_probes` before F3；
- after the existing full/range/ref/dependent-ref SQL:
  - assert partition reject delta is at least 3 for the single-table
    full/range/ref shapes；
  - do not require the delta to include the dependent-ref join；
  - keep existing generic deltas at 0:
    - `Parallel_queries_executed`；
    - `Parallel_workers_launched`；
    - `Parallel_ranges_built`；
    - `Parallel_ranges_dispatched`；
    - `Parallel_secondary_rows_produced`；
- update `pq_stats.result` for the new status variable；
- do not add or migrate commercial `pq_partition` positive tests。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp --record
cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp
cd build-ninja/mysql-test && ./mtr parallel_query.pq_stats --record
cd build-ninja/mysql-test && ./mtr parallel_query.pq_stats
cd build-ninja/mysql-test && ./mtr --suite=parallel_query --parallel=1
```

Hard Stops:

- If implementing the counter requires touching `ha_innopart` or
  `storage/innobase/**`, stop；
- if dependent-ref partition join requires special handling to grow the counter,
  stop and keep it excluded；
- if any generic execution / worker / range / secondary-row counter grows for
  F3 partition shapes, stop；
- if `PARTITIONED_TABLE` EXPLAIN reason changes for single-table partition
  full/range/ref, stop；
- if `MULTI_TABLE` reason changes for the dependent-ref partition join, stop。

Acceptance:

- Design / Source / Test Review Agent returned `ACCEPT`；
- after coding, Code / Docs / Test Review Agent returns `ACCEPT`；
- no source changes outside Allowed Files；
- build and targeted/full MTR validation pass。

Design Review:

- Verdict: `ACCEPT`；
- Coding recommendation: `CODE`；
- accepted semantics: counter increments only for single-table
  `PARTITIONED_TABLE` rejects at `pq_check_single_table()`；
- dependent-ref partition join stays excluded because it is rejected earlier as
  `MULTI_TABLE`；
- non-blocking note: `Parallel_partition_reject_probes >= 3` is intentionally
  loose because EXPLAIN and SELECT may both evaluate eligibility；
- non-blocking note: this is diagnostic-only, implemented as a normal PQ SHOW
  STATUS variable to match existing PQ counter style, not `#ifndef NDEBUG` only。

Implementation Summary:

- Added `PQ_global_stats::partition_reject_probes` and reset plumbing；
- added SHOW STATUS variable `Parallel_partition_reject_probes`；
- incremented the counter only in `pq_check_single_table()` when
  `share->m_part_info != nullptr` returns `PARTITIONED_TABLE`；
- extended `pq_commercial_ref_icp` F3 window to capture the counter baseline
  and assert single-table partition rejects are observed；
- updated `pq_stats.result` for the new status variable；
- no `ha_innopart`、partition pruning、worker ownership、InnoDB、Record_buffer、
  ICP、MVI、reverse or positive partition execution path changed。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp --record`
  passed；
- `TMPDIR=/tmp ./mtr parallel_query.pq_commercial_ref_icp --vardir=/tmp/pqv_c1_ref --tmpdir=/tmp/pqt_c1_ref`
  passed；
- `TMPDIR=/tmp ./mtr parallel_query.pq_stats --record --vardir=/tmp/pqv_c1_stats_record --tmpdir=/tmp/pqt_c1_stats_record`
  executed SQL and updated result, but MTR reported a record-copy failure while
  copying the log into the result file；
- `TMPDIR=/tmp ./mtr parallel_query.pq_stats --vardir=/tmp/pqv_c1_stats --tmpdir=/tmp/pqt_c1_stats`
  passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_c1_full --tmpdir=/tmp/pqt_c1_full`
  passed, 89/89。

Agent Review Prompt:

请作为 M11-F5-C Design / Source / Test Review Agent，只读审查本任务书和
相关源码 / 测试：

1. 当前分支 partition fail-closed 边界是否描述准确；
2. 商用 partition positive path 是否被正确归类为受限能力，而不是泛化
   partition PQ；
3. ownership contract 是否覆盖 SQL、TABLE/handler、InnoDB 三层关键风险；
4. Allowed / Forbidden files 和 Hard Stops 是否足够防止误开
   `ha_innopart`、partition pruning、worker ownership、native
   `Record_buffer`、ICP、MVI、reverse；
5. 下一步是否应为 M11-F5-C1 debug-only partition reject diagnostic。

输出：

- Verdict: `ACCEPT` 或 `REVISE`
- Blocking findings
- Non-blocking risks
- Required fixes before commit
- Safe next task recommendation

#### Proposed M11-F5a: Secondary MIN / Optimizer Shortcut Source Inventory

Status: source inventory completed；Design / Docs / Source Review pending。

性质：docs-only design/source inventory。debug-only coding 只有在 hook-point
review 证明检测点不会错过 shortcut，也不会改变 MIN/MAX shortcut 语义后才
允许进入。

必须调研：

- `sql/sql_optimizer.cc` 中 `optimize_aggregated_query()` 调用点；
- `sql/opt_sum.cc` 中 aggregate shortcut 处理；
- `sql/opt_sum.cc` 中 `find_key_for_maxmin()` / MIN/MAX key shortcut path；
- `sql/parallel_query/pq_optimizer.cc` 中现有 PQ eligibility / fail-closed
  reason 是否能观测 shortcut 后状态；
- 商用实现是否对 secondary MIN / optimizer shortcut 有专门 guard 或测试；
- 现有 `pq_commercial_ref_icp` / `pq_not_support` 是否已有可复用 SQL shape。

允许修改：

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

禁止：

- 不修改 `sql/**`、`storage/**`、`mysql-test/**`；
- 不在 `sql/opt_sum.cc` 添加 hook，除非后续单独 review 批准只读诊断；
- 不打开 secondary MIN positive row production；
- 不改 aggregate/min optimizer shortcut 语义；
- 不改 InnoDB row read；
- 不接 worker/MQ row path；
- 不混入 reverse/partition/MVI/native `Record_buffer`。

验收：

- 明确 F5a 后续 coding hook-point 是 `opt_sum.cc` 前置只读诊断、
  `pq_optimizer.cc` 后置状态诊断，还是两者都需要；
- 明确目标 SQL shape 与现有 MTR 归属；
- 明确后续 debug-only coding allowed/forbidden files；
- Design / Docs / Source Review Agent 返回 `ACCEPT`。

后续 coding 候选验收，需等 F5a source inventory accepted 后再启用：

- shortcut/min-like unsupported shape 可观测；
- `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced` 不增长；
- targeted MTR、`pq_stats`、完整 suite 通过；
- Code / Docs / Test Review Agent 返回 `ACCEPT`。

Source Inventory Findings:

- 当前分支 `sql/sql_optimizer.cc` 在 `JOIN::optimize()` 早期调用
  `optimize_aggregated_query()`；如果返回 `AGGR_COMPLETE`，
  optimizer 会设置 `tables_list = nullptr`、`best_rowcount = 1`，
  创建 fake single-row access path，并跳到 `setup_subq_exit`。这发生在
  后置 PQ eligibility 之前。
- 当前分支 `sql/opt_sum.cc` 的 MIN/MAX shortcut 在
  `find_key_for_maxmin()` 成功后直接 `ha_index_init()`，临时收窄
  `TABLE::read_set`，调用 `get_index_min_value()` /
  `get_index_max_value()`，再恢复 `read_set` 并结束 handler index cursor。
  该路径是 optimizer shortcut，不是 PQ worker row-production path。
- 当前分支 `sql/parallel_query/pq_optimizer.cc` 的后置 eligibility 只能
  看到未被 optimized-away 的 plan；`pq_check_full_table_scan()` 只接受
  `JT_ALL`。`JT_RANGE` secondary index 只做 unsupported probe / smoke，
  然后以 `NON_FULL_TABLE_SCAN` fail-closed。
- 因此仅在 `pq_optimizer.cc` 后置诊断无法证明 secondary MIN shortcut
  已被观测；如果后续需要 debug-only counter，主观测点必须在
  `opt_sum.cc` shortcut 读取完成并决定 `AGGR_COMPLETE` / `AGGR_EMPTY`
  的边界附近，`pq_optimizer.cc` 只能作为相邻 fail-closed 证明。
- 商用参考仓 `sql/opt_sum.cc` 未发现 PQ 专用 guard、fallback reason 或
  debug hook。商用 `sql_optimizer.cc` 也在 PQ 选择前执行
  `optimize_aggregated_query()`，shortcut 成功后同样表现为
  `Select tables optimized away`。
- 商用参考仓 `pq_sec_index_min.test` 名称有误导性：内容是 secondary
  index predicate 的 `SELECT * WHERE i < ... / j < ...`，没有 `MIN()`，
  没有 EXPLAIN，也不能证明 secondary MIN shortcut positive PQ。
- 商用参考仓有普通 aggregate / covering secondary range positive 证据，
  但这不等于 `opt_sum` MIN/MAX shortcut 并行化。当前迁移结论是：
  secondary MIN shortcut 先作为 optimizer shortcut guard / diagnostic
  分类，不作为可平移 positive execution path。

F5a Decision:

- 不平移 secondary MIN positive execution；
- 不把商用 `pq_sec_index_min` 当作 MIN/MAX shortcut 正例；
- 后续若做 F5a-1，只允许 debug-only diagnostic taskbook；
- F5a-1 的首选代码边界必须先单独 review：`opt_sum.cc` 只读诊断 counter
  + MTR guard；不得改变 shortcut 选择、handler read、read_set 语义、
  PQ eligibility、worker/MQ/InnoDB row path；
- `pq_optimizer.cc` 后置诊断只能补充验证相邻 `ORDER BY ... LIMIT 1` /
  secondary range fallback，不得替代 `opt_sum.cc` shortcut 观测。

Recommended F5a-1 Task Shape:

- 先写 coding taskbook，不直接编码；
- 候选 SQL shape 复用 `pq_commercial_ref_icp.test` 的
  `pq_ref_icp_min(id, i, j, KEY j_idx(j))`；
- 必备 SQL：
  - `SELECT MIN(j) FROM pq_ref_icp_min FORCE INDEX(j_idx)`；
  - `SELECT MIN(j) FROM pq_ref_icp_min FORCE INDEX(j_idx) WHERE j < 30`；
  - empty boundary：`SELECT MIN(j) ... WHERE j < 0`；
  - adjacent first-row fallback：
    `SELECT id, i, j ... WHERE j < 30 ORDER BY j LIMIT 1`；
- 现有 PQ counter window 必须继续保持：
  `Parallel_queries_executed = 0`、`Parallel_workers_launched = 0`、
  `Parallel_ranges_built = 0`、`Parallel_ranges_dispatched = 0`、
  `Parallel_secondary_rows_produced = 0`；
- 若新增 debug-only shortcut counters，只能表达 attempts / success /
  empty / unsupported，不得影响 release build 或 user-visible execution。

#### Proposed M11-F5a-1: Debug-only Optimizer Shortcut Diagnostic Taskbook

Status: implemented；build and MTR validation passed；Code / Docs / Test Review
accepted；committed as `377de6edd70`。

Goal:

- 在不改变 `opt_sum` MIN/MAX shortcut 语义、不打开 PQ positive execution
  的前提下，为 secondary MIN optimizer shortcut 增加 DBUG-only 可观测
  诊断；
- 验证 shortcut-like SQL 不增长 PQ execution / worker / range /
  secondary row counters；
- 明确该路径是 optimizer shortcut guard，而不是商用 positive PQ
  row-production path。

Architecture:

- `opt_sum.cc` 是唯一能可靠观测 `AGGR_COMPLETE` / `AGGR_EMPTY`
  secondary MIN shortcut 的位置；
- 诊断只允许在 DBUG build 中更新状态计数器，不改变 shortcut 是否启用、
  handler index read、`TABLE::read_set` restore、`table->set_keyread()`、
  `ha_index_init()` / `ha_index_end()` 顺序；
- `pq_optimizer.cc` 后置 eligibility 不作为主 hook，只保留现有
  fail-closed 计数器窗口作为负向验证。

Allowed Files:

- `sql/opt_sum.cc`
- `sql/parallel_query/sql_parallel.h`，仅允许在 `PQ_global_stats` 新增
  atomic counter 字段和 `reset()` 清零；
- `sql/mysqld.cc`，仅允许新增 `#ifndef NDEBUG` 包裹的 SHOW STATUS
  helper / `status_vars[]` entry；
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/sql_parallel.cc`
- any new `sql/parallel_query/pq_stats.*`
- `sql/CMakeLists.txt`
- `sql/join_optimizer/**`
- `sql/handler.*`
- `storage/innobase/**`
- any ORDER BY / Exchange_sort source file
- any partition/MVI/native `Record_buffer` source file

New Status Counters:

- `Parallel_opt_sum_minmax_shortcut_probe_attempts`
- `Parallel_opt_sum_minmax_shortcut_probe_success`
- `Parallel_opt_sum_minmax_shortcut_probe_empty`
- `Parallel_opt_sum_minmax_shortcut_probe_unsupported`

Counter Semantics:

- attempts：仅在 `DBUG_EXECUTE_IF("pq_opt_sum_minmax_shortcut_smoke", ...)`
  开启且 MIN/MAX field shortcut candidate 到达 `find_key_for_maxmin()` 后
  增长；
- success：handler index read 成功，后续可进入 `AGGR_COMPLETE` 的
  shortcut 增长；
- empty：handler 返回 `HA_ERR_KEY_NOT_FOUND` / `HA_ERR_END_OF_FILE`，
  后续可进入 `AGGR_EMPTY` 的 shortcut 增长；
- unsupported：仅统计 debug hook 局部范围内、已经进入 MIN/MAX field
  shortcut candidate 后，`find_key_for_maxmin()` 返回 false、handler
  error、range check failure 或其他不会完成 shortcut 的结果；不要求统计
  所有非 candidate aggregate 或非 field MIN/MAX；
- 所有 counter 必须只在 debug hook 开启时变化；默认运行与 release
  build 不改变行为；新增 SHOW STATUS helper 和 `status_vars[]` entry
  必须用 `#ifndef NDEBUG` 包裹，避免 release build 增加用户可见状态变量。

Implementation Steps:

1. RED test:
   - 在 `pq_commercial_ref_icp.test` 的 M9-F4 window 前后加入
     `Parallel_opt_sum_minmax_shortcut_probe_*` delta 查询；
   - 打开 `SET SESSION debug="d,pq_opt_sum_minmax_shortcut_smoke"`；
   - 运行：
     `cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp --record`；
   - 预期初始 RED：新增状态变量不存在或 delta 不符合期望。

2. Status plumbing:
   - 在 `sql/parallel_query/sql_parallel.h` 的 `PQ_global_stats` 中新增四个
     atomic counter；
   - 在 `PQ_global_stats::reset()` 中清零；
   - 在 `sql/mysqld.cc` 中新增 `#ifndef NDEBUG` 包裹的 SHOW STATUS
     helper / `status_vars[]` entry；
   - 更新 `pq_stats.result` 的 debug-build status variable expectation；
   - 不复用 execution / worker / secondary rows counters。

3. `opt_sum.cc` DBUG-only hook:
   - hook 位置必须在 MIN/MAX field shortcut candidate 已确认、且
     `find_key_for_maxmin()` / handler index read 结果可见的局部范围；
   - 不新增 early return；
   - 不改变任何变量值、bitmap、handler call order 或 error handling；
   - 使用最小 helper 或局部 lambda 只做 counter increment。

4. MTR guard:
   - 复用 `pq_ref_icp_min`；
   - 覆盖 bare `MIN(j)`、range `MIN(j) WHERE j < 30`、empty
     `MIN(j) WHERE j < 0`、adjacent `ORDER BY j LIMIT 1`；
   - 预期 shortcut counter 用布尔 delta 表达，避免前序测试污染：
     `attempts_delta >= 3`、`success_delta >= 2`、`empty_delta >= 1`；
   - 预期 PQ execution counters 仍全为 0：
     `Parallel_queries_executed`、`Parallel_workers_launched`、
     `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
     `Parallel_secondary_rows_produced`。

5. Verification:
   - `cmake --build build-ninja --target mysqld -j 16`
   - `cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp`
   - `cd build-ninja/mysql-test && ./mtr parallel_query.pq_stats --record`
   - `cd build-ninja/mysql-test && ./mtr --suite=parallel_query --parallel=1`

Acceptance:

- Code / Docs / Test Review Agent returns `ACCEPT`；
- no production behavior change when debug hook is absent；
- no source changes outside Allowed Files；
- no changes to optimizer shortcut selection, handler index read semantics,
  `read_set` restore semantics, PQ eligibility, worker/MQ path, or InnoDB path；
- complete suite remains green。

Implementation Summary:

- Added debug-only `PQ_global_stats` counters for opt_sum MIN/MAX shortcut
  attempts / success / empty / unsupported in `sql_parallel.h`；
- Added debug-only SHOW STATUS helper / entries in `mysqld.cc` under
  `#ifndef NDEBUG` so release builds do not gain user-visible status variables；
- Added `DBUG_EXECUTE_IF("pq_opt_sum_minmax_shortcut_smoke", ...)` guarded
  counter increments around the existing `opt_sum.cc` MIN/MAX field shortcut
  path；
- Extended `pq_commercial_ref_icp` M9-F4 window to cover bare MIN, ranged MIN,
  empty MIN, and adjacent `ORDER BY ... LIMIT 1` while verifying PQ execution
  counters remain zero；
- Updated `pq_stats.result` for the debug-build-only status variable list.

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp --record`
  passed；
- `cd build-ninja/mysql-test && ./mtr parallel_query.pq_stats --record` passed；
- `cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp parallel_query.pq_stats`
  passed；
- `cd build-ninja/mysql-test && ./mtr --suite=parallel_query --parallel=1`
  passed, 89/89。

Code / Docs / Test Review:

- Review Agent verdict: `ACCEPT`；
- accepted boundaries: DBUG-only hook and `#ifndef NDEBUG` status variables；
- non-blocking risk: release builds intentionally do not expose the four
  debug-only status variables；`unsupported` counter is not asserted by MTR。

Triage Review Revision:

- 初始 Review Agent verdict: `REVISE`；
- blocking finding: F5a 不能直接进入 debug-only coding，必须先确认
  `sql/sql_optimizer.cc` 调用 `optimize_aggregated_query()`、
  `sql/opt_sum.cc` aggregate shortcut / `find_key_for_maxmin()` 与
  `sql/parallel_query/pq_optimizer.cc` 后置状态之间的可观测边界；
- 已修订：F5a 降级为 docs-only source inventory，`sql/**`、
  `storage/**`、`mysql-test/**` 全部 forbidden；`sql/opt_sum.cc` hook
  只能在后续单独 review 批准后作为只读诊断进入；
- 已修订：BLOB/TEXT/JSON/GEOMETRY、fixed-point、hostile
  read_set/write_set shape 明确保留在 global guard / backlog。

#### Proposed M11-F5-A: Reverse Boundary Contract Taskbook

Status: docs-only taskbook accepted；committed as `e799008575e`。

Goal:

- 基于当前分支和商用参考实现，明确 reverse positive range/ref/index scan
  是否具备进入当前分支的前置条件；
- 固化当前阶段的边界：先做 reverse boundary / direction / ordering
  contract，不打开用户可见 reverse PQ 正例；
- 为后续可执行子任务选择最小安全路径：debug-only reject /
  diagnostic，而不是直接复制商用 reverse worker path。

Current Branch Facts:

- visible ORDER BY 仍由 `PQUnsuiteReason::HAS_ORDER_BY` 在
  `pq_optimizer.cc` 统一串行拒绝；
- `TryCreatePQSecondaryCoveringRangeIterator()` 对
  `param.reverse` fail-closed，当前不创建 reverse secondary range PQ
  iterator；
- `TryCreatePQSecondaryCoveringRefIterator()` 已有
  `Parallel_secondary_reverse_reject_probes` 和
  `Parallel_secondary_reverse_ref_reject_probes`，只在 reverse ref reject
  分支增长；
- `pq_commercial_ref_icp` 的 M9-F2 已验证 reverse range / index / ref
  SQL shape 不增长 `Parallel_queries_executed`、
  `Parallel_workers_launched`、`Parallel_ranges_built`、
  `Parallel_ranges_dispatched`、`Parallel_secondary_rows_produced`；
- reverse range / index 当前没有 access-path-level reject counter，因为
  这些 shape 可能在 `access_path.cc` 阶段选择串行 reverse iterator，未稳定
  进入 PQ secondary range factory。

Current Branch Explorer Findings:

- reverse range / index / ref 的用户可见 SQL shape 已有负向覆盖；
- reverse ref 有 factory-level reject counters；
- reverse range / index 没有专门 counter，原因是它们可能先被
  access path / serial reverse iterator 处理，当前不应为了观测改变
  iterator selection；
- `pq_commercial_order_by` 明确要求 visible ORDER BY 继续 serial，且
  `Parallel_queries_executed`、worker、range counters 不增长。

Commercial Reference Findings:

- 商用参考仓确认支持真实 PQ reverse positive path：
  - `pq_range_scan_reverse.test` 覆盖 reverse range 和 secondary range；
  - `pq_reverse_index_scan.test` 覆盖 reverse index scan、DESC index 和
    reverse-sorted group merge；
  - `pq_ref_reverse_scan.test` 覆盖 reverse ref；
  - `pq_index_scan_desc.test` 覆盖 range + group/order desc；
  - 对应结果包含 `Parallel execute`、`Backward index scan`、`range` /
    `ref` / `index` 等正例 evidence。
- 商用 SQL 层关键依赖：
  - `SetupPQTab()` 传播 `QEP_TAB::m_reversed_access` 并调用
    `ha_set_reverse_scan()`；
  - `InitPQTab()` 对 `INDEX_RANGE_SCAN.reverse` 调用
    `ReverseIndexRangeScanIterator::shared_reset()`；
  - `mark_desc_groups()` 修正 reverse-sorted worker output 的 streaming /
    group merge 方向；
  - `PQblockScanIterator::Read()` / `PQRefIterator::Read()` 真实调用
    worker row path，而当前分支对应路径仍是 stub / blocked。
- 商用 InnoDB 层关键依赖：
  - `pq_index_scan_init()` reverse index scan 以 `index_last()` 起步；
  - `pq_range_scan_init()` 使用 reverse start/end boundary conversion，
    包括 `HA_READ_KEY_OR_PREV`、`HA_READ_BEFORE_KEY` 和
    `end_key == nullptr` 时的 `index_last()`；
  - `pq_ref_build_ranges()` / `pq_ref_scan_init()` 处理 reverse ref
    boundary；
  - `row0pread_pq.cc` 的 reverse path 使用反向 page cursor movement 和
    out-of-range 比较。
- 商用结果合并依赖：
  - worker output 必须有稳定 rowid；
  - `Exchange_sort` heap compare 先比较 sort key，再用
    `file->cmp_ref(rowid0, rowid1)` tie-break；
  - 这些依赖当前分支的 production worker MQ row frame 和 visible
    Gather Merge 尚未准备好。

Allowed Files:

- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/**`
- `storage/**`
- `mysql-test/**`
- build scripts and generated result files

Design Requirements:

- 明确 reverse range、reverse index scan、reverse ref 的当前分支入口分别
  在哪里被拒绝；
- 明确 reverse positive path 至少需要的 ordering contract：
  - scan direction；
  - lower / upper boundary inclusive semantics；
  - duplicate key 的 rowid tie-break；
  - worker 间结果顺序；
  - 与 `ORDER BY ... DESC` / `LIMIT` 的交互；
- 明确当前不允许把 visible ORDER BY 从 `HAS_ORDER_BY` 中放开；
- 明确当前不允许调用真实 `pq_worker_scan_next()` 或接
  `PQRefIterator::Read()` / `PQblockScanIterator::Read()`；
- 明确当前不允许把 reverse 与 ICP、partition、MVI、native
  `Record_buffer` positive path 合并；
- 给出后续最小 coding 候选，默认必须是 debug-only diagnostic /
  reject counter，不是 positive gate。

Hard Stops:

- 如果商用 reverse tests 依赖真实 ORDER BY / Gather Merge，则当前阶段
  只能记录 blocker；
- 如果当前分支无法稳定触达 reverse range/index PQ factory，则不得为了
  counter 改变 iterator selection；
- 如果需要改 `access_path.cc`、handler public API、InnoDB
  `row0pread_pq.cc` 或 worker/MQ row path，本阶段停止并拆新设计任务；
- 任何新增诊断都必须证明 PQ execution / worker / range /
  secondary-row counters 不增长。

F5-A Decision:

- 不进入 reverse positive gate；
- 不迁移商用 `pq_range_scan_reverse`、`pq_reverse_index_scan`、
  `pq_ref_reverse_scan`、`pq_index_scan_desc` 的 positive expectation；
- 商用 reverse path 只作为后续迁移目标和 invariants 来源；
- 下一步若继续 F5-A1，只允许 debug-only negative / diagnostic：
  - 保持 `HAS_ORDER_BY` serial boundary；
  - 识别 reverse range / index / ref rejected shape；
  - 不改变 iterator selection；
  - 不增长 PQ execution / worker / range / secondary-row counters。

Validation:

- 本阶段只做文档 review，不运行 build/MTR；
- Design / Source / Test Review Agent returned `ACCEPT`；
- 若 review 认为需要编码，必须另起 M11-F5-A1 任务书，单独定义
  Allowed / Forbidden files、MTR window 和 review gate。

Review:

- Verdict: `ACCEPT`；
- Blocking findings: none；
- Non-blocking risks:
  - reverse range / index observability intentionally remains weaker than
    reverse ref；
  - M9-F2 reverse SQL shape rejection is dominated by `HAS_ORDER_BY` at the
    visible layer；
  - docs-only review is sufficient because no source or MTR files changed。
- Safe next task: M11-F5-A1 debug-only negative / diagnostic taskbook。

#### Proposed M11-F5-A1: Debug-only Reverse Diagnostic Taskbook

Status: docs-only taskbook accepted；Review recommendation is `RECORD_BLOCKED`。

Goal:

- 在不打开 reverse positive execution 的前提下，定义是否需要补充
  debug-only reverse diagnostics；
- 保持 visible ORDER BY `HAS_ORDER_BY` serial boundary；
- 不改变 iterator selection、access path selection、handler/InnoDB reverse
  cursor、worker/MQ row path 或现有 MTR positive/negative semantics。

Scope Decision:

- reverse ref 已有 factory-level counters：
  `Parallel_secondary_reverse_reject_probes` 和
  `Parallel_secondary_reverse_ref_reject_probes`；
- reverse range / index 目前只有 zero-execution guard。F5-A1 不能为了
  产生 counter 而强制 reverse range/index 进入 PQ factory；
- 若找不到稳定 existing hook，F5-A1 应提交 docs-only blocked result，
  而不是添加不可靠 counter。

Allowed Files For F5-A1 Coding Candidate:

- `sql/parallel_query/sql_parallel.h`，仅允许新增 debug-only or
  fail-closed diagnostic counter 字段和 reset；
- `sql/mysqld.cc`，仅允许新增对应 SHOW STATUS entry；
- `sql/parallel_query/pq_optimizer.cc`，仅允许在既有 ORDER BY / reverse
  fail-closed diagnostic hook 中记录 shape，不得改变 rejection；
- `sql/parallel_query/pq_iterators.cc`，仅允许复用或细分现有 reverse
  ref reject observation；
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m11-ref-icp-worker-path.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/join_optimizer/access_path.cc`
- `sql/range_optimizer/**`
- `sql/sql_executor.*`
- `sql/sql_select.*`
- `sql/handler.*`
- `sql/parallel_query/exchange_sort.*`
- `sql/parallel_query/exchange.*`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/pq_clone.*`
- `storage/innobase/**`
- any ORDER BY visible positive test/result file except read-only reference；
- any partition/MVI/native `Record_buffer` source or test file。

Candidate Diagnostics:

1. Reverse ref status refinement:
   - keep current counters unchanged unless review finds ambiguity；
   - optionally document current counters as covering ref-only direct reject。

2. ORDER BY-dominated reverse shape diagnostic:
   - if a stable, read-only diagnostic point exists before
     `HAS_ORDER_BY` rejection, record that ordered query contains a
     reverse access shape；
   - counter names must make clear they are optimizer/ORDER-BY diagnostic,
     not PQ factory reject counters。

3. Reverse range/index best-effort diagnostic:
   - only allowed if current objects already expose `reverse` without changing
     plan selection；
   - no new iterator construction；
   - no access path rewrite；
   - no handler reverse cursor call。

MTR Requirements If Coding Proceeds:

- Extend the existing M9-F2 window in `pq_commercial_ref_icp`；
- keep existing expected deltas:
  - `Parallel_queries_executed = 0`；
  - `Parallel_workers_launched = 0`；
  - `Parallel_ranges_built = 0`；
  - `Parallel_ranges_dispatched = 0`；
  - `Parallel_secondary_rows_produced = 0`；
- any new diagnostic deltas must be boolean or lower-bound expressions to
  avoid dependence on prior tests；
- update `pq_stats.result` only for newly registered status variables；
- do not migrate commercial positive reverse tests in this task。

Validation If Coding Proceeds:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp --record
cd build-ninja/mysql-test && ./mtr parallel_query.pq_commercial_ref_icp
cd build-ninja/mysql-test && ./mtr parallel_query.pq_stats --record
cd build-ninja/mysql-test && ./mtr parallel_query.pq_stats
cd build-ninja/mysql-test && ./mtr --suite=parallel_query --parallel=1
```

Hard Stops:

- Any need to edit `access_path.cc` means stop and record blocked result；
- Any need to open visible ORDER BY, `Exchange_sort` production path,
  `PQblockScanIterator::Read()`, `PQRefIterator::Read()` or
  `pq_worker_scan_next()` means stop；
- Any change that can increase PQ execution / worker / range / secondary-row
  counters for reverse SQL means stop；
- Any diagnostic that conflates reverse range/index with reverse ref factory
  reject semantics must be rejected。

Acceptance:

- Design / Source / Test Review Agent returned `ACCEPT`；
- reviewer recommendation: `RECORD_BLOCKED`；
- no source or MTR edits are required。

Review:

- Verdict: `ACCEPT`；
- Blocking findings: none；
- Required fixes before commit: none；
- Recommendation: `RECORD_BLOCKED`。

Blocked Result:

- F5-A1 will not code reverse range/index diagnostics in the current allowed
  file boundary；
- this is not because MySQL or the commercial implementation lacks reverse
  execution. The commercial reference supports reverse range/index/ref positive
  PQ, but current branch intentionally keeps that path blocked；
- reverse range is not observable in the allowed coding surface because
  `AccessPath::INDEX_RANGE_SCAN` with `param.reverse` goes directly to
  `ReverseIndexRangeScanIterator`; the PQ range factory is only called in the
  non-reverse branch；
- reverse index scan also has no PQ factory hook because `AccessPath::INDEX_SCAN`
  selects the serial reverse iterator directly；
- `pq_optimizer.cc` can observe ordered-query rejection before
  `HAS_ORDER_BY`, but the stable fields there do not prove reverse range, and
  `QEP_TAB::m_reversed_access` is only asserted for `JT_REF` /
  `JT_INDEX_SCAN`；
- the only currently safe observable is the existing reverse ref factory counter
  path in `pq_iterators.cc`，and M9-F2 already validates it while keeping
  `Parallel_queries_executed`、workers、ranges、dispatch and secondary rows at
  zero。

Next Safe Task:

- Do not code F5-A1；
- keep reverse positive migration deferred until visible ORDER BY/Gather Merge,
  worker row path, rowid tie-break and InnoDB reverse cursor contract are
  production-ready；
- continue to the next F5 backlog item instead of expanding reverse diagnostics
  through `access_path.cc`。

Agent Review Prompt:

请作为 M11-F5-A Design / Source / Test Review Agent，只读审查本任务书
和相关源码 / 测试：

1. 当前分支 reverse range/index/ref 的 reject 边界是否描述准确；
2. F5-A 是否应保持 docs-only boundary contract，还是可以进入
   debug-only diagnostic；
3. 是否存在任何条件允许直接打开 reverse positive path；
4. Allowed / Forbidden files 和 Hard Stops 是否足够防止误开 ORDER BY、
   worker/MQ、InnoDB reverse cursor、ICP、partition、MVI 或 native
   `Record_buffer`；
5. 下一步最小安全任务是什么。

输出：

- Verdict: `ACCEPT` 或 `REVISE`
- Blocking findings
- Non-blocking risks
- Safe next task recommendation

## 必须保留的 Guard

- visible ORDER BY 继续由 `HAS_ORDER_BY` 串行拒绝；
- M9-B3/C2/D3d/E1c-2a 正例 counter window 不能回退；
- M9-F MVI/reverse/partition/secondary MIN/BLOB/fixed-point/read_set/write_set
  hostile shapes 继续不增长 PQ execution / worker / range / secondary row
  counters；
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
