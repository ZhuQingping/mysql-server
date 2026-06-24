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

## W1 合同确认报告

Status: completed.

### 当前 typed API 是主合同

当前仓库同时保留 typed API 和商用兼容 `void *` API，但安全主路径是 typed
contract：

- `handler::pq_leader_scan_init(THD*, PQ_Leader_context**, ...)` 创建 leader
  context；
- `handler::pq_worker_scan_init(PQ_Worker_open_context*, PQ_Worker_context**)`
  创建 worker context；
- `handler::pq_worker_scan_next(PQ_Worker_context*, uchar*, bool *eof)` 是
  typed pull-row 入口；
- `handler::pq_worker_scan_callback_produce(PQ_Worker_context*, PQ_row_sink*)`
  是当前已经验证的 callback row-stream producer；
- `pq_worker_scan_end(PQ_Worker_context*)` 和
  `pq_leader_scan_end(PQ_Leader_context*)` 是 typed cleanup 边界。

商用兼容 wrappers `ha_pq_init()` / `ha_pq_next()` / `ha_pq_end()` 仍存在，
但当前 InnoDB `pq_worker_scan_next(void*, uchar*)` 直接返回 unsupported，不是
当前可依赖的安全主路径。

### 当前 InnoDB owner / read-view / cleanup

- `pq_leader_scan_init()` 创建 `InnoDB_pq_leader_ctx` 和 SQL wrapper；
  handler 内部保存 leader ctx，`PROBE` 是 fallback-safe，`EXECUTE` 是
  no-fallback commit point；
- leader init 使用 `Parallel_reader::available_threads()` 获取线程预算；
  失败路径和 leader end 必须释放预算；
- leader trx owns read view，worker 不能创建独立 read view，也不能 mutate
  leader trx；
- `pq_leader_scan_end()` 先删除 worker contexts，再释放线程预算/read view，
  最后删除 leader ctx/wrapper；
- `pq_worker_scan_init()` 要求独立 worker TABLE/handler/record buffer；
  worker end 删除 SQL wrapper，并由 wrapper 析构释放底层
  `InnoDB_pq_worker_ctx`。

### 当前可复用边界

可以复用当前 callback producer，而不是绕过它：

- `InnoDB_pq_scan_ctx::store_mysql_record()` 统一负责把 InnoDB record 转为
  MySQL record；
- `produce_callback_rows_for_range()` 通过 `Parallel_reader::add_scan()` /
  `run()` 使用 leader trx/read-view；
- `PQ_row_sink` 要求 SQL 层 deep-copy record image；
- leader / worker error、KILL、abort、MQ ERROR token priority 已有测试护栏。

### 商用调用链

商用仓库的正路径是：

1. worker `PQblockScanIterator::Init()` 调
   `table()->file->pq_worker_scan_init(keyno, m_pq_ctx)`；
2. worker `PQblockScanIterator::Read()` 调
   `table()->file->ha_pq_next(m_record, m_pq_ctx)`；
3. `handler::ha_pq_next()` 包 IO wait wrapper 后调用
   `pq_worker_scan_next(scan_ctx, buf)`；
4. InnoDB `pq_worker_scan_next(void*, uchar*)` 从 `Parallel_leader` dispatch
   `PQ_Ctx_Base`，再调 `ctx->read_record(buf, m_prebuilt)`；
5. `PQ_Ctx::read_record()` 用商用自有 cursor / mtr / visibility path 读页并
   materialize MySQL record。

### 不能整块拷贝商用 InnoDB 的原因

- 商用 `void *scan_ctx == Parallel_leader*` 假设与当前 typed
  `PQ_Leader_context` / `PQ_Worker_context` owner 不一致；
- 商用 `PQ_Ctx::read_record()` 低层 cursor path 会绕过当前 8.0.46 已验证的
  `Parallel_reader` thread budget、read-view cleanup、callback materialization
  边界；
