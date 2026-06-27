# Parallel Query 商用全量迁移冲刺

Last synced: 2026-06-24

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

Status: completed

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

开发阶段执行约束：

- 每个可独立验收的小任务必须单独 commit，禁止把多个迁移点积累成一个大提交；
- 开发期只运行当前功能相关的 build/MTR 目标验证；
- 完整 `parallel_query` suite、`mysqld --verbose --help` 和更大范围回归放到阶段
  收尾或开发结束后统一执行；
- 若小任务只改文档，可运行 `git diff --check` 后提交，不强制编译。

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
- 已进入 worker 执行链路小步迁移：typed pull bridge、Record_buffer 诊断、
  worker ExecuteIterator smoke gate、readinfo gate 和 production readinfo
  fail-closed 风险收敛均已独立提交；
- 最新 worker 执行链路提交：
  - `098d9af3aa5` Add PQ worker iterator construction smoke；
  - `ad69503c511` Use worker table for PQ iterator smoke；
  - `3a1907cccf1` Add PQ worker iterator init smoke；
  - `dea81de92e5` Add PQ worker iterator read smoke。
- 当前已证明 debug-only smoke 能用 worker THD + worker TABLE 构造
  `PQ_BLOCK_SCAN -> PQblockScanIterator`，完成 root iterator `Init()/Read()`，
  并通过 worker-owned `Query_result_mq` 发送/解码 ROW + FINISH smoke frame；
- D1.6d 已将 worker output fields 推进到 `Query_block::fields` /
  `base_ref_items` worker-owned `Item_field`；
- 当前最新本地批次已将 worker `PQWR` 消费链路推进到
  `PQTableScanIterator::Read()` debug-only gate：leader 通过
  `MQ_record_gather -> Exchange_nosort -> table->record[0]` 消费
  `Query_result_mq` worker-result row；
- 下一步：继续缩小商用 `ParallelScanIterator` 主路径差距，优先把
  worker-thread producer、leader record gather 和可见 fullscan gate 的
  生命周期顺序对齐；开发阶段仍只跑相关模块 MTR，不跑全量 MTR。

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

Status: completed and committed

目标：

- 新增商用独有 `sql/parallel_query` 文件；
- 调整 CMake，使商用源文件进入构建；
- 不打开真实执行 gate，只解决编译表面和类型可见性；
- 明确当前自研 `pq_iterator.*`、`pq_aggregate.*`、
  `pq_group_aggregate_iterator.*` 与商用路径的并存/删除策略。

Completion Report - Batch B0:

- changed files:
  - `sql/CMakeLists.txt`
  - `sql/parallel_query/barrier.h`
  - `sql/parallel_query/bloom_filter.h`
  - `sql/parallel_query/chunk_files_wrapper.h`
  - `sql/parallel_query/explain_pq_access_path.cc`
  - `sql/parallel_query/explain_pq_access_path.h`
  - `sql/parallel_query/pq_hash_join_shared_context.cc`
  - `sql/parallel_query/pq_hash_join_shared_context.h`
- implementation:
  - copied commercial-only source/header files into current tree；
  - added `parallel_query/explain_pq_access_path.cc` and
    `parallel_query/pq_hash_join_shared_context.cc` to `sql/CMakeLists.txt`；
  - kept `barrier.h` / `bloom_filter.h` / `chunk_files_wrapper.h` as imported
    commercial headers；
  - adapted `explain_pq_access_path.cc` to a compile-only 8.0.46 compatibility
    shell because the commercial implementation depends on not-yet-migrated
    `ExplainChild` / `WalkAccessPathsProxy` / iterator timing hooks；
  - adapted `pq_hash_join_shared_context.h` to a compile-only shell because
    the commercial implementation depends on not-yet-migrated VFD /
    `ChunkFilesWrapper` / hash join spill integration；
  - these shells are not skip decisions：C1/hash-join batches must restore the
    real commercial behavior when their main hooks are migrated.
- validation:
  - `git diff --check -- sql/CMakeLists.txt sql/parallel_query` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed。
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - `explain_pq_access_path.*` is compile-visible but not functionally wired；
  - `pq_hash_join_shared_context.*` is compile-visible but not functionally wired；
  - `chunk_files_wrapper.h` still references commercial VFD/hash-spill headers
    and must not be included by production code until the hash join spill gate
    migrates its dependencies.
- review:
  - Code / Docs / Test review accepted before commit `0ccab8e5f54`。

### Batch B1 - Commercial SQL/PQ core replacement

Status: completed

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

#### Batch B1a - pq_handler commercial dispatch substrate

Status: completed and committed

目标：

- 保留当前 8.0.46 typed API：`PQ_Leader_context`、
  `PQ_Worker_context`、`PQ_Scan_ctx`、`PQ_Ctx`、`PQ_Range`；
- 不回退到商用旧 handler API；
- 迁入商用队列/切片管理/ref-key dispatch 语义，为后续
  `PQblockScanIterator` / `PQRefIterator` / worker pull 路径提供基础设施。

Completion Report - Batch B1a:

- changed files:
  - `sql/parallel_query/pq_handler.h`
  - `sql/parallel_query/pq_handler.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added commercial-equivalent `PQ_queue` / `PQ_dual_queue` /
    `PQ_single_queue` / `PQ_ranges_queue` / `PQ_slices_mngr`；
  - added vector-owned ref-key map classes：`PQ_slices_map` / `PQ_ref_map`；
  - extended `PQ_Config` with commercial ref-key fields
    `m_ref_key` / `m_ref_key_len` / `m_ref_depend`；
  - restored `PQ_Range::split()` commercial flow：border extraction、
    index S-lock partition、ordered split handoff；
  - restored `PQ_Leader_context::build_ranges()` commercial flow：
    create scan ctx、insert default/dependent-ref queue、partition and push ranges；
  - restored `PQ_Worker_context::dispatch_ctx()` / `get_ctx()` commercial flow：
    global slice fetch、split retry、local dual queue round rotation、ref-key check；
  - kept current InnoDB `PQ_Worker_open_context` and wrapper kind checks
    unchanged.
- validation:
  - `git diff --check -- sql/parallel_query/pq_handler.h
    sql/parallel_query/pq_handler.cc Docs/pq_tasks/commercial-full-port-sprint.md`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - current InnoDB wrapper still uses `InnoDB_pq_leader_ctx::dispatch_next_range()`
    for existing callback producer path；后续 D1 必须把 worker pull/ref/range
    execution 接到 B1a 的 queue/ref-key dispatch substrate；
  - `PQ_Scan_ctx::make_ctx()` remains abstract；真实 worker pull context 仍需在
    InnoDB migration 中补齐；
  - reverse/range/ref/dependent-ref/ICP 真实场景仍未打开，本批只恢复基础设施。
- review:
  - Code / Docs / Test review accepted；no Critical / Important findings；
  - review noted that split wait / dependent-ref key bookkeeping must be
    revisited before exposing this substrate to user-visible ref/range paths.
  - committed as `6a2f2ad99e3`。

#### Batch B1b - worker iterator typed handler lifecycle

Status: completed and committed

目标：

- 将当前 `PQblockScanIterator` / `PQRefIterator` 从裸 fail-closed stub 推进到
  当前 8.0.46 typed handler API 的生命周期骨架；
- 保持商用 worker iterator 的 init/read/end 语义方向，但不恢复旧
  `ha_pq_next(void*)` API；
- 不打开用户可见场景：当前 InnoDB `pq_worker_scan_next()` 仍返回
  `HA_ERR_UNSUPPORTED`，所以本批只建立调用面。

Completion Report - Batch B1b:

- changed files:
  - `sql/parallel_query/pq_iterators.h`
  - `sql/parallel_query/pq_iterators.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added `PQ_Worker_context *` ownership fields and EOF/init state to
    `PQblockScanIterator` and `PQRefIterator`；
  - `Init()` now obtains `PQ_worker_info` from worker THD, binds/validates
    `PQ_Worker_open_context` against the current worker TABLE/handler, and
    calls `handler::pq_worker_scan_init()`；
  - `Read()` now calls `handler::pq_worker_scan_next()` with the typed worker
    context, handles EOF, deleted rows, examined row accounting, and optional
    rowid positioning；
  - `End()` / destructor now call `handler::pq_worker_scan_end()` idempotently
    and clear `PQ_worker_info::m_worker_ctx` when it owns the same context；
  - `PQRefIterator` keeps first-row lookup construction before pulling rows.
- validation:
  - `git diff --check -- sql/parallel_query/pq_iterators.h
    sql/parallel_query/pq_iterators.cc` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed。
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - InnoDB pull row path is still disabled, so this lifecycle will currently
    fail closed with `HA_ERR_UNSUPPORTED`；
  - DOP>1 worker-id binding depends on the worker execution thread wiring in
    `sql_parallel.*`；本批只使用 worker THD 上的 `PQ_worker_info`；
  - ref-key dependent range build is not restored yet；后续 D1/C1 需要把
    `PQRefIterator` 的 ref lookup 与 B1a 的 ref-key slice map 接通。
- review:
  - initial review required preserving fail-closed semantics when
    `pq_worker_scan_next()` returns `HA_ERR_UNSUPPORTED` with `eof=true`；
  - fixed `Read()` to treat EOF only when `error == 0 && eof`；
  - fixed `Init()` to publish `PQ_Worker_context` only after successful local
    init, and to reject replacing an existing worker context.
  - re-review accepted；no remaining required fixes.
  - committed as `e2cd506b429`。

#### Batch B1c - InnoDB typed pull-row path decision

Status: deferred to D1

结论：

- 不在 B1 阶段把 `ha_innobase::pq_worker_scan_next()` 改成
  callback-backed row cache；
- 不接入当前 latent `InnoDB_pq_ctx::read_record()` /
  worker-local `row_search_mvcc()` path；
- 继续保持 `pq_worker_scan_next()` fail-closed unsupported，直到 D1
  正式迁移商用 InnoDB `PQ_Ctx::read_record()` cursor path 或等价安全实现。

依据：

- 当前仓库 `row0pread_pq.h` 明确说明 worker-local `row_search_mvcc()` latent
  path 不应接入真实 `PQTableScanIterator::Read()`；
- 商用 `ha_pq_next()` 并不是预缓存全部 row image，而是通过
  `dispatch_ctx()` / `PQ_Ctx::read_record()` 逐行拉取，range EOF 后继续派发；
- callback producer 已是当前仓库验证过的 row flow，更适合后续先接
  `sql_parallel.*` / MQ worker 主路径；
- row-cache pull adapter 对大表/宽表/DOP 的内存、KILL、error propagation、
  ref-key 变化语义风险较高。

D1 必须处理：

- 迁移或等价实现商用 `storage/innobase/row/row0pread_pq.cc::PQ_Ctx` cursor
  path；
- 将 B1a 的 `PQ_Worker_context::dispatch_ctx()` / ref-key queue 与 InnoDB
  worker scan 真实接通；
- 明确 fullscan/range/ref/dependent-ref/ICP/reverse 的 gate 和测试覆盖；
- 保证 `pq_worker_scan_next()` unsupported/error 不伪装 EOF。

### Batch C1 - SQL main hook alignment

Status: completed

目标：

- 对齐 THD / Query_block / JOIN / AccessPath / EXPLAIN / sysvar / status /
  handler hook；
- 先让商用 plan rewrite 能生成和 explain，不要求所有场景执行成功；
- 不成功必须记录 blocker/gate，不等同 skip；fallback/skip 必须有明确 reason。

#### Batch C1a - Gather_operator commercial state carriers

Status: completed and committed

目标：

