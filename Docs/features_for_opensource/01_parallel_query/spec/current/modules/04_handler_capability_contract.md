# Parallel Query Handler 能力与扫描契约

> 模块 ID：`PQ-MOD-004`
> 状态：`current-contract`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 快照清单：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`；`working_tree=clean`
> source_tree_sha256：`a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> test_tree_sha256：`066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`
> 最后静态核对：2026-07-20
> 最后运行验证：见 `current/quality/verification_evidence.md`

哈希覆盖边界：`source_tree_sha256` 仅覆盖 manifest 定义的
`sql/parallel_query/**`，以及 `storage/innobase/include/row0pread_pq.h`、
`storage/innobase/row/row0pread_pq.cc`、`storage/innobase/handler/ha_innodb_pq.cc`。
它不单独 content-hash `sql/handler.cc`、`sql/handler.h`、`sql/table.cc`、
`sql/table.h`、`sql/sql_parse.cc`、`sql/sql_class.cc`、`sql/sql_class.h`、
`sql/sql_lex.h`、`sql/sql_select.cc` 或 TempTable 接入文件；这些和其他外部接入证据仅是
绑定 commit + bound PQ source/test tree 上的 `repository/path::QualifiedSymbol` 静态核对，不能据此
推断整个 workspace 或本模块全部 source evidence 具有可复现 content hash。

本文描述清单绑定 bound PQ source/test tree 中 SQL 层与 storage engine 之间的 as-is 契约。行为
状态只使用 manifest 的 `status_vocabulary`；源码观察的证据为 `static-evidence`，已找到
但未运行的测试证据为 `test-present`，运行验证为 `none`。

## 1. 职责与非目标

本模块负责：

- 定义 SQL 层调用的 `ha_pq_init/ha_pq_next/ha_pq_end` 包装器，以及 storage engine 必须
  实现的 leader init/end、worker init/next/end、dependent-ref range 构建接口。
- 在 optimizer 阶段限制可作为 DIV/CUT table 的引擎、表类型、partition 数量和 access
  path，避免把当前未知 handler 送入 PQ。
- 规定 engine-owned opaque context、共享 range/scan context、worker-local handler 状态
  的所有权和销毁顺序。
- 把 engine `HA_ERR_*`、EOF、generated column 和 MVI duplicate filtering 结果正确交回
  worker iterator。

本模块不决定是否值得使用 PQ（`PQ-MOD-001`），不克隆/改写 SQL 计划
（`PQ-MOD-002`），不调度或 join worker（`PQ-MOD-003`）。通用
`sql/parallel_query/pq_handler.h` 提供切片、队列和 context 基类，但 B+Tree cursor、MVCC、
record conversion 和具体错误转换仍由 engine 实现。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 入口 | `sql/handler.cc::handler::ha_pq_init` | leader 调用 engine init；成功把 `handler::inited` 设为 `PQ`，失败设为 `NONE` |
| 入口 | `sql/handler.cc::handler::ha_pq_next` | worker 从 engine context 取一行，更新 generated column、MVI filter 与 row status |
| 入口 | `sql/handler.cc::handler::ha_pq_end` | 结束底层 rnd/index，再按 `pq_ctx` 是否非空选择 leader end 或 worker end |
| 入口 | `sql/parallel_query/sql_parallel.cc::InitPQTab` | 配置 access 模式，调用 `ha_pq_init(dop,keyno)`，保存 `handler::pq_ctx` 到 `PQTab` |
| 入口 | `sql/parallel_query/pq_iterators.cc::PQblockScanIterator::Init`、`sql/parallel_query/pq_iterators.cc::PQblockScanIterator::Read`、`sql/parallel_query/pq_iterators.cc::PQblockScanIterator::End` | worker init/end 和 full/index/range scan 取行 |
| 入口 | `sql/parallel_query/pq_iterators.cc::PQRefIterator::Init`、`sql/parallel_query/pq_iterators.cc::PQRefIterator::Read`、`sql/parallel_query/pq_iterators.cc::PQRefIterator::End` | dependent ref key 变化时构建 ranges 并取行 |
| capability gate | `sql/table.cc::TABLE::suite_for_pq_division` | 只接受 InnoDB/TempTable、允许的临时表、单 used partition、非 fulltext |
| capability gate | `sql/parallel_query/pq_optimizer.cc::pq_check_not_support_tab` | 只接受当前 access type，并拒绝 dynamic range 和不合法 BKA/outer/semi inner division |
| engine API | `sql/handler.h::handler::pq_leader_scan_init`、`sql/handler.h::handler::pq_worker_scan_init`、`sql/handler.h::handler::pq_ref_build_ranges`、`sql/handler.h::handler::pq_worker_scan_next`、`sql/handler.h::handler::pq_leader_scan_end`、`sql/handler.h::handler::pq_worker_scan_end` | 派生 handler 的 virtual contract；进入 PQ 的引擎不得落到基类默认实现 |
| 输入 | `dop`、`keyno`、reverse/range/ref 配置 | leader plan 产生；engine 必须用同一配置建立共享 context |
| 输入 | `void *scan_ctx` | engine 创建、leader 拥有的 opaque pointer；worker 只借用且不得释放 |
| 输出 | row buffer 或 `HA_ERR_*` | 0 表示一行；EOF 和其他错误遵守 handler 既有返回约定 |
| 输出 | worker-local scan attachment | engine handler instance 拥有，`pq_worker_scan_end()` 必须可重复清理 |

