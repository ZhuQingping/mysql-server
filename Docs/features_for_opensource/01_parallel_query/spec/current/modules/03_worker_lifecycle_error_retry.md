# Parallel Query Worker 生命周期、错误与重试契约

> 模块 ID：`PQ-MOD-003`
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

本文描述清单绑定 bound PQ source/test tree 的 as-is 契约。行为状态只使用 manifest 的
`status_vocabulary`；源码观察的证据为 `static-evidence`，已找到但未运行的测试证据为
`test-present`，运行验证为 `none`。

## 1. 职责与非目标

本模块负责从改写后的 `ParallelScanIterator` 开始，初始化共享输入和 exchange、创建
worker 线程、构造 worker-local 计划、复制一致性读快照、执行、传播 KILL/错误、detach
MQ、等待所有已创建线程到达终态并 join。它还定义两类失败的分界：

- PQ 计划/初始化阶段的 `ER_PARALLEL_FAIL_INIT` 可以在严格前置条件下重新 parse、优化并
  串行执行整条语句。
- worker/handler/MQ 运行期错误不能从中间切回串行 iterator，也不属于自动重试入口。

本模块不选择 PQ 或 DOP（`PQ-MOD-001`），不克隆 template 和改写 leader
（`PQ-MOD-002`），不定义存储引擎 `PQ_Ctx` 的扫描实现（`PQ-MOD-004`）。它也不保证
任意插件线程初始化失败都会自动转成终态；该路径是本快照的审计项。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 入口 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::Init` | 按固定顺序初始化共享物化、子查询、MQ、handler range、leader snapshot 和 worker |
| 入口 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker` | 顺序创建 DOP 个线程，并等待每个已创建线程报告首次可观察状态 |
| 入口 | `sql/parallel_query/sql_parallel.cc::pq_worker_exec` | worker 线程主函数：建图、snapshot、READY、执行、OVER、清理、终态 |
| 入口 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::End` | detach、等待并 join 所有已创建 worker，再合并错误 |
| 入口 | `sql/sql_class.cc::THD::awake` | 将 leader KILL 传播给 Gather 中已经 READY 的 worker |
| 入口 | `sql/sql_parse.cc::retry_without_parallel_query` | 对精确的初始化错误执行一次完整的无 PQ redispatch |
| 输入 | `Gather_operator`、template JOIN、`PQ_worker_manager[DOP]` | 由 leader/Gather 拥有，必须覆盖全部线程生命周期 |
| 输入 | 每 worker 的 `MQueue_handle` | exchange 创建并分配；worker/leader 共享 detach 状态 |
| 输入 | leader THD、read view、KILL 与诊断状态 | worker 仅借用 leader 指针；snapshot 复制失败必须终止 worker |
| 输出 | MQ 数据或 `ERROR_MSG` | 正常数据由 exchange 消费；错误先通知 MQ 并最终合并到 leader diagnostics |
| 输出 | worker 终态和 joined thread handle | 每个成功创建的线程必须到达 `COMPELET` 或 `ERROR` 并被 join |
| 输出 | 客户端 SQL/handler 错误 | 优先传播具体 worker/handler 错误；无具体错误时使用 `ER_PARALLEL_QUERY_ERROR` |

源码枚举 `PQ_worker_state` 的拼写和数值是：`INIT=1`、`READY=2`、
`COMPELET=4`、`ERROR=8`、`OVER=16`。文档保留 `COMPELET` 的当前拼写，避免把不存在的
symbol 当成接口。

## 3. 主流程

执行初始化的直接顺序为：

```text
ParallelScanIterator::Init()
  for each query block:
    force_create_iterators() when needed
    para_exec_init()                         # shared materialized inputs
  exec_scalar_uncorrelated_subquery()        # leader evaluates shared scalar input
  pq_init_record_gather()                    # MQ/exchange and handles
  Gather_operator::init()
    -> handler::ha_pq_init()                 # engine builds shared scan ranges/context
  pq_create_innodb_snapshot(leader)
  pq_launch_worker()
```

worker 创建和执行：

```text
for worker slot i in [0, dop):
  state = INIT
  mysql_thread_create(pq_worker_exec, manager[i])
  if create failed:
    detach that slot's MQ
    continue
  leader wait READY | COMPELET | ERROR | OVER
  if ERROR or injected partial-launch failure:
    mark active workers pq_error
    stop launching

