# Slow Log InnoDB IO 高层设计

## 概述

本文档描述一个面向 MySQL 8.0.41 的高层设计方案，用于增强慢查询日志，
使其能够输出语句级别、由 buffer pool miss 触发的 InnoDB 底层存储读
信息。目标是帮助定位某条慢 SQL 是否很可能由 IO 导致，尤其是在冷数据
场景中：第一次执行较慢，后续执行由于页已经进入 buffer pool 而明显变快。

该设计参考了 Percona Server 的慢日志扩展思路，但目标不是照搬 Percona
的完整实现，而是在 MySQL 8.0.41 上实现一个更小、更稳、更低侵入的版本。

## 问题定义

当前 MySQL 慢日志提供的主要信息包括：

- `Query_time`
- `Lock_time`
- `Rows_sent`
- `Rows_examined`

这些字段对分析慢 SQL 很有帮助，但它们无法直接回答一个非常关键的问题：

- 这条语句是否因为发生了由 buffer pool miss 触发的底层存储读 IO 才变慢？

这个缺口在以下场景中尤其明显：

1. 查询访问的是冷数据。
2. 第一次执行较慢，因为目标页不在 buffer pool 中。
3. 第二次执行明显变快，因为所需页已经被加载到缓存中。

如果慢日志里没有 IO 维度的数据，仅凭日志本身很难确认“慢”的主要原因
是不是 buffer pool miss 引起的底层存储读成本。

## 为什么这个功能值得做

这个功能是有必要的，因为它能显著提升慢查询的第一轮归因效率，
减少人工关联多个观测面的成本。

预期收益包括：

- 帮助 DBA 和研发快速判断慢 SQL 是否主要受 IO 影响。
- 让“第一次慢、第二次快”的模式在慢日志中直接可见。
- 降低对 Performance Schema、SHOW ENGINE INNODB STATUS、
  多次重复执行实验等手工分析手段的依赖。
- 在冷数据、buffer pool 容量不足、工作集抖动等问题上提供更直接的证据。

这个功能的定位应当是“诊断增强”，而不是完整的根因分析系统。

## 非目标

第一版实现不应该试图一次性解决所有性能归因问题。

第一版不应包含：

- 取代 Performance Schema 或其他可观测性体系。
- 自动判断所有慢查询的根因。
- 引入一个大而全的通用慢日志扩展框架。
- 在第一版就扩展 `mysql.slow_log` 系统表结构。
- 收集与“判断是否为 IO 导致”无关的大量 InnoDB 内部统计项。

## MySQL 8.0.41 当前基础情况

在 MySQL 8.0.41 中，慢日志文件输出的核心逻辑集中在：

- `sql/log.cc`

其中主要输出函数为：

- `File_query_log::write_slow()`

这是一个很适合扩展的落点，因为：

- 慢日志文件格式输出已经集中在这里。
- 当前代码已经支持 `log_slow_extra` 这类可选附加输出。
- SQL 层已经存在“语句开始时保存状态”的入口，例如：
  `sql/sql_parse.cc`、`sql/srv_session.cc`。

因此，如果要增加语句级 IO 统计输出，最合理的方向是：

- 执行过程中在语句上下文中累计 IO 统计；
- 写慢日志时在 `File_query_log::write_slow()` 中统一打印。

## 来自 Percona Server 的参考

Percona Server 已经实现了较完整的慢日志增强能力，其大致思路是：

- InnoDB 在执行过程中收集语句级统计。
- 将这些统计保存在 `THD` 上。
- 当 `File_query_log::write_slow()` 输出慢日志时，如果存在 InnoDB
  统计，则打印对应字段。

值得借鉴的核心思想有：

- 以“语句”为统计边界，而不是以全局或整个实例为边界。
- 在执行路径上收集统计，在日志输出时统一格式化。
- 由 SQL 层持有最终聚合结果，引擎侧只负责上报事件。

但不建议机械照搬的地方包括：

- Percona 的实现属于一个更大的 slow log extension 体系。
- 它引入的字段、开关、状态和代码路径都比当前目标所需的更多。
- 如果直接搬过来，会明显增加 MySQL 8.0.41 上的侵入性和维护成本。

## 设计原则

建议本功能遵循以下原则：

1. 关闭时默认零成本或近零成本。
2. 优先解决核心问题：由 buffer pool miss 触发的底层存储读 IO 归因。
3. SQL 层与 InnoDB 层职责清晰。
4. 第一版避免修改系统表定义。
5. 以小步、隔离、可验证的改动为主。
6. 输出格式尽量简洁，并与 Percona 的使用习惯保持一定相似性。

