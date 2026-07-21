# PQ Context Ownership Refactor

> 文档 ID：`PQ-ARCH-CTX-013`
> 状态：`commit-ready`（完整 Release/Debug/ASAN MTR 矩阵和性能对照仍待后续验收）
> 稳定对照基线：`stable_branch` 的 `1b9ffd755d4`
> 范围：THD、Query_block、JOIN 的 Parallel Query（PQ）状态所有权重构

## 1. 问题与目标

历史实现把 PQ 状态直接散落在 `THD`、`Query_block` 和 `JOIN` 三个 MySQL
核心对象中。字段和方法与通用 SQL 状态交错，导致 PQ 生命周期、clone、fallback
和 worker 销毁路径难以完整审阅，也提高了后续增加 PQ 状态时遗漏 owner 的风险。

本变更的目标是把已存在的 PQ 状态按原 owner 聚合到三个按值持有的 context 中：

```text
THD         -- owns by value --> PQ_thd_context
Query_block -- owns by value --> PQ_query_block_context
JOIN        -- owns by value --> PQ_join_context
```

它是物理所有权迁移，不是优化器或执行器功能变更。以下行为不在本变更范围内：

- 支持的 SQL、hint、DOP、资格检查和 serial fallback 规则；
- DIV_TAB/CUT_TAB 选择、Gather 插入、worker 计划、InnoDB PQ scan；
- MQ 协议、错误传播、KILL 语义、内存资源模型；
- DStore capability gate、C API bulk bind、MTR 框架或 result 选择逻辑。

这些不变量是本次验收的首要合同；出现 Gather、worker 数、访问路径、join shape
或 fallback 原因变化时，必须定位根因，禁止仅更新 result 接受差异。

## 2. 设计边界

### 2.1 THD context

`PQ_thd_context` 位于 `sql/parallel_query/pq_context.h`，负责一条连接/worker
所拥有的 PQ 状态：PQ `MEM_ROOT`、leader/worker 拓扑、gather 集合、worker manager、
DOP、retry/error/executed 标记、found rows、clone phase 和 EXPLAIN ANALYZE 状态。

构造、析构和语句 cleanup 仍由 `THD` 触发：

```text
THD constructor       -> initialize_mem_root()
THD destructor        -> destroy_mem_root()
cleanup_after_query() -> cleanup_statement()
worker clone          -> copy_from()
```

`THD::in_sp_trigger` 不迁入 context。它是通用 stored-program 生命周期状态，PQ
仅用它拒绝不适合并行化的语句；迁入会扩大非 PQ 行为风险。

### 2.2 Query_block context

`PQ_query_block_context` 聚合 query-block 级的 PQ 资格、clone 双向链接、where/having
备份、Item clone 临时标记、distinct worker 标记，以及 group/order/window 的
backup/restore 对象。found-rows 状态仍属于 `PQ_thd_context`。`Query_block` 保留原有薄包装 API，例如 `pq_link_clone()`、
`pq_unlink_clone()`、`pq_backup()`、`pq_restore()` 和 `pq_is_clone()`。

clone 链接仍是双向关系；serial fallback 恢复后必须断开过期 clone 链接，避免 worker
或 template 生命周期结束后留下悬挂引用。

### 2.3 JOIN context

`PQ_join_context` 聚合计划改写期间的 `JOIN` 状态：保存的优化器 flags、QEP_TAB
backup/indirection slices、临时表/fields 状态、DIV/CUT table index、排序与 group
状态、ref item slices、MQ handler、having pushdown 和 COUNT DISTINCT 标记。

`JOIN` 继续拥有原 API 和调用时序，例如 `save_optimized_vars()`、
`restore_optimized_vars()`、`alloc_qep1()`、`alloc_indirection_slices1()`、
`setup_tmp_table_info()` 和 `pq_restore()`。`JOIN::shallow_clone()` 不复制 PQ state，
保持原成员的非复制语义。

## 3. 等价性合同

| 范畴 | 必须保持的行为 | 静态证据 |
| --- | --- | --- |
| THD 生命周期 | 同一时点初始化、清理和释放 PQ MEM_ROOT；trigger/SP guard 不变 | `sql_class.cc` 与 `PQ_thd_context::{initialize,destroy,cleanup}_*` |
| Worker 拓扑 | worker 指向 leader；只复制原有 `has_pq`/DOP 状态 | `sql_parallel.cc`、`PQ_thd_context::copy_from` |
| 错误/KILL | local error、leader killed、leader PQ error、leader THD error 的短路顺序不变 | `THD::is_pq_error()` |
| Clone | 原/clone 双向链接、prepare MEM_ROOT 切换、unlink 时机不变 | `PQ_query_block_context` |
| Fallback | 恢复 where/having、group/order、optimizer/QEP state 后重建 serial plan | `PQ_query_block_context::restore`、`PQ_join_context::restore_plan` |
| Plan rewrite | leader/template/worker 的计划对象和调用序不变 | `pq_clone.cc`、`pq_optimizer.cc`、`sql_parallel.cc` |
| Non-PQ state | stored-program trigger state、通用 THD/JOIN/Query_block 字段不迁移 | owner header review |

