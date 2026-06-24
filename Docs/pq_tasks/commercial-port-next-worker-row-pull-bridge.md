# Next Task: Clustered Fullscan Worker Row Pull Bridge

## 状态

Design taskbook prepared. Coding not started.

## 背景

E1-A 已完成商用名 adapted 测试迁移：

- fullscan / blob / not-equal / aggregate no-record；
- worker error / kill / external kill；
- found_rows / read-view / record-visible / read-record crash。

当前测试套已经能证明若干 DOP=2 threaded row-stream 场景，但主路径与
`/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 仍有核心差距：
商用 worker-side row source 最终通过 handler/InnoDB `ha_pq_next()` /
`pq_worker_scan_next()` 拉取真实记录；当前仓库仍主要依赖安全的 typed
callback/shadow/row-stream adapter，商用 `ParallelScanIterator` 与 worker
JOIN plan 还未完全接上。

只读 Code Gap Explorer 建议下一批优先做“clustered fullscan worker row pull
bridge”，不要整块搬 InnoDB 商用实现。

## 目标

在不打开 range/ref/dependent-ref/ICP/partition/MVI 的前提下，收敛 clustered
fullscan worker row production 与商用实现的差距：

- 明确当前 typed `PQ_Worker_context` 与商用 `void *scan_ctx` / `ha_pq_next()`
  的兼容边界；
- 为 clustered fullscan worker row pull 提供最小可执行或最小可验证的桥接；
- 保留当前 8.0.46 已验证的 read-view、KILL、worker error、cleanup 语义；
- 不让 `PROBE` 阶段产生不可回退状态；
- `EXECUTE`/worker-start 之后不得静默串行 fallback。

## 推荐工作拆分

### W1: Read-only Contract Confirmation

输出一个短报告到本文件：

- 当前 `sql/handler.*` PQ worker scan API；
- 当前 `storage/innobase/handler/ha_innodb_pq.cc` typed worker next 行为；
- 商用 `ha_pq_next()` / `pq_worker_scan_next(void*, uchar*)` 的最小调用链；
- 当前 callback row-stream adapter 里已经能复用的 read-view / row materialize
  边界；
- 不能整块拷贝商用 InnoDB 的具体 API 差异。

### W2: Minimal Bridge Implementation

允许修改：

- `sql/handler.h`
- `sql/handler.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- 必要时：`sql/parallel_query/pq_handler.*`
- 本任务书。

禁止修改：

- `sql/parallel_query/pq_clone*`
- `sql/parallel_query/pq_resolver.*`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/exchange*`
- `sql/join_optimizer/access_path.*`
- `sql/sql_executor.*`
- ORDER BY / GROUP BY / ref / ICP 相关主路径；
- 大批量 MTR result 重写。

实现约束：

- 只针对 clustered fullscan；
- 不改变 secondary/ref/ICP gate；
- 不引入第二套不受当前 read-view/KILL cleanup 保护的 InnoDB scan owner；
- 若复用商用函数名，必须保留 8.0.46 typed context 的所有权和生命周期语义；
- 如果发现只能通过绕过当前 callback read-view 保护来实现，则停止在设计报告，
  不编码。

### W3: Review and Verification

每次编码后必须启动独立 Review Agent，重点看：

- read-view 生命周期；
- worker context owner；
- handler `end` 是否 exactly once；
- KILL / ERROR priority；
- 是否意外打开 range/ref/ICP/partition；
- 是否只是增加 smoke，而没有向商用真实路径靠近。

验证命令：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_fullscan pq_read_view pq_rec_visible pq_read_record_crash \
  pq_worker_error pq_kill pq_kill_query \
  pq_read_threaded_dop2_read_view pq_read_threaded_dop2_external_kill \
  --parallel=1 --vardir=/tmp/pq-worker-row-pull-target-vardir \
  --tmpdir=/tmp/pq-worker-row-pull-target-tmpdir
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pq-worker-row-pull-full-vardir \
  --tmpdir=/tmp/pq-worker-row-pull-full-tmpdir
```

## 当前验收基线

最近已验证：

- E1-A3 targeted MTR: all 5 tests successful；
- full `parallel_query` suite: all 104 tests successful。

后续任何代码迁移必须保持这个基线，除非明确记录新的商用 positive result 替换
旧 fallback/boundary result。