## 用户可见行为设计

### 新增配置项

建议新增一个独立的全局布尔变量：

- `log_slow_innodb_io`

推荐行为：

- 默认值：`OFF`
- 作用域：`GLOBAL`
- 仅当 slow log 输出包含 `FILE` 时真正生效
- 如果当前仅启用了 `TABLE` 输出，则允许设置，但给出 warning，
  提示该功能对 table logging 无效

之所以建议单独新增开关，而不是直接绑定到 `log_slow_extra`，
原因包括：

- 避免“想看 IO”必须顺带打开一大批额外字段。
- 功能边界更清晰。
- 更利于灰度启用和后续演进。

### 慢日志输出格式

当功能开启且当前语句存在 InnoDB 相关统计时，建议在慢日志中增加一行：

```text
# InnoDB_IO_r_ops: 128  InnoDB_IO_r_bytes: 2097152  InnoDB_IO_r_wait: 0.842731
```

为了在社区常见 slow log 解析工具上取得尽可能好的兼容性，实现上应遵循
一个严格的“兼容性优先”约束：

- 不修改现有标准 slow log header 行。
- 特别是，不要把新字段直接拼接到标准的
  `# Query_time: ... Lock_time: ... Rows_sent: ... Rows_examined: ...` 这一行。
- InnoDB IO 信息只能以“新增的一行独立注释行”的方式输出。
- 功能默认关闭，只有用户显式开启后才改变日志格式。

推荐格式：

```text
# Query_time: 1.238451  Lock_time: 0.000071 Rows_sent: 10 Rows_examined: 100000
# InnoDB_IO_r_ops: 128  InnoDB_IO_r_bytes: 2097152  InnoDB_IO_r_wait: 0.842731
SELECT ...
```

不推荐格式：

```text
# Query_time: 1.238451  Lock_time: 0.000071 Rows_sent: 10 Rows_examined: 100000 InnoDB_IO_r_ops: 128 ...
```

各字段含义：

- `InnoDB_IO_r_ops`
  - 当前语句触发的底层存储读次数，这里的前提是目标页原本不在
    buffer pool 中。
- `InnoDB_IO_r_bytes`
  - 这些由 buffer pool miss 触发的底层存储读对应的读取字节数。
- `InnoDB_IO_r_wait`
  - 当前语句因这些底层存储读累计等待的时间，单位秒，保留微秒级精度。

对于“判断这条慢 SQL 是否很可能是 IO 导致”的核心目标来说，
这 3 个字段已经足够有价值。

### 与社区工具兼容性的设计原则

业界很多开源工具会解析 MySQL slow log，但其实现方式并不统一：

- 有些工具能容忍未知注释行，只提取自己认识的字段。
- 有些工具使用较严格的正则或状态机，默认标准行格式固定。

因此，这里的设计目标应表述为：

- 最大限度兼容社区常见工具

而不是：

- 保证对所有工具零影响

明确的设计原则如下：

- 保持标准 slow log entry 结构不变。
- 只新增一行以 `#` 开头的扩展注释行。
- 绝不修改标准 `Query_time` 行的字段集合、顺序和语义。
- 将该能力定义为“默认关闭、显式开启”的 slow log 扩展能力。
- 在文档中明确说明：部分第三方 parser 可能忽略新增行，少量严格 parser
  仍可能需要升级或适配。

这种实现方式不能保证与所有第三方 parser 绝对兼容，但相比修改现有标准
字段行，兼容性风险会明显更低。

### 代表性工具兼容性观察

社区中存在多种不同风格的 MySQL slow log parser，它们对扩展格式的容忍度
并不一致。

- `mysqldumpslow`
  - 使用固定正则匹配标准 header 行。
  - 如果修改标准 `# Query_time` 行，风险很高。
  - 即使只新增独立扩展行，也应视为“谨慎兼容”，不能保证完全无影响。
- `pt-query-digest`
  - 使用更泛化的 slow log parser 与 key/value 提取方式。
  - 相比固定格式工具，更可能兼容独立的 `# InnoDB_IO_...` 行。
  - 有较强证据表明其事件模型已经能容纳 Percona 扩展字段，例如
    `InnoDB_IO_r_ops`。
- `percona/go-mysql` 与 PMM agent parser
  - 采用较通用的 header 检测和 metric 提取逻辑。
  - 更可能接受独立扩展注释行，并把值归入指标集合。
- 较严格的第三方 parser
  - 有些实现会明确声明不支持 `log_slow_extra` 等非标准格式。
  - 这说明并不是所有生态 parser 都天然能容忍格式扩展。

