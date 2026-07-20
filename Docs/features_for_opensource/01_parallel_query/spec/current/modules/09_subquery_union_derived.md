# Parallel Query Subquery、UNION 与 Derived 当前契约

> 模块 ID：`PQ-MOD-009`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 最后运行验证：见 `current/quality/verification_evidence.md`

> Snapshot authority：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`。该 manifest 绑定
> implementation commit `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9`、`stable_branch` 已提交基线、
> `source_tree_sha256=a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> 和 `test_tree_sha256=066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`。
> 两个 hash 只覆盖 manifest 定义的 PQ source/test file set，不覆盖或复现整个 bound PQ source/test tree。

## 1. 职责与非目标

本模块定义高级 SQL shape 在 PQ 中的当前边界，重点是：

- subquery 的资格分类和执行频率；
- correlated subquery 在父 worker 中执行的边界；
- UNION ALL、UNION DISTINCT 和 materialization 的计划接入；
- derived、view、CTE 和共享 materialized storage 的生命周期；
- Prepared Statement、Stored Procedure、Stored Function 和 Trigger 与上述路径的交叉边界。

本模块不重复定义通用 Item/AccessPath clone、worker 状态机、MQ 协议和聚合算法；这些
契约分别属于 `modules/02_plan_rewrite_clone_ownership.md`、
`modules/03_worker_lifecycle_error_retry.md`、
`modules/06_mq_exchange_record_protocol.md` 和
`modules/07_aggregation_order_limit.md`。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 资格分类 | `sql/sql_union.cc::Query_expression::subquery_suite_for_parallel_query` | 将 query expression 分类为 `kImpossible`、`kOnceByLeader` 或 `kMultipleByWorkers` |
| Query block gate | `sql/parallel_query/pq_resolver.cc::Query_block::suite_for_parallel_query` | `kImpossible` 必须使当前 query block 不进入 PQ |
| Worker 内 subquery gate | `sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | `kMultipleByWorkers` 的 subquery 本身不建立新的 Gather，而是在父 query block 的 worker 内串行执行 |
| Query expression 改写 | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan` | 对满足条件的 simple/UNION/materialized query expression 建立 PQ leader plan |
| Leader 预执行 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::exec_scalar_uncorrelated_subquery` | worker 启动前执行可共享的无关 scalar subquery |
| 共享 materialization | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::Init`、`para_exec_init` | worker 启动前完成共享临时表初始化 |
| Clone | `sql/parallel_query/pq_clone.cc`、`sql/parallel_query/pq_clone_item.cc` | 构造 template/worker query graph，并修复 outer reference、table 和 Item 引用 |
| PTRC key 修复 | `sql/item_subselect.cc::Item_subselect` PQ 特殊路径 | 根据原 query block 的 PTRC key table 建立 clone 对应关系 |

输入是传统优化器已经 prepare/optimize 的 `Query_expression`、`Query_block`、`JOIN`、
subquery Item 和 materialization AccessPath。输出可能是：

1. 当前 query block 的 Gather；
2. 在 leader 上预执行并共享的 scalar value 或 materialized storage；
3. 在父 worker 中执行的 correlated subquery clone；
4. 带稳定拒绝原因的串行计划。

## 3. 主流程与执行频率

```text
prepare / resolve
  -> subquery_suite_for_parallel_query()
     -> kImpossible
        -> current query block serial-fallback
     -> kOnceByLeader
        -> clone metadata
        -> ParallelScanIterator::Init()
        -> materialize shared inputs
        -> execute uncorrelated scalar subquery once
        -> initialize Gather/read view
        -> launch workers
     -> kMultipleByWorkers
        -> do not create a nested Gather for this subquery
        -> clone into parent worker query graph
        -> evaluate according to the parent worker's row flow
```

当前分类规则的静态事实如下：

