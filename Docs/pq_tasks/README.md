# Parallel Query V1 Progress Board

本文件是 PQ V1 的总进度看板。各阶段详细说明保存在对应 phase 文档中。

## Current Summary

- Last synced: 2026-06-11
- Current phase: Phase 0-9 已提交完成；PQ V1 风险收敛、V2-0、V2-1、V2-2、V2-3、V2-4 已提交；当前进入 V2-5 Worker THD + Minimal Worker Scan。
- Latest commits:
  - Phase 9: `9c7e9aede42` Add PQ phase 9 test suite migration
  - V1 risk convergence: `69ed0ac66e7` Tighten PQ V1 risk boundaries
  - V2-2: `a123cdabe7c` Add PQ V2-2 handler context bridge
  - V2-3: `54bd78429cf` Tighten PQ V2-3 read view boundary
  - V2-4: `0b7f7485a7b` Add PQ V2-4 range planning export
  - V2-4 observable: `79d1355cf2f` Add PQ V2-4 range planning status
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
  - [v2-5-worker-thd-minimal-scan.md](v2-5-worker-thd-minimal-scan.md): V2-5 当前任务书，收敛 worker THD/lifecycle smoke 与 minimal worker scan 边界。
  - [v2-execution-path-roadmap.md](v2-execution-path-roadmap.md): V2 真实执行路径拆分，覆盖 SQL iterator、worker THD、Exchange/Gather row 流、InnoDB 分片扫描、full scan 闭环和基础聚合。
  - [v2-test-matrix.md](v2-test-matrix.md): V1/V2 阶段化 MTR 测试矩阵，明确 DOP=1 first 和 DOP>1 range-partition gate。
- Next recommended action: 开始 V2-5 Worker THD + Minimal Worker Scan；先做 worker lifecycle smoke 和错误/清理可观测，真实 row stream 留到 V2-6。
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
| V2-5 - Worker THD + Minimal Worker Scan | In Progress | Codex Orchestrator + parallel explorers | [v2-5-worker-thd-minimal-scan.md](v2-5-worker-thd-minimal-scan.md) | 当前任务拆分：先做 worker lifecycle smoke，不打开真实 row stream |

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
