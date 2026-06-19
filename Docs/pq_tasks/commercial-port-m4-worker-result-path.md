# M4 Query_result_mq And Worker Result Path Taskbook

## 状态

Planned。

## 目标

对齐商用 worker result 输出路径。本阶段拆成两个提交：

- M4a: MQ wire protocol contract；
- M4b: worker `Query_result_mq` path。

先明确当前 typed row-image header 与商用 `Field_raw_data` 协议的主路线，再接最小 worker result 正例。由于 M3 只建立 clone activation probe，不生成可执行 worker plan，M4 的正例必须限制为 synthetic/受控 worker result path，或者只在 M3 probe 边界之后验证 result path contract；不得越过 M6 fullscan gate 打开真实 InnoDB 并行执行。

## 允许修改

- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/exchange.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/parallel_query/pq_iterators.*`
- `mysql-test/suite/parallel_query/t/pq_commercial_worker_result.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_worker_result.result`
- `Docs/pq_tasks/commercial-port-m4-worker-result-path.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 先支持单表 SELECT projection；
- WHERE predicate 先沿用 worker plan，不新增手写过滤；
- BLOB/TEXT/JSON/GEOMETRY 等复杂类型先 fallback；
- worker ERROR、leader KILL、early EOF 必须复用当前 cleanup 语义；
- M4 不迁移 aggregation / COUNT DISTINCT / Batch_buffer spill；
- MQ 格式必须有严格长度校验或等价版本边界；
- leader 必须识别 FINISH / ERROR / DETACHED；
- 真实 InnoDB worker row source 仍由 M6 打开，M4 不得提前接入；
- 不改变默认 OFF 行为。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_worker_result pq_read_threaded_external_kill pq_read_threaded_worker_error pq_read_threaded_row_image_datatypes --parallel=1 --vardir=/tmp/pqv_m4 --tmpdir=/tmp/pqt_m4
```

## Completion Report

Pending.