因此，推荐的兼容性目标应是：

- 最大限度兼容常见工具链

而不是：

- 对所有工具保证零兼容性影响

## 第一版建议范围

第一版建议只聚焦以下 3 个指标：

- 因 buffer pool miss 触发的底层存储读次数
- 因 buffer pool miss 触发的底层存储读字节数
- 因 buffer pool miss 触发的底层存储读等待时间

第一版不建议包含：

- 锁等待统计
- InnoDB 队列等待统计
- distinct pages 统计
- 更细粒度的页面分类
- 广义的慢日志 verbosity 扩展体系

这样做的好处是范围清晰、实现可控，并且与当前最核心的诊断目标直接对齐。

## 总体架构

推荐架构如下：

1. SQL 层持有语句级慢日志 IO 统计状态。
2. InnoDB 通过一个很小的桥接接口上报相关 IO 事件。
3. 慢日志格式化阶段统一输出最终聚合结果。

### SQL 层职责

SQL 层负责：

- 定义用户可见的系统变量。
- 持有语句级统计结构。
- 在顶层语句开始时 reset 统计。
- 在 `File_query_log::write_slow()` 中按需打印字段。

潜在涉及文件：

- `sql/sql_class.h`
- `sql/sql_class.cc`
- `sql/sql_parse.cc`
- `sql/srv_session.cc`
- `sql/sys_vars.cc`
- `sql/log.cc`

### InnoDB 层职责

InnoDB 层负责：

- 识别与当前语句相关、由 buffer pool miss 触发的底层存储读事件。
- 以最小必要信息向 SQL 层上报。
- 不关心慢日志的最终打印格式。

潜在涉及文件：

- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/buf/buf0buf.cc`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/ha_prototypes.h`

## 推荐的内部状态模型

不建议像 Percona 那样第一步就往 `THD` 上散落添加一串独立字段。
更建议引入一个小型、专用的语句级结构体，由 `THD` 持有。

概念上这个结构可以包含：

- 当前语句是否启用了该功能
- 当前语句是否使用了 InnoDB
- 底层存储读次数（由 buffer pool miss 触发）
- 底层存储读字节数（由 buffer pool miss 触发）
- 底层存储读等待时间（微秒）

使用独立结构体的好处：

- 与 `THD` 其它状态隔离更好。
- 生命周期更容易管理。
- 后续如需扩展更多 InnoDB 慢日志字段，更容易演进。
- 避免把 `THD` 进一步变成零散计数器的堆积区。

## 语句生命周期设计

这些统计必须以“语句”为边界，而不是以“事务”为边界。

推荐生命周期如下：

1. 顶层语句开始时 reset 统计状态。
2. 执行过程中 InnoDB 持续上报由 buffer pool miss 触发的底层存储读活动。
3. 写慢日志时读取并打印聚合结果。
4. 下一条顶层语句重新从干净状态开始。

推荐直接复用 MySQL 当前已有的语句起始处理点，例如：

- `sql/sql_parse.cc`
- `sql/srv_session.cc`

这样可以减少额外改动，因为类似 `log_slow_extra` 的基础机制已经存在。

## 子语句处理策略

第一版建议采用一个简单且工程上稳妥的规则：

- 只以顶层语句为统计边界；
- 顶层语句内部触发的存储过程、函数、触发器等执行路径中产生的相关
  InnoDB IO，统一累计到外层语句。

这个策略是合理的，因为：

- 慢日志本身就是按顶层语句输出的。
- 用户真正关心的是“这条外层 SQL 为什么慢”。
- 可以避免第一版就引入复杂的 backup/restore 子语句状态同步逻辑。

如果后续测试发现某些嵌套执行场景会导致统计污染，再进一步补充
子语句状态保存与恢复机制会更稳妥。

## InnoDB 如何上报数据

推荐设计一个很小的 SQL 层 / InnoDB 层桥接接口。

这个桥接接口应具备以下特征：

- 足够小
- 只服务当前功能
- InnoDB 不感知慢日志格式
- 判断是否开启时足够快
- 在热点路径上只做整数累加，不做复杂逻辑

概念上，需要支持上报的内容包括：

- 一次由 buffer pool miss 触发的底层存储读操作
- 若干字节底层存储读
- 若干微秒底层存储读等待时间

实现方式可以是：

- 一个带 type 的统一上报函数

或

- 少量几个专用 helper 函数

具体 API 形态不是最关键的，最关键的是职责边界清晰：

- InnoDB 负责上报事件
- SQL 层负责聚合状态

