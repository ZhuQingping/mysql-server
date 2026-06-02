# Phase 3 - MQueue 与 Exchange MVP

## Goal

实现 PQ V1-MVP 的 worker 到 leader 数据传输基础设施骨架：`MQueue`、消息 token、`Exchange_nosort` 最小接口。Phase 3 不接入执行器，不创建 worker，不做 plan rewrite。

## Scope

- 新增 `msg_queue.h/.cc`：固定容量 ring queue 或等价线程安全队列骨架。
- 新增 `exchange.h/.cc`：leader 端轮询/接收接口骨架。
- 定义消息类型：ROW、FINISH、ERROR、ABORT/CLOSED。
- 明确消息内存归属：Phase 3 可先使用 byte buffer，不依赖 MySQL `Field`/`TABLE`。
- 提供最小可编译实现，不要求单元测试框架。

## Allowed Files

- `sql/parallel_query/msg_queue.h`
- `sql/parallel_query/msg_queue.cc`
- `sql/parallel_query/exchange.h`
- `sql/parallel_query/exchange.cc`
- `sql/CMakeLists.txt`
- `Docs/pq_tasks/phase3-mqueue-exchange.md`

## Forbidden Files

- `sql/sql_optimizer.cc`
- `sql/sql_select.cc`
- `sql/handler.h`
- `storage/**`
- `mysql-test/**`
- worker/thread lifecycle
- iterator 接入代码

## Design Requirements

### MVP 消息模型

建议定义：

```c++
enum class MQMessageType { ROW, FINISH, ERROR, ABORT };

struct MQMessage {
  MQMessageType type;
  uint32 worker_id;
  int error_code;
  std::vector/自有 buffer 或指针+长度;
};
```

如果使用 MySQL 风格而避免 STL，也可以用 `uchar *data`、`size_t len`、明确 owner。

### 行为要求

- `send()` 在 queue closed/abort 后返回错误。
- `receive()` 能区分 ROW、FINISH、ERROR。
- FINISH token 可用于 leader 判断 worker 完成。
- ERROR token 可用于 leader 设置 abort。
- Phase 3 不跨线程实际运行也可以，但接口必须为后续并发设计。

## Claude Code 任务书

复制给 Claude Code：

```text
请先阅读 AGENTS.md，并以 CLAUDE.md 为唯一权威上下文。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 Phase 3：MQueue 与 Exchange MVP。

请先应用主工作树当前 baseline patch，确保包含 Phase 0/1：
git apply /private/tmp/pq-phase0-1-baseline.patch

请阅读：
- CLAUDE.md
- Docs/pq_agent_workflow.md
- Docs/pq_tasks/README.md
- Docs/pq_tasks/phaseA-interface-contract.md
- Docs/pq_tasks/phase3-mqueue-exchange.md
- 参考 spec: /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/Docs/features_for_opensource/01_parallel_query/specs/WL006-PQ-Message-Queue.md

任务目标：
1. 新增 msg_queue.h/.cc；
2. 新增 exchange.h/.cc；
3. 定义 ROW/FINISH/ERROR/ABORT 等消息类型；
4. 实现最小可编译的 send/receive/close/abort 接口；
5. 不接入执行路径，不创建 worker，不使用 handler/InnoDB；
6. 在完成报告中说明内存归属和后续集成风险。

允许修改：
- sql/parallel_query/msg_queue.h
- sql/parallel_query/msg_queue.cc
- sql/parallel_query/exchange.h
- sql/parallel_query/exchange.cc
- sql/CMakeLists.txt
- Docs/pq_tasks/phase3-mqueue-exchange.md

禁止修改：
- sql/sql_optimizer.cc
- sql/sql_select.cc
- sql/handler.h
- storage/**
- mysql-test/**
- worker/thread lifecycle
- iterator 接入代码

验证：
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server-claude-phase3/build-ninja
ninja mysqld -j 16

如果 worktree 没有可用 build-ninja，不要重配大工程，说明原因即可。

完成后：
1. 把完成报告写入 Docs/pq_tasks/phase3-mqueue-exchange.md 的 Code Agent Completion Report 小节；
2. 生成 patch：
git diff > /Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase3-mqueue-exchange.patch
3. 输出 changed files、实现说明、构建/测试结果、风险点、patch 路径。
```

