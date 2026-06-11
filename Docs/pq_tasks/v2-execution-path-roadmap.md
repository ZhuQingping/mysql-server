# V2 真实执行路径拆分

## Goal

在 V1 scaffold/test migration 收口后，拆分 V2 真实并行执行路径。V2 的第一目标不是一次性追平 `taurusdbondstore`，而是先跑通单表 clustered full scan 的真实 PQ 闭环：

1. optimizer 判定 eligible；
2. SQL iterator 返回真实 PQ iterator；
3. leader 初始化 InnoDB 分片扫描；
4. worker 执行受控扫描；
5. worker row/result 通过 Exchange/Gather 回传；
6. leader 输出与串行一致的结果；
7. error/kill/fallback/resource cleanup 可控。

## Current V1 Boundary

- `TryCreatePQTableScanIterator()` 仍返回 `nullptr`；
- `PQTableScanIterator::Init()` / `Read()` 是 stub；
- `PQ_worker_manager` 和 `Gather_operator` 已有状态机和资源框架，但未接真实 THD/plan execution；
- InnoDB `row0pread_pq.*` 当前是 pull-row adapter scaffold，分区逻辑仍是单 range 简化；
- `parallel_query` suite 当前验证的是 V1 行为边界，不验证真实并行加速。

## Explorer Findings

### SQL Execution Explorer

Franklin 的只读调研结论：

- SQL 层入口已经放在 `TABLE_SCAN -> TryCreatePQTableScanIterator()`；
- 真实 PQ 不能直接打开，缺少：
  - 返回非空 iterator 后仍能正确 fallback 的 iterator ownership；
  - worker 侧可执行计划或最小 worker scan；
  - MQ row/record 协议；
  - `eligible / rewritten / executed / fallback` 状态分离；
  - EXPLAIN 不能继续把 candidate 误报成 actual execution。
- V2 MVP 不建议先做完整 plan clone。建议先让 `PQTableScanIterator` 成为并行 table scan iterator，输出 base row 到 leader 的 `table->record[0]`，由 leader 继续执行上层 FILTER/projection。
- fallback 只允许在 `PQTableScanIterator::Init()` 的安全窗口发生。一旦 worker 已启动或 row 已流出，错误必须作为查询错误返回，不能静默串行重跑。
- SQL 层可以先以 DOP=1 真实 worker 跑通 iterator/worker/MQ/error 语义；DOP>1 gate 由 InnoDB range partition 决定。

### InnoDB/Handler Explorer

James 的只读调研结论：

- `sql/handler.h` 当前只有上游 `parallel_scan_init/parallel_scan/parallel_scan_end` push/batch 回调接口，不是 PQ worker 所需的 pull-row 接口。
- `sql/parallel_query/pq_handler.h` 把 PQ handler API 写成契约说明，尚未正式加入 `handler.h`。
- `ha_innobase::pq_leader_scan_init/pq_worker_scan_init/pq_worker_scan_next/end` 已存在，但 leader/worker 输出参数仍为 `nullptr`，worker scan next 直接 EOF。
- `row0pread_pq.cc` 当前只创建 whole-table range，注释明确说明 DOP>1 会重复读，不满足真实并行正确性。
- 上游 `row0pread.cc` 已有真正 B+tree range partition、visibility、worker 调度和 thread budget；V2 优先方向应是复用 `Parallel_reader`，通过薄 adapter 暴露 boundary/ctx，而不是复制完整 `row0pread_pq` fork。

## V2 Phase Plan

### V2-0: Execution State Contract

目标：

- 定义 `eligible / rewritten / iterator_selected / executed / fallback` 状态；
- 修正 EXPLAIN，不把 `eligible` 误报成真实 parallel execution；
- 为 V1 风险收敛和 V2 激活 iterator 建立共同语义。

Allowed files:

- `sql/parallel_query/pq_iterator.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/sql_optimizer.h`
- `sql/sql_class.h`
- `sql/opt_explain.cc`
- `sql/join_optimizer/explain_access_path.cc`
- `mysql-test/suite/parallel_query/**`

验收：

- `EXPLAIN` 能区分 candidate 与 actual PQ；
- `parallel_query=OFF` 无 PQ annotation；
- fallback counter 不被 explain-only 路径污染。

串并行关系：

- 必须先做；
- 可与 InnoDB handler 契约设计并行，但源码集成以 V2-0 为先。

### V2-1: Iterator Ownership + Safe Fallback

目标：

- 明确 `PQTableScanIterator` 的生命周期、fallback 机制、serial iterator 内嵌策略；
- 明确 `Init()` 失败后如何不崩溃、不改变结果；
- 明确 `Read()` row ownership、`examined_rows`、handler state 的更新边界。