- 当前 `row0pread_pq.*` 已明确 worker-local pull-row path 不能在 snapshot
  合同未证明前接入主路径；
- 直接打开商用 pull-row 会绕过当前 KILL / ERROR / row sink deep-copy 护栏。

### W2 决策

W2 只允许窄桥接：

- 保持 typed API 为主；
- 不启用商用低层 `PQ_Ctx::read_record()` cursor path；
- 不让 `void *` API 成为真实 row source；
- 如需增加 bridge，只能把当前已验证的 callback producer /
  `PQ_row_sink` materialization 封装成可逐步替换 worker pull 的兼容层；
- 如果实现需要共享 leader `row_prebuilt_t`、绕过 typed worker context、绕过
  `Parallel_reader::available_threads()/release_threads()` 或绕过 leader
  read-view close-on-end，则停止编码。

## W2 完成报告

Status: completed.

### 实现内容

- `ha_innobase::pq_worker_scan_next(PQ_Worker_context*, uchar*, bool*)`
  不再直接 unsupported，改为 typed callback-backed pull bridge；
- bridge 继续使用当前已验证的
  `InnoDB_pq_scan_ctx::produce_callback_rows_for_range()`，不启用
  commercial `void *` API，也不启用 latent `InnoDB_pq_ctx::read_record()`
  / `row_search_mvcc()` cursor path；
- `InnoDB_pq_worker_ctx` 增加 worker-local record-image buffer，第一次
  typed pull 时通过 `PQ_row_sink` deep-copy assigned range 的 MySQL record
  image，后续逐行 copy 到传入的 worker `record`；
- typed next 每次重新校验 `open_ctx` owner：worker handler、worker table、
  worker THD、record buffer 必须与当前 handler/prebuilt 一致；
- buffer 增加 8MB hard cap、8192 row cap，并取 session
  `parallel_memory_limit` 的较小值；cap 命中或 `std::bad_alloc` 转为
  `DB_OUT_OF_MEMORY` / fail-closed；
- 新增 `Parallel_worker_typed_pull_next_calls/rows/eofs` status counters；
- 新增 debug-only MTR `pq_worker_typed_pull_next_smoke`，证明 typed pull-next
  被执行、读出 3 行并到达 EOF，同时用户可见查询仍走 serial fallback。

### 保持不变的边界

- `pq_worker_scan_next(void*, uchar*)` 仍保持 unsupported；
- 没有打开 secondary/ref/ICP/partition/MVI/ORDER BY/GROUP BY 主路径；
- read-view、thread budget、worker TABLE/handler 独立性仍由现有
  leader/worker init 与 cleanup gate 负责；
- W2 bridge 是向商用 worker row-at-a-time pull 形态靠近的过渡层，不是最终
  商用 `PQ_Ctx::read_record()` full port。

### Review

- 独立 Review Agent 第一轮发现两个 Important：buffer 无上限/OOM 转换不足、
  typed next 未重新校验 open context owner；
- 已修复并复审通过；复审结论为无 Critical/Important；
- 剩余建议：未来 full port 需要专门覆盖 cap/OOM 负向路径，并以 streaming /
  cursor pull 替代当前 bounded eager buffer。

### 验证

已验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_typed_pull_next_smoke pq_stats \
  pq_fullscan pq_read_view pq_rec_visible pq_read_record_crash \
  pq_worker_error pq_kill pq_kill_query \
  pq_read_threaded_dop2_read_view pq_read_threaded_dop2_external_kill \
  --parallel=1 --vardir=/tmp/pq-worker-row-pull-target-vardir \
  --tmpdir=/tmp/pq-worker-row-pull-target-tmpdir
```

结果：上述 targeted MTR 含 `shutdown_report` 共 12 项通过。

完整套验证：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pq-worker-row-pull-full-vardir \
  --tmpdir=/tmp/pq-worker-row-pull-full-tmpdir
```

结果：full `parallel_query` suite 含 `shutdown_report` 共 105 项通过。