## InnoDB 侧建议埋点位置

第一版建议只关注那些最能代表“当前前台语句因 buffer pool miss 发生了
真实底层存储读”的路径。

优先考虑的区域包括：

- `buf0buf.cc`
  - 适合记录由页不在 buffer pool 中而触发的底层页读取动作
- `row0pread.cc`
  - 适合记录同步读或用户可感知读等待时间
- `ha_innodb.cc`
  - 适合放快速开关判断和桥接 helper

埋点时应尽量避免统计以下内容：

- 后台 IO 线程行为
- 无法稳定归因到当前语句的 read-ahead
- 语义不清晰、容易误导的广义内部读活动

第一版最重要的不是“尽量多收集”，而是“保证语义可信”。

## 日志输出规则

推荐输出规则如下：

- 如果当前语句没有使用 InnoDB，则不打印该行。
- 如果当前语句使用了 InnoDB，但没有发生可归因的底层存储读，则打印 0 值行。
- 如果当前语句使用了 InnoDB，且发生了这类底层存储读，则打印实际值。

相比输出类似 “No InnoDB statistics available for this query” 的文本提示，
这种方式更适合 MySQL 8.0.41 第一版实现，因为：

- 日志更干净
- 更容易被脚本解析
- 0 值本身已经携带有效信息

## 为什么第一版不扩展 `mysql.slow_log`

如果要支持 `TABLE` 格式输出，就需要修改 `mysql.slow_log` 系统表定义，
这会牵扯到：

- 表结构定义
- 系统表兼容性
- 相关测试与行为验证
- 升级与运维约束

这个改动面明显大于“增强 FILE 慢日志输出”，但对当前最重要的目标
帮助并不大，因为实际诊断场景通常是直接看 slow log file。

因此第一版建议明确限定：

- 支持 `FILE`

第一版明确不支持：

- 给 `mysql.slow_log` 新增列

## 性能考虑

这个功能必须严格控制性能影响。

### 功能关闭时

当 `log_slow_innodb_io=OFF` 时，额外成本应尽量限制为：

- 一个廉价的开关判断
- 不发生实质性的额外统计更新

### 功能开启时

开启后，额外成本也应尽量保持很小，因为：

- 只更新少量整数计数器
- 主要埋点位于底层存储读相关路径
- 额外开销主要出现在本身已经较慢的 IO 路径上

实现时应避免：

- 引入重锁
- 在执行路径中做字符串格式化
- 每次访问都做昂贵的跨层查找
- 在所有 handler read 热路径上做大面积统计

## 正确性风险

这个功能最大的风险不是格式输出，而是统计口径是否准确。

主要风险包括：

- 错把后台 IO 计入当前语句
- 把 read-ahead 计入后导致归因失真
- 漏掉部分真正同步读，造成低估
- reset 时机不对，导致跨语句污染
- 嵌套执行路径导致状态串扰

因此，第一版应优先追求“语义准确、口径可信”，而不是追求统计面过宽。

## 与 Percona 全量扩展方案相比的取舍

本方案明确比 Percona 的完整慢日志扩展做得更少，这是有意为之。

这种缩小范围的方案具备以下优势：

- 代码改动面更小
- review 风险更低
- 行为回归风险更低
- 更符合 upstream 风格的小步演进方式
- 更容易验证和上线

本方案刻意延后的内容包括：

- 锁等待字段
- 队列等待字段
- distinct pages 字段
- 更通用的 slow log verbosity 扩展体系

## 建议的分阶段实施方式

### Phase 1：最小可用诊断能力

交付内容：

- `log_slow_innodb_io`
- `THD` 上的语句级 IO 统计状态
- InnoDB 到 SQL 层的轻量桥接
- FILE 慢日志中新增 IO 行
- 针对性的 MTR 覆盖

成功标准：

- 能通过慢日志清楚地区分 InnoDB 查询的冷读与热读行为。

### Phase 2：可选增强

只有在 Phase 1 证明价值明确且实现稳定后，再考虑扩展：

- 锁等待信息
- 队列等待信息
- 更丰富的 InnoDB 诊断字段
- 与更广义慢日志扩展框架的统一
- 可选补充 InnoDB buffer pool 逻辑读次数统计，用于与底层存储读指标配合，
  进一步区分“IO 导致的慢查询”和“访问量大但主要命中缓存的查询”

## 验证策略

至少建议覆盖以下测试场景：

1. 功能关闭
   - 慢日志中不出现新增字段。
2. 功能开启，首次冷读
   - `InnoDB_IO_r_ops > 0`
   - `InnoDB_IO_r_wait > 0`
