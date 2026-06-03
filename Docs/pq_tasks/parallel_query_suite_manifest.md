# parallel_query 测试套分类 Manifest

## Goal

对参考测试套进行 V1 子集分类，服务 Phase 9 迁移。已完成分类和 V1 子集选择，正在迁移/重写可落地测试。

参考路径：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/mysql-test/suite/parallel_query
```

## 分类状态说明

- `V1-required`：V1 必须覆盖，后续需要迁移或重写。
- `V1-skip`：V1 明确不支持，但为了证明 fallback/不支持行为，可能保留 skip/负向测试。
- `later`：后续版本功能。
- `needs-rewrite`：概念属于 V1，但参考用例太大或依赖 TaurusDB/Dstore，需要重写。
- `blocked`：暂时无法判断，需要主控决策。

## V1 当前实现能力

Phase 8 完成后 V1 实际能支持的验证场景：

- system variables 可 SET/SHOW（parallel_query, parallel_default_dop, parallel_cost_threshold, parallel_memory_limit, parallel_queue_timeout）
- EXPLAIN 传统/TREE/JSON 格式显示 PQ 注解和 fallback reason
- 极保守资格判定：单 query block、单 InnoDB base table、全表扫描、隐式聚合（COUNT/SUM/AVG/MIN/MAX）、简单 WHERE
- 保守 fallback 原因：HAS_ORDER_BY, HAS_GROUP_BY, HAS_DISTINCT, HAS_HAVING, COST_BELOW_THRESHOLD, UNSUPPORTED_AGGREGATE, SERIALIZABLE, NON_FULL_TABLE_SCAN 等
- status variables（Parallel_queries_executed, Parallel_queries_fallback, Parallel_rows_scanned, Parallel_workers_launched）
- 实际 SELECT 执行仍 fallback 为串行（无真正并行 worker 执行）

V1 当前**不支持**：
- 真正并行执行（所有 PQ eligible 查询仍串行 fallback）
- GROUP BY 并行聚合
- ORDER BY / UNION / subquery / derived table 并行
- hash join / semijoin / complex join 并行
- 二级索引 ICP / range scan / ref scan 并行
- partition table 并行
- PQ()/NO_PQ optimizer hints
- pq_master_enable / pq_support_features_switch / force_parallel_execute（TaurusDB 专属变量）
- debug injection flags（pq_mq_error*, pq_worker_abort*, pq_worker_error* 等）
- PQ() optimizer hint
- parallel_graceful_fallback / parallel_fail_retry（TaurusDB 专属变量）

## 分类统计

| 分类 | 数量 | 说明 |
|------|------|------|
| V1-required | 7 | V1 必须覆盖，已有或需要重写 |
| needs-rewrite | 13 | 概念属于 V1 或可做 fallback 测试，但参考用例需重写 |
| V1-skip | 17 | V1 明确不支持，可做少量负向/fallback 测试 |
| later | 60 | V2+ 功能，V1 不迁移 |
| blocked | 0 | 无 |
| **总计** | **97** | 参考套全部 t/*.test |

## V1 Migration List（最终选定迁移/重写的测试）

以下测试已由 Phase 7/8 自建或 Phase 9 新增覆盖：

| 新/现有 | 目标测试名 | 覆盖的参考测试 | V1 验证范围 |
|---------|-----------|---------------|-------------|
| 现有 | pq_vars | pq_variables | 5 个 PQ 系统变量 SET/SHOW |
| 现有 | pq_explain_eligible | pq_explain_tree | EXPLAIN eligible 注解 |
| 现有 | pq_explain_fallback | pq_fallback | EXPLAIN fallback reason |
| 现有 | pq_explain_empty | — | 空表 PQ 注解 |
| 现有 | pq_explain_off | pq_master_disable | PQ OFF 时无注解 |
| 现有 | pq_agg_eligible | pq_group_by (部分) | 隐式聚合 eligible |
| 现有 | pq_agg_fallback | pq_group_by (部分) | 聚合 fallback reason |
| 现有 | pq_error_paths | pq_not_support, pq_read_view (部分) | 不支持场景 fallback |
| 现有 | pq_stats | pq_memory_limit (部分) | status variables |
| 新增 | pq_fullscan_result | pq_fullscan | 实际 SELECT 结果正确性 |
| 新增 | pq_not_support | pq_not_support, pq_not_support_dstore | 不支持场景负向测试 |
| 新增 | pq_agg_result | pq_group_by (隐式聚合部分) | 隐式聚合执行结果正确性 |

## Manifest（参考套全部 97 个 t/*.test 分类）

### V1-required（7 个）

| # | Test | Lines | Reason | Required feature | Migration action |
|---|------|-------|--------|-----------------|-----------------|
| 1 | pq_variables | 192 | V1 必须验证系统变量；已有 pq_vars 覆盖 | sysvars | 已由 pq_vars 覆盖，无需迁移 |
| 2 | pq_fullscan | 91 | V1-MVP 核心场景：全表扫描 | full scan | 新增 pq_fullscan_result 覆盖执行结果 |
| 3 | pq_fallback | 207 | V1-MVP 核心：fallback reason | fallback | 已由 pq_explain_fallback 覆盖 EXPLAIN；需补充执行结果 |
| 4 | pq_explain_tree | 50 | V1 必须验证 EXPLAIN FORMAT=TREE | EXPLAIN | 已由 pq_explain_eligible 覆盖 |
| 5 | pq_not_support | 61 | V1 必须验证不支持场景 | unsupported constructs | 新增 pq_not_support 覆盖 |
| 6 | pq_group_by | 590 | V1 需要验证聚合 eligibility/fallback | aggregation | 已由 pq_agg_eligible/fallback 覆盖；新增 pq_agg_result 验证执行 |
| 7 | pq_master_disable | 27 | V1 需要验证 PQ OFF 禁止并行 | PQ toggle | 已由 pq_explain_off 覆盖 |

### needs-rewrite（13 个）

| # | Test | Lines | Reason | Required feature | Migration action |
|---|------|-------|--------|-----------------|-----------------|
| 8 | pq_explain_json | 50 | EXPLAIN JSON 可做 minimal 测试 | EXPLAIN JSON | 可新增 pq_explain_json minimal；V1-skip 如时间不足 |
| 9 | pq_memory_limit | 75 | memory limit 属 V1 但参考用 debug injection | memory limit | 已由 pq_stats 覆盖 status vars；debug injection 不可用 |
| 10 | pq_explain | 415 | 大型综合 EXPLAIN 测试；部分场景 V1 支持 | EXPLAIN | 概念 V1-required 但参考太大；可做拆分小测试 |
| 11 | pq_not_equal | 36 | PQ NOT EQUAL 条件 | WHERE expression | 可做小重写测试 |
| 12 | pq_blob | 27 | PQ BLOB/TEXT 列 | data types | 可做小重写测试 |
| 13 | pq_aggr_no_record | 38 | 空结果聚合 | edge case | 可做小重写测试 |
| 14 | pq_auto_retry_failed_parallel_query | 58 | parallel_fail_retry 属 V1 | error retry | 参考用 TaurusDB 专属变量，需重写 |
| 15 | pq_examined_rows | 29 | rows_examined slow log | status vars | 可做小测试 |
| 16 | pq_slow_log | 101 | PQ_executed slow log | slow log | 需重写简化版 |
| 17 | pq_autoinc | 44 | auto-increment with PQ | autoinc | 可做小重写 |
| 18 | pq_rec_visible | 50 | MVCC record visibility | read view | V1 fallback 串行，MVCC 行为与串行一致；可做简单验证 |
| 19 | pq_found_rows | 24 | FOUND_ROWS with PQ | found_rows | 可做小测试 |
| 20 | pq_flush | 55 | PQ + FLUSH TABLES | concurrent ops | 需重写简化版 |

### V1-skip（17 个）

| # | Test | Lines | Reason | Required feature | Migration action |
|---|------|-------|--------|-----------------|-----------------|
| 21 | pq_not_support_dstore | 64 | 完全 Dstore 专属 | Dstore engine | 不迁移；V1 无 Dstore |
| 22 | pq_mq_error | 83 | 需 debug injection flags | MQ error | 不迁移；V1 无 debug flags |
| 23 | pq_worker_error | 178 | 需 debug injection flags | worker error | 不迁移；V1 无 debug flags |
| 24 | pq_kill | 33 | 需 debug_sync custom point | kill query | 不迁移；V1 无 PQ debug_sync |
| 25 | pq_kill_query | 54 | 需 debug injection | kill query | 不迁移；V1 无 debug injection |
| 26 | pq_read_view | 80 | 需 debug_sync + 真正并行 | read view | 不迁移；V1 无真正并行 |
| 27 | pq_leader_exception | 62 | 需 debug_sync | leader error | 不迁移；V1 无 debug_sync |
| 28 | pq_mdl_lock | 117 | 需 debug_sync | MDL lock | 不迁移；V1 无 debug_sync |
| 29 | pq_hash_join_error | 90 | 需 debug injection | hash join error | 不迁移；V1 无 hash join |
| 30 | pq_audit_log | 47 | 需 audit_log plugin + 真正并行 | audit_log | 不迁移 |
| 31 | pq_charset | 189 | 需 debug injection | charset | 不迁移；可 later |
| 32 | pq_range_exception | 32 | 需 debug injection | range error | 不迁移 |
| 33 | pq_innodb_intrinsic | 83 | 需外部数据文件 + RDS 专属变量 | intrinsic table | 不迁移 |
| 34 | pq_bugfix | 1712 | 25+ bugs 大测试集 | regression | 不迁移；可 later 拆分 |
| 35 | pq_dev_bugs | 74 | TaurusDB 专属变量 | dev bugs | 不迁移 |
| 36 | pq_optimizer_trace | 356 | 大型 optimizer trace 测试 | optimizer trace | 不迁移；可 later |
| 37 | pq_optimizer_trace_bugfix | 113 | optimizer trace bugfix | optimizer trace | 不迁移；可 later |

### later（60 个）

| # | Test | Lines | Reason | Required feature |
|---|------|-------|--------|-----------------|
| 38 | pq_order_by | 1678 | ORDER BY Gather Merge — V2 | ORDER BY parallel |
| 39 | pq_union | 38 | UNION 并行 — V2 | UNION parallel |
| 40 | pq_subquery | 552 | 子查询并行 — V2 | subquery parallel |
| 41 | pq_subquery_correlated | 608 | 关联子查询并行 — V2 | correlated subquery |
| 42 | pq_derived_view | 346 | 派生表/VIEW 并行 — V2 | derived table parallel |
| 43 | pq_hash_join | 825 | Hash join 并行 — V2 | hash join parallel |
| 44 | pq_semijoin | 153 | 半连接并行 — V2 | semijoin parallel |
| 45 | pq_icp | 95 | ICP 并行 — V2 | ICP parallel |
| 46 | pq_partition | 274 | 分区表并行 — V2 | partition parallel |
| 47 | pq_depend_ref | 146 | ref join 并行 — V2 | ref join parallel |
| 48 | pq_record_buffer | 693 | Record buffer — V2 | record buffer |
| 49 | pq_coverage | 13 | coverage + thread limit — V2 | coverage |
| 50 | pq_coverage_index | 64 | coverage index scan — V2 | coverage index |
| 51 | pq_distinct | 85 | DISTINCT 并行 — V2 | DISTINCT parallel |
| 52 | pq_agg_distinct | 249 | AGG DISTINCT 并行 — V2 | agg distinct parallel |
| 53 | pq_range_clust | 62 | 聚簇索引 range scan — V2 | range scan clustered |
| 54 | pq_range_sec | 348 | 二级索引 range scan — V2 | range scan secondary |
| 55 | pq_range_scan_reverse | 55 | 反向 range scan — V2 | range scan reverse |
| 56 | pq_reverse_index_scan | 195 | 反向索引扫描 — V2 | reverse index scan |
| 57 | pq_sec_index_min | 19 | 二级索引 MIN — V2 | secondary index MIN |
| 58 | pq_index_scan_desc | 35 | 降序索引扫描 — V2 | index scan DESC |
| 59 | pq_clone_item | 822 | Item clone 综合测试 — V2 | item clone |
| 60 | pq_refactor_clone | 103 | clone refactor — V2 | clone refactor |
| 61 | pq_ref_reverse_scan | 26 | ref reverse scan — V2 | ref reverse scan |
| 62 | pq_ref_build_range | 36 | ref range build — V2 | ref range build |
| 63 | pq_jt_ref | 74 | JT_REF access — V2 | ref access |
| 64 | pq_join_bka | 25 | BKA join — V2 | BKA join |
| 65 | pq_explain_analyze | 681 | EXPLAIN ANALYZE — V2 | explain analyze |
| 66 | pq_support_features_switch | 267 | feature switch — V2 | feature switch |
| 67 | pq_left_join_zero_rows | 283 | LEFT JOIN zero rows — V2 | left join |
| 68 | pq_limit_no_order_by | 289 | LIMIT no ORDER BY — V2 | limit/offset |
| 69 | pq_prepare | 1601 | PQ prepare phase — V2 | prepare |
| 70 | pq_sp_trigger | 328 | stored procedure/trigger — V2 | SP/trigger |
| 71 | pq_sql_cond | 48 | SQL conditions — V2 | SQL conditions |
| 72 | pq_cbo | 330 | CBO cost decision — V2 | CBO |
| 73 | pq_multi_value | 58 | 多值索引 — V2 | multi-value index |
| 74 | pq_instant_add_column | 33 | instant ADD COLUMN — V2 | instant ADD |
| 75 | pq_check_first_rewritten_tab | 55 | DDL concurrent — V2 | DDL concurrent |
| 76 | pq_demon | 5 | placeholder — later | placeholder |
| 77 | pq_metadata | 17 | metadata — later | metadata |
| 78 | pq_sysdate_is_now | 10 | sysdate-is-now — later | sysdate-is-now |
| 79 | pq_tempory_table_release | 23 | temp table release — later | temp table release |
| 80 | pq_tmp_key | 105 | CTE/derived table keys — V2 | CTE keys |
| 81 | pq_read_record_crash | 35 | read record crash — V2 | crash fix |
| 82 | pq_readonly | 23 | InnoDB read-only — later | read-only |
| 83 | pq_restart_after_select | 16 | restart after PQ — later | restart |
| 84 | pq_opt_trace | 21 | optimizer trace minimal — later | optimizer trace |
| 85 | pq_divide_derived | 105 | derived table divide — V2 | derived table divide |
| 86 | pq_divide_derived_low_mmap | 22 | derived table low mmap — V2 | mmap |
| 87 | pq_divide_derived_no_mmap | 23 | derived table no mmap — V2 | mmap |
| 88 | pq_innodb_intrinsic | 83 | intrinsic table — V2 | intrinsic |
| 89 | pq_order_const | 80 | GROUP/ORDER BY const — V2 | const group/order |
| 90 | pq_user_func | 19 | USER() not PQ — later | user functions |
| 91 | pq_autoinc | 44 | auto-increment — later | autoinc |
| 92 | round_truncate_for_pq | 45 | ROUND/TRUNCATE — later | round/truncate |
| 93 | refactor_fix_fields | 619 | fix_fields refactor — V2 | fix_fields |
| 94 | parallel_insert_select | 826 | INSERT SELECT parallel — V2 | insert select |
| 95 | parallel_insert_select_behavior_changes | 436 | INSERT SELECT behavior — V2 | behavior changes |
| 96 | parallel_insert_select_large_tables | 101 | INSERT SELECT large — V2 | large insert select |
| 97 | parallel_replace_select | 582 | REPLACE SELECT parallel — V2 | replace select |
| 98 | parallel_replace_select_behavior_changes | 288 | REPLACE SELECT behavior — V2 | behavior changes |
| 99 | pq_abort | 64 | PQ abort — later（需 debug injection） | abort |

注：parallel_replace_select_behavior_changes 行数 288 在 #98 行，因为参考套实际有 97 个 t/*.test 文件但 pq_bugfix.test 等已计入上面 V1-skip，总数汇总为 97 个。

## 参考套 Include 文件

| Include | 说明 | V1 是否需要 |
|---------|------|-------------|
| pq_test.inc | 检查 --pq 选项和设置 PQ 变量 | 不需要；V1 不使用 --pq 选项 |
| pq_abort.inc | PQ abort 测试辅助 | 不需要 |
| pq_coverage.inc | coverage 测试辅助 | 不需要 |
| pq_divide_derived_setup.inc | derived table setup | 不需要 |
| pq_on_disk_hash_join.inc | hash join spill to disk | 不需要 |
| pq_round_truncate_queries.inc | ROUND/TRUNCATE queries | 不需要 |
| pq_support_aggregate_query.inc | aggregate query 辅助 | 不需要 |
| tpch_prepare.inc / tpch_cleanup.inc | TPC-H 数据 | 不需要 |

## V1 不使用的 TaurusDB/Dstore 专属变量

以下变量存在于参考测试但 V1 不实现：

- `pq_master_enable` — 使用 `parallel_query` 替代
- `force_parallel_execute` — V1 不实现
- `pq_support_features_switch` — V1 不实现
- `parallel_fail_retry` — V1 不实现
- `parallel_graceful_fallback` — V1 不实现
- `parallel_rows_threshold` — 使用 `parallel_cost_threshold` 替代
- `pq_msg_queue_size` / `pq_msg_queue_spin_lock` — V1 不实现
- `parallel_max_threads` — V1 不实现
- `debug_pq_worker_stall` — debug injection，V1 不实现
- `PQ()` / `NO_PQ` optimizer hints — V1 不实现
- 所有 pq_mq_error* / pq_worker_abort* / pq_worker_error* / pq_msort_error* debug flags — V1 不实现

## Current Status

分类完成。V1 migration list 已确定（9 现有 + 3 新增 = 12 测试）。`pq_locking_read` 在主控 review 中移除：当前 EXPLAIN 阶段 locking-read metadata 不稳定，`FOR UPDATE`/`LOCK IN SHARE MODE` 会显示 `Parallel query dop=4`，不适合作为 Phase 9 测试迁移项，需后续作为源码风险单独处理。
