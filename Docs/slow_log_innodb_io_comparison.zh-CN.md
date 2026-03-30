# Community MySQL、Percona、MariaDB 与我们当前实现的慢日志能力对比

## 概述

本文档对以下几类实现进行横向对比：

- Community MySQL 8.0.41
- Percona Server
- MariaDB Server
- 我们当前基于 Community MySQL 8.0.41 的实现

对比重点聚焦在一个问题上：

- 慢日志是否能够帮助判断某条慢 SQL 是否由 IO 导致，特别是由
buffer pool miss 触发的底层存储读导致。

## 结论摘要

从整体上看：

- Community MySQL 提供标准 slow log 和 `log_slow_extra`，但不能直接输出
语句级 InnoDB 底层存储读归因信息。
- Percona 在 slow log 中增加了较完整的 InnoDB 扩展字段，诊断能力较强，
但实现更重、更侵入，也绑定在更大的 extended slow log 体系上。
- MariaDB 提供了更广义的 engine 统计视角，不仅有页读，还包含页访问、
预读、engine time 等，同时它还同步扩展了自家的 slow log 解析工具。
- 我们当前实现有意做得更窄：只聚焦“由 buffer pool miss 触发的底层存储读”，
目标是以更低风险、更低侵入、更低兼容性影响，补齐 Community MySQL 在
IO 慢查询定位上的能力缺口。

## 总体对比表


| 维度                    | Community MySQL 8.0.41 | Percona Server    | MariaDB Server          | 我们当前实现                                |
| --------------------- | ---------------------- | ----------------- | ----------------------- | ------------------------------------- |
| 标准 slow log           | 支持                     | 支持                | 支持                      | 支持                                    |
| 扩展 slow log 字段        | `log_slow_extra`       | 更丰富的 slow log 扩展  | `log_slow_verbosity` 扩展 | 新增专用扩展                                |
| InnoDB / engine IO 归因 | 无语句级 InnoDB IO 归因      | 有，偏 InnoDB        | 有，偏 engine 统计           | 有，聚焦 InnoDB buffer pool miss 触发的底层存储读 |
| 指标范围                  | 偏通用                    | 中等，偏 InnoDB       | 更宽，偏 engine             | 更窄，偏 IO 定位                            |
| 输出方式                  | 标准行，可选 extra 字段        | 标准行 + 扩展行         | 标准行 + 扩展行               | 标准行 + 独立扩展行                           |
| 是否改标准 `Query_time` 行  | 否                      | 否                 | 否                       | 否                                     |
| 默认是否开启                | 否                      | 否                 | 依赖 verbosity 配置         | 否                                     |
| 是否扩展 slow log table   | 否                      | 主要还是文件扩展          | 主要还是文件扩展                | 否                                     |
| 官方解析工具是否跟进            | 有限                     | 在 Percona 工具体系内较好 | 在 MariaDB 工具体系内较好       | 目前未扩展 MySQL 官方工具                      |
| 实现侵入性                 | 最低                     | 较高                | 较高                      | 较低                                    |


## Community MySQL 8.0.41

### 已有能力

Community MySQL 8.0.41 提供：

- 标准 slow log
- `log_slow_extra`
- 较好的基础兼容性

典型字段包括：

- `Query_time`
- `Lock_time`
- `Rows_sent`
- `Rows_examined`
- 以及在 `log_slow_extra=ON` 时的一些通用计数器，例如 handler 读统计、
sort 统计、临时表统计、开始/结束时间等

### 能力缺口

Community MySQL 不能直接回答：

- 这条 SQL 是否因为页不在 buffer pool 中，触发了底层存储读，才变慢？
- 它到底有没有发生明显的 IO wait？

### 优势

- 语义简单
- 工具兼容性最好
- 维护成本最低

### 劣势

- 无法直接从 slow log 判断“是不是 IO 导致慢”
- 对“第一次慢、第二次快”的冷数据场景支持不足

## Percona Server

### 增强能力

Percona 在 slow log 中增加了较丰富的 InnoDB 诊断字段，例如：

- `InnoDB_IO_r_ops`
- `InnoDB_IO_r_bytes`
- `InnoDB_IO_r_wait`
- `InnoDB_rec_lock_wait`
- `InnoDB_queue_wait`
- `InnoDB_pages_distinct`

这使它非常适合用于 InnoDB 慢查询诊断。

### 实现特点

Percona 的实现特点包括：

- 在 `THD` 和 InnoDB 之间做了较深的统计集成
- 以语句为边界收集 InnoDB-specific 统计
- 属于一个更大的 extended slow log 体系

### 优势

- 对 InnoDB 慢查询的诊断价值高
- 能直接看到 IO 和部分等待维度
- 与 Percona 自家工具链适配较好

### 劣势

- 改动面更大，侵入性更高
- 不只是解决“IO 是否导致慢”，而是扩成了更广义 slow log 扩展
- 对于我们当前目标来说，范围偏大

## MariaDB Server

### 增强能力

MariaDB 通过 `log_slow_verbosity` 暴露更广义的 engine 统计，例如：

- `Pages_accessed`
- `Pages_read`
- `Pages_prefetched`
- `Pages_updated`
- `Old_rows_read`
- `Pages_read_time`
- `Engine_time`

它关注的不只是“有没有发生 IO”，而是一个更宽的 engine 画像。

### 实现特点

MariaDB 的实现特点包括：

- 通过 `handler_stats` 在语句级聚合统计
- 在 statement 级决定是否采集 engine stats
- slow log 中输出 engine 扩展行
- 官方解析工具 `mariadb-dumpslow` 也同步支持这些字段

