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

## Selected Design

V2-8D 选择方案 B：复用 upstream `Parallel_reader` 的 cursor traversal /
visibility 语义，避免直接把 worker handler 的 `row_search_mvcc()` 接入真实 row
read。

理由：

- upstream `Parallel_reader::add_scan(trx, config, callback)` 已把 leader trx 作为
  covering transaction 传入 `Scan_ctx`；
- `Scan_ctx::check_visibility()` 只读取 `trx->read_view`，并用
  `ReadView::changes_visible()` / `row_vers_build_for_consistent_read()` 做可见性
  判断，不触发 `trx_assign_read_view()`；
- `row0pread-adapter.cc` 已有可参考路径：`Parallel_reader` 产出 visible `rec`，
  再用 `row_sel_store_mysql_rec()` 按 `row_prebuilt_t` template 转成 MySQL record；
- 这比让 worker-local handler 直接调用 `row_search_mvcc()` 更可控，因为后者会
  触发 worker trx 的 statement read view 分配，也会修改 handler/prebuilt cursor
  状态。

因此后续 real row read 应拆为：

1. leader 在 EXECUTE commit point 创建并 pin `trx->read_view`；
2. InnoDB PQ worker 不调用 `row_search_mvcc()`；
3. InnoDB PQ worker 使用 `Parallel_reader` range/cursor/visibility adapter 获取
   visible clustered record；
4. worker 使用自己的 `row_prebuilt_t` template 和 blob heap 调用
   `row_sel_store_mysql_rec()` 转成 worker-local `record[0]`；
5. SQL 层按 V2-8B row image protocol copy 到 MQ，leader materialize 到
   leader `table->record[0]`。

## Required Design

V2-8D 必须在编码前确定一种安全方案：

1. leader 在 no-fallback EXECUTE commit point 创建并 pin statement snapshot；
2. worker 不自行调用 `trx_assign_read_view()` 产生独立 snapshot；
3. worker 可见性检查使用 leader-equivalent snapshot；
4. worker 不共享 leader `row_prebuilt_t` / cursor / mutable handler state；
5. post-start error 走 fatal/error path，不 transparent serial fallback。

`row_search_mvcc()` 只能保留为 disabled latent adapter，不作为 V2-8D/V2-8E
真实执行路线。禁止把 `pq_worker_scan_next()` 直接接到该 latent adapter。

## Implementation Tasks

### Task 1: 防误用边界

- 保持 `pq_worker_scan_next()` disabled；
- 在 latent adapter 注释中明确 first-row/read-view gate；
- 对 `row0pread_pq` 首行定位协议做静态修正，避免后续接线继承错误参数。

### Task 2: Read View Contract 设计

- leader `trx->read_view` 必须在 EXECUTE commit point 已 active；
- worker 只读 leader `ReadView`，不修改 leader `trx_t`；
- worker-local handler/prebuilt 仅负责 MySQL record conversion，不负责
  statement snapshot 分配；
- 禁止 worker-local `trx_assign_read_view()` 出现在 real row read 路径；
- 明确 cleanup：normal EOF、error、KILL、worker open 后失败。

### Task 3: Parallel_reader Pull Adapter 设计

- 从 upstream `Parallel_reader::Scan_ctx` 抽出或包裹一个 pull-style cursor；
- 复用 `check_visibility()` 和 `row_vers_build_for_consistent_read()`；
- DOP=1 first gate 只处理 whole clustered scan；
- DOP>1 range end boundary 后续再打开；
- conversion 使用 worker prebuilt template + per-worker blob heap +
  `row_sel_store_mysql_rec()`。

### Task 4: Execute Commit Point 设计

- `PQ_leader_scan_mode::EXECUTE` 的 no-fallback boundary；
- EXECUTE 成功后状态从 candidate/iterator-selected 进入 executed-ready；
- EXECUTE 失败如何返回错误而不是污染 fallback counter。

### Task 5: 验证计划

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
- [x] read-view ownership 方案选型完成：选择 `Parallel_reader` visibility adapter
  路线，不直连 worker `row_search_mvcc()`；
- [ ] EXECUTE commit point 方案完成；
- [ ] 并发可见性 MTR 准备完成；
- [ ] 真实 `pq_worker_scan_next()` 仍保持 disabled，直到上述 gate 完成。

## Current Status

- Status: Design Selected
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

V2-8D 已完成 first-row/read-view 方案选型：后续真实读取不直接调用
worker-local `row_search_mvcc()`，而是复用/抽出 upstream `Parallel_reader` 的
range/cursor/visibility 语义，再用 worker prebuilt template 做 MySQL record
conversion。EXECUTE commit point 和 pull-style adapter 仍需后续小步实现。
