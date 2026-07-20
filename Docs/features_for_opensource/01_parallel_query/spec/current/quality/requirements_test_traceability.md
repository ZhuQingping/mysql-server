# Parallel Query Requirement—Test Traceability

> 文档 ID：`PQ-QUALITY-TRACE-001`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 运行验证：见 `current/quality/verification_evidence.md`

> Snapshot authority：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`。该 manifest 绑定
> implementation commit `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9`、`stable_branch` 已提交基线、
> `source_tree_sha256=a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> 和 `test_tree_sha256=066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`。
> 两个 hash 只覆盖 manifest 定义的 PQ source/test file set，不覆盖或复现整个 bound PQ source/test tree。

## 1. 使用规则

本表将 Task 4 的 Requirement 映射到规范性不变量、当前源码、MTR 静态资产、运行时观测
和已知缺口。它回答“应该证明什么、现有资产在哪里、还缺什么”，不回答“目标 build 是否
已经通过”。

证据状态：

| 状态 | 含义 |
|---|---|
| `static-source` | 当前工作树存在明确实现或拒绝 gate |
| `test-present` | 命名 MTR/结果文件存在；是否被某次运行覆盖以 verification evidence 为准 |
| `static-partial` | 源码与测试资产覆盖部分维度，不能形成发布证明 |
| `static-gap` | 缺少直接测试、稳定观测或契约闭环 |
| `runtime-verified` | 绑定 commit/build 的命令输出证明该 Requirement 的定向 oracle 通过 |

任何 Requirement 只有同时具备：

```text
current contract
  + stable source path/symbol
  + positive and negative/fault test
  + path observability
  + commit/build-bound run report
```

才能升级为 release evidence。

## 2. 高级 SQL Traceability