当前找到的完整实现者是 `ha_innobase` 和 `temptable::Handler`；`ha_innopart` 覆盖
leader/worker init 并继承其余 InnoDB 行为。不能仅因为某 engine 继承 `handler` 就推断其
具备 PQ 能力。

## 3. 主流程

```text
traditional optimizer / PQ-MOD-001
  TABLE::suite_for_pq_division()
  pq_check_not_support_tab()
    | reject -> serial plan
    v
Gather_operator::init() on leader
  InitPQTab(PQTab, dop)
    configure table/index/range/ref/reverse flags
    handler::ha_pq_init(dop, keyno)
      engine::pq_leader_scan_init(keyno, scan_ctx, dop)
        build physical ranges / queues / shared error state
      success: handler::inited=PQ; handler::pq_ctx=scan_ctx
    PQTab::m_pq_ctx = handler::pq_ctx
    |
worker iterator Init()
  worker_handler::pq_worker_scan_init(keyno, leader_scan_ctx)
    attach worker-local Parallel_worker / cursor state
    |
Read loop
  handler::ha_pq_next(record, leader_scan_ctx)
    engine::pq_worker_scan_next()
    generated-column update
    MVI duplicate filtering
    TABLE row-status update
    |
worker iterator End()
  worker_handler::pq_worker_scan_end()         # idempotent
    |
leader ParallelScanIterator::End()
  detach MQ; wait terminal states; join all workers
    |
leader iterator destructor
  handler::ha_index_or_rnd_end()
    -> handler::ha_pq_end()
       -> engine::pq_leader_scan_end(scan_ctx)
  MQ cleanup
```

