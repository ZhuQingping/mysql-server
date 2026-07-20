# Parallel Query 当前支持矩阵

> 文档 ID：`PQ-CONTRACT-SUPPORT-001`
> 状态：`current-contract`，证据级别为 `static-evidence`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 运行验证：见 `current/quality/verification_evidence.md`

## 1. 使用规则

本矩阵是当前 PQ 支持边界的唯一人工维护事实源。`implemented` 表示当前快照中存在
对应代码路径，不表示本轮已经运行 MTR。任何未列场景默认为 `unknown`，不能根据
历史 WL 推断为支持。

“串行回退”表示 SQL 通常仍可由 MySQL 串行执行；它不是 PQ 执行错误。强制变量或
`PQ` Hint 不能绕过硬资格检查。

“代表测试”指 `stable_branch` 中存在的 `.test`/`.result-pq` 资产。全套 `--pq`
运行结果见 `verification_evidence.md`；它不替代场景行所需的组合级 oracle。原快照中的
customer/trigger overlay probes 不属于本分支，不得作为当前覆盖证据。

## 2. 命令与执行上下文

| 场景 | 状态 | 当前行为/条件 | 主要静态证据 | 代表测试 |
|---|---|---|---|---|
| 普通 `SELECT` | `implemented` | 仍需通过 Query_block/JOIN/engine/资源/CBO 全部检查 | `THD::suite_for_parallel_query`、`make_pq_unit_plan` | `pq_fullscan`、`pq_optimizer_trace` |
| `INSERT ... SELECT` | `partial` | 受 `insert_select` feature flag、binlog、锁和副作用约束；默认 flag 为 off | `PQ_SUPPORT_INSERT_SELECT`、SQL insert 接入点 | `parallel_insert_select*` |
| `REPLACE ... SELECT` | `partial` | 与 INSERT SELECT 类似，需单独验证并发和唯一键副作用 | SQL command gate、replace-select tests | `parallel_replace_select*` |
| 普通 INSERT/UPDATE/DELETE | `serial-fallback` | 不是 PQ 主命令类型 | `THD::suite_for_parallel_query` command gate | `pq_optimizer_trace` |
| SQL `PREPARE/EXECUTE` | `serial-fallback` | 执行阶段 `lex->in_execute_ps=true`，PQ 资格检查拒绝 | `sql_prepare.cc::set_thd_to_ps`、`pq_resolver.cc` | `pq_optimizer_trace` |
| Binary protocol Prepared Statement | `serial-fallback` | 走相同 Prepared_statement 执行上下文 | `Prepared_statement` execution path | 需要独立协议级覆盖 |
| Stored Procedure 内部 SQL | `serial-fallback` | `THD::in_sp_trigger=true` | `sp_head::execute_procedure`、`pq_resolver.cc` | `pq_sp_trigger` |
| Stored Function 内部 SQL | `serial-fallback` | Stored program 执行期间 `in_sp_trigger=true` | `sp_head::execute_function` | `pq_sp_trigger` |
| 顶层 SQL 直接调用 Stored Function | `serial-fallback` | 表达式树中的 `Item_func::FUNC_SP` 命中 `NO_PQ_SUPPORTED_FUNC_TYPES`，整个相关 query block 不进入 PQ | `pq_optimizer.cc::NO_PQ_SUPPORTED_FUNC_TYPES` | `pq_sp_trigger`；仍需协议/组合矩阵 |
| Trigger 内部 SQL | `serial-fallback` | Trigger 执行期间 `in_sp_trigger=true` | `sp_head::execute_trigger` | `pq_sp_trigger`；仍缺 trigger restore 专项故障覆盖 |
| Attachable transaction | `serial-fallback` | THD 级资格检查拒绝 | `THD::suite_for_parallel_query` | optimizer trace 类测试 |
| Hypergraph optimizer | `serial-fallback` | PQ 依赖 QEP_TAB，明确互斥 | `lex->using_hypergraph_optimizer()` gate | `pq_not_support` |

