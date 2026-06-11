# V2-6 Exchange/Gather Row Stream 任务书

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task-by-task. 本项目阶段文档使用中文；代码标识、命令、MySQL 概念保留英文。

**Goal:** 建立 V2-6 Exchange/Gather row stream 的最小可验证闭环，先用 synthetic MQ token/row stream 验证 SQL 层消息协议、EOF/error 语义和 cleanup，不接真实 InnoDB worker row scan。

**Architecture:** V2-6 不改变 optimizer eligibility，不让 `PQTableScanIterator::Read()` 返回真实并行行。先在 `Gather_operator` / `Exchange_nosort` 内增加 synthetic producer/consumer smoke，让 MQ `ROW/FINISH/ERROR` token 的发送、读取、关闭和统计可测试。真实 worker THD、record image 到 `table->record[0]`、InnoDB `pq_worker_scan_next()` 留到 V2-7/V2-8 之后。

**Tech Stack:** MySQL 8.0.46 debug build, SQL iterator framework, `sql/parallel_query/msg_queue.*`, `sql/parallel_query/exchange.*`, MTR `parallel_query` suite.

---

## Current Baseline

- Baseline commit: `265e5e10a69 Add PQ V2-5 worker lifecycle smoke`
- V2-5 已完成：
  - `Gather_operator::run_worker_lifecycle_smoke()`；
  - `PQTableScanIterator` owns `PQ_Leader_context` / `Gather_operator`，并在 safe window 清理；
  - `Parallel_worker_smoke_runs` observable；
  - `pq_worker_dop1` 验证 fallback/range/smoke，真实 `executed/workers/rows` 仍为 0。
- 当前 `Gather_operator::gather_rows()` 只调用 `Exchange_nosort::read_mq_message()`；没有 producer，也没有 row image 转换。
- 当前 `PQTableScanIterator::Read()` 仍只 delegate 到 serial fallback iterator。

## Scope

允许：

- 给 `Exchange_nosort` 或 `Gather_operator` 增加 synthetic MQ smoke helper；
- 使用固定小 payload 验证 `ROW -> FINISH` token 顺序；
- 增加 global status observable，例如 `Parallel_exchange_smoke_rows` 和 `Parallel_exchange_smoke_finishes`；
- 新增 `pq_exchange_rows_dop1` 或类似 MTR，验证 synthetic row token 被 gather 读取，但真实执行 counters 仍为 0；
- 更新 `pq_stats`、README、V2 task doc。

禁止：

- 不允许 `PQTableScanIterator::Read()` 向上层返回 synthetic row；
- 不允许把 synthetic payload 填进 `table->record[0]`；
- 不允许启动真实 worker THD；
- 不允许调用 `ha_innobase::pq_worker_scan_next()` 读取 row；
- 不允许增加 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_rows_scanned`；
- 不允许改变 EXPLAIN 文案为 actual executed。

## File Structure

- Modify `sql/parallel_query/msg_queue.h` / `.cc`
  - 只在现有 API 不足时补小 helper；优先复用 `send()`、`send_control_token()`、`receive()`。
- Modify `sql/parallel_query/exchange.h` / `.cc`
  - 增加 synthetic producer/consumer smoke helper，或暴露足够小的测试入口。
- Modify `sql/parallel_query/sql_parallel.h` / `.cc`
  - 在 `Gather_operator` 增加 `run_exchange_row_stream_smoke()`；
  - 增加 smoke stats，不污染真实 execution stats。
- Modify `sql/parallel_query/pq_iterator.cc`
  - 在 V2-5 lifecycle smoke 后追加 V2-6 exchange smoke，仍立即 cleanup + serial fallback。
- Modify `sql/mysqld.cc`
  - 暴露新增 smoke status variables。
- Create `mysql-test/suite/parallel_query/t/pq_exchange_rows_dop1.test`
- Create `mysql-test/suite/parallel_query/r/pq_exchange_rows_dop1.result`
- Modify `mysql-test/suite/parallel_query/t/pq_stats.test`
- Modify `mysql-test/suite/parallel_query/r/pq_stats.result`
- Modify `Docs/pq_tasks/README.md`
- Modify `Docs/pq_tasks/v2-6-exchange-gather-row-stream.md`

## Implementation Tasks

### Task 1: 写 RED 测试

**Files:**

- Create: `mysql-test/suite/parallel_query/t/pq_exchange_rows_dop1.test`
- Create: `mysql-test/suite/parallel_query/r/pq_exchange_rows_dop1.result`

Test intent:

- 使用和 `pq_worker_dop1` 类似的多页 InnoDB full scan；
- 设置 `parallel_query=ON`、`parallel_cost_threshold=0`、DOP=1/2/4；
- 执行三次 `SELECT * FROM t1`，隐藏大结果；
- 断言：
  - `Parallel_queries_executed` delta = 0；
  - `Parallel_workers_launched` delta = 0；
  - `Parallel_rows_scanned` delta = 0；
  - `Parallel_queries_fallback` delta = 3；
  - `Parallel_worker_smoke_runs` delta >= 1；
  - `Parallel_exchange_smoke_rows` delta >= 1；
  - `Parallel_exchange_smoke_finishes` delta >= 1。

Expected RED:

```text
Unknown status variable / NULL delta for Parallel_exchange_smoke_rows
```

Run:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_exchange_rows_dop1
```

