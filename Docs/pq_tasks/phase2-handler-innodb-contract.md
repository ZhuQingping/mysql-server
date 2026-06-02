# Phase 2 - Handler/InnoDB 契约骨架与路线验证

## Goal

为 PQ V1-MVP 定义 SQL 层到 handler/InnoDB 的最小契约骨架，并验证 InnoDB 路线：优先复用上游 `Parallel_reader`，只有确有必要时才引入薄 `ha_innodb_pq` / `row0pread_pq` 适配层。

本阶段不实现真正 InnoDB 并行扫描，不修改 `sql/handler.h`，不修改 `storage/**`。

## Scope

- 新增 SQL 层 PQ handler 契约类型，例如 `PQ_Slice`、`PQ_Range`、`PQ_Scan_ctx`、`PQ_Leader_context`、`PQ_Worker_context`。
- 输出 InnoDB 路线验证结论：`Parallel_reader` 可复用能力、缺口、后续是否需要 handler API。
- 仅提供头文件/轻量 `.cc` 骨架，不接执行路径。
- 如果必须修改 `sql/handler.h` 或 `storage/innobase/**`，Code Agent 必须停止并写明原因。

## Allowed Files

- `sql/parallel_query/pq_handler.h`
- `sql/parallel_query/pq_handler.cc`
- `sql/CMakeLists.txt`：仅用于加入新增 `.cc`
- `Docs/pq_tasks/phase2-handler-innodb-contract.md`

## Forbidden Files

- `sql/handler.h`
- `sql/sql_optimizer.cc`
- `sql/sql_select.cc`
- `storage/**`
- `mysql-test/**`
- worker/thread、plan rewrite、iterator 接入代码

## Design Requirements

### 最小契约建议

建议新增类型：

```c++
struct PQ_Range;
struct PQ_Slice;
class PQ_Scan_ctx;
class PQ_Leader_context;
class PQ_Worker_context;
```

建议能力：

- 表示 leader 分配出来的扫描 slice/range。
- 表示 worker 领取 slice 的状态。
- 表示扫描上下文生命周期，但不持有 InnoDB 私有对象。
- 提供错误码/状态字段，但不调用 handler/InnoDB。

### InnoDB 路线验证必须回答

- `Parallel_reader` 当前能否提供 clustered full scan 的分块和可见性检查？
- `Parallel_reader` 当前是 push-row 模型还是 pull-row 模型？
- SQL worker 执行 plan + MQ 是否需要 pull API？
- 是否能通过薄 adapter 复用 `Parallel_reader`，而不是复制 `row0pread` 算法？
- Phase 6 前必须修改哪些中心文件，分别由哪个后续阶段处理？

## Claude Code 任务书

复制给 Claude Code：

```text
请先阅读 AGENTS.md，并以 CLAUDE.md 为唯一权威上下文。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 Phase 2：Handler/InnoDB 契约骨架与路线验证。

请先应用主工作树当前 baseline patch，确保包含 Phase 0/1：
git apply /private/tmp/pq-phase0-1-baseline.patch

请阅读：
- CLAUDE.md
- Docs/pq_agent_workflow.md
- Docs/pq_tasks/README.md
- Docs/pq_tasks/phaseA-interface-contract.md
- Docs/pq_tasks/phase2-handler-innodb-contract.md
- storage/innobase/include/row0pread.h
- storage/innobase/row/row0pread.cc
- sql/handler.h

任务目标：
1. 新增 sql/parallel_query/pq_handler.h 和 pq_handler.cc 的最小契约骨架；
2. 定义 PQ_Range/PQ_Slice/PQ_Scan_ctx/PQ_Leader_context/PQ_Worker_context 或等价类型；
3. 不调用 handler/InnoDB，不改执行路径；
4. 在文档中写明 InnoDB 路线验证结论；
5. 如果认为必须改 sql/handler.h 或 storage/**，停止并说明原因，不要直接改。

允许修改：
- sql/parallel_query/pq_handler.h
- sql/parallel_query/pq_handler.cc
- sql/CMakeLists.txt
- Docs/pq_tasks/phase2-handler-innodb-contract.md

禁止修改：
- sql/handler.h
- sql/sql_optimizer.cc
- sql/sql_select.cc
- storage/**
- mysql-test/**
- worker/thread、plan rewrite、iterator 接入代码

验证：
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase2/build-ninja
ninja mysqld -j 16

如果 worktree 没有可用 build-ninja，不要重配大工程，说明原因即可。

完成后：
1. 把完成报告写入 Docs/pq_tasks/phase2-handler-innodb-contract.md 的 Code Agent Completion Report 小节；
2. 生成 patch：
git diff > /Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase2-handler-innodb-contract.patch
3. 输出 changed files、实现说明、构建/测试结果、风险点、patch 路径。
```

## Code Agent Completion Report

