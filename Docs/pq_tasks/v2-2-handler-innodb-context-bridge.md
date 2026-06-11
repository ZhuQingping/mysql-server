# V2-2 Handler/InnoDB Context Bridge 任务书

## Goal

在 V2-1 iterator ownership 已建立后，补齐 SQL PQ 层与 InnoDB PQ skeleton 之间的 context bridge。V2-2 的目标不是跑通真实 row stream，而是让 SQL 层能够安全地探测并持有 InnoDB leader context：

1. DOP=1 leader init/end 可成功闭环；
2. unsupported 场景可以安全 fallback；
3. context ownership 明确；
4. worker/read-view/row stream 的高风险语义继续留到后续阶段。

## Current Baseline

- Latest commit: `9a58ff94cdf Add PQ V2-1 iterator safe fallback`
- `PQTableScanIterator` 已可返回非空，并在 `Init()` 安全窗口串行 fallback。
- InnoDB `ha_innobase::pq_leader_scan_init()` 当前会创建内部 `InnoDB_pq_leader_ctx`，但 `PQ_Leader_context **leader_ctx` 输出仍为 `nullptr`。
- `ha_innobase::pq_worker_scan_init()` 当前也不会向 SQL 层输出真实 `PQ_Worker_context *`。

## Scope

允许：

- 在 `handler` virtual surface 中补充 PQ pull-row 探测 API，默认 unsupported；
- 在 `sql/parallel_query/pq_handler.*` 或 InnoDB adapter 中补充轻量 bridge helper / wrapper；
- 在 `ha_innobase` PQ API 中让 leader context 对 SQL 层可见；
- 在 `PQTableScanIterator::Init()` 中做 DOP=1 leader init/end 探测，然后仍 fallback 到 serial；
- unsupported 路径 fallback 到 serial；fatal/OOM handler error 上抛；
- 加最小 MTR 验证 repeated execution 无泄漏、fallback counter 正确、结果不变。

禁止：

- 启动 worker THD；
- 调用 `pq_worker_scan_next()` 产出真实 row；
- 复用/并发修改 leader THD 的 `row_prebuilt_t` 做 worker 扫描；
- 宣称 DOP>1 正确性；
- 扩大 eligibility 范围；
- 引入完整 `row0pread_pq` fork。

## Design Direction

### Preferred bridge

V2-2 采用通用 `handler` virtual API + InnoDB override，而不是让 SQL 层
downcast 到 `ha_innobase`。原因：

- SQL iterator 只应依赖 handler contract，避免把 InnoDB 类型暴露到 SQL 执行层；
- 现有 `handler::parallel_scan_*` 是上游 push/batch API，不等价于 PQ worker
  pull-row API；
- 默认实现返回 unsupported，非 InnoDB 或未支持场景自然 serial fallback；
- InnoDB override 只提供 DOP=1 leader init/end smoke，worker context 和 row stream
  接入留到后续阶段。

### Minimum contract

建议在 `handler` virtual API 和 InnoDB PQ adapter 中形成以下语义：

- leader init success: SQL 层拿到非空 opaque leader handle；
- unsupported: 返回 `HA_ERR_UNSUPPORTED` 或等价 handler error，SQL iterator fallback serial；
- fatal/OOM: 返回错误，不掩盖为 serial fallback；
- leader end: idempotent，释放内部 InnoDB leader context 和 thread budget；
- V2-2 不要求 SQL 层拿到 worker context。
- SQL-visible leader wrapper 不拥有 InnoDB context；InnoDB handler 成员统一释放
  SQL wrapper、InnoDB ctx 和 thread budget，避免双重释放。

### Iterator integration

`PQTableScanIterator::Init()` 的 V2-2 顺序：

1. 设置 `ITERATOR_SELECTED`；
2. 尝试 DOP=1 leader init；
3. 如果 unsupported，进入 serial fallback；
4. 如果 success，立即 leader end，然后进入 serial fallback；
5. 不启动 worker，不读 row；
6. serial fallback counter 仍只计一次。

这样可以验证 bridge ownership，同时不改变查询结果。

## Allowed Files

- `sql/parallel_query/pq_handler.h`
- `sql/parallel_query/pq_handler.cc`
- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/**`
- `Docs/pq_tasks/v2-2-handler-innodb-context-bridge.md`
- `Docs/pq_tasks/README.md`

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

## Acceptance Checklist

- [x] SQL/PQ 层可以拿到非空 leader context 或明确 unsupported；
- [x] DOP=1 leader init/end 可重复执行且不泄漏；
- [x] unsupported 自动 serial fallback；
- [x] V2-2 不启动 worker、不读取 PQ row；
- [x] `PQTableScanIterator` 仍能安全 serial fallback；
- [x] `mysqld` build 通过；
- [x] 完整 `parallel_query` suite 通过。

## Current Status

- Status: Verified, pending commit
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Parallel Agent Notes

- SQL handler contract explorer 建议新增 `handler::pq_*` virtual API，默认
  `HA_ERR_UNSUPPORTED`，避免 SQL 层依赖 InnoDB downcast。
- InnoDB explorer 确认当前 `parallel_scan_*` push/batch API 不适合直接复用为
  PQ pull-row contract，并指出 `Parallel_reader::available_threads()` 获取预算后
  的失败路径必须显式 release。
- V2-2 收窄为 DOP=1 leader init/end bridge，不启动 worker，不调用
  `pq_worker_scan_next()`，不宣称 DOP>1 正确性。

## Completion Report

### Changed Files

- `sql/handler.h`
- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-2-handler-innodb-context-bridge.md`

### Implementation Summary

- 新增通用 `handler::pq_*` virtual API，默认返回 `HA_ERR_UNSUPPORTED`。
- InnoDB override `pq_leader_scan_init()` 现在返回 SQL-visible
  `PQ_Leader_context` wrapper，并通过 `actual_dop` 输出实际预算。
- 修复 `Parallel_reader::available_threads()` 后的失败路径预算释放：
  budget 不足、OOM、leader init 失败、SQL wrapper OOM 都会 release。
- `PQTableScanIterator::Init()` 在 worker/Exchange/row stream 之前执行
  DOP=1 leader init/end smoke，然后仍走 owned serial iterator fallback。
- fatal/OOM handler error 不被吞为 fallback；只有 `HA_ERR_UNSUPPORTED`
  进入 serial fallback。

### Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback pq_stats pq_fullscan_result
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

结果：

- `mysqld` build 通过。
- targeted MTR: 4/4 pass。
- full `parallel_query` suite: 14/14 pass。

### Remaining Risks

- V2-2 只验证 leader context init/end ownership，不启用 worker。
- `row0pread_pq` 当前仍是单 whole-table range，DOP>1 正确性必须继续 gate。
- worker `PQ_Worker_context` SQL-visible wrapper 尚未建立，留到 V2-3。