迁移后的 context 仍暂时暴露字段给现有 PQ 调用点。这是降低本轮等价性风险的刻意
边界；后续如需收敛到窄 API，应作为单独变更逐步处理，不能与所有权迁移混合。

## 4. 热路径与性能合同

`MQueue_handle::send_bytes()` 和 `receive_bytes()` 在轮询中调用
`THD::is_pq_error()`。该 predicate 必须保持头内联，不能重构为跨编译单元的
`PQ_thd_context` wrapper：在本项目 `WITH_LTO=OFF` 的 Release 构建中，若调用点
不可见，通常会形成每轮 MQ 检查的跨编译单元调用；必须以目标二进制反汇编确认，
不能仅凭源代码推断。

最终实现保留基线的判断顺序：

```text
leader == null ? local_error :
  local_error || leader_killed || leader_pq_error || leader_thd_error
```

Release 符号审计要求 `mysqld` 中不存在
`PQ_thd_context::is_error` 符号；`MQueue_handle::{send_bytes,receive_bytes}`
应继续直接包含该 predicate 的优化代码。验收命令为：

```bash
cmake --build build-ninja-release --target mysqld -j 8
nm -nm build-ninja-release/runtime_output_directory/mysqld | c++filt | \
  rg 'PQ_thd_context::is_error|MQueue_handle::(send_bytes|receive_bytes)'
otool -tvV build-ninja-release/runtime_output_directory/mysqld
```

记录构建提交、编译器、架构和 `WITH_LTO` 值；接受条件是没有前述 wrapper 符号，且
MQ 两个循环不会调用该 wrapper。context 是 owner 内嵌对象；本重构没有引入新的
mutex、atomic、virtual dispatch、shared ownership 或由 context 引入的额外 heap
indirection（不对既有 PQ 容器的分配行为作泛化声明）。

对象布局可能发生轻微变化，因此“没有新增同步/分配”不等于已经证明零性能回归。
最终验收必须以同机器、同配置的 stable/refactor 对照记录 PQ 吞吐、P95/P99 和
每连接常驻内存；未取得该证据前不得宣传绝对性能无影响。

已在 macOS arm64、AppleClang Debug 二进制上用
`lldb -b -o 'image lookup -t <type>'` 读取 DWARF 的 `byte-size`，得到：

| 类型 | stable `1b9ffd755d4` | refactor | 差值 |
| --- | ---: | ---: | ---: |
| `THD` | 14304 | 14312 | +8 bytes |
| `JOIN` | 1136 | 1144 | +8 bytes |
| `Query_block` | 1056 | 1056 | 0 |

这只量化对象布局，不能替代 Release 工作负载、缓存行为和连接常驻内存的对照。

## 5. 计划结构合同

下表把允许归一化的动态 metrics 与不可掩盖的结构事实区分开：

| 场景 | 代表 MTR | 必须保持 | 可归一化 |
| --- | --- | --- | --- |
| 并行 full/index/range scan | `pq_fullscan`、`pq_range_sec`、`pq_icp` | Gather、parallel scan、关键 access path、DOP | cost、rows estimate、actual time |
| 聚合/排序 | `pq_group_by`、`pq_agg_distinct`、`pq_limit_no_order_by` | Gather 边界、partial/final aggregate、sort/limit shape | 动态 metrics |
| Join | `pq_hash_join`、`pq_join_bka`、`pq_left_join_zero_rows` | join type/order、Gather 位置、结果 | 动态 metrics |
| Subquery/clone | `pq_subquery_correlated`、`pq_derived_view`、`pq_explain_analyze` | clone/refix 语义、Gather、subquery access | cost、actual time、row estimate |
| 生命周期/错误 | `pq_clone_item`、`refactor_fix_fields`、`pq_fallback`、`pq_record_buffer`、`pq_read_view`、`pq_kill`、`pq_kill_query`、`pq_worker_error`、`pq_mq_error` | clone cleanup、serial fallback、record-buffer、worker/read-view 生命周期、KILL/error 传播 | 不适用 |
| Fallback/negative | `pq_prepare`、`pq_sp_trigger`、`pq_not_support` | 无 PQ 或规定 fallback reason；语义与错误码 | 不适用 |
| Capability | `pq_not_support_dstore` | 无 DStore 时 DStore-only 用例 capability skip | 不适用 |