- 先把商用 `Gather_operator` 依赖的状态字段引入当前 8.0.46 代码；
- 不打开 `make_pq_gather_operator()`、worker plan clone、ORDER BY/hash join
  执行路径；
- 保持现有 worker lifecycle 和 fail-closed 行为不变。

Completion Report - Batch C1a:

- changed files:
  - `sql/parallel_query/sql_parallel.h`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added commercial-like `PQTab` state descriptor；
  - added `Gather_operator(uint32 dop, MEM_ROOT *root, uint32 ring_size)`
    constructor for later MEM_ROOT-owned subquery state；
  - added inert commercial alignment fields:
    `m_template_join`、`pq_tabs`、`tab_set`、`m_ha_err`、
    `m_pq_hash_join_shared_context`、`m_uncorrelated_subqueries`、
    `m_desc_groups`；
  - included the current 8.0.46 headers needed for `PQTabType`、
    `Mem_root_array` and the complete
    `HashJoin::PQHashJoinSharedContext` type。
- validation:
  - `git diff --check -- sql/parallel_query/sql_parallel.h` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - this batch is only a state-carrier step; it does not migrate commercial
    `make_pq_gather_operator()` or leader/worker QEP rewrite；
  - `PQTab::m_pq_ctx` remains opaque until D1/C1 later connects the handler
    and InnoDB worker context；
  - `tab_set` initial value follows commercial intent but must be rechecked
    when the optimizer table-classification hook is opened。
- review:
  - independent Review Agent accepted the patch；
  - confirmed `PQHashJoinSharedContext` complete-type ownership is safe；
  - confirmed new fields are inert and do not open new execution behavior；
  - noted `access_path.h` include is broad but acceptable for C1a; future
    cleanup may split `PQTabType` into a smaller shared type header.

#### Batch C1b - sql_parallel commercial entry surface and thread budget guard

Status: completed and committed

目标：

- 先把商用 `sql_parallel` 主路径需要的入口符号和状态类型补齐到当前
  8.0.46 分支；
- 对齐商用 `check_pq_running_threads()` / `release_pq_running_threads()`
  的全局线程预算等待/唤醒语义；
- 不接 `JOIN::optimize()`、`sql_select.cc`、handler/InnoDB，不打开真实
  plan rewrite 或 worker 执行。

Completion Report - Batch C1b:

- changed files:
  - `sql/parallel_query/sql_parallel.h`
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/pq_resource_stat.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added commercial state carrier `PQ_optimized_var` and `PQ_exec_status`；
  - declared commercial entry functions:
    `make_pq_gather_operator()`、`make_pq_leader_plan()`、
    `make_pq_unit_plan()`、`pq_worker_exec()`、
    `pq_make_join_readinfo()`、`pq_check_stable_sort()`、
    `EstimatePQGatherOperatorCost()`；
  - implemented C1b fail-closed stubs: plan functions return `SEQ_EXEC` or
    `nullptr`，join readinfo returns failure，cost function is no-op；
  - added `check_pq_running_threads()` with mutex/cond timed wait and refusal
    counter；
  - updated `release_pq_running_threads()` to use the same mutex and broadcast
    `COND_pq_threads_running` after release。
- validation:
  - `git diff --check -- sql/parallel_query/sql_parallel.h
    sql/parallel_query/sql_parallel.cc sql/parallel_query/pq_resource_stat.cc`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - `PQ_optimized_var` is only a type carrier; `JOIN::pq_optimized_var` remains
    the existing opaque pointer until the optimizer state save/restore batch；
  - per-THD `pq_threads_running` from commercial code is not introduced in this
    batch to avoid widening THD layout changes；
  - the thread-budget mutex/cond must be initialized before future real calls
    are made from optimizer/execution hooks。
- review:
  - independent Review Agent accepted the patch；
  - confirmed entry stubs fail closed and do not open optimizer/executor
    behavior；
  - confirmed thread-budget accounting is protected by the PQ mutex and
    release broadcasts waiters；
  - noted no current call sites, so mutex/cond initialization risk is not
    reachable until future hooks call the budget guard。

#### Batch C1c - PQ thread-budget runtime init and commercial status surface

Status: completed and committed

目标：

- 对齐商用 PQ 线程预算同步对象的 server lifecycle；
- 暴露商用基础 `PQ_*` status，便于后续真实 hook 打开后观察线程/内存预算；
- 不打开 optimizer / executor / handler / InnoDB 行为。

Completion Report - Batch C1c:

- changed files:
  - `sql/mysqld.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - included `sql/parallel_query/pq_resource_stat.h` in `mysqld.cc`；
  - added PSI keys for `LOCK_pq_threads_running` and
    `COND_pq_threads_running`；
  - initialized the PQ mutex/cond in `init_thread_environment()` and destroyed
    them in `clean_up_mutexes()`；
  - registered the PQ mutex/cond with server PSI instrumentation；
  - added basic commercial status variables:
    `PQ_threads_refused`、`PQ_memory_refused`、`PQ_threads_running`、
    `PQ_memory_used`、`PQ_stmt_executed`。
- validation:
  - `git diff --check -- sql/mysqld.cc` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - status counters are infrastructure only; current true execution still
    depends on later C/D batches；
  - `PQ_stmt_executed` is present for commercial parity but is not yet updated
    by the current scaffold execution path；
  - no optimizer or execution hook was opened in this batch。
- review:
  - independent Review Agent accepted the patch；
  - confirmed PSI key declaration/registration/init/destroy is complete；
  - confirmed basic `PQ_*` status variable types match current global
    counters；
  - confirmed no optimizer/executor/handler/InnoDB behavior was opened。

### Batch D1 - Handler/InnoDB full worker path

Status: in progress

目标：

- 建立 8.0.46 typed API 与商用 `ha_pq_next()`/worker pull 语义的兼容层；
- 接 fullscan、clustered range、secondary range、ref/depend_ref、ICP、
  Record_buffer、read-view、KILL/error cleanup；
- partition/MVI/reverse/intrinsic temp table 按高风险 gate 逐项打开或记录。

#### Batch D1.0 - handler commercial PQ API bridge

Status: completed

目标：

- 先恢复商用 handler 层字段和薄包装函数，给后续 worker iterator 切回
  `ha_pq_next()` / pull-row path 提供稳定接口；
- 保留当前 8.0.46 typed `PQ_Leader_context` / `PQ_Worker_context` API；
- 不修改 handler `inited` 状态机，不打开 InnoDB 正路径。

Completion Report - Batch D1.0:

- changed files:
  - `sql/handler.h`
  - `sql/handler.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added commercial handler state fields:
    `pq_reverse_scan`、`pq_ref_depend`、`pq_ref`、`pq_table_scan`、
    `do_parallel_scan`、`pq_range_type`、`pq_ref_key`、`pq_ctx`；
  - added public wrappers:
    `ha_pq_init()`、`ha_pq_end()`、`ha_pq_next()`、
    `ha_reverse_scan()`、`ha_set_reverse_scan()`；
  - added commercial virtual bridge methods:
    `pq_leader_scan_init(uint, void *&, uint)`、
    `pq_worker_scan_init(uint, void *)`、
    `pq_ref_build_ranges(void *, Key_ref &)`、
    `pq_worker_scan_next(void *, uchar *)`、
    no-arg `pq_worker_scan_end()` and `pq_leader_scan_end(void *)`；
  - all new virtual defaults fail closed with `HA_ERR_UNSUPPORTED` or no-op
    cleanup；
  - `ha_pq_next()` mirrors the commercial wrapper shape for generated-column
    update, MVI duplicate filtering, and row status update, but remains
    unreachable until an engine overrides the commercial pull API。
- validation:
  - `git diff --check -- sql/handler.h sql/handler.cc` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - this batch deliberately does not add `inited == PQ`; true handler state
    transitions must be restored together with InnoDB pull-row path；
  - `PQ_shared_info` is only forward-declared in `handler.h`; real shared
    hash-join/worker data support remains later；
  - default no-op `pq_worker_scan_end()` / `pq_leader_scan_end(void *)` are
    safe for the bridge surface but must be overridden by InnoDB before real
    execution is opened。
- review:
  - independent Review Agent accepted the bridge; no critical, important, or
    minor findings were reported。

#### Batch D1.1 - pq_handler commercial type alignment

Status: completed

目标：

- 在现有 8.0.46 typed `pq_handler` API 上补齐商用实现依赖的类型名和
  生命周期 carrier；
- 让后续 InnoDB 商用代码可以引用 `PQ_Config_Base`、
  `PQ_Scan_ctx_Base`、`PQ_Ctx_Base`、`Parallel_leader_Base`、
  `Parallel_worker`、`PQ_shared_info`、`Key_ref`；
- 保持当前 typed `PQ_Leader_context` / `PQ_Worker_context` 调用点不变；
- 不打开 handler/InnoDB 执行路径。

Planned implementation:

- `sql/parallel_query/pq_handler.h`
  - add commercial-compatible aliases for existing typed classes where the
    semantics already match；
  - add `Parallel_worker` as a commercial-name worker dispatcher over
    `PQ_slices_map` / `PQ_ref_map`；
  - add `PQ_temp_table_type`、`PQ_shared_info`、`Key_ref` definitions。
- `sql/parallel_query/pq_handler.cc`
  - implement `Parallel_worker::dispatch_ctx()` and `get_ctx()` using the same
    dispatch semantics as `PQ_Worker_context`。

Completion Report - Batch D1.1:

- changed files:
  - `sql/parallel_query/pq_handler.h`
  - `sql/parallel_query/pq_handler.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added commercial-compatible aliases:
    `Iter_Base`、`Iters`、`Ranges`、`Slices_mngr`、
    `PQ_Config_Base`、`PQ_Scan_ctx_Base`、`PQ_Ctx_Base`、
    `Parallel_leader_Base`；
  - added `PQ_Scan_ctx::max_threads()` and default `index_s_own()` for
    commercial subclass compatibility；
  - added commercial-compatible `Parallel_worker` dispatcher over
    `PQ_slices_map` / `PQ_ref_map`；
  - added `PQ_temp_table_type`、`PQ_shared_info`、`Key_ref` carriers；
  - kept existing typed `PQ_Leader_context` / `PQ_Worker_context` call sites
    unchanged and did not open any handler/InnoDB path。
- validation:
  - `git diff --check -- sql/parallel_query/pq_handler.h sql/parallel_query/pq_handler.cc Docs/pq_tasks/commercial-full-port-sprint.md` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - `PQ_Config_Base` is an alias to current `PQ_Config`; it intentionally does
    not restore commercial raw-pointer `clear()` ownership yet；
  - `Parallel_worker` is a compatibility adapter and is not wired into
    `row_prebuilt_t` or `ha_innodb_pq.cc` in this batch；
  - dependent-ref, partition, MVI, reverse scan, and temp-table sharing remain
    later gated execution work。
- review:
  - independent Review Agent accepted the patch；
  - confirmed aliases and carriers do not break existing typed API or open
    execution paths；
  - noted that `PQ_Config_Base` / `PQ_Scan_ctx_Base` / `PQ_Ctx_Base` /
    `Parallel_leader_Base` are aliases, not independent commercial base
    classes, so future direct commercial `row0pread_pq.h` porting still needs
    adaptation。

Validation:

- `git diff --check -- sql/parallel_query/pq_handler.h sql/parallel_query/pq_handler.cc Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`

Risk constraints:

- `Parallel_worker` is infrastructure only until `row_prebuilt_t` and
  `ha_innodb_pq.cc` are aligned；
- `PQ_shared_info` is carrier-only; temp table/hash join sharing is not opened
  in this batch；
- high-risk partition/MVI/reverse/ref-dependent execution remains gated by
  later D1.x batches。

#### Batch D1.2 - row_prebuilt_t commercial PQ carriers

Status: completed

目标：

- 在 `row_prebuilt_t` 中恢复商用 InnoDB PQ pull-row path 依赖的状态
  carrier；
- 只补字段和 include 依赖，不接 `ha_innodb_pq.cc` 商用执行路径；
- 为后续 `pq_worker_scan_init(uint, void*)`、`ha_pq_next()`、
  dependent-ref/range path 提供存储位置。

Planned implementation:

- `storage/innobase/include/row0mysql.h`
  - include `sql/parallel_query/pq_handler.h`；
  - add carrier fields:
    `is_attach_ctx`、`pq_heap`、`pq_tuple`、`pq_index_read`、
    `pq_ref_info`、`old_index`、debug-only `pq_prev_ctx`、`pq_worker`。

Validation:

- `git diff --check -- storage/innobase/include/row0mysql.h Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`

Risk constraints:

- `pq_heap` / `pq_tuple` allocation and cleanup remain later work；this batch
  only adds default-null carriers；
- `pq_prev_ctx` / `pq_worker` are raw `void *` placeholders in this batch
  because `row_prebuilt_t` is allocated by `mem_heap_zalloc()`；true
  `std::shared_ptr` ownership requires an explicit construction/destruction
  policy and is deferred to the commercial worker init path；
- `old_index` is carrier-only until the commercial old-share path is aligned；
- this batch must not change ordinary InnoDB reads or current typed PQ path。

Completion Report - Batch D1.2:

- changed files:
  - `storage/innobase/include/row0mysql.h`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - included `sql/parallel_query/pq_handler.h` for `PQ_Ref_info`；
  - added commercial POD/raw-pointer carriers to `row_prebuilt_t`:
    `is_attach_ctx`、`pq_heap`、`pq_tuple`、`pq_index_read`、
    `pq_ref_info`、`old_index`、debug-only `pq_prev_ctx`、`pq_worker`；
  - intentionally did not add `std::shared_ptr` fields because
    `row_prebuilt_t` is allocated by `mem_heap_zalloc()` in
    `row_create_prebuilt()` and would need explicit C++ construction and
    destruction policy。
- validation:
  - `git diff --check -- storage/innobase/include/row0mysql.h Docs/pq_tasks/commercial-full-port-sprint.md` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`。
