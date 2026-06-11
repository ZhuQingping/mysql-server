# PQ V1/V2 测试矩阵

## Goal

为 V1 风险收敛和 V2 真实执行路径提供阶段化 MTR 测试矩阵。原则：

- V1 先补语义和负例测试，避免 scaffold 被误解为真实并行执行；
- V2 前半段先用 DOP=1 验证真实执行管线；
- 只有 V2-4 InnoDB range partition 通过后，才允许 DOP>1 正确性测试作为验收项。

## V1 Immediate Tests

| Priority | Test | Coverage |
|---|---|---|
| P0 | `pq_locking_read_fallback` | `FOR UPDATE`、`LOCK IN SHARE MODE`、`FOR SHARE` 必须显示 `Not parallel LOCKING_READ`，不能显示 `Parallel query dop=N` |
| P0 | `pq_explain_state_contract` | EXPLAIN 文案区分 candidate/eligible 与 actual executed；V1 不暗示真实并行已发生 |
| P0 | `pq_explain_counter_stable` | `EXPLAIN`、`EXPLAIN FORMAT=TREE/JSON` 前后 `Parallel_queries_fallback` 不变 |
| P0 | `pq_access_path_negative` | `FORCE INDEX`、PK lookup、indexed predicate、covering index scan、range/ref access 都应 `Not parallel NON_FULL_TABLE_SCAN` |
| P1 | `pq_prepare_state_reset` | `PREPARE/EXECUTE` 多次执行后 `pq_eligible`、fallback reason、counter 不串状态 |
| P1 | `pq_parallel_off_stats_noop` | `parallel_query=OFF` 时执行和 EXPLAIN 都不产生 PQ annotation，也不改 PQ counters |
| P1 | `pq_const_zero_table_negative` | `SELECT 1`、const table、empty table 边界不越界 |
| P1 | `pq_unsupported_agg_execution_counter` | `GROUP_CONCAT/STD/VARIANCE/COUNT(DISTINCT)` 执行不应增加 eligible fallback counter |

## V2 Phase Tests

| Phase | Minimal MTR |
|---|---|
| V2-0 Execution State Contract | `pq_explain_state_contract`: EXPLAIN 显示 candidate/actual 区分；`parallel_query=OFF` 无注解；EXPLAIN 不污染 fallback counter |
| V2-1 Iterator Ownership + Safe Fallback | `pq_iterator_safe_fallback`: `TryCreatePQTableScanIterator()` 返回非空后，Init 安全窗口失败能串行 fallback，结果一致，counter 正确 |
| V2-2 Handler/InnoDB Context Bridge | `pq_handler_bridge_dop1`: DOP=1 leader init/end 成功；unsupported engine/path fallback；context 释放可重复执行 |
| V2-3 Read View / Trx Contract | `pq_read_view_dop1`: RR/RC 下并发 insert/update/delete 与串行一致；early abort/KILL 后再次查询正常 |
| V2-4 Parallel_reader Adapter + Range Partition | `pq_range_partition_dop`: DOP=1/2/4 验证空表、小表、多页表 `COUNT(*)` / `SUM(pk)` 无重复无漏；这是 DOP>1 gate |
| V2-5 Worker THD + Minimal Worker Scan | `pq_worker_dop1`: 真实 worker 启动、等待、清理；空表/小表可扫；nested PQ fallback 或拒绝 |
| V2-6 Exchange/Gather Row Stream | `pq_exchange_rows_dop1`: 单 worker row stream，`SELECT *`、EOF、error token 不死等；leader 填 `table->record[0]` 正确 |
| V2-7 Predicate/Projection Boundary | `pq_projection_where_dop1`: worker 只产 base row，leader 执行 WHERE/projection；覆盖 `SELECT cols`、简单 AND/OR、空结果 |
| V2-8 Single Table Full Scan Closure | `pq_fullscan_real_dop1` 先跑通真实闭环；V2-4 后增加 `pq_fullscan_real_dop2_4`，验证 status `executed/workers/rows` |
| V2-9 Basic Aggregation | `pq_agg_real_dop1`: `COUNT/SUM/AVG/MIN/MAX` 无 GROUP BY；V2-4 后加 DOP=2/4；explicit GROUP BY 继续 fallback |

## DOP Policy

### DOP=1 First

先用 DOP=1 的测试：

- V2-1 到 V2-3：iterator ownership、safe fallback、handler bridge、read view/trx 生命周期；
- V2-5 到 V2-7：worker THD、Exchange/Gather、row image、leader-side WHERE/projection；
- V2-8 / V2-9 第一版：真实 full scan 和基础聚合闭环。

DOP=1 的目的不是证明性能，而是验证真实执行管线、资源释放、error/kill/fallback 语义。

### DOP>1 Gate

必须等 V2-4 range partition 后才能 DOP>1：

- 任何会读取 InnoDB base rows 的真实执行测试；
- `SELECT *`、`COUNT(*)`、`SUM(pk)`、WHERE full scan、基础聚合；
- 空表、小表、单页、多页、多层 B+tree；
- read view 并发测试；
- worker kill/error 测试。

原因：当前 `row0pread_pq.cc` 仍是 whole-table single range scaffold，DOP>1 会重复读，不具备正确性基础。

## TaurusDB Reference Suite Migration Priority

| Priority | Reference Tests | Strategy |
|---|---|---|
| P0 | `pq_fullscan`, `pq_fallback`, `pq_explain`, `pq_explain_tree`, `pq_explain_json`, `pq_not_support`, `pq_variables`, `pq_master_disable` | 继续重写成小型 MySQL 8.0.46 用例；不要照搬 Taurus 变量和 hint |
| P0 | `pq_range_clust` | V2-4 后优先迁移精简版，用于 DOP>1 分片正确性 |
| P0 | `pq_read_view`, `pq_kill_query`, `pq_worker_error`, `pq_mq_error` | V2 worker/Exchange 接通后迁移精简版；当前 V1 只保留风险项，不迁移 debug injection 依赖 |
| P1 | `pq_group_by` 的 implicit aggregate 部分、`pq_aggr_no_record`, `pq_blob`, `pq_not_equal`, `pq_found_rows`, `pq_examined_rows` | 按 V2-8 / V2-9 分批重写，主要验证结果一致性和 status |
| P2 | `pq_prepare`, `pq_sp_trigger`, `pq_rec_visible`, `pq_flush`, `pq_mdl_lock` | V2 执行路径稳定后迁移，防 statement 生命周期和并发资源问题 |
| Later | `pq_order_by`, `pq_union`, `pq_subquery*`, `pq_derived_view`, `pq_hash_join`, `pq_semijoin`, `pq_icp`, `pq_range_sec`, `pq_partition`, `pq_agg_distinct`, `pq_record_buffer` | 对应 ORDER BY、子查询、join、二级索引/ICP、分区表、distinct aggregate、record buffer 能力完成后再迁移 |

## Current Status

测试矩阵已由 Test Explorer 只读调研完成并整理。后续每个 V2 phase 创建任务书时，必须引用本矩阵选择最小验收测试。