Allowed files:

- `sql/parallel_query/pq_iterator.*`
- `sql/iterators/**`（只读优先）
- `sql/join_optimizer/**`（只读优先）
- `Docs/pq_tasks/v2-execution-path-roadmap.md`

验收：

- `TryCreatePQTableScanIterator()` 可以返回非空；
- `PQTableScanIterator` 内部持有 serial `TableScanIterator` fallback；
- 在未启动 worker / 未改变 handler 状态前的 Init 失败可安全串行 fallback；
- 已启动 worker 后不允许静默串行重跑，只能返回错误或中止。

串并行关系：

- 依赖 V2-0；
- 可先以 DOP=1 / storage unsupported fallback 验证，不宣称真实并行。

### V2-2: Handler/InnoDB Context Bridge

目标：

- 定稿 handler/PQ 契约：通用 `handler` virtual API 或正式 InnoDB-only downcast；
- `pq_leader_scan_init()` 必须返回真实 `PQ_Leader_context`，不能继续返回 `nullptr`；
- `pq_worker_scan_init()` 必须可从 SQL worker 拿到真实 worker ctx；
- unsupported 场景自动 fallback。

Allowed files:

- `sql/handler.h`
- `sql/parallel_query/pq_handler.*`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`

验收：

- DOP=1 可完成 leader init/end；
- unsupported 自动 fallback；
- context ownership 明确；
- 编译通过。

串并行关系：

- SQL iterator/worker 依赖它；
- 设计可并行，源码接入必须串行 review。

### V2-3: InnoDB Read View / Trx Contract

目标：

- leader 创建并 pin 一致性 read view；
- worker 不并发复用会被 `row_search_mvcc()` 修改的同一 `row_prebuilt_t/trx_t` 状态；
- KILL / early abort 后 read view 和 trx 状态释放正确。

Allowed files:

- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- 必要时 `storage/innobase/include/row0pread.h`
- 必要时 `storage/innobase/row/row0pread.cc`

验收：

- RR/RC 下并发 insert/update/delete 与串行结果一致；
- KILL / early abort 后 read view 释放；
- debug build 无 assertion。

### V2-4: Parallel_reader Thin Adapter + Range Partition

目标：

- 优先复用上游 `Parallel_reader::Scan_ctx::partition()` / Ctx traversal；
- 暴露受控 boundary/ctx wrapper；
- 替换当前 single whole-range；
- 每个 worker 领取互斥 `[start,end)`，支持 clustered full scan 正向。

Allowed files:

- `storage/innobase/include/row0pread.h`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`

验收：

- DOP=1/2/4 range 数和边界可验证；
- 空表、小表、单页、多层 B+tree 无重复无漏；
- `COUNT(*)`、`SUM(pk)`、主键连续性校验与串行一致。

串并行关系：

- 这是 DOP>1 的硬 gate；
- 可与 SQL worker/MQ 设计并行，最终集成必须串行。

### V2-5: Worker THD + Minimal Worker Scan

目标：

- 将 Phase 4 worker lifecycle scaffold 扩展为真实 worker THD 管理；
- 明确 worker 的 `THD::pq_is_worker`、security context、diagnostics area、MEM_ROOT；
- MVP 阶段 worker 只扫描 base row，不做完整 JOIN clone；
- 禁止 nested PQ。

Allowed files:

- `sql/parallel_query/sql_parallel.*`
- `sql/sql_class.h`
- `sql/conn_handler/**`
- `sql/sql_executor.cc`
- `Docs/pq_tasks/v2-execution-path-roadmap.md`

验收：

- worker 可启动、等待、清理；
- error/kill 传播可测；
- DOP=1 worker 可扫空表/小表；
- 不要求完整 worker-side predicate/projection。

串并行关系：

- 依赖 V2-2 context bridge；
- 与 row stream 接入串行。

### V2-6: Exchange/Gather Row Stream

目标：

- 定义 worker 到 leader 的 row/result 格式；
- MVP 建议发送 MySQL record image / null bitmap / field bytes；
- 明确 MQueue close/error/EOF token；
- 支持 leader `Read()` 从 Exchange 拉一行并填充 `table->record[0]`。

Allowed files:

- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/exchange.*`
- `sql/parallel_query/pq_iterator.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/record_buffer.h`

验收：

- 单 worker synthetic row 流单测或 MTR smoke；
- `SELECT * FROM t WHERE ...` 与串行结果一致；
- EOF/error 不死等；
- memory ownership 明确。

串并行关系：

- 可先用 synthetic producer 与 V2-5/V2-4 解耦；
- 接入真实 worker 后必须串行 review。

### V2-7: Predicate/Projection Boundary

目标：

- MVP 推荐 worker 只产 base row，leader 继续上层 FILTER / projection；
- 避免早期引入完整 Item clone / JOIN clone；
- base row 流稳定后，再考虑 worker-side filter / partial aggregation。

Allowed files:

- `sql/parallel_query/pq_iterator.*`
- `sql/join_optimizer/access_path.cc`
- `sql/sql_executor.*`

验收：

- `SELECT *`、`SELECT cols`、简单 WHERE 正确；
- 未引入 worker Item clone 复杂度。

### V2-8: 单表 full scan 真实闭环

目标：

- 将 V2-0 到 V2-7 串起来；
- 启用 `TryCreatePQTableScanIterator()` 返回真实 iterator；
- 仅允许单表 InnoDB clustered full scan；
- 失败保守 fallback。

Allowed files:

- `sql/parallel_query/**`
- `sql/handler.h`
- `storage/innobase/**` 中 V2-2 / V2-3 / V2-4 明确文件
- `mysql-test/suite/parallel_query/**`

验收：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt pq_fullscan_result pq_stats
```

新增验收：

- `Parallel_queries_executed` 或等价状态能反映真实执行；
- `queries_fallback` 只统计真实 fallback；
- EXPLAIN 与执行状态一致。

### V2-9: 基础聚合真实执行

目标：

- 在 full scan row 流稳定后支持 `COUNT/SUM/AVG/MIN/MAX`；
- 先支持 implicit aggregation，无 explicit GROUP BY；
- explicit GROUP BY 仍后移，除非 partial aggregation 方案独立完成。

Allowed files:

- `sql/parallel_query/pq_aggregate.*`
- `sql/parallel_query/pq_iterator.*`
- `sql/item_sum.*`（谨慎）
- `mysql-test/suite/parallel_query/**`

验收：

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt pq_agg_result pq_agg_eligible pq_agg_fallback
```

## Parallel Agent Dispatch Plan

可并行调研：

| Agent | 任务 | 输出 |
|------|------|------|
| Design-SQL | V2-0 / V2-1 / V2-5 / V2-6 SQL 层执行路径设计 | SQL path task doc + risk list |
| Design-InnoDB | V2-2 / V2-3 / V2-4 handler/InnoDB 分片扫描设计 | InnoDB task doc + interface contract |
| Test | V1/V2 测试矩阵设计 | MTR migration matrix + required new tests |

必须串行集成：

1. V1 风险收敛；
2. V2-0 execution state contract；
3. V2-1 iterator safe fallback；
4. V2-2 到 V2-4 handler/InnoDB context 与 range partition；
5. V2-5 / V2-6 worker + row flow；
6. V2-8 真实 full scan 闭环；
7. V2-9 基础聚合。

## Agent Task Prompt: V2 SQL Design

```text
请先阅读 AGENTS.md，并遵守 CLAUDE.md。

你的角色是 Design Agent。
主控 Agent 是 Codex。
当前任务是 V2 SQL 执行路径设计。

只读调研，不改源码。

请阅读：
- Docs/pq_tasks/README.md
- Docs/pq_tasks/v2-execution-path-roadmap.md
- sql/parallel_query/**
- sql/iterators/**
- sql/sql_executor.cc
- sql/join_optimizer/**

请输出：
1. 从 V1 scaffold 到真实 PQ iterator 的最小源码步骤；
2. worker THD / Exchange / Gather / fallback 的接口契约；
3. 每一步 allowed files、风险、验收命令；
4. 哪些可以并行，哪些必须串行。
```

## Agent Task Prompt: V2 InnoDB Design

```text
请先阅读 AGENTS.md，并遵守 CLAUDE.md。

你的角色是 Design Agent。
主控 Agent 是 Codex。
当前任务是 V2 InnoDB/handler 分片扫描设计。

只读调研，不改源码。

请阅读：
- Docs/pq_tasks/README.md
- Docs/pq_tasks/v2-execution-path-roadmap.md
- sql/handler.h
- storage/innobase/include/row0pread.h
- storage/innobase/row/row0pread.cc
- storage/innobase/include/row0pread_pq.h
- storage/innobase/row/row0pread_pq.cc

请输出：
1. handler 到 InnoDB 的最小接口集；
2. read view / trx / handler state 生命周期；
3. B+tree range partition 的最小可用方案；
4. 与 SQL worker/Exchange 的接口边界；
5. 验收测试建议。
```

## Current Status

主控 Agent 已创建初版拆分；等待 Explorer 反馈后补充为最终派发版。