- risks carried forward:
  - true commercial `pq_ctx` / `pq_worker` ownership remains deferred until the
    worker init/end path is ported with explicit construction/destruction；
  - no `ha_innodb_pq.cc`、`row0sel.cc`、or read path behavior changed in this
    batch。
- review:
  - independent Review Agent accepted the batch with no Critical issues；
  - reviewer noted `pq_prev_ctx` is intentionally debug-only and that
    default member initializers are documentary only under `mem_heap_zalloc()`；
  - reviewer also noted `row0mysql.h` -> `pq_handler.h` is a heavier layering
    dependency but found no direct circular include or compile blocker。

#### Batch D1.3 - row_prebuilt_t PQ shared ownership lifecycle

Status: completed and committed

目标：

- 将 D1.2 中暂时使用的 `void *` PQ ownership carrier 恢复为商用形态
  `std::shared_ptr<PQ_Ctx_Base>`、debug-only `std::shared_ptr<PQ_Ctx>`、
  `std::shared_ptr<Parallel_worker>`；
- 在 8.0.46 `row_prebuilt_t` 仍通过 `mem_heap_zalloc()` 分配的前提下，
  为这些新增 shared ownership 成员补上显式 construction/destruction；
- 只建立生命周期基础，不启用 `ha_innodb_pq.cc` 商用 pull-row 执行路径。

Planned implementation:

- `storage/innobase/include/row0mysql.h`
  - add `pq_ctx` as `std::shared_ptr<PQ_Ctx_Base>`；
  - change debug-only `pq_prev_ctx` to `std::shared_ptr<PQ_Ctx>`；
  - change `pq_worker` to `std::shared_ptr<Parallel_worker>`。
- `storage/innobase/row/row0mysql.cc`
  - explicitly placement-new the three PQ shared_ptr members immediately after
    `mem_heap_zalloc()` in `row_create_prebuilt()`；
  - explicitly destroy those PQ shared_ptr members in `row_prebuilt_free()`
    before `mem_heap_free(prebuilt->heap)`。
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
  - override commercial `pq_worker_scan_init(uint, void*)` and
    no-arg `pq_worker_scan_end()`；
  - keep commercial `pq_worker_scan_next(void*, uchar*)` fail-closed with
    `HA_ERR_UNSUPPORTED`。

Validation:

- `git diff --check -- storage/innobase/include/row0mysql.h storage/innobase/row/row0mysql.cc storage/innobase/handler/ha_innodb.h storage/innobase/handler/ha_innodb_pq.cc Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d13-mtr-vardir --tmpdir=/tmp/pq-d13-mtr-tmpdir`

Risk constraints:

- this batch must not call `ha_pq_next()` or wire commercial
  `pq_worker_scan_next(void*, uchar*)`；
- direct full-object placement construction/destruction for `row_prebuilt_t`
  is intentionally not introduced in this batch because it would alter a broad
  legacy allocation contract；only the new PQ shared_ptr fields are managed；
- commercial worker init/end semantics remain a later batch after this storage
  location is safe to own real objects。

Completion Report - Batch D1.3:

- changed files:
  - `storage/innobase/include/row0mysql.h`
  - `storage/innobase/row/row0mysql.cc`
  - `storage/innobase/handler/ha_innodb.h`
  - `storage/innobase/handler/ha_innodb_pq.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - restored commercial shared ownership carriers in `row_prebuilt_t`:
    `pq_ctx`、debug-only `pq_prev_ctx`、`pq_worker`；
  - added `row_prebuilt_pq_construct()` / `row_prebuilt_pq_destroy()` so these
    shared_ptr members have a valid C++ lifetime despite `mem_heap_zalloc()`；
  - added InnoDB overrides for commercial worker init/end and kept
    `pq_worker_scan_next(void*, uchar*)` unsupported；
  - `pq_worker_scan_init(uint, void*)` prepares `Parallel_worker`,
    `pq_ctx`、`pq_ref_info`、`is_attach_ctx` and active index state, but does
    not introduce commercial `inited == PQ` state or produce rows。
- validation:
  - `git diff --check -- storage/innobase/include/row0mysql.h storage/innobase/row/row0mysql.cc storage/innobase/handler/ha_innodb.h storage/innobase/handler/ha_innodb_pq.cc Docs/pq_tasks/commercial-full-port-sprint.md` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    returned `rc=0`；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d13-mtr-vardir --tmpdir=/tmp/pq-d13-mtr-tmpdir`
    passed: all 89 tests successful。
- risks carried forward:
  - `pq_worker_scan_next(void*, uchar*)` still does not read rows；
  - commercial `inited == PQ` state is not available in this branch and remains
    deferred to the real pull-row execution batch；
  - commercial leader `pq_leader_scan_init(uint, void*&, uint)` still remains
    separate from the typed leader path and is not opened here；
  - direct full-object construction/destruction of `row_prebuilt_t` remains out
    of scope。
- review:
  - independent Review Agent accepted the batch with no Critical or Important
    issues；
  - reviewer confirmed the commercial worker next path remains fail-closed and
    no-arg worker cleanup releases `pq_ctx`、debug `pq_prev_ctx`、`pq_worker`、
    ref info、attach state、and fetch counters。

#### Batch D1.4 - commercial leader lifecycle bridge

Status: completed and committed

目标：

- 为 InnoDB 覆盖 commercial `pq_leader_scan_init(uint, void *&, uint)` 和
  `pq_leader_scan_end(void *)`；
- 将 commercial `void *` leader context 适配到当前 8.0.46 typed
  `PQ_Leader_context *` 生命周期；
- 只接 clustered full-scan leader init/end lifecycle，不启用
  `ha_pq_next()` row production。

Planned implementation:

- `storage/innobase/handler/ha_innodb.h`
  - declare commercial leader init/end overrides。
- `storage/innobase/handler/ha_innodb_pq.cc`
  - implement `pq_leader_scan_init(uint, void *&, uint)` as a thin adapter
    over typed `pq_leader_scan_init(ha_thd(), &ctx, EXECUTE, ...)`；
  - reject unsupported commercial state in this batch:
    reverse scan、ref scan、range scan、invalid prebuilt/DOP；
  - implement `pq_leader_scan_end(void *)` by casting back to
    `PQ_Leader_context *` and calling the typed cleanup path。

Validation:

- `git diff --check -- storage/innobase/handler/ha_innodb.h storage/innobase/handler/ha_innodb_pq.cc Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d14-mtr-vardir --tmpdir=/tmp/pq-d14-mtr-tmpdir`

Risk constraints:

- do not migrate commercial range/ref/partition leader dispatch in this batch；
- do not route optimizer/executor into `ha_pq_init()` yet；
- keep `pq_worker_scan_next(void*, uchar*)` and therefore `ha_pq_next()`
  fail-closed for real row reads；
- void leader cleanup must never fall through to the base no-op cleanup。

Completion Report - Batch D1.4:

- changed files:
  - `storage/innobase/handler/ha_innodb.h`
  - `storage/innobase/handler/ha_innodb_pq.cc`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added InnoDB override declarations for commercial leader init/end；
  - `pq_leader_scan_init(uint, void *&, uint)` now validates D1.4 scope,
    switches the requested active index, delegates to typed EXECUTE leader init,
    and stores the typed `PQ_Leader_context *` in the commercial `void *`
    output only on success；
  - `pq_leader_scan_end(void *)` delegates to the typed end path so thread
    budget、read view、worker contexts、and handler-owned leader state are
    released。
- validation:
  - `git diff --check -- storage/innobase/handler/ha_innodb.h storage/innobase/handler/ha_innodb_pq.cc Docs/pq_tasks/commercial-full-port-sprint.md`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    passed；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d14-mtr-vardir --tmpdir=/tmp/pq-d14-mtr-tmpdir`
    passed, all 89 tests successful。
- risks carried forward:
  - `ha_pq_init()` can now create a leader context for supported full-scan
    shapes, but `ha_pq_next()` remains unable to produce rows because
    commercial worker next is still unsupported；
  - commercial range/ref leader dispatch remains later work。
- review:
  - independent Review Agent pending; this commit is a validated safeguard
    commit to preserve current PQ work remotely。

#### Batch D1.5 - typed worker next minimal fullscan bridge design

Status: deferred after review reject

目标：

- 评估是否可以打开一个极窄的 InnoDB typed worker pull-row next 路径；
- 只支持当前 typed `PQ_Worker_context *` API 下的 clustered full table scan；
- 不打开 commercial `ha_pq_next()` / `void *scan_ctx` 正式执行路径；
- 不打开 range/ref/dependent-ref/reverse/secondary/ICP/MVI/partition。

Design review result:

- Commercial diff Agent 和 D1.5 Design Review Agent 结论均为 SPLIT：
  可以做 typed clustered fullscan worker next；不能一次性打开商用全量
  `ha_pq_next()` 语义。
- 原因：
  - 当前 SQL worker iterator 已经调用 typed
    `pq_worker_scan_next(PQ_Worker_context *, ...)`；
  - 当前 commercial `void *` next 没有真实 SQL caller；
  - 当前 D1.4 `void *` leader bridge 背后是 typed
    `PQ_Leader_context`，不是商用 `Parallel_leader` + commercial slice map；
  - callback producer 与 latent `row_search_mvcc()` pull producer 仍需保持清晰
    边界，不能同时大范围打开两套不一致路径。

Planned implementation:

- `storage/innobase/handler/ha_innodb_pq.cc`
  - evaluated typed `pq_worker_scan_next(PQ_Worker_context *, uchar *, bool *)`
    delegating to `InnoDB_pq_worker_ctx::read_record()`；
  - gate this path to:
    clustered fullscan、single full-range、active leader read view、
    no reverse、no ref/range/dependent-ref、no ICP、no BLOB table、
    worker-owned table record buffer；
  - keep commercial `pq_worker_scan_next(void *, uchar *)` fail-closed with
    `HA_ERR_UNSUPPORTED`。

Validation:

- `git diff --check -- storage/innobase/handler/ha_innodb_pq.cc Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_commercial_fullscan parallel_query.pq_parallel_scan_iterator_row_values parallel_query.pq_leader_row_stream_smoke parallel_query.pq_read_threaded_dop2_worker_error parallel_query.pq_read_threaded_dop2_external_kill --parallel=1 --vardir=/tmp/pq-d15-target-vardir --tmpdir=/tmp/pq-d15-target-tmpdir`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d15-mtr-vardir --tmpdir=/tmp/pq-d15-mtr-tmpdir`

