# Parallel Query 资源准入与 Accounting 当前契约

> 模块 ID：`PQ-MOD-011`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 最后运行验证：见 `current/quality/verification_evidence.md`

> Snapshot authority：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`。该 manifest 绑定
> implementation commit `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9`、`stable_branch` 已提交基线、
> `source_tree_sha256=a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> 和 `test_tree_sha256=066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`。
> 两个 hash 只覆盖 manifest 定义的 PQ source/test file set，不覆盖或复现整个 bound PQ source/test tree。

## 1. 职责与非目标

本模块定义 PQ 对线程、PQ MEM_ROOT、MQ ring、临时表、batch spill、hash spill 和 VFD 的
当前资源契约，区分：

- 采用 PQ 计划前的准入；
- 计划/执行期间的实际资源增长；
- 正常、fallback、KILL、partial launch 和 error 路径的归还；
- 当前已有 accounting 与尚未被硬上限覆盖的资源。

本模块不把 `parallel_memory_limit` 描述为进程级或 statement 级完整硬限制。当前静态实现
只对 `key_memory_pq_mem_root` 的部分分配进行 bucket accounting，并在采用计划前检查全局
总量；worker temp、普通 iterator、batch cached file、hash spill 和其他 allocator 不能由该
计数自动推断。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 线程准入 | `sql/parallel_query/sql_parallel.cc::check_pq_running_threads` | 原子化检查全局上限，成功时同时增加全局和 leader THD reservation |
| 线程归还 | `sql/parallel_query/pq_resource_stat.cc::release_pq_running_threads` | 减少全局 reservation 并 broadcast waiter |
| Statement cleanup | `sql/sql_class.cc::THD::cleanup_after_query` | 归还 THD reservation、清空 `pq_mem_root`、复位 PQ/VFD 状态 |
| Memory accounting | `sql/parallel_query/pq_resource_stat.cc::add_pq_memory/sub_pq_memory/get_pq_memory_total` | 只统计 `key_memory_pq_mem_root` bucket |
| Memory admission | `sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | 当前全局已统计量达到 `parallel_memory_limit` 时拒绝 PQ |
| MQ allocation | `sql/parallel_query/exchange.cc::Exchange::init/cleanup` | leader `pq_mem_root` 分配每 worker ring、event、queue 和 handle |
| Batch spill | `sql/parallel_query/msg_queue.cc::Batch_buffer::write` | 超过 batch memory buffer 后使用 cached temporary file |
| Hash spill/VFD | `sql/iterators/hash_join_iterator.cc::GetVfdManager`、`InitializeChunkFiles` | PQ worker 借用 leader THD 的 VFD manager 和共享 chunk files |
| VFD concurrency | `sql/sql_executor.cc` 的 PQ hash-join `SetNeedsMutex(true)` 路径 | worker 共享 VFD manager 时开启互斥保护 |
| Worker creation | `sql/parallel_query/pq_iterators.cc::pq_launch_worker` | 当前使用 `mysql_thread_create()`，不是 thread-pool task |

## 3. 主流程

```text
JOIN::suite_for_parallel_query()
  -> get_pq_memory_total() >= parallel_memory_limit ? reject
  -> choose DIV/CUT and validate Items
  -> check_pq_running_threads(requested DOP, queue timeout)
       -> no capacity and timeout=0: reject
       -> timeout>0: wait on COND_pq_threads_running
       -> success: global += DOP; leader THD reservation += DOP
  -> feature-switch check
  -> CBO check
       -> reject: reservation remains until statement cleanup
       -> accept: build PQ plan
  -> allocate pq_mem_root plan/MQ objects
  -> launch zero to DOP actual worker threads
  -> runtime temp/batch/hash/VFD allocations
  -> End/join/handler cleanup
  -> THD::cleanup_after_query()
       -> release reserved DOP
       -> pq_mem_root->Clear()
       -> reset VFD manager mutex mode
