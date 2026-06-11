# V2-9C Debug DOP2 Threaded Shadow 任务书

## 目标

V2-9C 目标是在 debug-only 条件下打开 DOP=2 threaded shadow path，验证多 worker 启动、range dispatch、empty worker FINISH 和 leader Exchange 消费闭环。

本阶段仍不改变默认行为：

- `parallel_query_experimental_threaded_dop1` 仍只允许 DOP=1；
- DOP=2 只通过 `SET SESSION debug="d,pq_read_threaded_dop2_shadow_path"` 打开；
- 不新增正式 DOP>1 实验变量；
- `pq_worker_scan_next()` 继续 disabled。

## 改动

- `PQTableScanIterator::should_enter_threaded_read_shadow_path()` 新增 debug flag `pq_read_threaded_dop2_shadow_path`；
- threaded EXECUTE path 使用 requested DOP 创建 leader ctx、Gather 和 worker open contexts；
- `Gather_operator::run_worker_callback_threaded_producer()` 支持配置并启动所有 worker；
- `PQ_worker_manager` 原有 join/cleanup 逻辑继续复用；
- `InnoDB_pq_leader_ctx::dispatch_next_range()` 改为 atomic fetch，支持多 worker 并发领取 range；
- `pq_worker_scan_init()` 允许 `actual_dop > 1`，但只有 debug shadow path 会进入。

## 验证

新增测试：

`pq_read_threaded_dop2_shadow`：

- `parallel_default_dop=2`
- debug flag `pq_read_threaded_dop2_shadow_path`
- `SELECT id, val FROM t1`
- 预期：
  - `executed_delta=1`
  - `fallback_delta=0`
  - `rows_delta=5`
  - `workers_delta=2`
  - `ranges_dispatched >= 1`
  - `empty_worker_seen >= 1`

`pq_read_threaded_dop2_worker_error`：

- `parallel_default_dop=2`
- debug flag `pq_read_threaded_dop2_shadow_path,pq_read_threaded_shadow_force_worker_error`
- 预期 worker 0 注入 ERROR，leader 返回 `ER_GET_ERRNO`
- 预期：
  - `executed_delta=0`
  - `fallback_delta=0`
  - `rows_delta=0`
  - `workers_delta=2`

`pq_read_threaded_dop2_external_kill`：

- `parallel_default_dop=2`
- debug flag `pq_read_threaded_dop2_shadow_path`
- `DEBUG_SYNC` 在 worker 启动后外部 `KILL QUERY`
- 预期：
  - `ER_QUERY_INTERRUPTED`
  - `executed_delta=0`
  - `fallback_delta=0`
  - `workers_delta=2`
  - kill 后同表串行 `COUNT(*)` 正常

当前 range planning 在小表上通常只有 1 个 range，因此 DOP=2 下一个 worker 读完整 range，另一个 worker empty FINISH。该测试证明多 worker lifecycle/Exchange/EOF 可以闭环，但不宣称已经有多 range 性能收益。

## 验证结果

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_shadow \
  --parallel=1 --vardir=/tmp/pqv_dop2shadow --tmpdir=/tmp/pqt_dop2shadow
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

结果：

- `pq_read_threaded_dop2_shadow` 单测通过；
- `pq_read_threaded_dop2_worker_error` / `pq_read_threaded_dop2_external_kill` 单测通过；
- `pq_read_threaded_dop2_shadow` / `pq_read_threaded_dop2_guard` / `pq_read_threaded_projection_where` / `pq_range_dispatch_dop1` 相关回归组通过；
- 完整 `parallel_query` suite 通过，共 32 项。

## 风险

- 小表只有单 range，尚未证明多个非空 range 的无重无漏；
- DOP=2 external KILL/worker ERROR 已有 debug-only 回归覆盖；
- 正式 DOP>1 实验变量必须等多 range correctness 测试通过后再加。

当前状态：Completed，完整 suite 已通过，等待提交。
