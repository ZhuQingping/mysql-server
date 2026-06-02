# Phase 4 - Worker 生命周期与 Gather 骨架

## Goal

实现 PQ V1-MVP 的 SQL 层 worker lifecycle / Gather skeleton，使后续 Phase 5/6 可以挂接 plan rewrite、Exchange 和 handler PQ scan。

Phase 4 只建立可编译骨架，不接入 optimizer，不启动真实 InnoDB scan，不改变任何查询执行结果。

## Scope

- 新增 `sql/parallel_query/sql_parallel.h`
- 新增 `sql/parallel_query/sql_parallel.cc`
- 定义 `Gather_operator`、`PQ_worker_info`、`PQ_worker_manager` 或等价骨架类型。
- 定义 worker 状态枚举、错误传播字段、启动/等待/清理接口。
- 为 Phase 3 的 `Exchange_nosort` 预留连接点。
- 使用 Phase 0 的 `THD::pq_*` 字段，但不强行改动 THD 初始化流程。

## Allowed Files

- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/CMakeLists.txt`
- `Docs/pq_tasks/phase4-worker-lifecycle.md`

如确需新增更小的 helper 文件，必须在完成报告中说明理由。

## Forbidden Files

- `sql/sql_optimizer.cc`
- `sql/sql_select.cc`
- `sql/handler.h`
- `storage/**`
- `mysql-test/**`
- optimizer hook
- plan rewrite
- InnoDB scan implementation

## Design Requirements

- 所有接口必须默认 inert：未显式调用时不影响现有查询。
- `Gather_operator` 应持有 DOP、worker 数组、Exchange 指针、错误状态、完成状态。
- worker manager 暂可只提供接口和 stub，不要求真正创建 OS thread。
- 如果实现 thread create stub，必须默认不被任何执行路径调用。
- 错误优先级按 Phase A：kill > leader fatal/OOM > worker fatal > MQ closed > normal finish。
- 内存分配先用最小安全方式；如果未接 `pq_mem_root`，必须在风险点说明 Phase 5/6 前需替换。

## Validation

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase4-worker
cmake --build build-ninja --target mysqld -j 16
```

如果 worktree 没有 build-ninja，不要重配大工程；可在完成报告说明，并由主控在主工作树验证。

## Claude Code 任务书

```text
请先阅读 AGENTS.md，并以 CLAUDE.md 为唯一权威上下文。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 W2-A / Phase 4：Worker 生命周期与 Gather 骨架。

请阅读：
- CLAUDE.md
- Docs/pq_agent_workflow.md
- Docs/pq_tasks/parallel_wave2_tasks.md
- Docs/pq_tasks/phaseA-interface-contract.md
- Docs/pq_tasks/phase3-mqueue-exchange.md
- Docs/pq_tasks/phase4-worker-lifecycle.md
- sql/parallel_query/exchange.h
- sql/parallel_query/msg_queue.h
- sql/sql_class.h

任务目标：
1. 新增 sql/parallel_query/sql_parallel.h/.cc；
2. 定义 Gather_operator、PQ_worker_info、PQ_worker_manager 或等价 skeleton；
3. 预留 worker start/wait/abort/cleanup 接口；
4. 预留 Exchange_nosort 挂接字段；
5. 不接 optimizer，不改执行路径，不调用 InnoDB/handler；
6. 修改 sql/CMakeLists.txt 仅加入新增 .cc；
7. 把完成报告写入 Docs/pq_tasks/phase4-worker-lifecycle.md。

禁止修改：
- sql/sql_optimizer.cc
- sql/sql_select.cc
- sql/handler.h
- storage/**
- mysql-test/**

验证：
cmake --build build-ninja --target mysqld -j 16

完成后生成 patch：
git diff > /Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase4-worker-lifecycle.patch
```

## Code Agent Completion Report

Claude Code 已完成 Phase 4 patch：

- Patch: `/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase4-worker-lifecycle.patch`
- 新增文件：`sql/parallel_query/sql_parallel.h`
- 新增文件：`sql/parallel_query/sql_parallel.cc`
- 修改文件：`sql/CMakeLists.txt`
- 修改文件：本任务文档

实现摘要：

- 定义 `PQ_worker_info`：worker id、状态、worker THD、错误码、MQ handle、worker scan context。
- 定义 `PQ_worker_manager`：`start()`、`wait()`、`abort()`、`cleanup()` skeleton。
- 定义 `Gather_operator`：DOP、worker 数组、`Exchange_nosort`、worker manager、错误状态、完成状态。
- Phase 4 不创建 OS thread，不接 optimizer，不改执行路径，不调用 handler/InnoDB。
- `Gather_operator::init()` 当前使用 `new[]/delete[]` 创建 worker info 和 Exchange，后续需切换到 `pq_mem_root`。

## Orchestrator Review

审查时间：2026-06-02

结论：Phase 4 范围符合任务要求，已合入主工作树并通过构建。

Patch 流程说明：

- `claude-phase4-worker-lifecycle.patch` 不能整包应用，因为主工作树已有 `Docs/pq_tasks/phase4-worker-lifecycle.md` 任务书。
- 本次只提取 Phase 4 独有源码改动：`sql_parallel.h/.cc` 和 `sql/CMakeLists.txt` 中 `parallel_query/sql_parallel.cc` 一行。
- 文档 Completion Report 由主控合并到现有 Phase 4 任务书。

Orchestrator 修正：

- 将 `sql_parallel.h/.cc` 新增源码中的非 ASCII 注释符号替换为 ASCII。
- 给 Phase 4 stub 中暂未使用的 `leader_thd` 参数加 `[[maybe_unused]]`，消除编译 warning。

构建验证：

```bash
cmake --build build-ninja --target mysqld -j 16
```

结果：通过，且重跑后无 `sql_parallel.cc` 相关 warning。构建日志确认编译了：

- `sql/CMakeFiles/sql_main.dir/parallel_query/sql_parallel.cc.o`

已知风险：

- 当前 `new[]/delete[]` 仅用于 Phase 4 skeleton；Phase 5/6 接入真实执行路径前必须改为 `pq_mem_root` 或明确 allocator 归属。
- `PQ_worker_manager::wait()` 是 stub，若未来未调用 `start()` 就直接 `wait()`，当前不会把 `NOT_STARTED` worker 转为 terminal，但会返回 normal finish。Phase 5 接入真实生命周期时必须重写等待状态机。
- `resolve_error_priority()` 当前不检查 `THD::killed` 和 leader fatal，只扫描 worker error/killed 状态；Phase 5 接入实际调用点时必须补齐 kill/leader fatal 优先级。
- 当前没有真实多线程、MQ、abort、full queue、empty queue 行为测试；这些留到 Phase 5/7。

## Acceptance Checklist

- [x] Worker/Gather skeleton 可编译。
- [x] 未接入 optimizer 或执行路径。
- [x] 未修改 handler/InnoDB。
- [x] CMake 接入完成。
- [x] `mysqld` 构建通过。

## Current Status

Phase 4 已合入主工作树，`cmake --build build-ninja --target mysqld -j 16` 已通过。等待主控提交。