```

当前 reservation 是请求 DOP，不是实际成功创建 worker 数，也不是有效取得 scan context 的
worker 数。三者必须分别理解：

```text
requested/reserved DOP >= created workers >= workers that consumed a context
```

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | 分类 | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| `parallel_threads_running` | server global | global PQ admission | shared under mutex | decrement by statement cleanup | 不得 underflow/overflow；等待者必须在归还后被唤醒 |
| `THD::pq_threads_running` | admission | leader THD | statement reservation | `THD::cleanup_after_query` | 记录预留量，不是 live thread 精确计数 |
| `pq_memory_used[16]` | PSI callbacks | server global | lock-free buckets | matching free callbacks | add/sub 必须成对且整数宽度足够 |
| `THD::pq_mem_root` | leader THD | leader | statement arena | `Clear()`/THD release | 所有从该 arena 借用的对象完成后才能清空 |
| MQ ring/events/handles | `Exchange::init` | Gather/leader MEM_ROOT | per-worker + shared receiver | `Exchange::cleanup` + MEM_ROOT clear | detach、event cleanup 先于 arena clear |
| Receive/materialized temp table | leader plan/iterator | leader/Gather | leader/shared | iterator/query cleanup | 最后一个 worker 使用结束后释放 |
| `Batch_buffer` heap buffer | batch manager | slot/buffer object | shared by protocol owner | destructor | `delete[]` 与 cached file close 必须都执行 |
| Batch cached file | `Batch_buffer::write` | `Batch_buffer` | temporary file | `Batch_buffer` destructor | error/KILL 也必须 close/remove |
| Hash `ChunkFilesWrapper` | hash iterator/shared context | leader/shared hash context | shared | shared-context/iterator destruction | barrier、mutex 和文件生命周期必须覆盖所有 workers |
| `VfdManager` | THD constructor | leader THD | shared by PQ hash workers | leader THD | worker 不得 delete；statement cleanup 恢复 mutex mode |
| Worker OS thread | `pq_launch_worker` | worker manager + leader join responsibility | per-worker | join/OS | 只对创建成功的 thread join，reservation 按 statement 统一归还 |

## 5. 并发、锁和公平性

### 5.1 线程准入

- `LOCK_pq_threads_running` 保护 check、increment 和 decrement；归还后 broadcast
  `COND_pq_threads_running`。
- `parallel_queue_timeout` 实现以 `timeout_ms` 命名，并用 `/1000` 秒及余数乘
  `1,000,000` 纳秒换算，即按毫秒解释；sysvar help 当前写 microseconds。
- waiter 被唤醒后按剩余毫秒重新等待，没有 FIFO、会话权重、普通 OLTP 保留配额或 DOP
  自动降级契约。
- reservation 发生在 feature-switch 和 CBO 之前；后续串行回退依赖 statement cleanup
  才归还，因此长时间串行执行可能占用 PQ quota。这是静态确认的时序，运行影响仍需测量。

### 5.2 Memory 和文件

- `pq_memory_used` 和总和使用 `uint`，allocator callback 的长度是 `size_t`；大分配或长期
  并发累计存在截断/回绕审计风险。
- admission 使用“当前全局总量是否达到 limit”，没有按候选 DOP/预计新增量预留 budget；
  多个 statement 可能在检查后并发增长。
- MQ ring 来自 `pq_mem_root`，但 Batch_buffer 使用普通 heap 并可能 spill cached file；hash
  join 还使用 iterator memory、shared chunk files 和 leader VFD manager。
- `parallel_memory_limit` 因而是部分全局准入信号，不是所有运行期对象的强制上限。

### 5.3 锁顺序

Parallel-aware hash join 的已记录锁顺序是：

```text
PQHashJoinSharedContext::HashTableMutex
  -> ChunkFilesWrapper global/per-chunk mutex
  -> Barrier mutex
  -> VfdManager LRU mutex
