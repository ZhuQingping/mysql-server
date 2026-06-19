# M9 Secondary Index / Ref / ICP Taskbook

## 状态

Planned。

## 目标

迁移 `PQRefIterator`、secondary index、ICP 能力。该阶段依赖 M5/M6 的 handler/InnoDB execution gate 稳定。

推荐拆分：

- M9-A: secondary index/range eligibility 仍默认 fallback，补负向 MTR；
- M9-B: `PQblockScanIterator` secondary index range 最小正例，不开 ICP；
- M9-C: `PQRefIterator` 非唯一 ref 最小正例，含 `pq_ref_build_ranges`；
- M9-D: ICP pushdown，迁移 `make_cond_for_index` / `idx_cond_push` 路径；
- M9-E: MVI unique filter、reverse scan、secondary index MIN 等边角。

## 允许修改

- `sql/parallel_query/pq_iterators.*`
- `sql/handler.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 先支持 ref/range 最小正例；
- ICP 打开前必须有负向 fallback 测试；
- `pushed_idx_cond` 存在但未验证时必须拒绝；
- MVI/unique filter 逻辑按商用实现迁移并单测；
- 不支持 partition/subquery 形态时必须 fallback。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_range_planning_dop pq_range_dispatch_dop1 pq_read_threaded_dop2_multirange pq_explain_fallback --parallel=1 --vardir=/tmp/pqv_m9 --tmpdir=/tmp/pqt_m9
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9_full --tmpdir=/tmp/pqt_m9_full
```

若完整 suite 因耗时或环境问题未执行，Completion Report 必须明确说明原因，并至少保留 ref/ICP targeted suite、build、review Agent 检视结论。

## Completion Report

Pending.
