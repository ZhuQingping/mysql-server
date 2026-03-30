# Slow Log InnoDB IO 低层设计

## 概述

本文档在高层设计基础上进一步细化 MySQL 8.0.41 上的具体实现方式。
实现目标是在默认关闭、低侵入、低风险前提下，为 slow log 增加语句级
InnoDB 底层存储读统计，这里的统计对象限定为：

- 由 buffer pool miss 触发的底层存储读

## 实现范围

第一版包含：

- 新增全局系统变量 `log_slow_innodb_io`
- 在 `THD` 上增加语句级聚合状态
- 在 InnoDB 同步读与等待路径上进行上报
- 在 FILE slow log 中增加一行扩展输出
- 增加 MTR 用例覆盖：
  - 功能关闭
  - 功能开启且有正值统计
  - 功能开启但输出 0 值统计
  - sysvar 行为和 warning

第一版不包含：

- 扩展 `mysql.slow_log` 表结构
- buffer pool 逻辑读次数统计
- lock wait / queue wait 指标
- 更通用的 slow log verbosity 框架整合

## 输出格式

第一版实现保持现有标准 slow log 行完全不变，只新增一行扩展注释行：

```text
# Query_time: ...
# InnoDB_storage_read_ops: 1  InnoDB_storage_read_bytes: 16384  InnoDB_storage_read_wait: 0.001000
SELECT ...
```

兼容性约束：

- 绝不修改标准 `# Query_time ...` 行
- 只追加单独的 `# ...` 扩展行
- 功能默认关闭

## 用户可见变量

### 变量定义

- 名称：`log_slow_innodb_io`
- 作用域：`GLOBAL`
- 类型：`BOOL`
- 默认值：`OFF`

### 语义

- 仅影响 `FILE` slow log 输出。
- 当 `log_output` 不包含 `FILE` 时，允许设置，但给出与
  `log_slow_extra` 相同模式的 warning。

### 涉及文件

- `sql/mysqld.h`
- `sql/mysqld.cc`
- `sql/sys_vars.cc`

## 语句级状态设计

### 数据结构

在 `THD` 上增加一个独立的小结构，保存：

- `innodb_used`
- `storage_read_ops`
- `storage_read_bytes`
- `storage_read_wait_us`

### 设计原因

- 避免把一堆零散计数器直接平铺到 `THD`
- 方便统一 reset / 聚合 / 输出
- 能支持“使用了 InnoDB 但没有触发底层存储读”时输出 0 值行

### 建议方法

在 `THD` 上增加轻量 helper：

- `reset_slow_log_innodb_io_stats()`
- `note_slow_log_innodb_used()`
- `add_slow_log_storage_read_stats(bytes, wait_us)`

### 涉及文件

- `sql/sql_class.h`

## 语句生命周期

### reset 时机

在 `sql/sql_parse.cc` 中的：

- `THD::reset_for_next_command()`

里统一 reset 这份语句级状态。

这样做的原因：

- 这是当前 top-level statement 生命周期的自然边界
- 不需要额外在多个 slow log 初始化路径重复接线
- 改动点更集中

### 状态归属

- 状态归属顶层语句
- 嵌套执行路径统一累计到外层语句
- 第一版不额外实现 substatement 的 save/restore

## SQL 层 slow log 输出

### 输出规则

在 `File_query_log::write_slow()` 中：

- 当 `log_slow_innodb_io=OFF` 时，不输出任何新增行
- 当 `log_slow_innodb_io=ON` 且 `innodb_used=false` 时，不输出新增行
- 当 `log_slow_innodb_io=ON` 且 `innodb_used=true` 时，输出一行扩展信息

这样可以实现：

- 非 InnoDB 语句不输出
- 使用了 InnoDB 但没有触发底层存储读时输出 0 值
- 真正发生底层存储读时输出正值

### 涉及文件

- `sql/log.cc`

## InnoDB 侧集成

实现上使用一个非常小的桥接层，把 InnoDB 事件路由到 SQL 层的 `THD`
语句级状态。

### helper 声明

