# V2-8B Row Image Protocol 任务书

## Goal

V2-8B 是 V2-8 真实 full scan 前的 row payload gate。目标是在不接真实
InnoDB worker row scan 的前提下，定义并验证 worker-local record buffer 到 MQ
payload，再到 leader `TABLE::record[0]` 的 copy/materialize contract。

本阶段允许修改 SQL PQ 的 Exchange/MQ/iterator 边界和 synthetic smoke 测试；禁止打开
真实 InnoDB worker scan。

## Current Baseline

- Baseline commit: `4146f63fab5 Record PQ V2-8A completion status`
- V2-8A 已确认 worker 必须拥有独立 `TABLE` / handler / `row_prebuilt_t`。
- 当前 `MQueue_handle::send()` / `receive()` 已支持 raw byte message：
  `[len:4B][data:lenB]`。
- 当前 `Exchange_nosort::get_next_from_worker()` 用 `raw_len == 1` 推断
  FINISH/ERROR control token；这不能承载通用 row image，因为 1 字节 row payload 会
  被误判。
- 当前 `run_synthetic_row_stream_smoke()` 只发送固定 `uint32[2]` payload，不 materialize
  到 `TABLE::record[0]`。

## Design Requirements

### Message Format

V2-8B 必须引入显式 row message header，禁止继续用 payload length 猜测消息类型。

建议格式：

```text
PQRowMessageHeader {
  uint32 magic;
  uint16 version;
  uint16 type;
  uint32 payload_len;
  uint32 flags;
}
payload bytes
```

- `type=ROW`：payload 是 fixed MySQL record image。
- `type=FINISH`：payload_len 必须为 0。
- `type=ERROR`：payload 可先放 `int handler_error` 或 0，真实错误传播后续扩展。
- `magic` 用于拒绝旧 synthetic payload 或损坏消息。

### Row Image MVP

- V2-8B row image MVP 使用 `table->s->reclength` 字节 fixed copy。
- worker-side 发送语义：复制 worker-local `TABLE::record[0]` 的 `reclength` bytes 到 MQ
  payload。
- leader-side materialize：收到 ROW 后，确认 payload_len 等于 leader
  `table->s->reclength`，然后 `memcpy(table->record[0], payload, reclength)`。
- materialize 后必须更新 row buffer status，满足 `TABLE` 注释中“making a row
  available in record[0]”的要求。
- 本阶段不实现 Field-level serialization、BLOB deep copy、压缩、spill-to-disk 或
  cross-TABLE layout conversion。

### Scope Boundary

允许修改：

