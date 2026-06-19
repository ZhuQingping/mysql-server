# M1 Commercial Core Skeleton Port Taskbook

## 状态

Completed。

本任务书已执行。M1 只完成商用核心模块边界和可编译骨架落位，不接入执行路径，不改变默认行为。

## 场景

`feature-development`

## 目标

从 `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 迁移 Parallel Query 商用 SQL 层核心骨架文件到当前 `support_parallel_query_8.0` 分支，并完成 MySQL 8.0.46 编译适配。

M1 只建立可编译骨架，不接入执行路径，不改变默认行为。

## 设计约束

- 默认 OFF，不新增可达执行路径；
- 只新增商用核心骨架文件和 build glue；
- 不替换当前 `PQTableScanIterator`；
- 不修改 InnoDB 行为；
- 不打开 `ParallelScanIterator` / `PQblockScanIterator` / `PQRefIterator`；
- 不迁移 `Exchange_sort`；
- 不接 `Query_result_mq` 到 worker 执行；
- 不修改当前 MTR 预期结果；
- worker plan clone 相关函数可以编译存在，但不能从 optimizer 主路径调用。

## 当前依据

已完成 intake 和迁移总计划：

- [commercial-port-gap-analysis.md](commercial-port-gap-analysis.md)
- [README.md](README.md)

当前分支已有 PQ 基座：

- `sql/parallel_query/exchange.*`
- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/pq_aggregate.*`
- `sql/parallel_query/pq_group_aggregate_iterator.*`
- `sql/parallel_query/pq_handler.*`
- `sql/parallel_query/pq_iterator.*`
- `sql/parallel_query/pq_optimizer.*`
- `sql/parallel_query/sql_parallel.*`

商用参考仓库额外核心文件：

- `sql/parallel_query/pq_clone.*`
- `sql/parallel_query/pq_clone_item.cc`
- `sql/parallel_query/pq_resolver.*`
- `sql/parallel_query/pq_refix_fields_item.cc`
- `sql/parallel_query/pq_replace_base_item.cc`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/pq_resource_stat.*`

## 允许修改

M1 源码阶段允许的最大边界：

- Create: `sql/parallel_query/pq_clone.h`
- Create: `sql/parallel_query/pq_clone.cc`
- Create: `sql/parallel_query/pq_clone_item.cc`
- Create: `sql/parallel_query/pq_resolver.h`
- Create: `sql/parallel_query/pq_resolver.cc`
- Create: `sql/parallel_query/pq_refix_fields_item.cc`
- Create: `sql/parallel_query/pq_replace_base_item.cc`
- Create: `sql/parallel_query/query_result_mq.h`
- Create: `sql/parallel_query/query_result_mq.cc`
- Create: `sql/parallel_query/pq_resource_stat.h`
- Create: `sql/parallel_query/pq_resource_stat.cc`
- Modify: `sql/CMakeLists.txt`
- Modify: `Docs/pq_tasks/commercial-port-m1-core-skeleton.md`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`
- Modify: `Docs/pq_tasks/README.md`

如果编译适配必须修改额外 SQL 头文件，必须先停止并记录缺口，不要扩大范围。

## 禁止修改

- `storage/innobase/**`
- `sql/handler.h`
- `sql/sql_class.h`
- `sql/sql_lex.h`
- `sql/sql_optimizer.*`
- `sql/join_optimizer/**`
- `sql/parallel_query/pq_iterator.*`
- `sql/parallel_query/pq_group_aggregate_iterator.*`
- `sql/parallel_query/sql_parallel.*`，除非只为 include/build 消除未使用依赖，且不改变执行路径
- `mysql-test/suite/parallel_query/t/*`
- `mysql-test/suite/parallel_query/r/*`
- 任何 patch、日志、构建产物、`.DS_Store`

## 参考文件

商用源文件：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone.h
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone.cc
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone_item.cc
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resolver.h
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resolver.cc
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_refix_fields_item.cc
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_replace_base_item.cc
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/query_result_mq.h
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/query_result_mq.cc
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resource_stat.h
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resource_stat.cc
```

构建参考：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/CMakeLists.txt
/Users/zhuqingping/Work/Database/MySQL/mysql-server/sql/CMakeLists.txt
```

当前分支构建接入点：

```text
sql/CMakeLists.txt
```

当前分支已有 PQ 源码在 `sql/CMakeLists.txt` 中的相邻区域：

