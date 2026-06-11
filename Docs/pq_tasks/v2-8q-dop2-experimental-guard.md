# V2-8Q DOP2 Experimental Guard 任务书

## 目标

V2-8Q 目标是在进入 DOP>1 range correctness 前，确认默认 OFF 的 experimental threaded gate 不会误打开 DOP>1 真实执行。

即使显式设置：

- `parallel_query=ON`
- `parallel_query_experimental_threaded_dop1=ON`
- `parallel_default_dop=2`
- `parallel_cost_threshold=0`

查询也必须继续 serial fallback，而不是启动真实 threaded row stream。

## 范围

允许修改：

- `mysql-test/suite/parallel_query/t/pq_read_threaded_dop2_guard.test`
- `mysql-test/suite/parallel_query/r/pq_read_threaded_dop2_guard.result`
- `Docs/pq_tasks/README.md`
- 本任务文档

禁止修改：

- 放开 `should_enter_threaded_read_shadow_path()` 的 `requested_dop == 1` gate；
- 打开 InnoDB DOP>1；
- 修改 worker DOP=1 gate；
- 修改默认实验变量。

## 验证

新增 `pq_read_threaded_dop2_guard`：

- 查询结果应与串行一致；
- `executed_delta=0`
- `fallback_delta=1`
- `rows_delta=0`
- `workers_delta=0`

已执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_guard \
  --parallel=1 --vardir=/tmp/pqv_dop2guard --tmpdir=/tmp/pqt_dop2guard
```

待提交前执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## 完成报告

实现内容：

- 新增 no-debug MTR `pq_read_threaded_dop2_guard`。
- 验证 experimental threaded gate 只允许 DOP=1，不会误启 DOP=2。

风险点：

- 该测试只验证 guard，不验证 DOP>1 正确性。
- 后续 DOP>1 range correctness 仍需独立任务拆分。

当前状态：Completed，完整 suite 已通过，等待提交。
