# MDL Pinbox Capacity Test Report

> Date: 2026-06-18
> Branch: `1million_connection`
> Worktree: `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore-mdl-1m-connections`

---

## 1. 缩小 pinbox 的集成复现

为验证线上 `ER_MDL_OUT_OF_RESOURCES` 是否由 MDL LF pinbox 容量触发，曾在独立实验 worktree 中把 pinbox 上限缩小到 1024 和 2048：

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore-mdl-repro
```

该实验没有修改主工作树。测试使用 Unix socket，并让每个 holder 连接执行真实表访问：

```sql
SELECT COUNT(*) FROM mdl_repro.t;
```

没有使用 `SELECT 1`，因为它可能不触发表级 MDL。

### 1.1 1024 实验结果

```text
holder_success=1016
first_holder_failure.stage=connect
first_holder_failure.errno=3985
first_holder_failure.sqlstate=HY000
new_connection_connect=3985 HY000
Threads_connected=1017
Connection_errors_max_connections=0
ER_MDL_OUT_OF_RESOURCES SUM_ERROR_RAISED=3
already_pinned_table_query=OK
preexisting_idle_table_query=OK
```

### 1.2 2048 实验结果

```text
holder_success=2040
first_holder_failure.stage=connect
first_holder_failure.errno=3985
first_holder_failure.sqlstate=HY000
new_connection_connect=3985 HY000
Threads_connected=2041
Connection_errors_max_connections=0
ER_MDL_OUT_OF_RESOURCES SUM_ERROR_RAISED=2
already_pinned_table_query=OK
preexisting_idle_table_query=OK
```

### 1.3 集成复现结论

把上限从 1024 改为 2048 后，首次失败点从 1016 个 holder 连接移动到 2040 个 holder 连接，差值正好为 1024。这强支撑了判断：触发阈值由 MDL LF pinbox 容量控制，而不是由 `max_connections` 控制。

---

## 2. TDD 验证

先修改 gunit，使默认 LF pinbox 必须能够分配超过旧 65535 上限的 pins：

```bash
ninja -C build-1m-mdl mysys_lf-t -j 8
./build-1m-mdl/runtime_output_directory/mysys_lf-t \
  --gtest_filter='Mysys.LFPinboxAllocatesPastOldLimit'
```

在旧默认容量仍为 65535 的实现上，该测试按预期失败：

```text
Expected: (nullptr) != (allocated), actual: (nullptr) vs NULL
allocation 65535
```

随后将 LF pinbox 改为 64-bit index/version 编码，并把固定容量提升到 2M 级别。

---

## 3. 本 MR 构建与单测验证

以下命令已通过：

```bash
ninja -C build-1m-mdl mysys_lf-t mdl-t mysqld -j 8
```

```bash
./build-1m-mdl/runtime_output_directory/mysys_lf-t \
  --gtest_filter='Mysys.LFPinboxAllocatesPastOldLimit'
```

```bash
./build-1m-mdl/runtime_output_directory/mdl-t \
  --gtest_filter='MDLTest.AllocatesPastOldPinboxLimit'
```

覆盖点：

- 默认 LF pinbox 能分配超过旧 65535 可用 pin 上限。
- MDL 全局 map 能分配超过旧 65535 可用 pin 上限。
- 测试结束后显式归还 pins。

---

## 4. 参数解析验证

建议验证：

```bash
./build-1m-mdl/runtime_output_directory/mysqld \
  --no-defaults \
  --max-connections=1000000 \
  --verbose --help
```

应确认：

```text
max-connections                                              1000000
```

本 MR 不再支持或暴露 `mdl_pinbox_max_pins`。

---

## 5. MTR 验证

建议保留基础 smoke test：

```bash
cd build-1m-mdl/mysql-test && ./mtr 1st
```

原先用于验证 `mdl_pinbox_max_pins` sysvar 和 `Mdl_pins_*` status 的 MTR 已删除，因为当前方案不再引入这些变量。

---

## 6. 线上验证 SQL

建议在测试或生产类环境中执行：

```sql
SHOW GLOBAL STATUS LIKE 'Threads_connected';
SHOW GLOBAL STATUS LIKE 'Max_used_connections';
SHOW GLOBAL STATUS LIKE 'Connection_errors_max_connections';

SELECT ERROR_NUMBER, ERROR_NAME, SUM_ERROR_RAISED
  FROM performance_schema.events_errors_summary_global_by_error
 WHERE ERROR_NAME = 'ER_MDL_OUT_OF_RESOURCES'
    OR ERROR_NUMBER = 3985;

SELECT USER, HOST, COUNT(*)
  FROM information_schema.PROCESSLIST
 GROUP BY USER, HOST
 ORDER BY COUNT(*) DESC;
```

如果 PFS 错误统计不可用，可以使用 error log、客户端错误计数和压测脚本统计替代。
