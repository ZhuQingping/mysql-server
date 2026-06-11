# V2-9A Range Dispatch Contract 任务书

## 目标

V2-9A 目标是建立 worker range dispatch 的可观测 contract，但不打开 DOP>1 row stream。

本阶段只做：

- `pq_worker_scan_init()` 从 leader ctx 领取一个 `InnoDB_pq_range`；
- worker ctx 保存 assigned range；
- 新增 status counters 观测 range dispatch；
- DOP>1 gate 保持关闭。

## 改动

新增 status：

- `Parallel_ranges_dispatched`
- `Parallel_empty_worker_ranges`

行为：

- DOP=1 当前路径中，worker scan init 会调用 `dispatch_next_range()`；
- 领到 range 时 `Parallel_ranges_dispatched++`；
- 没有 range 时 `Parallel_empty_worker_ranges++`；
- `pq_worker_scan_init()` 仍拒绝 `actual_dop != 1`；
- `pq_worker_scan_next()` 继续 disabled；
- callback producer 仍使用当前 full-range producer，V2-9B 再改成 range-aware producer。

## 验证

新增 `pq_range_dispatch_dop1`：

- 使用 `parallel_query_experimental_threaded_dop1=ON`；
- 普通 projection/WHERE 查询触发 threaded DOP=1 row stream；
- 预期结果正确；
- `ranges_built_delta=2`
- `ranges_dispatched_delta=2`
- `empty_worker_ranges_delta=0`
- `executed_delta=1`
- `workers_delta=1`

说明：`ranges_built_delta=2` / `ranges_dispatched_delta=2` 来自同一查询中的 safe-window producer smoke 和真实 threaded producer 各初始化一次 worker scan。V2-9A 的语义是 worker-scan-init 层总 dispatch 计数，不是单条最终执行 worker 数。

## 验证命令

已执行：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_range_dispatch_dop1 \
  --parallel=1 --vardir=/tmp/pqv_dispatch --tmpdir=/tmp/pqt_dispatch
```

待提交前执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## 风险

- 这不是 DOP>1 range correctness；
- 当前 callback producer 仍未按 assigned range 限定扫描范围；
- V2-9B 必须把 `produce_callback_rows()` 改为 range-aware 后，才能考虑放开 DOP>1 gate。

当前状态：Completed，完整 suite 已通过，等待提交。
