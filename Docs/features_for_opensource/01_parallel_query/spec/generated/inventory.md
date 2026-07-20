# Parallel Query 生成清单

> **GENERATED — DO NOT EDIT.** 由 `spec/tools/build_handbook.py` 生成；
> 修改 manifest、当前规格源文档或源码/测试资产后必须重新生成。

## 1. 绑定状态

| 项目 | 值 |
|---|---|
| implementation commit | `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` |
| branch | `stable_branch` |
| bound PQ source/test tree | `clean` |
| captured at | `2026-07-20` |
| source hash scope | `sql/parallel_query/** plus storage/innobase/include/row0pread_pq.h, storage/innobase/row/row0pread_pq.cc, storage/innobase/handler/ha_innodb_pq.cc` |
| external source evidence | other SQL/InnoDB integration paths are commit-bound path::symbol static evidence only; they are not individually content-hashed |
| test hash scope | `mysql-test/suite/parallel_query/t/*.test plus r/*.result-pq on stable_branch` |
| build verified | `true` |
| MTR verified | `true` |

`dirty` 只表示 manifest 声明的 PQ source/test 路径存在未提交修改；
无关 build/log 文件不影响本清单。运行验证范围见
`current/quality/verification_evidence.md`，不得由生成动作自行推导。

## 2. 源码与测试统计

| 范围 | workspace | tracked | SHA-256 tree digest |
|---|---:|---:|---|
| `sql/parallel_query/` | 33 files / 17802 lines | n/a | `a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`（含下列 InnoDB PQ 文件） |
| InnoDB PQ core | 3 files / 2264 lines | n/a | 同上 |
| PQ `.test` | 98 | 98 | `066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`（与 result 合并） |
| PQ `.result-pq` | 98 | 98 | 同上 |

## 3. SQL/PQ 源文件

- `sql/parallel_query/barrier.h`
- `sql/parallel_query/binary_heap.h`
- `sql/parallel_query/bloom_filter.h`
- `sql/parallel_query/chunk_files_wrapper.h`
- `sql/parallel_query/exchange.cc`
- `sql/parallel_query/exchange.h`
- `sql/parallel_query/exchange_sort.cc`
- `sql/parallel_query/exchange_sort.h`
- `sql/parallel_query/explain_pq_access_path.cc`
- `sql/parallel_query/explain_pq_access_path.h`
- `sql/parallel_query/msg_queue.cc`
- `sql/parallel_query/msg_queue.h`
- `sql/parallel_query/pq_clone.cc`
- `sql/parallel_query/pq_clone.h`
- `sql/parallel_query/pq_clone_item.cc`
- `sql/parallel_query/pq_handler.cc`
- `sql/parallel_query/pq_handler.h`
- `sql/parallel_query/pq_hash_join_shared_context.cc`
- `sql/parallel_query/pq_hash_join_shared_context.h`
- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/pq_iterators.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/pq_optimizer.h`
- `sql/parallel_query/pq_refix_fields_item.cc`
- `sql/parallel_query/pq_replace_base_item.cc`
- `sql/parallel_query/pq_resolver.cc`
- `sql/parallel_query/pq_resolver.h`
- `sql/parallel_query/pq_resource_stat.cc`
- `sql/parallel_query/pq_resource_stat.h`
- `sql/parallel_query/query_result_mq.cc`
- `sql/parallel_query/query_result_mq.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/sql_parallel.h`

## 4. InnoDB PQ 核心文件

- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`

## 5. Workspace PQ 测试