## 3. 事务、隔离和锁

| 场景 | 状态 | 当前行为/条件 | 主要证据 | 代表测试/缺口 |
|---|---|---|---|---|
| READ UNCOMMITTED | `unknown` | 未形成发布级组合契约 | transaction/read-view paths | 需要隔离级别矩阵 |
| READ COMMITTED | `partial` | 代码可建立/克隆 read view，但 statement view 语义需系统验证 | `pq_create_innodb_snapshot`、InnoDB trx | `pq_read_view` |
| REPEATABLE READ | `partial` | 同上；显式事务和 purge 生命周期未在本轮运行验证 | InnoDB PQ scan context | `pq_read_view`、`pq_rec_visible` |
| SERIALIZABLE | `serial-fallback` | THD 级硬拒绝 | `tx_isolation == ISO_SERIALIZABLE` | `pq_not_support` |
| `SELECT ... FOR UPDATE` | `serial-fallback` | PQ 不实现 locking read | Query_block/lock checks | `pq_mdl_lock`、insert-select tests |
| `LOCK IN SHARE MODE` / `FOR SHARE` | `serial-fallback` | 显式 locking read 不进入 PQ | locking clause checks | insert/replace-select tests |
| 显式 `LOCK TABLES` | `serial-fallback` | explicit table lock 被拒绝 | Query_block eligibility | `pq_not_support` |
| Autocommit consistent read | `implemented` | 仍需满足全部支持条件 | normal SELECT path | `pq_fullscan`、`pq_read_view` |
| 显式事务 | `partial` | read view/cleanup 需按隔离级别和并发 DML 验证 | THD/trx lifecycle | `pq_read_view`，缺完整矩阵 |
| XA | `unknown` | 当前规范不声明支持 | no approved contract | 需要定向测试 |
| 并发 INSERT/UPDATE/DELETE/purge | `partial` | 有零散测试，未形成完整 MVCC × scan matrix | InnoDB visibility/cursor paths | `pq_rec_visible`、customer probes |
| 并发 DDL/MDL | `partial` | MDL 有覆盖，page/metadata 变化仍属高风险 | MDL and table metadata paths | `pq_mdl_lock`、`pq_instant_add_column` |

## 4. Engine、表和 partition

| 场景 | 状态 | 当前行为/条件 | 证据 | 代表测试 |
|---|---|---|---|---|
| InnoDB base table | `implemented` | 当前实例 engine gate 要求 InnoDB | `get_instance_storage_engine_type()` | 大多数 PQ tests |
| DSTORE | `serial-fallback` | 明确不支持 | engine gate | `pq_not_support_dstore` |
| 其他普通 engine | `serial-fallback` | handler capability 不满足 | table/handler checks | `pq_not_support` |
| Internal non-transactional temp table | `partial` | materialization 等特定路径可以共享或扫描 | `TABLE::suite_for_pq_division` | `pq_innodb_intrinsic`、derived tests |
| 用户 transactional temporary table | `serial-fallback` | table eligibility 拒绝 | table checks | `pq_not_support` |
| System table/system schema | `serial-fallback` | eligibility 拒绝 | table-list checks | `pq_not_support` |
| 非分区表 | `implemented` | 标准 InnoDB PQ scan | handler/InnoDB PQ | scan tests |
| 分区表且仅一个 used partition | `partial` | 只在该 partition 内做 B+Tree PQ scan | `num_partitions_used()==1` | `pq_partition` |
| 多个 used partitions | `serial-fallback` | 当前明确拒绝；WL008 是历史设计 | `TABLE::suite_for_pq_division` | `pq_partition` |
| Subpartition | `unknown` | 不超出单 used partition 约束的细节需验证 | partition info | 需要定向测试 |
| Table function | `serial-fallback` | Query_block eligibility 拒绝 | table-function check | `pq_not_support` |
| Fulltext | `serial-fallback` | 表 eligibility 拒绝 | `is_fulltext_searched()` | `pq_not_support` |