Risk constraints:

- do not wire `PQblockScanIterator` back to commercial `ha_pq_next()` until
  handler `inited == PQ` state and commercial caller contract are migrated；
- do not treat unsupported/error as EOF；
- do not expose multi-range pull-row behavior until range boundaries have
  explicit no-duplicate/no-gap tests；
- do not use a row cache wrapper around callback producer without memory、
  kill、error、and early-exit limits。

Completion Report - Batch D1.5:

- changed files:
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - attempted implementation was reviewed and rejected before commit；
  - source change was reverted so typed and commercial worker next remain
    fail-closed；
  - commercial `pq_worker_scan_next(void *, uchar *)` remains unsupported。
- validation:
  - `git diff --check -- storage/innobase/handler/ha_innodb_pq.cc` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `./build-ninja/runtime_output_directory/mysqld --no-defaults --verbose --help`
    passed；
  - targeted MTR command above passed, all 6 tests successful。
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d15-mtr-vardir --tmpdir=/tmp/pq-d15-mtr-tmpdir`
    passed, all 89 tests successful。
- review:
  - independent Code Review Agent rejected enabling the typed pull path；
  - Critical: the latent `row_search_mvcc()` path reads through the worker
    `row_prebuilt_t` / worker transaction and does not prove leader snapshot
    visibility；
  - Important: typed `PQRefIterator` also calls
    `pq_worker_scan_next(PQ_Worker_context *, ...)`, so commercial
    `pq_ref/pq_range_type` fields are not a sufficient access-shape gate；
  - Important: direct `row_search_mvcc()` bypasses normal InnoDB handler
    concurrency and abort wrappers。
- risks carried forward:
  - D1.5 row production remains deferred；
  - commercial `ha_pq_next()` remains a future batch；
  - next implementation must either use the existing controlled
    callback/Parallel_reader row producer as canonical, or prove worker
    `row_search_mvcc()` uses leader snapshot and cannot be reached from ref/range
    typed iterators。

#### Batch D1.6a - callback row producer EOF contract

Status: completed and committed

目标：

- 将现有 callback/Parallel_reader row producer 的内部 EOF contract 从 smoke
  假设收敛为可测试的 producer contract；
- 仍只在显式 debug/experimental gate 下运行，不打开默认用户可见 PQ；
- 保持 typed/commercial `pq_worker_scan_next()` fail-closed；
- 为后续 D1.6b 用户可见 iterator 接入准备 empty/natural EOF 验证。

Design review result:

- D1.6 row producer Agent 和并发/快照 Review Agent 均建议 split：
  - D1.6a：先 productionize callback row producer 的内部 contract，
    仍 behind explicit gate；
  - D1.6b：再接入真实用户可见 iterator/eligibility。
- Blockers carried before D1.6b:
  - leader snapshot / worker materialization contract needs explicit tests；
  - kill/early-exit and worker error priority are still not production-grade；
  - SQL worker thread budget and InnoDB Parallel_reader budget are still mixed；
  - empty/EOF semantics must be unified before user-visible execution。

Planned implementation:

- `sql/parallel_query/sql_parallel.cc`
  - allow `run_worker_callback_limited_producer()` to finish successfully with
    zero rows；
  - keep FINISH as the only EOF signal。
- `sql/parallel_query/pq_iterator.cc`
  - update guarded `pq_leader_row_stream_smoke` to accept 0..row_limit rows
    instead of exactly two rows。
- `mysql-test/suite/parallel_query/t|r/pq_leader_row_stream_empty`
  - add empty-table EOF coverage under the existing debug gate。

Validation:

- `git diff --check -- sql/parallel_query/sql_parallel.cc sql/parallel_query/pq_iterator.cc mysql-test/suite/parallel_query/t/pq_leader_row_stream_empty.test mysql-test/suite/parallel_query/r/pq_leader_row_stream_empty.result Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_leader_row_stream_empty parallel_query.pq_leader_row_stream_smoke --parallel=1 --vardir=/tmp/pq-d16a-target-vardir --tmpdir=/tmp/pq-d16a-target-tmpdir`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d16a-mtr-vardir --tmpdir=/tmp/pq-d16a-mtr-tmpdir`

Risk constraints:

- no default user-visible PQ enablement in this batch；
- do not change optimizer eligibility；
- do not change `PQblockScanIterator` / `PQRefIterator` next behavior；
- do not claim snapshot、kill、or worker error priority are complete。

Completion Report - Batch D1.6a:

- changed files:
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/pq_iterator.cc`
  - `mysql-test/suite/parallel_query/t/pq_leader_row_stream_empty.test`
  - `mysql-test/suite/parallel_query/r/pq_leader_row_stream_empty.result`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - `run_worker_callback_limited_producer()` no longer treats zero produced
    rows as failure；
  - `pq_leader_row_stream_smoke` accepts natural EOF with 0..row_limit rows；
  - added empty-table debug-gated row stream test。
- validation:
  - `git diff --check -- sql/parallel_query/sql_parallel.cc sql/parallel_query/pq_iterator.cc mysql-test/suite/parallel_query/t/pq_leader_row_stream_empty.test mysql-test/suite/parallel_query/r/pq_leader_row_stream_empty.result Docs/pq_tasks/commercial-full-port-sprint.md`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_leader_row_stream_empty parallel_query.pq_leader_row_stream_smoke --parallel=1 --vardir=/tmp/pq-d16a-target-vardir --tmpdir=/tmp/pq-d16a-target-tmpdir`
    passed, all 3 tests successful；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d16a-mtr-vardir --tmpdir=/tmp/pq-d16a-mtr-tmpdir`
    passed, all 90 tests successful。
- review:
  - independent Code Review Agent accepted the batch；
  - reviewer confirmed D1.6a remains debug/experimental gated and does not
    open default user-visible PQ；
  - reviewer confirmed zero-row EOF is signaled by FINISH, while
    unsupported/error paths remain failures；
  - reviewer noted the new MTR files must be explicitly included in the commit。

#### Batch D1.6b-pre - row-stream worker error priority contract

Status: completed and committed

目标：

- 将 debug-gated leader row stream ERROR token 与 `Gather_operator`
  worker error priority 解析打通；
- 在 iterator 收到 MQ ERROR 时优先使用 `resolve_error_priority()` 的
  worker fatal error code，而不是裸 `HA_ERR_INTERNAL_ERROR`；
- 为后续 D1.6b 用户可见 iterator 接入准备 worker error propagation
  contract；
- 仍不打开默认用户可见 PQ，也不恢复 typed `pq_worker_scan_next()`。

Planned implementation:

- `sql/parallel_query/sql_parallel.cc`
  - `prepare_leader_row_stream_error_smoke()` 在发送 ERROR token 前后标记
    worker `RUNNING -> ERROR`，并设置 `m_error_code`；
  - 新增 debug-only `pq_leader_row_stream_error_smoke_out_of_mem` 注入，
    用可区分的 `HA_ERR_OUT_OF_MEM` 验证 priority bridge；
- `sql/parallel_query/pq_iterator.cc`
  - `PQTableScanIterator::Read()` 收到 `Materialize_status::ERROR` 后调用
    `resolve_error_priority()`；
  - 将解析出的 error code 写入 `THD::pq_error` 并用于 `PrintError()`；
- `mysql-test/suite/parallel_query/t|r/pq_leader_row_stream_error_priority`
  - 验证最终 SQL 错误使用 worker priority 解析出的错误码，而不是固定
    `HA_ERR_INTERNAL_ERROR`；
- 沿用现有 `pq_leader_row_stream_error_smoke` MTR 验证 error/cleanup 计数和
  无 serial fallback 语义。

Validation:

- `git diff --check -- sql/parallel_query/sql_parallel.cc sql/parallel_query/pq_iterator.cc mysql-test/suite/parallel_query/t/pq_leader_row_stream_error_priority.test mysql-test/suite/parallel_query/r/pq_leader_row_stream_error_priority.result Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_leader_row_stream_error_priority parallel_query.pq_leader_row_stream_error_smoke parallel_query.pq_leader_row_stream_empty parallel_query.pq_leader_row_stream_smoke --parallel=1 --vardir=/tmp/pq-d16bpre-target-vardir --tmpdir=/tmp/pq-d16bpre-target-tmpdir`

Risk constraints:

- no optimizer eligibility change；
- no default user-visible PQ enablement；
- no typed worker pull-row path enablement；
- kill/early-exit and thread-budget convergence remain separate D1.6b
  blockers。

Completion Report - Batch D1.6b-pre:

- changed files:
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/pq_iterator.cc`
  - `mysql-test/suite/parallel_query/t/pq_leader_row_stream_error_priority.test`
  - `mysql-test/suite/parallel_query/r/pq_leader_row_stream_error_priority.result`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - debug-gated row stream ERROR producer now marks worker 0
    `RUNNING -> ERROR` and records `m_error_code` before enqueuing the ERROR
    token；
  - `PQTableScanIterator::Read()` resolves the worker error priority before
    cleanup and prints the resolved handler error code；
  - added a distinguishable `HA_ERR_OUT_OF_MEM` debug injection to prove the
    iterator no longer always prints fixed `HA_ERR_INTERNAL_ERROR`。
- validation:
  - `git diff --check -- sql/parallel_query/sql_parallel.cc sql/parallel_query/pq_iterator.cc mysql-test/suite/parallel_query/t/pq_leader_row_stream_error_priority.test mysql-test/suite/parallel_query/r/pq_leader_row_stream_error_priority.result Docs/pq_tasks/commercial-full-port-sprint.md`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - first full-suite run before the distinguishable-code review fix passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pq-d16bpre-full-vardir --tmpdir=/tmp/pq-d16bpre-full-tmpdir`,
    all 90 tests successful；
  - after adding the distinguishable-code test:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_leader_row_stream_error_priority parallel_query.pq_leader_row_stream_error_smoke parallel_query.pq_leader_row_stream_empty parallel_query.pq_leader_row_stream_smoke --parallel=1 --vardir=/tmp/pq-d16bpre-target-vardir --tmpdir=/tmp/pq-d16bpre-target-tmpdir`
    passed, all 5 tests successful。
- review:
  - first independent Review Agent rejected the batch only because existing
    tests could not distinguish the resolved worker error code from the old
    fixed internal error；
  - the distinguishable-code MTR was added to address that Important finding；
  - follow-up Review Agent was retried with a reduced prompt but failed due
    intermittent subagent connection failures；the original Important finding
    is covered by the new distinguishable-code MTR。

#### Batch D1.6b-kill - row-stream leader kill priority contract

Status: completed and committed

目标：

