# 08 Join 执行契约

- 模块 ID：`PQ-MOD-008`
- 文档状态：`current-contract`，仅代表当前稳定基线静态实现
- 代码基线：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9 (`stable_branch`)`
- 基线范围：manifest 中的 commit、`stable_branch` 已提交基线、`source_tree_sha256` 与 `test_tree_sha256` 仅覆盖其定义的 PQ source/test file set，不声明覆盖整个 workspace。
- 静态复核日期：2026-07-20
- 运行时验证：见 `current/quality/verification_evidence.md`；不把 suite 成功解释为模块级全覆盖
- 上游契约：`../00_architecture_overview.md`、`../01_current_support_matrix.md`
- 相关模块：`05_innodb_range_cursor_mvcc.md`、`06_mq_exchange_record_protocol.md`、`07_aggregation_order_limit.md`

## 1. 职责与非职责

### 1.1 职责

本模块定义 PQ worker 内 Join 的当前执行契约：

- 在 access-path tree 中标记 hash join 的 build/probe 输入是否包含 DIV_TAB 或 CUT_TAB 的 partial results；
- 区分 parallel-oblivious hash join 与 build/probe 都是 partial input 的 parallel-aware hash join；
- 选择 CUT_TAB，并阻止 LIMIT、去重、weedout、非 inner join 保留侧和 rowid/spill 等不安全切分；
- 在 parallel-aware hash join 中管理 per-worker hash maps、共享查找视图、总内存预算、执行 barrier 和销毁 barrier；
- 在内存不足时执行 hash-table refill，或在 feature switch 允许时使用共享 chunk files spill；
- 保持 inner、semi、anti、left outer 的 match/unmatched、NULL key、extra condition 和 duplicate 语义；
- 约束普通 nested-loop/BKA join 中可被选为 DIV/CUT 的表；
- 在 worker 启动失败、提前 End、KILL 和 dependent-subquery 重入时维护 barrier participant；
- 由 leader 预物化可共享的 semijoin materialized table，再让 workers 只读复用。

### 1.2 非职责

本模块不负责：

- 选择整个语句是否进入 PQ、clone THD/JOIN 或实现 MQ framing；
- 跨 worker shuffle/repartition join key；当前 parallel-aware 模型是共享访问 per-worker maps/chunks，不是网络式 repartition exchange；
- 支持 FULL OUTER hash join；当前 iterator 对 `JoinType::FULL_OUTER` 只有不可达断言；
- 承诺任意 semi/anti/outer/BKA 计划都可并行；当前为条件性、部分支持；
- 证明多个 parallel-aware hash join nodes 共用一个 gather context 时相互隔离；当前 gather 只有一个 shared-context 指针，该形态列为质量风险；
- 证明 spill、barrier、KILL 的所有时序已动态通过；本任务只做静态审计。

## 2. 入口、输入与输出

### 2.1 入口

| 入口 | 作用 |
|---|---|
| `JOIN::choose_parallel_tables()` | 选择 DIV_TAB，并在 access path 建好后调用 `MarkCutTable()` |
| `MarkCutTable()` | 从 hash build subtree 选择安全 CUT_TAB |
| `pq_make_join_readinfo()` | 标记 DIV/CUT handler scan，并调用 `MarkPartialInputsForHashJoin()` |
| `MarkPartialInputsForHashJoin()` | 设置 build/probe partial flags；需要时创建 shared context |
| `CreateIteratorFromAccessPath()` | 根据 partial flags 把 shared context 传给 worker `HashJoinIterator` |
| `HashJoinIterator::Init()/BuildHashTable()/Read()` | 执行 build、barrier、probe、refill/spill 和 join-type 语义 |
| `HashJoinIterator::End()` / destructor | 退出 execution barrier，并保护跨 worker hash-row storage 的销毁 |
| `ParallelScanIterator::pq_launch_worker()` | reset barriers，并为未构造/未启动 worker drop participant |
| `ParallelScanIterator::para_exec_init()` | worker 启动前由 leader 初始化可共享 materialized tables |

### 2.2 输入

- original/worker access path tree、QEP_TAB 顺序和 table maps；
- DIV_TAB/CUT_TAB 标记、build/probe row estimates、DOP；
- equijoin keys、非等值 extra conditions、JoinType；
- `join_buffer_size`、spill permission、rowid storage requirement；
- `parallel_rows_threshold`、`pq_hash_join_max_hash_table_refills`；
- `pq_support_features_switch.hash_join_spill_to_disk`；
- worker THD KILL/error、leader-owned VfdManager 和 PQ mem root。

### 2.3 输出

- inner join 的全部匹配组合；
- semijoin 每个 probe row 至多一个匹配输出；
- antijoin 仅无匹配 probe rows；
- left outer join 的全部匹配或一条 build-side NULL-complemented row；
- hash join 的 EOF 或 worker error；
- EXPLAIN 可用的 partial build/probe 标志；
- spill chunks、probe-saving files、shared maps 在 statement 结束前完成清理。

## 3. 主流程

### 3.1 DIV/CUT 标记与安全边界

1. DIV_TAB 由 hint、`parallel_rows_threshold` 或最大估算表选择；候选必须支持 PQ scan。
2. `MarkCutTable()` 找到 probe subtree 含 DIV_TAB 的 hash join，并在其 build subtree 收集 TABLE/REF/INDEX/INDEX_RANGE scan 候选。
3. 以下 subtree 不允许包含 CUT_TAB：非 inner nested-loop/BKA/hash join 的 inner/build 保留语义侧；`LIMIT_OFFSET`；REMOVE_DUPLICATES；索引去重；duplicate-removal semijoin；WEEDOUT。
4. `allow_spill_to_disk && store_rowids` 的 hash join 不选择该 CUT 方案，避免 parallel-aware spill 破坏 rowid。
5. threshold 大于 0 时按 prefix rows 超过阈值一半选择首个安全候选；threshold 为 0 时按估算 records 从大到小选择。
6. `IsPartialResultsInputHashJoin()` 根据 DIV/CUT table map 标记 `partial_results_from_build_input` 和 `partial_results_from_probe_input`。

CUT 的意义是让原本需要每 worker 完整扫描的大 build input 也被分片。它只能在去重、LIMIT 和非 inner join unmatched 语义仍可由 hash join 全局阶段恢复时使用。

### 3.2 parallel-oblivious 与 parallel-aware

| 模式 | partial input | hash table | probe lookup | 典型约束 |
|---|---|---|---|---|
| parallel-oblivious | build/probe 至多一侧 partial | 每 worker 独立 map | 只查本 worker map | 另一侧必须提供语义所需完整数据，或每 worker 结果并集天然正确 |
| parallel-aware | build 和 probe 都 partial | 每 worker 写自己的 map，context 汇总 map 指针 | 每 worker probe 查所有 worker maps | 必须 build barrier 后才允许跨 map 读 |

`MarkPartialInputsForHashJoin()` 只有在某 hash node 两侧都 partial 时才在 gather 上创建 `PQHashJoinSharedContext`。worker iterator 构造时取得该 context；仅一侧 partial 时 `m_pq_hash_join == nullptr`，仍执行普通 HashJoinIterator，但每 worker 数据范围不同。

### 3.3 shared context 与 build/probe 阶段

1. shared context 以 DOP 初始化 execution/destruction barriers，以 `join_buffer_size` 初始化共享总内存预算，并创建 mutex-protected `ChunkFilesWrapper`。
2. 每个 worker `HashJoinIterator` 构造时递增 constructed-worker 计数，并取得共享 chunk wrapper。
3. 每个 worker 的 `HashJoinRowBuffer` 仍拥有自己的 hash map 和 MEM_ROOT；`Init()` 在 `HashTableMutex` 下把 map pointer 注册到 context。
4. build round 开始时所有 workers 进入 execution barrier；completion function 清 `has_more_data` 和 map pointer list/count。
5. workers 并行读取各自 build input，只写自己的 map。遇到 buffer full 时设置共享 `has_more_data=true`。
6. scope guard 中的第二个 execution barrier 分隔 build 和 probe。barrier 完成前不得读取其他 worker map。
7. probe 阶段 `HashJoinRowBuffer::pq_find()/all()` 遍历 context 中所有 maps；此时 map list 和 map contents 只读。
8. 下一次 refill/chunk round 再通过 barrier 清 maps、重建并重新进入只读 probe phase。

`m_hash_maps` 返回无锁 const vector 是阶段隔离设计，不是任意并发安全容器：注册/清理必须发生在 barrier 管控的 build 边界，probe 期间不得修改 vector 或 map。

### 3.4 join key、extra conditions 与 JoinType

1. build row 由全部 equijoin conditions 构造 hash key；普通 `=` 中出现 SQL NULL 时不插入，因为永不匹配；NULL-safe equality 由 condition 自身编码语义处理。
2. probe row 同样构造 key；NULL key 对 inner/semi 直接无输出，对 anti/outer 进入 unmatched 路径。
3. hash lookup 得到候选链后，先恢复 build table buffers，再评估 non-equi extra conditions；只有通过 extra conditions 才算 match。
4. JoinType 输出规则：
   - INNER：返回所有通过条件的 build matches；
   - SEMI：返回第一个 match 后转下一 probe row；
   - ANTI：发现任一 match 即丢弃 probe row；全程无 match 时返回 probe row；
   - OUTER：返回所有 matches；全程无 match 时设置 build input NULL-row flag 并返回一条补 NULL row。
5. semi/anti 在无 extra condition 时可拒绝同一 local map 内重复 key以节省内存；不能把这个优化解释为跨 workers 的全局去重协议。
6. degenerate anti join 无 condition 且 build 非空时可直接 EOF；build 全局是否为空必须等 parallel-aware build barrier 后才能判断。

### 3.5 Bloom filter

- 只有 build partial、probe 非 partial 且存在 equijoin key时才考虑 worker-local Bloom filter。
- build/probe 都 partial 的 shared-map 模式禁用 Bloom filter；constructor 对该组合有断言。
- Bloom 只允许 false positive，不允许 false negative；返回 false 才可直接排除 probe key。
- hash join 转 spill 后禁用 Bloom，因为内存 map 只含部分 build rows，继续用它会错误过滤属于磁盘 chunk 的匹配。

### 3.6 内存预算、refill 与 spill

parallel-aware 内存预算通过 `PQHashJoinSharedContext::MemoryFull()` 汇总各 worker MEM_ROOT allocation delta。任一 worker使总量超过 `join_buffer_size` 后，所有 workers 在 barrier 后按共享 `has_more_data` 判断 build 尚未耗尽。

spill 关闭时：

- iterator 将 `allow_spill_to_disk=false`，通过反复 build/probe refill 完成；
- 非 inner join启用 probe-row saving file，只把尚未匹配 rows 带到下一轮，防止 semi/anti/outer 重复或过早 unmatched 输出；
- 优化阶段估算 refills 超过 `pq_hash_join_max_hash_table_refills`（默认 1）时直接取消 PQ 资格。

spill 开启时：

1. 第一个内存满 worker在 `HashTableMutex` 下初始化共享 build/probe chunk pairs；估算 build rows 乘 DOP。
2. 所有 worker按 join-key hash 将剩余 build/probe rows 写入同一组 chunks；每个 chunk 使用分片 mutex。
3. barrier completion function推进 `CurrentChunkIndex()`、rewind build/probe chunk、清 shared maps。
4. workers从当前 build chunk并发填充各自 maps，barrier 后对同一 probe chunk查全体 maps。
5. outer join probe chunk保存 match flag；build chunk一次装不下或多轮 refill 时，只有所有 rounds 都未匹配才输出 NULL-complemented row。
6. shared wrapper使用 leader THD 的 mutex-enabled VfdManager；worker不得 delete 该 manager。

`ChunkFilesWrapper` 的锁顺序必须保持：shared-context hash-table mutex → wrapper global mutex → chunk mutex → Barrier mutex → VfdManager LRU mutex。`Size()`、`Clear()` 和 current-chunk 的部分无锁访问依赖 barrier/phase前提。

### 3.7 barrier participant 与销毁

- barriers 初始 participant 为 DOP。
- worker iterator构造后计入 `NumberOfConstructedWorkers()`；worker plan construction 的现有调用路径依赖串行化。
- 启动前，对 dependent subquery 重用的 context先 `Reset(DOP)`，此时不得有 active waiter。
- 未构造/未启动的 workers由 `pq_launch_worker()` scope guard对两个 barriers执行 `ArriveAndDrop()`。
- 正常/提前 `End()`：先从 execution barrier drop，令剩余 workers不等已退出线程；随后在 destruction barrier等待，防止本 worker MEM_ROOT 中的 map rows仍被其他 worker读取。
- iterator 已构造但执行前失败且未调用 End：destructor 对两个 barriers drop。

该顺序是避免 skew、partial startup 和 error deadlock的核心契约。worker MEM_ROOT 只有在 destruction barrier允许后才能释放。

### 3.8 semijoin materialization 共享

`Semijoin_mat_exec::pq_clone()` 在 leader PQ mem root中创建共享的 `PQ_shared_info *` cell。`ParallelScanIterator::Init()` 在 MQ、DIV/CUT 和 worker launch之前，对 access paths post-order执行 `para_exec_init()`：leader先完整 materialize并通过 handler发布 shared info。

worker创建 iterator时若发现 shared info已存在，就不构造/执行 materialization child，而把自己的 temp-table handler附着到共享 table。内存临时表已转换成 InnoDB 时，worker按共享 type转换本地表壳。每次 Init都重新 attach，每次 End都 reset handler shared info；底层数据的 owner仍是 leader初始化的共享表。

### 3.9 nested-loop 与 BKA

- 非 hash joins仍在每个 worker cloned plan中按普通 iterator语义执行；PQ 只分片被标记的 DIV/CUT scan。
- `pq_check_not_support_tab()` 禁止把 outer join inner table、semijoin inner table或 active BKA inner table选为普通 DIV_TAB。
- CUT 屏蔽按 access-path node 的直接语义发生：对当前 nested-loop、BKA 或 hash node，若它本身是 semi/anti/outer，则只把该 node 的直接 inner（hash join 为 build）input table map 加入屏蔽集；它不是把该 join 的整个 subtree 一概判为不可 CUT。
- 候选 subtree 内若嵌套了 non-inner node，则该嵌套 node 自己的 inner/build map 仍会被屏蔽；其 outer/probe 一侧及该 node 外的候选路径不因该嵌套 node 被整体排除。`SetCutTable()` 继续只收集普通 TABLE/REF/INDEX/RANGE scan 候选。
- `UseBKA()` 当前只接受可表示为 left-deep 的 BKA outer join；多表 right side不走该 BKA路径。
- outer BKA 与 hash/BNL 的不安全混用通过 `QueryMixesOuterBKAAndBNL()` 让 iterator选择避开两种优化组合。
- 无显式 ORDER 时，hash/BKA造成 row shuffle，stable-sort gate不以 DIV_TAB rowid强行恢复一个虚构的串行顺序。

## 4. 所有权与生命周期

| 对象/资源 | 创建者 | 所有者 | 借用者 | 清理/终点 |
|---|---|---|---|---|
| `PQHashJoinSharedContext` | `MarkPartialInputsForHashJoin()` | gather 的智能指针；storage从 leader PQ mem root取得 | 所有相关 worker iterators | 所有 workers join后随 gather清理 |
| per-worker `HashJoinIterator` | worker access-path iterator factory | worker JOIN/MEM_ROOT | executor | End/destructor |
| per-worker hash map/row MEM_ROOT | `HashJoinRowBuffer::Init()` | 对应 worker row buffer | 其他 workers在 probe phase借用 rows | destruction barrier后才可销毁 |
| `m_hash_maps` pointer vector | shared context | shared context | probe readers | barrier completion下 clear；不得留下悬空 map |
| global memory/count atomics | shared context | shared context | all workers | 每 build round reset/更新 |
| execution barrier | shared context | shared context | all constructed/launched workers | End/drop后 participant减少；重入前 reset |
| destruction barrier | shared context | shared context | workers退出 | 最后 participant到达后允许 MEM_ROOT销毁 |
| `ChunkFilesWrapper` | shared context | `shared_ptr` | all worker iterators | 最后 shared_ptr/context清理 |
| chunk pair/Vfd files | wrapper，使用 leader VfdManager | wrapper/leader VfdManager | workers | Clear/destructor/statement cleanup |
| probe-row saving files | 各 worker iterator | worker iterator，VfdManager来自 leader | 当前 worker | round切换/iterator销毁 |
| Bloom filter | worker iterator | 当前 worker | 当前 probe phase | rebuild/spill禁用/iterator销毁 |
| semijoin `PQ_shared_info` cell | leader clone | leader PQ mem root | leader/worker Semijoin_mat_exec | plan restore/free；handler End只 reset attachment |
| shared materialized table | leader `para_exec_init()` | leader table/handler | worker handlers只读借用 | leader/gather statement cleanup |

## 5. 并发、锁与状态机

### 5.1 parallel-aware round

```text
ROUND_BEGIN
  -> barrier(clear maps, has_more=false)
  -> BUILD_PRIVATE_MAPS
  -> barrier(publish complete map set)
  -> PROBE_ALL_MAPS
  -> REFILL_OR_NEXT_CHUNK
  -> ROUND_BEGIN | END