## 5. Access path 和扫描

| 场景 | 状态 | 当前行为/条件 | 代表 symbol | 代表测试 |
|---|---|---|---|---|
| Full table scan (`JT_ALL`) | `implemented` | 可作为 DIV/CUT | `PQblockScanIterator` | `pq_fullscan` |
| Full index scan | `implemented` | 支持正向/部分反向路径 | handler/InnoDB index init | `pq_coverage_index`、`pq_index_scan_desc` |
| Clustered range scan | `implemented` | Range AccessPath 必须可转换为支持的单一 index range | `pq_range_scan_init` | `pq_range_clust` |
| Secondary range scan | `implemented` | 包含 MVCC/回表约束 | `find_visible_record` | `pq_range_sec` |
| Static `JT_REF` | `implemented` | Key 在计划/round 中确定 | `PQ_BLOCK_SCAN` | `pq_jt_ref` |
| Dependent ref | `partial` | 外表 key 变化时动态构造/切换 ranges | `PQRefIterator`、`pq_ref_build_ranges` | `pq_depend_ref`、`pq_ref_build_range` |
| Reverse range/index scan | `implemented` | 边界和 cursor restore 方向敏感 | InnoDB PQ cursor paths | `pq_range_scan_reverse`、`pq_reverse_index_scan` |
| ICP | `implemented` | 二级索引可先 ICP，必要时回聚簇索引 | `find_visible_record` | `pq_icp` |
| Covering secondary index | `implemented` | 仍需 MVCC 可见性判断 | secondary scan paths | `pq_coverage_index` |
| Non-covering secondary index | `implemented` | 需要 clustered lookup | visible-record paths | `pq_range_sec` |
| Record buffer | `implemented` | context 切换、restart 和 generated column 是高风险边界 | `PQ_Ctx::read_record` | `pq_record_buffer` |
| Dynamic range | `serial-fallback` | 当前 RBO 拒绝 | scan-type checks | `pq_not_support` |
| Multi-range shape not reducible to supported range path | `serial-fallback` | 需满足 AccessPath 约束 | range AccessPath checks | `pq_not_support` |
| MVI duplicate filter | `partial` | 有专门状态和测试，但组合覆盖有限 | iterator duplicate-filter path | `pq_multi_value` |

## 6. SQL operators

| 场景 | 状态 | 当前行为/条件 | 代表测试 |
|---|---|---|---|
| Projection/WHERE | `implemented` | Item 必须可 clone/refix，类型和函数必须支持 | `pq_coverage`、`pq_clone_item` |
| SUM/COUNT/AVG/MIN/MAX | `implemented` | 按 partial/final 规则转换；NULL/overflow/warning 仍需组合验证 | `pq_group_by`、`pq_aggr_no_record` |
| GROUP BY | `implemented` | Worker partial + leader final；排序/临时表路径受约束 | `pq_group_by` |
| `COUNT(DISTINCT ...)` | `partial` | 受 `count_distinct` feature flag、参数类型和 Batch_buffer 内存约束；历史 group reshuffle 不是当前事实 | `pq_agg_distinct`、`pq_distinct` |
| `SUM(DISTINCT ...)` / `AVG(DISTINCT ...)` | `serial-fallback` | `SUM_DISTINCT_FUNC`、`AVG_DISTINCT_FUNC` 位于当前不支持聚合类型列表 | `sql/parallel_query/pq_optimizer.cc::NO_PQ_SUPPORTED_AGG_FUNC_TYPES`、`pq_agg_distinct` |
| HAVING | `partial` | 是否 worker 下推/leader final 取决于聚合和 clone | aggregate tests |
| ORDER BY | `implemented` | Worker 局部有序或 leader sort；可使用 gather merge | `pq_order_by`、`pq_order_const` |
| ORDER BY + LIMIT/OFFSET | `partial` | 稳定性、early detach 和 offset pushdown 有额外条件 | order/limit tests |
| LIMIT without ORDER BY | `partial` | 由 `parallel_limit_no_order_by` 控制；返回集合顺序不保证 | `pq_limit_no_order_by` |
| `SQL_BUFFER_RESULT` | `serial-fallback` | Query_block 资格检查拒绝 | `pq_optimizer_trace` |
| ROLLUP | `serial-fallback` | 当前拒绝 | `pq_not_support`、distinct tests |
| Window functions | `serial-fallback` | 当前拒绝 | `pq_optimizer_trace` |
| DISTINCT SELECT | `partial` | 取决于 distinct/temporary-table/aggregate 路径 | `pq_distinct` |
| Hash Join | `implemented` | build/probe、CUT_TAB、spill flag、barrier 有额外约束 | `pq_hash_join` |
| Semi/Anti/Outer Hash Join | `partial` | Join 类型和 plan shape 受约束 | `pq_semijoin`、hash tests |
| BKA | `partial` | 部分 inner/table shape 被 RBO 拒绝 | `pq_join_bka` |
| Outer join inner 作为 DIV_TAB | `serial-fallback` | DIV_TAB 选择限制 | join eligibility；缺独立负向用例 |
| Semijoin inner 作为 DIV_TAB | `serial-fallback` | DIV_TAB 选择限制 | `pq_semijoin` |

