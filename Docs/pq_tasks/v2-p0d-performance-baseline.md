# V2 P0-D Performance Baseline

## 目标

建立可复现的 Parallel Query 性能基线，用于观察当前 threaded full scan row stream 在 serial、DOP=1、DOP=2、DOP=4 下的趋势。

本任务不改变执行路径，不把性能阈值写入 MTR。debug build 或 macOS 本机结果只用于趋势参考，不能作为最终性能承诺。

## 范围

覆盖：

- clustered index full scan；
- implicit aggregate：`COUNT(*)` / `SUM(id)`；
- `WHERE pad='a'` 保持与现有 threaded row stream MTR 一致；
- serial、DOP=1、DOP=2、DOP=4 四组；
- status delta：`Parallel_queries_executed`、`Parallel_queries_fallback`、`Parallel_rows_scanned`、`Parallel_workers_launched`。

不覆盖：

- GROUP BY partial aggregation；
- ORDER BY / Gather Merge；
- secondary index / ICP；
- partition table；
- 多连接并发压测；
- release build 最终性能定论。

## 基线脚本

脚本：

- [pq_perf_baseline.sql](pq_perf_baseline.sql)

建议命令：

```bash
{ echo "SET @pq_perf_rows=1048576; SET @pq_perf_repeat=3;"; \
  cat Docs/pq_tasks/pq_perf_baseline.sql; } | \
  mysql --socket=/path/to/mysql.sock -uroot test
```

参数：

- `@pq_perf_rows`：目标行数，默认 `1048576`；
- `@pq_perf_repeat`：每种模式重复次数，默认 `3`。

## 模式定义

| Mode | Session Variables | 预期 |
|------|-------------------|------|
| serial | `parallel_query=OFF` | 不增加 PQ executed / workers |
| dop1 | `parallel_query=ON`, `parallel_default_dop=1`, `parallel_query_experimental_threaded_dop1=ON` | executed 增加，workers=1 |
| dop2 | `parallel_query=ON`, `parallel_default_dop=2`, `parallel_query_experimental_threaded_dop=ON` | executed 增加，workers=2 |
| dop4 | `parallel_query=ON`, `parallel_default_dop=4`, `parallel_query_experimental_threaded_dop4=ON` | executed 增加，workers=4 |

## 采样要求

每次采样至少记录：

- commit hash；
- build type：debug / release；
- OS / CPU / core count；
- MySQL 启动参数；
- `innodb_parallel_read_threads`；
- table rows；
- repeat count；
- `pq_perf_runs` 输出。

建议将一次采样保存为本地日志，例如：

```text
Docs/pq_tasks/pq_perf_baseline.<date>.<host>.log
```

性能日志默认不提交，除非需要形成正式评审材料。

## 验收标准

- 基线脚本可在已启动 mysqld 上完成建表、填充、采样和清理；
- 每种模式至少输出 `@pq_perf_repeat` 行；
- DOP=1/2/4 的 `fallback_delta=0`；
- DOP=1/2/4 的 `workers_delta` 与 DOP 一致；
- serial 模式不增加 `Parallel_queries_executed`；
- 文档明确 debug/macOS 结果不能作为最终性能结论。

## 当前状态

状态：Ready for baseline run。

已完成：

- 定义 P0-D 性能基线范围；
- 新增 SQL 基线脚本；
- 明确 serial/DOP1/DOP2/DOP4 对比方法；
- 明确 status delta 验收项；
- 新增 `pq_perf_baseline_contract`，只验证 counter contract，不做 timing assertion。

待执行：

- 在 release build 或目标环境采样；
- 根据采样结果决定是否继续优化 worker wait、MQ wait 或 range dispatch。