pq_worker_exec(manager)
  my_thread_init()
    failure -> my_thread_exit() -> EXIT_WITHOUT_STATUS  # current static risk
  make_pq_worker_plan()                      # clone from template
  pq_clone_innodb_snapshot(worker, leader)   # when worker graph uses base InnoDB
  signal READY
  copy leader killed state
  ExecuteIteratorQuery()
  signal OVER
  on error:
    leader->pq_error = true
    send_exception_msg(ERROR_MSG)
  detach MQ
  cleanup Query_result / JOIN
  under template THD::LOCK_thd_query:
    merge status, examined_rows and Diagnostics_area
  free worker THD
  signal COMPELET or ERROR
  my_thread_end()/my_thread_exit()
```

结束路径：

```text
ParallelScanIterator::End()
  pq_wait_workers_finished()
    detach every MQ handle
    wait active created threads for COMPELET | ERROR
    my_thread_join() every created thread handle
  pq_error_code()
    preserve HA_ERR_TABLE_DEF_CHANGED special case
    send KILL message when killed
    merge template diagnostics/status into leader
    propagate specific error, otherwise ER_PARALLEL_QUERY_ERROR

ParallelScanIterator::~ParallelScanIterator()
  table()->file->ha_index_or_rnd_end()
  MQ_record_gather::mq_scan_end()
```

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | worker-local/shared/borrowed | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| `PQ_worker_manager` | `make_pq_gather_operator` | Gather/leader PQ MEM_ROOT | leader/worker shared control block | `pq_free_gather` | 必须晚于对应 thread join |
| `my_thread_handle` | `mysql_thread_create` | manager slot | leader-owned handle | `my_thread_join` | 每个非零 handle 必须且只须 join 一次 |
| worker `THD` 对象 | worker plan clone，内存来自 template THD PQ root | worker cleanup；template root 提供存储 | worker-local；含 borrowed leader/Gather pointers | `pq_free_thd` | 必须在终态 signal 前完成诊断合并和本地资源释放 |
| worker Query_block/JOIN/iterator tree | `make_pq_worker_plan` | worker THD/root | worker-local，借用 shared contexts | `pq_free_join` | 退出 barriers、handler 和 MQ 使用后才能销毁 |
| leader THD pointer | manager/template copy | 外部连接线程 | borrowed by worker | 连接/statement owner | 所有 worker join 前不得销毁 |
| `MQueue_handle` | leader exchange 初始化 | exchange/Gather | leader/worker shared | MQ scan/exchange cleanup | detach 状态必须在等待 worker 前可见，避免生产者永久阻塞 |
| template Diagnostics_area | Gather/template THD | Gather | shared aggregation target | Gather cleanup | worker 写入时必须持有 `LOCK_thd_query` |
| leader/template/worker read view | InnoDB snapshot helpers | 各 THD/事务对象 | leader source、worker-local clone | 事务/THD cleanup | worker 读取基表前必须复制成功或报错 |
| hash join shared context/barriers | Gather | Gather | workers shared | Gather destroy | 未启动 worker 必须 `ArriveAndDrop`，已启动 worker 必须参与或退出 barrier |
| handler PQ context/ranges | `Gather_operator::init` / engine | leader handler/engine | workers borrowed | leader handler end | 所有 worker join 后才可释放，详见 `PQ-MOD-004` |

worker 完成后先清理本地 Query_result/JOIN，再在 template 诊断区合并状态，最后释放 worker
THD 并发终态信号。leader 只有 join 全部已创建线程后才能销毁 manager、Gather、共享
hash context 或 handler scan context。

## 5. 并发、锁和状态机

正常发布路径与当前静态风险：

```text
INIT -> READY -> OVER -> COMPELET
  \       \       \
   +-------+-------+-> ERROR