| Shape | 分类/行为 | 关键约束 |
|---|---|---|
| Non-correlated scalar subquery | `kOnceByLeader` | 必须可缓存、不是 max/min 特殊 subquery |
| Non-correlated EXISTS/simple scalar | `kOnceByLeader` | `uncacheable == 0` |
| Non-correlated derived/view | `kOnceByLeader` | derived 不得 outer-correlated |
| Correlated EXISTS/single-row subquery | `kMultipleByWorkers` | simple query expression；feature switch 开启；位置受限；不含未实现的嵌套 shape |
| Lateral dependency | `kImpossible` | 当前不进入 PQ |
| Recursive subquery expression | `kImpossible` | 当前分类器拒绝，相关 query block 使用 serial-fallback |
| Top-level/independent recursive CTE | 不适用 | 未由 subquery classifier 独立定义；当前支持状态为 `unknown` |
| Unsupported IN/ALL/ANY/row shape | `kImpossible` 或后续拒绝 | 必须以实际 trace reason 判断，不能由语法名称推断支持 |

`kMultipleByWorkers` 的含义不是“每次 outer row 创建一组新 PQ workers”。源码在
`JOIN::suite_for_parallel_query()` 中明确让该 subquery query block 不独立并行；它被
包含在父 query block 的 worker 计划中。因此执行次数由父 worker 实际消费的行数、
表达式位置、排序/临时表复用和 MySQL subquery cache 共同决定。

UNION 路径由 `Query_expression::optimize()` 在普通优化和 materialization 安排完成后调用
`make_pq_unit_plan()`：

- simple query block 可建立一个 Gather；
- UNION ALL 的成员 query block 可分别成为 PQ 候选；
- UNION DISTINCT 需要额外 materialization，不能按 UNION ALL streaming 语义解释；
- INTERSECT/EXCEPT 在 resolver gate 中拒绝 PQ；
- derived/view/CTE 的 materialized table 只有在 storage、相关性和 DIV_TAB 选择均满足时
  才能进入部分支持路径。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | 分类 | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| 原 `Query_expression/Query_block` | Parser/Resolver | leader THD | borrowed by PQ planning | 普通 statement cleanup | clone、refix 和 fallback 完成前必须有效 |
| `Gather_operator::m_uncorrelated_subqueries` 条目 | leader plan 构造 | Gather/leader | shared metadata | Gather cleanup | 指向的原 Item 在 leader 预执行结束前有效 |
| 无关 scalar subquery 结果 | leader 预执行 | leader Item/query graph | immutable shared value | leader statement cleanup | worker 启动前完成，worker 只能读取 |
| Shared materialized temp table | leader/materialization iterator | leader/Gather | shared storage | leader/Gather cleanup | 初始化完成先于 worker 读取，销毁晚于最后一个 worker |
| Template subquery graph | plan clone | Gather | template | Gather cleanup | 不执行，只作为 worker clone 蓝图和 EXPLAIN 结构 |
| Worker subquery graph | `make_pq_worker_plan` | worker THD | worker-local | worker cleanup | Field/Table/Item 不得借用会提前释放的 leader-local 对象 |
| Correlated outer reference | clone/refix | worker graph 或显式 shared owner | worker-local/borrowed | 随对应 owner | 必须绑定到同一 worker 的父 query block 行上下文 |
| `m_query_blocks_to_materialize` | UNION/materialization setup | query expression/leader | leader mutable state | query-expression cleanup | graceful fallback 前必须恢复串行计划需要的原状态 |
| PTRC key mapping | `Item_subselect` PQ 路径 | worker/template THD | worker-local metadata | `ptrc::cleanup`/THD cleanup | key table 必须映射到 clone 后的 TABLE，不能引用原 TABLE |

## 5. 并发、锁和状态

- Leader 对 shared materialization 和无关 scalar subquery 的成功初始化，必须
  happens-before 任一 worker 开始执行。
