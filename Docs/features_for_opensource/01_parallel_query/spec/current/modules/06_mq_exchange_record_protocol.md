# 06 MQ、Exchange 与 Record Protocol 契约

- 模块 ID：`PQ-MOD-006`
- 文档状态：`current-contract`，仅代表当前稳定基线静态实现
- 代码基线：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9 (`stable_branch`)`
- 基线范围：manifest 中的 commit、`stable_branch` 已提交基线、`source_tree_sha256` 与 `test_tree_sha256` 仅覆盖其定义的 PQ source/test file set，不声明覆盖整个 workspace。
- 静态复核日期：2026-07-20
- 运行时验证：见 `current/quality/verification_evidence.md`；不把 suite 成功解释为模块级全覆盖
- 上游契约：`../00_architecture_overview.md`、`../01_current_support_matrix.md`

## 1. 职责与非职责

### 1.1 职责

本模块定义 worker 到 leader 的进程内消息交换契约：

- 每个 worker 一个 SPSC ring queue，所有 worker 共享一个 leader receiver event；
- 用单调读写字节计数和 power-of-two ring 处理回绕；
- 用 4 字节 payload length framing 组装一条完整消息；
- 把 worker 临时表字段、NULL/常量状态、可选 rowid 和变长字段编码为 record payload；
- 把 payload 解码回 leader 临时表 `record[0]`；
- 在无序路径进行轮询汇总，在有序路径对各 worker 的局部有序流做 k-way merge；
- 用 detach 表示 producer 完成或 leader 提前停止消费，并在 detach 后先排空已发布数据；
- 用长度为 1 的特殊消息通知 leader worker/发送路径错误；
- 为 `COUNT(DISTINCT)` 的进程内 batch buffer 指针传递提供专用编码和回收通道。

### 1.2 非职责

本模块不负责：

- 跨进程、跨主机或持久化协议兼容；协议包含原生指针，明确只适用于同一 mysqld 进程；
- 自描述 schema 协商；leader 和 worker 必须由同一克隆计划得到一致字段布局；
- 选择 ORDER BY、stable sort 或最终 LIMIT 的 SQL 语义；
- 为多 producer/多 consumer 提供通用无锁队列；每个 ring 的拓扑是一个 worker sender 和一个 leader receiver；
- 证明当前 `detached` 普通枚举字段在 C++ memory model 下不存在 data race；静态实现缺少显式原子/锁，列为质量风险；
- 把 `MQ_TMP_DETACHED` 作为已验证的运行状态；静态检索只看到判断，未找到当前路径的设置者。

## 2. 入口、输入与输出

### 2.1 入口

| 入口 | 调用方 | 作用 |
|---|---|---|
| `MQ_record_gather::mq_scan_init()` | leader `ParallelScanIterator` | 根据 Filesort/stable 需求创建 `Exchange_sort` 或 `Exchange_nosort` |
| `Exchange::init()` | leader | 为每个 worker 分配 ring、sender event、handle，并创建共享 receiver event |
| `Query_result_mq::send_result_set_metadata()` | worker | 创建 worker 临时表及字段编码工作区 |
| `Query_result_mq::send_data()` | worker | 编码一行并按段写入对应 MQ |
| `MQueue_handle::send()` | worker | 发送 framed payload 或字段 raw segment |
| `MQueue_handle::receive()` | leader | 非阻塞/阻塞拼装一条完整 framed payload |
| `Exchange_nosort::read_mq_record()` | leader | round-robin 取下一行并解码 |
| `Exchange_sort::read_mq_record()` | leader | 从各局部有序流取全局下一行并解码 |
| `ParallelScanIterator::pq_wait_workers_finished()` | leader | LIMIT/KILL/结束时先 detach 全部 queue，再等待 worker |

### 2.2 输入

- worker 输出字段列表与 leader 接收字段列表；
- worker 临时表 field 原始存储格式；
- 每个字段的 const/null 状态；
- 可选 DIV_TAB `handler::ref` rowid；
- Filesort order、group 方向、stable-output 标志；
- `pq_msg_queue_size`、`pq_msg_queue_spin_lock`；
- `parallel_batch_max_slot`、`parallel_batch_max_mem_size`；
- worker/leader KILL 和 `pq_error` 状态。

### 2.3 输出

- leader 临时表中的一条重建 record；
- 对 stable/order 路径，是全局比较意义下的下一条 record；
- `MQ_SUCCESS`、`MQ_WOULD_BLOCK` 或 `MQ_DETACHED`；
- 所有 producer 完成后的 EOF；
- 特殊 error payload 触发的 leader `pq_error`；
- `COUNT(DISTINCT)` leader merge 所消费并释放的 `Batch_buffer`。

## 3. 主流程

### 3.1 队列建立

1. `Exchange::init()` 读取 `pq_msg_queue_size`，通过 `lower_exponent()` 向下取不大于配置值的 2 的幂作为实际 ring size。
2. leader 在 `pq_mem_root` 上创建一个共享 `MQ_event m_receiver`。
3. 对每个 worker 创建一个 sender event、一段 ring buffer、一个 `MQueue` 和一个 `MQueue_handle`。
4. handle 初始化 1 KiB 接收缓存和一个 `Batch_buffer_manager`。接收缓存可按消息长度倍增。
5. `pq_init_record_gather()` 把第 `i` 个 handle 借给第 `i` 个 worker；ring 不在 worker THD 的 mem_root 上所有。

ring size 向下取整意味着配置值不是实际容量的精确值；容量相关诊断应报告实际 `m_ring_size`。

### 3.2 SPSC 发送、接收与回绕

发送方：

1. 原子读取 `m_bytes_read` 和 `m_bytes_written`，计算 `used = written - read` 与 available。
2. available 为 0 时唤醒 receiver；blocking 模式等待 sender event，nowait 模式返回 `MQ_WOULD_BLOCK`。
3. 有空间时按 ring 尾部长度拆分 `memcpy()`；数据拷贝完成后更新单调 write counter，再唤醒 receiver。
4. 检测 leader/PQ error 或 `MQ_HAVE_DETACHED` 时停止发送并返回 `MQ_DETACHED`。

接收方：

1. 从 `m_bytes_read + m_consume_pending` 计算逻辑读位置，并读取已发布 write counter。
2. 足够时一次读取目标字节；跨 ring 尾部时执行两段复制。
3. 不足但尾部有连续数据时先消费该段，调用者在下一次 receive 继续拼装。
4. read counter 采用 `m_consume_pending` 批量提交；超过 ring 四分之一或需要等待时更新共享读位置并唤醒 sender。
5. nowait 且暂时无数据返回 `MQ_WOULD_BLOCK`；blocking 路径等待共享 receiver event。

`MQueue_handle` 保存 length-word 是否完整、已收 payload 字节数和期望长度，因此一条消息可以跨 ring 边界，也可以跨多次 nowait receive 调用。

### 3.3 framing 与 detach

通用 `send(void *, len)` 的 wire framing 是：

```text
uint32 payload_length (native in-process representation)
byte[payload_length] payload
```

`receive()` 先完整拼出 4 字节长度，再确保本地 buffer 足够，最后拼出全部 payload 才返回 `MQ_SUCCESS`。

detach 有两类来源：

- worker 正常完成或错误退出后设置 `MQ_HAVE_DETACHED`；
- leader 已取得足够行（典型为 LIMIT）、KILL 或清理时，先对所有 handle 设置 `MQ_HAVE_DETACHED`，再等待/join workers，解除满 ring 上的 sender 阻塞。

接收端观察 detach 时只承诺排空已成功 framing 的完整消息：在确认 write counter 稳定且不存在可完成的完整 frame 后返回 `MQ_DETACHED`。若 detach 发生在一条消息只写完 length 或部分 payload 的窗口，handle 不会把该不完整尾帧作为成功 record 返回；其已消费字节是保留还是丢弃，依当前 receive/cleanup 路径处理，不构成成功消息或额外的 drain 承诺。该时序的资源与错误语义需要 fault injection 证明。

### 3.4 record payload 编码

`Query_result_mq::send_data()` 依次构造以下 segments；第一段是 framing length，自身不计入 payload：

```text
uint32 payload_length
payload :=
  [rowid bytes]                    // stable output 时存在
  uint16 null_header_length
  byte[null_header_length] null/const header
  byte[...] fields                // 仅 non-const 且 non-null 字段
