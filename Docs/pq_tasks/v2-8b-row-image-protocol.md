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

## Validation

最低验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

若只改文档，可不跑 build/MTR，但必须说明未跑原因。

## Acceptance Checklist

- [ ] 显式 row message header 已定义；
- [ ] ROW/FINISH/ERROR 不再依赖 `raw_len == 1` 猜测；
- [ ] fixed record image copy 使用 `table->s->reclength`；
- [ ] leader materialize 后更新 `TABLE` row status；
- [ ] synthetic row image smoke 覆盖 MQ -> leader record copy；
- [ ] 真实 InnoDB worker scan 仍未启用；
- [ ] eligible 查询仍安全 fallback，真实 execution counters 不增加；
- [ ] README 当前状态已更新。

## Current Status

- Status: In Progress
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Active agents:
  - `Helmholtz`: Exchange/MQ payload
  - `Mill`: Iterator/record buffer boundary

## Completion Report

待实现后补充。