- 为 debug-gated leader row stream Read 路径补齐 kill priority smoke；
- 验证 leader kill 在 materialize rows 前生效；
- 确认 kill 场景不会计入 `Parallel_queries_executed` 或
  `Parallel_leader_row_stream_smoke_rows`；
- 仍不打开默认用户可见 PQ，也不恢复 typed worker pull-row。

Planned implementation:

- `sql/parallel_query/pq_iterator.cc`
  - 在 PQ row-stream `Read()` 循环中加入 debug-only
    `pq_leader_row_stream_force_kill`，设置 `THD::KILL_QUERY` 后走现有
    `check_leader_kill()` / `propagate_kill_to_workers()`；
- `mysql-test/suite/parallel_query/t|r/pq_leader_row_stream_kill_priority`
  - 使用 `pq_leader_row_stream_smoke + pq_leader_row_stream_force_kill`；
  - 断言 SQL 返回 `ER_QUERY_INTERRUPTED`；
  - 断言 stream attempt/selected 增加，但 row/executed 计数不增加。

Validation:

- `git diff --check -- sql/parallel_query/pq_iterator.cc mysql-test/suite/parallel_query/t/pq_leader_row_stream_kill_priority.test mysql-test/suite/parallel_query/r/pq_leader_row_stream_kill_priority.result Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_leader_row_stream_kill_priority parallel_query.pq_leader_row_stream_error_priority parallel_query.pq_leader_row_stream_error_smoke parallel_query.pq_leader_row_stream_empty parallel_query.pq_leader_row_stream_smoke --parallel=1 --vardir=/tmp/pq-d16bkill-target-vardir --tmpdir=/tmp/pq-d16bkill-target-tmpdir`

Risk constraints:

- no optimizer eligibility change；
- no default user-visible PQ enablement；
- no worker THD async KILL propagation change；
- external KILL for threaded/groupby paths remains covered by existing tests。

Completion Report - Batch D1.6b-kill:

- changed files:
  - `sql/parallel_query/pq_iterator.cc`
  - `mysql-test/suite/parallel_query/t/pq_leader_row_stream_kill_priority.test`
  - `mysql-test/suite/parallel_query/r/pq_leader_row_stream_kill_priority.result`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added debug-only `pq_leader_row_stream_force_kill` inside the PQ row-stream
    `Read()` loop；
  - the flag sets `THD::KILL_QUERY` before the existing
    `check_leader_kill()` branch, so normal kill propagation and cleanup are
    used；
  - added MTR coverage proving row-stream selection happens but no row or
    executed-query counters are incremented after kill。
- validation:
  - `git diff --check -- sql/parallel_query/pq_iterator.cc mysql-test/suite/parallel_query/t/pq_leader_row_stream_kill_priority.test mysql-test/suite/parallel_query/r/pq_leader_row_stream_kill_priority.result Docs/pq_tasks/commercial-full-port-sprint.md`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_leader_row_stream_kill_priority parallel_query.pq_leader_row_stream_error_priority parallel_query.pq_leader_row_stream_error_smoke parallel_query.pq_leader_row_stream_empty parallel_query.pq_leader_row_stream_smoke --parallel=1 --vardir=/tmp/pq-d16bkill-target-vardir --tmpdir=/tmp/pq-d16bkill-target-tmpdir`
    passed, all 6 tests successful。
- review:
  - independent Review Agent accepted the batch；
  - reviewer confirmed the forced kill is debug-gated before row
    materialization；
  - reviewer confirmed the MTR verifies selected stream without row/executed
    counter increments and preserves `ER_QUERY_INTERRUPTED`。

#### Batch D1.6b-budget - threaded row-stream thread budget contract

Status: completed and committed

目标：

- 为 threaded callback row producer 接入 PQ thread budget acquire/release
  contract；
- 默认 `parallel_max_threads=0` 仍表示未配置预算，不改变现有 debug-gated
  threaded tests；
- 预算拒绝时不启动 worker，不增加 `Parallel_workers_launched`，不泄漏
  `PQ_threads_running`；
- 为后续用户可见 threaded iterator 接入准备资源治理语义。

Planned implementation:

- `sql/parallel_query/sql_parallel.h`
  - `Gather_operator` 增加 `m_thread_budget_acquired`；
- `sql/parallel_query/sql_parallel.cc`
  - `run_worker_callback_threaded_producer()` 在启动 worker 前按
    `parallel_max_threads > 0` 获取预算；
  - debug-only `pq_read_threaded_force_thread_budget_refuse` 强制执行拒绝路径；
  - `start_workers()` 失败和 `destroy()` 释放已获取预算；
- `mysql-test/suite/parallel_query/t|r/pq_read_threaded_thread_budget_refuse`
  - 验证预算拒绝时 `PQ_threads_refused` 增加、`PQ_threads_running` 和
    `Parallel_workers_launched` 不增加。

Validation:

- `git diff --check -- sql/parallel_query/sql_parallel.cc sql/parallel_query/sql_parallel.h mysql-test/suite/parallel_query/t/pq_read_threaded_thread_budget_refuse.test mysql-test/suite/parallel_query/r/pq_read_threaded_thread_budget_refuse.result Docs/pq_tasks/commercial-full-port-sprint.md`
- `cmake --build build-ninja --target mysqld -j 16`
- `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_read_threaded_thread_budget_refuse parallel_query.pq_read_threaded_shadow_dop1 parallel_query.pq_read_threaded_worker_error --parallel=1 --vardir=/tmp/pq-d16bbudget-target-vardir --tmpdir=/tmp/pq-d16bbudget-target-tmpdir`

Risk constraints:

- no default user-visible PQ enablement；
- no global/sysvar contract change for `parallel_max_threads` in this batch；
- no worker THD scheduling policy change beyond guarded budget acquire/release。

Completion Report - Batch D1.6b-budget:

- changed files:
  - `sql/parallel_query/sql_parallel.h`
  - `sql/parallel_query/sql_parallel.cc`
  - `mysql-test/suite/parallel_query/t/pq_read_threaded_thread_budget_refuse.test`
  - `mysql-test/suite/parallel_query/r/pq_read_threaded_thread_budget_refuse.result`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - `Gather_operator` now tracks acquired thread budget；
  - threaded callback producer acquires budget before worker launch when
    `parallel_max_threads > 0`；
  - default `parallel_max_threads=0` keeps existing tests and behavior
    unchanged；
  - start failure and destroy both release acquired budget；
  - debug-only refusal flag verifies the negative path without changing sysvar
    surface。
- validation:
  - `git diff --check -- sql/parallel_query/sql_parallel.cc sql/parallel_query/sql_parallel.h mysql-test/suite/parallel_query/t/pq_read_threaded_thread_budget_refuse.test mysql-test/suite/parallel_query/r/pq_read_threaded_thread_budget_refuse.result Docs/pq_tasks/commercial-full-port-sprint.md`
    passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr parallel_query.pq_read_threaded_thread_budget_refuse parallel_query.pq_read_threaded_shadow_dop1 parallel_query.pq_read_threaded_worker_error --parallel=1 --vardir=/tmp/pq-d16bbudget-target-vardir --tmpdir=/tmp/pq-d16bbudget-target-tmpdir`
    passed, all 4 tests successful。
- review:
  - independent Review Agent accepted the batch；
  - reviewer confirmed budget is acquired only after successful
    `check_pq_running_threads()` and released on start failure / destroy；
  - reviewer confirmed default `parallel_max_threads=0` preserves existing
    threaded behavior；
  - reviewer confirmed the MTR covers refused/running/workers deltas。

#### Batch D1.6c-root - worker root iterator result-bound read smoke

Status: completed and committed

目标：

- 将 worker execute smoke 从 direct `PQblockScanIterator` Read 推进到
  factory-created worker root iterator `Init()/Read()`；
- 将 worker-owned `Query_result_mq` 绑定前移到 root iterator `Read()` 之前；
- 仍不调用 `ExecuteIteratorQuery()`，不发送 `Query_result_mq` data frame，
  不改变默认用户可见 PQ gate。

Completion Report - Batch D1.6c-root:

- changed files:
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/sql_parallel.h`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
  - `Docs/pq_tasks/commercial-port-next-query-expression-root-iterator.md`
- implementation:
  - `worker_join->root_access_path()` now temporarily points to the
    worker-owned `PQ_BLOCK_SCAN` access path；
  - `CreateIteratorFromAccessPath()` builds the root iterator locally, runs
    `Init()` and one `Read()`，then destroys the iterator before QEP_TAB/TABLE
    detach；
  - old direct hand-written `PQblockScanIterator` Init/Read path is no longer
    run in the smoke, avoiding double consumption of one worker scan context；
  - `worker_plan.bind_result()` now happens before root iterator `Read()`；
  - new status
    `Parallel_worker_execute_iterator_smoke_result_bound_before_read` proves
    worker query expression / query block result was bound before the root
    read path；
  - after root `Read()` succeeds, the smoke creates a restricted worker-owned
    `Item_field` list from `worker->m_open_ctx.worker_table->field[]` and sends
    one ROW plus one FINISH frame through the bound `Query_result_mq`；
  - the smoke immediately decodes the worker `MQueue_handle` ROW/FINISH frames
    and exposes
    `Parallel_worker_execute_iterator_smoke_result_row_sent` /
    `Parallel_worker_execute_iterator_smoke_result_eof_sent`。
- validation:
  - `git diff --check && cmake --build build-ninja --target mysqld -j 16`
    passed；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query
    pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats
    --parallel=1 --vardir=/tmp/pq-worker-result-bound-read-vardir
    --tmpdir=/tmp/pq-worker-result-bound-read-tmpdir` passed, all 4 tests
    successful。
  - later targeted validation:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query
    pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats
    pq_commercial_worker_result_adapter --parallel=1
    --vardir=/tmp/pq-worker-result-send-vardir2
    --tmpdir=/tmp/pq-worker-result-send-tmpdir2` passed, all 5 tests
    successful。
- review:
  - independent Review Agent accepted root iterator Read commit；
  - independent Review Agent accepted result-bound-before-root-read commit；
  - independent Review Agent accepted worker-owned MQ ROW/FINISH smoke；
  - no Critical or Important findings remained。
- commits:
  - `adbf2c51fe5 Add PQ worker root iterator read smoke`
  - `1b35cc9802d Bind PQ worker result before root read`
  - `c660519d854 Send PQ worker root read result smoke`

#### Batch D1.6d - worker Query_block output Item_field smoke

Status: completed locally；Code/Docs/Test Review accepted。

目标：

- 将 D1.6c 的局部临时 `Item_field` result adapter 推进到 worker
  `Query_block::fields` / `base_ref_items`；
- 只支持单表、纯 visible base-table `Item_field` SELECT list；
- 仍不搬完整 `Item::pq_clone()` / `refix_fields()`，不触碰
  QueryExpression root ownership，也不调用 `ExecuteIteratorQuery()`。

Completion Report - Batch D1.6d:

- changed files:
  - `sql/parallel_query/pq_clone.h`
  - `sql/parallel_query/pq_clone.cc`
  - `sql/parallel_query/sql_parallel.h`
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
- implementation:
  - added `pq_clone_worker_base_table_fields_smoke()`；
  - the helper first validates the full SELECT list, then populates worker
    `Query_block::fields` and `base_ref_items` with worker-table-owned
    `Item_field` objects；
  - the helper rejects empty output, hidden items, non-`FIELD_ITEM` expressions,
    leader-table mismatch, invalid field indexes, and worker-field table
    mismatch before mutating the worker query block；
  - `send_current_row_result()` now sends through `worker_join->fields` and
    verifies that pointer is exactly `&worker_join->query_block->fields`；
  - this avoids the optimized-only `Query_expression::get_field_list()` API
    while still proving the worker result path reads worker query-block output
    fields；
  - added status counters
    `Parallel_worker_execute_iterator_smoke_blocked_output_fields` and
    `Parallel_worker_execute_iterator_smoke_output_fields_cloned`。
- validation:
  - `git diff --check && cmake --build build-ninja --target mysqld -j 16`
    passed；
  - first MTR attempt exposed an assertion in
    `Query_expression::get_field_list()` because the worker shell is not
    optimized；the send path was corrected to use `worker_join->fields`；
  - final targeted validation passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query
    pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats
    pq_commercial_worker_result_adapter --parallel=1
    --vardir=/tmp/pq-worker-output-fields-vardir5
    --tmpdir=/tmp/pq-worker-output-fields-tmpdir5`，all 5 tests successful。
