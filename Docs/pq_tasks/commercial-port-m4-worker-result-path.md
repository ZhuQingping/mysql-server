# M4 Query_result_mq And Worker Result Path Taskbook

## 状态

M4a Completed。M4b Completed。

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

M4a completed by Codex Orchestrator.

Changed files:

- `sql/parallel_query/query_result_mq.h`
- `sql/parallel_query/query_result_mq.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_worker_result.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_worker_result.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m4-worker-result-path.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- 定义 M4a worker-result MQ frame contract：`PQ_worker_result_frame_header`、`PQ_worker_result_message_type`、`Field_raw_data` 占位结构；
- 新增 `pq_validate_worker_result_frame()`，校验 magic/version/type、ROW/FINISH/ERROR shape、NULL bitmap/payload 长度；
- 新增本地 `pq_run_query_result_mq_contract_smoke()`，通过本地 `MQueue` synthetic ROW + FINISH 验证协议边界；
- 新增 `Parallel_worker_result_smoke_rows` / `Parallel_worker_result_smoke_finishes` status counters；
- `Query_result_mq::send_data()` 继续 fail-closed，不发送真实 SQL worker rows；
- 不创建 worker plan，不启动 worker，不读 InnoDB row，不进入真实 fullscan gate；
- 保持现有 typed row-image (`PQRM`) 和 partial group (`PQGP`) 路径不变；M4a worker-result frame 使用独立 magic (`PQWR`)。

Review:

- Explorer Agent 确认 M4 最小安全目标应是协议边界和 synthetic validation，不应打开真实 worker fullscan；
- 第一轮 Review Agent 发现 worker-result frame 发送/解码的 32-bit length addition overflow blocker；
- 已改为发送侧使用 `uint64` 计算总长并拒绝 `> UINT32_MAX`，解码侧使用 `remaining` 递减校验；
- 已新增 malformed huge length smoke 覆盖该边界；
- 第二轮 Review Agent 确认 blocker 已解决，建议可提交；
- 记录后续风险：`exchange.cc` 旧 typed MQ helper 仍有类似 header + payload_len 32-bit addition 模式，后续应独立收敛。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_worker_result pq_stats pq_read_threaded_external_kill pq_read_threaded_worker_error pq_read_threaded_row_image_datatypes --parallel=1 --vardir=/tmp/pqv_m4_target --tmpdir=/tmp/pqt_m4_target
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m4_full --tmpdir=/tmp/pqt_m4_full
```

Result:

- `mysqld` build passed；
- M4 targeted suite passed；
- full current `parallel_query` suite passed，71 tests successful。

Remaining:

M4b completed by Codex Orchestrator.

Changed files:

- `sql/parallel_query/query_result_mq.h`
- `sql/parallel_query/query_result_mq.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_worker_result.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_worker_result.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-m4-worker-result-path.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- `Query_result_mq::send_data()` 从 fail-closed 改为 controlled worker-result ROW frame 发送；
- ROW frame payload 当前为内部 length-prefixed string 序列，NULL bitmap 使用 `ceil(field_count / 8)` 并由 validator 强校验；
- `Query_result_mq::send_eof()` 发送 FINISH frame；`thd->is_error()` 时发送 synthetic ERROR frame 并保持失败语义；
- 新增 `pq_run_query_result_mq_send_data_smoke()`，通过本地 `MQueue` 和 synthetic `Item_int(7), Item_int(42)` 验证 `send_data()` + FINISH + ERROR；
- 新增 `Parallel_worker_result_smoke_errors` status counter，并扩展 `pq_commercial_worker_result` / `pq_stats` 护栏；
- smoke 调用前后保存并恢复 leader THD `sent_row_count`，避免 synthetic row 污染用户语句诊断；
- M4b 不创建 cloned JOIN，不启动 worker，不读取 InnoDB row，不把 `Query_result_mq` 接入真实 worker execution。

Review:

- 第一轮 Review Agent 发现 `send_data()` smoke 使用 leader THD 会污染 `sent_row_count`，判定为 blocker；
- 已通过 `get_sent_row_count()` / `set_sent_row_count()` 在 smoke 内恢复计数闭环；
- 同步收紧 ROW `null_bitmap_len == ceil(field_count / 8)`；
- 第二轮 Review Agent 确认 blocker 已解决，未发现新的提交前必须修改项，建议可提交。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query pq_commercial_worker_result pq_stats
./mtr --suite=parallel_query
```

Result:

- `mysqld` build passed；
- M4b targeted suite passed；
- full current `parallel_query` suite passed，73 tests successful。

Remaining:

- 真实 worker execution 尚未改为 `Query_result_mq` result path；
- ERROR frame payload 仍是 synthetic string，后续真实 worker path 需要携带 MySQL error code/message；
- `std::vector` OOM 未映射为 MySQL error，真实 result transport 前需收敛；
- 是否长期保留独立 `PQWR` frame，或与 `Exchange` typed header flags 收敛，仍需在后续集成阶段确认。