## Code Agent Completion Report

Claude Code 已完成 Phase 3 patch：

- Patch: `/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase3-mqueue-exchange.patch`
- 新增文件：`sql/parallel_query/msg_queue.h`、`sql/parallel_query/msg_queue.cc`
- 新增文件：`sql/parallel_query/exchange.h`、`sql/parallel_query/exchange.cc`
- 修改文件：`sql/CMakeLists.txt`

实现摘要：

- `MQueue`：基于 power-of-2 ring buffer 的 worker -> leader 消息队列骨架。
- `MQueue_handle`：提供 `send()`、`receive()`、`close_producer()`、`abort_consumer()`。
- `MQMessageType`：定义 `ROW`、`FINISH`、`ERROR`、`ABORT`。
- `Exchange`：管理每个 worker 对应的 `MQueue_handle`、ring buffer 和事件对象。
- `Exchange_nosort`：提供 round-robin 方式读取 worker MQ 的最小接口。

Phase 3 未接入执行路径，未创建 worker，未修改 handler/InnoDB。

## Orchestrator Review

审查时间：2026-06-02

结论：Phase 3 任务边界正确，已合入 Phase 3 独有改动。

Patch 流程说明：

- `claude-phase3-mqueue-exchange.patch` 夹带了 Phase 0/1 baseline，不能整包应用到当前主工作树。
- 主工作树当前已包含 Phase 0/1/2 commits，因此本次只合入 Phase 3 独有改动：`msg_queue.*`、`exchange.*`、`sql/CMakeLists.txt` 中 `exchange.cc` / `msg_queue.cc` 两行。

Orchestrator 修正：

- `msg_queue.h` 增加 `<chrono>`，因为 `PQ_mq_event::wait_latch()` 直接使用 `std::chrono::microseconds`。
- `exchange.h` 增加 `<cassert>`，因为 header 内联函数直接使用 `assert()`。
- `exchange.cc` 修正 `void **datap` 输出参数赋值，避免只改局部指针。
- `exchange.cc` 修正 round-robin 扫描逻辑，已完成队列不会重复扣减 `m_active_readers`，单轮扫描覆盖 `m_nqueues`。

构建验证：

```bash
cmake --build build-ninja --target mysqld -j 16
```

结果：通过。构建日志确认编译了：

- `sql/CMakeFiles/sql_main.dir/parallel_query/exchange.cc.o`
- `sql/CMakeFiles/sql_main.dir/parallel_query/msg_queue.cc.o`

已知风险：

- 当前 `new[]/delete[]` 仅用于 Phase 3 骨架；Phase 4/worker lifecycle 接入时应切换到 `pq_mem_root` 或明确 allocator 归属。
- 控制 token 当前以 1-byte payload 编码，因此 1-byte ROW payload 会被 `Exchange_nosort` 识别为控制 token。Phase 5 做行序列化时需要引入明确 message header，不能继续依赖 payload 长度区分。
- `receive()` 返回的 data 指向 `MQueue_handle` 本地 buffer，调用方必须在下一次 receive 前复制。
- 当前没有真实多线程测试；Phase 4/5 接入 worker 后需要补充并发、abort、error、full queue、empty queue 场景。

## Acceptance Checklist

- [x] 任务边界已定义。
- [x] Claude Code 任务书已生成。
- [x] `msg_queue.h/.cc` 实现完成。
- [x] `exchange.h/.cc` 实现完成。
- [x] CMake 接入完成。
- [x] Orchestrator 审查 patch。
- [x] `ninja mysqld` 通过。

## Current Status

Phase 3 已合入主工作树，`cmake --build build-ninja --target mysqld -j 16` 已通过。
