# Batch E1 - 商用 parallel_query 测试迁移

Last synced: 2026-06-24

## 目标

将 `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/mysql-test/suite/parallel_query`
中的商用测试套按当前代码能力迁入当前仓库。E1 不以旧 V1 manifest 为准，
而以商用支持场景对齐为目标。

## 当前基线

按 `.test` 文件统计：

| 项 | 数量 | 说明 |
|---|---:|---|
| 当前仓库 `mysql-test/suite/parallel_query/t/*.test` | 92 | 当前完整 MTR 为 93/93，包含 `shutdown_report` |
| 商用仓库 `mysql-test/suite/parallel_query/t/*.test` | 98 | 主迁移目标 |
| 同名测试 | 1 | 仅 `pq_not_support` 同名 |
| 商用 include | 8 | `include/*.inc` |
| 商用 `*-master.opt` | 11 | 部分依赖商用 sysvar / debug 环境 |

结论：当前已有大量改写测试，但文件名与商用 suite 不同，不能用同名数量判断
覆盖率。后续 E1 需要同时维护“商用测试名映射”和“当前改写护栏”。

## 商用 include / opt 文件

商用 include：

- `pq_abort.inc`
- `pq_coverage.inc`
- `pq_divide_derived_setup.inc`
- `pq_on_disk_hash_join.inc`
- `pq_round_truncate_queries.inc`
- `pq_support_aggregate_query.inc`
- `tpch_cleanup.inc`
- `tpch_prepare.inc`

当前策略：

- 不直接复制启用 TPC-H、hash spill、abort/debug 专属 include；
- 在迁移对应测试前，逐个 include 做依赖裁剪；
- 若 include 只包含通用建表/查询片段，可按当前 sysvar 名称适配后迁入；
- 若依赖 Dstore/TaurusDB 专属变量、数据文件、debug flag，则记录为 deferred。

商用 `*-master.opt`：

- `pq_autoinc-master.opt`
- `pq_bugfix-master.opt`
- `pq_divide_derived_low_mmap-master.opt`
- `pq_divide_derived_no_mmap-master.opt`
- `pq_explain_analyze-master.opt`
- `pq_fallback-master.opt`
- `pq_innodb_intrinsic-master.opt`
- `pq_leader_exception-master.opt`
- `pq_limit_no_order_by-master.opt`
- `pq_subquery_correlated-master.opt`
- `pq_sysdate_is_now-master.opt`

当前策略：

- 不单独迁入未启用测试的 `master.opt`；
- 随对应 `.test` 一起迁移和校准；
- 所有使用商用私有启动参数的 opt 必须先改写或 deferred。

## 第一批迁移优先级

### E1-A：当前能力已接近，可迁移为商用名 adapted 测试

这些测试优先做“商用 SQL shape 精简迁移”，不追求一次拷贝完整大文件：

- `pq_fullscan`
- `pq_blob`
- `pq_not_equal`
- `pq_aggr_no_record`
- `pq_found_rows`
- `pq_read_view`
- `pq_rec_visible`
- `pq_read_record_crash`
- `pq_worker_error`
- `pq_kill`

验收：

- 每个测试单独运行通过；
- 完整 `parallel_query` suite 通过；
- 保留商用测试名或在文档中记录当前 adapted 测试名映射。

### E1-B：必须等主路径继续打开后再迁移 positive result

这些测试代表当前与商用的主要场景差异，不能直接整文件启用：

- `pq_order_by`
- `pq_prepare`
- `pq_bugfix`
- `pq_hash_join`
- `pq_subquery`
- `pq_subquery_correlated`
- `pq_derived_view`
- `pq_partition`
- `pq_record_buffer`
- `pq_icp`
- `pq_range_sec`

当前处理：

- 先抽最小 SQL shape 做 fallback/deferred boundary；
- 等对应主路径打开后，再改为 positive result；
- 不把多个未打开主路径混在一个大测试里。

### E1-C：允许 deferred 的商用/环境专属项

- Dstore/TaurusDB 专属变量或引擎；
- audit plugin；
- replica topology；
- TPC-H 外部数据；
- hash spill / mmap 环境依赖；
- restart/debug injection 本地不可稳定构造项；
- DML/DDL 语义超出当前 SELECT PQ 迁移主线的测试。

## 下一步任务拆分

### E1-A1：fullscan edge 商用名映射

目标：

- 将当前 `pq_commercial_fullscan*` 与商用 `pq_fullscan` / `pq_blob` /
  `pq_not_equal` / `pq_aggr_no_record` 建立文件级映射；
- 优先新增或重命名 adapted 测试，不删除当前已通过护栏；
- 不引入 ORDER BY、subquery、partition、ref/ICP。

验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_fullscan \
  pq_commercial_fullscan_edges --parallel=1 \
  --vardir=/tmp/pq-e1a1-target-vardir --tmpdir=/tmp/pq-e1a1-target-tmpdir
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pq-e1a1-full-vardir --tmpdir=/tmp/pq-e1a1-full-tmpdir
```

### E1-A2：kill / worker error 商用名映射

目标：

- 将当前 `pq_leader_row_stream_*`、`pq_read_threaded_*worker_error`、
  external kill 测试映射到商用 `pq_worker_error`、`pq_kill`、`pq_kill_query`
  的可迁移子集；
- 保留不可构造 debug injection 为 deferred。

### E1-B1：ORDER / prepare / subquery deferred boundary

目标：

- 分别从商用 `pq_order_by`、`pq_prepare`、`pq_subquery_correlated` 抽一个
  最小 SQL shape；
- 当前先验证 fallback/deferred reason；
- 等 Exchange_sort / clone / subquery 主路径打开后转为 positive result。

## 风险约束

- 不一次性复制 98 个商用 `.test`；
- 不引入会使完整 suite 大面积失败的商用 result；
- 不为了测试名对齐而删除当前 92 个已通过 `.test` 护栏；
- 每个迁移批次必须跑 targeted MTR 和 full `parallel_query` suite；
- 每个源码/测试批次后启动独立 review Agent。

## 状态

Status: E1 planning baseline completed. 下一步进入 E1-A1。
