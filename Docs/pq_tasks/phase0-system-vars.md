# Phase 0 - 系统变量与最小上下文字段

## Goal

增加 Parallel Query V1-MVP 后续阶段所需的最小配置项和上下文字段骨架，但不接入任何执行路径。

Phase 0 必须保证可编译，并且对所有现有非 PQ 查询保持行为不变。

## Language Rule

本文件以及后续 Phase 0 相关任务日志默认使用中文编写；中文可能产生歧义或需要对齐 MySQL/C++ 代码语义时，可以保留英文术语，例如 `System_variables`、`THD`、`Query_block`、`JOIN`、`MEM_ROOT`、`optimizer_switch`。

## Scope

设计和后续实现范围：

- 按 MySQL 现有 sysvar 风格增加 PQ global/session 变量。
- 在 `System_variables` 中增加 session 变量对应的最小字段。
- 在 `THD`、`Query_block`、`JOIN` 中增加后续资格判定、plan rewrite、worker 生命周期和诊断所需的最小字段。
- 所有字段初始化为串行/no-PQ 默认值。
- 暂不调用资格判定、plan rewrite、worker 创建、handler PQ API 或 InnoDB 代码。

## Allowed Files

后续 Code Agent 允许修改：

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `sql/sql_class.h`
- `sql/sql_class.cc` if initialization/cleanup requires code outside inline defaults
- `sql/sql_lex.h`
- `sql/sql_optimizer.h`
- `sql/sql_const.h`：仅当明确接受在 Phase 0 增加 `optimizer_switch` flag 时才允许修改

## Forbidden Files

Phase 0 禁止修改：

- `sql/sql_optimizer.cc`
- `sql/sql_select.cc`
- `sql/handler.h`
- `storage/**`
- `mysql-test/**` except optional later test-only MTR after implementation
- 任何执行路径接入代码

## Assigned Agents

暂未分配外部 Design Agent。Orchestrator 已完成本地设计确认。

Code Agent：
- 角色：Code
- 执行方：Claude Code
- Patch：`/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase0-system-vars.patch`
- 完成报告：待补充到 `## Code Agent Completion Report`

原建议下一步 Code Agent：
- 角色：Code
- 任务：用户批准后，仅实现 Phase 0 骨架。

实现后的建议 Test Agent：
- 角色：Test
- 任务：验证 `SHOW VARIABLES LIKE 'parallel%';`、`SET SESSION ...` 和 `ninja mysqld`。

## Design Notes

### 现有代码落点

- 系统变量声明位于 `sql/sys_vars.cc`。
- session 变量的后端字段位于 `sql/system_variables.h` 的 `struct System_variables`。
- `THD` 在 `sql/sql_class.h` 中，已有 `struct System_variables variables` 和查询内存根。
- `Query_block` 在 `sql/sql_lex.h` 中，持有 query block 状态和 `JOIN *join`。
- `JOIN` 的优化器/执行器状态位于 `sql/sql_optimizer.h`。
- `optimizer_switch` bit 定义在 `sql/sql_const.h`；当前 `OPTIMIZER_SWITCH_LAST` 是 `1ULL << 26`，技术上还能新增 bit，但首版 Phase 0 不必须做。

### 推荐 MVP 变量

Phase 0 建议先增加一组小而稳定的变量：

| 变量 | 作用域 | 类型 | 默认值 | 用途 |
|----------|-------|------|---------|---------|
| `parallel_query` | GLOBAL/SESSION | bool | OFF | 总开关。默认 OFF，保证上游行为不变。 |
| `parallel_default_dop` | GLOBAL/SESSION | uint | 4 | 后续资格判定/规划默认请求的 DOP。 |
| `parallel_cost_threshold` | GLOBAL/SESSION | ulonglong | 10000 | 后续 PQ 资格判定的最低 optimizer cost。 |
| `parallel_memory_limit` | GLOBAL/SESSION | ulonglong | 256M | 后续 PQ 分配和准入的内存上限。 |
| `parallel_queue_timeout` | GLOBAL/SESSION | uint | 0 | 后续等待 worker/thread 资源的超时时间；0 表示不等待或禁用超时。 |

