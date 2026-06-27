# D1.7 商用 Worker Pull Fullscan 任务书

## 状态

Status: implemented locally；targeted build/MTR passed；independent code review
in progress。

## 背景

当前分支已经具备两类 fullscan row source：

- 用户可见 DOP2/DOP4 fullscan：主要通过 `PQWR -> MQ_record_gather ->
  Exchange_nosort -> PQTableScanIterator` 路径消费 worker row image；
- typed worker pull bridge：`pq_worker_scan_next(PQ_Worker_context *, ...)`
  已通过 InnoDB `Parallel_reader` callback producer 读取 row，并保留
  leader read-view、worker error、KILL、cleanup 护栏。

但商用 taurusdbondstore 主路径仍有关键差异：

- 商用 worker iterator 调 `handler::ha_pq_next(record, scan_ctx)`；
- `ha_pq_next()` 再调 InnoDB `pq_worker_scan_next(void *, uchar *)`；
- InnoDB commercial `void *scan_ctx` 是 `Parallel_leader` / worker dispatch
  语义，当前 8.0.46 分支的 `void *scan_ctx` 背后是 typed
  `PQ_Leader_context` adapter；
- 当前 `ha_innobase::pq_worker_scan_next(void *, uchar *)` 仍返回
  `HA_ERR_UNSUPPORTED`。

因此 D1.7 的目标不是重复已完成的 typed bridge，也不是一次性恢复 ref/range/ICP
正路径，而是把商用 `ha_pq_next(void *)` fullscan pull-row 语义推进到一个
受控、可测试、可回滚的小切口。

## 目标

- 为 clustered fullscan 建立 commercial `void *scan_ctx` worker-pull bridge；
- 让 `ha_pq_next(record, scan_ctx)` 在显式 DBUG-only smoke 下能读取当前 worker
  handler 的 clustered fullscan rows；
- 复用当前已验证的 typed InnoDB worker context 和 callback-backed pull bridge，
  不启用 latent `row_search_mvcc()` cursor path；
- 保持默认用户可见 fullscan 仍走当前 PQWR record_gather 路径，直到 D1.7 通过
  review 和 targeted MTR 后再考虑替换；
- 不打开 range、ref、dependent-ref、secondary ICP、partition、MVI、reverse、
  native Record_buffer、ORDER BY。

## 非目标

- 不修改 `PQRefIterator::Read()` production 行为；
- 不调用或实现 positive `pq_ref_build_ranges()`；
- 不把 visible AccessPath REF/RANGE 改写到 worker；
- 不改 `Query_result_mq` frame 协议；
- 不绕过当前 leader read-view 和 worker cleanup 合同；
- 不跑全量 `parallel_query` suite 作为开发期必需步骤。

## 文件边界

允许修改：

- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`（仅允许增加 DBUG-only smoke trigger）
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-d1-worker-pull-fullscan.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

禁止修改：

- `sql/parallel_query/pq_iterators.cc` production iterator behavior；
- `sql/parallel_query/pq_iterator.cc` production iterator behavior；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange.*`；
- `sql/join_optimizer/access_path.cc`；
- `sql/handler.h` / `sql/handler.cc` public API shape；
- `storage/innobase/row/row0pread_pq.cc` latent cursor path，除非只改注释或
  debug-only contract helper；
- ORDER BY / GROUP BY / ref / ICP / partition / MVI 主路径。

## 设计约束

1. `void *scan_ctx` 必须先被验证为 typed `PQ_Leader_context *` adapter；
   不得假设它是 taurusdbondstore 的 `Parallel_leader *`。
2. worker-side `ha_pq_next()` smoke 必须从独立 worker TABLE / handler 调用；
   不得复用 leader `TABLE::record[0]` 或 leader handler。
3. row production 必须复用当前 typed
   `pq_worker_scan_next(PQ_Worker_context *, uchar *, bool *)`，或复用同一
   InnoDB callback-backed materialization helper；不得启用 worker-local
   `row_search_mvcc()` cursor path。
4. `ha_pq_next()` smoke 必须显式创建 typed `PQ_Worker_open_context` 并调用
   `pq_worker_scan_init(PQ_Worker_open_context *, PQ_Worker_context **)`；
   不能只依赖 legacy `pq_worker_scan_init(uint, void *)`，因为后者不会创建
   callback-backed typed worker context。
5. unsupported/error 不得伪装成 EOF；EOF 只能来自正常 `eof=true` 或
   `HA_ERR_END_OF_FILE` 到 `ha_pq_next()` 的标准转换。
6. DBUG smoke 必须验证 cleanup exactly once：
   `pq_worker_scan_end(PQ_Worker_context *)` 或等价 cleanup 必须释放 worker
   context；`pq_leader_scan_end(void *)` 必须释放 leader context。