INIT -. my_thread_init() failure .-> EXIT_WITHOUT_STATUS
```

`OVER` 表示 `ExecuteIteratorQuery()` 已返回，但本地资源清理和诊断合并尚未结束；它不是
终态。`COMPELET` 与 `ERROR` 是 leader 可 join 前等待的终态。由于 `signal_status()` 是
赋值而非累积 bit，等待代码把多个枚举值按位 OR 只作为“任一状态”掩码使用。

`EXIT_WITHOUT_STATUS` 不是 `PQ_worker_state` 枚举值，而是对当前
`sql/parallel_query/sql_parallel.cc::pq_worker_exec` 静态分支的风险标记：
`my_thread_init()` 失败时函数在取得 manager 并调用 `signal_status()` 之前退出。结合
`PQ_worker_manager::wait_for_status()` 无总 deadline，bounded termination 的当前行为是
`unknown`，不能宣称 INV-002 已由现实现满足。

同步契约：

- `PQ_worker_manager::signal_status()` 在 `m_mutex` 下写 `m_status/thd_worker` 并 signal
  `m_cond`；`wait_for_status()` 在同一 mutex 下循环读取，形成状态发布的 happens-before。
- `wait_for_status()` 每次做 5 秒 timed wait，但没有总 deadline，也不因单次 timeout 返回；
  只有观察到目标状态才退出，返回值仅区分最终是否 `ERROR`。
- leader 顺序 launch：第 i 个 worker 报告 READY/OVER/终态后才创建第 i+1 个，因而同一时刻
  只有一个 worker 在初始 `make_pq_worker_plan()` 区段内。
- worker 合并诊断、rewritten query 相关状态和累计 examined rows 时持有 template THD 的
  `LOCK_thd_query`。
- MQ detach 必须先于等待终态，以解除 worker 发送数据时的反压等待。
- `THD::awake()` 遍历 `THD::pq_gathers`，只对 thread 已创建、active、状态为 READY 且
  `thd_worker` 非空的槽调用 `set_kill_state()`，随后 leader 自身进入 killed 状态。
- manager 的部分字段在 launch/join 路径存在直接访问；当前静态核查没有证明所有读写都由
  `m_mutex` 覆盖，改变状态发布方式时必须整体审计，而不能只改单个 accessor。

## 6. 规范性不变量

- `PQ-WORKER-INV-001`：`ParallelScanIterator::Init()` MUST 保持共享物化、标量子查询、
  MQ、handler context、leader snapshot、worker launch 的当前依赖顺序。
- `PQ-WORKER-INV-002`（hardening target，当前行为 `unknown`）：每个成功创建的 worker
  thread MUST 发布一个可观察的首次状态，并最终发布 `COMPELET` 或 `ERROR`；leader MUST
  join 每个非零 thread handle。`my_thread_init()` 失败的当前早退分支尚未满足“发布终态”
  的静态证明要求。
- `PQ-WORKER-INV-003`：`READY` MUST 仅在 worker plan 和所需 snapshot 已成功建立后发布；
  leader/worker 对 `thd_worker` 的使用 MUST 发生在该发布之后。
- `PQ-WORKER-INV-004`：`OVER` MUST NOT 被当成资源已释放的终态；销毁 manager/Gather
  前 MUST 等待 `COMPELET/ERROR` 并 join。
- `PQ-WORKER-INV-005`：任何 worker 执行错误 MUST 设置 leader PQ error、detach MQ，
  并使同组 worker 能停止；MUST NOT 把已经失败的并行执行报告为正常完整结果。
- `PQ-WORKER-INV-006`：leader 提前满足 LIMIT、发生错误或 KILL 时 MUST 先 detach 全部
  MQ，再等待 worker，避免 worker 永久阻塞在生产路径。
- `PQ-WORKER-INV-007`：worker diagnostics 合并 MUST 持有 `LOCK_thd_query`，具体 SQL/
  handler 错误 MUST 优先于通用 `ER_PARALLEL_QUERY_ERROR`。
- `PQ-WORKER-INV-008`：只有非 fatal、未 killed、精确为 `ER_PARALLEL_FAIL_INIT`、DML、
  且 `!lex->is_exec_completed()` 的语句 MAY 自动串行 retry。
- `PQ-WORKER-INV-009`：retry MUST 重新 parse、prepare、optimize 和 execute 整条语句，
  MUST 设置 `no_pq/retry_without_pq`，MUST NOT 从失败的 PQ iterator 中点继续。
- `PQ-WORKER-INV-010`：运行期 worker/MQ/handler 错误 MUST NOT 进入
  `retry_without_parallel_query()`；客户端可能已观察执行副作用或结果协议状态。
- `PQ-WORKER-INV-011`：statement cleanup MUST 释放准入阶段预留的线程额度、清空 PQ
  MEM_ROOT 和 `pq_gathers`，并重置本语句 PQ 状态。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | 能否 fallback/retry | 必须清理的对象 |
|---|---|---|---|---|
| point of no return 后的 PQ 计划/初始化失败 | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan`、`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | 第一次产生 `ER_PARALLEL_FAIL_INIT` | 满足 INV-008 时整语句串行 retry | 第一次尝试的计划、PQ root、诊断和状态 |
| worker thread create 全部失败 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker` | Init 失败，最终传播 PQ 错误 | 无运行期自动 retry | 每槽 MQ detach；所有非零 handle join；barrier participant drop |
| thread 已创建但 `my_thread_init()` 失败 | `sql/parallel_query/sql_parallel.cc::pq_worker_exec` | leader 可能持续等待状态；bounded termination 为 `unknown` | 不得作为已支持 fallback/retry | 需要 hardening 令 manager 获得 ERROR 终态、MQ detach 且 thread 可 join |
| 部分 worker create/启动失败 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker`、`sql/parallel_query/sql_parallel.cc::pq_worker_exec` | 整个并行执行失败 | 无运行期自动 retry | 未启动槽 detach，已启动 worker 标错、detach、终态、join |
| worker plan/snapshot 失败 | `sql/parallel_query/sql_parallel.cc::pq_worker_exec` | 合并具体诊断或通用 PQ 错误 | 无运行期自动 retry | MQ、worker JOIN/THD、snapshot、本地 result、所有 peers |
| handler/MQ/range 运行错误 | worker iterator/engine/exchange | 传播具体 handler 错误；无具体条件时 `ER_PARALLEL_QUERY_ERROR` | 无运行期自动 retry | handler ctx、MQ、worker tree、线程、共享输入 |
| `HA_ERR_TABLE_DEF_CHANGED` | Gather/handler | 保留该 handler 错误供上层既有逻辑处理 | 由上层表定义变化策略决定，不是 PQ retry | 同上 |
| KILL QUERY | `sql/sql_class.cc::THD::awake` 与 workers | `ER_QUERY_INTERRUPTED` 等 kill 语义 | 不 retry | 全 MQ detach、全部线程终态/join、handler/Gather |
| leader 正常提前停止读取 | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::End` | 正常上层语义，例如 LIMIT | 不需要 fallback | detach 未消费 MQ，等待/join，随后 handler/MQ cleanup |