- review:
  - independent Code/Docs/Test Review accepted；
  - no Critical or Important findings；
  - minor follow-up: add a future negative smoke for expression/hidden SELECT
    output rejected by `blocked_output_fields`。

#### Batch D1.6e - QueryExpression-owned worker root iterator smoke

Status: completed locally；Code/Docs/Test Review accepted with minor notes。

目标：

- 将 worker root iterator 从 local `CreateIteratorFromAccessPath()` scope 推进到
  worker `Query_expression::m_root_access_path` /
  `Query_expression::m_root_iterator` ownership；
- 只允许 worker THD、simple query、空 QueryExpression root state、且
  `JOIN::root_access_path()` 为 `AccessPath::PQ_BLOCK_SCAN` 的 smoke 场景；
- 仍不调用 `force_create_iterators()`，不调用 `ExecuteIteratorQuery()`，不打开
  用户可见 PQ gate。

Completion Report - Batch D1.6e:

- changed files:
  - `sql/sql_lex.h`
  - `sql/sql_union.cc`
  - `sql/parallel_query/sql_parallel.h`
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
  - `Docs/pq_tasks/commercial-port-next-query-expression-root-iterator.md`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added `Query_expression::create_pq_worker_root_iterator_smoke()` as a
    guarded smoke-only bridge toward the commercial root iterator wrapper；
  - helper rejects non-worker THD, null `thd->lex`, non-simple query, join
    mismatch, pre-existing QueryExpression root state, null root path, and
    non-`PQ_BLOCK_SCAN` root path；
  - caller tracks `query_expression_root_owned` and calls
    `clear_root_access_path()` only when this smoke successfully created the
    QueryExpression-owned root；
  - cleanup order is root clear, JOIN root restore, worker QEP_TAB/TABLE detach,
    worker plan cleanup；
  - new status
    `Parallel_worker_execute_iterator_smoke_root_owned` proves the smoke used
    QueryExpression-owned root iterator state。
- validation:
  - `git diff --check && cmake --build build-ninja --target mysqld -j 16`
    passed；
  - `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query
    pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats
    pq_commercial_worker_result_adapter --parallel=1
    --vardir=/tmp/pq-worker-root-owned-vardir5
    --tmpdir=/tmp/pq-worker-root-owned-tmpdir5` passed, all 5 tests
    successful。
- review:
  - first independent review returned `NEEDS_FIX` for unconditional
    `clear_root_access_path()` and broad helper guards；
  - fixed by tracking ownership and adding worker/PQ_BLOCK_SCAN guards；
  - re-review returned `ACCEPT_WITH_MINOR`，no Critical or Important findings；
  - remaining minor: public smoke-only `Query_expression` API should be treated
    as temporary until replaced by the real commercial wrapper path。

Next blocker:

- `ExecuteIteratorQuery()` is still not called in worker smoke；
- next source batch should define and verify the smallest
  `ExecuteIteratorQuery()` preflight contract: worker result metadata/data/EOF
  ownership, `join_free()` cleanup ordering, and error/KILL propagation。

#### Batch D1.6f - ExecuteIteratorQuery preflight counters

Status: completed and committed as `b0b546579e6`；Code / Docs / Test Review
accepted。

目标：

- 在不调用 `ExecuteIteratorQuery()` 的前提下，记录 worker execute smoke 已满足
  哪些正式执行前置条件；
- 验证 THD/root/result/fields 已经 ready；
- 显式记录当前阻断点仍是 worker `Query_expression` unit 未达到
  `prepared + optimized + not executed` 合同；
- 不改 `sql/sql_union.cc` / `sql/sql_lex.h`，不打开用户可见 PQ gate。

Completion Report - Batch D1.6f:

- changed files:
  - `sql/parallel_query/sql_parallel.h`
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added `PQ_worker_execute_smoke_plan::preflight_execute_iterator_query()`；
  - preflight runs after QueryExpression-owned root iterator is created and
    before root `Init()/Read()` consumes the iterator；
  - preflight records `xpf_thd_ready` when worker THD has worker identity and
    worker info；
  - preflight records `xpf_root_ready` when QueryExpression root path/iterator
    exists and root path is `PQ_BLOCK_SCAN`；
  - preflight records `xpf_result_ready` when both worker
    `Query_expression` and `Query_block` point at the same `Query_result_mq`
    with the worker MQ handle；
  - preflight records `xpf_fields_ready` when `JOIN::fields` is exactly
    `Query_block::fields` and non-empty；
  - preflight records `xpf_blocked_unit` while `xpf_unit_ready` remains zero,
    documenting the current blocker before a safe `ExecuteIteratorQuery()`
    call can be attempted；
  - SHOW STATUS names use short `xpf_*` suffixes because longer
    `exec_preflight_*` names were not visible through
    `performance_schema.global_status`。
- validation:
  - `git diff --check && cmake --build build-ninja --target mysqld -j 16`
    passed；
  - first MTR attempt exposed overlong status variable names returning NULL；
    names were shortened to `xpf_*`；
  - final targeted validation passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr --suite=parallel_query
    pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats
    pq_commercial_worker_result_adapter --parallel=1
    --vardir=/tmp/pq-worker-exec-preflight-vardir3
    --tmpdir=/tmp/pq-worker-exec-preflight-tmpdir3`，all 5 tests successful。

Next blocker:

- worker `Query_expression` unit state is not optimized/prepared enough for
  `ExecuteIteratorQuery()` contracts；
- next source batch should decide whether to migrate the commercial
  `make_pq_worker_plan()` / `pq_make_join_readinfo()` unit optimization path or
  add a narrower smoke-only optimized unit transition before guarded execute。

#### Batch D1.6g - Worker unit execute-ready state

Status: completed locally；Code / Docs / Test Review accepted。

目标：

- 对齐商用 worker plan 的最小 unit 状态顺序：
  `Query_expression::set_prepared()` 后再设置 `JOIN::set_optimized()` 与
  `Query_expression::set_optimized()`；
- 仅在 DBUG/worker execute smoke 的 guarded worker-owned shell 中执行该状态
  transition；
- 保持 `ExecuteIteratorQuery()` 仍不调用，用户可见 PQ gate 不扩大；
- 在 worker plan cleanup 中对已标记 optimized 的 worker unit 做 full
  cleanup，避免 `Query_expression::destroy()` 的 optimized-but-not-cleaned
  断言风险。

Completion Report - Batch D1.6g:

- changed files:
  - `sql/parallel_query/sql_parallel.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `Docs/pq_tasks/commercial-full-port-sprint.md`
- implementation:
  - added `PQ_worker_execute_smoke_plan::prepare_execute_iterator_unit_state()`；
  - guards require worker THD/LEX, worker JOIN, worker query block, simple unit,
    matching `lex->unit`, matching `first_query_block()`, matching
    `Query_block::join`, not executed, and no unfinished materialization；
  - after QueryExpression-owned root iterator construction, smoke marks the
    worker unit prepared/optimized before `preflight_execute_iterator_query()`；
  - cleanup restores cloned query block ownership before full unit cleanup；
    when this smoke marked the unit execute-ready, `unit->cleanup(true)` owns
    worker JOIN cleanup/destruction；the old non-optimized fallback path still
    calls `JOIN::destroy()` directly；
  - MTR now expects `xpf_unit_ready` to grow and `xpf_blocked_unit` to stay
    zero while `blocked_execute` still grows。
- validation:
  - first targeted MTR exposed two cleanup-order defects:
    `unit->cleanup(true)` before `pq_restore()` crashed in
    `Query_block::pq_restore()`；calling `JOIN::destroy()` after
    `unit->cleanup(true)` double-destroyed the worker JOIN；
  - fixed by restoring clone ownership before full unit cleanup and by letting
    `unit->cleanup(true)` own worker JOIN destruction for the execute-ready
    path；
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr
    --suite=parallel_query pq_worker_execute_iterator_smoke
    pq_clone_diagnostics pq_stats pq_commercial_worker_result_adapter
    --parallel=1 --vardir=/tmp/pq-worker-unit-ready-vardir4
    --tmpdir=/tmp/pq-worker-unit-ready-tmpdir4`，all 5 tests successful。

Next blocker:

- `ExecuteIteratorQuery()` remains intentionally blocked after the preflight；
- the next source batch must either call `ExecuteIteratorQuery()` under a
  stricter smoke-only contract or migrate the commercial
  `make_pq_worker_plan()` / `pq_make_join_readinfo()` path far enough that
  result metadata/data/EOF and `join_free()` cleanup ordering are production
  equivalent。

#### Batch D1.6h - Query_result_mq execute-order contract

Status: completed locally；Code / Docs / Test Review accepted。

目标：

- 对齐 `ExecuteIteratorQuery()` 调用 `Query_result` 的最小顺序：
  `start_execution()` -> `send_result_set_metadata()` -> `send_data()` ->
  `send_eof()` -> `cleanup()`；
- 不调用 `ExecuteIteratorQuery()`，不修改 `sql/sql_union.cc`，不打开用户可见
  PQ gate；
- 让 worker execute smoke 可观测 result contract ready/blocker 状态，作为后续
  full-execute smoke 的前置。

Completion Report - Batch D1.6h:

- changed files:
  - `sql/parallel_query/query_result_mq.h`
  - `sql/parallel_query/query_result_mq.cc`
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/sql_parallel.h`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
- implementation:
  - added `Query_result_mq::start_execution()` and internal started /
    metadata-sent / finished state；
  - `send_result_set_metadata()` now records the field list size and is required
    before `send_data()`；
  - `send_data()` rejects calls before the result contract is ready or after
    EOF；
  - `send_eof()` requires metadata and marks the result finished after FINISH；
  - `cleanup()` resets the result contract state and is called explicitly from
    worker plan result restore；
  - existing local worker-result smokes were updated to use the same
    start/metadata/data/eof order；
  - added `Parallel_worker_execute_iterator_smoke_xrc_ready` and
    `Parallel_worker_execute_iterator_smoke_xrc_blocked` status variables；
  - `pq_worker_execute_iterator_smoke` asserts result contract ready grows,
    result contract blocked remains zero, `blocked_execute` still grows, and
    `success` remains zero。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr
    --suite=parallel_query pq_worker_execute_iterator_smoke
    pq_commercial_worker_result pq_commercial_worker_result_adapter pq_stats
    --parallel=1 --vardir=/tmp/pq-worker-result-contract-vardir4
    --tmpdir=/tmp/pq-worker-result-contract-tmpdir4`，all 5 tests successful。

Next blocker:

- full `ExecuteIteratorQuery()` smoke still needs an exclusive execution mode
  that does not first run manual `Init()/Read()` or
  `send_current_row_result()`；
- full execute also still needs MQ drain/backpressure and
  `join_free()`/worker table cleanup ordering reviewed before opening。

#### Batch D1.6i - ExecuteIteratorQuery direct-call blocker

Status: completed and committed as `ec8cc2dbe7b`；Code / Docs / Test Review
accepted。

目标：

- 不直接调用 full `ExecuteIteratorQuery()`；
- 在 worker root iterator / result / field / unit 合同均 ready 时，明确记录当前
  direct call 的真实阻断点：同线程 smoke 缺少并发 MQ consumer，不能保证
  `ExecuteIteratorQuery()` 连续发送所有 rows + EOF 时不阻塞；
- 保留现有单行 root iterator `Init()/Read()` + `Query_result_mq`
  ROW/FINISH smoke，用户可见 PQ gate 继续关闭。

Completion Report - Batch D1.6i:

- changed files:
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/sql_parallel.h`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
  - `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
- implementation:
  - factored worker result frame drain logic out of the single-row send smoke；
  - added `record_execute_iterator_query_blocker()` after root iterator
    ownership is proven；
  - added SHOW STATUS counters:
    `Parallel_worker_execute_iterator_smoke_xiq_called`,
    `Parallel_worker_execute_iterator_smoke_xiq_success`,
    `Parallel_worker_execute_iterator_smoke_xiq_rows`,
    `Parallel_worker_execute_iterator_smoke_xiq_finishes`,
    `Parallel_worker_execute_iterator_smoke_blocked_no_consumer`,
    `Parallel_worker_execute_iterator_smoke_blocked_drain`；
  - MTR asserts `xiq_called/success/rows/finishes` remain zero,
    `blocked_no_consumer` grows, and `blocked_drain` remains zero。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr
    --suite=parallel_query pq_worker_execute_iterator_smoke
    pq_commercial_worker_result pq_commercial_worker_result_adapter pq_stats
    --parallel=1 --vardir=/tmp/pq-worker-exec-blocker-vardir2
    --tmpdir=/tmp/pq-worker-exec-blocker-tmpdir2`，all 5 tests successful。