- worker 启动后，共享 scalar value 和只读 materialized input 不得再被 leader 改写。
- correlated subquery 的 mutable execution state 必须 worker-local；没有明确同步协议的
  cache、iterator、handler 或 diagnostics 不得跨 worker 共享。
- materialized temp table 若允许多个 worker 读取，handler shared state 和 readinfo 的
  clone/attach 必须覆盖 EOF、KILL 和 worker error。
- UNION 多个 Gather 的错误必须汇聚到同一客户端 statement，不能让一个成员成功掩盖
  另一个成员失败。
- PTRC 与 subquery clone 存在显式映射逻辑，不能把“PTRC 是另一个特性”视为无交叉影响。

## 6. 规范性不变量与 Requirement

### `PQ-HSQL-REQ-001` / `PQ-HSQL-INV-001`：分类决定执行位置

每个 subquery query expression MUST 在计划改写前得到唯一执行分类；
`kImpossible` MUST NOT 进入 worker 图，`kOnceByLeader` MUST NOT 在多个 worker 重复执行，
`kMultipleByWorkers` MUST NOT 为该 subquery 创建嵌套 worker 集合。

### `PQ-HSQL-REQ-002` / `PQ-HSQL-INV-002`：Leader 预执行先于 Worker

可共享 scalar subquery 和 materialized input MUST 在启动第一个 worker 前成功完成；失败时
MUST NOT 启动依赖该结果的 worker。

### `PQ-HSQL-REQ-003` / `PQ-HSQL-INV-003`：相关引用隔离

Correlated subquery 的 outer reference MUST 绑定到当前 worker 的父 query block 对象，
MUST NOT 读取另一个 worker 或 leader 的可变 row buffer。

### `PQ-HSQL-REQ-004` / `PQ-HSQL-INV-004`：共享 Materialization 生命周期

共享 materialized storage MUST 在最后一个消费者退出后才销毁；初始化失败、KILL、LIMIT
早停和 worker error 都 MUST 只清理一次。

### `PQ-HSQL-REQ-005` / `PQ-HSQL-INV-005`：Set operation 语义等价

UNION ALL MUST 保留重复行；UNION DISTINCT MUST 保留全局去重语义。PQ OFF 与 PQ ON 的
rows、error、warning 和要求的 order MUST 等价。

### `PQ-HSQL-REQ-006` / `PQ-HSQL-INV-006`：执行上下文拒绝不可绕过

Prepared Statement 执行、Stored Program 内部 SQL、Trigger 内部 SQL 和表达式树中包含
`FUNC_SP` Item 的相关 query block MUST 整体串行回退，PQ Hint 和 force 变量 MUST NOT
绕过。

### `PQ-HSQL-REQ-007` / `PQ-HSQL-INV-007`：拒绝无计划副作用

高级 SQL shape 被拒绝后，MUST 保留可执行的串行 query expression，且不得残留 shared
materialization、DIV/CUT 标记、PTRC clone key 或线程预留。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | fallback/retry | 必须清理/恢复 |
|---|---|---|---|---|
| Subquery 分类为不可能 | resolver/query block | 正常串行执行 | serial-fallback | 不得创建 PQ 对象 |
| Subquery/derived clone 或 refix 早期失败 | leader planner | 通常继续串行或返回初始化错误 | 仅在 graceful fallback point-of-no-return 前可原地回退 | clone map、saved Items、materialization 列表、DIV/CUT |
| Leader scalar subquery 预执行失败 | leader | 按普通 scalar subquery/IGNORE 语义返回 error 或 warning | worker 尚未启动；是否进入完整 retry 取决于最终错误码，当前没有独立发布契约 | scalar result、diagnostics、未启动 Gather 资源 |
| Shared materialization 初始化失败 | leader iterator | 初始化失败 | 不允许假定总能 retry；必须由 lifecycle contract 和错误码决定 | temp table、handler、MQ、read view 前置资源 |
| Correlated subquery worker 运行错误 | worker，leader 汇总 | statement error，不能部分成功 | runtime 阶段禁止串行续跑 | worker graph、MQ、shared materialization、全部 worker join |
| UNION member leader rewrite 失败 | leader planner | 串行回退或 `ER_PARALLEL_FAIL_INIT` | 按改写 point-of-no-return 决定 | 所有 member 的串行状态和 materialization 列表 |
| KILL/client disconnect | leader + workers | cancel/error | 禁止隐藏式串行 retry | 所有消费者退出后再释放 shared storage |