## 7. Subquery、UNION、Derived 和 CTE

| 场景 | 状态 | 当前行为/条件 | 代表测试 |
|---|---|---|---|
| 无关 scalar subquery | `partial` | 可由 leader 预执行并共享；位置和 Item 类型受限 | `pq_subquery` |
| Correlated subquery | `partial` | 支持部分 NST clone/query-block shape，不是普遍支持 | `pq_subquery_correlated` |
| Materialized subquery | `partial` | 临时表共享和生命周期受限 | `pq_subquery`、`pq_semijoin` |
| UNION ALL | `partial` | simple query block 可分别建立 Gather | `pq_union` |
| UNION DISTINCT | `partial` | 进入额外 materialization 路径 | `pq_union` |
| INTERSECT/EXCEPT | `serial-fallback` | Query_block 资格检查拒绝 | `pq_not_support` |
| Materialized derived/view | `partial` | shared storage、DIV_TAB 选择和 outer reference 受限 | `pq_divide_derived`、`pq_derived_view` |
| Outer-correlated derived/view | `serial-fallback` | eligibility 拒绝 | derived tests |
| CTE | `partial` | 与 materialization/query-expression shape 相关 | derived/union tests |
| Recursive CTE | `unknown` | 当前规范不声明 PQ 支持 | 需要定向测试 |

## 8. Item、函数和数据类型

| 场景 | 状态 | 当前行为/条件 | 证据/测试 |
|---|---|---|---|
| 常见数值/字符/日期表达式 | `implemented` | Item 类型必须有完整 clone/refix | `pq_clone_item`、`pq_charset`、`round_truncate_for_pq` |
| BLOB | `partial` | 有专门测试，消息和临时表大小需约束 | `pq_blob` |
| JSON/GEOMETRY | `serial-fallback` | 列在不支持字段/函数类型中 | optimizer unsupported lists |
| MATCH/fulltext | `serial-fallback` | 不支持函数/表访问 | optimizer/table checks |
| Stored Function (`FUNC_SP`) | `serial-fallback` | 不允许进入 worker Item 图 | `NO_PQ_SUPPORTED_FUNC_TYPES` |
| UDF | `serial-fallback` | `UDF_FUNC`/aggregate UDF 不支持 | unsupported lists、`pq_user_func` |
| 用户变量 | `serial-fallback` | `SUSERVAR_FUNC` 不支持，避免共享副作用 | unsupported lists |
| RAND/SYSDATE 等非确定性行为 | `partial` | 需要保持串行语义、seed 和 binlog 行为 | `pq_sysdate_is_now`、clone tests |
| Generated column | `partial` | record conversion/buffer 有专门路径 | `pq_instant_add_column`、record tests |

