# V2-8 Single Table Full Scan Closure 任务书

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task-by-task. 本项目阶段文档使用中文；代码标识、命令、MySQL 概念保留英文。

**Goal:** 在单表 InnoDB clustered full scan 场景下，第一次让 PQ row stream 具备真实执行闭环：worker 读取 base row，Exchange/Gather 传递 row image，leader `PQTableScanIterator::Read()` 填充 `table->record[0]` 并让上层 SQL executor 继续执行 WHERE/projection。

**Architecture:** V2-8 是 V2 的第一个真实 execution gate。只有在独立 worker handler/prebuilt/read-view、row image ownership、error/EOF/kill cleanup 都可验证后，才能设置 `PQ_execution_state::EXECUTED` 和 `Parallel_queries_executed`。若任一硬 gate 不满足，必须保持 V2-7 行为：在 `Init()` safe window 内 serial fallback。

---

## Current Baseline

- Baseline commit: `958077831c5 Add PQ V2-7 predicate projection boundary`
- V2-4 已能规划 disjoint ranges，但 worker row read disabled。
- V2-5 已能做 worker lifecycle smoke，但不创建真实 OS worker THD。
- V2-6 已能做 synthetic MQ ROW/FINISH smoke，但不 materialize row。
- V2-7 已锁定 predicate/projection boundary：worker 不 clone Item/JOIN，leader 执行 WHERE/projection。

## Hard Gates

V2-8 不能绕过以下 gate：

1. Worker 不能共享 leader `ha_innobase::m_prebuilt` / `row_prebuilt_t`；
2. Worker 必须有独立 mutable scan state；
3. read view 必须在不允许 fallback 之后绑定，并在 cleanup 中释放；
4. `PQ_Worker_context` 必须有真实类型层次，不能继续 `reinterpret_cast`；
5. row image payload 必须有明确 ownership，leader copy 后才能继续下一次 MQ receive；
6. 一旦 `Read()` 返回过 PQ row，就不能再透明 serial fallback；
7. `Parallel_queries_executed/workers_launched/rows_scanned` 只能在真实 row path 成功启动后增加。

## Scope

允许：

- 设计并实现真实 `PQ_Worker_context` wrapper；
- 让 InnoDB worker init 分配 range，并在安全路径上读取 row；
- 定义 row image payload，先支持 fixed record image copy；
- 让 `PQTableScanIterator::Read()` 从 Exchange 读取 row image 并填充 `table->record[0]`；
- 新增 DOP=1 fullscan real MTR；
- DOP>1 只在 range/row boundary 确认无重复无漏后启用。

禁止：

- 不允许 worker-side WHERE/projection/Item clone；
- 不允许 ORDER BY/GROUP BY/JOIN/secondary index/ICP/partition table；
- 不允许共享 leader handler mutable cursor；
- 不允许 fatal error after worker start 静默 fallback；
- 不允许把 synthetic smoke 计为真实 execution。

## Proposed Phase Split

### V2-8A: Worker Handler/Prebuilt Contract Design

输出：

- `PQ_Worker_context` 真实 wrapper 设计；
- worker handler/prebuilt/read-view ownership 图；
- InnoDB cleanup/error path 清单；
- 不改源码或只改文档。

### V2-8B: Row Image Protocol

输出：

- MQ payload header；
- record image copy / null bitmap / length；
- leader copy 到 `table->record[0]` 的 API；
- synthetic row image MTR，不接 InnoDB。

### V2-8C: DOP=1 Real Full Scan

输出：

- `pq_fullscan_real_dop1`；
- `SELECT *` / `SELECT cols` / simple WHERE 与 serial 一致；
- counters: executed=1, workers_launched>=1, rows_scanned>0, fallback=0。

### V2-8D: DOP=2/4 Range Correctness

输出：

- `pq_fullscan_real_dop2_4`；
- `COUNT(*)`、`SUM(pk)`、连续主键校验；
- no duplicate / no missing。

## Initial Task Recommendation

先执行 V2-8A。不要直接编码真实 row scan。原因：当前最大风险不是 MQ，而是 InnoDB worker 独立 mutable scan state 和 read-view 生命周期。

## Validation Target

V2-8A:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

V2-8C/D 后再加入真实执行 MTR。

## Acceptance Checklist

- [ ] V2-8A worker handler/prebuilt/read-view contract 完成；
- [ ] row image ownership 明确；
- [ ] `PQTableScanIterator::Read()` 接管条件明确；
- [ ] fatal-after-start 不 fallback 的错误路径明确；
- [ ] DOP=1 real full scan MTR 通过；
- [ ] DOP>1 correctness gate 通过后才扩大。

## Current Status

- Status: In Progress
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

待实现后补充。