除非为了参考实现兼容性必须提前增加，否则以下变量延后：

- `force_parallel_execute`
- `pq_support_features_switch`
- thread-pool/resource-stat variables
- record-buffer/prefetch variables
- any hint-specific variables

理由：
- 参考 specs 明确提到 `parallel_default_dop`、`parallel_cost_threshold`、`parallel_memory_limit` 和 `parallel_queue_timeout`。
- 对 MVP 来说，独立的 `parallel_query` bool 比立刻改 `optimizer_switch` 风险更低。
- `optimizer_switch='parallel_query=on/off'` 兼容语法可以后续再加：在 `sql/sql_const.h` 增加 `OPTIMIZER_SWITCH_PARALLEL_QUERY`，并在 `sql/sys_vars.cc` 的 `optimizer_switch_names` 注册。

### 最小 THD 字段

只增加带默认值的 inert 字段：

```text
bool pq_is_worker = false;
bool pq_executed = false;
uint pq_dop = 0;
int pq_error = 0;
MEM_ROOT *pq_mem_root = nullptr;
void *pq_leader = nullptr;
void *pq_worker_info = nullptr;
void *pq_gathers = nullptr;
```

说明：
- 指针字段在 Phase 0 保持 opaque，避免提前引入头文件依赖或循环依赖。
- Phase 0 不分配 `pq_mem_root`，只预留字段；具体分配和生命周期属于后续 worker lifecycle 阶段。
- 后续如果需要强类型，可以在 `sql/parallel_query` 类存在后通过 forward declaration 替换。

### 最小 Query_block 字段

只增加规划/结果标记：

```text
bool parallel_exec = false;
bool pq_candidate = false;
void *pq_unsuite_info = nullptr;
```

说明：
- `parallel_exec` 表示该 query block 已在后续阶段被接受进入 PQ。
- `pq_candidate` 可供 Phase 1 资格判定记录 rewrite 前的候选状态。
- `pq_unsuite_info` 在 `pq_optimizer.*` 出现前保持 opaque。

### 最小 JOIN 字段

只增加 optimizer 阶段占位字段：

```text
bool pq_eligible = false;
bool pq_plan_rewritten = false;
bool need_tmp_pq_leader = false;
uint pq_dop = 0;
void *pq_optimized_var = nullptr;
```

说明：
- Phase 0 中这些字段不得影响 `JOIN::optimize()` 或执行。
- Phase 1 可以填充 `pq_eligible`；Phase 5 可以填充 rewrite 相关字段。

### 行为不变式

所有新变量默认禁用 PQ 或保持 inert 值。由于 Phase 0 不接执行路径，所有现有查询必须和上游行为完全一致。

### 实现后的验证计划

编译：

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja
ninja mysqld -j 16
```

安装并启动 server 后手工 SQL：

```sql
SHOW VARIABLES LIKE 'parallel%';
SET SESSION parallel_query = ON;
SET SESSION parallel_default_dop = 4;
SET SESSION parallel_cost_threshold = 10000;
SET SESSION parallel_memory_limit = 268435456;
SET SESSION parallel_queue_timeout = 0;
```

后续 MTR：

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja/mysql-test
./mtr main.1st
```

## Implementation Log

Claude Code 已完成 Phase 0 编码任务并产出 patch：

```text
/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase0-system-vars.patch
```

Orchestrator 初步观察：
- 当前主工作树中已经存在与该 patch 对应的源码修改。
- 仍需 Orchestrator 正式审查 patch、确认是否越界、运行构建，并更新验收清单。

## Code Agent Completion Report

### Claude Code - 2026-06-01 20:03

**Status:** ✅ Done — Phase 0 skeleton 实现完成，编译通过，patch 已生成。

**Changed files:**