## 9. 控制、资源和可观测性

| 场景 | 状态 | 当前行为/条件 | 证据/测试 |
|---|---|---|---|
| `PQ` Hint | `implemented` | 设置候选 DOP/table/qb 信息，不能绕过硬检查 | hint parser、`pq_variables` |
| `NO_PQ` Hint | `implemented` | 稳定禁用 PQ | resolver no_pq gate |
| `force_parallel_execute` | `implemented` | 只在未设置 DOP 时进入候选流程，不保证选中 | `sql_parse.cc` |
| RBO | `implemented` | Hint → threshold candidate → threshold=0 最大候选 | `JOIN::choose_parallel_tables` |
| CBO | `implemented` | 比较串行/并行成本和传输/setup cost | access-path cost rewrite、`pq_cbo` |
| 全局线程准入 | `implemented` | 上限 64；拒绝/等待行为受 timeout 控制 | resource stat/sys vars |
| 全局内存准入 | `partial` | 默认 100MB；当前更接近准入 accounting，不应理解为所有运行期分配硬上限 | `pq_resource_stat`、`pq_memory_limit` |
| Worker thread pool dispatch | `design-only` | 当前主路径直接 `mysql_thread_create()` | `pq_iterators.cc` |
| Traditional EXPLAIN | `implemented` | 显示 `<gatherN>`/Parallel execute | `pq_explain` |
| EXPLAIN TREE/JSON/ANALYZE | `implemented` | worker timing/plan 展示仍需格式稳定性测试 | `pq_explain_tree/json/analyze` |
| Optimizer trace rejection | `implemented` | `not_apply_pq_plan` + reason | `pq_optimizer_trace` |
| Global status | `implemented` | 线程、内存、statement 等全局统计 | `pq_variables`、`pq_examined_rows` |
| Per-statement skew/range/MQ 指标 | `unknown` | 当前未形成完整产品接口 | hardening candidate |

## 10. Fallback、retry 和错误

| 场景 | 状态 | 当前行为/条件 | 代表测试 |
|---|---|---|---|
| Eligibility/RBO/CBO 拒绝 | `serial-fallback` | 保留串行计划并记录原因 | `pq_optimizer_trace`、`pq_cbo` |
| Clone/refix 早期失败 | `partial` | `parallel_graceful_fallback` 开启时可恢复当前串行计划 | `pq_fallback` |
| Leader 后期改写/初始化失败 | `partial` | 可能抛 `ER_PARALLEL_FAIL_INIT`，满足条件时完整串行 retry | `pq_auto_retry_failed_parallel_query` |
| Worker 运行期错误 | `rejected` | 终止 PQ，传播错误并 join workers；不得继续返回部分成功 | `pq_worker_error`、`pq_mq_error` |
| 客户端已收到部分结果后的 retry | `rejected` | 不安全，必须禁止 | lifecycle contract |
| KILL QUERY/CONNECTION | `implemented` | leader 向 worker 传播 kill 并清理 | `pq_kill`、`pq_kill_query` |
| LIMIT 早停 | `partial` | leader 必须 detach queues，避免 sender 永久阻塞 | order/limit and lifecycle tests |

## 11. 组合测试要求

单个 `implemented` 行不能推出任意组合都支持。发布或扩大支持范围时至少交叉：

```text
PQ OFF / accepted / rejected / fallback / runtime error
× DOP 1 / 2 / 4 / 8 / DOP > contexts
× full / clustered / secondary / ref / reverse / ICP
× empty / NULL / duplicate / skew / wide row
× autocommit / explicit transaction / isolation level
× no concurrency / DML / purge / DDL / KILL / disconnect
```

Requirement 和测试映射见 `quality/requirements_test_traceability.md`。