### Task 2: 增加 Exchange synthetic stream helper

**Files:**

- Modify: `sql/parallel_query/exchange.h`
- Modify: `sql/parallel_query/exchange.cc`

Required behavior:

- 新增 helper，例如：

```c++
bool Exchange_nosort::run_synthetic_row_stream_smoke(uint32 worker_id,
                                                     const uchar *payload,
                                                     uint32 payload_len,
                                                     uint32 *rows_read,
                                                     uint32 *finishes_read);
```

- helper 内部必须：
  - 获取 `MQueue_handle`；
  - `send(MQMessageType::ROW, payload, payload_len)`；
  - `send_control_token(MQMessageType::FINISH)` 或等价 close；
  - 调用 `read_mq_message()` 读取 row；
  - 验证 row payload 长度和值；
  - 读取 finish token 或确认 all finished；
  - 不阻塞等待真实 worker。

### Task 3: Gather smoke 封装和状态变量

**Files:**

- Modify: `sql/parallel_query/sql_parallel.h`
- Modify: `sql/parallel_query/sql_parallel.cc`
- Modify: `sql/mysqld.cc`

Required behavior:

- `PQ_global_stats` 增加：

```c++
std::atomic<uint64> exchange_smoke_rows{0};
std::atomic<uint64> exchange_smoke_finishes{0};
```

- `Gather_operator` 增加：

```c++
bool run_exchange_row_stream_smoke(THD *leader_thd);
```

- `run_exchange_row_stream_smoke()` 只能在 initialized gather 上运行，或像 V2-5 smoke 一样自行 init/destroy；
- 成功后只增加 `exchange_smoke_*`，不得增加真实 execution counters。

### Task 4: 接入 PQTableScanIterator safe window

**Files:**

- Modify: `sql/parallel_query/pq_iterator.cc`

Required behavior:

- 在 leader init 成功后：
  - 创建 `Gather_operator`；
  - 运行 `run_worker_lifecycle_smoke()`；
  - 运行 `run_exchange_row_stream_smoke()`；
  - cleanup PQ resources；
  - serial fallback。
- 如果 synthetic exchange smoke 失败，返回 query error，不 fallback 掩盖内部错误。
- 不改变 `Read()`：仍 delegate 到 serial fallback iterator。

### Task 5: 更新 stats 测试和文档

**Files:**

- Modify: `mysql-test/suite/parallel_query/t/pq_stats.test`
- Modify: `mysql-test/suite/parallel_query/r/pq_stats.result`
- Modify: `Docs/pq_tasks/README.md`
- Modify: `Docs/pq_tasks/v2-6-exchange-gather-row-stream.md`

