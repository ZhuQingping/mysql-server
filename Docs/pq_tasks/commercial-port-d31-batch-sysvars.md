# D31 parallel_batch_* 变量契约

## 目标

补齐商用 Parallel Query 中 Batch_buffer 相关的两个 session 变量：

- `parallel_batch_max_slot`
- `parallel_batch_max_mem_size`

本任务只做变量契约迁移，不启用或改造当前 MQ / Batch_buffer / Record_buffer
执行路径。

## 商用参考

商用仓 `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore` 中定义：

- `parallel_batch_max_slot`: `ulong`，范围 `0..1024`，默认 `8`
- `parallel_batch_max_mem_size`: `ulong`，范围 `0..ULONG_MAX`，默认 `1024 * 1024`

商用使用点位于 `sql/parallel_query/msg_queue.h` 的
`Batch_buffer_manager` 构造路径。当前分支本小步不接该行为路径。

## 范围

允许修改：

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_batch_sysvars.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_batch_sysvars.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

禁止修改：

- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/exchange.*`
- InnoDB / handler worker scan 路径

## 验证计划

1. RED：新增 `pq_commercial_batch_sysvars` 后先运行 MTR，确认因 unknown system
   variable 失败。
2. GREEN：
   - `cmake --build build-ninja --target mysqld -j 8`
   - `cd build-ninja/mysql-test && ./mtr --suite=parallel_query pq_commercial_batch_sysvars pq_vars`
   - `git diff --check`
3. 独立 review agent 检视代码、测试、文档和商用资料对齐。

## 完成报告

- RED：`pq_commercial_batch_sysvars` 在实现前因
  `Unknown system variable 'parallel_batch_max_slot'` 失败。
- GREEN：`cmake --build build-ninja --target mysqld -j 8` 已通过；
  targeted MTR
  `./mtr --suite=parallel_query pq_commercial_batch_sysvars pq_vars` 已通过；
  `git diff --check` 已通过。
- Review：独立 review agent 已 ACCEPT，无 Critical / Important / Minor
  findings；review agent 也确认未改动 MQ、Batch_buffer、Record_buffer、
  worker、handler 或 InnoDB 执行路径。