### 优势

- engine 侧可观测性更强
- statement-level 的采集开关思路很好
- 工具链配套比“只改日志格式、不改解析器”更完整

### 劣势

- 指标范围更宽，语义更复杂
- 并不是所有字段都直接服务于“buffer pool miss 导致底层存储读”这个核心问题
- 如果直接照搬到 MySQL 8.0.41，改动面会明显变大

## 我们当前实现

### 设计目标

我们当前实现有意选择了一个更窄、更稳的方向：

- 只针对 InnoDB
- 只针对语句级 slow log
- 只聚焦 buffer pool miss 触发的底层存储读
- 只新增一行扩展输出
- 默认关闭
- 不修改标准 `# Query_time ...` 行

### 当前输出字段

当前实现输出的是：

- `InnoDB_storage_read_ops`
- `InnoDB_storage_read_bytes`
- `InnoDB_storage_read_wait`

这些字段回答的是最核心的运维问题：

- 这条慢 SQL 是否因为 buffer pool miss 触发底层存储读而变慢？

### 为什么不直接照搬 Percona

相比 Percona，我们当前实现刻意做得更小：

- 第一版不做 lock wait / queue wait
- 不引入更大范围的 THD / InnoDB 扩展统计体系
- 不引入更宽的 slow log verbosity 框架依赖

### 为什么不直接照搬 MariaDB

相比 MariaDB，我们当前实现更聚焦：

- 不做更广义的 page access / prefetch / engine time 统计
- 不做 buffer pool 逻辑读统计
- 也还没有扩展 MySQL 官方 slow log 解析工具

## 我们当前实现的优势

### 1. 更贴合当前目标

我们当前实现不是做一个“大而全”的 engine profiling 框架，而是针对一个
很具体、很高频的问题：

- 慢 SQL 是否因为 buffer pool miss 导致底层存储读而变慢

这使它更容易理解，也更适合作为第一版能力。

### 2. 侵入性更低

相比 Percona 和 MariaDB，我们当前实现改动面更小。

直接收益包括：

- review 更容易
- 回归风险更低
- 不容易影响 slow log 其它既有能力

### 3. 兼容性风险更低

我们保持了标准 slow log header 不变，只追加一行独立扩展行。

相比去改 `# Query_time ...` 行，这种方式对已有工具更友好。

### 4. 性能风险更低

当前实现的统计路径是刻意收窄的：

- 默认关闭
- 只在同步缺页读与等待路径采集
- 只做语句级轻量聚合

### 5. 更符合 upstream 风格的小步演进

作为 MySQL 8.0.41 上的增强项，这种“小而准”的实现更现实：

- 先补最小可用诊断能力
- 再视价值决定是否扩展更多维度

## 我们当前实现的劣势

### 1. 诊断范围不如 Percona 完整

当前我们没有提供：

- lock wait 扩展字段
- queue wait 扩展字段
- page distinctness 指标

因此如果目标是“尽可能丰富的 InnoDB slow log 诊断”，Percona 更强。

### 2. engine 侧视角不如 MariaDB 宽

当前我们没有提供：

- page access 统计
- page prefetch 统计
- page update 统计
- engine time

因此如果目标是“更广义的 engine profiling”，MariaDB 更强。

### 3. 官方解析工具还没有同步增强

MariaDB 的一个明显优势是，它连 `mariadb-dumpslow` 都一起更新了。

而我们当前实现虽然已经尽量降低了兼容性风险，但还没有扩展：

- `mysqldumpslow`

所以当前的策略是：

- 尽量减少兼容性影响

而不是：

- 让 MySQL 官方工具立刻完整消费这些新字段

### 4. 第一版更像“聚焦功能”，不是“通用框架”

这是优点，也是限制。

如果未来需求迅速扩大，当前实现还需要继续演进。

## 哪些点值得借鉴

### 借鉴 Percona 的点

- 语句级 InnoDB 统计聚合
- 直接在 slow log 中体现 IO 行为

### 借鉴 MariaDB 的点

- statement-level 的采集 gating 思路
- 如果功能长期保留，官方解析工具最好同步跟进

## 哪些点暂时不建议借鉴

### 暂不借鉴 Percona 的点

- 第一版不引入完整的 InnoDB 扩展 slow log 体系
- 第一版不把更多等待分类一次性加进来

### 暂不借鉴 MariaDB 的点

- 第一版不做更广义的 engine 统计
- 第一版不为解决当前问题而引入更大的 handler_stats 体系

## 综合评估

如果评价标准是：

- 以较低风险、较低侵入，尽快让 Community MySQL 具备实用的 IO 慢查询定位能力

那么我们当前实现是合适的。

如果评价标准是：

- 立刻获得最丰富的 InnoDB slow log 诊断能力

那么 Percona 更强。

如果评价标准是：

- 获得更宽的 engine 统计视角，并让官方解析工具一起跟进

那么 MariaDB 的思路更完整。

我们当前实现刻意处在一个中间位置：

- 比 Community MySQL 原生能力明显更有用
- 比直接引入 Percona / MariaDB 更大范围模型的侵入性明显更低
- 更适合作为 MySQL 8.0.41 上的第一版 upstream 风格增强

## 后续演进建议

后续最有价值的方向包括：

- 增加可选的 buffer pool 逻辑读次数统计
- 考虑引入类似 MariaDB 的 statement-level active gating
- 如果该能力长期保留，考虑同步扩展 `mysqldumpslow`
- 视收益再决定是否选择性引入部分 Percona 风格的等待字段

