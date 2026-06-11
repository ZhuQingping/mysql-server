# V2-4 Parallel_reader Thin Adapter + Range Partition 任务书

## Goal

V2-4 是 DOP>1 正确性的硬 gate。目标是在不直接打开真实 worker row stream 的前提下，替换 `row0pread_pq.cc` 当前 single whole-range scaffold，建立可验证的 disjoint range partition contract：

1. 复用或薄封装上游 `Parallel_reader::Scan_ctx::partition()`；
2. PQ adapter 能获得互斥 `[start,end)` range 元数据；
3. range ownership / deep-copy 规则明确；
4. DOP=1/2/4 的 range 数和边界可观测；
5. 仍不宣称真实 PQ row stream、worker THD、DOP>1 查询结果已经完成。

## Current Baseline

- Latest commit: `54bd78429cf Tighten PQ V2-3 read view boundary`
- `row0pread_pq.cc` 当前只创建一个 whole-table range，start/end 均为 `nullptr`。
- `Parallel_reader::Scan_ctx::partition()`、`Range`、`Iter`、`create_contexts()` 当前是 `Parallel_reader::Scan_ctx` private surface。
- V2-3 已禁止 worker row read，避免共享 leader `m_prebuilt`。

## Scope

允许：

- 给 `Parallel_reader` 增加最小 public/protected wrapper，用于收集 disjoint range metadata；
- 在 `row0pread_pq.*` 中引入能够持有 range boundary 的 InnoDB PQ range/iter；
- 增加 debug/status observable 或 MTR helper，用于验证 range count / no overlap / no whole-range fallback；
- 增加 V2-4 MTR，限定为 DOP=1/2/4 range partition smoke，不执行真实 worker row stream；
- 更新 V2 roadmap/test matrix。

禁止：

- 直接暴露 `Parallel_reader::Ctx` 作为 SQL PQ worker execution context；
- 在 V2-4 中启动 worker THD 或读取真实 row；
- 直接 fork 大段 TaurusDB `row0pread_pq` 实现而不适配 MySQL 8.0.46 上游结构；
- 打开 DOP>1 SELECT 结果验收；
- 改动 optimizer eligibility 使用户认为 DOP>1 已可执行。

## Design Decisions

1. `Parallel_reader::Scan_ctx::Range` / `Iter` / `Ctx` 继续保持 private；
   V2-4 只新增 stable boundary DTO，不暴露 traversal 对象。
2. DTO 必须深拷贝 `dtuple_t` boundary，不能保存 `Parallel_reader::Iter`
   heap、page record 或 persistent cursor 的裸指针。
3. V2-4 不迁移 TaurusDB 完整 `row0pread_pq` fork。TaurusDB 的实现包含
   low-level cursor/MTR/record-buffer traversal、ICP、secondary index、
   partition/ref 路径，超过当前阶段边界。
4. V2-4 只替换 single whole-range partition scaffold；worker row read 仍保持
   disabled，真实按 boundary seek/end 截断留到 worker prebuilt/trx 合同完成后。

## Allowed Files