Claude Code 已完成 Phase 2 v2 patch：

- Patch: `/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase2-handler-innodb-contract-v2.patch`
- Worktree: `/Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase2`
- 新增文件：`sql/parallel_query/pq_handler.h`、`sql/parallel_query/pq_handler.cc`
- 修改文件：`sql/CMakeLists.txt`、本任务文档

Claude Code 报告结论：

- `Parallel_reader` 可复用 B+tree 分区、可见性检查、线程预算、错误传播模式。
- `Parallel_reader` 当前是 push-row 模型，SQL PQ worker 执行 iterator plan 需要 pull-row API。
- Phase 6 推荐 HYBRID 路线：复用 `Parallel_reader` 的分区/可见性/线程预算能力，只新增薄 `row0pread_pq` / `ha_innodb_pq` 适配层。
- Phase 2 不需要修改 `sql/handler.h`，后续 PQ handler API 作为 `ha_innobase` 专有方法，由 SQL PQ 路径在确认 InnoDB 后调用。
- Claude Code 报告称已通过主仓库 `build-ninja` 临时拷贝方式执行 `ninja mysqld -j 16`。
- v2 已按 Orchestrator 初审意见修正 `PQ_ref_key` 所有权、非 ASCII 注释、`make_scan_ctx()` pure virtual 定义混乱等问题。

## Orchestrator Review

审查时间：2026-06-02

结论：Phase 2 方向正确，但暂不建议直接合入 patch，需要先修正若干基础问题。

必须修正：

- `PQ_ref_key` 只实现了析构函数和拷贝构造函数，未实现拷贝赋值、移动构造、移动赋值，也未显式 delete。当前默认拷贝赋值会浅拷贝 `ptr`，后续一旦赋值会导致 double free。应按 Rule of Five 补全，或改用 `std::vector<uchar>` / `std::string` 持有 key bytes。
- `pq_handler.h` 在头文件中使用 `malloc/free/memcpy/memcmp`，但没有直接 include `<cstdlib>` 和 `<cstring>`。不能依赖间接 include。
- `PQ_ref_key` 的构造函数未处理 `src == nullptr && n > 0`、`malloc` 失败等情况。基础类型应避免未定义行为。
- `pq_handler.cc` 中有非 ASCII 注释符号，应按本仓库新增源码默认 ASCII 的约定替换。
- `PQ_Leader_context::make_scan_ctx()` 在头文件中声明为 pure virtual，但 `.cc` 又提供了 base implementation，报告描述也混用“pure virtual/abstract method”。C++ 允许 pure virtual 有定义，但当前没有必要，容易误导。建议删除 `.cc` 中该定义，或将接口改为非 pure virtual 并明确返回 `nullptr` 的语义。

流程注意：

- 当前 Phase 2 patch 不能直接应用到主工作树；`git apply --check` 已失败，原因是主工作树已有 Phase 0/1 文档和 `sql/CMakeLists.txt` 已被修改。
- 合入 Phase 2 时应抽取 Phase 2 独有改动：`pq_handler.h`、`pq_handler.cc`、`sql/CMakeLists.txt` 中仅 `parallel_query/pq_handler.cc` 一行、本文件的完成报告。
- 建议先固定 Phase 0/1 baseline commit，再合入 Phase 2 修正版，避免后续多个 Claude worktree 的 patch 互相包含 baseline。

## Orchestrator Review - v2

审查时间：2026-06-02

结论：v2 已处理初审阻塞项，可以合入 Phase 2 独有改动。

已确认：

- `PQ_ref_key` 改用 `std::vector<uchar>` 持有 key bytes，消除了默认拷贝赋值 double free 风险。
- `PQ_ref_key` 构造函数已处理 `src == nullptr`，不会在空指针上做字节拷贝。
- `pq_handler.cc` 中非 ASCII 注释符号已替换为 ASCII。
- `PQ_Leader_context::make_scan_ctx()` 只保留 pure virtual 声明，不再提供误导性的 base implementation。
- Phase 2 仍未修改 `sql/handler.h`、`storage/**`、执行路径或测试目录，符合任务边界。

Orchestrator 追加修正：

- `pq_handler.h` 直接 include `<cstddef>` 和 `<utility>`，确保 `std::size_t`、`std::pair` 自包含。
- 调整 `PQ_Worker_context::dispatch_ctx()` 参数缩进。

## Acceptance Checklist

- [x] 任务边界已定义。
- [x] Claude Code 任务书已生成。
- [x] 契约骨架实现完成。
- [x] InnoDB 路线验证结论写入文档。
- [x] Orchestrator 审查 patch。
- [x] Phase 2 修正项处理完成。
- [x] Phase 2 独有改动合入主工作树。
- [x] `ninja mysqld` 通过。

## Current Status

Phase 2 v2 已合入主工作树，`cmake --build build-ninja --target mysqld -j 16` 已通过。