`PQRefIterator` 在每个新的 dependent key 首次读取前调用
`pq_ref_build_ranges(scan_ctx, Key_ref&)`，然后由 worker 从新 ranges 取行。当前 call site
忽略该函数的 int 返回值；在明确“无匹配”和“构建错误”的返回语义前，不能宣称错误已被
完整传播。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | worker-local/shared/borrowed | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| `handler::pq_ctx` raw pointer | engine `pq_leader_scan_init` | leader handler/engine | workers borrowed through `PQTab` | engine `pq_leader_scan_end` | 必须覆盖全部 worker init/read/end 和 thread join |
| `PQTab::m_pq_ctx` | `InitPQTab` 复制 raw pointer | Gather 仅保存引用 | borrowed | 不单独销毁 | 与 `handler::pq_ctx` 指向同一 engine object |
| `Parallel_leader_Base` 派生对象 | InnoDB/TempTable leader init | engine leader context | workers shared/borrowed | 对应 leader end | range queues、maps 和 error state 的顶层 owner |
| `PQ_Scan_ctx_Base` 派生对象 | engine leader context | `Parallel_leader_Base` 的 shared ownership 容器 | workers shared | 最后一个 shared owner | cursor/range 所需 engine state 必须比 `PQ_Ctx` 活得久 |
| `PQ_Range` | scan context partition | shared_ptr queues/maps | shared work item | shared_ptr 最后释放 | 一个 range 只能按 engine 调度规则被消费 |
| `PQ_Ctx_Base` 派生对象 | scan context/worker dispatch | worker shared_ptr/local queue | worker-local active context，引用 shared range | worker/end/queue clear | `read_record` 期间 scan/range owner 必须有效 |
| `Parallel_worker` | engine `pq_worker_scan_init` | worker handler/prebuilt shared_ptr | worker-local，借用 leader queues/maps | `pq_worker_scan_end` | repeated End 必须安全清空或保持空状态 |
| InnoDB `m_prebuilt->pq_ctx/pq_worker/pq_ref_info` | worker init/dispatch | worker handler/prebuilt | worker-local | `ha_innobase::pq_worker_scan_end` | key 改变、EOF、error 与 re-init 时不得引用旧 context |
| TempTable `cur_pq_ctx/pq_worker/pq_ref_info` | worker init/dispatch | TempTable worker handler | worker-local | `temptable::Handler::pq_worker_scan_end` | 同上；当前 division 只允许 full scan |
| handler record buffer/MVI unique filter | worker iterator/handler | worker TABLE/handler | worker-local | iterator End/destructor/handler cleanup | re-init 必须 reset，不得跨 worker 共享记录内存 |

leader `scan_ctx` 是 engine-created/leader-owned；传给 worker 的 `void *` 没有类型或引用计数
保护。`PQ-MOD-003` 必须先 join 全部 worker，之后 leader iterator 才能调用
`pq_leader_scan_end()`。颠倒这个顺序会让 worker 的 context、range 和队列指针悬空。

## 5. 并发、锁和状态机

handler 生命周期可归纳为：

```text
leader handler:
  NONE/INDEX/RND -> ha_pq_init -> PQ_ACTIVE
      init error -> NONE
      PQ_ACTIVE -> all workers joined -> ha_pq_end -> ENDED

worker handler:
  NONE -> pq_worker_scan_init -> PQ_ATTACHED
      -> dispatch/read context zero or more times
      -> pq_worker_scan_end -> DETACHED
      -> pq_worker_scan_end -> DETACHED           # 必须幂等
```

并发协调主要在 engine 的 `Parallel_leader_Base`、range/context queue、key map 和 error state
内部；generic handler wrapper 没有为 opaque `scan_ctx` 增加锁。必须由具体 engine 保证：

- leader 完成 range/context 建立后才发布给 worker；`ParallelScanIterator::Init()` 的顺序
  提供初始 happens-before。
- 多 worker dispatch 不得把同一不可共享 cursor 同时交给两个 worker；共享 range 对象的
  可变状态必须受 engine 同步保护。
- 一个 worker 设置的共享 engine error 必须让其他 worker 停止继续取行，并最终转换为
  leader 可观察的 `HA_ERR_*`/PQ error。
- reverse、key、range/ref 配置必须在发布前固定；worker 使用从 leader 复制的配置，不能
  读取正在变化的 leader handler 字段。
- worker cleanup 可从 MaterializeIterator 的重复 Init/End 路径多次进入，因此
  `pq_worker_scan_end()` 必须在未 open、已 end 或空 state 上安全返回。

KILL 由 worker 上层 iterator/THD 检查并驱动 detach；handler scan next 仍必须及时返回
已有 engine error/EOF。generic API 没有单独的 cancel callback。

## 6. 规范性不变量

- `PQ-HANDLER-INV-001`：optimizer MUST 只把当前明确允许的 engine/table/access 组合标为
  DIV/CUT；`PQ` hint MUST NOT 绕过 capability gate。
