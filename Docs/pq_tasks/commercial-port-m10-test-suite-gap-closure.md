# M10 Commercial Test Suite Gap Closure Taskbook

## 状态

M10-A manifest diff completed / Docs-Test Review accepted。
M10-B1 sysvars/fullscan edge small rewrite completed / Code-Docs-Test Review
accepted。
M10-B2 EXPLAIN JSON/TREE and fallback counter minimal tests completed /
Code-Docs-Test Review accepted。
M10-C1 supported/deferred subset declaration completed / Docs-Task Review
accepted。

## 目标

对齐 taurusdbondstore `mysql-test/suite/parallel_query` 商用测试套，完成缺口分类、迁移、skip/fallback 说明和当前分支测试闭环。

推荐拆分：

- M10-A: 测试 manifest：enabled / adapted / deferred；
- M10-B: 基础商用套件名称对齐，如 `pq_variables` vs 当前 `pq_vars`；
- M10-C: 按功能启用 aggregation、ORDER BY、ref/ICP，其他 deferred；
- M10-D: 完整 `parallel_query` suite clean run。

## M10-A: Commercial Suite Manifest

Status: completed / Docs-Test Review accepted。

本阶段只做只读差异统计和文档分类，不启用新测试、不修改源码、不复制
商用 `.result-pq`。

### 输入路径

当前仓库：

```text
mysql-test/suite/parallel_query/t/*.test
```