| 文件 | 改动行数 | 改动内容 |
|------|---------|---------|
| `sql/system_variables.h` | +36 | 5 个 PQ session 字段（`parallel_query`、`parallel_default_dop`、`parallel_cost_threshold`、`parallel_memory_limit`、`parallel_queue_timeout`） |
| `sql/sys_vars.cc` | +40 | 5 个 Sys_var 定义（`Sys_parallel_query`、`Sys_parallel_default_dop`、`Sys_parallel_cost_threshold`、`Sys_parallel_memory_limit`、`Sys_parallel_queue_timeout`） |
| `sql/sql_class.h` | +30 | 8 个 THD PQ 字段（`pq_is_worker`、`pq_executed`、`pq_dop`、`pq_error`、`pq_mem_root`、`pq_leader`、`pq_worker_info`、`pq_gathers`） |
| `sql/sql_lex.h` | +15 | 3 个 Query_block PQ 字段（`parallel_exec`、`pq_candidate`、`pq_unsuite_info`） |
| `sql/sql_optimizer.h` | +21 | 5 个 JOIN PQ 字段（`pq_eligible`、`pq_plan_rewritten`、`need_tmp_pq_leader`、`pq_dop`、`pq_optimized_var`） |

总计 **194 行 patch**，改动 **5 个文件**，不触碰任何执行路径。

**Implementation summary:**

- **系统变量**：5 个 SESSION 级变量，使用 `SESSION_VAR()` 宏与 `System_variables` 字段名精确对应。`parallel_query` 默认 OFF 保证上游行为不变；`parallel_default_dop` 范围 1~256 默认 4；`parallel_cost_threshold` 默认 10000；`parallel_memory_limit` 默认 268435456（256MB）；`parallel_queue_timeout` 默认 0（无超时）。
- **THD 字段**：8 个字段全部有安全默认值（bool=false, uint=0, int=0, nullptr）。`pq_mem_root`、`pq_leader`、`pq_worker_info`、`pq_gathers` 使用 `void*` 避免引入 PQ 头文件依赖。
- **Query_block 字段**：`parallel_exec`、`pq_candidate` 标记 query block 的 PQ 候选/执行状态；`pq_unsuite_info` 为 opaque 指针。
- **JOIN 字段**：`pq_eligible`、`pq_plan_rewritten`、`need_tmp_pq_leader` 为 bool 标记；`pq_dop` 为 DOP 值；`pq_optimized_var` 为保存优化器状态的 opaque 指针。
- **禁止项全部遵守**：未修改 `sql_optimizer.cc`、`sql_select.cc`、`handler.h`、`storage/**`、`mysql-test/**`、CMake 文件，未接入任何执行路径。

**Commands run:**

```bash
# 增量编译
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja
ninja mysqld -j 16

# 生成 patch
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server
git diff > claude-phase0-system-vars.patch
```

**Build result:** ✅ 编译成功。`ninja mysqld` 完成 2664/2664 步，mysqld 二进制已生成（62MB，时间戳 2026-06-01 20:03）。无编译错误、无编译警告。

**Test result:** Phase 0 是 skeleton-only，没有接入任何执行路径，未运行 mtr 功能测试。建议启动 MySQL 实例后运行以下 SQL 验证：

```sql
SHOW VARIABLES LIKE 'parallel%';
SET SESSION parallel_query = ON;
SET SESSION parallel_default_dop = 8;
SET SESSION parallel_cost_threshold = 5000;
SET SESSION parallel_memory_limit = 536870912;
SET SESSION parallel_queue_timeout = 100;
```