- `PQ-HANDLER-INV-002`：进入 PQ 的 concrete handler MUST 覆盖其路径可能调用的全部
  virtual PQ 方法，MUST NOT 落到基类 `assert(false); return 0` 默认实现。
- `PQ-HANDLER-INV-003`：成功 `ha_pq_init()` MUST 产生非空、类型匹配、可供 DOP workers
  使用的 leader context，并将 `handler::inited` 置为 PQ；失败 MUST 返回非零 `HA_ERR_*`
  且保持 `inited=NONE`。
- `PQ-HANDLER-INV-004`：worker MUST 在第一次 `ha_pq_next()` 前成功执行
  `pq_worker_scan_init()`，并在退出路径执行幂等 `pq_worker_scan_end()`。
- `PQ-HANDLER-INV-005`：engine scan context 和共享 range/map MUST 比最后一个 worker
  使用者活得更久；leader end MUST 发生在全部 worker join 之后。
- `PQ-HANDLER-INV-006`：`pq_worker_scan_next()` MUST 用 0 表示有效行，用
  `HA_ERR_END_OF_FILE` 表示耗尽，并用非零 handler 错误表示失败；MUST NOT 用成功返回掩盖
  未实现能力。
- `PQ-HANDLER-INV-007`：`ha_pq_next()` MUST 在成功取行后完成 generated-column 更新、
  MVI duplicate filtering 和 row-status 更新，与普通 handler wrapper 语义保持一致。
- `PQ-HANDLER-INV-008`：dependent ref 的 key/range 更新 MUST 在读取该 key 的首行之前完成；
  构建失败和“无工作”的返回语义 MUST 可被 caller 区分并传播。
- `PQ-HANDLER-INV-009`：partitioned table 只有一个 used partition 时 MAY 进入当前
  InnoDB partition PQ；多个 used partitions MUST 串行回退。
- `PQ-HANDLER-INV-010`：worker end 和 shared-info reset/set 的幂等要求 MUST 在具体 engine
  实现中保持，不能仅依赖 Debug assertion。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | 能否 fallback/retry | 必须清理的对象 |
|---|---|---|---|---|
| optimizer capability 拒绝 | `sql/table.cc::TABLE::suite_for_pq_division`、`sql/parallel_query/pq_optimizer.cc::pq_check_not_support_tab` | 正常串行执行 | serial-fallback | 无 handler PQ context |
| leader handler init 返回错误 | concrete engine 与 `sql/handler.cc::handler::ha_pq_init` | `sql/parallel_query/sql_parallel.cc::InitPQTab` 打印具体 handler 错误，PQ Init 失败 | 已越过计划 fallback 边界时不原地续跑；按上层错误契约处理 | 部分 engine leader/context/ranges，handler init state |
| worker handler init 返回错误 | concrete engine 与 `sql/handler.h::handler::pq_worker_scan_init` | 整个 PQ 执行失败 | 无运行期 serial retry | worker-local handler/prebuilt state、所有 peers、leader ctx |
| scan next 返回 EOF | concrete engine | 正常结束该 worker/range | 不需要 fallback | 当前 worker context 按 End 清理 |
| scan next 返回 deadlock/DDL/内部错误 | concrete engine 与 `sql/handler.cc::handler::ha_pq_next` | 传播具体 `HA_ERR_*`/SQL condition | 无运行期 serial retry | MQ detach、worker end/join、leader context |
| dependent-ref range 构建异常 | concrete engine | 当前 caller 返回消费语义不明确 | 当前行为 `unknown`；缺口类型为 `audit-candidate` | 新旧 ref context、range queues、worker attachment |
| worker repeated End | iterator/engine | 应安全返回，不能覆盖原始错误 | 不适用 | 已空的 worker pointers/buffers |
| leader end | concrete engine | 正常无客户端输出；错误按 handler cleanup 规则处理 | 不适用 | engine leader、scan contexts、ranges/maps、shared error state |

