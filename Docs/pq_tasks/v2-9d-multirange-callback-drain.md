# V2-9D Multi-range Callback Drain 任务书

## 目标

V2-9D 目标是修复 DOP=2 threaded shadow path 在 `ranges > workers` 时只读取首批 range 的缺口。

修复后，每个 worker 完成当前 assigned range 后，会继续从 leader ctx 原子领取下一个 range，直到全部 range 被领取完成。

本阶段仍不改变默认行为：

- DOP=2 只通过 debug flag `pq_read_threaded_dop2_shadow_path` 打开；
- 不新增正式 DOP>1 实验变量；
- 不启用 `pq_worker_scan_next()`；
- 不引入 partial aggregation、ORDER BY、secondary index/ICP 或 partition table。

## 改动

- `ha_innobase::pq_worker_scan_callback_produce()` 从“生产当前 assigned range”扩展为“循环领取并生产剩余 ranges”；
- 每次额外领取 range 时递增 `Parallel_ranges_dispatched`；
- `InnoDB_pq_worker_ctx::init()` 支持重新绑定 range 前释放旧 cursor ctx，避免后续扩展 pull path 时留下生命周期隐患。

## 验证

新增 `pq_read_threaded_dop2_multirange`：

- 1024 行 InnoDB 表，`pad` 非索引过滤强制 full scan；
- `parallel_default_dop=2`；
- debug flag `pq_read_threaded_dop2_shadow_path`；
- 输出 `SELECT id FROM t1 WHERE pad='a'`，用 `--sorted_result` 验证完整 1..1024 集合；
- 预期：
  - `executed_delta=1`
  - `fallback_delta=0`
  - `rows_delta=1024`
  - `workers_delta=2`
  - `ranges_built_ge_2=1`
  - `ranges_dispatched_ge_2=1`
  - `empty_worker_ranges_delta=0`

验证命令：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_multirange \
  --parallel=1 --vardir=/tmp/pqv_dop2_multirange4 --tmpdir=/tmp/pqt_dop2_multirange4
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full4 --tmpdir=/tmp/pqt_full4
```

结果：

- `mysqld` build 通过；
- `pq_read_threaded_dop2_multirange` 单测通过；
- 完整 `parallel_query` suite 通过，共 33 项。

## 风险

- 当前仍是 debug-only DOP2 shadow path；
- 多 range 正确性已覆盖 clustered full scan + leader-side WHERE + projection，不覆盖 ORDER BY 或显式 GROUP BY；
- 正式 DOP>1 实验变量应在独立阶段打开，继续保持默认 OFF。

当前状态：Completed，等待提交。
