# M11-B Worker Result / Query_result_mq Adapter Taskbook

## 状态

M11-B0 design-only completed / Docs-Design Review accepted。
M11-B1/B2 PQWR leader decode adapter coding/validation completed /
Code-Docs-Test Review accepted。
M11-B3a guarded worker-result wiring probe design completed /
Docs-Design Review accepted。

## 目标

为商用 worker result path 平移建立 `Query_result_mq` / leader adapter
合同。M11-B0 不改源码；B1/B2 才引入 `PQWR` leader decode adapter 与
synthetic smoke。真实 worker execution 接线延后到 B3。

## 当前事实

当前仓：

- 已有独立 `PQWR` worker-result frame；
- `Query_result_mq::send_data()` 可发送 controlled ROW frame；
- `Query_result_mq::send_eof()` 可发送 FINISH / synthetic ERROR frame；
- `pq_commercial_worker_result` 验证 wire smoke 和 send_data smoke；
- 当前 smoke 不创建 cloned JOIN、不启动 worker、不读 InnoDB；
- `Exchange_nosort` 主要消费当前 typed row-image / partial-group payload，
  还没有 `PQWR` ROW frame 到 leader record/tmp row 的 adapter。

商用仓：

- worker result 以 `Field_raw_data` / NULL bitmap / CONST bitmap 为主；
- `Exchange` 通过 `convert_mq_data_to_record()` 将 worker payload 重建到
  leader record；
- `make_pq_worker_plan()` 会 clone JOIN、创建 `Query_result_mq` 并挂入
  worker `query_result()`；
- stable output / COUNT DISTINCT / Batch_buffer spill 等能力耦合更深。

结论：当前分支不应直接迁移商用 `Field_raw_data` / Batch_buffer。M11-B
先以当前 `PQWR` 为 adapter shell，打通 leader decode，再评估真实
worker result 接线。

## M11-B0 设计输出

本任务书即 M11-B0 的设计输出，必须被 review 后提交。后续编码任务按
本文件拆分执行。

## 支持边界

M11-B1/B2 初始只允许：

- synthetic `PQWR` ROW / FINISH / ERROR；
- fixed length-prefixed string payload decode；
- explicit field count / NULL bitmap length validation；
- leader-side decode helper，不写入真实 SQL 用户结果；
- MTR 只验证 adapter counters / decoded values / no worker launch。

M11-B1/B2 不支持：

- BLOB/TEXT/JSON/GEOMETRY；
- decimal/time/binary charset-sensitive materialization；
- aggregation / COUNT DISTINCT；
- ORDER BY merge；
- stable output rowid；
- true worker JOIN execution；
- `Field_raw_data` / Batch_buffer spill。

## 后续任务拆分

### M11-B1: PQWR Leader Decode Adapter Skeleton

目标：

- 新增 leader 端 `PQWR` ROW decode helper；
- helper 将 payload 解成字段数组或等价只读 view；
- 保持 compile-only / smoke-only，不接真实执行；
- ERROR payload 暂保持 synthetic string，但必须显式标注非最终协议。

允许文件：

- `sql/parallel_query/query_result_mq.h`；
- `sql/parallel_query/query_result_mq.cc`；
- `sql/parallel_query/exchange.h`；
- `sql/parallel_query/exchange.cc`；
- `sql/parallel_query/sql_parallel.h` / `.cc` only for adding the local smoke
  wrapper；
- `sql/parallel_query/pq_iterator.cc` only for wiring the local smoke into the
  existing guarded `PQTableScanIterator::Init()` smoke chain；
- `Docs/pq_tasks/commercial-port-m11-worker-result-path.md`。

禁止：

- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_select.cc`；
- `sql/sql_lex.h`；
- `sql/sql_class.h`；
- `sql/join_optimizer/access_path.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/parallel_query/pq_iterators.*`；
- any `pq_iterator.*` change outside the existing guarded
  `PQTableScanIterator::Init()` smoke chain；
- any `sql_parallel.*` change outside a local smoke wrapper；

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_worker_result pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11b_base --tmpdir=/tmp/pqt_m11b_base
```

### M11-B2: Synthetic Worker-result Adapter MTR

目标：

- 通过 synthetic `PQWR` ROW + FINISH 验证 leader adapter；
- 只验证 decode / counters / no worker launch；
- 不要求 SQL 用户结果来自 worker。

允许文件追加：

- `mysql-test/suite/parallel_query/t/pq_commercial_worker_result_adapter.test`；
- `mysql-test/suite/parallel_query/r/pq_commercial_worker_result_adapter.result`；
- 必要时 `mysql-test/suite/parallel_query/r/pq_stats.result`。