7. 任何 worker-start 后的错误都必须计入明确的 diagnostic counter，不能
   silent serial fallback。

## 推荐实现切片

### D1.7a: RED MTR 和状态变量合同

新增状态变量：

- `Parallel_worker_void_pull_attempts`
- `Parallel_worker_void_pull_success`
- `Parallel_worker_void_pull_rows`
- `Parallel_worker_void_pull_eofs`
- `Parallel_worker_void_pull_unsupported`
- `Parallel_worker_void_pull_failures`
- `Parallel_worker_void_pull_cleanup`

在 `pq_commercial_fullscan.test` 增加 DBUG-only smoke window：

- 建表沿用当前 `t1` clustered fullscan；
- 在 no-DBUG baseline 下断言上述 counter 不增长；
- 在 `SET SESSION debug='d,pq_worker_void_pull_fullscan_smoke'` 下执行一个
  当前已经会进入 `PQTableScanIterator` / `Gather_operator` 的 DOP2 fullscan
  查询，由 `pq_iterator.cc` 或 `Gather_operator` 中的 DBUG-only smoke harness
  主动调用 `ha_pq_init()`、typed worker init、`ha_pq_next()`、cleanup；
- 断言 attempts/success/rows/eofs/cleanup 为正；
- 断言 unsupported/failures 为 0；
- 断言 cleanup delta 精确等于 success delta；
- 断言 visible PQWR counters 不因 smoke 额外增长；
- SQL 结果继续用当前 serial oracle。

RED 命令：

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 pq_commercial_fullscan \
  --vardir=/tmp/pq-d17a-red-vardir --tmpdir=/tmp/pq-d17a-red-tmpdir
```

预期：实现前失败，原因是新增状态变量不存在或 DBUG smoke counter 不增长。

### D1.7b: DBUG-only void pull bridge

新增一个显式可达的 DBUG-only smoke harness：

- 首选位置：`Gather_operator` 中复用现有 worker open helper，让 smoke 拿到
  independent worker THD/TABLE/handler；
- 备选位置：`PQTableScanIterator::Init()` 里已有 DBUG smoke 区域，但只能增加
  DBUG-only 调用，不能改变 production iterator 行为；
- harness 必须按顺序调用：
  1. leader handler `ha_pq_init(dop, keyno)`，拿到 handler `pq_ctx`；
  2. worker handler typed
     `pq_worker_scan_init(PQ_Worker_open_context *, PQ_Worker_context **)`；
  3. worker handler `ha_pq_next(worker_record, leader_handler->pq_ctx)` 循环，
     直到 EOF；
  4. worker typed `pq_worker_scan_end(worker_ctx)`；
  5. leader handler `ha_pq_end()`。

在 `ha_innobase::pq_worker_scan_next(void *, uchar *)` 中：

- 默认仍 fail-closed；
- 仅在 `DBUG_EXECUTE_IF("pq_worker_void_pull_fullscan_smoke", ...)` 下进入
  D1.7 bridge；
- bridge 必须从 worker handler 找到由 harness 创建的 typed worker context；
- 调用当前 typed `pq_worker_scan_next(PQ_Worker_context *, uchar *, bool *)`；
- `eof=true` 时返回 `HA_ERR_END_OF_FILE`，让 `ha_pq_next()` 走既有
  EOF/row status wrapper；
- 非 EOF row 返回 0；
- error 原样返回 handler error。

实现必须保留默认路径：

```cpp
int ha_innobase::pq_worker_scan_next(void *scan_ctx, uchar *buf) {
  DBUG_EXECUTE_IF("pq_worker_void_pull_fullscan_smoke", {
    return pq_worker_scan_next_void_fullscan_smoke(scan_ctx, buf);
  });
  return HA_ERR_UNSUPPORTED;
}
```

### D1.7c: Review、GREEN、提交

验证命令：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d17-final-vardir --tmpdir=/tmp/pq-d17-final-tmpdir
```

完成后启动独立 Review Agent，问题清单：

- 是否仍是 DBUG-only；
- 是否没有改变默认 `pq_worker_scan_next(void *, uchar *)` fail-closed 行为；
- `ha_pq_next()` 是否通过明确可达的 DBUG smoke harness 触发，而不是依赖普通
  serial SELECT 自然触发；
- 是否显式调用 typed worker init 并验证 worker THD/TABLE/handler ownership；
- 是否复用了 typed worker context / callback-backed materialization；
- 是否没有打开 ref/range/ICP/ORDER BY；
- MTR 是否证明 no-DBUG zero、DBUG positive、PQWR visible counter 不被 smoke
  污染；
- cleanup 是否 exactly once，且 cleanup delta 与 success delta 精确一致。

## 设计 Review 结果

Initial docs/design review found three blocking issues, all fixed in this
taskbook revision:

- 普通 serial query 不会自然调用 `ha_pq_next()`；任务书现在要求显式
  DBUG-only smoke harness；
- allowed files 漏掉 smoke harness 位置；现在允许 `sql_parallel.cc` 和
  `pq_iterator.cc` 的 DBUG-only hook；
- `void *` bridge 不能只依赖 legacy `pq_worker_scan_init(uint, void *)`；
  任务书现在要求先创建 typed `PQ_Worker_context *`。

Review accepted 后单独 commit，建议提交信息：

```text
Add PQ void worker pull fullscan smoke
```

## 风险与暂缓项

- 直接把商用 `PQ_Ctx::read_record()` / low-level cursor path 搬入当前分支风险
  大：它会绕过当前已验证的 `Parallel_reader` callback materialization、
  read-view、KILL/error priority 护栏。本任务禁止这样做。
- ref/dependent-ref 需要 `pq_ref_build_ranges()`、per-ref key dispatch、
  worker-side cloned ICP 生命周期，放到 F6f/F6g 后续任务。
- visible fullscan 默认路径切换到 `ha_pq_next(void *)` 必须等 D1.7 smoke、
  review、targeted MTR 均通过后再单独设计。

## 完成报告

### 实现摘要

- 新增 D1.7 状态变量：
  `Parallel_worker_void_pull_attempts`、
  `Parallel_worker_void_pull_success`、
  `Parallel_worker_void_pull_rows`、
  `Parallel_worker_void_pull_eofs`、
  `Parallel_worker_void_pull_unsupported`、
  `Parallel_worker_void_pull_failures`、
  `Parallel_worker_void_pull_cleanup`。
- 在 `PQTableScanIterator::Init()` 增加
  `pq_worker_void_pull_fullscan_smoke` DBUG-only hook；默认 production iterator
  行为不变。
- 在 `Gather_operator::run_worker_void_pull_fullscan_smoke()` 中显式执行：
  `ha_pq_init(1, MAX_KEY)`、worker TABLE/THD open、typed
  `pq_worker_scan_init(PQ_Worker_open_context*, PQ_Worker_context**)`、
  `ha_pq_next(worker_record, leader_pq_ctx)` 循环、typed worker cleanup、
  leader `ha_pq_end()`。
- 在 InnoDB `pq_worker_scan_next(void*, uchar*)` 中保留默认
  `HA_ERR_UNSUPPORTED`；只有 DBUG hook 启用时才复用当前 typed
  `PQ_Worker_context` 和 callback-backed pull bridge。
- smoke harness 在调用 `ha_pq_init()` 前把 handler PQ 状态归一化为
  clustered fullscan，避免旧的 ref/range/reverse 状态阻断 DBUG-only
  contract；该归一化只发生在 smoke harness 中。

### RED/GREEN 证据

- RED：实现前运行
  `pq_commercial_fullscan`，失败点为 7 个新增状态变量不存在，
  smoke delta 为 `NULL`，证明测试能捕获缺失功能。
- 调试确认：首轮 GREEN 失败发生在 `ha_pq_init()` 前置 gate；临时分支标记显示
  failure stage 为 leader init，root cause 是旧 handler PQ shape 状态未归一化。
  临时诊断已移除。
- GREEN：

```bash
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d17-targeted2-vardir \
  --tmpdir=/tmp/pq-d17-targeted2-tmpdir
```

结果：全部通过。

### 当前风险

- D1.7 只证明 debug-only `ha_pq_next(void*)` fullscan pull bridge；默认可见
  fullscan 尚未切换到 commercial void-pull 路径。
- `m_pq_void_pull_smoke_worker_ctx` 是 handler-local smoke carrier，由 typed
  worker init 设置，并在 typed worker cleanup、legacy worker cleanup、
  leader cleanup 中清理；独立 review 仍需确认是否有遗漏路径。

### Code Review 处理记录

独立 review 提出 1 个 Important 和 1 个 Minor，均已处理：

- Important：`pq_worker_scan_next(void*, uchar*)` 必须验证 `scan_ctx` 是 typed
  InnoDB leader adapter，不能只依赖 handler-local worker context。已补充
  `PQ_Leader_context::kind() == INNODB`、`InnoDB_pq_sql_leader_context::innodb_ctx()`
  非空、worker context 绑定的 InnoDB leader 与 `scan_ctx` 一致的校验。
- Minor：测试需要直接证明 DBUG smoke 不污染 visible PQWR counters。已在
  `pq_commercial_fullscan` 中 snapshot
  `Parallel_visible_pqwr_record_gather_selected` 和
  `Parallel_visible_pqwr_record_gather_rows`，DBUG smoke 后断言 delta 为 0。

复核验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d17-reviewfix-vardir \
  --tmpdir=/tmp/pq-d17-reviewfix-tmpdir
```

结果：全部通过。