商用参考仓：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/mysql-test/suite/parallel_query/t/*.test
```

### 数量统计

| 项 | 数量 |
|---|---:|
| 当前 `t/*.test` | 73 |
| 商用参考 `t/*.test` | 98 |
| 同名重合 | 1 |
| 当前独有 | 72 |
| 商用独有 | 97 |

同名重合只有 `pq_not_support`。因此 M10 不能按文件名直接判断覆盖关系，
必须按 capability 建立 `enabled / adapted / deferred` 映射。

Review accepted 后的 E/A/D 统计：

| Classification | Count |
|---|---:|
| enabled | 1 |
| adapted | 33 |
| deferred | 64 |

### 商用独有测试分类

| 分类 | 数量 | 测试 |
|---|---:|---|
| sysvars | 5 | `pq_variables`, `pq_master_disable`, `pq_support_features_switch`, `pq_memory_limit`, `pq_replica_enable` |
| fallback | 5 | `pq_fallback`, `pq_auto_retry_failed_parallel_query`, `pq_coverage`, `pq_not_support_dstore`, `pq_cbo` |
| explain | 4 | `pq_explain`, `pq_explain_tree`, `pq_explain_json`, `pq_explain_analyze` |
| fullscan | 7 | `pq_fullscan`, `pq_not_equal`, `pq_blob`, `pq_found_rows`, `pq_autoinc`, `pq_rec_visible`, `pq_flush` |
| aggregation | 6 | `pq_group_by`, `pq_agg_distinct`, `pq_aggr_no_record`, `pq_distinct`, `pq_order_const`, `round_truncate_for_pq` |
| order-by | 2 | `pq_order_by`, `pq_limit_no_order_by` |
| ref-icp | 5 | `pq_icp`, `pq_depend_ref`, `pq_jt_ref`, `pq_ref_build_range`, `pq_join_bka` |
| secondary-range | 4 | `pq_range_clust`, `pq_range_sec`, `pq_coverage_index`, `pq_sec_index_min` |
| partition | 1 | `pq_partition` |
| record-buffer | 1 | `pq_record_buffer` |
| reverse | 4 | `pq_range_scan_reverse`, `pq_reverse_index_scan`, `pq_ref_reverse_scan`, `pq_index_scan_desc` |
| mvi | 1 | `pq_multi_value` |
| subquery-union-derived | 8 | `pq_union`, `pq_subquery`, `pq_subquery_correlated`, `pq_derived_view`, `pq_divide_derived`, `pq_divide_derived_low_mmap`, `pq_divide_derived_no_mmap`, `pq_tmp_key` |
| hashjoin-semijoin | 3 | `pq_hash_join`, `pq_hash_join_error`, `pq_semijoin` |
| dml | 7 | `parallel_insert_select`, `parallel_insert_select_behavior_changes`, `parallel_insert_select_large_tables`, `parallel_replace_select`, `parallel_replace_select_behavior_changes`, `pq_sp_trigger`, `pq_instant_add_column` |
| diagnostics-debug | 18 | `pq_abort`, `pq_audit_log`, `pq_bugfix`, `pq_charset`, `pq_clone_item`, `pq_dev_bugs`, `pq_examined_rows`, `pq_kill`, `pq_kill_query`, `pq_leader_exception`, `pq_mdl_lock`, `pq_mq_error`, `pq_optimizer_trace`, `pq_optimizer_trace_bugfix`, `pq_range_exception`, `pq_read_view`, `pq_slow_log`, `pq_worker_error` |
| other | 16 | `pq_check_first_rewritten_tab`, `pq_demon`, `pq_innodb_intrinsic`, `pq_left_join_zero_rows`, `pq_metadata`, `pq_opt_trace`, `pq_prepare`, `pq_read_record_crash`, `pq_readonly`, `pq_refactor_clone`, `pq_restart_after_select`, `pq_sql_cond`, `pq_sysdate_is_now`, `pq_tempory_table_release`, `pq_user_func`, `refactor_fix_fields` |

### Enabled / Adapted / Deferred

Enabled:

- 当前 73 个本仓 `parallel_query` tests 继续作为 enabled local guards；
- 商用同名 enabled 目前只有 `pq_not_support`；
- 完整 suite 最近记录为 74/74 通过，其中额外 1 项来自 MTR
  `shutdown_report`。

Adapted:

| 商用测试 | 当前覆盖 | M10 处理 |
|---|---|---|
| `pq_variables` | `pq_vars` | M10-B 名称/变量语义对齐候选 |
| `pq_master_disable` | `pq_explain_off` | M10-B 小型改写候选 |
| `pq_memory_limit` | `pq_stats`, 当前 sysvar/status guards | M10-B 只迁移可稳定验证子集 |
| `pq_fullscan` | `pq_fullscan_result`, `pq_commercial_fullscan`, threaded fullscan tests | M10-B/M10-C 分层补齐 |
| `pq_fallback` | `pq_explain_fallback`, `pq_error_paths`, `pq_locking_read_fallback` | M10-B 小型改写候选 |
| `pq_explain`, `pq_explain_tree`, `pq_explain_json` | `pq_explain_*` | M10-B 优先补 JSON/tree 差异 |
| `pq_group_by` | `pq_agg_*`, `pq_groupby_*` | M10-C 只启用非 DISTINCT / 已支持 partial subset |
| `pq_order_by` | `pq_commercial_order_by` | M10-C 只保留当前 ORDER BY/Gather Merge 窄子集 |
| `pq_icp` | `pq_commercial_ref_icp` | M10-C 只启用 non-covering ICP range 已支持子集 |
| `pq_range_sec` | `pq_commercial_ref_icp` | M10-C 只启用 strict covering integer secondary range 子集 |
| `pq_jt_ref` | `pq_commercial_ref_icp` | M10-C 只启用 constant covering ref 子集 |
| `pq_depend_ref` | `pq_commercial_ref_icp` | M10-C 只启用 leader-local dependent ref 子集 |
| `pq_worker_error`, `pq_kill`, `pq_read_view`, `pq_mdl_lock` | `pq_read_threaded_*` guards | 保留当前 8.0.46 适配测试，不原样迁移商用 debug tests |

Deferred:

| 分类 | 商用测试 | 原因 |
|---|---|---|
| partition | `pq_partition` | 当前只做 guard/contract，尚未打开 partition positive path |
| record-buffer | `pq_record_buffer` | 当前只完成 native RecordBuffer diagnostics，performance adapter 未设计/实现 |
| reverse | `pq_range_scan_reverse`, `pq_reverse_index_scan`, `pq_ref_reverse_scan`, `pq_index_scan_desc` | 当前只 guard，未打开 reverse positive path |
| mvi | `pq_multi_value` | 当前只 guard，MVI unique filter/materialization contract 未实现 |
| subquery/union/derived | `pq_union`, `pq_subquery*`, `pq_derived_view`, `pq_divide_derived*`, `pq_tmp_key` | worker plan clone/resolver/subquery execution 尚未完整平移 |
| hashjoin/semijoin | `pq_hash_join*`, `pq_semijoin` | hash join shared context / semijoin materialization 未平移 |
| DML | `parallel_insert_select*`, `parallel_replace_select*`, `pq_sp_trigger`, `pq_instant_add_column` | 当前目标仍是 SELECT PQ，DML/DDL 语义未纳入 |
| distinct/agg-distinct | `pq_distinct`, `pq_agg_distinct` | DISTINCT / AGG DISTINCT 并行未打开 |
| prepare/fix-fields | `pq_prepare`, `refactor_fix_fields`, `pq_refactor_clone`, `pq_clone_item` | 完整商用 clone/fix_fields/refactor 仍未平移 |
| TaurusDB/Dstore/debug | `pq_not_support_dstore`, `pq_auto_retry_failed_parallel_query`, `pq_range_exception`, `pq_leader_exception`, `pq_abort`, `pq_audit_log`, `pq_replica_enable`, `pq_support_features_switch` | 依赖 TaurusDB/Dstore 变量、插件或 debug injection，不可原样迁移；`pq_mq_error` 原样商用 debug 用例 deferred，但当前 worker/MQ error 窄子集按 adapted 跟踪 |
| optimizer trace / diagnostics | `pq_optimizer_trace*`, `pq_opt_trace`, `pq_bugfix`, `pq_dev_bugs`, `pq_slow_log`, `pq_examined_rows` | 需要按当前 8.0.46 输出重写，不能复制商用 result |

### Complete Commercial Test E/A/D Matrix

| 商用测试 | capability | M10 classification | 当前覆盖或 deferred reason | 后续阶段 |
|---|---|---|---|---|
| `pq_not_support` | unsupported guard | enabled | 当前同名测试已启用 | M10-D clean run |
| `pq_variables` | sysvars | adapted | `pq_vars` 覆盖当前变量语义 | M10-B |
| `pq_master_disable` | sysvars/toggle | adapted | `pq_explain_off` 覆盖 PQ OFF | M10-B |
| `pq_memory_limit` | sysvars/status | adapted | `pq_stats` 覆盖 status，memory 子集需重写 | M10-B |
| `pq_fullscan` | fullscan | adapted | `pq_fullscan_result` / `pq_commercial_fullscan` | M10-B |
| `pq_fallback` | fallback | adapted | `pq_explain_fallback` / `pq_error_paths` | M10-B |
| `pq_explain` | explain | adapted | 当前 `pq_explain_*` 覆盖基础口径 | M10-B |
| `pq_explain_tree` | explain tree | adapted | `pq_explain_eligible` 覆盖 TREE | M10-B |
| `pq_explain_json` | explain json | adapted | 需要按 8.0.46 JSON 输出重写 | M10-B |
| `pq_not_equal` | fullscan predicate | adapted | 可用当前 fullscan/WHERE path 小型改写 | M10-B |
| `pq_blob` | datatype guard | adapted | 当前 BLOB 多为 fallback/guard，需小型改写 | M10-B |
| `pq_aggr_no_record` | aggregate empty | adapted | 可用当前 aggregate/fallback path 小型改写 | M10-B |
| `pq_found_rows` | found rows | adapted | 可按当前执行语义小型改写 | M10-B |
| `pq_autoinc` | autoinc fullscan | adapted | 可按当前 fullscan/fallback 小型改写 | M10-B |
| `pq_rec_visible` | MVCC visibility | adapted | 当前 read-view guards 可覆盖窄子集 | M10-B |
| `pq_flush` | FLUSH interaction | adapted | 可做小型并发/metadata guard | M10-B |
| `pq_group_by` | GROUP BY | adapted | 当前 `pq_groupby_*` 覆盖非 DISTINCT 子集 | M10-C |
| `pq_order_by` | ORDER BY | adapted | `pq_commercial_order_by` 覆盖窄子集 | M10-C |
| `pq_icp` | ICP | adapted | `pq_commercial_ref_icp` 覆盖 non-covering ICP range 子集 | M10-C |
| `pq_range_sec` | secondary range | adapted | `pq_commercial_ref_icp` 覆盖 strict covering integer range 子集 | M10-C |
| `pq_jt_ref` | constant ref | adapted | `pq_commercial_ref_icp` 覆盖 constant covering ref 子集 | M10-C |
| `pq_depend_ref` | dependent ref | adapted | `pq_commercial_ref_icp` 覆盖 leader-local dependent ref 子集 | M10-C |
| `pq_range_clust` | clustered range | adapted | 可先做 fallback 或窄正例 | M10-C |
| `pq_coverage_index` | index coverage | adapted | 可先做 fallback 或 secondary coverage 子集 | M10-C |
| `pq_sec_index_min` | secondary MIN | adapted | M9-F4 已记录 optimizer shortcut/guard | M10-C |
| `pq_join_bka` | BKA join | deferred | BKA join PQ 未平移 | post-M10 |
| `pq_ref_build_range` | ref range build | deferred | ref range builder 商用路径未完整平移 | post-M10 |
| `pq_limit_no_order_by` | LIMIT | adapted | 当前 limit counters guards 可覆盖窄子集 | M10-C |
| `pq_order_const` | const group/order | adapted | 可作为 GROUP/ORDER 窄子集改写 | M10-C |
| `round_truncate_for_pq` | scalar functions | adapted | 可作为 projection expression 小型改写 | M10-B |
| `pq_cbo` | cost model | deferred | 商用 CBO/rows threshold 语义未完整平移 | post-M10 |
| `pq_coverage` | coverage/thread limit | deferred | 商用 coverage/thread-limit 语义未完整平移 | post-M10 |
| `pq_auto_retry_failed_parallel_query` | auto retry | deferred | 依赖商用 retry/failover 变量 | post-M10 |
| `pq_not_support_dstore` | Dstore | deferred | Dstore 专属 | none |
| `pq_support_features_switch` | feature switch | deferred | 商用 feature switch 未平移 | post-M10 |
| `pq_replica_enable` | replica | deferred | replica/商用变量未纳入当前目标 | post-M10 |
| `pq_explain_analyze` | EXPLAIN ANALYZE | deferred | explain analyze PQ 展示未完整平移 | post-M10 |
| `pq_agg_distinct` | aggregate DISTINCT | deferred | AGG DISTINCT 并行未打开 | post-M10 |
| `pq_distinct` | DISTINCT | deferred | DISTINCT 并行未打开 | post-M10 |
| `pq_partition` | partition | deferred | partition positive path 未打开 | post-M10 |
| `pq_record_buffer` | native RecordBuffer | deferred | 只完成 diagnostics，adapter 未实现 | post-M10 |
| `pq_multi_value` | MVI | deferred | MVI positive unique filter 未实现 | post-M10 |
| `pq_range_scan_reverse` | reverse range | deferred | reverse positive path 未打开 | post-M10 |
| `pq_reverse_index_scan` | reverse index | deferred | reverse positive path 未打开 | post-M10 |
| `pq_ref_reverse_scan` | reverse ref | deferred | reverse positive path 未打开 | post-M10 |
| `pq_index_scan_desc` | descending index | deferred | descending/reverse positive path 未打开 | post-M10 |
| `pq_union` | UNION | deferred | UNION plan clone/execution 未平移 | post-M10 |
| `pq_subquery` | subquery | deferred | subquery plan clone/execution 未平移 | post-M10 |
| `pq_subquery_correlated` | correlated subquery | deferred | correlated subquery/NST clone 未平移 | post-M10 |
| `pq_derived_view` | derived/view | deferred | derived/view PQ 未平移 | post-M10 |
| `pq_divide_derived` | derived divide | deferred | derived divide/mmap path 未平移 | post-M10 |
| `pq_divide_derived_low_mmap` | derived mmap | deferred | derived divide/mmap path 未平移 | post-M10 |
| `pq_divide_derived_no_mmap` | derived mmap | deferred | derived divide/mmap path 未平移 | post-M10 |
| `pq_tmp_key` | CTE/derived temp key | deferred | derived/temporary key PQ 未平移 | post-M10 |
| `pq_hash_join` | hash join | deferred | hash join shared context 未平移 | post-M10 |
| `pq_hash_join_error` | hash join error | deferred | hash join shared context/error path 未平移 | post-M10 |
| `pq_semijoin` | semijoin | deferred | semijoin materialization/thread-pool 未平移 | post-M10 |
| `parallel_insert_select` | DML | deferred | INSERT SELECT PQ 不在当前 SELECT 闭环 | post-M10 |
| `parallel_insert_select_behavior_changes` | DML | deferred | INSERT SELECT behavior 未纳入 | post-M10 |
| `parallel_insert_select_large_tables` | DML | deferred | INSERT SELECT large table 未纳入 | post-M10 |
| `parallel_replace_select` | DML | deferred | REPLACE SELECT PQ 不在当前 SELECT 闭环 | post-M10 |
| `parallel_replace_select_behavior_changes` | DML | deferred | REPLACE SELECT behavior 未纳入 | post-M10 |
| `pq_sp_trigger` | SP/trigger | deferred | stored procedure / trigger PQ 未平移 | post-M10 |
| `pq_instant_add_column` | DDL | deferred | instant ADD COLUMN 并发语义未纳入 | post-M10 |
| `pq_prepare` | prepare | deferred | 商用 prepare/fix_fields path 未平移 | post-M10 |
| `refactor_fix_fields` | fix_fields | deferred | fix_fields refactor 未平移 | post-M10 |
| `pq_refactor_clone` | clone refactor | deferred | clone refactor 未平移 | post-M10 |
| `pq_clone_item` | item clone | deferred | 完整 Item clone 测试未平移 | post-M10 |
| `pq_abort` | abort/debug | deferred | 依赖商用 debug injection | post-M10 |
| `pq_audit_log` | audit | deferred | audit plugin/商用输出未纳入 | post-M10 |
| `pq_bugfix` | bugfix mega-suite | deferred | 需要拆分成当前能力小用例 | post-M10 |
| `pq_charset` | charset/debug | deferred | 依赖商用 debug/广泛 charset matrix | post-M10 |
| `pq_dev_bugs` | dev bugfix | deferred | TaurusDB 专属回归 | post-M10 |
| `pq_examined_rows` | diagnostics | deferred | rows_examined/slow-log 需按 8.0.46 重写 | post-M10 |
| `pq_kill` | kill/debug | adapted | 当前 `pq_read_threaded_*kill*` 覆盖 8.0.46 guards | M10-C/M10-D |
| `pq_kill_query` | kill/debug | adapted | 当前 external KILL guards 覆盖窄子集 | M10-C/M10-D |
| `pq_leader_exception` | debug exception | deferred | 依赖商用 debug injection | post-M10 |
| `pq_mdl_lock` | MDL concurrency | adapted | 当前 `pq_read_threaded_mdl_concurrency` 覆盖窄子集 | M10-D |
| `pq_mq_error` | MQ error/debug | adapted | 当前 worker/MQ error guards 覆盖窄子集 | M10-D |
| `pq_optimizer_trace` | optimizer trace | deferred | trace 输出需按 8.0.46 重写 | post-M10 |
| `pq_optimizer_trace_bugfix` | optimizer trace | deferred | trace 输出需按 8.0.46 重写 | post-M10 |
| `pq_range_exception` | range error/debug | deferred | 依赖商用 debug injection | post-M10 |
| `pq_read_view` | read view | adapted | 当前 read-view concurrency guards 覆盖窄子集 | M10-D |
| `pq_slow_log` | slow log | deferred | slow-log 输出需按 8.0.46 重写 | post-M10 |
| `pq_worker_error` | worker error | adapted | 当前 worker ERROR guards 覆盖窄子集 | M10-D |
| `pq_check_first_rewritten_tab` | DDL/concurrency | deferred | first rewritten tab / DDL 并发未平移 | post-M10 |
| `pq_demon` | placeholder | deferred | placeholder，无当前迁移价值 | none |
| `pq_innodb_intrinsic` | intrinsic table | deferred | 依赖外部数据/商用变量 | post-M10 |
| `pq_left_join_zero_rows` | left join | deferred | multi-table/left join PQ 未平移 | post-M10 |
| `pq_metadata` | metadata | deferred | 商用 metadata 语义未纳入 | post-M10 |
| `pq_opt_trace` | optimizer trace | deferred | trace 输出需按 8.0.46 重写 | post-M10 |
| `pq_read_record_crash` | crash regression | deferred | 需按当前实现复现后重写 | post-M10 |
| `pq_readonly` | readonly | deferred | InnoDB read-only path 未纳入 | post-M10 |
| `pq_restart_after_select` | restart | deferred | restart after select 商用语义未纳入 | post-M10 |
| `pq_sql_cond` | SQL condition | deferred | SQL condition 并行语义未平移 | post-M10 |
| `pq_sysdate_is_now` | sysdate | deferred | sysdate-is-now 特例未纳入 | post-M10 |
| `pq_tempory_table_release` | temp table | deferred | temporary table release path 未平移 | post-M10 |
| `pq_user_func` | user func | deferred | user function PQ 语义未纳入 | post-M10 |

### 后续执行顺序

M10-B candidates:

- `pq_variables` -> `pq_vars`；
- `pq_master_disable` -> `pq_explain_off`；
- `pq_memory_limit` -> `pq_stats`/status guard 子集；
- `pq_fullscan` -> `pq_fullscan_result` / `pq_commercial_fullscan`；
- `pq_fallback` -> `pq_explain_fallback` / `pq_error_paths`；
- `pq_explain_tree` / `pq_explain_json` -> 小型 EXPLAIN 改写；
- `pq_not_equal`, `pq_blob`, `pq_aggr_no_record`, `pq_found_rows`,
  `pq_autoinc`, `pq_rec_visible`, `pq_flush` -> 小型 fullscan/fallback 改写。

M10-C candidates:

- `pq_group_by` 非 DISTINCT 子集；
- `pq_order_by` 当前 Gather Merge 窄子集；
- `pq_icp` non-covering ICP range 子集；
- `pq_range_sec` strict covering integer secondary range 子集；
- `pq_jt_ref` constant covering ref 子集；
- `pq_depend_ref` leader-local dependent ref 子集；
- `pq_range_clust` / `pq_coverage_index` 先做 fallback 或窄正例。

M10-D:

- 完整 `parallel_query` suite clean run；
- 记录最终 enabled/adapted/deferred 数量；
- 若新增测试导致当前 suite 数量变化，更新 README 和本任务书。

## M10-C1: Supported / Deferred Subset Declaration

Status: completed / Docs-Task Review accepted。

本阶段只收敛 M10-C 的 capability 口径，不新增测试、不改源码。
M10-C 不应把当前分支的 legacy / experimental path 误称为商用完整
aggregation、ORDER BY 或 ref/ICP 实现。

### Aggregation

Current active/adapted subset:

- implicit aggregate without explicit `GROUP BY`：
  `COUNT/SUM/AVG/MIN/MAX`，结果正确性由 `pq_agg_*` 和 threaded aggregate
  tests 覆盖；
- explicit `GROUP BY` legacy typed-state experimental subset：
  - single InnoDB table；
  - single direct integer group key；
  - single aggregate；
  - `COUNT(*)`、`COUNT(field)`、`SUM/MIN/MAX(integer field)`；
  - DOP=1 使用 `parallel_query_experimental_groupby_dop1`；
  - DOP=2/DOP=4 依赖 threaded DOP gate；
  - current tests: `pq_groupby_*`；
- unsupported aggregation guard：
  `HAVING`、`COUNT(DISTINCT ...)`、unsupported aggregate、expression group
  key 等继续 fallback/deferred。

Important boundary:

- `Parallel_groupby_commercial_selected` 和
  `Parallel_groupby_commercial_executed` 当前必须保持 0；
- 当前可声明的是 legacy typed-state / experimental subset，不是 commercial
  worker-plan aggregation path。

Deferred:

- real commercial aggregation through worker plan / `Query_result_mq`；
- multi-key GROUP BY；
- `AVG(...) GROUP BY` positive path；
- decimal/double/timestamp typed GROUP BY positive path；
- `DISTINCT` / aggregate DISTINCT；
- `HAVING` positive path；
- join/subquery/derived GROUP BY；
- GROUP BY + ORDER BY / LIMIT merge。

### ORDER BY

Current active/adapted subset:

- `pq_commercial_order_by` only covers synthetic `Exchange_sort` smoke and
  serial boundary；
- real user-visible ORDER BY still falls back with `HAS_ORDER_BY`；
- true ORDER BY should not increase `Parallel_queries_executed` or
  `Parallel_queries_fallback` in current boundary tests。

Deferred:

- real ORDER BY worker output / Gather Merge；
- expression ORDER BY；
- multi-column ASC/DESC；
- index-order / descending-index path；
- tie-break / rowid stability；
- ORDER BY + LIMIT / GROUP BY / aggregate merge；
- commercial `pq_msort_error*` debug injection cases。

### Ref / ICP / Secondary Range

Current active/adapted subset is leader-local, bounded, and no-worker/no-MQ:

- fixed integer covering secondary half-open range；
- fixed integer constant covering secondary ref；
- fixed integer two-table dependent covering secondary ref；
- fixed integer non-covering secondary range ICP with clustered
  materialization；
- current coverage lives in `pq_commercial_ref_icp` with positive counters and
  fallback windows。

Deferred:

- worker-side `PQRefIterator` / `ha_pq_next` / `Query_result_mq` ref path；
- ref ICP / dependent-ref ICP / non-covering ref ICP；
- multi-range / OR range；
- inclusive endpoint expansion beyond current safe subset；
- reverse / descending index；
- partition / MVI；
- spatial / nullable / varlen / string collation / prefix key；
- BLOB/JSON read-set-hostile secondary paths；
- BKA/hash/semi/derived/subquery dependent-ref commercial regressions。

### M10-C Next Implementation Candidates

Safe next test-only candidates:

- `pq_commercial_group_by_supported_subset`：documented as legacy
  typed-state experimental subset, with commercial counters asserted 0；
- `pq_commercial_group_by_deferred_boundary`：HAVING / DISTINCT /
  expression group key / GROUP BY + ORDER BY fallback boundaries；
- optional `pq_commercial_order_by_boundary`：only serial `HAS_ORDER_BY`
  boundary and synthetic `Exchange_sort` smoke, no positive real ORDER BY；
- optional ref/ICP additions should extend `pq_commercial_ref_icp` rather than
  create duplicate fixtures。

Not safe for M10-C first coding pass:

- claiming commercial aggregation selected/executed；
- opening real ORDER BY Gather Merge；
- copying commercial `pq_group_by`, `pq_order_by`, `pq_icp`, `pq_range_sec`,
  `pq_jt_ref`, or `pq_depend_ref` wholesale。

Review:

- Docs-Task Review Agent returned `ACCEPT`；
- confirmed aggregation boundary distinguishes legacy/experimental typed-state
  subset from commercial worker-plan aggregation；
- confirmed ORDER BY remains synthetic smoke / serial boundary only；
- confirmed ref/ICP/secondary range remains leader-local/no-worker/no-MQ
  subset；
- confirmed next test candidates are safe and do not invite wholesale
  commercial test copies。

## M10-B1: Sysvars And Fullscan Edge Small Rewrites

Status: completed / Code-Docs-Test Review accepted。

目标：

- 对齐商用 `pq_variables` 的当前分支可稳定变量边界；
- 对齐商用 `pq_not_equal`、`pq_aggr_no_record`、`pq_found_rows`、
  `pq_autoinc` 的小型稳定子集；
- 不迁移商用 debug、UNION、并发、hint、large matrix 内容；
- 不改源码。

实现：

- 扩展 `pq_vars`：
  - `parallel_default_dop` 边界：1 / 256；
  - 当前 8.0.46 sysvar 行为：0 / 257 被截断并产生 warning；
  - `parallel_memory_limit` 和 `parallel_queue_timeout` 可 SET/SHOW；
  - reset 后确认默认值恢复。
- 新增 `pq_commercial_fullscan_edges`：
  - `actor_id != 1`；
  - empty/non-empty `COUNT(*)` range aggregate；
  - simple `SQL_CALC_FOUND_ROWS` / `FOUND_ROWS()`；
  - AUTO_INCREMENT + duplicate key guard + point/fullscan select。

验证：

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_vars pq_commercial_fullscan_edges
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_vars pq_commercial_fullscan_edges
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

结果：

- targeted record/replay passed；
- full `parallel_query` suite passed: 75/75；
- no source edits, no build required。

## 允许修改

- `mysql-test/suite/parallel_query/**`
- `Docs/pq_tasks/commercial-port-m10-test-suite-gap-closure.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 先生成测试差异清单；
- V1/M1-M9 已支持能力迁移为 active tests；
- 未支持能力必须记录 skip 原因和目标阶段；
- 不把商用 `.result-pq` 原样复制成错误预期；
- enabled / adapted / deferred 分类必须可追踪到 capability；
- 完整 suite 必须可重复运行。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m10 --tmpdir=/tmp/pqt_m10
```

## Completion Report

M10-A:

- changed docs only；
- no source edits；
- no MTR tests enabled or modified；
- no MTR run, because this stage is docs-only manifest classification；
- Test Manifest Diff Agent returned `M10-A DIFF READY`；
- Docs-Test Review Agent returned `ACCEPT`；
- review confirmed matrix covers 98/98 commercial tests with no missing,
  extra, or duplicated test names。

M10-B1:

- changed MTR tests only plus task docs；
- targeted record/replay passed；
- full `parallel_query` suite passed: 75/75；
- Code-Docs-Test Review Agent returned `ACCEPT`；
- review confirmed `pq_vars` sysvar boundary behavior and
  `pq_commercial_fullscan_edges` stable commercial narrow subset coverage。

## M10-B2: EXPLAIN JSON/TREE And Fallback Counter Minimal Tests

Status: completed / Code-Docs-Test Review accepted。

目标：

- 对齐商用 `pq_explain_tree` / `pq_explain_json` 的当前稳定注解子集；
- 对齐商用 `pq_fallback` 的最小 counter contract；
- 不迁移完整商用 EXPLAIN result、hint、subquery、derived、hash join、
  semijoin 或 debug 注入内容。

实现：

- 新增 `pq_explain_json_tree_minimal`：
  - eligible TREE 显示 `parallel query eligible, execution disabled,
    serial fallback (dop=2)`；
  - eligible JSON 显示 `parallel_query_state=eligible` 和 dop=2 注解；
  - ORDER BY TREE 显示 `not parallel (HAS_ORDER_BY)`；
  - EXPLAIN 不增加 `Parallel_queries_executed` 或
    `Parallel_queries_fallback`。
- 新增 `pq_fallback_counters_minimal`：
  - eligible full scan 当前 safe fallback，`fullscan_fallback_delta=1`；
  - GROUP BY 与 locking read 属 ineligible，不增加 fallback counter；
  - `executed_delta=0`、`rows_delta=0`、`workers_delta=0`。

验证：

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record \
  pq_explain_json_tree_minimal pq_fallback_counters_minimal
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query \
  pq_explain_json_tree_minimal pq_fallback_counters_minimal
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

结果：

- targeted record/replay passed；
- full `parallel_query` suite passed: 77/77；
- no source edits, no build required。

Completion:

- changed MTR tests plus task docs；
- Code-Docs-Test Review Agent first returned `REVISE` for fragile EXPLAIN
  cost/row output and unsorted `FOR UPDATE` output；
- fixed TREE/JSON cost/row/data-read masking and added `--sorted_result` for
  `FOR UPDATE`；
- targeted record/replay passed after fixes；
- full `parallel_query` suite passed: 77/77；
- Code-Docs-Test Review Agent returned `ACCEPT`。
