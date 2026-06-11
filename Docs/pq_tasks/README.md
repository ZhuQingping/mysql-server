# Parallel Query V1 Progress Board

本文件是 PQ V1 的总进度看板。各阶段详细说明保存在对应 phase 文档中。

## Current Summary

- Last synced: 2026-06-11
- Current phase: Phase 0-9 已提交完成；PQ V1 风险收敛、V2-0、V2-1、V2-2、V2-3、V2-4、V2-5、V2-6、V2-7 已提交；V2-8A worker handler/prebuilt contract design 已完成；V2-8B Row Image Protocol 已完成；V2-8C contract/gate 第一段、leader probe/execute mode API、worker open context carrier 生命周期、worker THD/TABLE helper、safe-window open-table smoke 和 handler init/end smoke 已实现；V2-8D 已完成 first-row/read-view 方案选型；V2-8E EXECUTE commit point primitive 已完成；V2-8F callback conversion smoke primitive 已完成；V2-8G EXECUTE callback smoke observability 已完成；V2-8H row stream activation boundary 已完成；V2-8I iterator runtime state contract 已完成；V2-8J-1 callback row producer smoke 已完成；V2-8J-2 worker producer FINISH/EOF、ERROR 和 abort skeleton 已完成；V2-8J-3 `Read()` shadow path scaffold 已完成，真实 DOP=1 full scan 仍未打开。
- Latest commits:
  - Phase 9: `9c7e9aede42` Add PQ phase 9 test suite migration
  - V1 risk convergence: `69ed0ac66e7` Tighten PQ V1 risk boundaries
  - V2-2: `a123cdabe7c` Add PQ V2-2 handler context bridge
  - V2-3: `54bd78429cf` Tighten PQ V2-3 read view boundary
  - V2-4: `0b7f7485a7b` Add PQ V2-4 range planning export
  - V2-4 observable: `79d1355cf2f` Add PQ V2-4 range planning status
  - V2-5: `1e8078544e8` Add PQ V2-5 worker lifecycle smoke
  - V2-6: `d0b4988a584` Add PQ V2-6 exchange row stream smoke
  - V2-7: `1d39b510348` Add PQ V2-7 predicate projection boundary
  - V2-8A dispatch: `25f216d9a9d` Dispatch PQ V2-8A contract design
  - V2-8A range findings: `332361020f4` Record PQ V2-8A range dispatch findings
  - V2-8A complete: `8dc30b6a1d2` Complete PQ V2-8A contract design
  - V2-8B: `db14645bcf4` Add PQ V2-8B row image protocol
  - V2-8C plan: `75dc2e297db` Plan PQ V2-8C DOP1 real full scan
  - V2-8C contract/gate: `24209d09ce3` Add PQ V2-8C worker context gates
  - V2-8C leader mode: `6638e36def7` Add PQ V2-8C leader scan mode contract
  - V2-8C worker open carrier: `36b2d9a764c` Add PQ V2-8C worker open context carrier
  - V2-8C worker table helper: `09033e1c2a0` Add PQ V2-8C worker table open helper
  - V2-8C worker THD helper: `9c242c61cc7` Add PQ V2-8C worker THD lifecycle helper
  - V2-8C worker open smoke: `2dab52a2d9c` Add PQ V2-8C worker open table smoke helper
  - V2-8C worker open smoke run: `a79c2109d7a` Run PQ V2-8C worker open table smoke
  - V2-8C worker handler smoke: `13ce85e4d47` Run PQ V2-8C worker handler init smoke
  - V2-8C status: `cb34ece63ef` Record PQ V2-8C worker handler smoke status
  - V2-8D first row contract: `1d39beafe15` Tighten PQ V2-8D first row contract
  - V2-8D read-view strategy: `50f7ed3dbd5` Select PQ V2-8D read view strategy
  - V2-8E execute commit point: `657cc94eb9a` Add PQ V2-8E execute commit point
  - V2-8F pull adapter plan: `d5c004c9461` Plan PQ V2-8F pull adapter
  - V2-8F pull adapter gates: `e73c73f053f` Add PQ V2-8F pull adapter gates
  - V2-8F visibility helper: `379e3c60581` Extract PQ V2-8F visibility helper
  - V2-8F conversion helper: `62f8058375d` Add PQ V2-8F record conversion helper
  - V2-8F callback accessors: `e817a9176ec` Add PQ V2-8F callback row accessors
  - V2-8F callback conversion helper: `f23320fb5f7` Add PQ V2-8F callback conversion helper
  - V2-8F callback conversion smoke: `816a6808699` Add PQ V2-8F callback conversion smoke
  - V2-8G EXECUTE callback smoke: `03fb7eb4bf8` Add PQ V2-8G execute callback smoke
  - V2-8H row stream activation boundary: `ccc187e271c` Add PQ V2-8H row stream boundary helper
  - V2-8I iterator runtime state contract: `eb0cd14e659` Add PQ V2-8I iterator runtime state
  - V2-8J real read activation plan: planned，真实 `Read()` 打开前硬阻塞拆分
  - V2-8J-1 callback row conversion gate: 本轮提交，`Parallel_callback_smoke_rows >= 1` 稳定通过
  - V2-8J-1 typed MQ producer smoke: 本轮提交，callback-converted worker record image 经 MQ 被 leader materialize
  - V2-8J-2 worker producer ERROR smoke: 本轮提交，typed ERROR 可被 leader 观察并由 smoke 内部消费
  - V2-8J-2 worker producer abort smoke: 本轮提交，leader abort 可 detach MQ 并由 leader 观察 EOF
  - V2-8J-3 Read shadow path scaffold: 本轮提交，debug-only gate，默认不可达
  - V2-1: `9a58ff94cdf` Add PQ V2-1 iterator safe fallback
  - V2-0: `a420e8a3f26` Add PQ V2-0 execution state contract
  - Phase 8: `113d2ba44c1` Add PQ phase 8 V1 completion scaffolding
  - Phase 7: `9af8fe777d2` Add PQ phase 7 EXPLAIN and MVP tests
