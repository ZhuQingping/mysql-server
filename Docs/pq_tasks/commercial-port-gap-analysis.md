# Parallel Query Commercial Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 中已商用的 Parallel Query 实现，按风险可控的方式迁移到当前 `support_parallel_query_8.0` 分支。

**Architecture:** 迁移目标从“继续补当前 V2 小功能”切换为“对齐商用主架构”。当前分支保留为 MySQL 8.0.46 适配基线和回归护栏，商用实现中的 plan clone、worker plan、Query_result_mq、ParallelScanIterator/PQblockScanIterator/PQRefIterator、Exchange_sort、资源控制和 InnoDB PQ 路径按模块分阶段迁移。

**Tech Stack:** MySQL 8.0.46 C++17、CMake/Ninja、MTR `parallel_query` suite、InnoDB handler/row 层、SQL optimizer/access path/iterator 框架。

---

## 状态

M0/M1/M2/M3/M4a 已完成，M4b-M10 任务书已启动。

本文档是商用实现平移的总差异清单和迁移计划。M3-M10 后续执行采用 Codex 主控 + 子 Agent 只读/实现后 review 的方式推进：每个阶段先按任务书实施，实施完成后启动独立 review 子 Agent 检视阶段 diff、测试证据和风险项，主控确认意见闭环后再提交。

## 背景结论

当前分支前期工作仍然有价值，但定位需要调整：

- 作为 MySQL 8.0.46 上的 PQ 适配基座；
- 作为 worker lifecycle、read view、KILL、fallback、DOP2/DOP4、MTR 的回归护栏；
- 作为商用实现迁移时的接口风险验证材料。

但如果目标切换为“平移 taurusdbondstore 商用实现”，后续不应继续优先扩展 V2-12C GROUP BY 小 shape。主线应切换为商用主架构对齐。

## 对比输入

当前仓库：

```text
/Users/zhuqingping/Work/Database/MySQL/mysql-server
branch: support_parallel_query_8.0
```

商用参考仓库：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore
```

商用参考文档：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/Docs/features_for_opensource/01_parallel_query
```

商用 patch：

```text
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0001-support-parallel-query-for-InnoDB-table.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0002-fix-mtr-test-cases-for-Dstore-engine.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0003-fix-PQ-review-comments.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0004-fix-assert-failure-in-SingleStmtTrxCommit.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0005-add-BIT-data-type-test-cases.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0006-fix-PQ-review-comments-continued.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0007-optimize-parallel-query-code.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0008-fix-crash-in-pq_clone_sj_mat_exec-for-semi-join.patch
Docs/features_for_opensource/01_parallel_query/patches/parallel_query-0009-update-PQ-result-files.patch
```

## 当前进度定位

当前分支已经完成：

- PQ 基础系统变量；
- 保守 eligibility 和 EXPLAIN/fallback/status；
- handler/InnoDB bridge；
- worker THD/TABLE/handler lifecycle；
- typed MQ row-image protocol；
- DOP1/DOP2/DOP4 threaded row stream 实验路径；
- DOP2/DOP4 range dispatch 与多 range drain；
- worker ERROR、external KILL、read-view concurrency 验证；
- DOP1/DOP2/DOP4 implicit aggregate 覆盖；
- DOP1 GROUP BY typed-state；
- DOP2/DOP4 GROUP BY partial aggregation 的单 group key、单 aggregate `COUNT/SUM/MIN/MAX` 子集；
- `parallel_query` suite 约 68 组 `.test/.result` 文件，完整 suite 曾记录 69 项通过；
- V2-12C-1 multi aggregate contract 文档。

当前分支尚不等同商用实现：

- 没有完整 worker plan clone；
- 没有完整 `ParallelScanIterator` 商用执行框架；
- 没有 `PQblockScanIterator` / `PQRefIterator`；
- 没有 `Query_result_mq`；
- 没有 `Exchange_sort`；
- 没有完整 `pq_resource_stat`；
- 没有 hash join shared context；
- 没有商用版完整 subquery / UNION / derived / dependent ref / secondary index ICP / partition 等能力；
- InnoDB PQ 集成仍与商用 `ha_innodb_pq.cc` 形态不同。

## 模块差异清单

### SQL parallel_query 目录