3. 同一 SQL 再次执行，数据已热
   - IO 指标变为 0 或显著下降
4. 非 InnoDB 查询
   - 不打印 InnoDB IO 行
5. `log_output=TABLE`
   - 变量可设置但给出 warning
   - 不尝试扩展 `mysql.slow_log`

实现后建议的验证命令：

```bash
cd build/mysql-test
./mtr main.slow_log_innodb_io
```

## 最终建议

这个功能值得实现。

对于 MySQL 8.0.41，最合适的做法不是直接照搬 Percona 的完整慢日志扩展，
而是借鉴其核心思路，做一个更窄、更稳的实现：

- 收集语句级、由 buffer pool miss 触发的 InnoDB 底层存储读统计
- 由 SQL 层线程上下文保存聚合结果
- 在 FILE 慢日志中打印一行紧凑的 IO 信息
- 第一版不改 TABLE 慢日志
- 第一版只聚焦 storage read ops、storage read bytes、storage read wait

兼容性目标也应明确表达为：

- 最大限度兼容社区常见 slow log 工具

而不是：

- 保证对所有工具零影响

在当前已确认的工具行为下，“默认关闭 + 不改标准 `Query_time` 行 +
新增独立扩展注释行”是目前兼容性风险最低的推荐实现方式。



## InnoDB 内部实现是什么样的

`Innodb_buffer_pool_reads`

`Innodb_pages_read`

`innobase_register_slow_log_storage_read`

调用接口如下：

`buf_read_page_low`

`buf_page_get_zip`

`buf_wait_for_read` : innobase_register_slow_log_storage_read(0, wait_us);

```
./buf/buf0rea.cc:144:    innobase_register_slow_log_storage_read(page_size.physical(), wait_us);
./buf/buf0buf.cc:3370:      innobase_register_slow_log_storage_read(0, wait_us);
./buf/buf0buf.cc:3556:    innobase_register_slow_log_storage_read(0, wait_us);
```





Percona 调用接口

```c++

buf_wait_for_read

/** Wait for the block to be read in.
@param[in]      block   The block to check
@param          trx     Transaction to account the I/Os to */
static void buf_wait_for_read(buf_block_t *block, trx_t *trx) {
  /* Note:
  This unlocked read of IO fix is safe as we have the block buf-fixed. The page
  can only transition away from the IO_READ state, and once this is done, it
  will not be IO_READ again as long as we have it buf-fixed.

  The repeated reads of io_fix will not be optimized out because it's an atomic
  variable.*/
  std::chrono::steady_clock::time_point start_time;
  while (block->page.was_io_fix_read()) {
    if (start_time == std::chrono::steady_clock::time_point{})
      start_time = trx_stats::start_io_read(trx, 0);
    /* Page is X-latched on block->lock until the read is completed.
    Let's just wait for S-lock on block->lock, it will be granted as soon as the
    read completes. */
    rw_lock_s_lock(&block->lock, UT_LOCATION_HERE);
    rw_lock_s_unlock(&block->lock);
  }
  if (start_time != std::chrono::steady_clock::time_point{})
    trx_stats::end_io_read(trx, start_time);
}

/** Does a synchronous read operation in Posix.
@param[in]      type            IO flags
@param[in]      file            handle to an open file
@param[out]     buf             buffer where to read
@param[in]      offset          file offset from the start where to read
@param[in]      n               number of bytes to read, starting from offset
@param[out]     err             DB_SUCCESS or error code
@return number of bytes read, -1 if error */
[[nodiscard]] static ssize_t os_file_pread(IORequest &type, os_file_t file,
    ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦  void *buf, ulint n,
    ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦  os_offset_t offset, trx_t *trx,
    ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦  dberr_t *err) {
#ifdef UNIV_HOTBACKUP
  static meb::Mutex meb_mutex;

  meb_mutex.lock();
#endif /* UNIV_HOTBACKUP */
  ++os_n_file_reads;
#ifdef UNIV_HOTBACKUP
  meb_mutex.unlock();
#endif /* UNIV_HOTBACKUP */

  const auto start_time = trx_stats::start_io_read(trx, n);

  os_n_pending_reads.fetch_add(1);
  MONITOR_ATOMIC_INC(MONITOR_OS_PENDING_READS);

  ssize_t n_bytes = os_file_io(type, file, buf, n, offset, err, nullptr);

  trx_stats::end_io_read(trx, start_time);

  os_n_pending_reads.fetch_sub(1);
  MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_READS);

  return (n_bytes);
}
```



`buf_wait_for_read`