验证：

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_worker_result_adapter \
  --parallel=1 --vardir=/tmp/pqv_m11b_adapter --tmpdir=/tmp/pqt_m11b_adapter

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11b_full --tmpdir=/tmp/pqt_m11b_full
```

## M11-B1/B2 Implementation Notes

Status: coding/validation completed / Code-Docs-Test Review accepted。

Changed scope:

- `sql/parallel_query/query_result_mq.h`
- `sql/parallel_query/query_result_mq.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_worker_result_adapter.test`

Implementation:

- added `PQ_worker_result_decoded_field` and `pq_decode_worker_result_row()`；
- added local `pq_run_query_result_mq_adapter_smoke()`；
- wired the adapter smoke after existing M4a/M4b Query_result_mq smokes in the
  guarded `PQTableScanIterator::Init()` smoke chain；
- reused existing `Parallel_worker_result_smoke_rows/finishes/errors`
  counters；
- did not add new status variables；
- the adapter smoke itself uses only local MQ and does not start workers,
  attach `Query_result_mq` to real worker execution, create cloned JOIN,
  modify AccessPath, or touch handler/InnoDB；
- the surrounding guarded `PQTableScanIterator::Init()` smoke chain still runs
  its pre-existing worker lifecycle smoke before this adapter smoke。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_commercial_worker_result_adapter \
  --parallel=1 --vardir=/tmp/pqv_m11b_record --tmpdir=/tmp/pqt_m11b_record

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_worker_result_adapter \
  --parallel=1 --vardir=/tmp/pqv_m11b_replay --tmpdir=/tmp/pqt_m11b_replay

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_worker_result \
  pq_commercial_worker_result_adapter pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11b_target --tmpdir=/tmp/pqt_m11b_target

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11b_full --tmpdir=/tmp/pqt_m11b_full
```

Result:

- `mysqld` build passed；
- `--record` generated `.result` but reported the known copy errno 1 at the
  end；
- targeted replay passed；
- targeted set passed: 4/4；
- full `parallel_query` suite passed: 80/80；
- no server restarts or reinitialization。

### M11-B3: Guarded Worker-result Wiring Probe

目标：

- 只在 debug/smoke gate 下，把受控 worker callback output 改走
  `Query_result_mq`；
- leader 使用 B1/B2 adapter 消费；
- 仍不接 commercial cloned JOIN；
- 不改 AccessPath，不打开默认用户可见路径。

Design scope:

- B3 is a guarded wiring probe, not a commercial worker JOIN execution path；
- input side must be an existing controlled smoke producer, not
  `pq_make_join()` / cloned JOIN；
- transport must be `Query_result_mq` / `PQWR`；
- leader side must reuse B1/B2 decode adapter；
- output is observed through counters / smoke assertions only, not returned as
  SQL user result；
- default user-visible execution remains serial/fallback。

Suggested split:

#### M11-B3a: Design-only Wiring Contract

目标：

- define the exact smoke producer and consumer boundary；
- choose whether to reuse an existing callback producer or add a local synthetic
  `Query_result_mq` wiring helper；
- define counters and MTR assertions；
- forbid AccessPath / cloned JOIN / InnoDB path changes。

允许文件：

- `Docs/pq_tasks/commercial-port-m11-worker-result-path.md`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-gap-analysis.md`。

Status: design completed / Docs-Design Review accepted。

#### M11-B3b: Local Query_result_mq Wiring Helper

目标：

- add a local helper that constructs `Query_result_mq` with a local
  `MQueue_handle`；
- feed controlled `Item` values through `Query_result_mq::send_data()` and
  `send_eof()`；
- leader decodes the received `PQWR` ROW with `pq_decode_worker_result_row()`；
- validate decoded values and FINISH；
- do not start worker thread, do not attach to cloned JOIN, do not touch
  handler/InnoDB。

允许文件：

- `sql/parallel_query/query_result_mq.h`；
- `sql/parallel_query/query_result_mq.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterator.cc` only inside existing guarded smoke chain；
- `mysql-test/suite/parallel_query/t/pq_commercial_worker_result_adapter.test`；
- `mysql-test/suite/parallel_query/r/pq_commercial_worker_result_adapter.result`；
- this taskbook。

禁止：

- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_select.cc`；
- `sql/sql_lex.h`；
- `sql/sql_class.h`；
- `sql/join_optimizer/access_path.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/parallel_query/pq_iterators.*`；
- real worker thread launch；
- `pq_make_join()` positive path；
- returning decoded `PQWR` data as user SQL result。

Counters / assertions:

- reuse `Parallel_worker_result_smoke_rows` and
  `Parallel_worker_result_smoke_finishes` only if the helper successfully
  decodes the local `PQWR` frames；
- `Parallel_workers_launched` delta must stay 0 for the local wiring helper；
- do not increment `Parallel_queries_executed`；
- targeted MTR should distinguish B3b from B1/B2 by checking a strictly larger
  rows/finishes delta or an additional decoded-value marker in result output。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_worker_result_adapter pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11b3_target --tmpdir=/tmp/pqt_m11b3_target

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11b3_full --tmpdir=/tmp/pqt_m11b3_full
```

Status: coding/validation completed / Code-Docs-Test Review accepted。

Implementation:

- added a local `pq_run_query_result_mq_wiring_smoke()` helper；
- constructs local `MQueue` / `MQueue_handle` / `Query_result_mq` only；
- sends two controlled rows through `Query_result_mq::send_data()` and one
  FINISH through `send_eof()`；
- leader reads the local `PQWR` frames and validates decoded values with
  `pq_decode_worker_result_row()`；
- restores the leader `sent_row_count` after the local send path；
- increments only `Parallel_worker_result_smoke_rows` and
  `Parallel_worker_result_smoke_finishes` after successful decode；
- fails closed if ROW appears after FINISH, if FINISH appears before the two
  expected ROW frames, or if FINISH is duplicated；
- does not start worker threads, attach cloned JOIN, modify AccessPath,
  touch handler/InnoDB, or return decoded `PQWR` data as user SQL result。

Validation result:

- `mysqld` build passed；
- `pq_commercial_worker_result_adapter pq_commercial_worker_result pq_stats`
  targeted MTR passed: 4/4；
- full `parallel_query` suite passed: 80/80；
- no server restarts or reinitialization in the full suite。
- after Code-Docs-Test Review REVISE, frame-order fail-closed check was added；
- post-revision `mysqld` build passed；
- post-revision targeted MTR passed: 4/4。

Review:

- Code-Docs-Test Review first returned `REVISE` because the helper did not
  require FINISH after exactly two ROW frames；
- the helper now rejects ROW after FINISH, FINISH before two ROW frames, and
  duplicate FINISH in the expected three-frame smoke；
- re-review returned `ACCEPT`；
- remaining non-blocking risk: this local smoke validates the expected three
  frames only and does not drain for tail frames after a correct sequence。

#### M11-B3c: Worker-thread Guarded Probe

Only after B3b review is accepted, evaluate a debug-only worker-thread probe
that writes `PQWR` through `Query_result_mq`. This must still avoid cloned JOIN
and must have a separate design/review step before coding.

Status: design completed / Docs-Design Review accepted。

Review:

- Docs-Design Review returned `ACCEPT`；
- B3b is limited to local MQ + `Query_result_mq` + `PQWR` decode；
- B3b must not start worker threads, attach cloned JOIN, modify
  AccessPath/handler/InnoDB, or return decoded data as SQL user result；
- B3c remains a separate debug-only worker-thread probe that requires its own
  design/review before coding。

## 风险

- `PQWR` length-prefixed string payload 不等价于商用 `Field_raw_data`；
- 真实 materialization 需要类型、charset、NULL、decimal/time/binary 处理；
- ERROR frame 需要 errno/sqlstate/message，当前只有 synthetic string；
- `Exchange` 旧 typed row-image 和 `PQWR` 双协议必须显式路由；
- stable output rowid 仍缺失；
- 直接搬 Batch_buffer 会扩大 MQ、内存所有权和 spill 风险。

## Review 要求

M11-B0 只接受 docs/design review。B1/B2/B3 每个源码或测试子任务必须单独
启动 Code-Docs-Test Review Agent，review accepted 后才提交。

## Review

Docs-Design Review Agent accepted this B0 taskbook after confirming it keeps
`PQWR` as the near-term adapter shell, does not directly migrate
`Field_raw_data` / Batch_buffer, and does not attach real worker JOIN execution.

Code-Docs-Test Review Agent first returned REVISE for B1/B2 because the
taskbook allowed/forbidden file boundaries did not mention the local
`sql_parallel.*` wrapper and guarded `pq_iterator.cc` smoke-chain hook, the
decoded field pointer lifetime was implicit, and the documentation could be
read as saying the whole guarded smoke chain never starts workers. The follow-up
revision narrowed those statements: adapter smoke itself uses local MQ only,
while the surrounding `PQTableScanIterator::Init()` guarded smoke chain still
runs pre-existing worker lifecycle smoke. Re-review returned ACCEPT.