| Module | 当前分支 | 商用仓库 | 迁移策略 |
|---|---|---|---|
| `msg_queue.*` | 已有 typed MQ 版本 | 已有商用 MQ | 保留当前实现为基础，逐项对比商用功能；不优先整体替换 |
| `exchange.*` | 已有 `Exchange_nosort` 和 typed row-image helper | 已有 `Exchange_nosort`，并配套商用 record gather | 部分保留；补齐商用 record gather 所需接口 |
| `exchange_sort.*` | 缺失 | 存在 | 新增迁移，承接 ORDER BY/Gather Merge |
| `query_result_mq.*` | 缺失 | 存在 | 新增迁移，作为 worker result 输出主路径 |
| `pq_iterator.*` | 当前自研 `PQTableScanIterator` | 商用为 `pq_iterators.*` | 当前文件后续降级为适配/回归参考；迁移商用 `ParallelScanIterator` 架构 |
| `pq_iterators.*` | 缺失 | 存在 `ParallelScanIterator`、`PQblockScanIterator`、`PQRefIterator` | 新增迁移，是 SQL 执行主路径核心 |
| `pq_clone.*` | 缺失 | 存在 | 新增迁移，是 worker plan clone 核心 |
| `pq_clone_item.cc` | 缺失 | 存在 | 新增迁移，支持 Item tree 深拷贝 |
| `pq_resolver.*` | 缺失 | 存在 | 新增迁移，支持 clone 后 resolver |
| `pq_refix_fields_item.cc` | 缺失 | 存在 | 新增迁移，修正 clone 后 field 引用 |
| `pq_replace_base_item.cc` | 缺失 | 存在 | 新增迁移，替换 base Item |
| `pq_optimizer.*` | 已有保守 eligibility | 商用完整 eligibility / RBO / choose table | 以商用为目标重构；保留当前 fallback reason 和 MTR 护栏 |
| `sql_parallel.*` | 已有 worker/Gather scaffold 与 stats | 商用完整 make plan、worker exec、fallback、thread budget | 逐段迁移商用主流程，保留当前生命周期测试 |
| `pq_resource_stat.*` | 缺失 | 存在 | 新增迁移，承接 `parallel_max_threads` 等资源控制 |
| `explain_pq_access_path.*` | 缺失 | 存在 | 新增迁移，承接商用 EXPLAIN 展示 |
| `pq_hash_join_shared_context.*` | 缺失 | 存在 | 后置迁移，不作为第一闭环阻断 |
| `barrier.h` / `binary_heap.h` / `bloom_filter.h` / `chunk_files_wrapper.h` | 缺失 | 存在 | 按依赖迁移，`binary_heap.h` 优先服务 `Exchange_sort` |
| `pq_group_aggregate_iterator.*` | 当前新增的 MySQL 8.0.46 typed GROUP BY 子集 | 商用聚合走完整 worker plan/result path | 作为测试护栏保留；商用架构稳定后评估是否删除或改为 fallback/实验路径 |
| `pq_aggregate.*` | 当前聚合 eligibility helper | 商用聚合规则分布在 optimizer/plan | 保留为测试护栏，后续与商用规则合并 |

### SQL 核心文件

| Area | 当前分支 | 商用仓库 | 迁移策略 |
|---|---|---|---|
| `sql_class.h` THD 字段 | 已有最小 PQ 字段和 experimental vars | 商用字段更完整 | 逐字段对齐，避免一次性覆盖 8.0.46 结构 |
| `sql_lex.h` Query_block/Query_expression | 当前较保守 | 商用有 subquery suite、parallel_exec 等字段 | 迁移商用字段和方法，但保留当前 fallback reason |
| `sql_optimizer.h/.cc` | 当前 `JOIN::optimize()` 保守 hook | 商用多处 `choose_parallel_tables()` / plan rewrite 标记 | 迁移商用 hook 点，先保持默认 OFF |
| `join_optimizer/access_path.*` | 当前只接保守 factory | 商用创建 `ParallelScanIterator` / worker iterators | 分阶段迁移 access path 类型和 factory |
| `sql_select.cc` | 当前较少改动 | 商用存在 qep_tab/path 切换逻辑 | 随 `ParallelScanIterator` 迁移 |
| `opt_explain` / `explain_access_path` | 当前有基础 execution state | 商用有 PQ access path explain | 合并展示，不保留重复口径 |
| `handler.h` | 当前 handler PQ virtual API 偏适配 Parallel_reader | 商用有 `do_parallel_scan`、`pq_ref_depend`、`pq_ref_key` 等字段 | 逐字段对齐，重点保护 8.0.46 handler ABI 编译 |

### InnoDB 层