| Requirement | Invariant | Source | Test（静态映射；suite 运行范围见 verification evidence） | Observability | Gap | 状态 |
|---|---|---|---|---|---|---|
| `PQ-HSQL-REQ-001` | `PQ-HSQL-INV-001` 分类唯一决定执行位置 | `sql/sql_union.cc::Query_expression::subquery_suite_for_parallel_query`；`sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | `mysql-test/suite/parallel_query/t/pq_subquery.test`；`mysql-test/suite/parallel_query/t/pq_subquery_correlated.test`；`mysql-test/suite/parallel_query/t/pq_optimizer_trace.test` | per-query-block EXPLAIN + `not_apply_pq_plan` | 缺分类表的机器检查和执行次数观测 | `static-partial` |
| `PQ-HSQL-REQ-002` | `PQ-HSQL-INV-002` leader-once 先于 worker | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::exec_scalar_uncorrelated_subquery`；`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::Init` | `mysql-test/suite/parallel_query/t/pq_subquery.test`；`mysql-test/suite/parallel_query/t/pq_explain_analyze.test`；`mysql-test/suite/parallel_query/t/pq_fallback.test` | Gather plan + worker launch sync + result/error | 缺“恰好一次”计数和逐阶段 init fault | `static-partial` |
| `PQ-HSQL-REQ-003` | `PQ-HSQL-INV-003` correlated outer reference worker-local | `sql/parallel_query/pq_clone_item.cc::Item_field::pq_clone`；`sql/parallel_query/pq_refix_fields_item.cc::Item_subselect::refix_fields`；`sql/item_subselect.cc::Item_subselect::pq_clone_common` | `mysql-test/suite/parallel_query/t/pq_subquery_correlated.test` | worker plan/tree + PQ OFF/ON result | 缺随机差分、pointer-owner assertion 和多 DOP 组合 | `static-partial` |
| `PQ-HSQL-REQ-004` | `PQ-HSQL-INV-004` shared materialization 生命周期覆盖消费者 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::para_exec_init`；`sql/parallel_query/pq_clone.cc::Semijoin_mat_exec::pq_clone` | `mysql-test/suite/parallel_query/t/pq_divide_derived.test`；`mysql-test/suite/parallel_query/t/pq_derived_view.test`；`mysql-test/suite/parallel_query/t/pq_semijoin.test` | temp-table status + workers/resource baseline | partial launch/KILL/error/cleanup re-entry 不完整 | `static-partial` |
| `PQ-HSQL-REQ-005` | `PQ-HSQL-INV-005` UNION 重复/去重语义等价 | `sql/sql_union.cc::Query_expression::optimize`；`sql/parallel_query/sql_parallel.cc::make_pq_unit_plan` | `mysql-test/suite/parallel_query/t/pq_union.test`；`mysql-test/suite/parallel_query/t/pq_explain_tree.test`；`mysql-test/suite/parallel_query/t/pq_explain_json.test` | member Gather + ordered/multiset oracle | 缺 UNION member fault、warning、materialization cleanup 矩阵 | `static-partial` |
| `PQ-HSQL-REQ-006` | `PQ-HSQL-INV-006` PS/SP/FUNC_SP/trigger 不可绕过 | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query`；`sql/parallel_query/pq_optimizer.cc::NO_PQ_SUPPORTED_FUNC_TYPES` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`；`mysql-test/suite/parallel_query/t/pq_sp_trigger.test` | 无 Gather + trace reason + serial result | binary PS/reprepare、trigger restore fault、reason 细分缺失 | `static-partial` |
| `PQ-HSQL-REQ-007` | `PQ-HSQL-INV-007` 高级 SQL 拒绝无副作用 | `sql/parallel_query/sql_parallel.cc::restore_leader_plan`；`sql/parallel_query/sql_parallel.cc::reset_derived_materialize_query_blocks` | `mysql-test/suite/parallel_query/t/pq_fallback.test`；`mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test` | final serial plan + resource/status baseline | 缺所有 rewrite point 和 materialization state digest | `static-partial` |

## 3. 事务、DML 与 Binlog Traceability

| Requirement | Invariant | Source | Test（静态映射；suite 运行范围见 verification evidence） | Observability | Gap | 状态 |
|---|---|---|---|---|---|---|
| `PQ-TRX-REQ-001` | `PQ-TRX-INV-001` leader/worker/scan context 快照相容 | `storage/innobase/handler/ha_innodb_pq.cc::pq_create_innodb_snapshot`；`storage/innobase/handler/ha_innodb_pq.cc::pq_clone_innodb_snapshot`；`storage/innobase/row/row0pread_pq.cc::PQ_Scan_ctx::find_visible_record` | `mysql-test/suite/parallel_query/t/pq_read_view.test`；`mysql-test/suite/parallel_query/t/pq_rec_visible.test` | sync point + PQ OFF/ON checksum + transaction/read-view evidence | 缺 purge/restore 专项、完整 owner 证明和 isolation×access×concurrency 矩阵 | `static-partial` |
| `PQ-TRX-REQ-002` | `PQ-TRX-INV-002` locking read 必须拒绝 | `sql/parallel_query/pq_resolver.cc::Query_block::pq_check_table_list` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`；`mysql-test/suite/parallel_query/t/parallel_insert_select.test`；`mysql-test/suite/parallel_query/t/parallel_replace_select.test` | trace reason + 无 Gather | 缺 FOR SHARE/LOCK TABLES 的稳定 reason 全覆盖 | `static-partial` |
| `PQ-TRX-REQ-003` | `PQ-TRX-INV-003` target DML 副作用只归 leader | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::Read`；`sql/sql_insert.cc::Query_result_insert::send_data` | `mysql-test/suite/parallel_query/t/parallel_insert_select.test`；`mysql-test/suite/parallel_query/t/parallel_insert_select_behavior_changes.test`；`mysql-test/suite/parallel_query/t/parallel_replace_select.test`；`mysql-test/suite/parallel_query/t/parallel_replace_select_behavior_changes.test` | Gather select side + affected rows/target checksum/trigger state | 缺 owner assertion、auto-inc/unique/trigger 完整组合 | `static-partial` |
| `PQ-TRX-REQ-004` | `PQ-TRX-INV-004` parallel DML 仅 ROW binlog | `sql/parallel_query/pq_optimizer.cc::check_pq_suite_for_insert_select` | `mysql-test/suite/parallel_query/t/parallel_insert_select.test`；`mysql-test/suite/parallel_query/t/parallel_replace_select.test`；`mysql-test/suite/parallel_query/t/pq_replica_enable.test` | trace/EXPLAIN + current binlog format + binlog event | ROW/MIXED/STATEMENT、`sql_log_bin`、GTID 矩阵不足 | `static-partial` |
| `PQ-TRX-REQ-005` | `PQ-TRX-INV-005` retry 前无任何外部副作用 | `sql/sql_parse.cc::retry_without_parallel_query`；`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | `mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test`；`mysql-test/suite/parallel_query/t/pq_fallback.test` | warning/error + single target/log/binlog effect + no PQ second attempt | 缺所有 error raise site 的 side-effect proof | `static-gap` |
| `PQ-TRX-REQ-006` | `PQ-TRX-INV-006` error/warning/IGNORE/affected rows 等价 | `sql/parallel_query/query_result_mq.cc::Query_result_mq::send_data`；`sql/sql_insert.cc::Query_result_insert::send_data` | `mysql-test/suite/parallel_query/t/parallel_insert_select_behavior_changes.test`；`mysql-test/suite/parallel_query/t/parallel_replace_select_behavior_changes.test` | error code/SQLSTATE/warnings/affected rows/checksum | 缺系统边界数据与 multi-DOP oracle | `static-partial` |
| `PQ-TRX-REQ-007` | `PQ-TRX-INV-007` 日志/instrumentation 表达一次逻辑语句 | `sql/sql_parse.cc::retry_without_parallel_query`；`sql/log.cc::File_query_log::write_slow` | `mysql-test/suite/parallel_query/t/pq_slow_log.test`；`mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test`；`mysql-test/suite/parallel_query/t/pq_audit_log.test` | general/slow/audit/PFS/digest/binlog count | audit 内容、PFS、binlog exact-once 缺专项断言 | `static-gap` |
| `PQ-TRX-REQ-008` | `PQ-TRX-INV-008` SERIALIZABLE/attachable/PS/SP gate 不可绕过 | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`；`mysql-test/suite/parallel_query/t/pq_sp_trigger.test` | no Gather + rejection trace | RU/XA/attachable 的定向覆盖不足 | `static-partial` |

## 4. 资源 Traceability

| Requirement | Invariant | Source | Test（静态映射；suite 运行范围见 verification evidence） | Observability | Gap | 状态 |
|---|---|---|---|---|---|---|
| `PQ-RES-REQ-001` | `PQ-RES-INV-001` thread reservation 成对 | `sql/parallel_query/sql_parallel.cc::check_pq_running_threads`；`sql/parallel_query/pq_resource_stat.cc::release_pq_running_threads`；`sql/sql_class.cc::THD::cleanup_after_query` | `mysql-test/suite/parallel_query/t/pq_variables.test`；`mysql-test/suite/parallel_query/t/pq_worker_error.test` | before/peak/after `PQ_threads_running` | 缺所有 fallback/error 路径精确 delta 和 underflow assertion | `static-partial` |
| `PQ-RES-REQ-002` | `PQ-RES-INV-002` reserved/created/effective DOP 分离 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker`；`sql/parallel_query/sql_parallel.cc::PQ_worker_manager::signal_status` | `mysql-test/suite/parallel_query/t/pq_variables.test`；`mysql-test/suite/parallel_query/t/pq_worker_error.test` | requested/created/context-worker counts | 当前产品指标无法区分三者 | `static-gap` |
| `PQ-RES-REQ-003` | `PQ-RES-INV-003` memory accounting 不回绕 | `sql/parallel_query/pq_resource_stat.cc::add_pq_memory`；`sql/parallel_query/pq_resource_stat.cc::sub_pq_memory`；`sql/parallel_query/pq_resource_stat.cc::get_pq_memory_total` | `mysql-test/suite/parallel_query/t/pq_memory_limit.test` | before/peak/after `PQ_memory_used/refused` | `uint`×`size_t`、>4GiB、concurrent growth 未覆盖 | `static-gap` |
| `PQ-RES-REQ-004` | `PQ-RES-INV-004` MQ per-worker 资源完整清理 | `sql/parallel_query/exchange.cc::Exchange::init`；`sql/parallel_query/exchange.cc::Exchange::cleanup`；`sql/parallel_query/msg_queue.cc::MQueue_handle::receive` | `mysql-test/suite/parallel_query/t/pq_mq_error.test`；`mysql-test/suite/parallel_query/t/pq_kill.test` | queue state + worker join + memory baseline | ring wrap/partial message/detach race/cleanup re-entry 不完整 | `static-partial` |
| `PQ-RES-REQ-005` | `PQ-RES-INV-005` temp/spill/VFD 无泄漏 | `sql/parallel_query/msg_queue.h::Batch_buffer::~Batch_buffer`；`sql/parallel_query/chunk_files_wrapper.h::ChunkFilesWrapper`；`sql/vfd/vfd_manager.cc::VfdManager::Close` | `mysql-test/suite/parallel_query/t/pq_tempory_table_release.test`；`mysql-test/suite/parallel_query/t/pq_hash_join.test`；`mysql-test/suite/parallel_query/t/pq_agg_distinct.test` | temp/file/VFD count + disk/memory baseline | 缺 file I/O fault 和 exact resource inventory | `static-partial` |
| `PQ-RES-REQ-006` | `PQ-RES-INV-006` 拒绝不长期占 quota | `sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query`；`sql/sql_class.cc::THD::cleanup_after_query` | `mysql-test/suite/parallel_query/t/pq_support_features_switch.test`；`mysql-test/suite/parallel_query/t/pq_cbo.test` | live reservation duration + serial statement duration | 无定向 MTR；当前直到 statement cleanup 归还 | `static-gap` |
| `PQ-RES-REQ-007` | `PQ-RES-INV-007` wait 有界且单位明确 | `sql/parallel_query/sql_parallel.cc::check_pq_running_threads`；`sql/sys_vars.cc::Sys_parallel_queue_timeout` | `mysql-test/suite/parallel_query/t/pq_variables.test` | measured wait + reason + KILL responsiveness | help microseconds/实现 milliseconds；公平性/KILL wait 缺失 | `static-gap` |
| `PQ-RES-REQ-008` | `PQ-RES-INV-008` thread-pool 声明符合实现 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker` | `none` | PFS thread + source path | 无 thread-pool path/MTR；历史 WL 只能 design-only | `static-source` |

## 5. 可观测性与集成 Traceability

| Requirement | Invariant | Source | Test（静态映射；suite 运行范围见 verification evidence） | Observability | Gap | 状态 |
|---|---|---|---|---|---|---|
| `PQ-OBS-REQ-001` | `PQ-OBS-INV-001` 采用路径区分 leader-plan 与 worker-run 证据 | `sql/join_optimizer/explain_access_path.cc::PrintQueryPlan`；`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan`；`sql/log.cc::File_query_log::write_slow` | `mysql-test/suite/parallel_query/t/pq_explain.test`；`mysql-test/suite/parallel_query/t/pq_explain_tree.test`；`mysql-test/suite/parallel_query/t/pq_explain_json.test`；`mysql-test/suite/parallel_query/t/pq_explain_analyze.test` | Gather + leader-plan marker + worker launch/READY/row-consumption + result | `PQ_executed`/`PQ_stmt_executed` 只证明预启动 leader plan；缺稳定 worker-run 指标 | `static-gap` |
| `PQ-OBS-REQ-002` | `PQ-OBS-INV-002` 拒绝原因可归因 | `sql/parallel_query/pq_optimizer.cc::add_reason_to_trace`；`sql/parallel_query/pq_optimizer.cc::PQ_UNSUITABLE_INFO` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`；`mysql-test/suite/parallel_query/t/pq_opt_trace.test` | `not_apply_pq_plan` + select number + final serial plan | reason 是字符串且多场景合并 | `static-partial` |
| `PQ-OBS-REQ-003` | `PQ-OBS-INV-003` 指标并发准确且语义明确 | `sql/parallel_query/sql_parallel.cc::make_pq_leader_plan`；`sql/mysqld.cc::status_vars` | `mysql-test/suite/parallel_query/t/pq_variables.test`；`mysql-test/suite/parallel_query/t/pq_examined_rows.test` | controlled delta + query-block/UNION correlation | 预启动、全局粒度、UNION 可多增、并发 `++` 精度和整数宽度未证明 | `static-gap` |
| `PQ-OBS-REQ-004` | `PQ-OBS-INV-004` PQ 不复用 incompatible cached plan | `sql/sql_plan_cache.cc::plan_cache::invalidate_cached_plan`；`sql/sql_plan_cache.cc::plan_cache::set_uncacheable` | `mysql-test/suite/parallel_query/t/pq_prepare.test`；`mysql-test/suite/parallel_query/t/pq_optimizer_trace.test` | cache hit/miss/invalidation + plan shape | 无 PQ plan cache；缺 reprepare/fallback 矩阵 | `static-partial` |
| `PQ-OBS-REQ-005` | `PQ-OBS-INV-005` PTRC clone 与 cleanup 正确 | `sql/item_subselect.cc::Item_subselect::pq_clone_common`；`sql/parallel_query/sql_parallel.cc::pq_free_thd` | `mysql-test/suite/parallel_query/t/pq_bugfix.test` | PTRC plan/hit state + result + memory cleanup | 无专用四象限、KILL/error/fallback 测试 | `static-gap` |
| `PQ-OBS-REQ-006` | `PQ-OBS-INV-006` 日志表达一次逻辑 statement | `sql/sql_parse.cc::retry_without_parallel_query`；`sql/log.cc::File_query_log::write_slow` | `mysql-test/suite/parallel_query/t/pq_slow_log.test`；`mysql-test/suite/parallel_query/t/pq_audit_log.test`；`mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test` | general/slow/audit/PFS/digest counts | audit test 未断言内容；retry exact-once 未闭环 | `static-gap` |
| `PQ-OBS-REQ-007` | `PQ-OBS-INV-007` PS/SP/FUNC_SP/trigger 观测符合 serial-fallback | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query`；`sql/parallel_query/pq_optimizer.cc::NO_PQ_SUPPORTED_FUNC_TYPES` | `mysql-test/suite/parallel_query/t/pq_sp_trigger.test`；`mysql-test/suite/parallel_query/t/pq_optimizer_trace.test` | no Gather + reason + serial result | binary PS/reprepare、reason 细分、trigger 组合不足 | `static-partial` |
| `PQ-OBS-REQ-008` | `PQ-OBS-INV-008` binlog/replica 语义等价 | `sql/parallel_query/pq_optimizer.cc::check_pq_suite_for_insert_select`；`sql/sql_insert.cc::Query_result_insert::send_eof` | `mysql-test/suite/parallel_query/t/pq_replica_enable.test`；`mysql-test/suite/parallel_query/t/parallel_insert_select.test`；`mysql-test/suite/parallel_query/t/parallel_replace_select.test` | source plan + binlog/GTID + replica checksum | test 主要覆盖 replica SELECT；apply/restart/event 矩阵缺失 | `static-gap` |

## 6. 必需的交叉测试维度

任何扩大 support status 的变更至少覆盖：

| 维度 | 必选值 |
|---|---|
| PQ path | OFF、accepted、eligibility reject、CBO/resource reject、graceful fallback、full retry、runtime error |
| DOP | 1、2、4、8、DOP 大于 scan contexts |
| SQL context | plain SELECT、subquery、derived/CTE、UNION ALL/DISTINCT、INSERT/REPLACE SELECT |
| Execution context | text protocol、SQL PREPARE、binary PS、SP、FUNC_SP、trigger |
| Transaction | autocommit、显式 transaction、RU/RC/RR、SERIALIZABLE refusal、XA audit |
| Concurrency | none、INSERT、UPDATE、DELETE、purge、DDL、KILL、disconnect |
| Resource | no pressure、thread wait/reject、memory threshold、MQ backpressure、temp/batch/hash spill、VFD pressure |
| Integration | plan cache、PTRC、slow/general/audit/PFS、ROW binlog、replica |
| Oracle | rows/order/multiset、error/SQLSTATE、warning、affected rows、external side effects、resource baseline |

## 7. 当前 Release 解释

- 已完成的 suite 运行见 `verification_evidence.md`；它不自动把所有 Requirement 升级为 `runtime-verified`。
- `pq_prepare` 主要覆盖 prepare/resolve/clone regression，不是 Prepared Statement execute
  支持证明。
- `pq_sp_trigger` 包含大量通用 stored-program regression，不等于 PS/SP/FUNC_SP/trigger
  四类拒绝矩阵已经完成。
- `pq_audit_log` 执行多连接 SQL，但没有静态可见的 audit event 内容和次数断言。
- `pq_replica_enable` 主要覆盖 read-only replica 上 SELECT/PQ 开关，不等于 parallel DML
  binlog/replica apply 已验证。
- `--pq` MTR 模式会强制候选并调整成本参数；默认配置和 CBO 选择必须另有 release tier。
- `mysql-test/collections/disabled-pq.def` 中的跨套件禁用项必须逐项有 owner、原因、替代覆盖
  和退出条件，不能从 PQ suite 单独通过推断跨套件无回归。

## 8. 维护规则

1. Requirement 状态变化必须同步对应模块、`01_current_support_matrix.md`、本表和
   `conformance_gaps.md`。
2. 新测试必须同时声明 path proof、oracle、DOP、cleanup assertion 和执行 tier。
3. 删除或改名 source symbol/MTR 时必须在同一变更更新本表。
4. 测试结果必须绑定完整 commit SHA、build 类型、MTR 命令和结果 artifact。
5. 未运行的测试始终标记为静态资产；不得写成“通过”或“已验证”。
