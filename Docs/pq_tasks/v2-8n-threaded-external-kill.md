# V2-8N Threaded External KILL 任务书

## 目标

V2-8N 目标是在 V2-8M 默认 OFF 实验变量保护路径之后，补齐 threaded DOP=1 row stream 的 external `KILL QUERY` 资源回收验证。

本阶段只验证 debug-only threaded path：

- worker thread 已启动；
- leader 还未消费 row stream；
- 另一个连接执行 `KILL QUERY`；
- leader `Read()` 检测 kill，传播 abort，join worker，并释放 leader ctx；
- 查询返回 `ER_QUERY_INTERRUPTED`，不 fallback、不标记 executed。

## 范围

允许修改：

- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_read_threaded_external_kill.test`
- `mysql-test/suite/parallel_query/r/pq_read_threaded_external_kill.result`
- `Docs/pq_tasks/README.md`
- 本任务文档

禁止修改：

- 默认打开真实 DOP=1 threaded path；
- 打开 DOP>1；
- 启用 `pq_worker_scan_next()`；
- 接入 partial aggregation；
- 修改 optimizer eligibility。

## 设计要求

新增 debug sync 点 `pq_read_threaded_worker_started`，位置在 `run_worker_callback_threaded_producer()` 成功返回之后、leader `Read()` 开始消费之前。

MTR 采用确定性多连接流程：

1. 默认连接启用 `pq_read_threaded_shadow_path`；
2. 设置 `DEBUG_SYNC='pq_read_threaded_worker_started SIGNAL pq_worker_started WAIT_FOR pq_kill_continue TIMEOUT 10'`；
3. 默认连接 `--send SELECT * FROM t1`；
4. killer 连接等待 `pq_worker_started` 后 `KILL QUERY $target_id`；
5. killer 连接 signal `pq_kill_continue`；
6. 默认连接 `--reap`，预期 `ER_QUERY_INTERRUPTED`；
7. 校验 `executed_delta=0`、`fallback_delta=0`、`workers_delta=1`；
8. 跑串行 `COUNT(*)` 验证后续连接/表状态可继续使用。

## 验证

已执行：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_external_kill --parallel=1 --vardir=/tmp/pqv_kill --tmpdir=/tmp/pqt_kill
```

最终提交前已执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_read_threaded_external_kill \
  pq_read_threaded_worker_error \
  pq_read_threaded_abort_cleanup \
  pq_read_threaded_projection_where \
  pq_read_threaded_experimental_var \
  --parallel=1 --vardir=/tmp/pqv_kill_group --tmpdir=/tmp/pqt_kill_group

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## 完成报告

实现内容：

- 在 `PQTableScanIterator::Init()` threaded producer 启动后加入 `DEBUG_SYNC(thd(), "pq_read_threaded_worker_started")`。
- 新增 `pq_read_threaded_external_kill` debug-only MTR，覆盖 external `KILL QUERY` 后的 abort/join/cleanup。

风险点：

- 该用例仍是 debug-only shadow path，不代表默认真实路径已打开。
- 只覆盖 DOP=1 threaded path；DOP>1 range correctness 后续单独推进。
- `KILL QUERY` 在 worker 已启动后、leader 消费前触发；worker 消费中途 kill 仍可另立用例。

当前状态：Completed，完整 suite 已通过，等待提交。