禁止使用 `--record` 自动接受计划差异；动态 EXPLAIN ANALYZE 指标可以使用明确、
最小范围的正则归一化，但不得匹配或替换表名、Gather、DOP、join/access path 或
fallback reason。

## 6. 验收矩阵

对 stable 与 refactor 必须执行同一逻辑矩阵，并记录成功数、失败数、framework skip
和用例自身 capability skip。每个失败都先与 stable 对照归类为基线/环境/重构回归。

| 构建 | 通用回归 | PQ 回归 | 额外要求 |
| --- | --- | --- | --- |
| Release | `--suite=main` | `--pq --suite=main,parallel_query` | Debug-only 用例应说明为预期 skip |
| Debug | `--suite=main` | `--pq --suite=main,parallel_query` | 覆盖 DBUG-only clone/fallback/error 路径 |
| ASAN | `--sanitize --suite=main` | `--sanitize --pq --suite=main,parallel_query` | `sanitize_report` 通过；不关闭 ODR 检测 |

矩阵必须在相应构建目录的 `mysql-test/` 下执行，使用以下命令：

```bash
./mtr --force --max-test-fail=0 --retry=0 --parallel=8 --suite=main
./mtr --force --max-test-fail=0 --retry=0 --parallel=8 \
  --pq --suite=main,parallel_query
```

ASAN 对应命令增加 `--sanitize`。每轮记录可执行/成功/失败数、framework skip、用例
自身 capability skip、skip 原因和日志摘要（或校验值）。所有矩阵默认
`--parallel=8`；对非确定性失败可使用 `--parallel=1 --repeat=N` 复现，但不得以
定向通过替代未记录的全量结果。Release 的 Debug-only PQ 用例为预期 skip；ASAN
是否执行它们取决于构建类型。本项目的 ASAN Debug 配置应执行，并与 stable 基线
逐项比较。

`--parallel=8` 只在单个 MTR 进程内创建 worker。**同一 build 目录同时只能运行一个
MTR 进程**：MTR 启动阶段会处理构建目录下的默认运行 `var/`，因此仅为第二个进程指定不同
`--vardir` 仍可能破坏第一个进程的运行目录。跨 build/worktree 并行时必须同时使用
不重叠的 `--mtr-port-base`；同一 build 内只能串行运行两条矩阵命令。

### 6.1 稳定基线隔离门禁

重构提交在提交前必须通过以下机械检查：

```bash
git diff --check 1b9ffd755d4..HEAD
git diff --name-only 1b9ffd755d4..HEAD
```

以下路径不得进入重构提交：

```text
mysql-test/include/have_dstore.inc
mysql-test/include/not_have_only_dstore.inc
libmysql/
sql-common/
testclients/
mysql-test/lib/mtr_cases.pm
mysql-test/mysql-test-run.pl
**/*.result-pq-debug
```

允许的文档只限 PQ spec。额外测试、稳定性或环境修复必须先提交到 `stable_branch`，
然后重新派生或回植，不能与 PQ context 重构混合。

### 6.2 性能对照合同

在同一台空闲机器、相同 Release 配置和相同数据集上，stable/refactor 各独立运行
至少 5 次，记录原始数据及 median/P95/P99。负载至少包括并行 scan、MQ/Gather 大
结果集、hash join/aggregate 和短查询并发。吞吐、P95、P99 的可接受回归阈值需由
项目确认；在阈值和实测数据均未取得前，性能结论只能标记为 `pending-validation`。

## 7. 审核结论与后续边界

独立审核覆盖了字段迁移完整性、生命周期、MQ 热路径、稳定基线隔离、计划测试与
设计文档一致性。静态代码和设计审核未发现已经证实的等价性缺陷；运行时等价性、
内存安全和性能结论以第 6 节完整验收记录为准。必须保留头内联 `is_pq_error()`。

截至本提交，已取得的针对性证据记录在
[`verification_evidence.md`](quality/verification_evidence.md)：Release `mysqld` 已完成
增量构建，符号审计未发现 `PQ_thd_context::is_error` wrapper，且 10 个覆盖 clone、
fallback、record buffer、read view、KILL 和 MQ/worker 错误路径的 Debug PQ 用例通过。
这些证据不替代第 6 节的完整矩阵或性能对照。

非阻断的后续事项：

1. 约束 `pq_context()` 的公开可变字段，逐步引入窄状态转换 API；
2. 仓外扩展代码若使用旧 `THD::PQ_EXECUTEION` 枚举，需要迁移到
   `PQ_clone_phase::EXECUTION`；仓内无旧引用；
3. 对对象尺寸变化完成可重复的 stable/refactor 性能和内存对照后，才能关闭性能
   验收项。
