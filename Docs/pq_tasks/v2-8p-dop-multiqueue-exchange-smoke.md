# V2-8P DOP Multi-Queue Exchange Smoke 任务书

## 目标

V2-8P 目标是在不打开 InnoDB DOP>1 真实 row stream 的前提下，收紧 SQL 层 Exchange/Gather 多队列 row-image smoke 验证。

当前 DOP>1 的最大风险是 worker 分片消费尚未打开。V2-8P 只覆盖它之前的一层：

- `Exchange_nosort` 可以从多个 worker MQ 队列读取 ROW；
- 每个 worker 的 FINISH 都被消费；
- leader 能在 DOP=1/2/4 fallback probe 中完成 synthetic row-image materialization；
- 真实执行计数仍保持 0，查询仍 serial fallback。

## 范围

允许修改：

- `mysql-test/suite/parallel_query/t/pq_exchange_rows_dop1.test`
- `mysql-test/suite/parallel_query/r/pq_exchange_rows_dop1.result`
- `Docs/pq_tasks/README.md`
- 本任务文档

禁止修改：

- 打开 InnoDB DOP>1 worker row stream；
- 改 `pq_worker_scan_init()` 的 DOP=1 gate；
- 改 iterator EXECUTE DOP；
- 改默认实验变量。

## 设计

沿用现有 `pq_exchange_rows_dop1` 测试的三次 fallback probe：

- `parallel_default_dop=1`
- `parallel_default_dop=2`
- `parallel_default_dop=4`

每次 `PQTableScanIterator::Init()` 会创建 `Gather_operator(smoke_dop)`，并调用 `run_exchange_row_image_smoke()`。该 smoke 对每个 worker queue 写入一条 ROW 和一条 FINISH，因此三次查询在 row-image 专用 counter 上应精确产生：

- `row_image_rows_delta = 1 + 2 + 4 = 7`
- `row_image_finishes_delta = 1 + 2 + 4 = 7`

新增两个更窄的 status counters，避免和 callback smoke 共用的 `Parallel_exchange_smoke_rows` / `Parallel_exchange_smoke_finishes` 混淆：

- `Parallel_exchange_row_image_smoke_rows`
- `Parallel_exchange_row_image_smoke_finishes`

共享总 counter 仍保持可观测：当前三次 probe 的总 exchange delta 为 `16/13`，其中包含 row-image smoke 和后续 callback conversion/multirow smoke。

同时保持：

- `executed_delta=0`
- `fallback_delta=3`
- `rows_delta=0`
- `workers_delta=0`
- `worker_smoke_delta=3`
- `exchange_rows_delta=16`
- `exchange_finishes_delta=13`
- `row_image_rows_delta=7`
- `row_image_finishes_delta=7`

## 验证

已执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query pq_exchange_rows_dop1 \
  --parallel=1 --vardir=/tmp/pqv_exchange --tmpdir=/tmp/pqt_exchange

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## 完成报告

实现内容：

- 新增 `Parallel_exchange_row_image_smoke_rows` / `Parallel_exchange_row_image_smoke_finishes`。
- 将 `pq_exchange_rows_dop1` 的 row-image smoke 校验从共享总 counter 的 `>= 6` 收紧为专用 counter 的精确 delta。
- 明确 DOP=1/2/4 三次 probe 对应 7 条 synthetic ROW 和 7 个 FINISH。

风险点：

- 这仍是 synthetic Exchange smoke，不代表 InnoDB DOP>1 range correctness 已完成。
- 后续必须单独验证 range dispatch、每个 worker 的独立 TABLE/handler/read-view 语义和无重无漏。

当前状态：Completed，完整 suite 已通过，等待提交。