- `mysql-test/suite/parallel_query/t/parallel_insert_select.test`
- `mysql-test/suite/parallel_query/t/parallel_insert_select_behavior_changes.test`
- `mysql-test/suite/parallel_query/t/parallel_insert_select_large_tables.test`
- `mysql-test/suite/parallel_query/t/parallel_replace_select.test`
- `mysql-test/suite/parallel_query/t/parallel_replace_select_behavior_changes.test`
- `mysql-test/suite/parallel_query/t/pq_abort.test`
- `mysql-test/suite/parallel_query/t/pq_agg_distinct.test`
- `mysql-test/suite/parallel_query/t/pq_aggr_no_record.test`
- `mysql-test/suite/parallel_query/t/pq_audit_log.test`
- `mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test`
- `mysql-test/suite/parallel_query/t/pq_autoinc.test`
- `mysql-test/suite/parallel_query/t/pq_blob.test`
- `mysql-test/suite/parallel_query/t/pq_bugfix.test`
- `mysql-test/suite/parallel_query/t/pq_cbo.test`
- `mysql-test/suite/parallel_query/t/pq_charset.test`
- `mysql-test/suite/parallel_query/t/pq_check_first_rewritten_tab.test`
- `mysql-test/suite/parallel_query/t/pq_clone_item.test`
- `mysql-test/suite/parallel_query/t/pq_coverage.test`
- `mysql-test/suite/parallel_query/t/pq_coverage_index.test`
- `mysql-test/suite/parallel_query/t/pq_demon.test`
- `mysql-test/suite/parallel_query/t/pq_depend_ref.test`
- `mysql-test/suite/parallel_query/t/pq_derived_view.test`
- `mysql-test/suite/parallel_query/t/pq_dev_bugs.test`
- `mysql-test/suite/parallel_query/t/pq_distinct.test`
- `mysql-test/suite/parallel_query/t/pq_divide_derived.test`
- `mysql-test/suite/parallel_query/t/pq_divide_derived_low_mmap.test`
- `mysql-test/suite/parallel_query/t/pq_divide_derived_no_mmap.test`
- `mysql-test/suite/parallel_query/t/pq_examined_rows.test`
- `mysql-test/suite/parallel_query/t/pq_explain.test`
- `mysql-test/suite/parallel_query/t/pq_explain_analyze.test`
- `mysql-test/suite/parallel_query/t/pq_explain_json.test`
- `mysql-test/suite/parallel_query/t/pq_explain_tree.test`
- `mysql-test/suite/parallel_query/t/pq_fallback.test`
- `mysql-test/suite/parallel_query/t/pq_flush.test`
- `mysql-test/suite/parallel_query/t/pq_found_rows.test`
- `mysql-test/suite/parallel_query/t/pq_fullscan.test`
- `mysql-test/suite/parallel_query/t/pq_group_by.test`
- `mysql-test/suite/parallel_query/t/pq_hash_join.test`
- `mysql-test/suite/parallel_query/t/pq_hash_join_error.test`
- `mysql-test/suite/parallel_query/t/pq_icp.test`
- `mysql-test/suite/parallel_query/t/pq_index_scan_desc.test`
- `mysql-test/suite/parallel_query/t/pq_innodb_intrinsic.test`
- `mysql-test/suite/parallel_query/t/pq_instant_add_column.test`
- `mysql-test/suite/parallel_query/t/pq_join_bka.test`
- `mysql-test/suite/parallel_query/t/pq_jt_ref.test`
- `mysql-test/suite/parallel_query/t/pq_kill.test`
- `mysql-test/suite/parallel_query/t/pq_kill_query.test`
- `mysql-test/suite/parallel_query/t/pq_leader_exception.test`
- `mysql-test/suite/parallel_query/t/pq_left_join_zero_rows.test`
- `mysql-test/suite/parallel_query/t/pq_limit_no_order_by.test`
- `mysql-test/suite/parallel_query/t/pq_master_disable.test`
- `mysql-test/suite/parallel_query/t/pq_mdl_lock.test`
- `mysql-test/suite/parallel_query/t/pq_memory_limit.test`
- `mysql-test/suite/parallel_query/t/pq_metadata.test`
- `mysql-test/suite/parallel_query/t/pq_mq_error.test`
- `mysql-test/suite/parallel_query/t/pq_multi_value.test`
- `mysql-test/suite/parallel_query/t/pq_not_equal.test`
- `mysql-test/suite/parallel_query/t/pq_not_support.test`
- `mysql-test/suite/parallel_query/t/pq_not_support_dstore.test`
- `mysql-test/suite/parallel_query/t/pq_opt_trace.test`
- `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`
- `mysql-test/suite/parallel_query/t/pq_optimizer_trace_bugfix.test`
- `mysql-test/suite/parallel_query/t/pq_order_by.test`
- `mysql-test/suite/parallel_query/t/pq_order_const.test`
- `mysql-test/suite/parallel_query/t/pq_partition.test`
- `mysql-test/suite/parallel_query/t/pq_prepare.test`
- `mysql-test/suite/parallel_query/t/pq_range_clust.test`
- `mysql-test/suite/parallel_query/t/pq_range_exception.test`
- `mysql-test/suite/parallel_query/t/pq_range_scan_reverse.test`
- `mysql-test/suite/parallel_query/t/pq_range_sec.test`
- `mysql-test/suite/parallel_query/t/pq_read_record_crash.test`
- `mysql-test/suite/parallel_query/t/pq_read_view.test`
- `mysql-test/suite/parallel_query/t/pq_readonly.test`
- `mysql-test/suite/parallel_query/t/pq_rec_visible.test`
- `mysql-test/suite/parallel_query/t/pq_record_buffer.test`
- `mysql-test/suite/parallel_query/t/pq_ref_build_range.test`
- `mysql-test/suite/parallel_query/t/pq_ref_reverse_scan.test`
- `mysql-test/suite/parallel_query/t/pq_refactor_clone.test`
- `mysql-test/suite/parallel_query/t/pq_replica_enable.test`
- `mysql-test/suite/parallel_query/t/pq_restart_after_select.test`
- `mysql-test/suite/parallel_query/t/pq_reverse_index_scan.test`
- `mysql-test/suite/parallel_query/t/pq_sec_index_min.test`
- `mysql-test/suite/parallel_query/t/pq_semijoin.test`
- `mysql-test/suite/parallel_query/t/pq_slow_log.test`
- `mysql-test/suite/parallel_query/t/pq_sp_trigger.test`
- `mysql-test/suite/parallel_query/t/pq_sql_cond.test`
- `mysql-test/suite/parallel_query/t/pq_subquery.test`
- `mysql-test/suite/parallel_query/t/pq_subquery_correlated.test`
- `mysql-test/suite/parallel_query/t/pq_support_features_switch.test`
- `mysql-test/suite/parallel_query/t/pq_sysdate_is_now.test`
- `mysql-test/suite/parallel_query/t/pq_tempory_table_release.test`
- `mysql-test/suite/parallel_query/t/pq_tmp_key.test`
- `mysql-test/suite/parallel_query/t/pq_union.test`
- `mysql-test/suite/parallel_query/t/pq_user_func.test`
- `mysql-test/suite/parallel_query/t/pq_variables.test`
- `mysql-test/suite/parallel_query/t/pq_worker_error.test`
- `mysql-test/suite/parallel_query/t/refactor_fix_fields.test`
- `mysql-test/suite/parallel_query/t/round_truncate_for_pq.test`