```text
parallel_query/exchange.cc
parallel_query/msg_queue.cc
parallel_query/pq_aggregate.cc
parallel_query/pq_group_aggregate_iterator.cc
parallel_query/pq_handler.cc
parallel_query/pq_iterator.cc
parallel_query/pq_optimizer.cc
parallel_query/sql_parallel.cc
```

## 实施步骤

### Step 1: 记录基线

Run:

```bash
git log --oneline -5
git status --short
```

Expected:

```text
确认基线提交包含 commercial port 文档，且只处理 M1 允许文件。
```

### Step 2: 复制商用骨架文件

Run:

```bash
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone.h sql/parallel_query/pq_clone.h
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone.cc sql/parallel_query/pq_clone.cc
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone_item.cc sql/parallel_query/pq_clone_item.cc
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resolver.h sql/parallel_query/pq_resolver.h
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resolver.cc sql/parallel_query/pq_resolver.cc
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_refix_fields_item.cc sql/parallel_query/pq_refix_fields_item.cc
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_replace_base_item.cc sql/parallel_query/pq_replace_base_item.cc
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/query_result_mq.h sql/parallel_query/query_result_mq.h
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/query_result_mq.cc sql/parallel_query/query_result_mq.cc
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resource_stat.h sql/parallel_query/pq_resource_stat.h
cp /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resource_stat.cc sql/parallel_query/pq_resource_stat.cc
```

Expected:

```text
11 个新文件出现在 sql/parallel_query/。
```

### Step 3: 修正明显 include 路径差异

检查：

```bash
rg -n "\"include/|securec.h|sql_plan_cache|is_clone_for_plan_cache|pq_hash_join|pq_iterators|exchange_sort|parallel_query/explain_pq_access_path\"" sql/parallel_query/pq_clone.* sql/parallel_query/pq_clone_item.cc sql/parallel_query/pq_resolver.* sql/parallel_query/pq_refix_fields_item.cc sql/parallel_query/pq_replace_base_item.cc sql/parallel_query/query_result_mq.* sql/parallel_query/pq_resource_stat.*
```

处理规则：

- `include/my_dbug.h` 改为当前仓库已有 include 风格；
- `include/scope_guard.h` 改为当前仓库已有 include 风格；
- `securec.h` 不存在时，改用标准 C/C++ 或当前仓库等价 helper；
- 引用尚未迁移的 `pq_hash_join_shared_context.*`、`pq_iterators.*`、`exchange_sort.*` 时，不引入这些模块，先删除未使用 include 或用前向声明隔离；
- 任何需要修改 `Item`、`Query_block`、`JOIN` 类声明的函数，先记录为 compile blocker，不在 M1 私自扩展核心类。

Expected:

```text
新文件只依赖当前仓库已有头文件，或依赖 M1 新增头文件。
```

### Step 4: 接入 build glue

修改 `sql/CMakeLists.txt`，在现有 `parallel_query/*` 源文件附近加入：

```cmake
  parallel_query/pq_clone.cc
  parallel_query/pq_clone_item.cc
  parallel_query/pq_resolver.cc
  parallel_query/pq_refix_fields_item.cc
  parallel_query/pq_replace_base_item.cc
  parallel_query/pq_resource_stat.cc
  parallel_query/query_result_mq.cc
```

不加入：

```cmake
parallel_query/pq_iterators.cc
parallel_query/exchange_sort.cc
parallel_query/explain_pq_access_path.cc
parallel_query/pq_hash_join_shared_context.cc
```

Expected:

```text
只有 M1 范围内的新源文件参与构建。
```

### Step 5: 首轮编译

Run:

```bash
cmake --build build-ninja --target mysqld -j 16
```

Expected:

```text
编译失败时只修 M1 允许文件内的 8.0.46 API 差异。
```

### Step 6: 收敛编译适配

允许修复类型：

- include 路径；
- 未使用商用 helper 的静态函数；
- `override` 签名差异；
- 当前 8.0.46 类型名差异；
- namespace 或 forward declaration；
- 当前阶段不可接入函数改为局部未调用实现，但不能伪造成功路径。

禁止修复类型：

- 给 `Item` / `JOIN` / `Query_block` / `THD` 增加大量商用字段；
- 接入 optimizer hook；
- 接入 worker execution；
- 修改 handler/InnoDB；
- 修改 MTR 结果。

如果遇到禁止修复类型，停止并把 blocker 写入本文档 Completion Report。

### Step 7: 复跑编译

Run:

```bash
cmake --build build-ninja --target mysqld -j 16
```

Expected:

```text
mysqld target build passes.
```

### Step 8: 做默认行为 smoke