`INSERT IGNORE ... SELECT (scalar subquery)` 有专门错误处理：scalar subquery 自身失败但错误被
IGNORE 规则容忍时，不应无条件使整个 PQ 失败。该行为必须与串行路径的 warning 和插入行数
一致。

## 8. 支持、限制和 feature switch

| 场景 | 当前状态 | 当前契约 |
|---|---|---|
| 无关 scalar subquery | `partial` | 可 leader-once；类型、cacheability 和 Item clone 受限 |
| Correlated EXISTS/single-row | `partial` | `correlated_subquery` switch 开启时支持部分位置和 simple shape |
| Correlated IN/ALL/ANY、复杂嵌套 | `serial-fallback` | 当前分类/clone 约束拒绝 |
| Lateral | `serial-fallback` | `m_lateral_deps` 直接拒绝 |
| Derived/view | `partial` | non-outer-correlated，materialization 与 temp engine 还需满足约束 |
| Outer-correlated derived/view | `serial-fallback` | table-list gate 拒绝 |
| UNION ALL | `partial` | simple member 可分别进入 PQ |
| UNION DISTINCT | `partial` | 依赖 leader materialization 与全局去重 |
| INTERSECT/EXCEPT | `serial-fallback` | resolver 明确拒绝 |
| CTE | `partial` | 取决于 merge/materialization 和 query-expression shape |
| Recursive subquery expression | `serial-fallback` | `Query_expression::subquery_suite_for_parallel_query()` 对 recursive subquery 返回 `kImpossible` |
| Top-level/independent recursive CTE | `unknown` | 尚无独立发布级契约和定向测试；不从 recursive subquery 拒绝外推 |
| SQL/Binary Prepared Statement execute | `serial-fallback` | `lex->in_execute_ps` 硬拒绝；`pq_prepare` 不是协议 PS 支持证明 |
| Stored Procedure 内部 SQL | `serial-fallback` | `THD::in_sp_trigger` 硬拒绝 |
| Stored Function 内部 SQL | `serial-fallback` | stored program 执行上下文硬拒绝 |
| 顶层 SQL 调用 Stored Function | `serial-fallback` | 表达式树中的 `Item_func::FUNC_SP` 命中 unsupported gate，整个相关 query block 不进入 PQ |
| Trigger 内部 SQL | `serial-fallback` | trigger nested statement 不进入 PQ |
| PQ INSERT/REPLACE SELECT 触发 target trigger | `partial` | select side 可为 PQ 候选，target DML/trigger 在 leader 侧；副作用契约见模块 10 |

## 9. 可观测性

- EXPLAIN TREE/JSON 中每个 `<gatherN>` 只能证明对应 query block 进入 PQ，不能证明整个
  statement 的所有 subquery 都并行。
- `not_apply_pq_plan` 的 `UNSUPPORTED_SUBQUERY_TYPE`、`SUB_MTIP_WORKER`、
  `VIEW_OR_DERIVED_TABLE` 等 reason 用于说明拒绝或 worker 内执行分类。
- `PQ_stmt_executed` 在 worker launch 前按采用的 leader query-block plan 递增；
  UNION/多 query block statement 可增加多次。它不是 worker-run 证据，也不能表达
  scalar subquery 执行一次还是 correlated subquery 实际执行多少次。
