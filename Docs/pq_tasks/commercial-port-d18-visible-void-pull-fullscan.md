# D1.8 Visible Fullscan Void-Pull Gate

## 状态

Status: design prepared；coding not started。

## 背景

D1.7 已在 DBUG-only smoke 下证明 clustered fullscan 的 commercial
`handler::ha_pq_next(record, scan_ctx) -> ha_innobase::pq_worker_scan_next(void*, uchar*)`
bridge 可以复用当前 typed worker context / callback-backed pull bridge 读取
worker rows。

当前用户可见 DOP2 fullscan 仍走：

```text
PQWR -> MQ_record_gather -> Exchange_nosort -> PQTableScanIterator::Read()
```

而 taurusdbondstore 商用 worker scan 主形态是 worker iterator 调
`ha_pq_next()`。D1.8 的目标是把最小用户可见 clustered fullscan gate 继续向
commercial void-pull route 收敛，但不一次性迁移 ref/range/ICP/ORDER BY。

## 目标

- 新增一个用户可见但受实验变量或 DBUG gate 控制的 clustered fullscan
  void-pull execution path；
- `PQTableScanIterator::Init()` 创建并持有一个 independent worker
  THD/TABLE/handler、typed worker context、leader `pq_ctx`；
- `PQTableScanIterator::Read()` 每次调用 worker handler
  `ha_pq_next(worker_record, leader_pq_ctx)`；
- 成功读到 worker row 后，把 worker record materialize/copy 到 leader
  `table()->record[0]`，让 SQL executor 继续从 leader TABLE 取值；
- EOF 时 cleanup worker context、worker TABLE/THD、leader context；
- 默认现有 PQWR fullscan gate 先不移除，D1.8 用独立 gate 对比验证。

## 非目标

- 不打开 secondary range、ref、dependent-ref、ICP、partition、MVI、reverse、
  native Record_buffer、ORDER BY、GROUP BY；
- 不迁移商用 low-level `PQ_Ctx::read_record()` / `row_search_mvcc()` cursor；
- 不改 `Query_result_mq` / `Exchange_nosort` frame 协议；
- 不改变 handler public API shape；
- 不删除现有 PQWR path，直到 D1.8 positive path、review、targeted MTR
  通过后再单独做切换决策。

## 设计要求

1. 必须复用 D1.7 的 typed worker init 和 void-pull bridge，不新增第二套
   InnoDB scan owner。
2. Worker TABLE / handler / record buffer 必须 independent；不得把
   leader `table()->record[0]` 传给 worker handler。
3. `ha_pq_next()` 输出到 worker record 后，SQL 层必须使用现有安全 record
   image copy/materialization helper 或等价字段 copy，把 row 放到 leader record。
   如果只能通过共享 record buffer 实现，则停止编码。
4. `Read()` 中 KILL、worker error、EOF cleanup 必须保持当前
   `PQTableScanIterator` error priority 语义；worker-start 后不得 silent serial
   fallback。
5. `m_pq_void_pull_smoke_worker_ctx` 不能成为 production carrier。若需要持久
   visible path，应在 `PQTableScanIterator` / `Gather_operator` 持有
   `PQ_worker_info::m_worker_ctx`，由 iterator cleanup 结束。
6. 新状态变量必须与 PQWR counters 区分，建议：
   - `Parallel_visible_void_pull_selected`
   - `Parallel_visible_void_pull_rows`
   - `Parallel_visible_void_pull_eofs`
   - `Parallel_visible_void_pull_cleanup`
   - `Parallel_visible_void_pull_failures`
7. D1.8 path 必须先以 DBUG gate 或实验变量 gate 打开；不要直接替换现有默认
   DOP2 PQWR gate。

## 允许修改

- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-d18-visible-void-pull-fullscan.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/exchange.*`
- `sql/join_optimizer/access_path.cc`
- `sql/sql_executor.*`
- `sql/handler.h` / `sql/handler.cc` public API shape
- `storage/innobase/row/row0pread_pq.cc` latent cursor path
- ORDER BY / GROUP BY / ref / ICP / partition / MVI production path

## RED 测试

在 `pq_commercial_fullscan` 增加 DBUG gate：

```sql
SET SESSION debug = 'd,pq_visible_void_pull_fullscan_path';
SELECT id FROM t1 WHERE val >= 10;
SET SESSION debug = '';
```

断言：

- SQL row 与 serial oracle 一致；
- `Parallel_visible_void_pull_selected = 1`；
- `Parallel_visible_void_pull_rows = 8`；
- `Parallel_visible_void_pull_eofs = 1`；
- `Parallel_visible_void_pull_cleanup = 1`；
- `Parallel_visible_void_pull_failures = 0`；
- 同一窗口内 `Parallel_visible_pqwr_record_gather_selected/rows` 不增长。

RED 预期：实现前状态变量不存在或 counters 不增长。

## GREEN 验证

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d18-final-vardir \
  --tmpdir=/tmp/pq-d18-final-tmpdir
```

## Review Checklist

- 是否真正让用户可见 query 通过 `PQTableScanIterator::Read()` 调
  `ha_pq_next(void*)`；
- 是否没有复用 leader record buffer 作为 worker output；
- worker context / worker TABLE / worker THD / leader ctx 是否 exactly once
  cleanup；
- EOF、KILL、worker error 是否不会被当成普通 row 或 silent fallback；
- 是否保留 DBUG/实验 gate，不直接替换默认 PQWR；
- 是否没有打开 ref/range/ICP/ORDER BY 等禁止路径。