| Module | 当前分支 | 商用仓库 | 迁移策略 |
|---|---|---|---|
| `row0pread_pq.h/.cc` | 已有 8.0.46 Parallel_reader 适配路线 | 商用有完整 PQ scan/partition 实现 | 先 diff 对齐数据结构和 range 语义，再决定合并方向 |
| `ha_innodb_pq.cc` | 缺失，当前逻辑散在 `ha_innodb.cc` | 存在 | 优先迁移成独立文件，降低 `ha_innodb.cc` 冲突和后续维护成本 |
| `ha_innodb.cc` PQ hooks | 已有 `pq_leader_scan_init` 等桥接 | 商用调用独立 PQ 实现 | 将当前 hook 转为调用商用式 helper，保留 PROBE/EXECUTE 安全语义 |
| read view / trx | 当前已有针对 8.0.46 的 read-view 验证 | 商用已有成熟路径但版本不同 | 以当前已验证语义为准适配商用逻辑 |
| secondary index / ICP | 当前未打开 | 商用支持 | 后置迁移，先完成 clustered full scan 商用主路径 |
| partition table | 当前未打开 | 商用支持 | 后置迁移 |

### 系统变量与资源控制

| Variable / Area | 当前分支 | 商用仓库 | 迁移策略 |
|---|---|---|---|
| `parallel_query` | 已有 | 商用可能通过 force/RBO 控制 | 保留并对齐语义 |
| `parallel_default_dop` | 已有 | 已有，受 max threads 约束 | 对齐检查逻辑 |
| `parallel_cost_threshold` | 已有 | 已有 | 对齐成本模型 |
| `parallel_max_threads` | 缺失 | 存在 | 迁移，作为 worker resource gate |
| `parallel_rows_threshold` | 缺失 | 存在 | 迁移，作为 divided table 选择依据 |
| `parallel_tuple_cost` / `parallel_setup_cost` | 缺失 | 存在 | 迁移，服务商用 cost model |
| `parallel_graceful_fallback` | 缺失 | 存在 | 迁移，服务执行失败回退策略 |
| 当前 experimental gates | 已有 | 商用没有同形态 | 保留为过渡保护；商用主路径稳定后逐步收敛 |

### 测试套

| Area | 当前分支 | 商用仓库 | 迁移策略 |
|---|---|---|---|
| 文件规模 | 约 136 个 suite 文件 | 约 215 个 suite 文件 | 补齐缺失测试，先分类再启用 |
| V1/V2 回归 | 已有 | 商用覆盖更广 | 当前测试作为护栏，迁移后必须继续通过 |
| ORDER BY | 覆盖不足 | 商用覆盖 | 随 `Exchange_sort` 迁移 |
| ref / secondary index / ICP | 覆盖不足 | 商用覆盖 | 随 `PQRefIterator` / InnoDB 路径迁移 |
| subquery / UNION / derived | 覆盖不足 | 商用覆盖 | 随 plan clone/resolver 迁移 |
| KILL / worker error | 当前覆盖较强 | 商用也有 | 双方用例合并 |

## 当前代码分类

### 保留并作为迁移基座

- `Docs/pq_tasks/*` 看板和阶段文档；
- 当前 `parallel_query` MTR 中已验证的变量、fallback、EXPLAIN、KILL、worker error、read-view、DOP2/DOP4 回归；
- `PQ_leader_scan_mode::PROBE/EXECUTE` 安全边界；
- worker THD/TABLE/handler lifecycle helper；
- `Exchange_nosort` 中已经验证的 typed ROW/FINISH/ERROR materialization helper；
- DOP range dispatch 和 multi-range drain 中已经修复过的 8.0.46 适配点；
- 当前 InnoDB read-view 并发验证和 KILL cleanup 验证。

### 倾向被商用主架构替换或重构

- `PQTableScanIterator` 作为主执行入口；
- 当前 `TryCreatePQTableScanIterator()` 保守 factory；
- 当前 `pq_optimizer.*` 中的极保守 eligibility 主流程；
- 当前 `sql_parallel.*` 中仅服务 smoke/typed row 的部分 helper；
- 当前 `pq_group_aggregate_iterator.*` 作为主 GROUP BY 方向。

这些代码不应立刻删除。迁移阶段应先并存，商用路径默认 OFF 或受新 gate 控制，通过 MTR 后再决定收敛。

### 只作为测试护栏