Next blocker:

- 真正调用 `ExecuteIteratorQuery()` 需要把 smoke 放入 real worker thread，或先实现
  leader 并发 drain；不能在同线程 producer-only 路径直接调用。

#### Batch D1.6j - Query_result_mq pre-wait leader drain

Status: completed and committed as `9476338d18e`；Code / Docs / Test Review
accepted。

目标：

- 先不把 `ExecuteIteratorQuery()` 放入 worker task；
- 将已有 threaded `Query_result_mq` probe 从“先 wait worker、后 drain MQ”
  调整为“worker 运行期间 leader 先 drain PQWR frames、再 wait worker”；
- 证明 leader 侧已经具备与商用 worker execute 路径一致的 pre-wait consumer
  时序，为后续 worker-thread `ExecuteIteratorQuery()` smoke 解除同线程
  no-consumer blocker。

Completion Report - Batch D1.6j:

- changed files:
  - `sql/parallel_query/sql_parallel.cc`
  - `sql/parallel_query/sql_parallel.h`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t/pq_commercial_worker_result.test`
  - `mysql-test/suite/parallel_query/r/pq_commercial_worker_result.result`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
- implementation:
  - `run_query_result_mq_threaded_probe_smoke()` now drains the worker
    `Query_result_mq` PQWR ROW/FINISH frames before `wait_for_workers()`；
  - added bounded receiver-latch wait on `MQ_WOULD_BLOCK`；
  - added `Parallel_worker_result_smoke_prewait_drains` status counter；
  - MTR asserts the pre-wait drain counter grows by one。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `cd build-ninja/mysql-test && TMPDIR=/tmp ./mtr
    --suite=parallel_query pq_commercial_worker_result
    pq_commercial_worker_result_adapter pq_worker_execute_iterator_smoke
    pq_stats --parallel=1 --vardir=/tmp/pq-prewait-drain-vardir
    --tmpdir=/tmp/pq-prewait-drain-tmpdir`，all 5 tests successful。

Next blocker:

- 新增 `PQ_worker_task::EXECUTE_ITERATOR_SMOKE`，在线程内复用/拆分
  `PQ_worker_execute_smoke_plan`，由 leader 使用同一 PQWR pre-wait drain
  解析 ROW/FINISH/ERROR，成功后再递增 `xiq_*`。

#### Batch D1.6k - Worker-thread ExecuteIteratorQuery positive smoke

Status: completed and committed.

目标：

- 在 DBUG-only worker thread 中运行 worker plan precheck；
- 先以 precheck task 证明 worker thread 可以构造 worker JOIN/root/result/unit
  合同且不调用 `ExecuteIteratorQuery()`；
- 随后在独立 call smoke 中真实调用
  `Query_expression::ExecuteIteratorQuery(worker_thd)`；
- leader 在线程 wait 前 drain `PQWR` ROW/FINISH，避免同线程 no-consumer
  blocker；
- 最后校验 decoded worker output values，而不把 `PQWR` 接到用户 SQL 结果。

Completion Report - Batch D1.6k:

- commits:
  - `a995a880d50` Add PQ worker execute threaded precheck；
  - `e5355e7a8a1` Run PQ worker execute iterator in smoke；
  - `c2946599f21` Validate PQ worker execute output values。
- changed areas:
  - `sql/parallel_query/sql_parallel.*`
  - `sql/parallel_query/pq_iterator.cc`
  - `sql/parallel_query/pq_iterators.*`
  - `sql/parallel_query/pq_clone.cc`
  - `sql/mysqld.cc`
  - `mysql-test/suite/parallel_query/t|r/pq_worker_execute_threaded_*`
  - `mysql-test/suite/parallel_query/r/pq_stats.result`
  - `Docs/pq_tasks/commercial-port-next-worker-execute-iterator.md`
- implementation:
  - added worker-thread `EXECUTE_ITERATOR_SMOKE` precheck task；
  - added `EXECUTE_ITERATOR_CALL_SMOKE` task that calls
    `ExecuteIteratorQuery(worker_thd)` with worker-owned
    `Query_result_mq`；
  - added pre-wait leader `PQWR` drain for real worker `ROW` / `FINISH`
    frames；
  - added decoded numeric checksum counters for worker output values:
    `value_rows`、`value_errors`、`value_id_sum`、`value_v_sum`、
    `value_id_v_sum`；
  - fixed worker `PQblockScanIterator` handler lifecycle by pairing
    `ha_rnd_init(true)` with handler-state-guarded `ha_rnd_end()`；
  - reapplied worker table column bitmaps after worker select-list read bits
    are set, so `Query_result_mq::send_data()` reads real worker record values。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_worker_execute_threaded_call_smoke`
    `pq_worker_execute_threaded_precheck_smoke`
    `pq_worker_execute_iterator_smoke`
    `pq_worker_typed_pull_next_smoke`
    `pq_parallel_scan_iterator_row_values`
    `pq_parallel_scan_iterator_order_gather_smoke`
    `pq_worker_attach_contract_smoke`
    `pq_commercial_worker_result`
    `pq_commercial_worker_result_adapter`
    `pq_stats`。
- review:
  - independent Review Agent accepted；
  - low-risk unchecked checksum multiplication finding was fixed before
    commit；
  - no Critical or Important findings remained。

Next blocker:

- worker `ExecuteIteratorQuery()` positive smoke is now proven only for
  DBUG-gated single-table `SELECT id, v` with `PQWR` leader drain；
- default visible PQ path still consumes typed row-image `Exchange_nosort`,
  not worker `PQWR` result frames；
- next source batch should choose one of two serial paths:
  1. connect worker `PQWR` decoded rows to leader record materialization under
     a debug-only `ParallelScanIterator` path；or
  2. move from debug-only worker execute smoke toward a guarded visible
     single-table fullscan gate that still excludes ORDER BY/ref/ICP。

### Batch E1 - Commercial MTR migration

Status: started

目标：

- 尽量全量迁入商用 `parallel_query` 测试套；
- include/opt 随对应 `.test` 一起裁剪迁移，不单独复制未启用的
  include/opt；
- 只有 dstore/audit plugin/replica topology/TPCH data/hash-spill/restart-debug
  等本地不可构造或大风险项允许暂时 skip。

Current E1 baseline:

- detailed taskbook: [commercial-port-e1-test-migration.md](commercial-port-e1-test-migration.md)；
- current branch has 92 `parallel_query/t/*.test`；the latest full MTR reports
  93/93 including `shutdown_report`；
- commercial branch has 98 `parallel_query/t/*.test`；
- same-name overlap is only `pq_not_support`；
- do not bulk-copy all commercial tests into the active suite；migrate by
  adapted scenario batches so full suite stays green。

Next action:

- E1-A1 fullscan edge commercial-name mapping；
- then E1-A2 kill / worker-error mapping；
- then E1-B1 ORDER / prepare / correlated-subquery deferred boundary。

#### Batch D1.6l - PQWR leader materialization smoke

Status: completed, reviewed, committed, and pushed.

目标：

- 在 worker-thread `ExecuteIteratorQuery()` positive smoke 基础上，验证 leader
  能消费真实 worker `PQWR` ROW frame；
- 将 decoded row 临时 materialize 到 leader `TABLE::record[0]`；
- 使用 checksum counters 证明 materialized row values 与 worker 输出一致；
- 保持 debug-only，不打开默认 visible PQ path。

Implementation:

- added debug helper `pq_materialize_worker_result_smoke_row()`；
- helper saves/restores leader `record[0]`；
- helper temporarily enables/restores `write_set` to satisfy debug
  `Field::store()` bitmap checks；
- extended `pq_drain_worker_result_frames()` with optional
  `materialize_table`；
- added `mat_*` status counters and reset coverage；
- updated `pq_worker_execute_threaded_call_smoke` and `pq_stats` expected
  results。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_worker_execute_threaded_call_smoke`
  `pq_worker_execute_threaded_precheck_smoke`
  `pq_worker_execute_iterator_smoke`
  `pq_commercial_worker_result`
  `pq_commercial_worker_result_adapter`
  `pq_stats`。

Review:

- independent Review Agent accepted；
- only Minor finding was that the helper is not a general materializer；
- code comment now explicitly limits it to the current debug-only
  `SELECT id, v` worker smoke。

Commit:

- `e080e9065da` Add PQWR leader materialization smoke。

Notes:

- initial `--record` run exposed a debug assertion in `Field::store()` because
  the target fields were not in `write_set`；
- fixed by reusing existing `dbug_tmp_use_all_columns()` /
  `dbug_tmp_restore_column_map()` around the smoke-only store path；
- full `parallel_query` suite intentionally not run during development per
  current constraint。

#### Batch D1.6m - PQWR through Exchange_nosort adapter

Status: completed, reviewed, committed, and pushed.

目标：

- 将 worker `Query_result_mq` 的 `PQWR` frame 消费点从
  `Gather_operator` 临时 drain helper 移入 `Exchange_nosort`；
- 贴近商用 `MQ_record_gather -> Exchange_nosort -> table->record[0]`
  leader row collection 形状；
- 保持 debug-only，不改变默认 visible PQ path。

Implementation:

- added `Exchange_nosort::materialize_next_worker_result_status()`；
- `Exchange_nosort` now has a narrow PQWR materializer for current
  `SELECT id, v` worker smoke；
- added `run_synthetic_worker_result_smoke()` to cover a two-queue
  FINISH/ROW/FINISH sequence and read-done queue skipping；
- existing typed record-image `materialize_next_record_image_status()` remains
  unchanged；
- threaded worker `ExecuteIteratorQuery()` smoke now drains through
  `get_exchange()` instead of directly receiving from `worker->m_mq_handle`；
- `sql_parallel.cc` keeps lifecycle/statistics responsibility only。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_worker_execute_threaded_call_smoke`
  `pq_worker_execute_threaded_precheck_smoke`
  `pq_worker_execute_iterator_smoke`
  `pq_commercial_worker_result`
  `pq_commercial_worker_result_adapter`
  `pq_stats`。