retry 实现会清除第一次错误，重建 statement PSI、digest 和 parser state，暂时关闭 general
log，把 `parallel_cost_threshold` 设为 `ULONG_MAX`，设置 `no_pq=true` 和
`retry_without_pq=true`，再调用 `dispatch_sql_command(..., clean_pq_variables=true)`；返回后
恢复临时会话变量。第二次执行由 `mysql_execute_command` 产生相应 retry warning。该流程是
独立的新执行尝试，不延长旧 worker 或旧 handler context 的生命周期。

## 8. 支持、限制和 feature switch

| 能力 | 行为状态 | 当前边界 | 证据 | 缺口/Hardening |
|---|---|---|---|---|
| pthread 风格 worker 创建 | `implemented` | 使用 `mysql_thread_create`；顺序等待 READY，非 thread-pool dispatch | `static-evidence` | `PQ-GAP-CONF-003` |
| 部分 launch 失败处理 | `partial` | 未创建槽 detach；已创建槽进入错误清理，但 thread-init 早退不发布状态 | `static-evidence` | `PQ-GAP-HARD-005` |
| worker snapshot clone | `partial` | worker 图包含 base InnoDB 时复制 leader read view；失败终止 worker | `static-evidence` | `none` |
| MQ detach 与错误消息 | `implemented` | 进入 worker 通用错误路径时发送 `ERROR_MSG`，leader End 先 detach 全部 handle | `static-evidence` | thread-init 早退不进入该路径 |
| KILL 已 READY worker | `implemented` | `set_kill_state()` 可写已 READY worker THD | `static-evidence` | `none` |
| pre-READY KILL 收敛 | `unknown` | pre-READY 不能通过 `set_kill_state()` 直接写 worker THD | `static-evidence` | `PQ-GAP-HARD-005`、`audit-candidate` |
| worker wait deadline | `unknown` | 单次 5 秒 timed wait，但没有总 deadline | `static-evidence` | `PQ-GAP-HARD-005`、`audit-candidate` |
| thread-init failure bounded termination | `unknown` | `INIT` 可进入风险标记 `EXIT_WITHOUT_STATUS` | `static-evidence` | `PQ-GAP-HARD-005`、hardening target |
| 初始化失败串行 retry | `partial` | 只接受精确 `ER_PARALLEL_FAIL_INIT` 且满足 DML/未开始执行等条件 | `static-evidence` | `none` |
| worker 运行期 retry | `rejected` | 不重试，也不从中点串行续跑 | `static-evidence` | `none` |
| EXPLAIN ANALYZE worker timing | `partial` | worker OVER 后汇总 timing；运行验证为 none | `static-evidence` | `none` |