基类 virtual 默认实现当前在 Debug 中 `assert(false)`，而 Release 去除 assertion 后返回 0
或无动作。对 init/next/end 这会把“未实现”伪装为成功，属于静态确认的 fail-open 缺口，
不能把 optimizer 的硬编码 gate 当成充分的接口防线。

## 8. 支持、限制和 feature switch

| 能力 | 行为状态 | 当前边界 | 证据 | 缺口/Hardening |
|---|---|---|---|---|
| InnoDB full table/index scan | `implemented` | 正向和 reverse 配置进入 B+Tree range/context 切分 | `static-evidence` | `none` |
| InnoDB clustered/secondary range | `implemented` | 只接受单一 `INDEX_RANGE_SCAN` shape，MVCC/回表由 InnoDB 实现 | `static-evidence` | `none` |
| InnoDB static/dependent JT_REF | `partial` | dependent key 动态建 range；返回语义存在审计项 | `static-evidence` | dependent-ref `audit-candidate` |
| InnoDB partition | `partial` | 仅一个 used partition；`ha_innopart` 覆盖 init | `static-evidence` | `none` |
| InnoDB intrinsic/internal temp | `partial` | 受 table category、materialization 和 access path 限制 | `static-evidence` | `none` |
| TempTable full scan | `implemented` | 仅 internal TempTable full scan division | `static-evidence` | `none` |
| TempTable index/range/ref division | `serial-fallback` | optimizer 拒绝；engine init 也返回 `HA_ERR_UNSUPPORTED` | `static-evidence` | `none` |
| `DB_TYPE_DSTORE` table | `serial-fallback` | `sql/parallel_query/pq_optimizer.cc::JOIN::choose_parallel_tables` 明确拒绝 | `static-evidence` | `none` |
| 其他普通 storage engine | `serial-fallback` | 没有 capability 声明/override，当前不应进入 PQ | `static-evidence` | `PQ-GAP-HARD-001` |
| dynamic range / unsupported MRR shape | `serial-fallback` | optimizer access gate 拒绝 | `static-evidence` | `none` |
| BKA as DIV | `partial` | 只有 `pq_check_not_support_tab` 接受的特定 plan shape 可进入 | `static-evidence` | `none` |
| outer/semi inner as DIV | `serial-fallback` | 当前 division-table gate 拒绝 | `static-evidence` | `none` |
| fulltext / multi-used-partition / user transactional temp | `serial-fallback` | `TABLE::suite_for_pq_division` 拒绝 | `static-evidence` | `none` |
| generated column / MVI filter | `partial` | wrapper 有处理，组合覆盖有限 | `static-evidence` | `none` |
| generic engine capability query | `unknown` | 当前没有显式“支持哪些 PQ operation”的协商接口 | `static-evidence` | capability `audit-candidate` |

handler 能力没有独立 feature switch；全局 PQ 开关和 optimizer support flags 只控制候选，
不能让一个缺少 virtual override 的 engine 变成可用实现。

## 9. 可观测性

- `handler::ha_pq_init` 的 DBUG 点 `ha_pq_init_fail` 可返回
  `HA_ERR_TABLE_DEF_CHANGED`，但本次静态搜索未找到对应 MTR 使用。
- `sql/handler.cc::handler::ha_pq_next` 的 `ha_pq_next_deadlock` 被
  `mysql-test/suite/parallel_query/t/pq_worker_error.test` 使用，对应期望输出为
  `mysql-test/suite/parallel_query/r/pq_worker_error.result-pq`；资产意图是证明具体
  `HA_ERR_LOCK_DEADLOCK` 能穿过 worker diagnostics 到客户端，本任务未运行该用例。
- `InitPQTab` 调用 `handler::print_error()`；错误日志/客户端错误应保留 engine code，只有
  缺少具体 condition 时才由上层产生通用 PQ 错误。
- EXPLAIN/optimizer trace 可证明某 table 是否被选为 DIV/CUT，但不能证明具体 engine
  override 被调用；需要 DBUG、handler status 或运行堆栈辅助。