- `storage/innobase/include/row0pread.h`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb.*`（仅用于 bridge 调用和 observable）
- `mysql-test/suite/parallel_query/**`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-4-parallel-reader-range-partition.md`
- `Docs/pq_tasks/v2-execution-path-roadmap.md`
- `Docs/pq_tasks/v2-test-matrix.md`

## Validation

基础验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback pq_read_view_dop1 pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

若 V2-4 新增 range observable，则 targeted 命令必须加入新测试，例如：

```bash
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_range_partition_dop
```

## Acceptance Checklist

- [x] `Parallel_reader` private boundary 已明确，不暴露不稳定内部执行对象；
- [x] PQ adapter 不再只创建 single whole-table range；
- [x] range boundary ownership / deep-copy 规则明确；
- [x] DOP=1/2/4 range partition 可通过测试或 debug observable 验证；
- [x] 不启动 worker、不读取 PQ row；
- [x] DOP>1 查询结果验收仍被 gate；
- [x] `mysqld` build 通过；
- [x] 完整 `parallel_query` suite 通过。

## Current Status

- Status: Verified, pending follow-up commit
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Parallel Agent Notes

- `Parallel_reader` boundary explorer 确认当前 `Scan_ctx::partition()`、
  `Range`、`Iter`、`Ctx::traverse()` 都是 private；不能只改
  `row0pread_pq.cc` 拿到 disjoint ranges。
- 推荐新增 `Parallel_reader::export_scan_ranges()`，由上游内部持有 index
  S-lock、调用 private partition，然后返回 heap-owned boundary DTO。
- TaurusDB reference explorer 确认 TaurusDB 是独立 PQ scanner/fork，不是复用
  `Parallel_reader::run()`；只适合作为 `[start,end)` half-open range 语义参考。
- 两个 explorer 都建议 V2-4 只暴露 range boundaries，不暴露 `Ctx` traversal，
  不开启 worker row stream。

## Implementation Plan

1. 在 `Parallel_reader` 增加 `Exported_range` / `Exported_ranges` DTO。
2. 新增 `Parallel_reader::export_scan_ranges()`：内部创建 `Scan_ctx`、
   S-lock index、调用 private `partition()`，把 `Range` 转为深拷贝 DTO，
   不调用 `create_contexts()`，不读取 row。
3. 给 `InnoDB_pq_iter` 增加 heap-owned `dtuple_t` boundary。
4. `InnoDB_pq_scan_ctx::partition()` 调用 `export_scan_ranges()`，替换当前
   single whole-range scaffold。
5. 保持 `pq_worker_scan_init()` / `pq_worker_scan_next()` unsupported。

## Completion Report - Range Planning Export

### Changed Files

- `storage/innobase/include/row0pread.h`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-4-parallel-reader-range-partition.md`

### Implementation Summary

- 新增 `Parallel_reader::Exported_range` / `Exported_ranges`，作为
  heap-owned boundary DTO。
- 新增 `Parallel_reader::export_scan_ranges()`：内部创建 `Scan_ctx`、
  S-lock index、调用 private `partition()`，将 private `Range/Iter`
  转为深拷贝 DTO；不创建 execution `Ctx`，不读取 row。
- `InnoDB_pq_iter` 现在持有 heap-owned `dtuple_t` boundary。
- `InnoDB_pq_scan_ctx::partition()` 调用 `export_scan_ranges()`，将
  exported ranges 转换为 `InnoDB_pq_range`。
- 修复 range planning 使用 `Parallel_reader(0)`，避免未预留 thread budget
  时析构释放线程预算导致 debug assertion。

### Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback pq_read_view_dop1 pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

结果：

- `mysqld` build 通过。
- 崩溃回归用例 `pq_iterator_safe_fallback` 通过。
- targeted MTR: 4/4 pass。
- full `parallel_query` suite: 15/15 pass。

### Follow-up Observable

- 新增 `Parallel_ranges_built` global status variable。
- `PQTableScanIterator::Init()` 的 bridge smoke 使用 session
  `parallel_default_dop` 请求 leader init，但仍立即 leader end 并 serial fallback。
- 新增 `pq_range_planning_dop`，覆盖 DOP=1/2/4 下 range planning 可观测，
  同时断言 `executed/workers/rows` 仍为 0。

### Final Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_range_planning_dop pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

结果：

- `mysqld` build 通过。
- `pq_range_planning_dop pq_stats`: 3/3 pass。
- full `parallel_query` suite: 16/16 pass。

### Remaining Risks

- worker row read 仍 disabled；真实按 boundary seek/end 截断留到后续阶段。
- `Parallel_ranges_built` 证明 range planning 发生，不证明 DOP>1 查询结果正确。
- DOP>1 查询结果验收仍不能开启。
