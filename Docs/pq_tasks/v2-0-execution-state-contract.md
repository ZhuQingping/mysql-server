# V2-0 Execution State Contract 任务书

## Goal

为 V2 真实执行路径建立稳定的 PQ execution state contract，明确区分：

- optimizer candidate / eligible；
- execution iterator 是否被选中；
- PQ 是否真实执行；
- 是否发生 serial fallback；
- EXPLAIN 是否只是展示候选能力。

本阶段不启用真实并行扫描，不创建真实 worker，不接 InnoDB 分片扫描。`TryCreatePQTableScanIterator()` 仍可返回 `nullptr`，但必须把当前 V1 文案和统计语义演进为后续 V2-1 可以安全返回非空 iterator 的状态模型。

## Current Baseline

- Baseline commit: `69ed0ac66e7 Tighten PQ V1 risk boundaries`
- 当前 V1 行为：
  - `JOIN::pq_eligible` 表示 optimizer 层候选资格；
  - `TryCreatePQTableScanIterator()` 对 eligible execution 仍返回 `nullptr`，并增加 `Parallel_queries_fallback`；
  - EXPLAIN 已使用 `eligible, execution disabled, serial fallback`，但缺少结构化 state contract；
  - `THD::pq_executed` 只有布尔值，不足以表达 iterator selected / fallback / explain-only。

## Non-goals

- 不返回真实 `PQTableScanIterator`；
- 不启动 worker THD；
- 不调用 InnoDB PQ worker scan；
- 不实现 row stream / Exchange / Gather 数据流；
- 不改变 `parallel_query=OFF` 的现有行为；
- 不扩大 eligibility 范围。

## Contract Proposal

### Query-level State

建议新增轻量 enum，位置优先选择 `sql/parallel_query/sql_parallel.h`：

```c++
enum class PQ_execution_state {
  DISABLED = 0,          // parallel_query=OFF or not considered
  NOT_ELIGIBLE,          // optimizer rejected PQ
  ELIGIBLE,              // optimizer candidate, no execution iterator selected yet
  ITERATOR_SELECTED,     // execution chose PQ iterator path
  EXECUTED,              // PQ workers actually executed
  FALLBACK_SERIAL        // eligible path fell back to serial execution
};
```

V2-0 可以只用到 `ELIGIBLE` 和 `FALLBACK_SERIAL`，但 enum 必须为 V2-1/V2-8 预留真实执行状态。

### THD/JOIN Ownership

建议：

- `JOIN::pq_eligible` 继续表示 optimizer eligibility；
- `JOIN::pq_plan_rewritten` 暂不扩大语义，避免与 execution state 混用；
- `THD` 增加 per-statement execution state 字段，例如 `pq_execution_state`；
- `THD::pq_executed` 保留兼容，但由 state 派生式维护：只有 state 为 `EXECUTED` 时才为 true；
- `THD::pq_error` 仍只表示 execution error，不承载 eligibility reason。

如果直接在 `THD` 增加 enum 字段引入 include 依赖不合适，可使用 `uint` / forward-compatible integer 字段，并在 `sql_parallel.h/.cc` 提供 helper 函数封装状态设置与字符串转换。

### State Transitions

V2-0 期望状态转移：

```text
parallel_query=OFF:
  DISABLED

optimizer rejected:
  NOT_ELIGIBLE

optimizer eligible + EXPLAIN:
  ELIGIBLE
  不增加 Parallel_queries_fallback
  不设置 pq_executed=true

optimizer eligible + normal execution + V2-0 still no real iterator:
  ELIGIBLE -> FALLBACK_SERIAL
  Parallel_queries_fallback + 1
  pq_executed=false

future V2-1/V2-8:
  ELIGIBLE -> ITERATOR_SELECTED -> EXECUTED
  或
  ELIGIBLE -> ITERATOR_SELECTED -> FALLBACK_SERIAL
```

### EXPLAIN Semantics

Traditional EXPLAIN Extra：

- eligible but not executed: `Parallel query eligible, execution disabled, serial fallback, dop=N`
- future selected/executed state 可以扩展为 `Parallel query executed, dop=N`，但 V2-0 不应输出 executed。

TREE/JSON EXPLAIN：

- JSON `parallel_query` 不应是 boolean `true`；
- 当前建议继续输出字符串 label；
- 如新增 state，可加入 `parallel_query_state: "eligible"` 或类似字段，但不能破坏现有 V1 result 的稳定性，除非同步更新 MTR。

## Allowed Files

- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_optimizer.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/sql_class.h`
- `sql/sql_optimizer.h`
- `sql/opt_explain.cc`
- `sql/join_optimizer/explain_access_path.cc`
- `mysql-test/suite/parallel_query/t/pq_explain_eligible.test`
- `mysql-test/suite/parallel_query/r/pq_explain_eligible.result`
- `mysql-test/suite/parallel_query/t/pq_stats.test`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- 可新增：`mysql-test/suite/parallel_query/t/pq_explain_state_contract.test`
- 可新增：`mysql-test/suite/parallel_query/r/pq_explain_state_contract.result`
- `Docs/pq_tasks/v2-0-execution-state-contract.md`

## Forbidden Files

- `storage/innobase/**`
- worker lifecycle 真实线程实现；
- handler/InnoDB PQ API 行为改动；
- 大范围 optimizer / executor 重构；
- 修改与 V2-0 state contract 无关的 MTR 结果。

## Implementation Tasks

### Task 1: 定义 PQ execution state

1. 在 `sql/parallel_query/sql_parallel.h` 定义 `PQ_execution_state`。
2. 添加 `const char *pq_execution_state_to_string(PQ_execution_state state)`，实现在 `sql_parallel.cc`。
3. 在 `THD` 中新增当前 statement 的 PQ execution state 字段，默认值必须是 `DISABLED` 或 `NOT_ELIGIBLE` 中更保守的一种。
4. 保持 `THD::pq_executed` 兼容，不能把 explain-only 或 fallback serial 标为 executed。

### Task 2: 在 eligibility / iterator 边界设置状态

1. `parallel_query=OFF` 或 optimizer reject 后，state 不应显示 actual execution。
2. optimizer eligible 后，设置或保持 `ELIGIBLE`。
3. `TryCreatePQTableScanIterator()` 的非 EXPLAIN fallback 处设置 `FALLBACK_SERIAL`，并只在这里增加 `Parallel_queries_fallback`。
4. EXPLAIN 路径只能保持 candidate state，不能增加 fallback counter。
5. 不要为了设置状态而改变 `TryCreatePQTableScanIterator()` 返回值。

### Task 3: EXPLAIN 输出绑定 state contract

1. Traditional EXPLAIN 继续显示 V1 稳定文案，不暗示真实执行。
2. TREE/JSON EXPLAIN 继续避免 boolean `parallel_query=true`。
3. 如果新增 JSON 字段，字段值必须来自 `pq_execution_state_to_string()` 或同源 helper，避免多个字符串来源。

### Task 4: MTR 验收

必须至少覆盖：

1. `EXPLAIN SELECT * FROM t` 显示 eligible/fallback-disabled 文案，但不增加 `Parallel_queries_fallback`。
2. 普通 `SELECT * FROM t` 在 V2-0 仍走 serial fallback，并增加 `Parallel_queries_fallback`。
3. `parallel_query=OFF` 时 EXPLAIN 无 PQ annotation，执行不增加 PQ counter。
4. `EXPLAIN FORMAT=JSON` / `TREE` 不把 PQ candidate 表示成 actual executed。

优先复用 `pq_explain_eligible` 和 `pq_stats`；如果 result 过于拥挤，可新增 `pq_explain_state_contract`。

## Validation

从 build tree 执行：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt pq_explain_eligible pq_stats
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

如果本机遇到 socket path too long，可使用主控已验证过的 extern mysqld 流程，并在完成报告中写明。

## Claude Code Task Prompt

```text
请先阅读 AGENTS.md，并遵守其中指向的唯一权威上下文 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 V2-0 Execution State Contract。

请确认 baseline：
git log --oneline -5
git status --short

请阅读：
- CLAUDE.md
- Docs/pq_tasks/README.md
- Docs/pq_tasks/v2-execution-path-roadmap.md
- Docs/pq_tasks/v2-test-matrix.md
- Docs/pq_tasks/v2-0-execution-state-contract.md
- sql/parallel_query/sql_parallel.h
- sql/parallel_query/sql_parallel.cc
- sql/parallel_query/pq_iterator.cc
- sql/opt_explain.cc
- sql/join_optimizer/explain_access_path.cc
- sql/sql_class.h

任务目标：
1. 定义 PQ execution state contract；
2. 在 optimizer eligible、EXPLAIN、iterator fallback 边界维护状态；
3. 确保 V2-0 不启用真实并行执行；
4. 更新或新增最小 MTR，验证 EXPLAIN/counter/state 语义；
5. 完成后写 Completion Report，并生成 patch 到主仓库。

允许修改：
- sql/parallel_query/sql_parallel.h
- sql/parallel_query/sql_parallel.cc
- sql/parallel_query/pq_iterator.cc
- sql/parallel_query/pq_iterator.h
- sql/parallel_query/pq_optimizer.h
- sql/parallel_query/pq_optimizer.cc
- sql/sql_class.h
- sql/sql_optimizer.h
- sql/opt_explain.cc
- sql/join_optimizer/explain_access_path.cc
- mysql-test/suite/parallel_query/**
- Docs/pq_tasks/v2-0-execution-state-contract.md

禁止修改：
- storage/innobase/**
- 启用真实 PQ iterator
- 启动 worker THD
- 调用真实 InnoDB PQ scan
- 提交 commit
- 修改无关文件或历史 patch/log 文件

验证：
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt pq_explain_eligible pq_stats
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt

完成后：
1. 把完成报告追加到 Docs/pq_tasks/v2-0-execution-state-contract.md 的 Completion Report；
2. 报告 changed files、实现说明、测试结果、风险点；
3. 在主仓库生成 patch：
   git diff > /Users/zhuqingping/Work/Database/MySQL/mysql-server/claude-v2-0-execution-state-contract.patch
4. 不要 commit。
```

## Completion Report

等待 Code Agent 填写。

## Acceptance Checklist

- [ ] 定义了单一来源的 PQ execution state；
- [ ] EXPLAIN candidate 与 actual execution 语义清晰；
- [ ] EXPLAIN 不增加 `Parallel_queries_fallback`；
- [ ] V2-0 仍不启用真实 PQ iterator；
- [ ] `parallel_query=OFF` 无 PQ annotation、无 PQ counter side effect；
- [ ] `mysqld` build 通过；
- [ ] 目标 MTR 或完整 `parallel_query` suite 通过，或清楚记录环境原因。

## Current Status

- Status: Ready for Claude Code dispatch
- Owner: Codex Orchestrator -> Claude Code Code Agent
- Started: 2026-06-11