如果 build 通过，运行：

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_variables --parallel=1 --vardir=/tmp/pqv_m1_vars --tmpdir=/tmp/pqt_m1_vars
```

Expected:

```text
pq_variables 通过，说明默认变量和基础 suite 未被 M1 骨架破坏。
```

### Step 9: 更新 M1 Completion Report

在本文档末尾写入：

```text
Changed files:
Build result:
MTR result:
Commercial source deviations:
Blocked APIs:
Execution path status:
Risk notes:
Next suggested task:
```

要求：

- `Execution path status` 必须写明 M1 未接主路径；
- `Commercial source deviations` 必须说明与 taurusdbondstore 源文件的差异；
- 如果没有跑 MTR，必须写明原因。

### Step 10: 提交

仅在 Codex 主控 review 后提交。

建议提交范围：

```bash
git add \
  sql/parallel_query/pq_clone.h \
  sql/parallel_query/pq_clone.cc \
  sql/parallel_query/pq_clone_item.cc \
  sql/parallel_query/pq_resolver.h \
  sql/parallel_query/pq_resolver.cc \
  sql/parallel_query/pq_refix_fields_item.cc \
  sql/parallel_query/pq_replace_base_item.cc \
  sql/parallel_query/query_result_mq.h \
  sql/parallel_query/query_result_mq.cc \
  sql/parallel_query/pq_resource_stat.h \
  sql/parallel_query/pq_resource_stat.cc \
  sql/CMakeLists.txt \
  Docs/pq_tasks/commercial-port-m1-core-skeleton.md \
  Docs/pq_tasks/commercial-port-gap-analysis.md \
  Docs/pq_tasks/README.md
git diff --cached --check
git commit --only \
  sql/parallel_query/pq_clone.h \
  sql/parallel_query/pq_clone.cc \
  sql/parallel_query/pq_clone_item.cc \
  sql/parallel_query/pq_resolver.h \
  sql/parallel_query/pq_resolver.cc \
  sql/parallel_query/pq_refix_fields_item.cc \
  sql/parallel_query/pq_replace_base_item.cc \
  sql/parallel_query/query_result_mq.h \
  sql/parallel_query/query_result_mq.cc \
  sql/parallel_query/pq_resource_stat.h \
  sql/parallel_query/pq_resource_stat.cc \
  sql/CMakeLists.txt \
  Docs/pq_tasks/commercial-port-m1-core-skeleton.md \
  Docs/pq_tasks/commercial-port-gap-analysis.md \
  Docs/pq_tasks/README.md \
  -m "Port PQ commercial core skeleton"
```

## Agent Task Prompt

```text
Read first:
  - AGENTS.md
  - CLAUDE.md
  - /Users/zhuqingping/Work/Database/MySQL/mysql_ai_workflow/README.md
  - /Users/zhuqingping/Work/Database/MySQL/mysql_ai_workflow/scenarios/feature-development.md
  - /Users/zhuqingping/Work/Database/MySQL/mysql_ai_workflow/profiles/mysql-kernel/README.md
  - Docs/pq_tasks/README.md
  - Docs/pq_tasks/commercial-port-gap-analysis.md
  - Docs/pq_tasks/commercial-port-m1-core-skeleton.md

Role:
  Code Agent

Task:
  M1 Commercial Core Skeleton Port

Goal:
  从 taurusdbondstore 迁移商用 SQL 层核心骨架文件，并完成 MySQL 8.0.46 编译适配。不要接入执行路径。

Context:
  当前分支已有保守 PQ 适配基座。M1 是商用主架构迁移的第一个源码阶段，只允许新增骨架和 build glue。

Allowed files:
  - sql/parallel_query/pq_clone.h
  - sql/parallel_query/pq_clone.cc
  - sql/parallel_query/pq_clone_item.cc
  - sql/parallel_query/pq_resolver.h
  - sql/parallel_query/pq_resolver.cc
  - sql/parallel_query/pq_refix_fields_item.cc
  - sql/parallel_query/pq_replace_base_item.cc
  - sql/parallel_query/query_result_mq.h
  - sql/parallel_query/query_result_mq.cc
  - sql/parallel_query/pq_resource_stat.h
  - sql/parallel_query/pq_resource_stat.cc
  - sql/CMakeLists.txt
  - Docs/pq_tasks/commercial-port-m1-core-skeleton.md
  - Docs/pq_tasks/commercial-port-gap-analysis.md
  - Docs/pq_tasks/README.md