声明放在：

- `storage/innobase/include/ha_prototypes.h`

定义放在：

- `storage/innobase/handler/ha_innodb.cc`

helper 包括：

- `innobase_collect_slow_log_io()`
- `innobase_register_slow_log_storage_read(bytes, wait_us)`

职责：

- 判断当前前台上下文是否开启采集
- 将统计结果汇总到 `current_thd`

## InnoDB 使用标记

为了区分：

- 非 InnoDB 语句
- 使用了 InnoDB 但没有触发底层存储读的语句

需要在 InnoDB handler 路径上提前标记“本语句已使用 InnoDB”。

推荐在：

- `ha_innobase::update_thd(THD *thd)`

中调用 `note_slow_log_innodb_used()`。

原因：

- 这是已有的语句上下文绑定点
- 位于 InnoDB 表访问主路径上
- 不需要改 planner / executor 的更多位置

## 底层存储读统计

第一版只统计由 buffer pool miss 触发的底层存储读。

### 1. 同步底层存储读发起

文件：

- `storage/innobase/buf/buf0rea.cc`

埋点函数：

- `buf_read_page_low(...)`

行为：

- 当 `sync=true` 时，在读开始前记录时间
- 读完成后上报：
  - 一次 storage read op
  - 对应 page size 的字节数
  - 实际等待时间

这覆盖了“当前语句自己发起缺页读”的主要场景。

### 2. 等待其他线程正在进行的读

文件：

- `storage/innobase/buf/buf0buf.cc`

埋点位置：

- `buf_page_get_zip(...)` 中等待压缩页读完成的循环
- `buf_wait_for_read(buf_block_t *block)`

行为：

- 如果当前语句只是等待某个已在进行中的读完成，则只上报等待时间
- 不增加 ops / bytes，避免重复计数

这样既保留了当前语句感受到的 IO wait，又避免把同一次底层读重复算多次。

## 性能约束

### 功能关闭时

当 `log_slow_innodb_io=OFF` 时：

- 只应保留极少量条件判断
- 不应发生额外统计更新
- 不应产生任何额外 slow log 输出

### 功能开启时

当功能开启时：

- 工作集中在同步缺页读与等待路径
- 更新仅为 `THD` 上的整数累加
- 格式化只发生在真正写 slow log 时

明确避免：

- 全局共享状态
- 所有 handler 读路径的广泛埋点
- 重锁与高成本同步
- 引入大型事务级 / 引擎级统计框架

## 为稳定测试增加的 debug 钩子

为了让 MTR 用例不依赖 buffer pool 的真实冷热状态，第一版增加两个
仅测试使用的 `DBUG_EXECUTE_IF` 钩子：

- `innodb_slow_log_force_storage_read`
  - 只要语句使用了 InnoDB，就强制注入一条正值统计
- `innodb_slow_log_skip_storage_reads`
  - 继续标记语句使用了 InnoDB，但跳过 storage read 上报

这样可以稳定覆盖：

- 正值统计输出
- 0 值统计输出

而不会影响正常生产行为。

## 测试计划

### 新增测试

1. `main.slow_log_innodb_io`
   - 验证功能关闭时不输出新增行
   - 验证功能开启且有正值统计时输出正确格式
   - 验证功能开启但上报被抑制时输出 0 值行

2. `suite/sys_vars.log_slow_innodb_io_basic`
   - 验证变量类型检查
   - 验证作用域约束
   - 验证 `log_output` 不为 `FILE` 时的 warning

### 回归验证

至少运行：

- `main.slow_log_innodb_io`
- `suite/sys_vars.log_slow_innodb_io_basic`
- `main.slow_log_extra`
- `main.mysqldumpslow`
- `main.1st`

如果时间与环境允许，再继续扩大 `main` 范围验证。

## 后续待办

- 增加 buffer pool 逻辑读次数统计，作为与 storage read 配套的补充指标
- 增加 lock wait / queue wait 等增强字段
- 仅在第一版验证稳定后，再考虑与更通用的 slow log verbosity 体系整合