Required behavior:

- `pq_stats` 的 `Parallel%` 变量数量和名称包含新增 exchange smoke counters；
- V2-6 Completion Report 写明 changed files、validation、remaining risks；
- README 更新 V2-6 状态。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16

build-ninja/runtime_output_directory/mysqladmin --no-defaults --socket=/private/tmp/pq20.sock -uroot shutdown 2>/dev/null || true
rm -f /private/tmp/pq20.sock /private/tmp/pq-v2-4-mysql.pid /private/tmp/pq-v2-4-mysql.log
build-ninja/runtime_output_directory/mysqld --no-defaults \
  --datadir=/private/tmp/pq-v2-0-mysql \
  --basedir=$PWD/build-ninja \
  --lc-messages-dir=$PWD/share \
  --port=3350 \
  --socket=/private/tmp/pq20.sock \
  --pid-file=/private/tmp/pq-v2-4-mysql.pid \
  --log-error=/private/tmp/pq-v2-4-mysql.log \
  --skip-name-resolve \
  --mysqlx=0 \
  --daemonize

cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_exchange_rows_dop1 pq_worker_dop1 pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root

../../build-ninja/runtime_output_directory/mysqladmin --no-defaults --socket=/private/tmp/pq20.sock -uroot shutdown 2>/dev/null || true
```

## Acceptance Checklist

- [x] synthetic ROW token 能被 Exchange/Gather 读取；
- [x] FINISH/EOF token 能被读取或正确识别；
- [x] synthetic smoke 不污染真实 execution counters；
- [x] `PQTableScanIterator::Read()` 仍不返回 synthetic row；
- [x] serial fallback 结果不变；
- [x] targeted MTR 通过；
- [x] full `parallel_query` suite 通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11

## Completion Report

### Codex Orchestrator 实现结果（2026-06-11）

Changed files:

- `sql/parallel_query/exchange.h`
- `sql/parallel_query/exchange.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_exchange_rows_dop1.test`
- `mysql-test/suite/parallel_query/r/pq_exchange_rows_dop1.result`
- `mysql-test/suite/parallel_query/t/pq_stats.test`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-6-exchange-gather-row-stream.md`

Implementation summary:

- 新增 `Exchange_nosort::run_synthetic_row_stream_smoke()`：
  - 每个 worker queue 预填 8-byte synthetic ROW payload；
  - 发送显式 `FINISH` control token；
  - 通过现有 `read_mq_message()` round-robin 消费；
  - 校验 payload magic、row 数和 finish 数。
- 新增 `Gather_operator::run_exchange_row_stream_smoke()`，封装 V2-6
  synthetic Exchange/Gather smoke，不进入 SQL `Read()`。
- 新增 global status：
  - `Parallel_exchange_smoke_rows`
  - `Parallel_exchange_smoke_finishes`
- `PQTableScanIterator::Init()` 在 V2-5 worker lifecycle smoke 后追加
  V2-6 exchange smoke，然后继续 cleanup + serial fallback。
- 新增 `pq_exchange_rows_dop1`，验证 DOP=1/2/4 eligible SELECT 下：
  - fallback delta = 3；
  - worker smoke delta = 2；
  - exchange synthetic row/finish delta = 6；
  - real executed/workers/rows 仍为 0。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_exchange_rows_dop1 pq_worker_dop1 pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

结果：

- `mysqld` build 通过。
- targeted MTR: 4/4 pass。
- full `parallel_query` suite: 18/18 pass。

Remaining risks:

- `read_mq_message()` 的 `false` 仍同时表示 active-but-empty 和 all-done；
  V2-6 smoke 通过预填 ROW/FINISH 避开了真实异步等待问题。
- control token 仍依赖 1-byte payload 判定；真实 row stream 前需要更稳的
  message header/type contract。
- `PQTableScanIterator::Read()` 仍不消费 Exchange row，也不填充
  `table->record[0]`；真实结果流后移。

Commit:

- `d0b4988a584 Add PQ V2-6 exchange row stream smoke`