`parallel_fail_retry` 默认开启但不保证任何错误都会重试；`parallel_graceful_fallback` 控制
更早的计划克隆阶段，两个开关的边界不能合并解释。

## 9. 可观测性

- worker 错误首先通过 MQ `ERROR_MSG` 和 detach 状态唤醒 leader，随后由 template
  Diagnostics_area 合并具体 SQL condition；无具体 condition 时才生成
  `ER_PARALLEL_QUERY_ERROR`。
- `ER_PARALLEL_FAIL_INIT` 加 retry warning 表示发生过计划/初始化失败并重新执行；最终成功
  结果本身不能证明第一次没有失败。
- KILL 测试可观察 worker thread 状态、客户端 `ER_QUERY_INTERRUPTED` 以及 PQ 资源计数是否
  回落。
- `PQ_memory`、running thread status 和 handler 计数应在正常、部分 launch、错误、KILL
  四类路径前后对比，单个时点的非零值不能独立证明泄漏。
- `OVER` 是内部过渡状态，外部状态采样不得把它统计为已完成且可销毁。

## 10. 代码与测试映射

下表逐项列出真实测试输入和结果路径；这些文件存在，但本任务没有执行它们。

| Requirement/Invariant | repository/path::QualifiedSymbol | 正向测试资产 | 负向/故障测试资产 | 行为状态 | 证据 | 缺口/Hardening |
|---|---|---|---|---|---|---|
| Init 顺序与正常 worker 生命周期（INV-001/003/004） | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::Init`、`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker`、`sql/parallel_query/sql_parallel.cc::pq_worker_exec` | `mysql-test/suite/parallel_query/t/pq_fullscan.test`、`mysql-test/suite/parallel_query/r/pq_fullscan.result-pq` | `mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq` | `partial` | `static-evidence`、`test-present` | `PQ-GAP-HARD-005` |
| 全部/部分 launch 失败与 join（INV-002/006） | `sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker`、`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_wait_workers_finished` | `mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq` | `mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq`，使用 `pq_worker_error8/pq_worker_error9` 注入 | `partial` | `static-evidence`、`test-present` | `PQ-GAP-HARD-005` |
| thread-init failure 终态发布（INV-002） | `sql/parallel_query/sql_parallel.cc::pq_worker_exec`、`sql/parallel_query/sql_parallel.cc::PQ_worker_manager::wait_for_status`、`sql/parallel_query/sql_parallel.cc::PQ_worker_manager::signal_status` | `none` | `none`；未找到 `my_thread_init()` failure fault injection | `unknown` | `static-evidence` | `PQ-GAP-HARD-005`、hardening target |
| worker/hash barrier 错误（INV-005/006） | `sql/parallel_query/sql_parallel.cc::pq_worker_exec`、`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_launch_worker` | `mysql-test/suite/parallel_query/t/pq_hash_join_error.test`、`mysql-test/suite/parallel_query/r/pq_hash_join_error.result-pq` | `mysql-test/suite/parallel_query/t/pq_hash_join_error.test`、`mysql-test/suite/parallel_query/r/pq_hash_join_error.result-pq`、`mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq` | `partial` | `static-evidence`、`test-present` | `PQ-GAP-HARD-005` |
| MQ detach/cleanup（INV-005/006） | `sql/parallel_query/sql_parallel.cc::MQ_record_gather::mq_scan_end`、`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_wait_workers_finished` | `mysql-test/suite/parallel_query/t/pq_leader_exception.test`、`mysql-test/suite/parallel_query/r/pq_leader_exception.result-pq` | `mysql-test/suite/parallel_query/t/pq_mq_error.test`、`mysql-test/suite/parallel_query/r/pq_mq_error.result-pq`、`mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `partial` | `static-evidence`、`test-present` | thread-init early exit `audit-candidate` |
| KILL 传播（INV-003/006） | `sql/sql_class.cc::THD::awake`、`sql/parallel_query/sql_parallel.cc::PQ_worker_manager::set_kill_state` | `mysql-test/suite/parallel_query/t/pq_kill.test`、`mysql-test/suite/parallel_query/r/pq_kill.result-pq` | `mysql-test/suite/parallel_query/t/pq_kill_query.test`、`mysql-test/suite/parallel_query/r/pq_kill_query.result-pq` | `partial` | `static-evidence`、`test-present` | `PQ-GAP-HARD-005` |
| 具体 worker/handler 错误传播（INV-007/010） | `sql/parallel_query/sql_parallel.cc::pq_worker_exec`、`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::pq_error_code` | `mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq` | `mysql-test/suite/parallel_query/t/pq_range_exception.test`、`mysql-test/suite/parallel_query/r/pq_range_exception.result-pq`、`mysql-test/suite/parallel_query/t/pq_worker_error.test`、`mysql-test/suite/parallel_query/r/pq_worker_error.result-pq` | `partial` | `static-evidence`、`test-present` | `none` |
| 初始化错误整语句 retry（INV-008/009/010） | `sql/sql_parse.cc::retry_without_parallel_query` | `mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test`、`mysql-test/suite/parallel_query/r/pq_auto_retry_failed_parallel_query.result-pq` | `mysql-test/suite/parallel_query/t/refactor_fix_fields.test`、`mysql-test/suite/parallel_query/r/refactor_fix_fields.result-pq` | `partial` | `static-evidence`、`test-present` | `none` |
| 临时对象与 statement cleanup（INV-011） | `sql/sql_class.cc::THD::cleanup_after_query`、`sql/parallel_query/pq_iterators.cc::ParallelScanIterator::~ParallelScanIterator` | `mysql-test/suite/parallel_query/t/pq_tempory_table_release.test`、`mysql-test/suite/parallel_query/r/pq_tempory_table_release.result-pq` | `mysql-test/suite/parallel_query/t/pq_mq_error.test`、`mysql-test/suite/parallel_query/r/pq_mq_error.result-pq`、`mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `partial` | `static-evidence`、`test-present` | repeated End `audit-candidate` |

## 11. 已知缺口和变更影响

当前 `conformance_gaps.md` 已集中登记本模块的 bounded-termination 与证据缺口：

- `PQ-GAP-HARD-005` — 行为状态 `unknown`；证据 `static-evidence`；缺口类型为
  `audit-candidate`，bounded termination 是 hardening target。`pq_worker_exec()` 在
  `my_thread_init()` 失败时直接 `my_thread_exit()`，尚未取得 manager 也未发布终态；leader
  `wait_for_status()` 没有总 deadline，组合路径具有永久等待风险，需要可控 fault injection
  验证并定义终态发布责任。同一 GAP 也覆盖 `set_kill_state()` 只处理 READY worker、且
  wait 没有全局 deadline；worker 正常会在 READY 后复制 leader kill，但 worker plan build/
  snapshot 长时间停滞时的 KILL 收敛需要确定性测试。
- `PQ-GAP-HARD-010` — 现有 MTR 资产没有绑定本快照的统一运行报告；此外未找到覆盖
  repeated `End()`/cleanup re-entry、
  以及客户端在部分结果协议期间断连的确定性 MTR；现有 kill/MQ 测试不能替代这些销毁顺序
  场景。

### 关联文件（读者导航；下列完整文件路径不构成 stable mapping）

- `sql/parallel_query/pq_iterators.h`、`sql/parallel_query/pq_iterators.cc`、
  `sql/parallel_query/sql_parallel.h`、`sql/parallel_query/sql_parallel.cc`、MQ/exchange、
  hash join barrier、`sql/sql_class.h`、`sql/sql_class.cc`、`sql/sql_parse.cc` 和 InnoDB
  snapshot helpers。
- `02_plan_rewrite_clone_ownership.md` 的 point of no return、
  `04_handler_capability_contract.md` 的 handler cleanup 以及 `conformance_gaps.md`。
- 至少重跑第 10 节列出的全部精确 `.test` 输入并核对对应 `.result-pq`；并为 thread-init
  失败、pre-READY KILL 和重复 End 增加有总超时保护的故障验证。