- Phase 8 validation:
  - `cmake --build build-ninja --target mysqld -j 16` 通过
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help` 通过
  - `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt` 通过，完整 `parallel_query` suite 共 10 个测试通过
- Phase 9 validation:
  - `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt` 通过，实际 `parallel_query` 测试 12 个，加 `shutdown_report` 共 13 项通过
- Current V1 capability boundary: 当前分支具备 PQ 系统变量、保守 eligibility、EXPLAIN 注解、fallback/status 统计、SQL/InnoDB/worker/MQ 框架和 V1 测试闭环；真实并行执行仍未启用，eligible 查询最终仍保守走串行路径。
- Active orchestration:
  - [v1-risk-convergence.md](v1-risk-convergence.md): V1 风险收敛已提交，覆盖 locking read、fallback counter、PQUnsuiteInfo 生命周期、best_ref/qep_tab 访问安全和 V1 测试缺口。
  - [v2-0-execution-state-contract.md](v2-0-execution-state-contract.md): V2-0 已实现，定义 PQ execution state contract，供 V2-1 iterator ownership 使用。
  - [v2-1-iterator-safe-fallback.md](v2-1-iterator-safe-fallback.md): V2-1 已完成验证，建立 PQ iterator ownership 和 Init 安全回退。
  - [v2-2-handler-innodb-context-bridge.md](v2-2-handler-innodb-context-bridge.md): V2-2 已提交，补齐 handler virtual API 与 InnoDB leader context bridge。
  - [v2-3-read-view-trx-contract.md](v2-3-read-view-trx-contract.md): V2-3 已提交，收敛 InnoDB read view / trx / row_prebuilt 边界。
  - [v2-4-parallel-reader-range-partition.md](v2-4-parallel-reader-range-partition.md): V2-4 已提交，完成 Parallel_reader thin adapter、range planning observable 和 DOP>1 range gate。
  - [v2-5-worker-thd-minimal-scan.md](v2-5-worker-thd-minimal-scan.md): V2-5 已提交，完成 worker lifecycle smoke observable，真实 row stream 仍未打开。
  - [v2-6-exchange-gather-row-stream.md](v2-6-exchange-gather-row-stream.md): V2-6 已完成验证，先用 synthetic MQ token/row stream 验证 Exchange/Gather，不接真实 InnoDB row。
  - [v2-7-predicate-projection-boundary.md](v2-7-predicate-projection-boundary.md): V2-7 已提交，锁定 leader-side predicate/projection contract，避免提前引入 worker-side Item/JOIN clone。
  - [v2-8-single-table-fullscan-closure.md](v2-8-single-table-fullscan-closure.md): V2-8 当前任务书，准备真实单表 clustered full scan 闭环。
  - [v2-8a-worker-handler-prebuilt-contract.md](v2-8a-worker-handler-prebuilt-contract.md): V2-8A 已完成，确认 worker 独立 TABLE/handler/prebuilt、leader-pinned read view、typed worker wrapper、DOP=1 first gate。
  - [v2-8b-row-image-protocol.md](v2-8b-row-image-protocol.md): V2-8B 已完成，定义 typed MQ row message、fixed record image copy 和 synthetic materialization smoke。
  - [v2-8c-dop1-real-fullscan.md](v2-8c-dop1-real-fullscan.md): V2-8C 已完成两个只读确认、contract/gate 第一段、leader probe/execute mode API、worker open context carrier 生命周期、worker THD/TABLE helper 边界、safe-window open-table smoke 和 handler init/end smoke；已落地 worker open context、typed InnoDB worker wrapper、DOP=1/blob/independent TABLE/record gate，真实 row scan 仍未打开。
  - [v2-8d-first-row-read-view-contract.md](v2-8d-first-row-read-view-contract.md): V2-8D 已完成方案选型：首行定位按 `index_first()` 等价协议，真实读取走 `Parallel_reader` visibility adapter 路线，不直连 worker-local `row_search_mvcc()`；`pq_worker_scan_next()` 继续 disabled，直到 EXECUTE commit point 和 pull adapter 完成。
  - [v2-8e-execute-commit-point.md](v2-8e-execute-commit-point.md): V2-8E 已完成 InnoDB leader `EXECUTE` commit point primitive；`PROBE` 仍 fallback-safe，`EXECUTE` 在 leader 线程绑定 active read view，但当前 SQL iterator 尚不调用 EXECUTE；build 和完整 `parallel_query` suite 通过。
  - [v2-8f-parallel-reader-pull-adapter.md](v2-8f-parallel-reader-pull-adapter.md): V2-8F callback conversion smoke primitive 已完成，`InnoDB_pq_scan_ctx` 可用 `Parallel_reader(0)` 在当前线程转换最多一行 visible clustered record；不接 SQL `Read()`，不设置 EXECUTED；build 和完整 `parallel_query` suite 通过。
  - [v2-8g-execute-callback-smoke.md](v2-8g-execute-callback-smoke.md): V2-8G 已完成，SQL iterator 在 safe fallback window 内尝试固定 DOP=1 EXECUTE callback smoke；新增 `Parallel_callback_smoke_attempts` / `Parallel_callback_smoke_rows`；失败不影响 serial fallback；完整 `parallel_query` suite 通过。
  - [v2-8h-row-stream-activation.md](v2-8h-row-stream-activation.md): V2-8H 已完成，抽出 `Exchange_nosort::materialize_next_record_image()`，不接真实 `Read()`；完整 `parallel_query` suite 通过。
  - [v2-8i-iterator-runtime-state.md](v2-8i-iterator-runtime-state.md): V2-8I 已完成，增加 iterator runtime state contract，不打开真实 `Read()`；完整 `parallel_query` suite 通过。
  - [v2-8j-real-read-activation-plan.md](v2-8j-real-read-activation-plan.md): V2-8J 已拆分真实 `Read()` 激活前的硬阻塞；V2-8J-1 callback row producer smoke、V2-8J-2 FINISH/EOF/ERROR/abort skeleton 和 V2-8J-3 `Read()` shadow scaffold 已完成，下一步进入 DOP=1 real full scan gate。
  - [v2-execution-path-roadmap.md](v2-execution-path-roadmap.md): V2 真实执行路径拆分，覆盖 SQL iterator、worker THD、Exchange/Gather row 流、InnoDB 分片扫描、full scan 闭环和基础聚合。
  - [v2-test-matrix.md](v2-test-matrix.md): V1/V2 阶段化 MTR 测试矩阵，明确 DOP=1 first 和 DOP>1 range-partition gate。
- Next recommended action: 进入 V2-8J-4 DOP=1 real full scan gate；打开前必须补真实 producer ROW 进入持久 `m_gather` 的路径和 wait/kill policy，`pq_worker_scan_next()` 继续 disabled。
- Parallel-ready task overview: [parallel_wave2_tasks.md](parallel_wave2_tasks.md)
- Remaining risk: Phase 8 的 aggregate 当前仍是基础设施和 eligibility 扩展，真实并行聚合执行尚未启用；locking read 的 EXPLAIN annotation 当前仍可能显示 `Parallel query dop=4`，已从 Phase 9 测试中移除，后续需单独修复。

## Prepared Claude Code Worktrees

Baseline patch applied:

```text
/private/tmp/pq-phase0-1-baseline.patch
```

Worktrees:

| Task | Branch | Path | Prompt source |
|------|--------|------|---------------|
| Phase 2 - Handler/InnoDB contract | `claude-phase2` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase2` | [phase2-handler-innodb-contract.md](phase2-handler-innodb-contract.md) |
| Phase 3 - MQueue/Exchange MVP | `claude-phase3` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase3` | [phase3-mqueue-exchange.md](phase3-mqueue-exchange.md) |
| Test suite classification | `claude-test-suite` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-test-suite` | [parallel_query_suite_manifest.md](parallel_query_suite_manifest.md) |
| Phase 6B-2 - InnoDB pull-row adapter | `claude-phase6b2-innodb-pull-adapter` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6b2-innodb-pull-adapter` | [phase6b2-innodb-pull-adapter.md](phase6b2-innodb-pull-adapter.md) |
| Test manifest v2 fix | `claude-test-manifest-v2` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-test-manifest-v2` | [test-manifest-v2-fix.md](test-manifest-v2-fix.md) |