```

新增 spill/resource 代码 MUST 保持该顺序，且不得在等待 worker barrier 时持有会阻止其他
worker 清理的资源锁。

## 6. 规范性不变量与 Requirement

### `PQ-RES-REQ-001` / `PQ-RES-INV-001`：Reservation 成对

每次成功的线程 reservation MUST 在 statement 的所有正常、拒绝、fallback、retry、KILL
和 error 路径恰好归还一次。全局计数 MUST NOT underflow，等待者 MUST 最终被通知。

### `PQ-RES-REQ-002` / `PQ-RES-INV-002`：Live、Created 与 Reserved 不混淆

可观测性和准入逻辑 MUST 区分 requested/reserved DOP、成功创建 worker 数和有效扫描
worker 数；不得把 `PQ_threads_running` 解释为 OS live thread 的精确数量。

### `PQ-RES-REQ-003` / `PQ-RES-INV-003`：Memory accounting 不回绕

所有被声明计入 PQ memory limit 的分配 MUST 使用足够宽的计数并成对 add/sub；任何整数
回绕、bucket 越界或遗漏 MUST NOT 使超限 statement 被当作低用量。

### `PQ-RES-REQ-004` / `PQ-RES-INV-004`：MQ 资源按 Worker 完整清理

每个创建的 queue、ring、sender event 和 handle MUST 在 normal EOF、LIMIT、KILL、partial
launch 和 error 中 detach/cleanup；shared receiver MUST 晚于所有 sender 停止使用。

### `PQ-RES-REQ-005` / `PQ-RES-INV-005`：Temp/Spill/VFD 无泄漏

Receive/materialized temp table、Batch cached file、hash chunk file 和 VFD handle MUST 在
statement cleanup 后恢复基线；cleanup MUST 可处理未完全初始化的对象。

### `PQ-RES-REQ-006` / `PQ-RES-INV-006`：拒绝不长期占用资源

Eligibility、feature-switch、CBO 或计划失败后的串行执行 MUST NOT 长期占用不再需要的 PQ
配额。当前实现依赖 statement cleanup 的时序属于 `PQ-GAP-RES-002`。

### `PQ-RES-REQ-007` / `PQ-RES-INV-007`：公平等待有界

配置非零 timeout 时，等待 MUST 使用单一、明确的时间单位且总等待有界；KILL 和连接关闭
MUST 能终止等待。公平性策略必须显式定义，不能依赖 condvar 调度偶然性。

### `PQ-RES-REQ-008` / `PQ-RES-INV-008`：Thread-pool 声明符合实现

当前 worker 由 `mysql_thread_create()` 创建。任何文档或观测 MUST NOT 声称 worker 经
thread pool 调度；若未来集成，必须重新定义 admission、ownership、join 和 PFS 契约。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | fallback/retry | 必须清理的对象 |
|---|---|---|---|---|
| Memory admission reject | optimizer | 串行执行 + reason/status | serial-fallback | 不得新增 memory reservation |
| Thread capacity reject/timeout | admission | 串行执行 + reason/status | serial-fallback | waiter state，无 THD reservation |
| Feature/CBO 在 reservation 后拒绝 | optimizer | 串行执行 | serial-fallback | 当前由 statement cleanup 归还 reservation |
| MQ allocation/handle init 失败 | leader/Gather | init error | 是否 full retry 由 lifecycle 错误码决定 | 已创建 queue/event/handle、pq_mem_root |
| Worker 部分创建 | leader worker manager | error 或降低实际 worker 的当前路径 | 禁止假定可安全继续；按状态机处理 | 已创建 threads join、未创建 barrier participant 调整 |
| Batch cached file I/O 失败 | worker/leader aggregate path | statement error | runtime 禁止 retry | heap buffer、cached file、MQ、workers |
| Hash chunk/VFD I/O 失败 | hash iterator | statement error | runtime 禁止 retry | chunk files、shared hash context、VFD handles、barriers |
| KILL/LIMIT/disconnect | leader/workers | cancel/early EOF | 禁止隐藏 retry | MQ detach、threads join、temp/spill、reservation/memory |

## 8. 支持、限制和 feature switch

| 资源能力 | 当前状态 | 当前契约 |
|---|---|---|
| 全局线程 reservation 上限 | `implemented` | 默认 64；按 requested DOP 整体准入 |
| Capacity wait | `partial` | condvar + timeout；单位文档与实现冲突，公平性未定义 |
| DOP 自动降级 | `unknown` | 当前准入未显示从请求 DOP 自动降到可用 DOP |
| PQ MEM_ROOT accounting | `implemented` | PSI callback bucket 统计 |
| 全运行期 memory hard limit | `unknown` | 当前不可声明；多类分配不在该 accounting 中 |
| MQ ring accounting | `partial` | ring 位于 PQ MEM_ROOT；实际 size 取 `lower_exponent` |
| Batch memory/file | `partial` | 有 max memory/slot 和 cached-file spill，缺全局统一 accounting |
| Receive/materialized temp table | `partial` | owner path 存在，缺全退出路径基线断言 |
| Parallel hash spill/VFD | `partial` | 共享 leader VFD manager 与 chunk files；spill switch 默认 off |
| Worker thread-pool dispatch | `design-only` | 当前直接创建 OS threads |

## 9. 可观测性

当前全局 status：

- `PQ_threads_refused`；
- `PQ_memory_refused`；
- `PQ_threads_running`；
- `PQ_memory_used`；
- `PQ_stmt_executed`。

这些值存在以下解释限制：

- `PQ_threads_running` 更接近已预留 DOP，不是精确 live/active/effective worker；
- `PQ_memory_used` 只反映 PQ memory key 的部分分配；
- `PQ_stmt_executed` 在 worker launch 前按采用的 leader query-block plan 递增，UNION
  可多增；它不是逻辑 statement 或 actual worker-run 计数；
- refused 计数不带 query、digest、requested DOP、等待时间和拒绝阶段；
- 没有稳定的 MQ peak/wait、temp bytes、spill bytes/file count、VFD count 或 per-worker
  resource 指标。

发布级资源验证必须采集 statement 前基线、执行中峰值和 cleanup 后基线，单次
`SHOW STATUS` 截图不足以证明无泄漏。

## 10. 代码与测试映射

下列测试是静态 Requirement 映射；suite 运行范围见 `current/quality/verification_evidence.md`，不得据此推导组合级 `verified`。

| Requirement/Invariant | 源码 symbol | 正向测试 | 负向/故障测试 | 证据状态 |
|---|---|---|---|---|
| `PQ-RES-REQ-001` / `INV-001` | `check_pq_running_threads`、`release_pq_running_threads`、`cleanup_after_query` | `pq_variables` | `pq_check_first_rewritten_tab`、`pq_worker_error` | `static-partial` |
| `PQ-RES-REQ-002` / `INV-002` | `pq_launch_worker`、global status | `pq_variables` | partial-launch DBUG cases | `static-gap`; 无三类 DOP 指标 |
| `PQ-RES-REQ-003` / `INV-003` | `add_pq_memory/sub_pq_memory` | `pq_memory_limit` | memory refusal path | `static-gap`; 宽度/溢出未覆盖 |
| `PQ-RES-REQ-004` / `INV-004` | `Exchange::init/cleanup`、MQ detach | normal PQ suite | `pq_mq_error`、`pq_kill`、order/LIMIT cases | `static-partial` |
| `PQ-RES-REQ-005` / `INV-005` | Batch destructor、hash/VFD owner paths | `pq_tempory_table_release`、`pq_hash_join` | `pq_hash_join_error`、`pq_agg_distinct` low-memory case | `static-partial` |
| `PQ-RES-REQ-006` / `INV-006` | `JOIN::suite_for_parallel_query` admission order | 无定向用例 | feature/CBO rejection tests | `static-gap` |
| `PQ-RES-REQ-007` / `INV-007` | condvar timeout loop | `pq_variables` queue-timeout section | KILL during admission wait 缺失 | `static-partial` |
| `PQ-RES-REQ-008` / `INV-008` | `pq_launch_worker::mysql_thread_create` | none | none | `confirmed-static`; no thread-pool MTR |

## 11. 已知缺口和变更影响

- `PQ-GAP-RES-001`：`size_t` 分配长度累计到 `uint` bucket/total，存在截断和回绕风险；
  缺 limit−1/limit/limit+1、大分配和并发累计测试。
- `PQ-GAP-RES-002`：线程 reservation 早于 feature-switch/CBO，拒绝后直到 statement cleanup
  才归还；缺长串行 query 下的公平性验证。
- `PQ-GAP-RES-003`：`parallel_queue_timeout` help 写 microseconds，实现按 milliseconds；
  属于已确认接口漂移，需产品决策统一单位。
- `PQ-GAP-RES-004`：当前 memory limit 未覆盖 worker 普通 allocation、Batch heap/file、temp、
  hash 和 VFD 的完整运行期预算。
- `PQ-GAP-RES-005`：缺 requested/reserved/created/effective DOP、wait duration、MQ peak、temp
  bytes、spill/VFD 的 per-statement observability。
- `PQ-GAP-RES-006`：缺多 session PQ 与普通 OLTP 的公平性、饥饿和 p95/p99 release gate。
- `PQ-GAP-RES-007`：缺 cleanup re-entry、partial init、KILL during admission wait、file I/O error
  后精确资源基线断言。
- `PQ-GAP-RES-008`：历史 WL 声称 thread-pool dispatch，当前主路径直接创建 thread；当前
  contract 必须维持 `design-only`，直到代码和测试共同变化。

修改资源路径必须联动检查模块 03/06/08/10/12、全局 status、sysvar help、PFS memory/thread
instrumentation 和 fault-injection release gates。