- V2-8J/K/L/M/N/O/P/Q threaded shadow 和 experimental gate 测试；
- V2-9 range correctness 测试；
- V2-11 regression expansion；
- V2-12A/B GROUP BY partial aggregation 测试；
- 当前 status counter contract 测试；
- external KILL、worker ERROR、read-view concurrency 用例。

这些用例的价值是防止商用代码迁移后破坏 8.0.46 上已经收敛过的生命周期和一致性问题。

## 迁移总原则

- 默认 OFF，新增商用主路径必须由显式 gate 打开；
- 每个模块先编译通过，再接入执行路径；
- 每个执行路径先 RED MTR，再启用最小代码；
- worker started 后不允许 silent serial fallback；
- 8.0.46 已验证的 read-view / KILL / cleanup 语义优先级高于直接照搬；
- 不在同一个 commit 混合 SQL plan clone、InnoDB scan、ORDER BY、测试大迁移；
- patch 文件、构建产物、日志文件不提交；
- 每个阶段提交只包含任务相关文件。

## 迁移阶段计划

### M3-M10 启动与协作约束

**当前启动状态：**

- M3-M10 独立任务书已生成；
- M3-M6 保持串行，避免同时改动 plan clone、worker result、handler/InnoDB、full scan gate 这条同一执行链；
- M7-M10 在 M6 通过后再决定是否并行，其中 M10 的测试 manifest 分类可提前只读维护，但测试启用必须跟随能力 gate；
- 每个阶段完成代码后必须启动一个 review 子 Agent；review 只检查本阶段 diff、任务书验收项、构建/MTR 证据和与商用参考实现的偏差；
- review 意见闭环前不提交源码。

**已确认点：**

- M3 不直接平移完整商用 `pq_make_join()`；当前分支缺少完整 `Item::pq_clone()` / `refix_fields()` / `pq_restore()` 虚函数契约，第一步只做 clone activation probe、诊断和 worker-start 前 fallback。
- M4 拆分为 MQ wire protocol contract 与 worker `Query_result_mq` result path 两段；不得混用当前 typed row-image header 与商用 `Field_raw_data` / NULL bitmap / CONST bitmap 协议。
- M5 拆分为 handler contract alignment、`ha_innodb_pq.cc` 文件拆分、clustered full-scan row producer alignment；保留当前 `PROBE` / `EXECUTE` 安全语义。
- M6 默认 OFF，只打开单表 clustered full scan 的最小 gate；BLOB/TEXT/JSON/GEOMETRY、ORDER/GROUP、ref/ICP、partition 等 shape 先 fallback。
- M7 aggregation 必须保证 legacy typed-state path 与 commercial aggregation path 互斥，不能两个路径同时声称同一 query shape executed。
- M8 先做 `binary_heap.h` / `exchange_sort.*` compile-only 和 synthetic smoke，再接真实 ORDER BY。
- M9 先补 secondary/ref/ICP fallback 和负向测试，再打开正向 ref/range/ICP。
- M10 维护 enabled/adapted/deferred manifest；deferred 必须写明缺失能力，不删除测试意图。

**任务书入口：**

- [commercial-port-m3-plan-clone-resolver.md](commercial-port-m3-plan-clone-resolver.md)
- [commercial-port-m4-worker-result-path.md](commercial-port-m4-worker-result-path.md)
- [commercial-port-m5-innodb-path-alignment.md](commercial-port-m5-innodb-path-alignment.md)
- [commercial-port-m6-fullscan-execution-gate.md](commercial-port-m6-fullscan-execution-gate.md)
- [commercial-port-m7-aggregation-reconciliation.md](commercial-port-m7-aggregation-reconciliation.md)
- [commercial-port-m8-order-by-gather-merge.md](commercial-port-m8-order-by-gather-merge.md)
- [commercial-port-m9-ref-icp.md](commercial-port-m9-ref-icp.md)
- [commercial-port-m10-test-suite-gap-closure.md](commercial-port-m10-test-suite-gap-closure.md)

### Task M0: 固化商用差异清单

**Files:**

- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`
- Modify: `Docs/pq_tasks/README.md`

- [x] **Step 1: 统计当前与商用模块清单**

Run:

```bash
find sql/parallel_query storage/innobase -maxdepth 3 -type f 2>/dev/null | rg 'parallel_query|pq|pread|handler/ha_innodb_pq|row0pread' | sort
```

Expected:

```text
当前分支少于商用仓库，商用额外包含 pq_clone、pq_iterators、query_result_mq、exchange_sort、pq_resource_stat、ha_innodb_pq 等模块。
```

- [x] **Step 2: 统计测试套规模**

Run:

```bash
find mysql-test/suite/parallel_query -type f 2>/dev/null | wc -l
find /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/mysql-test/suite/parallel_query -type f 2>/dev/null | wc -l
```

Expected:

```text
当前约 136 个 suite 文件，商用约 215 个 suite 文件。
```

- [x] **Step 3: 生成本文档**

Run:

```bash
git diff --check -- Docs/pq_tasks/commercial-port-gap-analysis.md Docs/pq_tasks/README.md
```

Expected:

```text
无 trailing whitespace 报错。
```

### Task M1: Commercial Core Skeleton Port

**Goal:** 先迁移商用 SQL 层核心文件骨架，让代码可编译但不接执行路径。

**Status:** Completed as compile-only skeleton. 商用文件边界已进入当前分支构建，深层实现按 fail-closed stub 保留，核心 SQL 类扩展和执行路径接入后置。

Detailed taskbook:

- [commercial-port-m1-core-skeleton.md](commercial-port-m1-core-skeleton.md)

**Files:**

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
- Modify: `sql/parallel_query/CMakeLists.txt` or parent build list used by current tree
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 第一段只允许新增文件和 build glue；
- 函数可以先保持未接主路径，但必须能编译；
- 不替换当前 `PQTableScanIterator`；
- 不修改 InnoDB 行为；
- 不启用商用执行路径。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
```

Expected:

```text
mysqld build passes.
```

M1 actual validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m1_vars --tmpdir=/tmp/pqt_m1_vars
```

Result:

```text
both passed
```

`pq_variables` exists in the commercial reference suite, but the current branch test is named `pq_vars`; commercial test name alignment is deferred to M10.

### Task M2: Commercial Iterator Access Path Skeleton

**Goal:** 迁移 `ParallelScanIterator` / `PQblockScanIterator` / `PQRefIterator` 类型和 access path 创建骨架，但默认不执行。

**Status:** Completed as compile-only skeleton. 商用 iterator/access path 类型已进入构建，默认不可达，所有新 iterator `Init()` fail-closed。

**Files:**

- Create: `sql/parallel_query/pq_iterators.h`
- Create: `sql/parallel_query/pq_iterators.cc`
- Modify: `sql/join_optimizer/access_path.h`
- Modify: `sql/join_optimizer/access_path.cc`
- Modify: `sql/join_optimizer/explain_access_path.cc`
- Modify: `sql/join_optimizer/join_optimizer.cc`
- Modify: `sql/join_optimizer/walk_access_paths.h`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 新 access path 必须默认不可达；
- 先保留当前 `TryCreatePQTableScanIterator()`；
- 禁止一次性打开 `PQRefIterator`；
- `PQblockScanIterator` 的 handler 调用必须先受 gate 保护；
- EXPLAIN 文案先只显示 candidate，不声称 executed。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_variables pq_explain_basic --parallel=1 --vardir=/tmp/pqv_m2 --tmpdir=/tmp/pqt_m2
```

Expected:

```text
build passes; existing basic PQ tests pass; no default behavior change.
```

M2 actual validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m2_vars --tmpdir=/tmp/pqt_m2_vars
cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m2_full --tmpdir=/tmp/pqt_m2_full
```

Result:

```text
all passed; full current parallel_query suite: 69 tests successful
```

`pq_variables pq_explain_basic` are commercial-reference names that do not exist in this branch; current branch coverage is `pq_vars` plus the full existing `parallel_query` suite.

### Task M3: Commercial Plan Clone And Resolver Activation

**Goal:** 接入 `pq_make_join()` 和 clone/resolver 流程，先只对最小单表 SELECT 生成 worker plan，不启动 worker。

**Files:**

- Modify: `sql/sql_class.h`
- Modify: `sql/sql_lex.h`
- Modify: `sql/sql_optimizer.h`
- Modify: `sql/sql_optimizer.cc`
- Modify: `sql/parallel_query/pq_clone.*`
- Modify: `sql/parallel_query/pq_resolver.*`
- Modify: `sql/parallel_query/pq_optimizer.*`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- clone 成功和失败必须有 status/diagnostic；
- clone 失败必须 worker-start 前 fallback；
- worker THD 不允许递归开启 PQ；
- 不迁移复杂 subquery 前，不打开 subquery shape；
- 使用当前 read-view/KILL 回归测试作为护栏。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m3 --tmpdir=/tmp/pqt_m3
```

Expected:

```text
完整当前 parallel_query suite 通过。
```

### Task M4: Query_result_mq And Worker Result Path

**Goal:** 接入商用 worker result 输出路径，用 `Query_result_mq` 替代当前只服务 typed row-image 的窄路径。

**Files:**

- Modify: `sql/parallel_query/query_result_mq.*`
- Modify: `sql/parallel_query/msg_queue.*`
- Modify: `sql/parallel_query/exchange.*`
- Modify: `sql/parallel_query/sql_parallel.*`
- Modify: `sql/parallel_query/pq_iterators.*`
- Create/Modify: `mysql-test/suite/parallel_query/t/pq_commercial_worker_result.test`
- Create/Modify: `mysql-test/suite/parallel_query/r/pq_commercial_worker_result.result`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 先支持单表 SELECT projection；
- WHERE predicate 先沿用 worker plan 执行，不新增手写过滤；
- BLOB/复杂类型先 fallback；
- worker ERROR / leader KILL 必须复用当前 cleanup 语义。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_worker_result pq_read_threaded_external_kill --parallel=1 --vardir=/tmp/pqv_m4 --tmpdir=/tmp/pqt_m4
```

Expected:

```text
worker result path positive case passes; KILL cleanup still passes.
```

### Task M5: InnoDB Commercial PQ Path Alignment

Status: Completed on 2026-06-19. Current InnoDB PQ handler methods were split
from `ha_innodb.cc` into `handler/ha_innodb_pq.cc`; the MySQL 8.0.46
`PQ_Leader_context` / `PQ_Worker_context` bridge remains in place. M5 did not
open secondary index, ICP, partition, or ref paths.

**Goal:** 迁移商用 `ha_innodb_pq.cc` 形态，把当前散落在 `ha_innodb.cc` 的 PQ 逻辑收敛为独立实现文件。

**Files:**

- Create: `storage/innobase/handler/ha_innodb_pq.cc`
- Modify: `storage/innobase/handler/ha_innodb.cc`
- Modify: `storage/innobase/include/row0pread_pq.h`
- Modify: `storage/innobase/row/row0pread_pq.cc`
- Modify: `storage/innobase/CMakeLists.txt` or relevant build list
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 保留当前 `PROBE` fallback-safe / `EXECUTE` commit-point 语义；
- clustered full scan 先完成；
- secondary index、ICP、partition 不在本阶段打开；
- read-view concurrency MTR 必须继续通过。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_view_dop2 pq_read_threaded_external_kill --parallel=1 --vardir=/tmp/pqv_m5 --tmpdir=/tmp/pqt_m5
```

Expected:

```text
InnoDB PQ path build passes; read-view and KILL tests pass.
```

### Task M6: Commercial Full Scan Execution Gate

Status: Completed on 2026-06-19. The existing explicit DOP2/DOP4 threaded
gates now have a commercial-port M6 acceptance test covering default OFF,
`SELECT *`, simple projection/WHERE, DOP4, and TEXT/BLOB safe fallback before
worker launch.

**Goal:** 用商用主路径跑通单表 clustered full scan 的 DOP2/DOP4 查询。

**Files:**