## Second Wave

Recommended worktrees:

| Task | Branch | Path | Prompt source |
|------|--------|------|---------------|
| W2-A - Phase 4 worker lifecycle | `claude-phase4-worker` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase4-worker` | [phase4-worker-lifecycle.md](phase4-worker-lifecycle.md) |
| W2-B - Phase 5 optimizer hook | `claude-phase5-optimizer-hook` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase5-optimizer-hook` | [phase5-optimizer-hook.md](phase5-optimizer-hook.md) |
| W2-C - Phase 6 InnoDB/read-view design | `claude-phase6-innodb-design` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase6-innodb-design` | [phase6-innodb-readview-design.md](phase6-innodb-readview-design.md) |
| W2-D - test manifest v2 fix | `claude-test-manifest-v2` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-test-manifest-v2` | [test-manifest-v2-fix.md](test-manifest-v2-fix.md) |
| Phase 5B - SQL plan rewrite / iterator MVP | `claude-phase5b-plan-rewrite-iterator` | `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase5b-plan-rewrite-iterator` | [phase5b-plan-rewrite-iterator.md](phase5b-plan-rewrite-iterator.md) |

## Phase Status

| Phase | Status | Owner | Log | Notes |
|------|--------|-------|-----|-------|
| Phase A - V1-MVP interface contract | Completed | Design Agent Russell / Orchestrator | [phaseA-interface-contract.md](phaseA-interface-contract.md) | Hybrid InnoDB route accepted as working direction |
| Phase 0 - System variables and minimal context fields | Completed | Claude Code / Orchestrator | [phase0-system-vars.md](phase0-system-vars.md) | Commit `cb3253f97a8`; system variables and minimal context fields committed |
| Phase 1 - Conservative eligibility and fallback | Completed | Claude Code / Orchestrator | [phase1-eligibility-fallback.md](phase1-eligibility-fallback.md) | Main worktree build passed; new `sql/parallel_query/pq_optimizer.*` must be included in final patch/commit |
| Phase 2 - Handler/InnoDB contract and route validation | Completed | Claude Code / Orchestrator | [phase2-handler-innodb-contract.md](phase2-handler-innodb-contract.md) | v2 fixed `PQ_ref_key`; `mysqld` build passed |
| Phase 3 - MQueue and Exchange MVP | Completed | Claude Code / Orchestrator | [phase3-mqueue-exchange.md](phase3-mqueue-exchange.md) | `exchange.*` and `msg_queue.*`; `mysqld` build passed |
| Phase 4 - Worker lifecycle | Completed | Claude Code / Orchestrator | [phase4-worker-lifecycle.md](phase4-worker-lifecycle.md) | Commit `68bcdefebbb`; no optimizer/InnoDB changes |
| Phase 5 - Optimizer eligibility hook | Completed | Claude Code / Orchestrator | [phase5-optimizer-hook.md](phase5-optimizer-hook.md) | Commit `c2a2a537b64`; `mysqld` build passed |
| Phase 5B - Plan rewrite and iterator MVP | Completed | Claude Code / Orchestrator | [phase5b-plan-rewrite-iterator.md](phase5b-plan-rewrite-iterator.md) | Commit `af895e3edaf`; build passed; guarded SQL iterator hook remains serial-only in V1 |
| Phase 6 - InnoDB/read-view design | Completed | Codex Orchestrator fallback | [phase6-innodb-readview-design.md](phase6-innodb-readview-design.md) | Design-only; Claude Code dispatch stalled; no source changes |
| Phase 6B-1 - InnoDB PQ API skeleton | Completed | Codex Orchestrator fallback | [phase6b-innodb-fullscan.md](phase6b-innodb-fullscan.md) | Commit `f99f3aa7b5d`; `mysqld` build passed |
| Phase 6B-2 - InnoDB pull-row adapter | Completed | Claude Code / Orchestrator | [phase6b2-innodb-pull-adapter.md](phase6b2-innodb-pull-adapter.md) | Commit `f001e1631f7` (base) + `b0ac9740cfd` (review-fix); build passed |
| Phase 7 - EXPLAIN and MVP tests | Completed | Claude Code / Orchestrator | [phase7-explain-mvp-tests.md](phase7-explain-mvp-tests.md) | Commit `9af8fe777d2`; EXPLAIN annotation and MVP MTR committed |
| Phase 8 - V1-complete scaffolding | Completed | Claude Code + Codex Orchestrator fix | [phase8-v1-complete.md](phase8-v1-complete.md) | Commit `113d2ba44c1`; aggregation infrastructure, error path/stat counters, Phase 8 MTR, and macOS `Aligned_atomic` startup fix; full `parallel_query` suite passed |
| Phase 9 - parallel_query suite migration | Completed | Claude Code Test Agent + Codex Orchestrator review | [phase9-parallel-query-suite.md](phase9-parallel-query-suite.md), [parallel_query_suite_manifest.md](parallel_query_suite_manifest.md) | Commit `9c7e9aede42`; 参考测试套 97 个测试已分类；新增 3 个 V1 测试；完整 suite 通过 |
| V1 risk convergence | Completed | Codex Orchestrator + Review Agents | [v1-risk-convergence.md](v1-risk-convergence.md) | Commit `69ed0ac66e7`; EXPLAIN 文案收敛为 candidate/fallback-disabled；fallback counter 不被 EXPLAIN 污染；完整 suite 通过 |
| V2-0 - Execution State Contract | Completed | Codex Orchestrator | [v2-0-execution-state-contract.md](v2-0-execution-state-contract.md) | Commit `a420e8a3f26`; 定义 execution state contract；不启用真实 PQ iterator；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-1 - Iterator Ownership + Safe Fallback | Completed | Codex Orchestrator | [v2-1-iterator-safe-fallback.md](v2-1-iterator-safe-fallback.md) | Commit `9a58ff94cdf`; 允许 eligible execution 返回 PQ iterator，并在 Init 安全窗口串行 fallback；完整 suite 通过 |
| V2-2 - Handler/InnoDB Context Bridge | Completed | Codex Orchestrator + parallel explorers | [v2-2-handler-innodb-context-bridge.md](v2-2-handler-innodb-context-bridge.md) | Commit `a123cdabe7c`; DOP=1 leader init/end bridge；不启动 worker；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-3 - Read View / Trx Contract | Completed | Codex Orchestrator + parallel explorers | [v2-3-read-view-trx-contract.md](v2-3-read-view-trx-contract.md) | Commit `54bd78429cf`; leader probe 不再提前分配 read view；worker row read 明确 unsupported；新增 `pq_read_view_dop1`；完整 suite 通过 |
| V2-4 - Parallel_reader Thin Adapter + Range Partition | Completed | Codex Orchestrator + parallel explorers | [v2-4-parallel-reader-range-partition.md](v2-4-parallel-reader-range-partition.md) | Commit `0b7f7485a7b` + `79d1355cf2f`；`Parallel_reader::export_scan_ranges()`、`Parallel_ranges_built` 和 `pq_range_planning_dop`；完整 suite 通过 |
| V2-5 - Worker THD + Minimal Worker Scan | Completed | Codex Orchestrator + parallel explorers | [v2-5-worker-thd-minimal-scan.md](v2-5-worker-thd-minimal-scan.md) | Commit `1e8078544e8`; Worker lifecycle smoke 已接入；不打开真实 row stream；完整 suite 通过 |
| V2-6 - Exchange/Gather Row Stream | Completed | Codex Orchestrator + parallel explorers | [v2-6-exchange-gather-row-stream.md](v2-6-exchange-gather-row-stream.md) | Commit `d0b4988a584`; Synthetic MQ ROW/FINISH stream smoke 已接入；不进入 `Read()`；完整 suite 通过 |
| V2-7 - Predicate/Projection Boundary | Completed | Codex Orchestrator | [v2-7-predicate-projection-boundary.md](v2-7-predicate-projection-boundary.md) | Commit `1d39b510348`; Boundary MTR 已完成；不打开真实 worker row；完整 suite 通过 |
| V2-8 - Single Table Full Scan Closure | In Progress | Codex Orchestrator | [v2-8-single-table-fullscan-closure.md](v2-8-single-table-fullscan-closure.md) | 当前任务拆分：真实 row materialization / `Read()` 接管前的硬 gate |
| V2-8A - Worker Handler/Prebuilt Contract Design | Completed | Codex Orchestrator + Design Explorers | [v2-8a-worker-handler-prebuilt-contract.md](v2-8a-worker-handler-prebuilt-contract.md) | Commit `8dc30b6a1d2`; 完成 SQL/handler、InnoDB read-view/prebuilt、range dispatch 三项设计收敛；后续先做 V2-8B Row Image Protocol |
| V2-8B - Row Image Protocol | Completed | Codex Orchestrator + Design Explorers | [v2-8b-row-image-protocol.md](v2-8b-row-image-protocol.md) | typed MQ header、fixed record image synthetic materialization 已完成；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8C - DOP=1 Real Full Scan | Contract/Gate Implemented | Codex Orchestrator + Design Explorers | [v2-8c-dop1-real-fullscan.md](v2-8c-dop1-real-fullscan.md) | Commits `24209d09ce3`, `6638e36def7`, `36b2d9a764c`, `09033e1c2a0`, `9c242c61cc7`, `2dab52a2d9c`, `a79c2109d7a`, `13ce85e4d47`; 已落地 `PQ_Worker_open_context`、typed worker wrapper、leader PROBE/EXECUTE mode API、worker_info carrier、worker THD/TABLE helpers、safe-window open-table smoke、handler init/end smoke、DOP=1/blob/independent record gates；真实 row scan 仍未打开 |
| V2-8D - First Row / Read View Contract | Design Selected | Codex Orchestrator + Explorer Agents | [v2-8d-first-row-read-view-contract.md](v2-8d-first-row-read-view-contract.md) | 已确认 `index_first()` 等价首行参数为 `PAGE_CUR_G + match_mode 0`；真实读取选择 `Parallel_reader` visibility adapter 路线，避免 worker-local `row_search_mvcc()` 创建独立 read view；真实 row read 继续 disabled，直到 EXECUTE commit point 和 pull adapter 完成 |
| V2-8E - EXECUTE Commit Point | Completed | Codex Orchestrator | [v2-8e-execute-commit-point.md](v2-8e-execute-commit-point.md) | InnoDB leader `EXECUTE` mode 已能在 leader 线程绑定 active read view；当前 SQL iterator 仍只调用 `PROBE`，不打开真实 row stream；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8F - Parallel_reader Pull Adapter | Callback conversion smoke primitive completed | Codex Orchestrator | [v2-8f-parallel-reader-pull-adapter.md](v2-8f-parallel-reader-pull-adapter.md) | `InnoDB_pq_scan_ctx::smoke_callback_conversion()` 已实现，可用同步 `Parallel_reader` 转换最多一行；先不接 SQL `Read()`，不打开真实 row stream；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8G - EXECUTE Callback Smoke | Completed | Codex Orchestrator | [v2-8g-execute-callback-smoke.md](v2-8g-execute-callback-smoke.md) | SQL iterator safe fallback window 内尝试固定 DOP=1 EXECUTE callback smoke；新增 attempts/rows 状态变量；不打开真实 row stream；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8H - Row Stream Activation Boundary | Completed | Codex Orchestrator | [v2-8h-row-stream-activation.md](v2-8h-row-stream-activation.md) | 抽出 Exchange row/eof/error materialization helper；不接真实 `Read()`；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8I - Iterator Runtime State Contract | Completed | Codex Orchestrator | [v2-8i-iterator-runtime-state.md](v2-8i-iterator-runtime-state.md) | 显式记录 safe-fallback / started / row-returned 边界；不接真实 `Read()`；`mysqld` build 和完整 `parallel_query` suite 通过 |
| V2-8J - Real Read Activation Plan | In Progress | Codex Orchestrator | [v2-8j-real-read-activation-plan.md](v2-8j-real-read-activation-plan.md) | 已完成 callback row producer smoke、worker producer FINISH/EOF/ERROR/abort skeleton 和 `Read()` shadow scaffold；下一步进入 DOP=1 real full scan gate |

## Decisions

- Phase A working decision: use a hybrid InnoDB route. Reuse upstream `Parallel_reader` where possible and add only a thin adapter for SQL PQ gaps.
- Do not directly apply the reference `row0pread_pq.*` implementation as a full fork.
- MVP eligibility must be conservative and fallback serial on uncertainty.
- OOM before worker start may fallback; OOM after worker start is fatal.

## Open Decisions

- LOCKING_READ fallback 的 EXPLAIN 阶段检测需要单独修复；当前 `FOR UPDATE` / `LOCK IN SHARE MODE` / `FOR SHARE` 没有进入 Phase 9 测试。
- 真实并行聚合执行、GROUP BY partial aggregation、ORDER BY、复杂 join、二级索引/ICP 等后移到 V2。

## How To Update

- Update this board whenever a phase starts, completes, is blocked, or changes owner.
- Keep detailed logs in the phase-specific files.
- Do not store full chat transcripts here; store only structured status and decisions.
- Use Chinese by default for phase logs and this board. Keep English terms when they are code identifiers, commands, MySQL concepts, or clearer than a translated phrase.