- EXPLAIN ANALYZE 可展示 worker plan/timing，但当前没有稳定的 subquery execution-frequency
  产品指标。
- fallback/retry 必须联合观察 plan reason、warning/error、最终 leader-plan marker
  `PQ_executed` 和 worker/cleanup 证据；不能只检查 force 变量或把 marker 当作 worker run。

## 10. 代码与测试映射

下列测试是静态 Requirement 映射；suite 运行范围见 `current/quality/verification_evidence.md`，不得据此推导组合级 `verified`。

| Requirement/Invariant | 源码 symbol | 正向测试 | 负向/故障测试 | 证据状态 |
|---|---|---|---|---|
| `PQ-HSQL-REQ-001` / `INV-001` | `Query_expression::subquery_suite_for_parallel_query` | `pq_subquery`、`pq_subquery_correlated` | `pq_optimizer_trace` | `static-partial` |
| `PQ-HSQL-REQ-002` / `INV-002` | `ParallelScanIterator::exec_scalar_uncorrelated_subquery`、`Init` | `pq_subquery`、`pq_explain_analyze` | `pq_fallback` | `static-partial` |
| `PQ-HSQL-REQ-003` / `INV-003` | `pq_clone_item.cc`、`pq_refix_fields_item.cc` | `pq_subquery_correlated` | 同用例中的拒绝/历史 bug shapes | `static-partial` |
| `PQ-HSQL-REQ-004` / `INV-004` | `para_exec_init`、materialization clone paths | `pq_divide_derived`、`pq_derived_view` | `pq_divide_derived_low_mmap`、`pq_worker_error` | `static-partial` |
| `PQ-HSQL-REQ-005` / `INV-005` | `Query_expression::optimize`、`make_pq_unit_plan` | `pq_union`、`pq_explain_tree` | `pq_fallback` | `static-partial` |
| `PQ-HSQL-REQ-006` / `INV-006` | `THD::suite_for_parallel_query`、`NO_PQ_SUPPORTED_FUNC_TYPES` | `pq_sp_trigger` | `pq_optimizer_trace`；binary PS 与 trigger restore fault 为 `none` | `static-partial`; 缺独立协议/故障覆盖 |
| `PQ-HSQL-REQ-007` / `INV-007` | `restore_leader_plan`、`reset_derived_materialize_query_blocks` | `pq_fallback` | `pq_auto_retry_failed_parallel_query` | `static-partial` |

## 11. 已知缺口和变更影响

- `PQ-GAP-HSQL-001`：缺少按位置、相关性、cacheability、materialization 和执行频率组织的
  机器可检查支持矩阵。
- `PQ-GAP-HSQL-002`：缺少 binary protocol PS、reprepare、重复 EXECUTE 和 SQL PREPARE
  的独立拒绝覆盖。
- `PQ-GAP-HSQL-003`：`pq_sp_trigger` 以通用 stored-program regression 为主，尚未形成
  Procedure 内 SELECT、顶层 `FUNC_SP`、Trigger nested SQL 和 PQ DML target trigger 的
  路径矩阵。
- `PQ-GAP-HSQL-004`：没有产品级指标证明 leader-once 和 worker-multiple 的实际次数。
- `PQ-GAP-HSQL-005`：shared materialization 在 worker partial launch、KILL、error 和 cleanup
  重入下缺少统一生命周期断言。
- `PQ-GAP-HSQL-006`：PTRC × correlated subquery/view 的 clone key、cache lifetime 和
  fallback 只有零散回归资产。
- `PQ-GAP-HSQL-007`：Recursive subquery expression 为 `serial-fallback`；top-level/independent
  recursive CTE 和复杂 CTE reuse 仍为 `unknown`，不得从普通 CTE 用例推断支持。

修改本模块相关代码时，必须联动检查 resolver、`sql_union.cc`、clone/refix、
`ParallelScanIterator::Init`、PTRC、模块 02/03/10/12 以及质量追踪矩阵。