- Modify: `sql/parallel_query/pq_optimizer.*`
- Modify: `sql/parallel_query/sql_parallel.*`
- Modify: `sql/parallel_query/pq_iterators.*`
- Modify: `storage/innobase/handler/ha_innodb_pq.cc`
- Create/Modify: `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- Create/Modify: `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 新 gate 默认 OFF；
- 支持 `SELECT * FROM t` 和简单 projection；
- WHERE 可随 worker plan 执行；
- LIMIT 无 ORDER BY 先按商用规则处理或 fallback；
- 结果必须与串行一致。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_fullscan --parallel=1 --vardir=/tmp/pqv_m6 --tmpdir=/tmp/pqt_m6
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m6_full --tmpdir=/tmp/pqt_m6_full
```

Expected:

```text
commercial full scan gate passes targeted and current full suite.
```

### Task M7: Aggregation Strategy Reconciliation

**Goal:** 决定当前 V2-12 GROUP BY typed-state 子集与商用 aggregation path 的关系。

**Files:**

- Modify: `sql/parallel_query/pq_group_aggregate_iterator.*`
- Modify: `sql/parallel_query/pq_aggregate.*`
- Modify: `sql/parallel_query/pq_optimizer.*`
- Modify: `sql/parallel_query/sql_parallel.*`
- Modify: `mysql-test/suite/parallel_query/t/*group*`
- Modify: `mysql-test/suite/parallel_query/r/*group*`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 若商用 aggregation path 能覆盖当前 V2-12A/B，则当前 typed-state path 转为 fallback-only 或删除；
- 若商用 path 迁移成本过高，则当前 typed-state path 临时保留；
- 不允许两个路径同时声称同一 query shape executed；
- status counters 必须区分 commercial path 和 legacy typed-state path。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m7 --tmpdir=/tmp/pqt_m7
```

Expected:

```text
GROUP BY existing tests pass; commercial aggregation direction documented.
```

### Task M8: ORDER BY Gather Merge

**Goal:** 迁移 `Exchange_sort` 和 ORDER BY/Gather Merge 测试。

**Files:**

- Create: `sql/parallel_query/exchange_sort.h`
- Create: `sql/parallel_query/exchange_sort.cc`
- Create: `sql/parallel_query/binary_heap.h`
- Modify: `sql/parallel_query/exchange.*`
- Modify: `sql/parallel_query/sql_parallel.*`
- Modify: `sql/parallel_query/pq_iterators.*`
- Create/Modify: `mysql-test/suite/parallel_query/t/pq_commercial_order_by.test`
- Create/Modify: `mysql-test/suite/parallel_query/r/pq_commercial_order_by.result`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 先支持 ORDER BY group/key 简单场景；
- 不支持 shape 必须 fallback；
- 输出顺序必须由 MTR 验证。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_order_by --parallel=1 --vardir=/tmp/pqv_m8 --tmpdir=/tmp/pqt_m8
```

Expected:

```text
ORDER BY commercial path passes targeted tests.
```

### Task M9: Secondary Index / Ref / ICP

**Goal:** 迁移 `PQRefIterator`、secondary index、ICP 能力。

**Files:**

- Modify: `sql/parallel_query/pq_iterators.*`
- Modify: `sql/handler.h`
- Modify: `storage/innobase/handler/ha_innodb_pq.cc`
- Modify: `storage/innobase/row/row0pread_pq.cc`
- Create/Modify: `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- Create/Modify: `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 先支持 ref/range 的商用最小正例；
- ICP 打开前必须有负向 fallback 测试；
- MVI/unique filter 逻辑按商用实现迁移并单测。

**Validation:**

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp --parallel=1 --vardir=/tmp/pqv_m9 --tmpdir=/tmp/pqt_m9
```

Expected:

```text
ref/ICP targeted tests pass.
```

### Task M10: Commercial Test Suite Gap Closure

**Goal:** 补齐商用 `parallel_query` suite 缺失测试，按功能分类启用。

**Files:**

- Modify/Create: `mysql-test/suite/parallel_query/t/*`
- Modify/Create: `mysql-test/suite/parallel_query/r/*`
- Modify: `Docs/pq_tasks/parallel_query_suite_manifest.md`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`

**Rules:**

- 每个商用测试必须分类为 enabled / adapted / deferred；
- deferred 必须写明对应缺失能力；
- 不因当前未迁移能力而删除测试意图；
- 完整 suite 是最终验收。

**Validation:**

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m10 --tmpdir=/tmp/pqt_m10
```

Expected:

```text
已启用测试全部通过；deferred 测试在 manifest 中有明确理由。
```

## 推荐执行顺序

严格串行：

1. M1 Commercial Core Skeleton Port；
2. M2 Commercial Iterator Access Path Skeleton；
3. M3 Commercial Plan Clone And Resolver Activation；已完成 clone activation probe 与 diagnostics；
4. M4 Query_result_mq And Worker Result Path；M4a worker-result MQ contract smoke 已完成，M4b 真实 worker result path 待 M5/M6 后继续；
5. M5 InnoDB Commercial PQ Path Alignment；
6. M6 Commercial Full Scan Execution Gate。

M6 通过后再分支推进：

- M7 Aggregation Strategy Reconciliation；
- M8 ORDER BY Gather Merge；
- M9 Secondary Index / Ref / ICP；
- M10 Commercial Test Suite Gap Closure。

当前执行策略：

- 立即执行 M3；
- M3 提交后继续 M4；
- M4/M5/M6 继续串行；
- M7/M8/M9/M10 在 M6 后重评依赖，优先并行化 read-only test manifest、ORDER BY compile-only 和 ref/ICP fallback 分类，真实执行路径仍按风险串行。

## 下一步任务书

下一步应执行 M3，不继续 V2-12C-2。

M3 的具体任务：

```text
角色：Code Agent
主控：Codex
任务：Commercial Plan Clone And Resolver Activation

目标：
1. 从 taurusdbondstore 调研并迁移 `pq_make_join()`、clone/resolver/refix/base item 的最小声明与执行前失败边界。
2. 只允许在 worker-start 前建立 clone contract，不启动 worker，不接 `ParallelScanIterator` 真实执行。
3. clone 失败必须 fallback；不得引入 subquery/UNION/derived/semijoin 等复杂 shape。

验证：
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m3 --tmpdir=/tmp/pqt_m3

完成：
1. 更新本文档 Completion Report；
2. 说明新增/修改文件；
3. 说明与商用源码的偏差；
4. 说明未接入主路径的保护措施；
5. 提交只包含 M3 相关文件。
```

## Acceptance Checklist

- [x] 明确当前代码哪些保留；
- [x] 明确当前代码哪些会被商用主架构替换或重构；
- [x] 明确当前测试哪些只作为护栏；
- [x] 明确商用模块缺口；
- [x] 明确迁移阶段和验证命令；
- [x] 明确下一步从 M1 开始，不继续 V2-12C-2。

## Completion Report

M0 design-only task completed. No source code was changed. M1 taskbook was created.

M1 source migration completed as a compile-only commercial skeleton:

- 商用核心文件边界和 build glue 已落位；
- `mysqld` build 通过；
- 当前分支 `parallel_query.pq_vars` smoke 通过；
- `pq_clone` / `pq_resolver` / `Query_result_mq` 的商用主体因依赖 `Item`、`THD`、`Query_block`、`JOIN`、`TABLE`、成熟 MQ wire protocol 和 worker temp-table contract，已后置到 M3/M4；
- 当前提交不改变 optimizer、access path、handler、InnoDB 或默认执行路径。

M2 source migration completed as a compile-only iterator/access path skeleton:

- `pq_iterators.*` 已新增，包含 `ParallelScanIterator` / `PQblockScanIterator` / `PQRefIterator` 类壳；
- `AccessPath::PARALLEL_SCAN` / `PQ_BLOCK_SCAN` / `PQ_REF_SCAN`、accessor、factory、iterator creation switch、EXPLAIN case、walk/debug switch 已补齐；
- 当前 `TABLE_SCAN` hook 和 `TryCreatePQTableScanIterator()` 未改变；
- 所有新增商用 iterator 默认 `Init()` fail-closed，不启动 worker，不读 MQ，不调用 handler/InnoDB；
- `mysqld` build、`pq_vars` smoke、完整当前 `parallel_query` suite 69 项通过。

M3-M10 task launch completed:

- 已生成 M3-M10 阶段任务书；
- 已基于子 Agent 只读调研结果收敛 M3-M10 的拆分边界；
- 当前确认 M3 只做 clone activation probe 与 worker-start 前 fallback，不启动 worker，不接真实 `ParallelScanIterator::Read()`；
- 后续每个源码阶段必须经过独立 review 子 Agent 检视，意见闭环后再提交。

M3 source migration completed:

- `pq_clone_activation_probe()` 已接入 guarded `TryCreatePQTableScanIterator()`；
- 新增 `Parallel_clone_probe_attempts` / `Parallel_clone_probe_fallback` / `Parallel_clone_probe_success` / `Parallel_clone_probe_unsupported`；
- 当前 probe 不调用 `pq_make_join()`，不创建 cloned JOIN，不启动 worker，不调用 handler/InnoDB；
- 为保持当前分支 V2 smoke/回归护栏，probe 后继续进入既有 `PQTableScanIterator` fallback/smoke 路径；
- `mysqld` build、`pq_clone_diagnostics`、`pq_stats`、完整当前 `parallel_query` suite 70 项通过。

M4a worker-result protocol contract completed:

- 新增 `PQ_worker_result_frame_header` / `PQ_worker_result_message_type` / `Field_raw_data` 占位 contract；
- 新增本地 synthetic `Query_result_mq` worker-result frame smoke，不接真实 `send_data()`；
- 新增 `Parallel_worker_result_smoke_rows` / `Parallel_worker_result_smoke_finishes`；
- 修复 review 发现的 worker-result frame 发送/解码 32-bit length overflow 风险；
- `mysqld` build、M4 targeted suite、完整当前 `parallel_query` suite 71 项通过；
- `exchange.cc` 旧 typed MQ helper 的 length overflow 收敛作为后续独立风险项。