- `sql/parallel_query/msg_queue.h`
- `sql/parallel_query/msg_queue.cc`
- `sql/parallel_query/exchange.h`
- `sql/parallel_query/exchange.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/**` 中 V2-8B synthetic/protocol 测试
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8b-row-image-protocol.md`

禁止修改：

- `storage/innobase/**` 真实 worker row scan 行为；
- `sql/handler.h` worker open/read-view contract；
- optimizer eligibility / plan rewrite；
- `PQ_execution_state::EXECUTED` 和真实 execution counters；
- DOP>1 range correctness 行为。

## Active Dispatch

### Design Explorer A - Exchange/MQ Payload

- Agent: `019eb557-a0a7-71a2-b927-7dcab2705670` (`Helmholtz`)
- Mode: read-only
- Scope:
  - `sql/parallel_query/msg_queue.*`
  - `sql/parallel_query/exchange.*`
  - `sql/parallel_query/sql_parallel.*`
  - `sql/parallel_query/pq_iterator.*`
- Expected output:
  - 当前 synthetic MQ row/token 表达；
  - variable-size row image 承载能力；
  - worker-local record buffer 到 MQ copy 的 API 位置；
  - EOF/error token cleanup 缺口；
  - allowed/forbidden files 和测试建议。

### Design Explorer B - Iterator/Record Buffer Boundary

- Agent: `019eb557-c585-7aa2-bdf1-bb3aec025e01` (`Mill`)
- Mode: read-only
- Scope:
  - `sql/parallel_query/pq_iterator.*`
  - `sql/parallel_query/pq_handler.h`
  - `sql/handler.h`
  - `storage/innobase/handler/ha_innodb.*`
  - MySQL `TABLE::record[0]` 必要定义
- Expected output:
  - `PQTableScanIterator::Read()` materialize 边界；
  - `TABLE::record[0]` / null bitmap / reclength fixed copy 可行性；
  - handler API 是否应在 V2-8B 扩展；
  - V2-8B 到 V2-8C handoff contract。

## Implementation Plan

1. 在 Exchange 层新增 typed message header 和 helper：
   - encode ROW / FINISH / ERROR；
   - decode 并校验 magic/version/payload_len；
   - 移除 `raw_len == 1` 的 token 判断依赖。
2. 新增 row image helper：
   - `send_record_image(MQueue_handle*, const uchar*, uint32)`；
   - `materialize_record_image(TABLE*, const void*, uint32)` 或等价内部函数。
3. 扩展 synthetic smoke：
   - 构造固定长度 record image payload；
   - 经过 MQ/Exchange decode；
   - copy 到 caller-provided record buffer；
   - 校验 payload copy 和 FINISH 计数。
4. 在 `Gather_operator` / `PQTableScanIterator` 处只接入 synthetic materialization
   smoke，不改变真实 execution fallback 行为。
5. 新增 MTR 或扩展现有 `parallel_query` V2 smoke，验证：
   - synthetic row image smoke rows > 0；
   - FINISH token 正常；
   - eligible 查询仍 fallback serial；
   - `Parallel_queries_executed` 不增加。

## Explorer Results

### Exchange/MQ Payload

- 底层 `MQueue_handle::send()` / `receive()` 已能承载 variable-size payload，
  因为 outer wire format 已是 `[len:uint32][data:len]`，且 receive local buffer 可扩容。
- 原协议不能继续使用：`Exchange_nosort::get_next_from_worker()` 依赖
  `raw_len == 1` 判断 control token，1 字节 row image 会误判。
- `MQueue` 不应理解 `TABLE`；typed row image header 应放在 Exchange 层。
- ERROR/ABORT token 的真实错误码和 worker loop 后续仍未完整实现，V2-8B 只保证 header
  decode 后不会再用 payload length 猜测类型。

### Iterator/Record Buffer Boundary

- V2-8B 不新增 handler row-image virtual API。handler 仍只负责后续 V2-8C 通过
  `pq_worker_scan_next(PQ_Worker_context*, uchar *record, bool *eof)` 填充
  worker-local record buffer。
- `table->s->reclength` 可作为 fixed record image MVP 长度；整段 copy 会包含
  null bitmap、varchar length/data 等 MySQL row buffer 内容。
- BLOB/TEXT/JSON/GEOMETRY 不适合 fixed copy，因为 record buffer 存的是指针槽。
  V2-8B synthetic smoke 对 `blob_fields > 0` 的表直接跳过 materialization；
  V2-8C 打开真实执行前必须显式 gate 或实现 deep serialization。
- `PQTableScanIterator::Read()` 是未来真实 PQ row materialization 边界；本阶段只在
  Init safe window 内运行 synthetic row-image smoke，之后仍进入 serial fallback。

## Implementation Summary

- `sql/parallel_query/exchange.h`
  - 新增 `PQ_mq_message_header`、`PQ_MQ_MESSAGE_MAGIC`、
    `PQ_MQ_MESSAGE_VERSION`；
  - 新增 `Exchange_nosort::run_synthetic_row_image_smoke()`。
- `sql/parallel_query/exchange.cc`
  - 新增 typed message encode/decode helper；
  - `Exchange_nosort::get_next_from_worker()` 改为解析 header，不再依赖
    `raw_len == 1`；
  - synthetic row stream smoke 改为发送 typed ROW/FINISH；
  - 新增 row image smoke：按 `table->s->reclength` 构造 fixed payload，MQ
    decode 后 copy 到 `table->record[0]` 并调用 `table->set_found_row()`。
- `sql/parallel_query/sql_parallel.h/.cc`
  - 新增 `Gather_operator::run_exchange_row_image_smoke()`。
- `sql/parallel_query/pq_iterator.cc`
  - `PQTableScanIterator::Init()` 中将 V2-6 exchange smoke 升级为 V2-8B
    row-image materialization smoke；
  - 仍在 safe fallback window 内运行，不设置 `PQ_execution_state::EXECUTED`。

## V2-8C Handoff

- worker side 后续只需把 worker-local `TABLE::record[0]` 的
  `table->s->reclength` bytes 作为 ROW payload 发送。
- leader side 后续真实 `Read()` 分支必须在下一次 MQ receive 前 copy payload 到
  leader `table->record[0]`，并更新 row status。
- V2-8C 必须补：
  - typed `PQ_Worker_context` wrapper；
  - worker open context；
  - probe vs execute mode；
  - leader read-view commit point；
  - `blob_fields == 0` execution gate；
  - DOP=1 single range gate。
- DOP>1 range start/end、thread-safe dispatch、no duplicate/no missing 仍留给 V2-8D。

## Validation

最低验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

若只改文档，可不跑 build/MTR，但必须说明未跑原因。

## Acceptance Checklist

- [x] 显式 row message header 已定义；
- [x] ROW/FINISH/ERROR 不再依赖 `raw_len == 1` 猜测；
- [x] fixed record image copy 使用 `table->s->reclength`；
- [x] leader materialize 后更新 `TABLE` row status；
- [x] synthetic row image smoke 覆盖 MQ -> leader record copy；
- [x] 真实 InnoDB worker scan 仍未启用；
- [x] eligible 查询仍安全 fallback，真实 execution counters 不增加；
- [x] README 当前状态已更新。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11
- Agents:
  - `Helmholtz`: Exchange/MQ payload completed
  - `Mill`: Iterator/record buffer boundary completed

## Completion Report

V2-8B 已完成。typed MQ row image protocol 已接入 synthetic smoke，eligible 查询仍
保持 serial fallback，真实 InnoDB worker row scan 未启用。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_exchange_rows_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

结果：

- `mysqld` build 通过；
- `pq_exchange_rows_dop1` targeted MTR 通过；
- 完整 `parallel_query` suite 通过，18 个测试加 `shutdown_report` 共 19 项成功。