## 6. Workspace PQ 期望结果

- `mysql-test/suite/parallel_query/r/parallel_insert_select.result-pq`
- `mysql-test/suite/parallel_query/r/parallel_insert_select_behavior_changes.result-pq`
- `mysql-test/suite/parallel_query/r/parallel_insert_select_large_tables.result-pq`
- `mysql-test/suite/parallel_query/r/parallel_replace_select.result-pq`
- `mysql-test/suite/parallel_query/r/parallel_replace_select_behavior_changes.result-pq`
- `mysql-test/suite/parallel_query/r/pq_abort.result-pq`
- `mysql-test/suite/parallel_query/r/pq_agg_distinct.result-pq`
- `mysql-test/suite/parallel_query/r/pq_aggr_no_record.result-pq`
- `mysql-test/suite/parallel_query/r/pq_audit_log.result-pq`
- `mysql-test/suite/parallel_query/r/pq_auto_retry_failed_parallel_query.result-pq`
- `mysql-test/suite/parallel_query/r/pq_autoinc.result-pq`
- `mysql-test/suite/parallel_query/r/pq_blob.result-pq`
- `mysql-test/suite/parallel_query/r/pq_bugfix.result-pq`
- `mysql-test/suite/parallel_query/r/pq_cbo.result-pq`
- `mysql-test/suite/parallel_query/r/pq_charset.result-pq`
- `mysql-test/suite/parallel_query/r/pq_check_first_rewritten_tab.result-pq`
- `mysql-test/suite/parallel_query/r/pq_clone_item.result-pq`
- `mysql-test/suite/parallel_query/r/pq_coverage.result-pq`
- `mysql-test/suite/parallel_query/r/pq_coverage_index.result-pq`
- `mysql-test/suite/parallel_query/r/pq_demon.result-pq`
- `mysql-test/suite/parallel_query/r/pq_depend_ref.result-pq`
- `mysql-test/suite/parallel_query/r/pq_derived_view.result-pq`
- `mysql-test/suite/parallel_query/r/pq_dev_bugs.result-pq`
- `mysql-test/suite/parallel_query/r/pq_distinct.result-pq`
- `mysql-test/suite/parallel_query/r/pq_divide_derived.result-pq`
- `mysql-test/suite/parallel_query/r/pq_divide_derived_low_mmap.result-pq`
- `mysql-test/suite/parallel_query/r/pq_divide_derived_no_mmap.result-pq`
- `mysql-test/suite/parallel_query/r/pq_examined_rows.result-pq`
- `mysql-test/suite/parallel_query/r/pq_explain.result-pq`
- `mysql-test/suite/parallel_query/r/pq_explain_analyze.result-pq`
- `mysql-test/suite/parallel_query/r/pq_explain_json.result-pq`
- `mysql-test/suite/parallel_query/r/pq_explain_tree.result-pq`
- `mysql-test/suite/parallel_query/r/pq_fallback.result-pq`
- `mysql-test/suite/parallel_query/r/pq_flush.result-pq`
- `mysql-test/suite/parallel_query/r/pq_found_rows.result-pq`
- `mysql-test/suite/parallel_query/r/pq_fullscan.result-pq`
- `mysql-test/suite/parallel_query/r/pq_group_by.result-pq`
- `mysql-test/suite/parallel_query/r/pq_hash_join.result-pq`
- `mysql-test/suite/parallel_query/r/pq_hash_join_error.result-pq`
- `mysql-test/suite/parallel_query/r/pq_icp.result-pq`
- `mysql-test/suite/parallel_query/r/pq_index_scan_desc.result-pq`
- `mysql-test/suite/parallel_query/r/pq_innodb_intrinsic.result-pq`
- `mysql-test/suite/parallel_query/r/pq_instant_add_column.result-pq`
- `mysql-test/suite/parallel_query/r/pq_join_bka.result-pq`
- `mysql-test/suite/parallel_query/r/pq_jt_ref.result-pq`
- `mysql-test/suite/parallel_query/r/pq_kill.result-pq`
- `mysql-test/suite/parallel_query/r/pq_kill_query.result-pq`
- `mysql-test/suite/parallel_query/r/pq_leader_exception.result-pq`
- `mysql-test/suite/parallel_query/r/pq_left_join_zero_rows.result-pq`
- `mysql-test/suite/parallel_query/r/pq_limit_no_order_by.result-pq`
- `mysql-test/suite/parallel_query/r/pq_master_disable.result-pq`
- `mysql-test/suite/parallel_query/r/pq_mdl_lock.result-pq`
- `mysql-test/suite/parallel_query/r/pq_memory_limit.result-pq`
- `mysql-test/suite/parallel_query/r/pq_metadata.result-pq`
- `mysql-test/suite/parallel_query/r/pq_mq_error.result-pq`
- `mysql-test/suite/parallel_query/r/pq_multi_value.result-pq`
- `mysql-test/suite/parallel_query/r/pq_not_equal.result-pq`
- `mysql-test/suite/parallel_query/r/pq_not_support.result-pq`
- `mysql-test/suite/parallel_query/r/pq_not_support_dstore.result-pq`
- `mysql-test/suite/parallel_query/r/pq_opt_trace.result-pq`
- `mysql-test/suite/parallel_query/r/pq_optimizer_trace.result-pq`
- `mysql-test/suite/parallel_query/r/pq_optimizer_trace_bugfix.result-pq`
- `mysql-test/suite/parallel_query/r/pq_order_by.result-pq`
- `mysql-test/suite/parallel_query/r/pq_order_const.result-pq`
- `mysql-test/suite/parallel_query/r/pq_partition.result-pq`
- `mysql-test/suite/parallel_query/r/pq_prepare.result-pq`
- `mysql-test/suite/parallel_query/r/pq_range_clust.result-pq`
- `mysql-test/suite/parallel_query/r/pq_range_exception.result-pq`
- `mysql-test/suite/parallel_query/r/pq_range_scan_reverse.result-pq`
- `mysql-test/suite/parallel_query/r/pq_range_sec.result-pq`
- `mysql-test/suite/parallel_query/r/pq_read_record_crash.result-pq`
- `mysql-test/suite/parallel_query/r/pq_read_view.result-pq`
- `mysql-test/suite/parallel_query/r/pq_readonly.result-pq`
- `mysql-test/suite/parallel_query/r/pq_rec_visible.result-pq`
- `mysql-test/suite/parallel_query/r/pq_record_buffer.result-pq`
- `mysql-test/suite/parallel_query/r/pq_ref_build_range.result-pq`
- `mysql-test/suite/parallel_query/r/pq_ref_reverse_scan.result-pq`
- `mysql-test/suite/parallel_query/r/pq_refactor_clone.result-pq`
- `mysql-test/suite/parallel_query/r/pq_replica_enable.result-pq`
- `mysql-test/suite/parallel_query/r/pq_restart_after_select.result-pq`
- `mysql-test/suite/parallel_query/r/pq_reverse_index_scan.result-pq`
- `mysql-test/suite/parallel_query/r/pq_sec_index_min.result-pq`
- `mysql-test/suite/parallel_query/r/pq_semijoin.result-pq`
- `mysql-test/suite/parallel_query/r/pq_slow_log.result-pq`
- `mysql-test/suite/parallel_query/r/pq_sp_trigger.result-pq`
- `mysql-test/suite/parallel_query/r/pq_sql_cond.result-pq`
- `mysql-test/suite/parallel_query/r/pq_subquery.result-pq`
- `mysql-test/suite/parallel_query/r/pq_subquery_correlated.result-pq`
- `mysql-test/suite/parallel_query/r/pq_support_features_switch.result-pq`
- `mysql-test/suite/parallel_query/r/pq_sysdate_is_now.result-pq`
- `mysql-test/suite/parallel_query/r/pq_tempory_table_release.result-pq`
- `mysql-test/suite/parallel_query/r/pq_tmp_key.result-pq`
- `mysql-test/suite/parallel_query/r/pq_union.result-pq`
- `mysql-test/suite/parallel_query/r/pq_user_func.result-pq`
- `mysql-test/suite/parallel_query/r/pq_variables.result-pq`
- `mysql-test/suite/parallel_query/r/pq_worker_error.result-pq`
- `mysql-test/suite/parallel_query/r/refactor_fix_fields.result-pq`
- `mysql-test/suite/parallel_query/r/round_truncate_for_pq.result-pq`

## 7. 未跟踪 PQ 测试 overlay

### `.test`

- none

### `.result-pq`

- none
