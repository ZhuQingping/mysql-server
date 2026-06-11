# V2-3 Read View / Trx Contract 任务书

## Goal

在 V2-2 已经建立 handler/InnoDB leader context bridge 后，V2-3 的目标是收敛真实 worker row stream 之前最容易出错的 InnoDB 事务边界：

1. leader read view / trx 的创建、持有、释放语义清晰；
2. worker 不并发复用会被 `row_search_mvcc()` 修改的 leader `row_prebuilt_t`；
3. KILL / early abort / unsupported fallback 路径不会泄漏 PQ context 或 thread budget；
4. V2-3 不宣称 DOP>1 或真实 worker row stream 正确性。

## Current Baseline

- Latest commit: `a123cdabe7c Add PQ V2-2 handler context bridge`
- `PQTableScanIterator::Init()` 会执行 DOP=1 leader init/end smoke，然后仍 serial fallback。
- `ha_innobase::pq_leader_scan_init()` 会启动 leader trx、分配 read view、创建 `InnoDB_pq_leader_ctx`，并通过 SQL-visible `PQ_Leader_context` wrapper 返回。
- `ha_innobase::pq_worker_scan_init()` / `pq_worker_scan_next()` 仍保留早期 Phase 6B-2 skeleton，尚未作为 SQL worker row stream 使用。
- `row0pread_pq.cc` 当前仍是 single whole-table range，不具备 DOP>1 正确性。

## Scope

允许：

- 梳理并修正 leader init/end 的 read view / trx side effect 边界；
- 明确 `row_prebuilt_t`、`trx_t`、`m_prebuilt` 在 worker 中的禁止复用规则；
- 给 worker context API 增加 ownership guard，但不启动 worker、不读 row；
- 增加 MTR 验证 repeated execution、RR/RC 可见性在 serial fallback 下不被 leader smoke 破坏；
- 必要时补充 debug/status guard，证明 V2-3 没有启动 worker row stream。

禁止：

- 启动真实 worker THD；
- 调用 `pq_worker_scan_next()` 读取真实 row；
- clone 或并发复用 leader handler 的 `row_prebuilt_t`；
- 打开 DOP>1 真实读取；
- 引入完整 plan clone / Item clone / JOIN clone。

## Design Requirements

- `HA_ERR_UNSUPPORTED` 仍走 serial fallback；fatal/OOM 不掩盖为 fallback。
- leader context 成功后必须总是可通过 `pq_leader_scan_end()` 释放。
- worker row stream 接入前，`pq_worker_scan_init()` 不应暴露一个会被 SQL 层误用的可读 context。
- 如果后续需要 worker read rows，必须先定义独立 worker handler/prebuilt/trx/read-view contract。
- V2-3 的测试只能证明当前 smoke/fallback 不破坏可见性和资源释放，不能作为真实并行读取正确性证据。

## Allowed Files

- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `sql/parallel_query/pq_iterator.*`
- `mysql-test/suite/parallel_query/**`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-3-read-view-trx-contract.md`

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

V2-3 如果新增 read view / trx MTR，则 targeted 命令必须加入对应测试。

## Acceptance Checklist

- [x] read view / trx side effect 已明确记录；
- [x] worker 不安全复用 `m_prebuilt` / leader `trx_t` 的边界已明确；
- [x] unsupported/fatal/cleanup 策略与 V2-2 contract 一致；
- [x] repeated execution 不泄漏 leader context/thread budget；
- [x] RR/RC smoke 不被 leader init/end 破坏；
- [x] 不启动 worker、不读取 PQ row；
- [x] `mysqld` build 通过；
- [x] 完整 `parallel_query` suite 通过。

## Current Status

- Status: Verified, pending commit
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Parallel Agent Notes

- InnoDB trx/read-view explorer 确认 V2-2 的 leader probe 会提前调用
  `trx_start_if_not_started_xa()` / `trx_assign_read_view()`，这不是纯探测；
  V2-3 应避免在 serial fallback 前创建或 pin read view。
- InnoDB explorer 确认 `pq_worker_scan_next()` 通过 leader handler 的
  `m_prebuilt` 调用 `row_search_mvcc()` 不安全；`row_prebuilt_t` 是可变
  cursor/fetch/cache 状态，不能被 worker THD 并发复用。
- Test explorer 建议新增 `pq_read_view_dop1`，验证 RR snapshot 稳定、
  RC statement read view 不被 pin，以及 `executed/workers/rows` 仍为 0。

## Implementation Plan

1. 新增 `pq_read_view_dop1` MTR，覆盖 RR/RC snapshot 与 V2-3 统计边界。
2. 修改 `ha_innobase::pq_leader_scan_init()`：作为 DOP=1 bridge probe 时
   不启动 trx、不分配 read view；真实 PQ row production 后续单独建立
   execution init 边界。
3. 修改 `ha_innobase::pq_worker_scan_init()` / `pq_worker_scan_next()`：
   在独立 worker `row_prebuilt_t` / `trx` / read-view contract 完成前返回
   `HA_ERR_UNSUPPORTED`，不创建可被误用的 worker cursor。
4. 保持 `pq_leader_scan_end()` idempotent cleanup，继续释放 leader ctx、
   SQL wrapper、thread budget 和残留 worker ctx。

## Completion Report

### Changed Files

- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `mysql-test/suite/parallel_query/t/pq_read_view_dop1.test`
- `mysql-test/suite/parallel_query/r/pq_read_view_dop1.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-3-read-view-trx-contract.md`

### Implementation Summary

- `ha_innobase::pq_leader_scan_init()` 不再作为 DOP=1 bridge probe 时启动
  trx 或调用 `trx_assign_read_view()`；serial fallback 的正常 InnoDB read path
  继续负责 statement read view。
- `ha_innobase::pq_worker_scan_init()` / `pq_worker_scan_next()` 在 worker
  独立 mutable scan state 合同完成前明确返回 `HA_ERR_UNSUPPORTED`。
- 新增 `pq_read_view_dop1`，覆盖 RR snapshot 稳定、RC statement read view
  不被 pin，以及 PQ counters 仍显示未真实执行 worker row stream。

### Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_read_view_dop1 pq_iterator_safe_fallback pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

结果：

- `mysqld` build 通过。
- targeted MTR: 4/4 pass。
- full `parallel_query` suite: 15/15 pass。

### Remaining Risks

- worker `PQ_Worker_context` ownership 仍未对 SQL 层开放。
- 真实 worker row read 仍需要独立 handler/prebuilt/trx/read-view contract。
- DOP>1 仍被 V2-4 range partition gate 阻塞。