Review:

- independent Review Agent found one Important issue: the new public Exchange
  PQWR adapter did not skip queues already marked read-done, which could break
  future multi-queue use；
- fixed by checking `MQueue_handle::has_readdone()` before receive；
- added `pq_exchange_worker_result_smoke` coverage in
  `pq_commercial_worker_result_adapter` to prove queue0 FINISH followed by
  queue1 ROW/FINISH still drains correctly；
- final Review Agent accepted with no Critical or Important findings；
- the only Minor coverage note was fixed by making the smoke explicitly call
  the adapter once after queue0 FINISH and before queue1 ROW/FINISH is sent,
  so the next call exercises the read-done skip branch。

Notes:

- commit: `ad95b12cbd8` Route PQWR worker results through Exchange smoke；
- full `parallel_query` suite intentionally not run during development per
  current constraint。

#### Batch D1.6n - Minimal MQ_record_gather facade

Status: completed, reviewed, committed, and pushed.

目标：

- 引入商用同名 `MQ_record_gather` 的最小 debug-only facade；
- facade 非拥有地绑定当前 `Gather_operator` 持有的 `Exchange_nosort`；
- 通过 `mq_scan_next_worker_result()` 消费 `Query_result_mq` / `PQWR` row；
- 不迁移 `Filesort`、`Exchange_sort`、`QEP_TAB::split_table()`、
  `Field_raw_data` 全协议或 visible 执行路径。

Implementation:

- added `MQ_record_gather` declaration/implementation in
  `sql/parallel_query/sql_parallel.*`；
- `mq_scan_init(Gather_operator*)` only validates initialized gather and stores
  a non-owning exchange pointer；
- `mq_scan_end()` only clears the local pointer, leaving Exchange ownership to
  `Gather_operator::destroy()`；
- `pq_exchange_worker_result_smoke` now writes with `Query_result_mq` but reads
  through `MQ_record_gather::mq_scan_next_worker_result()`；
- the smoke explicitly covers read-done queue skipping by consuming queue0
  FINISH before sending queue1 ROW/FINISH。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_commercial_worker_result_adapter`
  `pq_worker_execute_threaded_call_smoke`
  `pq_stats`。

Notes:

- review: independent Review Agent accepted with no Critical or Important
  findings；
- commit: `4c54338aadb` Add minimal MQ record gather facade；
- full `parallel_query` suite intentionally not run during development per
  current constraint。

#### Batch D1.6o - PQTableScanIterator PQWR record_gather gate

Status: completed, reviewed, committed, and pushed.

目标：

- 在 `PQTableScanIterator` 真实 `TABLE_SCAN` 入口下增加一个 debug-only
  controlled gate；
- 验证 `PQTableScanIterator::Read()` 可以通过
  `MQ_record_gather -> Exchange_nosort -> table->record[0]` 消费
  `Query_result_mq` 产生的 `PQWR` worker-result row；
- 不扩大普通 `parallel_query=ON` 默认路径，不改 optimizer eligibility、
  worker clone、InnoDB range/ref/ICP 或 ORDER BY 路径。

Implementation:

- `PQTableScanIterator` 新增 `m_record_gather` 和
  `m_use_worker_result_record_gather`；
- 新 DBUG gate `pq_leader_pqwr_record_gather_smoke` 在 `Init()` 中：
  `pq_leader_scan_init(EXECUTE)` -> `Gather_operator::init()` ->
  `MQ_record_gather::mq_scan_init()` -> `Query_result_mq` 写入一行
  `PQWR` row + FINISH；
- `Read()` 在该 gate 下调用
  `MQ_record_gather::mq_scan_next_worker_result()`，并复用原有
  ROW/EOF/WOULD_BLOCK/KILL/cleanup 处理；
- `cleanup_pq_resources()` 先释放 facade，再销毁 `Gather_operator`，
  避免 facade 持有已销毁 Exchange 指针；
- 新增 MTR `pq_leader_pqwr_record_gather_smoke` 覆盖 visible SELECT
  返回 `7,42`、executed/rows/fallback 计数。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 8` passed；
- targeted MTR passed:
  `pq_leader_pqwr_record_gather_smoke`
  `pq_leader_row_stream_row_values`
  `pq_commercial_worker_result_adapter`。

Review:

- independent Review Agent accepted with no Critical findings；
- Required fix was packaging-only: ensure the new MTR `.test/.result` files
  are staged with the source change；
- Minor note: local `id_value` / `v_value` are only facade decode outputs; SQL
  result already verifies the materialized `7,42` row。

Notes:

- commit: `b9c521a933a` Add PQTableScanIterator PQWR gather smoke；
- full `parallel_query` suite intentionally not run during development per
  current constraint。

#### Batch D1.6p - Threaded PQWR record_gather fullscan gate

Status: completed, reviewed, committed, and pushed.

目标：

- 增加 debug-only DOP=2 worker-thread `PQWR` fullscan gate；
- worker 复用现有 callback scan/open/close 生命周期，但通过
  `Query_result_mq` 写 `PQWR` row/FINISH；
- leader 通过 `PQTableScanIterator::Read()` 中的
  `MQ_record_gather::mq_scan_next_worker_result()` 消费真实 worker thread
  产生的 `PQWR` rows；
- 不修改普通 `parallel_query=ON` 默认路径，不改 optimizer eligibility、
  AccessPath、InnoDB 非 fullscan、ORDER BY 或 GROUP BY 路径。

Implementation:

- 新增 `PQ_worker_task::CALLBACK_PQWR_PRODUCER`；
- 新增 `PQ_worker_result_mq_row_sink`，复用两个稳定 `Item_int`，避免每行
  在 worker mem_root 上分配 Item；
- 新增 `Gather_operator::run_worker_callback_pqwr_threaded_producer()`；
- `PQTableScanIterator` 新增
  `pq_read_threaded_pqwr_record_gather_path` DBUG gate，且只允许
  `parallel_query=ON`、eligible、DOP=2、两列以上非 BLOB 表进入；
- worker 失败时关闭 MQ producer，leader 在 `WOULD_BLOCK` 和 EOF 前检查
  worker terminal error，避免错误路径挂起；
- 新增成功路径 MTR `pq_read_threaded_pqwr_record_gather`；
- 新增错误路径 MTR `pq_read_threaded_pqwr_worker_error`。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 8` passed；
- targeted MTR passed:
  `pq_read_threaded_pqwr_record_gather`
  `pq_read_threaded_pqwr_worker_error`
  `pq_leader_pqwr_record_gather_smoke`
  `pq_read_threaded_dop2_shadow`。

Review:

- initial Review Agent requested fixes for worker error progress, row-order
  stability, and per-row mem_root growth；
- fixes were applied: worker error marks status before producer detach,
  success MTR uses sorted output, and PQWR sink reuses stable `Item_int`
  instances instead of per-row allocation；
- final Review Agent accepted with no remaining findings。

Notes:

- commit: `48c26181885` Add threaded PQWR record gather smoke；
- full `parallel_query` suite intentionally not run during development per
  current constraint。

#### Batch D1.6q - PQWR multi-field NULL materialization

Status: completed, reviewed, committed, and pushed.

目标：

- 将 PQWR worker-result path 从两列 INT smoke 扩展到多字段 typed
  materialization；
- 支持 worker TABLE 前 N 个字段序列化为 `PQWR` frame，并在 leader
  `table->record[0]` 上按 frame 字段数写回；
- 支持 NULL bitmap，不再要求所有字段非 NULL；
- 保持 debug-only gate，不扩大普通 `parallel_query=ON` 默认路径。

Implementation:

- `Query_result_mq` 新增 `send_table_row(THD*, TABLE*, uint32)`，直接从
  worker TABLE fields 序列化 `PQWR` ROW frame；
- `pq_materialize_worker_result_smoke_row()` 放宽为
  `1 <= field_count <= table->s->fields`，按字段写入并处理 NULL；
- `PQ_worker_result_mq_row_sink` 改为使用 dummy metadata Items +
  `send_table_row()`，避免为每个数据字段构造 per-row Items；
- `pq_read_threaded_pqwr_record_gather` MTR 扩展覆盖 nullable INT、
  VARCHAR、CHAR、DECIMAL、DATE。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 8` passed；
- targeted MTR passed:
  `pq_read_threaded_pqwr_record_gather`
  `pq_read_threaded_pqwr_worker_error`
  `pq_leader_pqwr_record_gather_smoke`
  `pq_commercial_worker_result_adapter`
  `pq_read_threaded_row_image_datatypes`。

Review:

- initial Review Agent requested a fail-closed guard for projection subsets
  because current positional `PQWR` frames have no column-id map；
- fix applied: `pq_read_threaded_pqwr_record_gather_path` now requires all
  table fields to be present in `read_set`；
- MTR now verifies subset projection over a wide table does not launch PQWR
  workers, then verifies all-column multi-field/NULL PQWR materialization；
- final Review Agent accepted with no remaining findings。

Notes:

- commit: `8b7e3221537` Support multi-field PQWR materialization；
- full `parallel_query` suite intentionally not run during development per
  current constraint。

#### Batch D1.6r - PQWR subset field-index frame

Status: completed, reviewed, committed, and pushed.

目标：

- 移除 D1.6q 为规避 positional frame 风险引入的 all-column read_set gate；
- 为 `PQWR` ROW frame 增加 read_set field-index 映射，使 worker 能发送
  subset/projection 字段，而不是只能发送 TABLE 前 N 列；
- leader materialize 按 frame 中携带的 base table field index 写回
  `table->record[0]`；
- 保持旧 `flags=0` positional frame 和 `STABLE_REF` frame 兼容。

Implementation:

- `PQ_WORKER_RESULT_FRAME_FLAG_FIELD_INDEXES` 新增 indexed-row frame flag；
- `pq_decode_worker_result_row()` 在该 flag 下先解析
  `field_count * uint32 field_index` 前缀，再解析原有 length-prefixed
  value payload；
- `Query_result_mq::send_table_read_set_row()` 按 worker `TABLE::read_set`
  收集字段号并发送 indexed `PQWR` ROW frame；
- `PQ_worker_result_mq_row_sink` 的 metadata count 和发送路径改为按
  worker read_set 字段数对齐；
- `pq_materialize_worker_result_smoke_row()` 按 decoded `field_index`
  写回 leader TABLE，并拒绝越界或重复字段号；
- `pq_read_threaded_pqwr_record_gather_path` gate 从 all-column read_set
  放宽为 read_set 非空，同时继续要求 debug gate、DOP=2、eligible、
  non-BLOB table。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 8` passed；
- targeted MTR passed:
  `pq_read_threaded_pqwr_record_gather`
  `pq_read_threaded_pqwr_worker_error`
  `pq_leader_pqwr_record_gather_smoke`
  `pq_commercial_worker_result_adapter`
  `pq_read_threaded_projection_where`。

Review:

- independent Review Agent accepted with no Critical or Important findings；
- one Minor wire-contract note was fixed: receiver-side validation now rejects
  `FIELD_INDEXES` on non-ROW frames, matching sender-side validation；
- remaining Minor coverage note: projection + WHERE where `read_set` includes
  predicate-only fields should be covered as a follow-up task。

Test coverage:

- prefix subset projection: `SELECT id, nullable_i FROM t1`；
- non-prefix subset projection: `SELECT wide_v, d FROM t1`；
- all-column typed/NULL regression: `SELECT id, nullable_i, wide_v, fixed_c,
  amount, d FROM t1`；
- worker error and legacy PQWR smoke paths retained。

Notes:

- commit: this commit (`Support PQWR subset field mapping`)；
- full `parallel_query` suite intentionally not run during development per
  current constraint。