- PQ memory/thread status 在 handler init、scan error、KILL 和 end 前后应回落；这仍需运行
  fault test，当前证据不是 `verified`。

## 10. 代码与测试映射

下表逐项列出真实测试输入和结果路径；这些文件存在，但本任务没有执行它们。

| Requirement/Invariant | repository/path::QualifiedSymbol | 正向测试资产 | 负向/故障测试资产 | 行为状态 | 证据 | 缺口/Hardening |
|---|---|---|---|---|---|---|
| InnoDB table/index scan（INV-003/004/005/006） | `storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_leader_scan_init`、`storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_worker_scan_init`、`storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_worker_scan_next`、`storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_worker_scan_end` | `mysql-test/suite/parallel_query/t/pq_fullscan.test`、`mysql-test/suite/parallel_query/r/pq_fullscan.result-pq` | `mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| clustered/secondary range | `storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_range_scan_init`、`storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_worker_scan_next` | `mysql-test/suite/parallel_query/t/pq_range_clust.test`、`mysql-test/suite/parallel_query/r/pq_range_clust.result-pq`、`mysql-test/suite/parallel_query/t/pq_range_sec.test`、`mysql-test/suite/parallel_query/r/pq_range_sec.result-pq` | `mysql-test/suite/parallel_query/t/pq_range_exception.test`、`mysql-test/suite/parallel_query/r/pq_range_exception.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| static/dependent ref（INV-008） | `sql/parallel_query/pq_iterators.cc::PQRefIterator::Read`、`storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_ref_build_ranges` | `mysql-test/suite/parallel_query/t/pq_jt_ref.test`、`mysql-test/suite/parallel_query/r/pq_jt_ref.result-pq`、`mysql-test/suite/parallel_query/t/pq_depend_ref.test`、`mysql-test/suite/parallel_query/r/pq_depend_ref.result-pq`、`mysql-test/suite/parallel_query/t/pq_ref_build_range.test`、`mysql-test/suite/parallel_query/r/pq_ref_build_range.result-pq` | `mysql-test/suite/parallel_query/t/pq_range_exception.test`、`mysql-test/suite/parallel_query/r/pq_range_exception.result-pq` | `partial` | `static-evidence`、`test-present` | dependent-ref `audit-candidate` |
| internal temp / TempTable | `storage/temptable/src/handler_pq.cc::temptable::Handler::pq_leader_scan_init`、`storage/temptable/src/handler_pq.cc::temptable::Handler::pq_worker_scan_next` | `mysql-test/suite/parallel_query/t/pq_innodb_intrinsic.test`、`mysql-test/suite/parallel_query/r/pq_innodb_intrinsic.result-pq` | `mysql-test/suite/parallel_query/t/pq_not_support.test`、`mysql-test/suite/parallel_query/r/pq_not_support.result-pq` | `partial` | `static-evidence`、`test-present` | `none` |
| one-used-partition（INV-009） | `sql/table.cc::TABLE::suite_for_pq_division`、`storage/innobase/handler/ha_innodb_pq.cc::ha_innopart::pq_leader_scan_init` | `mysql-test/suite/parallel_query/t/pq_partition.test`、`mysql-test/suite/parallel_query/r/pq_partition.result-pq` | `mysql-test/suite/parallel_query/t/pq_partition.test`、`mysql-test/suite/parallel_query/r/pq_partition.result-pq` | `partial` | `static-evidence`、`test-present` | `none` |
| wrapper row/error semantics（INV-006/007） | `sql/handler.cc::handler::ha_pq_next` | `mysql-test/suite/parallel_query/t/pq_fullscan.test`、`mysql-test/suite/parallel_query/r/pq_fullscan.result-pq`、`mysql-test/suite/parallel_query/t/pq_range_sec.test`、`mysql-test/suite/parallel_query/r/pq_range_sec.result-pq` | `mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq`，使用 `ha_pq_next_deadlock` 注入 | `implemented` | `static-evidence`、`test-present` | `none` |
| worker cleanup 幂等（INV-004/010） | `storage/innobase/handler/ha_innodb_pq.cc::ha_innobase::pq_worker_scan_end`、`storage/temptable/src/handler_pq.cc::temptable::Handler::pq_worker_scan_end` | `mysql-test/suite/parallel_query/t/pq_tempory_table_release.test`、`mysql-test/suite/parallel_query/r/pq_tempory_table_release.result-pq` | `mysql-test/suite/parallel_query/t/pq_mq_error.test`、`mysql-test/suite/parallel_query/r/pq_mq_error.result-pq`、`mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `partial` | `static-evidence`、`test-present` | repeated cleanup `audit-candidate` |
| unsupported engine/access gate（INV-001/002） | `sql/table.cc::TABLE::suite_for_pq_division`、`sql/parallel_query/pq_optimizer.cc::pq_check_not_support_tab`、`sql/handler.h::handler::pq_leader_scan_init` | `mysql-test/suite/parallel_query/t/pq_not_support.test`、`mysql-test/suite/parallel_query/r/pq_not_support.result-pq` | `none`；未发现直接调用基类默认 virtual 的组件测试 | `partial` | `static-evidence`、`test-present` | `PQ-GAP-HARD-001` |

## 11. 已知缺口和变更影响

当前 `conformance_gaps.md` 已登记 fail-closed 和测试证据缺口：

- `PQ-GAP-HARD-001` — 行为状态 `partial`；证据 `static-evidence`；fail-closed 是 hardening
  target。`handler` 的 PQ virtual 基类默认实现使用
  `assert(false)` 后返回成功值；Release 构建 fail-open，Debug 构建 abort。默认实现应统一
  fail-closed，并增加不支持 engine/fake handler 的契约测试。
- `PQ-GAP-HARD-010` — 当前 handler 正向和故障 MTR 均为 `test-present`，不能作为
  本快照的运行通过证据。
- 未单独登记的 capability 审计项 — 行为状态 `unknown`；证据 `static-evidence`；缺口类型
  为 `audit-candidate`。当前没有显式 capability query，optimizer 依赖
  engine type 和 access shape 硬编码。扩展 engine 或放宽 RBO 时容易遗漏某个 required
  virtual 或 idempotent cleanup，需要建立 capability-to-operation 映射并集中登记。
- 未单独登记的 dependent-ref 审计项 — 行为状态 `unknown`；证据 `static-evidence`；缺口
  类型为 `audit-candidate`。`pq_ref_build_ranges()` 返回 int，但
  `PQRefIterator::Read()` 和 InnoDB 初始化中的调用未消费返回值；返回值可能同时承载“无工作”
  与失败语义，需先定约、在集中 gap 表分配 ID，再增加 fault propagation 测试。

### 关联文件（读者导航；下列完整文件路径不构成 stable mapping）

- `sql/handler.h`、`sql/handler.cc`、`sql/table.h`、`sql/table.cc`、
  `sql/parallel_query/pq_handler.h`、`sql/parallel_query/pq_handler.cc`、
  `sql/parallel_query/pq_iterators.h`、`sql/parallel_query/pq_iterators.cc`、
  `sql/parallel_query/pq_optimizer.cc`、`sql/parallel_query/sql_parallel.cc`。
- `storage/innobase/handler/ha_innodb_pq.cc`、`storage/innobase/handler/ha_innodb.h`、
  `storage/innobase/handler/ha_innopart.h`、`storage/temptable/include/temptable/handler.h`、
  `storage/temptable/src/handler_pq.cc`。
- `01_current_support_matrix.md`、其他模块文档和 `conformance_gaps.md`；新增 engine/
  access path 必须同时声明 capability、错误码、context owner、幂等 cleanup 和负向测试。
- 至少重跑第 10 节列出的全部精确 `.test` 输入并核对对应 `.result-pq`，并新增基类
  fail-closed 与 `ha_pq_init_fail` 覆盖。
