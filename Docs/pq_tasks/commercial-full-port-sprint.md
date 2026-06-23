# Parallel Query 商用全量迁移冲刺

Last synced: 2026-06-23

## 目标

将 `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 中已经商用的
Parallel Query 实现尽量全量平移到当前 `support_parallel_query_8.0`
分支，使当前仓库支持场景与参考仓库保持一致。

当前策略不再按保守 V1/V2/M11 小步扩展推进，而是以商用实现为准做全量迁移。
只有出现明确的大风险时才允许临时跳过，并必须记录原因、影响范围和后续修复路径。

## 非目标

- 不再默认保留当前 debug-only / smoke-only / fail-closed scaffold 作为主路径；
  若与商用真实路径冲突，优先采用商用真实路径。
- 不做架构重写或额外优化。
- 不因为测试较多而主动缩小商用场景范围。

## 可跳过门槛

只有以下情况允许临时跳过：

- 依赖 taurusdbondstore 私有模块，当前 8.0.46 仓库没有等价依赖；
- MySQL 8.0.41 到 8.0.46 API 差异导致短期无法安全适配；
- 会破坏非 PQ 基础行为或导致 mysqld 启动/普通查询大面积失败；
- 测试依赖特定部署环境、dstore 能力、审计/复制环境，当前本地无法构造。

普通编译错误、result 差异、接口命名差异、guard 冲突不属于跳过理由，必须优先修复。

## 并行 Agent 分工

已启动只读差异审计 Agent：

- SQL/PQ 模块 Explorer：`sql/parallel_query/*` 与商用实现差异；
- SQL 主干 hook Explorer：optimizer/resolver/iterator/explain/sysvar/status 等主干改动；
- InnoDB/handler Explorer：`ha_innodb_pq.cc`、`row0pread_pq.*`、handler API；
- MTR Explorer：商用 `parallel_query` 测试套场景分类与迁移顺序。

编码阶段原则：

- 可以并行做只读审计和测试分类；
- SQL 主路径、handler API、InnoDB worker scan 属于同一跨层执行链路，编码集成必须由主控串行合并；
- 测试文件迁移可在主路径编译稳定后并行准备，但 result 校准由主控统一完成；
- 每个编码批次完成后必须启动独立 review Agent 做代码/任务/资料检视。

## 迁移批次

### Batch A - 商用差异审计与迁移清单

Status: in progress

输出：

- `sql/parallel_query` 必迁文件和冲突 scaffold 清单；
- SQL 主干 hook 必迁文件清单；
- InnoDB/handler 必迁文件清单；
- MTR 场景矩阵：已支持、需迁移、可暂缓。

验收：

- 四个 Explorer 输出均已合并到本文档；
- 主控形成 Batch B/C/D/E 的具体文件级任务。

### Batch B - SQL/PQ 核心模块全量平移

Status: pending

优先文件：

- `sql/parallel_query/*`
- 商用独有文件：`barrier.h`、`bloom_filter.h`、`chunk_files_wrapper.h`、
  `explain_pq_access_path.*`、`pq_hash_join_shared_context.*`

必须恢复/对齐：

- `ParallelScanIterator`
- `PQblockScanIterator`
- `PQRefIterator`
- `Gather_operator`
- `Query_result_mq`
- `Exchange_nosort` / `Exchange_sort`
- clone / resolver / refix / hash join shared context

验收：

- `git diff --check`
- `cmake --build build-ninja --target mysqld -j 16`

### Batch C - SQL 主干 hook 全量平移

Status: pending

范围由 Batch A 审计确认，预计包括：

- optimizer / resolver / JOIN::optimize / AccessPath / iterator factory；
- EXPLAIN / optimizer trace；
- sysvar/status var；
- THD / JOIN / Query_block / handler 字段；
- SQL base/open/close、worker thread 相关 hook。

验收：

- `mysqld --no-defaults --verbose --help` 可启动；
- 普通非 PQ smoke 查询不崩溃；
- PQ sysvar/status 可见。

### Batch D - InnoDB/handler PQ 路径全量平移

Status: pending

优先文件：

- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb.*`
- `sql/handler.*`
- `sql/parallel_query/pq_handler.*`

必须恢复/对齐：

- `ha_pq_next()`
- `pq_worker_scan_next()`
- fullscan/range/ref/dependent-ref worker scan；
- ICP / native Record_buffer / read view / KILL / error cleanup；
- reverse / partition / MVI 能迁则迁，除非触发可跳过门槛。

验收：

- build 通过；
- 核心商用测试优先通过：`pq_fullscan`、`pq_range_clust`、`pq_range_sec`、
  `pq_icp`、`pq_jt_ref`、`pq_depend_ref`、`pq_kill`、`pq_worker_error`。

### Batch E - 商用 MTR 测试套全量迁移

Status: pending

原则：

- 迁移参考仓库 `mysql-test/suite/parallel_query` 中测试、include、opt、result；
- 先按商用测试名保留，必要时适配 8.0.46 result；
- 只有满足可跳过门槛的测试可 skip，并记录。

执行顺序：

1. core scan/ref/ICP/error/kill；
2. ORDER BY / GROUP BY / Record_buffer / partition / MVI；
3. hash join / derived / subquery / semijoin / distinct；
4. insert-select / replace-select / trace / audit / slow log / replication/env tests；
5. full `parallel_query` suite。

## Review 与验证闭环

每个 Batch 编码完成后：

1. 主控自查 diff scope；
2. 启动独立 review Agent 做代码/任务/资料检视；
3. 修复 review 中 Critical/Important 问题；
4. 运行对应 build/MTR；
5. 单独 commit；
6. 更新本文档状态。

最终验收命令：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help
cd mysql-test
TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl --suite=parallel_query --parallel=1
```

## 当前状态

- Batch A 四个并行 Explorer 已返回；
- 尚未改源码；
- 下一步：完成 Batch A review 后进入 Batch B/B0 compile-surface 迁移。

## Batch A 审计结果

### SQL/PQ 模块差异

必须平移或等价实现：

- `sql/parallel_query/pq_iterators.*`：商用 `ParallelScanIterator` 初始化 MQ
  gather、InnoDB snapshot、worker，`Read()` 从 MQ 拉 row；商用
  `PQblockScanIterator` / `PQRefIterator` 调 `pq_worker_scan_init()` +
  `ha_pq_next()`。当前同名类仍含 fail-closed/debug shadow/stub，必须替换
  为商用语义。
- `sql/parallel_query/sql_parallel.*`：商用包含
  `make_pq_gather_operator()`、`make_pq_leader_plan()`、`pq_worker_exec()`、
  `pq_make_join_readinfo()`、worker THD clone、leader/worker plan rewrite。
  当前多为 lifecycle/smoke/helper，必须迁主控流程。
- `pq_clone.*`、`pq_clone_item.cc`、`pq_resolver.*`、
  `pq_refix_fields_item.cc`、`pq_replace_base_item.cc`：当前 clone 近似
  placeholder；商用覆盖大量 `Item::pq_clone()`、JOIN/QEP_TAB/ORDER/condition
  /subquery clone，不迁无法真实 worker plan 执行。
- `query_result_mq.*`：当前是简化 string frame / PQWR smoke；商用使用
  `Field_raw_data`、worker tmp fields、NULL bitmap、stable rowid。最终应切到
  商用协议。
- `exchange.*`、`exchange_sort.*`、`binary_heap.h`：商用有真实
  `Sort_param`、record group、heap merge、stable rowid tie-break；当前主要是
  shape/smoke。
- 商用独有文件：`barrier.h`、`bloom_filter.h`、`chunk_files_wrapper.h`、
  `explain_pq_access_path.*`、`pq_hash_join_shared_context.*`。
- `pq_optimizer.*`：当前是保守 eligibility + smoke hooks；商用有完整
  `PQUnsuiteInfo`、RBO、`pq_support_features_switch`、limit/order/group/subquery
  判断。
- `pq_handler.*` / `sql/handler.*`：商用 handler API 是 `ha_pq_init()`、
  `ha_pq_next()`、`pq_worker_scan_init(uint keyno, void*)`、
  `pq_ref_build_ranges()`；当前是 8.0.46 typed context API，必须做兼容层或
  等价适配。

当前冲突 scaffold：

- `pq_iterator.*` / `PQTableScanIterator` / `TryCreatePQTableScanIterator()` 与
  商用 `ParallelScanIterator` 主入口冲突，后续应删除或降为历史适配层。
- 当前 `PQblockScanIterator` / `PQRefIterator` fail-closed 实现必须替换。
- 当前 ORDER BY preflight blocker 会阻止商用 `Exchange_sort`，必须在 ORDER BY
  商用路径迁移时替换。
- 当前大量 DBUG smoke/contract/shadow path 只保留少数可复用验证项，其余不得
  阻塞商用真实路径。

### SQL 主干 hook 差异

P0 主干 hook：

- `sql/sql_class.*`：THD leader/worker、`pq_mem_root`、gather 列表、worker
  manager、错误/KILL/status merge、cleanup。
- `sql/sql_lex.*`：`Query_block::parallel_exec`、clone link、
  `pq_try_clone_item`、subquery/derived suitability、
  `Query_expression::m_pq_shared_info`。
- `sql/sql_optimizer.*`：`JOIN::choose_parallel_tables()`、
  `suite_for_parallel_query()`、`qep_tab0/qep_tab1`、`need_tmp_pq_leader`、
  `idx_div_tab/idx_cut_tab`、optimizer state save/restore。
- `sql/sql_select.cc`、`sql/sql_executor.*`：leader/worker QEP 切换、临时表
  切片、JOIN destroy cleanup、`ParallelScanIterator` 包装入口。
- `sql/join_optimizer/*`：`PARALLEL_SCAN`、`PQ_BLOCK_SCAN`、`PQ_REF_SCAN`、
  iterator factory、materialize/shared temp table、hash join partial input、
  EXPLAIN traversal。
- `sql/handler.*`、`storage/innobase/handler/ha_innodb.*`、
  `ha_innopart.*`、`row0pread_pq.*`、`row0sel.*`：PQ scan lifecycle、
  partition/range/ref/record-buffer/ICP。
- `sql/sys_vars.cc`、`sql/system_variables.h`、`sql/mysqld.*`、
  `sql/sql_parse.cc`：sysvar/status/PSI/retry-without-PQ。
- `sql/opt_explain*`：EXPLAIN/TREE/JSON/ANALYZE 的 PQ 展示和 fallback reason。
- `sql/CMakeLists.txt`、`storage/innobase/CMakeLists.txt`、
  `storage/temptable/CMakeLists.txt`：新增商用源文件，处理当前自研源文件
  并存/收敛。

P1 场景 hook：

- `sql/item*`、`item_sum.*`、`item_subselect.*`：全量 Item clone、聚合拆分、
  subquery clone。
- `sql/sql_tmp_table.*`、`sql/sql_union.*`、`sql/sql_derived.cc`、
  `sql/query_result.*`、`sql/table.*`、`sql/sql_opt_exec_shared.h`：GROUP BY、
  derived、UNION、semijoin materialization、shared temp table。
- `sql/iterators/hash_join_*`、`sql/filesort.*`、range optimizer、record buffer、
  heap/temptable storage：hash join、ORDER BY、reverse scan、secondary range/ref
  /ICP、shared temp table。

### InnoDB/handler 差异

必须平移或等价实现：

- 商用 handler 生命周期：`pq_reverse_scan`、`pq_ref_depend`、`pq_ref`、
  `pq_table_scan`、`do_parallel_scan`、`pq_ctx`、`ha_pq_init()`、
  `ha_pq_next()`、`ha_pq_end()`。
- 商用 PQ 队列/ref 分发模型：`PQ_slices_mngr`、`PQ_ranges_queue`、
  `PQ_ref_map`、`Parallel_worker`、`Key_ref`，支持 full/range/ref/depend_ref
  多轮 dispatch。
- 商用 InnoDB scan init：`pq_index_scan_init()`、`pq_range_scan_init()`、
  `pq_ref_scan_init()`、`pq_ref_build_ranges()`、`ha_innopart::pq_*`。
- 商用 worker pull/Record_buffer：`PQ_Ctx::read_record()` 做页游标扫描、
  MVCC/ICP/cluster lookup、Record_buffer 填充。当前 `pq_worker_scan_next()`
  disabled，fullscan 多靠 callback producer。
- `row_prebuilt_t` / `row0sel.cc` hook：商用新增 `pq_ctx`、
  `is_attach_ctx`、`pq_heap`、`pq_tuple`、`pq_index_read`、`pq_ref_info`、
  `pq_worker`、`old_index` 等，并保存边界 tuple、处理 intrinsic old index、
  Record_buffer out_of_range。

需要优先替换的 stub/unsupported：

- `PQblockScanIterator::Init/Read()`；
- `PQRefIterator::Init/Read()`；
- `ha_innobase::pq_worker_scan_next()`；
- reverse fullscan/range/ref unsupported；
- `ha_innopart::pq_*` 缺失；
- secondary range/ref/ICP 的 bounded leader-local producer。

### MTR 差异

规模：

- 商用：98 个 `.test`、98 个 `.result-pq`、8 个 suite include、11 个
  `*-master.opt`；
- 当前：88 个 `.test`、88 个 `.result`，没有 suite-local `include/*.inc`，
  没有商用 `*-master.opt`；
- 同名测试只有 `pq_not_support.test`，且当前是缩小版，不等价。

必须迁移的商用场景：

- fullscan / projection / WHERE / read-view / bugfix；
- clustered range / secondary range / ref / dependent ref / ICP；
- ORDER BY / LIMIT / DISTINCT / reverse scan；
- GROUP BY / aggregate / aggregate distinct / round-truncate；
- hash join / semijoin / left join edge；
- derived / view / divide derived / subquery / correlated subquery / UNION；
- INSERT SELECT / REPLACE SELECT；
- error / KILL / abort / MQ / MDL / memory limit；
- EXPLAIN / EXPLAIN ANALYZE / optimizer trace / opt trace / CBO / variables；
- slow log / audit log / replica / dstore。

MTR 迁移矩阵：

| 状态 | 场景/测试 | 处理要求 |
|---|---|---|
| 已有相近覆盖但不等价，需按商用重迁 | `pq_fullscan`、`pq_blob`、`pq_not_equal`、`pq_aggr_no_record`、`pq_found_rows`、`pq_read_view`、`pq_rec_visible`、`pq_read_record_crash` | 当前有 fullscan/threaded/read-view smoke，但测试名和商用语义不等价；迁入商用测试并校准 result |
| 已有相近覆盖但不等价，需按商用重迁 | `pq_range_clust`、`pq_range_exception`、`pq_range_scan_reverse` | 当前偏 range planning/dispatch；迁入商用 clustered range 结果正确性 |
| 缺商用真实正向路径，需新增迁移 | `pq_range_sec`、`pq_icp`、`pq_index_scan_desc`、`pq_reverse_index_scan`、`pq_sec_index_min`、`pq_ref_build_range`、`pq_ref_reverse_scan`、`pq_jt_ref`、`pq_depend_ref`、`pq_join_bka` | 当前 `pq_commercial_ref_icp` 多为 probe/fallback/contract；必须随 worker pull/ref/ICP 路径迁移 |
| 缺商用真实正向路径，需新增迁移 | `pq_order_by`、`pq_order_const`、`pq_limit_no_order_by`、`pq_distinct`、`pq_tmp_key`、`pq_multi_value` | 当前 ORDER BY 多为 skeleton/preflight；不得默认 skip，进入 ORDER BY gate 追踪 |
| 已有相近覆盖但不等价，需补商用扩展 | `pq_group_by`、`pq_agg_distinct`、`round_truncate_for_pq`、`pq_support_features_switch`、`pq_dev_bugs` | 当前覆盖基础 aggregate/partial group；商用 aggregate distinct、round/truncate、feature switch 需迁 |
| 缺商用真实正向路径，需新增迁移 | `pq_hash_join`、`pq_hash_join_error`、`pq_semijoin`、`pq_left_join_zero_rows` | 属于 hash join/shared context gate；不得标记 skip，除非触发 spill/VFD 等大风险 |
| 缺商用真实正向路径，需新增迁移 | `pq_derived_view`、`pq_divide_derived*`、`pq_subquery`、`pq_subquery_correlated`、`pq_union`、`refactor_fix_fields` | 属于 clone/materialization gate；失败记录 blocker，不等同 skip |
| 缺失，需新增迁移 | `parallel_insert_select*`、`parallel_replace_select*` | INSERT/REPLACE SELECT 语义风险高，但仍进入追踪；不在首批验收不等于 skip |
| 已有相近覆盖但不等价，需按商用重迁 | `pq_abort`、`pq_auto_retry_failed_parallel_query`、`pq_worker_error`、`pq_mq_error`、`pq_kill`、`pq_kill_query`、`pq_mdl_lock`、`pq_memory_limit`、`pq_leader_exception` | 当前有 worker error/KILL/MDL smoke；迁商用 debug/restart/error 注入 |
| 已有基础覆盖但需商用扩展 | `pq_explain*`、`pq_explain_analyze`、`pq_optimizer_trace*`、`pq_opt_trace`、`pq_variables`、`pq_cbo`、`pq_master_disable` | 当前基础 EXPLAIN/vars/status 已有；商用 trace/CBO/ANALYZE 需迁 |
| 仅满足跳过门槛才可暂缓 | `pq_not_support_dstore` | 依赖 Dstore 私有引擎，可暂缓并记录 |
| 仅满足跳过门槛才可暂缓 | `pq_audit_log`、`pq_replica_enable`、TPCH data 相关 `pq_explain_analyze` 片段 | 依赖插件、复制拓扑、本地 TPCH 数据；先记录环境 blocker |
| 仅满足跳过门槛才可暂缓 | `pq_hash_join*` 中 on-disk spill/PFS/VFD 子场景、`pq_divide_derived_low_mmap`、`pq_divide_derived_no_mmap`、restart/debug-injection 类测试 | 不是整个功能 skip，只允许对应环境/资源敏感子场景暂缓 |

商用 suite include 也需迁移或记录跳过：

- `pq_abort.inc`
- `pq_coverage.inc`
- `pq_divide_derived_setup.inc`
- `pq_on_disk_hash_join.inc`
- `pq_round_truncate_queries.inc`
- `pq_support_aggregate_query.inc`
- `tpch_prepare.inc`
- `tpch_cleanup.inc`

### 当前大风险约束

这些不是默认跳过项，但需要单独 gate、单独记录：

- Handler/InnoDB API 分叉：商用 `void *scan_ctx + ha_pq_next()` vs 当前
  `PQ_Leader_context/PQ_Worker_context/PQ_row_sink` typed API；
- Plan clone / Item clone 覆盖面极大；
- worker THD/TABLE/handler/read-view 生命周期；
- ORDER BY stable output 与 `position(record)` rowid / `cmp_ref()` tie-break；
- Hash join shared context / spill-to-disk；
- derived/subquery/UNION/semijoin materialization；
- INSERT SELECT 行锁/binlog/复制语义；
- non-covering secondary + ICP 多 worker clustered lookup/MVCC；
- partition/MVI/intrinsic temp table/reverse scan 组合。

## Batch B/C/D/E 文件级任务

### Batch B0 - Compile surface and commercial source inventory

Status: pending

目标：

- 新增商用独有 `sql/parallel_query` 文件；
- 调整 CMake，使商用源文件进入构建；
- 不打开真实执行 gate，只解决编译表面和类型可见性；
- 明确当前自研 `pq_iterator.*`、`pq_aggregate.*`、
  `pq_group_aggregate_iterator.*` 与商用路径的并存/删除策略。

### Batch B1 - Commercial SQL/PQ core replacement

Status: pending

目标：

- 平移/适配 `sql_parallel.*`、`pq_iterators.*`、`query_result_mq.*`、
  `exchange.*`、`exchange_sort.*`、`pq_optimizer.*`；
- 平移/适配 `pq_clone.*`、`pq_clone_item.cc`、`pq_resolver.*`、
  `pq_refix_fields_item.cc`、`pq_replace_base_item.cc`；
- 平移/适配 `pq_handler.*`，并与 `sql/handler.*` / InnoDB typed context 兼容；
- 新增并接入商用独有核心文件：`explain_pq_access_path.*`、
  `pq_hash_join_shared_context.*`、`barrier.h`、`bloom_filter.h`、
  `chunk_files_wrapper.h`；
- 当前 fail-closed guard 不得阻塞商用主路径；
- B1 不以打开 hash join / derived / insert-select 为验收条件，但不得把它们
  标记为 skip；必须进入后续 batch/gate 追踪。

### Batch C1 - SQL main hook alignment

Status: pending

目标：

- 对齐 THD / Query_block / JOIN / AccessPath / EXPLAIN / sysvar / status /
  handler hook；
- 先让商用 plan rewrite 能生成和 explain，不要求所有场景执行成功；
- 不成功必须记录 blocker/gate，不等同 skip；fallback/skip 必须有明确 reason。

### Batch D1 - Handler/InnoDB full worker path

Status: pending

目标：

- 建立 8.0.46 typed API 与商用 `ha_pq_next()`/worker pull 语义的兼容层；
- 接 fullscan、clustered range、secondary range、ref/depend_ref、ICP、
  Record_buffer、read-view、KILL/error cleanup；
- partition/MVI/reverse/intrinsic temp table 按高风险 gate 逐项打开或记录。

### Batch E1 - Commercial MTR migration

Status: pending

目标：

- 尽量全量迁入商用 `parallel_query` 测试套；
- 先迁 include/opt，再迁 `.test`/result；
- 只有 dstore/audit plugin/replica topology/TPCH data/hash-spill/restart-debug
  等本地不可构造或大风险项允许暂时 skip。
