# Parallel Query V1 Progress Board

本文件是 PQ V1 的总进度看板。各阶段详细说明保存在对应 phase 文档中。

## Current Summary

- Current phase: Phase 0-9 已提交完成；PQ V1 当前主线阶段已收口。
- Latest commits:
  - Phase 7: `9af8fe777d2` Add PQ phase 7 EXPLAIN and MVP tests
  - Phase 8: `113d2ba44c1` Add PQ phase 8 V1 completion scaffolding
- Phase 8 validation:
  - `cmake --build build-ninja --target mysqld -j 16` 通过
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help` 通过
  - `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt` 通过，完整 `parallel_query` suite 共 10 个测试通过
- Phase 9 validation:
  - `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt` 通过，实际 `parallel_query` 测试 12 个，加 `shutdown_report` 共 13 项通过
- Next recommended action: 进入 V1 总体验收和已知风险收敛。
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
| Phase 0 - System variables and minimal context fields | Patch received | Claude Code / Orchestrator | [phase0-system-vars.md](phase0-system-vars.md) | Needs review and build verification |
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
| Phase 9 - parallel_query suite migration | Completed | Claude Code Test Agent + Codex Orchestrator review | [phase9-parallel-query-suite.md](phase9-parallel-query-suite.md), [parallel_query_suite_manifest.md](parallel_query_suite_manifest.md) | Commit `f91ec75a051`; 参考测试套 97 个测试已分类；新增 3 个 V1 测试；完整 suite 通过 |

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
