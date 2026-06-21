# M11 Commercial Main Architecture Restart Taskbook

## 状态

M11-A0-A5 plan clone / resolver contract completed。
M11-B0-B3 worker result / `Query_result_mq` adapter and guarded wiring
completed。
M11-D0-D6 `ParallelScanIterator` lifecycle contract, guarded construction
probe, worker attach smoke, leader row stream smoke, post-commit ERROR cleanup
smoke, row-value correctness smoke, and debug-only commercial
`ParallelScanIterator` row-value positive path completed coding/validation；
D6 Code-Docs-Test Review returned `ACCEPT` and commit `ec873cffc8` was
created。

## 目标

M10 已完成当前 `parallel_query` suite 与商用测试 manifest 的收口。
M11 开始重新回到商用主架构迁移，但不直接把完整商用代码整块搬入。
本阶段目标是把 post-M10 的主链路拆成可验证、可回退、可 review 的小步：

- M11-A: plan clone / resolver contract；
- M11-B: worker result / `Query_result_mq` leader adapter；
- M11-C: guarded worker-result wiring probe；
- M11-D: `ParallelScanIterator` lifecycle contract；
- M11-E: ORDER BY / `Exchange_sort` real path gate；
- M11-F: ref / ICP worker path continuation。

## 背景

当前分支已经有很多商用命名文件和 class boundary，但多数仍是
fail-closed 或 smoke-only：

- `pq_make_join()` 返回 `nullptr`；
- `pq_clone_activation_probe()` 只记录 attempt/fallback/unsupported；
- `ParallelScanIterator` / `PQblockScanIterator` / `PQRefIterator`
  仍 fail-closed；
- `Query_result_mq::send_data()` 已有 controlled smoke，但未接真实
  worker JOIN；
- `Exchange_nosort` 仍主要消费当前 typed row-image，不消费 `PQWR`
  worker-result row frame；
- `Exchange_sort` 只有 synthetic smoke，真实 ORDER BY 仍 fallback；
- ref / ICP 当前是 leader-local gate，不是商用 worker-side path。

## M11 串行/并行规则

必须串行：

- plan clone contract 与 worker execution 接线；
- `Query_result_mq` 真实 worker result path 与 `ParallelScanIterator`
  lifecycle；
- ORDER BY real path 与 worker result path；
- ref / ICP worker path 与 handler/InnoDB secondary path。

可并行：

- M11-A0 / M11-B0 design-only；
- test manifest / deferred backlog 更新；
- read-only explorer 对商用仓与当前仓差异分析；
- review agents。

## 阶段拆分

### M11-A: Plan Clone / Resolver Contract

先做 design-only A0，再做 compile-only skeleton。A 系列不得启动 worker、
不得调用 handler/InnoDB、不得让 `pq_make_join()` 生成可执行 worker plan。

推荐拆分：

- M11-A0: design-only contract map；
- M11-A1: `Item` base contract compile-only skeleton；
- M11-A2: `Query_block` / `JOIN` clone-link compile-only skeleton；
- M11-A3: resolver/base-item helper compile-only subset；
- M11-A4: clone contract preflight probe；
- M11-A5: clone preflight MTR smoke。

任务书：[commercial-port-m11-plan-clone-resolver.md](commercial-port-m11-plan-clone-resolver.md)

### M11-B: Worker Result / Query_result_mq Adapter

先定义当前 `PQWR` worker-result frame 与商用 `Field_raw_data` 的关系，
再做 leader decode adapter 和 synthetic smoke。B1/B2 不接真实 cloned
JOIN，不启动 worker，不改 AccessPath。

推荐拆分：

- M11-B0: design-only worker result adapter contract；
- M11-B1: `PQWR` leader decode adapter compile-only skeleton；
- M11-B2: synthetic adapter MTR smoke；
- M11-B3: guarded worker-result wiring probe。

任务书：[commercial-port-m11-worker-result-path.md](commercial-port-m11-worker-result-path.md)

### M11-C: Guarded Worker Result Wiring Probe

只在 M11-B1/B2 通过后启动。目标是在 debug/smoke gate 下，把受控 worker
callback 的 output 改走 `Query_result_mq`，leader 通过 adapter 消费。
仍不接 commercial cloned JOIN。

### M11-D: ParallelScanIterator Lifecycle Contract

已完成 D0-D6：先建立 owner/cleanup/fallback/commit-point 合同，再用
debug-only construction probe 验证 `ParallelScanIterator::Init()` 当前保持
fail-closed 且 cleanup 可观测；随后完成 worker attach、leader row stream
post-commit ERROR cleanup、row-value correctness 护栏，并在 D6 通过
DBUG-only bridge 让 SQL executor 经 `PQTableScanIterator::Read()` 委托到
`ParallelScanIterator::Read()` 返回固定整数行值。D6 仍保持 debug-only、
DOP=1、无 worker thread、无 clone/JOIN、无默认 AccessPath 行为变化。

### M11-E: ORDER BY Gather Merge Real Path Gate

依赖 B/D。当前 `Exchange_sort` 只有 synthetic smoke，真实 ORDER BY 仍
`HAS_ORDER_BY` fallback。E 不得早于 worker result adapter。E0 已完成
design-only taskbook；E1 已完成 `Exchange_sort` commercial shape compile-only
和 ORDER BY negative boundary hardening，未改变默认 SQL 行为。下一步进入
E2 sorted row-frame adapter smoke 设计。

### M11-F: Ref / ICP Worker Path Continuation

依赖 A/B/D。当前 ref / ICP 是 leader-local/no-worker/no-MQ gate。
F 负责继续评估 `PQRefIterator`、`ha_pq_next`、secondary ICP worker path。
只读 Explorer 已确认不建议立即编码：当前仓使用 typed worker context，
`pq_worker_scan_next()` 仍 intentionally unsupported，M9 ref/ICP 是
leader-local gate。F 后续需要单独 taskbook/review，不与 E 混合。

## 验收边界

- 每个子阶段完成后必须启动独立 review Agent；
- review 返回 `ACCEPT` 后才允许提交；
- 源码阶段必须至少跑 `mysqld` build 和相关 targeted MTR；
- 任何 worker-start 后失败不得 silent serial fallback；
- 默认 OFF / explicit gate 保护必须保留；
- 不在单个 commit 混合 plan clone、worker result、ORDER BY、ref/ICP。

## 当前推荐下一步

1. 提交 M11-E1 compile-only shape；
2. 生成并 review M11-E2 sorted row-frame adapter smoke taskbook；
3. M11-F ref/ICP worker path 继续保持独立 taskbook，避免与 E2 混合。

## Review

Docs-Design Review Agent first pass returned `REVISE` because older
`commercial-port-gap-analysis.md` text still described `query_result_mq.*`,
`pq_iterators.*`, `pq_clone.*`, and related files as missing. The text was
updated to distinguish current skeleton/smoke-only files from missing complete
commercial capabilities. Final review returned `ACCEPT`.