mtr 回归验证建议在启动 MySQL 实例后执行：

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja/mysql-test
./mtr main.1st
```

**Known risks:**

1. **变量名冲突**：MySQL 已有 `innodb_parallel_read_threads` 等并行相关变量，但 `parallel_query` 这个名字在当前 8.0.46 中不存在。如果后续上游版本增加同名变量需注意冲突。
2. **结构体布局变化**：新增 5 个字段改变 `System_variables` 的布局和大小。sys_var 机制管理了 `memcpy`/同步操作，新增字段不影响现有行为，但插件/存储引擎若直接操作 `System_variables` 结构体可能受 ABI 影响（V1 不考虑 ABI 兼容性）。
3. **THD 内存微增**：8 个字段约增加 48 字节（3×bool + 1×uint + 1×int + 1×MEM_ROOT* + 3×void*），对每个连接内存影响极小。
4. **void* 需后续替换**：Phase 1+ 实现具体 PQ 类型后，`pq_leader`、`pq_worker_info`、`pq_gathers`、`pq_unsuite_info`、`pq_optimized_var` 需从 `void*` 替换为实际类型指针或 forward declaration。
5. **未增加 optimizer_switch bit**：当前未在 `sql/sql_const.h` 增加 `OPTIMIZER_SWITCH_PARALLEL_QUERY`，`parallel_query` 作为独立 bool 变量存在。如需兼容 `SET optimizer_switch='parallel_query=on/off'` 语法，需在后续 Phase 中额外实现。

**Patch path:**

```
/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase0-system-vars.patch
```

Patch 194 行，可通过 `git apply claude-phase0-system-vars.patch` 在任意同版本 worktree 上应用。

## Build Log

```bash
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja
ninja mysqld -j 16
# 结果：2664/2664 步完成，无错误无警告
# mysqld 二进制：62MB，时间戳 2026-06-01 20:03
```

✅ 编译成功。

## Test Log

Phase 0 skeleton-only，未运行 mtr。建议验证步骤：

```bash
# 启动 MySQL 实例后执行
cd /Users/zhuqingping/Work/Database/MySQL/mysql-server/build-ninja/mysql-test
./mtr main.1st
# SQL 验证
SHOW VARIABLES LIKE 'parallel%';
SET SESSION parallel_query = ON;
```

## Review Findings

Orchestrator 审查结论：
- Phase 0 不应增加执行 hook。
- Phase 0 不应增加 InnoDB 或 handler API。
- 暂不搬完整参考变量集，先增加 MVP 变量。
- 除非用户明确要求 Phase 0 兼容参考实现的 `optimizer_switch='parallel_query=on/off'` 语法，否则先延后。

### Orchestrator Patch Review - 2026-06-01

审查对象：

```text
/Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-phase0-system-vars.patch
```

审查结果：
- Patch 范围符合 Phase 0：只修改 `sql/system_variables.h`、`sql/sys_vars.cc`、`sql/sql_class.h`、`sql/sql_lex.h`、`sql/sql_optimizer.h`。
- 未修改禁止文件：`sql/sql_optimizer.cc`、`sql/sql_select.cc`、`sql/handler.h`、`storage/**`、`mysql-test/**`、CMake 文件。
- 未接入执行路径，符合 skeleton-only 要求。
- 变量集合与 Phase 0 设计一致。
- `Query_block` 和 `JOIN` 字段位于 public 区域，后续 Phase 可直接使用。

需要确认的问题：
- `THD` PQ 字段当前被加入到 `THD` 类尾部的 `private:` 区域。Phase 0 可编译且当前未使用，但 Phase 1+ 若直接访问 `thd->pq_*` 会失败。

建议二选一：
- 方案 A：保持 private，后续通过 accessor 方法访问。封装更好，但 Phase 1 前需要补 getter/setter。
- 方案 B：移动到 `THD` 的 public 状态字段区域，与 `variables`、`status_var` 等会话状态相邻。后续开发更直接。

主控建议：
- Phase 0 若追求后续开发效率，建议采用方案 B，在最终接受前把 `THD` PQ 字段移动到 public 区域。
- 如果希望严格封装，则保留当前位置，但 Phase 1 开始前必须增加 accessor。

## Acceptance Checklist

- [x] 已提出 Phase 0 变量集合。
- [x] 已提出 THD 最小字段。
- [x] 已提出 Query_block 最小字段。
- [x] 已提出 JOIN 最小字段。
- [x] 已定义允许/禁止修改文件。
- [x] 已定义测试/验证计划。
- [x] 用户批准实现计划。
- [x] Code Agent 完成实现。
- [x] `ninja mysqld` 通过。
- [ ] `SHOW/SET parallel%` 验证通过（需启动 MySQL 实例后执行）。

## Current Status

Phase 0 编码完成。`ninja mysqld` 通过。待确认 `THD` 字段访问策略，并在启动 MySQL 实例后验证 `SHOW/SET parallel%` 变量可见性与可操作性。
