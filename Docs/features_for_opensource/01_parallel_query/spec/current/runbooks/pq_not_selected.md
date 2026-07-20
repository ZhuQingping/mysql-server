# Runbook：查询未选择 Parallel Query

> 文档 ID：`PQ-RUN-001`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 运行验证：见 `current/quality/verification_evidence.md`；以下步骤来自当前源码与已有 MTR 的静态核对

## 1. 适用症状

- SQL 使用了 `PQ` hint 或 `force_parallel_execute=ON`，计划仍是串行。
- 同一 SQL 在数据量、会话、事务或服务器负载变化后不再进入 PQ。
- `EXPLAIN`、`EXPLAIN FORMAT=TREE` 中看不到 `PARALLEL_SCAN`/parallel 节点。
- `PQ_threads_refused` 或 `PQ_memory_refused` 增长。

未选择 PQ 通常是**可解释的准入结果**，不是执行错误。必须先取得拒绝原因，
再判断是预期 fallback、统计/成本问题、资源竞争，还是资格判断缺陷。

## 2. 最短取证路径

在复现连接内记录变量、事务上下文、计划和 optimizer trace。不要只保存 SQL 文本。

```sql
SELECT @@force_parallel_execute,
       @@parallel_default_dop,
       @@parallel_cost_threshold,
       @@parallel_rows_threshold,
       @@parallel_queue_timeout,
       @@parallel_graceful_fallback,
       @@parallel_fail_retry,
       @@pq_support_features_switch,
       @@transaction_isolation;

SHOW GLOBAL STATUS LIKE 'PQ%';

SET optimizer_trace='enabled=on';
EXPLAIN FORMAT=TREE <target statement>;
SELECT QUERY,
       JSON_EXTRACT(TRACE, '$.steps[*].not_apply_pq_plan') AS pq_reason,
       JSON_EXTRACT(TRACE,
         '$.steps[*].join_optimization.steps[*].not_apply_pq_plan')
         AS nested_pq_reason
  FROM INFORMATION_SCHEMA.OPTIMIZER_TRACE;
```

保存执行前后两次 `SHOW GLOBAL STATUS LIKE 'PQ%'`。全局计数有并发噪声，
若要归因于单条语句，应在隔离实例或无其他 PQ 流量时复现。

## 3. 分层定位

| 层次 | 典型拒绝 | 核对证据 | 主要源码 |
|---|---|---|---|
| 控制面 | `ZERO_DOP`、`NO_PQ`、master switch | hint、session/global 变量 | `sql/sql_parse.cc`、hint parser |
| THD/Prepare | PS、SP/trigger、SERIALIZABLE、非支持命令、hypergraph | SQL 入口、`lex->in_execute_ps`、`in_sp_trigger` | `sql/parallel_query/pq_resolver.cc` |
| Query block | window、ROLLUP、临时表、table function、锁、INTERSECT/EXCEPT | query shape 与 trace | `pq_resolver.cc` |
| Optimizer/RBO | 无可切分表、不支持 scan/ref/Item/type、多个 used partition | serial QEP、表属性、表达式树 | `pq_optimizer.cc` |
| Feature switch | INSERT SELECT、COUNT DISTINCT、spill 等开关关闭 | `@@pq_support_features_switch` | `pq_optimizer.h/.cc` |
| CBO | `PQ_COST_HIGHER`、行数门槛、zero result | statistics、估算 rows/cost、DOP | `pq_optimizer.cc` |
| 资源 | `IDLE_THREAD`、`MEMORY_LIMIT` | status 差值、线程/内存上限 | `pq_resource_stat.cc` |
| 重试 | `RETRY_WITHOUT_PQ` | warning、首轮 `ER_PARALLEL_FAIL_INIT` | `sql/sql_parse.cc` |

`PQUnsuiteInfo` 的枚举定义在 `sql/parallel_query/pq_optimizer.h`；trace 文本由
`JOIN::suite_for_parallel_query()` 写入 `not_apply_pq_plan`。诊断工具应同时读取顶层
和 nested `join_optimization` 路径，因为子查询的拒绝原因不一定出现在同一层。

## 4. PS、SP、Stored Function 的判定

- Prepared Statement 执行阶段 `lex->in_execute_ps` 为真，THD 级资格检查拒绝 PQ。
- 存储过程、存储函数体和 trigger 的内部语句通过 `THD::in_sp_trigger` 被拒绝。
- 普通 SQL 直接调用 stored function 时，表达式树出现 `Item_func::FUNC_SP`，也会被
  `NO_PQ_SUPPORTED_FUNC_TYPES` 拒绝。

因此这三类场景的当前预期都是**串行执行**，不是“hint 失效”的缺陷。若未来放开，
必须同时修改入口资格、Item 克隆/副作用契约与组合测试，不能只删除一处判断。

## 5. 判定与升级

| 观察 | 结论 | 下一步 |
|---|---|---|
| trace 有稳定且与能力矩阵一致的原因 | 预期拒绝/serial fallback | 记录原因；无需强制绕过 |
| `PQ_threads_refused` 增长 | DOP 资源不足或排队超时 | 降 DOP、隔离并发，核对 `parallel_max_threads` |
| `PQ_memory_refused` 增长 | 全局 PQ 内存准入拒绝 | 核对运行中 PQ 与内存计数回收 |
| trace 为 `PQ_COST_HIGHER` | CBO 选择串行 | 更新统计并对比估算与实际；不要用 force 掩盖统计问题 |
| trace 缺失且计划串行 | 可能在更早入口被拒绝或观测缺口 | 核对 DOP/no_pq/命令/PS/SP；保留最小复现 |
| 支持矩阵标为 implemented，但稳定得到非预期拒绝 | 一致性缺口 | 创建 conformance gap，附 commit、SQL、trace、plan、变量 |

## 6. 最小问题报告

必须包含：目标 commit 与 dirty 状态、建表/数据规模、完整 SQL、session/global 变量、
隔离级别与事务状态、`EXPLAIN FORMAT=TREE`、完整 optimizer trace、PQ status 前后差值、
串行结果是否正确，以及是否在 PS/SP/function/trigger 中执行。

代表性静态测试入口：`pq_optimizer_trace.test`、`pq_optimizer_trace_bugfix.test`、
`pq_cbo.test`、`pq_support_features_switch.test`、`pq_limit_no_order_by.test`。