```

build期间 map只有 owner worker写；probe期间所有 maps只读。禁止一个 worker仍在 `StoreRow()` 时另一个 worker调用 `pq_find()`。

### 5.2 共享状态保护

| 状态 | 保护 |
|---|---|
| map pointer注册/clear | `HashTableMutex`，clear由 barrier completion触发 |
| map contents | 单写 owner + build/probe execution barrier |
| total allocated bytes | atomic fetch-add delta |
| global row count、has-more-build | atomic |
| chunk set增删/推进 | global mutex或barrier completion |
| chunk read/write | chunk-hashed mutex |
| participant/generation | Barrier内部 mutex/cv |
| worker MEM_ROOT释放 | destruction barrier |
| shared temp-table建立 | leader先完成，workers启动后只读attach |

## 6. 规范性不变量

- `PQ-JOIN-INV-001`：包含 DIV/CUT 的 hash input **必须**准确标记 build/probe partial flags；错误标记会产生漏匹配或重复。
- `PQ-JOIN-INV-002`：build、probe都 partial 时，每个 probe lookup **必须**覆盖所有已发布 worker maps；只查local map会漏匹配。
- `PQ-JOIN-INV-003`：parallel-aware build和probe之间**必须**有全员 barrier；probe期间 map list和map contents **不得**修改。
- `PQ-JOIN-INV-004`：worker-owned map rows被其他 worker读取时，其 MEM_ROOT **必须**保持存活，直到 destruction barrier完成。
- `PQ-JOIN-INV-005`：未构造、未启动、提前End或构造后失败的 participant **必须**恰好 drop一次，避免永久等待或计数下溢。
- `PQ-JOIN-INV-006`：barrier `Reset(DOP)` **只能**在无active waiters时执行。
- `PQ-JOIN-INV-007`：对当前 semi/anti/outer join node，其直接 inner/build 保留语义侧**不得**被不安全 CUT；候选 subtree 中嵌套 non-inner node 的 inner/build map 也必须屏蔽。此规则不得被表述为 semi/anti/outer 整个 subtree 都不可 CUT；LIMIT/去重/WEEDOUT下也不得切出跨worker语义缺口。
- `PQ-JOIN-INV-008`：non-equi extra conditions **必须**在候选hash match后、确认 JoinType match之前评估。
- `PQ-JOIN-INV-009`：SEMI每probe row最多输出一次；ANTI发现任一有效match不得输出；OUTER只有全局无match才输出补NULL row。
- `PQ-JOIN-INV-010`：普通 equality的SQL NULL key不得匹配；ANTI/OUTER仍须保留其unmatched语义。
- `PQ-JOIN-INV-011`：non-inner join跨refill/spill时，unmatched row **不得**在后续 build部分尚未检查前提前输出。
- `PQ-JOIN-INV-012`：spill后 Bloom filter **必须**禁用，除非它覆盖全部build data且无false negative证明。
- `PQ-JOIN-INV-013`：shared chunk全局推进/clear和无锁Size/current访问**必须**位于既定barrier phase。
- `PQ-JOIN-INV-014`：parallel-aware spill使用rowid时**必须**遵守cut-selection限制；不得把来自不同worker表壳的rowid混为稳定身份。
- `PQ-JOIN-INV-015`：semijoin shared materialization **必须**在workers读取前由leader完整发布；workers不得并发重复写同一shared table。
- `PQ-JOIN-INV-016`：FULL OUTER不得进入当前hash iterator的成功路径。

## 7. 错误、回退、重试与清理

| 场景 | 当前传播 | 回退/重试 | 必需清理 |
|---|---|---|---|
| CUT/shared-context分配失败 | worker/plan构建返回错误 | 初始化期由PQ fallback/retry策略决定 | 未发布context、plan、handler状态 |
| 部分worker未启动 | leader launch失败路径 | 当前PQ终止或上层retry | 对缺席participant drop，已启动workers置error并join |
| build/probe child Init/Read错误 | HashJoinIterator返回1，worker发error sentinel | 当前PQ终止 | End/destructor drop barrier，MQ detach，worker join |
| join-key/extra-condition错误 | worker THD error | 当前PQ终止 | 同上，不能把error当no-match |
| hash map OOM | `FATAL_ERROR`报告 `ER_OUTOFMEMORY` | 当前PQ终止；BUFFER_FULL则refill/spill | maps/MEM_ROOT按barrier安全释放 |
| 估算refill过多且spill关闭 | 前置取消PQ，原因 `HASH_JOIN_SPILL` | 串行执行 | 无worker资源产生 |
| chunk/probe-saving IO错误 | iterator error | 当前PQ终止 | close Vfd/chunks，drop barriers，join workers |
| KILL/worker提前退出 | Read检查kill；End/drop唤醒其余workers | 不复用当前join round | execution/destruction participants、chunks/maps/MQ全部闭环 |
| shared materialization失败 | leader `para_exec_init()`置 `pq_error`，workers不启动 | 初始化期错误/上层策略 | shared info/table、已建iterators |
| semijoin attach type变化 | worker把本地temp壳转成shared InnoDB type；失败传播 | 当前PQ终止 | reset shared info，释放本地壳 |
| leader LIMIT提前结束 | PQ iterator detach MQ并等待workers | 正常提前停止 | hash End/drop、destruction wait、chunks/files清理 |

整句串行 retry必须重新创建 JOIN、hash maps、barriers和materialized table；不得重用已进入某一 generation 的 shared context。

## 8. 支持范围、限制与开关

| 能力 | 行为状态 | 证据 | 条件/限制 |
|---|---|---|---|
| inner nested-loop join | implemented | static-evidence | DIV候选和scan类型需通过PQ资格 |
| inner hash join | implemented | static-evidence | parallel-oblivious和部分parallel-aware |
| semijoin | partial | static-evidence | inner table不可普通DIV；策略/去重/materialization受约束 |
| antijoin | partial | static-evidence | unmatched必须跨全部build/refill确认 |
| left outer join | partial | static-evidence | inner侧不可不安全切分；NULL-complement和match flag |
| BKA | partial | static-evidence | BKA inner不可DIV；只接受当前left-deep条件，复杂BKA/hash混用降级 |
| shared semijoin materialization | partial | static-evidence | leader预物化、worker只读attach |
| Bloom filter | partial | static-evidence | partial build only；shared two-sided和spill禁用 |
| hash refill | implemented | static-evidence | spill关闭时估算超过阈值取消PQ |
| parallel-aware spill | partial | static-evidence | `hash_join_spill_to_disk`默认关闭；rowid/cut有限制 |
| FULL OUTER | rejected | static-evidence | 当前iterator断言不可达 |
| hypergraph optimizer | serial-fallback | static-evidence | 上层资格检查拒绝 |

关键变量：`join_buffer_size` 是parallel-aware全体workers共享预算，不是每worker各一份；`pq_hash_join_max_hash_table_refills`默认1，仅在parallel hash spill关闭时参与资格；`hash_join_spill_to_disk`默认关闭。

## 9. 可观测性

- EXPLAIN tree显示hash join和partial build/probe信息，可辅助判断parallel-oblivious/aware；DIV/CUT表也应可从plan看出。
- optimizer trace可显示DIV/CUT选择和 `HASH_JOIN_SPILL` 等不适合原因。
- Performance Schema hash-join memory统计、worker timing/examined rows和VFD资源可用于spill诊断。
- DBUG点覆盖OOM、Bloom miss、refill估算、hash worker错误和barrier/launch异常；只有实际fault injection能证明无挂起。
- hang现场至少采集：barrier generation/participant/remaining、constructed/launched workers、每worker state、map count/bytes、has-more、chunk index/row counters、VFD和MQ detach。
- 结果审计需按JoinType比较multiset，并单独验证NULL keys、duplicate keys、extra conditions和unmatched rows；不能只比较行数。

## 10. 源码与测试映射

### 10.1 源码锚点

| 契约 | 源码锚点 |
|---|---|
| CUT选择/partial标记/context建立 | `sql/join_optimizer/access_path.cc`：`SetCutTable()`、`MarkCutTable()`、`IsPartialResultsInputHashJoin()`、`MarkPartialInputsForHashJoin()` |
| hash access-path字段 | `sql/join_optimizer/access_path.h`：`AccessPath::hash_join` partial flags |
| shared context | `sql/parallel_query/pq_hash_join_shared_context.h`、`sql/parallel_query/pq_hash_join_shared_context.cc`：`PQHashJoinSharedContext`、`MemoryFull()` |
| barrier | `sql/parallel_query/barrier.h`：`Reset()`、`ArriveAndWait()`、`ArriveAndDrop()` |
| shared chunks/锁序 | `sql/parallel_query/chunk_files_wrapper.h`：`ChunkFilesWrapper` |
| Bloom | `sql/parallel_query/bloom_filter.h`：`BloomFilter` |
| iterator构造与spill gate | `sql/join_optimizer/access_path.cc` iterator factory；`sql/sql_executor.cc`：`CreateHashJoinAccessPath()` |
| build/probe/JoinType/spill | `sql/iterators/hash_join_iterator.cc`、`sql/iterators/hash_join_iterator.h`：`HashJoinIterator::Init()`、`HashJoinIterator::BuildHashTable()`、`HashJoinIterator::ReadNextHashJoinChunk()`、`HashJoinIterator::Read()` |
| per-worker maps/shared lookup | `sql/iterators/hash_join_buffer.cc`、`sql/iterators/hash_join_buffer.h`：`HashJoinRowBuffer::Init/StoreRow/pq_find/all/size` |
| worker context与barrier reset/drop | `sql/parallel_query/pq_iterators.cc`：`pq_launch_worker()`、`ParallelScanIterator::End()` |
| DIV/CUT handler和partial标记调用 | `sql/parallel_query/sql_parallel.cc`：`pq_make_join_readinfo()` |
| DIV/BKA/outer/semijoin资格 | `sql/parallel_query/pq_optimizer.cc`：`pq_check_not_support_tab()`；`sql/sql_executor.cc`：`UseBKA()`、`QueryMixesOuterBKAAndBNL()` |
| semijoin shared-info clone | `sql/parallel_query/pq_clone.cc`：`Semijoin_mat_exec::pq_clone()` |
| materialization发布/attach | `sql/parallel_query/pq_iterators.cc`：`para_exec_init()`；`sql/iterators/composite_iterators.cc`：`MaterializeIterator::pq_sharing_table/init_shared_table/Init/End` |

### 10.2 现有测试资产

| 主题 | MTR资产 | 证据 |
|---|---|---|
| oblivious/aware、refill、barrier、spill | `pq_hash_join`、`parallel_query/include/pq_on_disk_hash_join.inc` | test-present |
| hash fault injection | `pq_hash_join_error` | test-present |
| semi/materialization策略 | `pq_semijoin` | test-present |
| BKA | `pq_join_bka` | test-present |
| outer/zero rows | `pq_left_join_zero_rows`、`pq_hash_join` 的semi/anti/outer段 | test-present |
| derived/shared materialization | `pq_derived_view` | test-present |
| KILL/worker error/abort | `pq_kill`、`pq_kill_query`、`pq_worker_error`、`pq_abort` | test-present |
| ORDER与hash deadlock | `pq_hash_join` 中 ISSUE 627 场景 | test-present |

## 11. 已知缺口、变更影响与质量风险

### 11.1 当前质量风险

| ID | 风险 | 静态证据/影响 | 建议验证 |
|---|---|---|---|
| `PQ-JOIN-RISK-001` | gather只有一个hash shared context | context类声明面向单个hash node，但遍历可遇到多个two-sided partial nodes | 构造多parallel-aware hash nodes，检查map/chunk/barrier是否串扰；必要时按node隔离context |
| `PQ-JOIN-RISK-002` | barrier participant异常路径漏drop或重复drop | constructor、End、destructor、launch scope多处维护计数 | 部分plan构造/部分thread启动/Init错误/early EOF/KILL组合 |
| `PQ-JOIN-RISK-003` | worker MEM_ROOT过早释放 | shared map rows分配在worker MEM_ROOT，其他worker跨map读取 | ASAN + skew + worker提前End，验证destruction barrier |
| `PQ-JOIN-RISK-004` | map vector无锁读取依赖phase | `HashMaps()`直接返回vector；安全性完全依赖build/probe barrier | TSAN + barrier fault，增加debug phase断言 |
| `PQ-JOIN-RISK-005` | global map count初始化/round reset依赖 | atomic成员无显式member initializer，正常路径靠首个 `ClearHashMaps()` 置0 | 验证所有size/empty调用均晚于clear；建议显式初始化（不属本任务） |
| `PQ-JOIN-RISK-006` | non-inner unmatched跨refill/spill错误 | probe-saving和match flag路径复杂 | semi/anti/outer、NULL、extra condition、多refill、多chunk差分 |
| `PQ-JOIN-RISK-007` | CUT资格遗漏新access path | blacklist依赖逐类枚举 LIMIT/去重/weedout/non-inner | 新iterator接入时做cut-safety审查和negative EXPLAIN测试 |
| `PQ-JOIN-RISK-008` | shared chunk无锁Size/current假设破坏 | wrapper文档依赖唯一初始化者和barrier completion | TSAN + 小buffer/高DOP/VFD=1/KILL |
| `PQ-JOIN-RISK-009` | spill+rowid语义 | MarkCutTable有防护，但新weedout/materialization组合可能绕过 | rowid-required access paths + spill on/off差分 |
| `PQ-JOIN-RISK-010` | semijoin shared table attach/reset生命周期 | leader发布、多个worker handler attach、每次End reset | dependent subquery重复Init/End、heap-to-InnoDB转换、KILL |
| `PQ-JOIN-RISK-011` | test注释与当前eligibility可能漂移 | 部分测试描述parallel-aware semi/anti/outer；当前 `SetCutTable()` 仅屏蔽每个 non-inner node 的直接 inner/build map | 以EXPLAIN partial flags验证实际模式，不仅依赖测试注释 |

### 11.2 变更影响清单

修改以下任一内容时必须重新审查本模块：

- DIV/CUT选择和access-path traversal；
- JoinType方向、build/probe约定或extra-condition placement；
- `PQHashJoinSharedContext`粒度、map注册、memory accounting；
- Barrier participant/generation或worker launch/End顺序；
- hash-row MEM_ROOT ownership；
- chunk/VFD/rowid/spill/refill；
- semi/anti/outer unmatched和duplicate策略；
- BKA/nested-loop资格；
- semijoin/derived materialization sharing；
- stable ORDER对hash/BKA的gate。

变更审查必须同步核对 `PQ-JOIN-INV-*`、模块05的DIV/CUT扫描、模块06的error/detach、模块07的ORDER/LIMIT，以及当前支持矩阵。