```

字段 header 每个字段占 2 bit：

| bit pair | 含义 | 是否发送 field bytes |
|---|---|---|
| `00` | 非常量、非 NULL | 是 |
| `01` | 非常量、NULL | 否 |
| `10` | 常量、非 NULL | 否；leader 使用自身克隆 Item |
| `11` | 常量、NULL | 否；leader 更新 Item null 状态 |

编码细节：

- null header 第 0 字节保存协议自己的长度/布局信息，字段 bit 从后续位置读取；
- fixed-length field 发送 `Field::pack_length()` 字节；
- `VARCHAR/VAR_STRING` 先额外发送 1 字节 `length_bytes`，再发送 field 原始 `[length prefix + value]`；
- stable output 在 payload 最前发送 DIV_TAB `handler::ref`，长度为 `ref_length`；
- `AVG` 等附加状态通过 Item/Field 的额外长度接口并入字段原始表示；
- `COUNT(DISTINCT)` 使用 `PTR_SIGN`，发送的是 `Batch_buffer *` 指针值本身，长度为本机指针宽度，不复制整棵 distinct tree 到 ring；
- payload length 是 rowid、2 字节 header 长度、header 和实际字段 segments 的总和，不包含最前面的 4 字节 length word。

该协议没有版本号、字节序转换或 schema tag。leader 与 worker 对字段顺序、类型、pack length、rowid 长度和指针宽度必须完全一致。

### 3.5 解码与错误消息

1. `Exchange::convert_mq_data_to_record()` 先检查 KILL、leader `pq_error` 和 `msg_len == 1`。
2. 当前协议把任何长度为 1 的 payload 视为错误；worker 通过 `send_exception_msg(ERROR_MSG)` 发送该 sentinel。接收端没有再检查 payload byte 的枚举值。
3. stable 输出先复制 rowid；随后读取 `uint16 null_len` 和 bit header。
4. 对 `00` 字段按 field 类型复制 raw bytes，对 `01` 设置 field NULL，对常量状态更新 leader Item 的 `null_value`。
5. 变长字段先恢复 `length_bytes`，再据其读取 1/2 字节长度前缀和 value。
6. 解码执行近似消息长度上界检查；错误令 leader `pq_error=true`，本次 record 不输出。

因为 `msg_len == 1` 被保留给错误，正常 record payload **必须**不可能等于 1。当前 record 至少包含 2 字节 null-header length 和 header，满足该约束；未来压缩/协议重构必须重新审查。

### 3.6 无序 gather

`Exchange_nosort` 以 round-robin 访问尚未结束的 handles：

- 某 queue 返回 `MQ_WOULD_BLOCK` 时切换到下一 queue；
- 所有 active queues 本轮都无数据时，在共享 receiver event 上等待；
- queue detach 且数据已排空后，将其标记 `read_done` 并把 active reader 数减一；
- active reader 数为 0 时返回 EOF；
- 输出顺序只受到达和轮询影响，不承诺串行物理扫描顺序。

### 3.7 有序 gather

`Exchange_sort` 的正确性前提是每个 worker 自己的输出流已按同一比较规则单调有序：worker filesort、ordered index scan 或为 rebuilt GROUP 添加的局部排序承担该前提。

1. 初始化时为每个 worker 分配一个当前最小 record 和最多 100 条的 batch cache。
2. 首次建 heap 前，以 blocking 模式确保每个未完成 worker 至少贡献一条 head record，或确认该流已完成。
3. 以 binary heap 选择各 worker head 中的最小值；消费后只从同一 worker 补充并调整 heap。
4. 可 nonblocking 预取同一 worker 最多 100 条，并在 index-sort 场景依据 batch 最大值和其他 heap 节点决定能否连续输出。
5. 有 Filesort order 时由 `Sort_param::make_sortkey()` 生成比较 key；stable output 在 key 相等时以 rowid `handler::cmp_ref()` 作为 tie breaker。
6. 只有 stable 而无显式 sort order 时，直接按 rowid 比较。
7. GROUP 使用倒序索引时，`m_desc_groups` 修正 leader merge 的方向。

有序 merge 不能修复单个 worker 内部的乱序；任何 worker-side sort 消除、reverse 标志或 group 方向变更都必须同步审查 Exchange_sort。

## 4. 所有权与生命周期

| 对象/资源 | 创建者 | 所有者 | 借用者 | 清理 |
|---|---|---|---|---|
| `MQ_record_gather` | `ParallelScanIterator` | leader `pq_mem_root`/iterator | leader scan | `mq_scan_end()` 调 Exchange cleanup |
| `Exchange_*` | `MQ_record_gather::mq_scan_init()` | leader `pq_mem_root` | leader gather/read | `Exchange::cleanup()`；mem_root 最终回收存储 |
| shared receiver event | `Exchange::init()` | Exchange/leader | 所有 sender 与 leader | `Exchange::cleanup()` destroy |
| per-worker sender event | `Exchange::init()` | 对应 handle/Exchange | 一个 worker、leader receiver | handle cleanup destroy |
| ring buffer | `Exchange::init()` | leader `pq_mem_root`，handle cleanup 负责析构路径 | 对应 SPSC sender/receiver | worker 全停后 cleanup |
| `MQueue` | `Exchange::init()` | leader `pq_mem_root` | handle | mem_root 生命周期；其内部资源由 handle cleanup 处理 |
| `MQueue_handle` | `Exchange::init()` | leader `pq_mem_root`/Exchange | 对应 worker 和 leader | handle cleanup + mem_root |
| receive local buffer | handle init/扩容 | handle，分配在 leader `pq_mem_root` | leader receive | handle cleanup/mem_root |
| `Batch_buffer_manager` | handle init | handle；对象在 mem_root，内部 slots/buffers 为普通 heap | worker distinct 发送、leader merge | manager destructor 删除 slots/buffers |
| `Batch_buffer_slot` | manager | manager `m_all_slot` | 当前 distinct row | 所有 buffer release 后回 free stack |
| `Batch_buffer` | slot | slot | worker 写、leader 经指针读 | leader `release()`；最终 slot destructor 关闭 cached file 并 delete buffer |
| sort batch/heap/key | `Exchange_sort::alloc/init` | leader `pq_mem_root`/Exchange_sort | leader merge | `Exchange_sort::cleanup()` + mem_root |

`COUNT(DISTINCT)` 的指针消息建立了特殊跨线程借用：worker 创建的 batch buffer 实际由 leader-owned handle manager 管理，worker 发送指针后不得自行释放；leader merge 完整读完后必须 `release()`。错误、detach 或 leader 早停若跳过 release，会耗尽 slot 或遗留临时文件，属于重点审计路径。

## 5. 并发、锁与状态机

### 5.1 ring 并发模型

```text
worker-i (only producer) -> ring-i -> leader (only consumer)
                                      ^
