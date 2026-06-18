# MDL Pinbox Capacity Feature Specification

> Version: 2.0
> Target: TaurusDB on store / MySQL 8.0.41 branch
> Purpose: Remove the historical MDL LF pinbox 65535 usable-pin ceiling
> without introducing a new configuration variable.

---

## 1. 问题场景

线上实例配置较大的连接数上限，例如：

```text
max_connections=100000
```

但在连接峰值约 65000 左右时，业务连接和后台 agent 均可能收到：

```text
ER_MDL_OUT_OF_RESOURCES
Not enough resources to complete lock request.
```

这不是普通的 `Too many connections`。同机 agent 直连 mysqld 也能遇到该错误，说明不能只把问题归因于 Mycat、客户端 TCP 端口耗尽或网络路径。源码分析和缩小 pinbox 的集成实验表明，根因是 mysqld 进程内 MDL 子系统使用的 LF pinbox 容量先被耗尽。

---

## 2. 根因分析

基线实现中的错误触发链路如下：

```text
MDL_context::fix_pins()
  -> if (!m_pins) m_pins = mdl_locks.get_pins()
  -> MDL_map::get_pins()
  -> lf_hash_get_pins()
  -> lf_pinbox_get_pins()
  -> returns nullptr when pinbox is exhausted
  -> my_error(ER_MDL_OUT_OF_RESOURCES)
```

关键源码事实：

| 文件 | 证据 |
|---|---|
| `sql/mdl.cc` | `static MDL_map mdl_locks;` 是 mysqld 进程级全局 MDL map。 |
| `sql/mdl.cc` | `MDL_context` 构造时 `m_pins(nullptr)`，首次访问 MDL map 时惰性分配。 |
| `sql/mdl.cc` | `MDL_context::destroy()` 通过 `lf_hash_put_pins(m_pins)` 归还 pins。 |
| `mysys/lf_alloc-pin.cc` | 原 `pinstack_top_ver` 使用 32 bit 编码：低 16 bit 为 pin index，高 16 bit 为 ABA version。 |
| `mysys/lf_alloc-pin.cc` | index 0 保留为 NULL，原 `LF_PINBOX_MAX_PINS=65536` 实际只有 65535 个可用 index。 |

`max_connections` 控制连接接入层面的上限。MDL LF pinbox 是另一个更底层的进程内资源池。即使 `max_connections` 配置允许继续接入，只要需要分配 MDL pins 的上下文超过 pinbox 容量，仍可能返回 `ER_MDL_OUT_OF_RESOURCES`。

---

## 3. 设计目标

1. 支持数据库配置 `max_connections=1000000`。
2. 移除 MDL pinbox 16-bit index 带来的 65535 可用 pin 硬上限。
3. 不新增 `mdl_pinbox_max_pins` 之类配置变量，避免配置面和兼容性成本。
4. 不新增 MDL pinbox status 变量，保持 MR 变更范围集中。
5. 保持 LF pinbox 的 ABA 防护语义。
6. 不引入磁盘格式变化，不影响数据文件升级兼容。

---

## 4. 方案设计

### 4.1 LF pinbox 编码扩展

原设计：

```text
uint32 pinstack_top_ver
low 16 bits  : pin array index
high 16 bits : ABA version
```

新设计：

```text
uint64 pinstack_top_ver
low 32 bits  : pin array index
high 32 bits : ABA version
```

index 0 继续保留为 NULL。这样可以把可表达的 pin index 空间从 16 bit 扩展到 32 bit，同时保留 version 字段用于 ABA 防护。

### 4.2 固定容量

不引入新 sysvar。LF pinbox 使用编译期常量：

```c++
static constexpr uint32 LF_PINBOX_MAX_PINS = 2U * 1024U * 1024U;
```

由于 index 0 保留，实际可用 index 数约为 2M-1，足以覆盖 `max_connections=1000000` 并为 reserved/admin/internal 连接留出余量。

### 4.3 `max_connections` 上限

保留对已有 `max_connections` sysvar 的范围调整：

```text
MAX_CONNECTIONS_LIMIT = 1000000
```

该变更只扩大已有变量的合法范围，不新增配置项。

---

## 5. 实现摘要

| 文件 | 变更摘要 |
|---|---|
| `include/lf.h` | 将 `LF_PINBOX::pinstack_top_ver` 从 `uint32` 扩展为 `uint64`。 |
| `mysys/lf_alloc-pin.cc` | 将 LF pinbox free-list state 改为低 32 bit index / 高 32 bit ABA version，并把硬编码容量提升到 2M。 |
| `sql/sys_vars.h` / `sql/sys_vars.cc` | 将 `max_connections` 合法范围扩展到 1000000。 |
| `unittest/gunit/mysys_lf-t.cc` | 增加默认 LF pinbox 能分配超过旧 65535 上限的测试。 |
| `unittest/gunit/mdl-t.cc` | 增加 MDL 能分配超过旧 65535 上限的测试。 |

明确没有引入：

- 新 sysvar。
- 新 status 变量。
- 启动期 MDL pinbox 容量 warning。
- 可运行时或启动时调整的 pinbox 参数。

---

## 6. 兼容性说明

LF pinbox 状态属于 mysqld 进程内内存结构，不写入 redo、undo、binlog、表空间、数据字典或系统表。因此该改动不涉及数据文件格式升级，也不需要数据迁移。

兼容性影响集中在二进制和配置层：

- 新 binary 允许 `max_connections=1000000`。
- 降级到旧 binary 前，如果配置值超过旧版本上限，需要先把 `max_connections` 调回旧版本允许范围。
- 本方案没有新增配置参数，因此不存在删除新参数后才能降级的问题。

---

## 7. 测试建议

基础验证：

```bash
ninja -C build-1m-mdl mysys_lf-t mdl-t mysqld -j 8
./build-1m-mdl/runtime_output_directory/mysys_lf-t \
  --gtest_filter='Mysys.LFPinboxAllocatesPastOldLimit'
./build-1m-mdl/runtime_output_directory/mdl-t \
  --gtest_filter='MDLTest.AllocatesPastOldPinboxLimit'
./build-1m-mdl/runtime_output_directory/mysqld \
  --no-defaults --max-connections=1000000 --verbose --help
cd build-1m-mdl/mysql-test && ./mtr 1st
git diff --check
```

生产前压力建议：

- 使用 Linux 目标环境做 10 万、50 万、100 万长连接压力。
- 优先 Unix socket 或明确隔离 TCP ephemeral port 影响。
- 每个连接执行真实表访问 SQL，例如 `SELECT COUNT(*) FROM db.t`，不要用 `SELECT 1` 作为 MDL 压力。
- 同时观测 `Threads_connected`、`Max_used_connections`、`Connection_errors_max_connections` 和 `ER_MDL_OUT_OF_RESOURCES` 计数。

---

## 8. 风险与后续工作

该 MR 只解决 MDL pinbox 的 65535 可用 pin 硬上限，不代表单机 100 万连接已经在所有资源维度完成容量治理。仍需关注：

- OS fd 限制、`open_files_limit`、`back_log`。
- 线程栈、PFS sizing、内存占用和调度开销。
- Mycat 后端池、agent 和监控连接池对长连接的放大作用。
- 高并发 connect/disconnect churn 下的 LF pinbox ABA 行为。

后续可独立评估：

- 是否需要可配置 pinbox 容量。
- 是否需要 MDL pinbox 运行期观测指标。
- 是否需要启动期校验 `max_connections` 与 pinbox 容量关系。
- 100 万连接端到端压测。
