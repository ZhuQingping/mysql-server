# V2-8D First Row / Read View Contract 任务书

## Goal

V2-8D 的目标是收敛真实 worker row read 前最后两个硬边界：

1. `row_search_mvcc()` 首行定位必须等价 `ha_innobase::index_first()`；
2. worker 读取必须使用与 leader statement 一致的 snapshot，不能让每个 worker
   自己创建独立 read view，也不能让 worker 线程直接修改 leader `trx_t`。

本阶段默认不打开真实 PQ row stream。只有在上述契约被代码和测试同时证明后，才允许
进入后续 real DOP=1 full scan。

## Current Baseline

- V2-8C 已完成 worker THD/TABLE open smoke 和 handler init/end smoke。
- `pq_worker_scan_next()` 仍返回 unsupported，真实 row read 未打开。
- `row0pread_pq` 中 latent pull-row adapter 已修正为 `index_first()` 等价定位协议，
  但仍必须保持未接线状态，直到 read-view contract 完成。

## Read-Only Findings

### First Row Positioning

真实 `index_first()` 路径：

1. `ha_innobase::index_first(buf)`
2. `index_read(buf, nullptr, 0, HA_READ_AFTER_KEY)`
3. `convert_search_mode_to_innobase(HA_READ_AFTER_KEY)` -> `PAGE_CUR_G`
4. `key_ptr == nullptr` 时 `search_tuple` 字段数为 0
5. `match_mode == 0`
6. `row_search_mvcc(buf, PAGE_CUR_G, m_prebuilt, 0, 0)`

因此 PQ worker whole clustered scan 首行必须使用：

```cpp
dtuple_set_n_fields(prebuilt->search_tuple, 0);
row_search_mvcc(record, PAGE_CUR_G, prebuilt, 0, 0);
```

后续行才允许使用：

```cpp
row_search_mvcc(record, PAGE_CUR_UNSUPP, prebuilt, 0, ROW_SEL_NEXT);
```

`PAGE_CUR_UNSUPP + direction=0` 不能用于首行定位，因为它依赖 cursor 已经被
`index_read()` 定位过。

### Read View / Trx

- `row_search_mvcc()` 在 `prebuilt->sql_stat_start == true` 且
  `select_lock_type == LOCK_NONE` 时通过 `trx_assign_read_view(trx)` 创建或复用
  read view。
- worker 当前通过 `create_internal_thd()` 和 `open_ltable()` 打开独立 TABLE；
  该路径会绑定 worker 自己的 InnoDB `trx_t`。
- 如果 worker 直接调用标准 `row_search_mvcc()`，worker 首次读取会按自己的
  `trx_t` 创建 read view，快照时间可能不同于 leader 和其他 worker。
- 直接把 worker `prebuilt->trx` 指向 leader `trx_t` 也不安全；InnoDB 代码中存在
  当前线程可处理该 trx 的假设和断言，worker 线程不能随意修改 leader trx state。

## Required Design

V2-8D 必须在编码前确定一种安全方案：

1. leader 在 no-fallback EXECUTE commit point 创建并 pin statement snapshot；
2. worker 不自行调用 `trx_assign_read_view()` 产生独立 snapshot；
3. worker 可见性检查使用 leader-equivalent snapshot；
4. worker 不共享 leader `row_prebuilt_t` / cursor / mutable handler state；
5. post-start error 走 fatal/error path，不 transparent serial fallback。

可选方向：

- 方案 A：设计 worker-local trx 绑定 leader-equivalent read view 的生命周期，并证明
  `row_search_mvcc()` 可安全使用；
- 方案 B：绕开标准 `row_search_mvcc()` 的 trx/read-view 分配路径，使用受控 cursor
  和显式 `ReadView*` 可见性检查 adapter。

在方案 A/B 证明前，禁止把 `pq_worker_scan_next()` 接入真实 `Read()`。

## Implementation Tasks

### Task 1: 防误用边界

- 保持 `pq_worker_scan_next()` disabled；
- 在 latent adapter 注释中明确 first-row/read-view gate；
- 对 `row0pread_pq` 首行定位协议做静态修正，避免后续接线继承错误参数。

### Task 2: Read View Contract 设计

- 调研 `ReadView` ownership、clone/pin 可行性、RC/RR 语义；
- 明确 worker-local trx 是否允许引用 leader snapshot；
- 明确 cleanup：normal EOF、error、KILL、worker open 后失败。

### Task 3: Execute Commit Point 设计

- `PQ_leader_scan_mode::EXECUTE` 的 no-fallback boundary；
- EXECUTE 成功后状态从 candidate/iterator-selected 进入 executed-ready；
- EXECUTE 失败如何返回错误而不是污染 fallback counter。

### Task 4: 验证计划

新增或准备 MTR：

- `pq_readview_concurrent_insert_dop1`
- `pq_readview_concurrent_update_dop1`
- `pq_readview_concurrent_delete_dop1`
- `pq_worker_first_row_empty_single_many`

在真实 row read 未打开前，上述测试只能作为任务项，不应加入必跑 suite。

## Allowed Files

- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/handler/ha_innodb.h`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8c-dop1-real-fullscan.md`
- `Docs/pq_tasks/v2-8d-first-row-read-view-contract.md`

## Forbidden Files

- optimizer eligibility expansion；
- worker-side Item/JOIN clone；
- DOP>1 real execution；
- GROUP BY/ORDER BY/JOIN/secondary index/ICP/partition real execution；
- any change that makes `pq_worker_scan_next()` return real rows before
  read-view ownership is proven。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [x] first-row 定位协议修正为 `index_first()` 等价参数；
- [x] latent adapter 注释不再宣称 worker 可以直接使用 leader trx；
- [ ] read-view ownership 方案选型完成；
- [ ] EXECUTE commit point 方案完成；
- [ ] 并发可见性 MTR 准备完成；
- [ ] 真实 `pq_worker_scan_next()` 仍保持 disabled，直到上述 gate 完成。

## Current Status

- Status: In Progress
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

待 read-view contract 和 execute commit point 设计完成后补充。