worker-0..N sender event ---- shared receiver event
```

- byte counters 通过 CAS helper 原子读写，并辅以 acquire/release/full barrier；
- ring data 先复制，再发布 write counter；receiver 读取 published counter 后复制数据；
- 每个 sender event 只服务一个 queue；receiver event 由所有 workers 共享；
- `MQ_event::latch` 和 `wait_lock` 是原子，mutex/condition variable 只保护阻塞等待与唤醒；
- `detached` 是普通 enum，当前静态实现未显示与所有读写配对的原子或 mutex；不能仅凭 compiler barrier 宣称满足 C++ happens-before。

### 5.2 handle 接收状态

```text
NEED_LENGTH
  -> PARTIAL_LENGTH
  -> NEED_PAYLOAD(expected_bytes)
  -> PARTIAL_PAYLOAD
  -> COMPLETE
  -> NEED_LENGTH

任意阶段 -> DETACHED（仅完整消息返回前不得伪造成功）
```

`m_length_word_complete`、`m_partial_bytes`、`m_expected_bytes` 和 `m_consume_pending` 都是 leader 单线程私有状态，不允许多个 receiver 同时调用同一 handle。

### 5.3 batch manager 并发

slot free stack 与 slot 总表由 MySQL mutex/condition variable 保护。slot 达上限后 `get_next_slot()` 以 5 秒 timed wait 循环等待；当前循环没有显式检查 KILL、`pq_error` 或永久无 release 的终止条件，属于潜在挂起风险。

## 6. 规范性不变量

- `PQ-MQ-INV-001`：每个 `MQueue` **必须**保持一个 producer、一个 consumer；不得让两个 worker 写同一 ring 或两个 leader reader 并发读同一 handle。
- `PQ-MQ-INV-002`：sender **必须**先完成 ring data copy，再发布 write counter；receiver **必须**只读取已发布范围。
- `PQ-MQ-INV-003`：`written - read` **必须**始终位于 `[0, ring_size]`，单调计数不得回退。
- `PQ-MQ-INV-004`：实际 ring size **必须**为非零 2 的幂；MOD 回绕依赖该条件。
- `PQ-MQ-INV-005`：一条正常消息**必须**包含完整 4 字节 length 和完整 payload 后才返回 `MQ_SUCCESS`。
- `PQ-MQ-INV-006`：normal record payload 长度**不得**为 1；当前接收协议把所有 `msg_len == 1` 解释为 worker error。
- `PQ-MQ-INV-007`：leader 和 worker **必须**使用相同字段顺序、field pack layout、const/null bit 数和 rowid 长度；协议不提供运行时 schema 协商。
- `PQ-MQ-INV-008`：stable payload 中的 rowid **必须**来自 DIV_TAB 当前 record 的 `handler::position()` 结果，并使用同一 handler 的 `cmp_ref()` 比较。
- `PQ-MQ-INV-009`：`Exchange_sort` 使用的每个 worker 输入流**必须**局部单调有序；全局 heap merge 不得接收任意乱序流。
- `PQ-MQ-INV-010`：排序 key 相等且要求 stable output 时，**必须**使用 rowid tie breaker；不得退回 worker 到达顺序。
- `PQ-MQ-INV-011`：detach 后 receiver **必须**先排空已发布的完整数据，再把该 queue 标记 read done；leader 早停则必须先 detach 再等待 worker。
- `PQ-MQ-INV-012`：worker 错误时应先尽力发送 error sentinel，再 detach；若 sentinel 无法发送，`pq_error`/diagnostics **必须**提供另一条可观察错误通道。
- `PQ-MQ-INV-013`：`PTR_SIGN` 只可用于同进程、同生命周期域中的 `Batch_buffer *`；不得序列化到网络、磁盘结果或延迟到 handle manager 销毁后读取。
- `PQ-MQ-INV-014`：每个成功发送给 leader 的 distinct batch **必须**恰好 release 一次；不得提前释放或永久占用 slot。
- `PQ-MQ-INV-015`：所有等待 sender/receiver 的路径在 KILL、PQ error 或 detach 时**必须**最终可退出，避免 join worker 永久阻塞。

## 7. 错误、回退、重试与清理

| 场景 | 当前行为 | 回退/重试 | 清理要求 |
|---|---|---|---|
| Exchange/ring/handle 分配失败 | `Exchange::init()` 返回错误，PQ 初始化失败 | 可由上层 graceful fallback/整句 retry | 清理已分配 event、rings、handles、manager |
| sender 满 ring | blocking 等待；nowait 返回 `MQ_WOULD_BLOCK` | 同一消息状态继续 | detach/KILL 必须唤醒 sender |
| receiver 暂无数据 | nosort 切换 queue；全部空时等 receiver event | 继续轮询 | KILL/PQ error 退出 |
| receive buffer 扩容失败 | 报 `ER_STD_BAD_ALLOC_ERROR` 并返回 detached 类结果 | 当前 PQ 终止 | leader detach queues、join workers |
| worker row 编码/发送失败 | 尝试 `send_exception_msg(ERROR_MSG)`；worker 进入错误清理 | 当前 PQ 终止；传播错误 | worker temp table/arrays、MQ、batch pointer；leader detach、join workers |
| leader 收到长度 1 | `convert_mq_data_to_record()` 置 `pq_error`，不输出该 record | 当前 PQ 终止 | detach 其余 queues 并 join workers |
| 解码长度/字段错误 | leader `pq_error` | 当前 PQ 终止 | 不得继续把损坏 buffer 当 record |
| leader LIMIT 提前满足 | 所有 queues 设置 HAVE_DETACHED，然后等 worker | 正常终止，不是错误重试 | sender 必须从满 ring 等待中退出 |
| worker 正常 EOF | worker detach；leader 排空后 reader done | 继续其他 queues | active reader 只减一次 |
| distinct batch spill IO 错误 | worker/leader aggregate 报 PQ error | 当前 PQ 终止 | release slot、关闭 cached file |
| distinct slot 耗尽 | timed wait 循环 | 当前实现无本地取消分支 | 必须依赖 release；这是挂起审计重点 |
| KILL | event wait 检查 THD kill，leader/worker 设置错误/detach | 不在原 PQ 恢复 | detach、唤醒、join、Exchange cleanup |

error sentinel 与普通 rows 在同一 queue 中有序：leader可能先解码 sentinel 前已经发布的 rows，但一旦发现 sentinel，整次并行执行必须按错误处理，不能把此前部分 rows 作为成功结果提交给客户端。

worker 已启动后的编码、发送或执行错误属于运行期错误，**不得**把当前语句切换为串行 retry；
必须终止 PQ、传播诊断，并完成 detach、join 与资源清理。只有 worker 启动前、尚未输出结果、
且精确命中初始化失败契约的 `ER_PARALLEL_FAIL_INIT` 才可能触发整句串行 retry；完整边界见
`modules/03_worker_lifecycle_error_retry.md`。

## 8. 支持范围、限制与开关

| 能力 | 行为状态 | 证据 | 限制/开关 |
|---|---|---|---|
| 固定/变长字段 record exchange | implemented | static-evidence | 依赖相同 cloned schema；复杂不支持类型在优化器前置拒绝 |
| NULL/const 字段省略 | implemented | static-evidence | 2 bit/field；leader Item 状态必须一致 |
| BLOB/字符集 | partial | static-evidence | 仍依赖 field raw layout 与 clone 正确性 |
| 无序 round-robin gather | implemented | static-evidence | 不承诺串行物理顺序 |
| ORDER/GROUP k-way merge | implemented | static-evidence | 每 worker 局部有序是硬前提 |
| stable rowid tie-break | implemented | static-evidence | 只适用于可提供稳定 `handler::ref` 的 divided table |
| worker error sentinel | implemented | static-evidence | 以 payload length 1 识别，协议可扩展性弱 |
| detach drain | partial | static-evidence | 仅排空完整 framed messages；不完整尾帧不能成功，按当前 receive/cleanup 路径消费或丢弃 |
| COUNT(DISTINCT) pointer/batch | partial | static-evidence | feature 默认关闭；同进程原生指针；BLOB distinct 被前置拒绝 |
| MQ 跨进程/异构 ABI | rejected | static-evidence | 无 endian/schema/version/pointer portability |

关键变量：

- `pq_msg_queue_size`：每 worker 配置容量，实际向下取 2 的幂；
- `pq_msg_queue_spin_lock`：event 阻塞前 spin 次数；
- `parallel_batch_max_slot`：每 handle distinct batch slot 上限，默认 8；
- `parallel_batch_max_mem_size`：每 batch 内存阈值，默认 1 MiB，超出可转 cached file。

## 9. 可观测性

- worker/leader THD 的 `pq_error`、KILL、diagnostics 和 error log 是主要错误信号。
- `Query_result_mq` 维护 worker sent-row 等统计；Exchange active readers 和 queue read-done 可用于调试 EOF。
- EXPLAIN 可显示 merge sort/stable 选择，但不暴露 ring occupancy、partial framing 或 detach 原因。
- DBUG 点覆盖 MQ init、receive 扩容、field decode、send、merge sort allocation/store、异常消息失败等路径。
- 挂起现场应记录每 queue 的 `m_bytes_written`、`m_bytes_read`、`m_consume_pending`、framing state、detach、sender/receiver latch、worker 状态和 outstanding batch slots。
- 当前 event 帮助注释以 10ms 描述默认等待，但代码将按毫秒计算的差值作为 `microseconds` 传给 `wait_for()`；实际等待单位需要运行测量，不能只信注释。

## 10. 源码与测试映射

### 10.1 源码锚点

| 契约 | 源码锚点 |
|---|---|
| ring、event、framing、detach、batch 类型 | `sql/parallel_query/msg_queue.h`：`MQ_event`、`MQueue`、`MQueue_handle`、`Batch_buffer*` |
| send/receive 与回绕 | `sql/parallel_query/msg_queue.cc`：`send_bytes()`、`send()`、`receive_bytes()`、`receive()` |
| batch 内存/临时文件/slot | `sql/parallel_query/msg_queue.cc`：`Batch_buffer::write()`、`Batch_buffer::read()`、`Batch_buffer::release()`、`Batch_buffer_manager::get_next_slot()`、`Batch_buffer_manager::release_slot()` |
| worker record 编码 | `sql/parallel_query/query_result_mq.cc`：`pq_build_mq_fields()`、`pq_build_mq_count_distinct_item()`、`Query_result_mq::send_data()` |
| Exchange 建立/解码/nosort | `sql/parallel_query/exchange.cc`：`Exchange::init()`、`Exchange::convert_mq_data_to_record()`、`Exchange_nosort::get_next()`、`Exchange_nosort::read_next()`、`Exchange_nosort::read_mq_record()` |
| k-way merge/stable rowid | `sql/parallel_query/exchange_sort.cc`、`sql/parallel_query/exchange_sort.h`：`Exchange_sort::init()`、`Exchange_sort::build_heap()`、`Exchange_sort::read_mq_record()`、`Exchange_sort::cleanup()`、`heap_compare_records()` |
| sort/nosort 选择 | `sql/parallel_query/sql_parallel.cc`：`MQ_record_gather::mq_scan_init()` |
| handle 分配、early detach、join | `sql/parallel_query/pq_iterators.cc`：`pq_init_record_gather()`、`pq_wait_workers_finished()` |
| worker error/normal detach | `sql/parallel_query/sql_parallel.cc`：`pq_worker_exec()` |
| distinct tree merge | `sql/item_sum.cc`：`Aggregator_distinct::save_distinct_to_mq()`、`merge_count_distinct_tree()` |

### 10.2 现有测试资产

| 主题 | MTR 资产 | 证据 |
|---|---|---|
| MQ 初始化/发送/接收错误 | `pq_mq_error` | test-present |
| worker error/fallback | `pq_worker_error`、`pq_abort`、`pq_auto_retry_failed_parallel_query` | test-present |
| KILL/断开/early detach | `pq_kill`、`pq_kill_query`、`pq_limit_no_order_by` | test-present |
| order/stable/常量 | `pq_order_by`、`pq_order_const`、`pq_hash_join` 中 ISSUE 627 场景 | test-present |
| distinct batch | `pq_agg_distinct`、`pq_support_features_switch` | test-present |
| 字段编码 | `pq_blob`、`pq_charset`、`pq_clone_item` | test-present |
| 内存/临时资源 | `pq_memory_limit`、`pq_tempory_table_release` | test-present |
| hash join 错误经 MQ | `pq_hash_join_error` | test-present |

## 11. 已知缺口、变更影响与质量风险

### 11.1 当前质量风险

| ID | 风险 | 静态证据/影响 | 建议验证 |
|---|---|---|---|
| `PQ-MQ-RISK-001` | `detached` 跨线程读写缺少显式原子/锁 | 字段是普通 enum；compiler/full barrier 不能自动构成标准 C++ 同步 | TSAN 定向 send/receive/early LIMIT/KILL；必要时建立明确 atomic 契约 |
| `PQ-MQ-RISK-002` | event 等待时间单位不一致 | elapsed 按毫秒计算，`wait_for` 参数却是微秒 | 运行测量空队列 CPU、唤醒延迟和超时频率 |
| `PQ-MQ-RISK-003` | `MQ_TMP_DETACHED` 语义漂移 | 当前代码有判断和“以后恢复”注释，但静态调用图未见设置者 | 删除/启用前必须先定义状态转换和 framing 语义 |
| `PQ-MQ-RISK-004` | partial length/payload 与 detach 交叉 | receiver 保存部分状态，detach 后不完整消息不会完成 | fault injection 覆盖 0..4 字节 prefix、payload 各偏移、ring wrap |
| `PQ-MQ-RISK-005` | error 仅以 `msg_len==1` 识别 | 无 type/version 字段；未来短 record 或控制消息可能冲突 | 协议变更时引入显式 type 并做兼容审查 |
| `PQ-MQ-RISK-006` | distinct slot 永久等待 | slot 耗尽循环只有 5 秒 timed wait，没有显式 KILL/pq_error break | KILL、leader early exit、batch IO error 下验证 slot 全 release |
| `PQ-MQ-RISK-007` | native pointer 误越生命周期 | `PTR_SIGN` 直接发送 `Batch_buffer *` | ASAN + distinct error/detach；禁止协议跨进程复用 |
| `PQ-MQ-RISK-008` | ordered merge 隐含局部有序前提被破坏 | heap 只维护每流 head，无法检测中途降序 | 每 worker 输出单调性断言/调试计数，覆盖 reverse/group/index sort |
| `PQ-MQ-RISK-009` | nosort active-reader 索引维护复杂 | read_done 后 active 数和 `m_next_queue` 同时变化 | 部分 worker 未启动/先结束/错误组合测试 |
| `PQ-MQ-RISK-010` | 解码长度校验为上界估算而非完整逐段 bounds check | 字段 loop 依赖 cloned schema，损坏长度可能扩大影响 | fuzz record header、varlen length、rowid 与字段数 |

### 11.2 变更影响清单

修改以下任一内容时必须重新审查本模块：

- ring counter、barrier、event 或 detach 表示；
- `Field_raw_data`、null/const header、varstring encoding、AVG extra bytes；
- leader/worker temp-table schema clone；
- error/control message；
- stable rowid 生成或 `handler::ref_length/cmp_ref()`；
- worker-side filesort、ordered index、reverse/group direction；
- distinct batch owner、slot manager 或 cached-file 生命周期；
- leader LIMIT/KILL/worker join 清理顺序。

此类变更应同步复核 `PQ-MQ-INV-*`、聚合/排序模块、架构级 worker 清理不变量和相应 MTR 资产。