Forbidden files:
  - storage/innobase/**
  - sql/handler.h
  - sql/sql_class.h
  - sql/sql_lex.h
  - sql/sql_optimizer.*
  - sql/join_optimizer/**
  - sql/parallel_query/pq_iterator.*
  - sql/parallel_query/pq_group_aggregate_iterator.*
  - mysql-test/suite/parallel_query/t/*
  - mysql-test/suite/parallel_query/r/*

Reference files:
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone.*
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_clone_item.cc
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resolver.*
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_refix_fields_item.cc
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_replace_base_item.cc
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/query_result_mq.*
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_resource_stat.*
  - /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/CMakeLists.txt

Required commands:
  - cmake --build build-ninja --target mysqld -j 16
  - cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m1_vars --tmpdir=/tmp/pqt_m1_vars

Patch output:
  - Not needed when Codex implements directly in main worktree.

Code-edit permission:
  GRANTED only when the human explicitly asks to start M1 source migration. This taskbook creation does not grant source edit permission.

Expected output:
  - changed files
  - base commit
  - summary
  - commands run
  - results
  - commercial source deviations
  - blocked APIs
  - risks
  - next suggested task

Stop conditions:
  - needs edits outside allowed files
  - requires THD/JOIN/Query_block/Item class API expansion
  - requires InnoDB or handler changes
  - build failure points to M2/M3 dependency
  - execution path would become reachable

Commit policy:
  Do not commit until Codex Orchestrator reviews the diff.
```

## Completion Report

M1 source migration completed as a compile-only commercial skeleton.

### Changed files

- Added commercial module headers:
  - `sql/parallel_query/pq_clone.h`
  - `sql/parallel_query/pq_resolver.h`
  - `sql/parallel_query/query_result_mq.h`
  - `sql/parallel_query/pq_resource_stat.h`
- Added compile-only implementation boundaries:
  - `sql/parallel_query/pq_clone.cc`
  - `sql/parallel_query/pq_clone_item.cc`
  - `sql/parallel_query/pq_resolver.cc`
  - `sql/parallel_query/pq_refix_fields_item.cc`
  - `sql/parallel_query/pq_replace_base_item.cc`
  - `sql/parallel_query/query_result_mq.cc`
  - `sql/parallel_query/pq_resource_stat.cc`
- Updated build glue:
  - `sql/CMakeLists.txt`

### Commercial source deviations

The files were first copied from `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore`, then narrowed to M1-safe stubs where the commercial body requires broad core SQL class API expansion.

Deferred commercial bodies:

- `pq_clone.cc`
- `pq_clone_item.cc`
- `pq_resolver.cc`
- `pq_refix_fields_item.cc`
- `pq_replace_base_item.cc`
- `query_result_mq.cc`

Reason:

- Commercial clone/resolver code depends on `Item::pq_clone`, `Item::pq_copy_from`, `Item::refix_fields`, `Query_block::pq_backup`, `Query_block::pq_restore`, `Query_block::record_map_order`, `TABLE::pq_saved_const_table`, and related `JOIN`/`Query_block` state.
- Commercial `Query_result_mq` depends on the mature MQ wire protocol: `Field_raw_data`, `Batch_buffer`, `MQueue_handle::send(Field_raw_data *)`, worker temp table fields, and `JOIN::make_worker_tmp_table()`.
- These contracts belong to later migration stages and are intentionally not introduced in M1.

### Protection

- No optimizer hook was added.
- No access path factory was changed.
- No InnoDB or handler code was changed.
- `Query_result_mq::send_data()` returns error if called, so the skeleton cannot silently claim a successful worker result path.
- Clone/resolver helper stubs return conservative failure or `nullptr` values and are not reachable from the current execution path.

### Commands run

```bash
cmake --build build-ninja --target mysqld -j 16
```

Result:

```text
passed
```

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_variables --parallel=1 --vardir=/tmp/pqv_m1_vars --tmpdir=/tmp/pqt_m1_vars
```

Result:

```text
failed before running tests: current branch does not contain pq_variables
```

Replacement smoke test:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m1_vars --tmpdir=/tmp/pqt_m1_vars
```

Result:

```text
passed
```

### Blocked APIs for later phases

- M3: `Item` clone/copy/refix and `Query_block` backup/restore/base-ref contracts.
- M4: commercial `Query_result_mq` protocol, `Field_raw_data`, `Batch_buffer`, worker temp table output path.
- M5/M6: commercial InnoDB PQ scan path and execution gate.

### Next suggested task

Proceed to M2 Commercial Iterator Access Path Skeleton, still default OFF and unreachable.
