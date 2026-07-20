# 07 聚合、ORDER 与 LIMIT 契约

- 模块 ID：`PQ-MOD-007`
- 文档状态：`current-contract`，仅代表当前稳定基线静态实现
- 代码基线：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9 (`stable_branch`)`
- 基线范围：manifest 中的 commit、`stable_branch` 已提交基线、`source_tree_sha256` 与 `test_tree_sha256` 仅覆盖其定义的 PQ source/test file set，不声明覆盖整个 workspace。
- 静态复核日期：2026-07-20
- 运行时验证：见 `current/quality/verification_evidence.md`；不把 suite 成功解释为模块级全覆盖
- 上游契约：`../00_architecture_overview.md`、`../01_current_support_matrix.md`
- 下游协议：`06_mq_exchange_record_protocol.md`

## 1. 职责与非职责

### 1.1 职责

本模块定义 PQ 高层结果算子的当前契约：

- 把可分解聚合改写为 worker partial aggregate 和 leader final aggregate；
- 为 `SUM`、`COUNT`、`AVG`、`MIN`、`MAX` 和 `GROUP BY` 重建 leader 聚合表达式；
- 处理隐式分组/空输入，避免空 worker 发送伪造 partial row；
- 在安全条件下把 HAVING 放到 worker，否则保留在 leader final result 上；
- 支持特性开关控制的 `COUNT(DISTINCT)` worker tree/batch 与 leader global merge；
- 选择 worker filesort、ordered index、Exchange_sort merge 或 leader 后置普通 sort；
- 在需要 stable output 时用 DIV_TAB rowid 打破相等 sort key；
- 控制 LIMIT 是否可下推到 worker、leader 最终 LIMIT、无 ORDER BY LIMIT 的产品开关，以及 OFFSET pushdown 与 PQ 的优先级。

### 1.2 非职责

本模块不负责：

- ring queue 的 framing、detach 和字段 raw 编码细节；见模块 06；
- 定义 SQL 聚合函数的一般串行实现；本模块只描述 PQ 改写如何复用这些实现；
- 支持 window、ROLLUP、`GROUP_CONCAT`、JSON aggregate、variance/stddev、bit aggregate、`SUM(DISTINCT)` 或 `AVG(DISTINCT)`；当前资格检查拒绝这些形态；
- 承诺无 ORDER BY LIMIT 返回与某次串行物理扫描相同的行；SQL 未指定顺序且产品开关默认接受任意满足条件的 N 行；
- 承诺浮点 partial reduction 与固定串行加法顺序 bit-for-bit 相同；两阶段合并改变结合顺序，列为语义差分风险；
- 证明所有 warning/overflow/charset/collation 组合均已运行验证。

## 2. 入口、输入与输出

### 2.1 入口

| 入口 | 作用 |
|---|---|
| `JOIN::save_optimized_vars()` / `restore_optimized_vars()` | 保存并重建 GROUP/ORDER/HAVING、streaming aggregation 等优化结果 |
| `JOIN::set_push_down_having()` | 判定 HAVING 是否可由 worker 执行 |
| `JOIN::make_worker_tmp_table()` | 创建与 leader 接收布局一致的 worker 输出临时表 |
| `JOIN::make_leader_tmp_table()` | 创建 leader 接收临时表并重建 final aggregate |
| `pq_replace_avg_func()` | 标记 leader AVG 布局，使临时字段可携带 sum+count |
| `pq_build_sum_funcs()` | 把 leader temp-table `Item_field` 重建为 final `Item_sum` |
| `Item_sum_*::pq_rebuild_sum_func()` | 按具体聚合类型构造 leader combine 函数 |
| `ParallelScanIterator::pq_make_filesort()` | 构造 Exchange_sort 所需 group/order/stable Filesort |
| `MQ_record_gather::mq_scan_init()` | 选择有序或无序 Exchange |
| `disable_pq_if_limit_without_orderby()` | 根据会话开关拒绝无 ORDER LIMIT 的 PQ |
| worker plan clone 的 LIMIT 处理 | 对 aggregate/特定 rebuilt-group 禁止 worker LIMIT pushdown |

### 2.2 输入

- 优化后的 select fields、GROUP BY、HAVING、ORDER BY、DISTINCT、LIMIT/OFFSET；
- worker 和 leader 的 temp-table schema/ref slice；
- aggregate 类型、参数类型、NULL 属性、decimal precision/scale；
- worker 的局部 partial rows；
- ordered-index 使用状态、最后一个 filesort 位置、reverse group 方向；
- `parallel_limit_no_order_by`、`op_over_pq_offset_threshold`、`pq_support_features_switch`；
- stable output 所需 DIV_TAB rowid。

### 2.3 输出

- leader final aggregate/group rows；
- 满足 HAVING 的最终 rows；
- 按 ORDER BY 全局有序并在要求时稳定的 rows；
- 应用最终 OFFSET/LIMIT 后的 result；
- 不适合 PQ 时的明确资格原因，如 `LIMIT_NO_ORDERBY`、`AGGR_DISTINCT`、`COUNT_DISTINCT_NOT_SUPPORT_NEED_TEMP`、不支持聚合类型或 OFFSET pushdown 优先；
- worker/leader aggregate、sort、MQ 的错误和 warnings 汇总到 leader diagnostics。

## 3. 主流程

### 3.1 计划保存、worker clone 与 leader 重写

1. 串行优化完成后，`save_optimized_vars()` 保存 grouped、implicit grouping、streaming aggregation、ordered-index usage、GROUP/ORDER 列表以及 HAVING。
2. `set_push_down_having()` 基于表达式性质决定 HAVING 归属；决策随模板计划复制到 worker。
3. worker plan clone 重建 Items 和 temp table；`Query_result_mq` 从 `REF_SLICE_PQ_TMP` 发送 partial rows。
4. leader 调用 `restore_optimized_vars()` 恢复 GROUP/ORDER 与最终 HAVING 视图，随后 `make_leader_tmp_table()` 创建 `<receive_data>` 临时表。
5. leader 把原始 aggregate 映射到接收临时表字段，通过 `pq_build_sum_funcs()` 构造第二阶段 combine aggregate。
6. leader 在重写后的 plan 上再次建立 GROUP/sort/aggregate access paths，并从 MQ temp rows 计算最终结果。

worker/leader temp table 的字段数和 `reclength` 必须完全一致；worker 创建时对照 gather 保存的 leader 布局，不一致即发 error sentinel 并终止。

### 3.2 两阶段聚合映射

| 原聚合 | worker 发送 | leader 重建/合并 | 关键语义 |
|---|---|---|---|
| `SUM(x)` | 每 worker/group 的局部 SUM | 对 partial SUM 再执行 `Item_sum_sum` | 全 NULL group 保持 NULL；decimal 精度与 warning 需一致 |
| `COUNT(x/*)` | 每 worker/group 的局部 count | `Item_sum_count(..., is_fake=true)` 把输入 count 相加 | 不能把 partial row 个数误当原始行数 |
| `AVG(x)` | 局部 sum 作为 value，并通过 extra bytes 携带局部 count | `PQ_REBUILD` AVG 累加 sum 与 extra count，最终除总 count | count 为 0 时 NULL；worker 不先发送已经除过的平均值 |
| `MIN(x)` | 局部最小值 | 对 partial minima 再取 MIN | collation、temporal、NULL 规则复用 `Item_sum_hybrid` |
| `MAX(x)` | 局部最大值 | 对 partial maxima 再取 MAX | 同上 |
| `GROUP BY g` | 每 worker 按 g 局部聚合 | leader 再按同一 g 聚合 partial groups | 同一 g 可来自多个 workers，必须再次合并 |

`AVG` 的字段布局是专用协议：`PQ_WORKER` 返回 sum 而不是 avg，`val_extra()` 发送 `m_count`；leader `PQ_REBUILD` 的 `add/reset_field/update_field` 将输入 extra count 加入总 count。decimal 临时字段扩大以同时容纳 sum 与 `longlong` count，leader resolve 阶段避免重复增加 `div_precincrement`。

### 3.3 空输入与 NULL

`AggregateIterator::Read()` 对 PQ 有显式空-worker分支：

- 某个 worker 没读到任何输入时，不发送 `(COUNT=0, 其他字段=NULL)` 之类的伪 partial row；否则非聚合字段可能被 leader 错误污染。
- 如果所有 workers 都不发送行，leader 自己在 final aggregate 的空输入分支生成 SQL 所要求的一行隐式聚合结果：例如 COUNT 为 0，SUM/AVG/MIN/MAX 为 NULL。
- 有显式 GROUP BY 且无输入时不生成 group row。
- NULL 参数不增加 COUNT/AVG count；MIN/MAX/SUM 的 NULL 行继续由现有 Item_sum 语义过滤。

该分支是结果正确性的硬契约，不能用“每 worker 总要发一个 aggregate row”的常规直觉替换。

### 3.4 GROUP BY 与 streaming merge

1. worker 对各自输入做 local grouping，向 MQ 发送 group key 与 partial aggregates。
2. 若原计划用 `ORDERED_INDEX_GROUP_BY`，`pq_rebuilt_group` 被标记；worker 最后临时表增加按原 group fields 的排序，确保每个 worker 输出局部有序。
3. leader `pq_make_filesort()` 从保存的 group list 重建 merge order；`mark_desc_groups()` 标记倒序索引 group，避免 leader 固定按升序误合并。
4. `Exchange_sort` 合并各 worker group streams；leader streaming aggregate 可在相邻相同 key 上做 final combine。
5. 如果 leader 不采用有序 streaming merge，则由 leader temp table/普通 grouping 完成全局去重和聚合；不能省略第二阶段。

release 构建中还存在针对单主表、ordered-index GROUP BY + LIMIT 的记录数启发式，决定 PQ 预估扫描量是否劣于串行；该 gate 位于 `#ifdef NDEBUG`，debug/release 行为差异列为质量风险。

### 3.5 HAVING 归属

`JOIN::set_push_down_having()` 当前规则：

- HAVING 含 aggregate 或 grouping function：不得下推，worker 的 HAVING 清空，leader 对 final aggregate 评估。
- select list 含 aggregate、查询无 GROUP BY 且未优化掉 group：即使 HAVING 自身无 aggregate，也不得下推。原因是隐式聚合空输入仍会在 leader 产生一行，该行必须由 leader HAVING 决定是否过滤。
- 其余被判定安全的 HAVING 可下推：worker 保留 query-block HAVING，leader 将自身 HAVING 置空，避免重复过滤。

下推的 HAVING 必须只依赖 worker 已有的局部语义；任何新增 aggregate/grouping expression 识别都要同步修改该决策。

### 3.6 `COUNT(DISTINCT)` 特殊路径

当前只为 `COUNT(DISTINCT ...)` 提供专门 PQ combine：

1. 每个 worker 的 `Aggregator_distinct` 建立 Unique tree；NULL key 不加入。
2. worker 把 tree 的 keys 写入 `Batch_buffer`，必要时从内存转 cached file。
3. MQ 不复制 tree 内容，而发送 `Batch_buffer *` 原生指针。
4. leader `Aggregator_distinct::merge_count_distinct_tree()` 读取每个 batch，把 keys 加入 leader 自己的 global Unique tree；读完 release buffer。
5. leader `endup()` 按 global distinct key 数产生最终 count。

限制：

- `count_distinct` feature flag 默认关闭，必须显式开启；
- `SUM(DISTINCT)`、`AVG(DISTINCT)` 仍在不支持列表；
- aggregate 参数含 BLOB/TEXT 类不支持字段时拒绝；
- `need_tmp_before_win` 与 count distinct 组合拒绝；window 本身也不在当前 PQ 支持范围；
- 指针协议只适用于同进程且依赖 batch manager 生命周期，详见模块 06。

### 3.7 ORDER BY 与 stable output

ORDER 可能落在三种位置：

1. worker-side filesort/ordered index 产生局部有序流，leader `Exchange_sort` 全局归并；
2. leader 为 stable table/index scan 根据使用索引字段构造 merge Filesort，即使显式 ORDER 已被优化掉，也可恢复索引键比较；reverse range 同步反转 order direction；
3. 若 sort 位于 rewritten primary tables 之后的 leader temp stage，则保留/重建 leader 普通 filesort，而不是假设 MQ 流已经满足最终 ORDER。

`pq_check_stable_sort()` 会在不适合 merge-stable 的形态关闭 stable 输出。尤其在无显式 ORDER 且计划含 hash join 或 BKA 时，不强制按 DIV_TAB rowid 稳定归并：这些 join 已重排 rows，强制 stable 没有串行语义价值，并可能形成 worker 等 barrier、MQ 满、leader 等特定 worker 的死锁环。

有显式 ORDER 时，Exchange_sort 比较完整 sort key；只有要求 stable 时才在 key 相等后比较 rowid。SQL 只要求 ORDER key 的偏序时，stable tie-break 是实现稳定性策略，不改变 key 顺序。

### 3.8 LIMIT、OFFSET 与提前停止

最终 LIMIT 始终由 leader plan 保证。worker LIMIT 是否保留取决于安全性：

- 含 aggregate 时，worker clone 把 `m_select_limit` 和 `select_limit_cnt` 设为无限，避免在 partial aggregate 前截断原始输入。
- rebuilt GROUP 且最后 sort 位于 primary tables 之后时，同样禁止 worker LIMIT pushdown，避免每 worker 截断 group/sort 后破坏 global result。
- 其他安全形态可让 worker 保留局部 LIMIT；leader 仍施加 global LIMIT。ORDER BY top-N 依赖每 worker 局部有序，global top-N 必定位于各局部 top-N 的并集；无 ORDER 时接受任意 N 行。
- leader `LimitOffsetIterator` 达到最终 limit 后返回 EOF；PQ iterator 清理先 detach 所有 queues，再等 workers，使阻塞 sender 退出。
- `parallel_limit_no_order_by=false` 时，非内部、非隐式单行分组且无 ORDER 的 LIMIT 令 PQ 资格失败；默认值为 true。
- optimizer 发现可向 InnoDB push OFFSET 时，PQ 与 offset pushdown 不同时启用：offset 小于 `op_over_pq_offset_threshold` 时保留 PQ 并放弃 offset pushdown；达到阈值时禁用 PQ，采用 handler offset pushdown。
- worker 不能在并行 range 中独立跳过全局 OFFSET，因为各 worker 缺乏全局顺序/计数；OFFSET 应在 leader 最终流上执行，或选择非 PQ 的 handler pushdown 路径。

## 4. 所有权与生命周期

| 对象/资源 | 创建者 | 所有者 | 借用者 | 生命周期终点 |
|---|---|---|---|---|
| saved GROUP/ORDER/HAVING | original `JOIN` | leader PQ plan/pq mem root | template/worker clone | leader plan restore/free |
| worker aggregate Items | worker plan clone | worker `pq_mem_root`/JOIN | worker iterators、Query_result_mq | worker JOIN/THD 清理 |
| worker output temp table | `make_worker_tmp_table()` | worker JOIN | encoder | `Query_result_mq::cleanup()`/worker free |
| leader receive temp table | `make_leader_tmp_table()` | leader JOIN | Exchange decode、final aggregate/sort | leader JOIN/tmp-table cleanup |
| leader final `Item_sum` | `pq_build_sum_funcs()` | leader `pq_mem_root` | final AggregateIterator | leader plan cleanup |
| AVG sum/count state | worker/leader Item/Field | 对应 Item/临时 field | MQ encoder/decoder | Item/table cleanup；不能只复制 sum 丢失 count |
| Filesort/Sort_param | plan/`pq_make_filesort()` | leader/worker JOIN 或 Exchange_sort | local sort/global merge | sort/Exchange cleanup |
| stable rowid | DIV_TAB handler 当前 record → MQ record cache | cache record | heap comparator | record cache/Exchange cleanup |
| distinct `Batch_buffer` | worker 从 leader-owned handle manager 获取 | handle manager/slot | worker tree writer、leader global merge | leader merge release；manager 最终析构 |
| diagnostics/warnings | worker THD | worker，结束时合并到 template/leader | leader result/error reporting | statement结束 |

leader final aggregate 不得持有 worker Item 或 worker temp-table storage 的裸引用；跨线程传递必须经过 MQ raw value/extra state，count distinct 的受控 batch pointer 是当前唯一特例。

## 5. 并发、锁与状态机

### 5.1 聚合阶段

```text
worker W0..Wn independently:
  SCAN -> LOCAL GROUP/AGG -> LOCAL SORT(optional) -> MQ -> DONE

leader:
  MQ MERGE/DECODE -> FINAL GROUP/AGG -> HAVING -> FINAL SORT -> OFFSET/LIMIT
```

worker aggregate state完全私有；leader combine 只消费已经物化的 partial rows。除 count-distinct batch 生命周期外，聚合函数内部状态不在 workers 之间共享。

### 5.2 ORDER 状态

- local filesort/ordered index 完成后才能发布有序流；
- Exchange_sort 为每个 active worker 保留 head/batch，heap 只在所有未完成流都有 head 或 EOF 后输出；
- leader early LIMIT 可在 heap/aggregate 上游仍有数据时触发 detach；worker 侧必须把 detach 视为正常提前终止，不再阻塞发送。

### 5.3 diagnostics

每个 worker 有独立 THD/Diagnostics_area。worker 结束时在 leader/template THD 的 query lock 下调用 `pq_merge_status()` 并复制 conditions。错误、warning 数量和顺序可能受并发结束顺序影响；对同一数据的语义一致性需要差分验证。

## 6. 规范性不变量

- `PQ-AGG-INV-001`：所有可分解聚合**必须**保留足以重建 final result 的状态；AVG **必须**同时传 sum 和 count。
- `PQ-AGG-INV-002`：COUNT 的 leader combine **必须**累加 worker counts，不得计算 partial rows 的数量。
- `PQ-AGG-INV-003`：同一 GROUP key 可出现在多个 worker；leader **必须**进行全局 regroup/combine，不能直接串接 partial groups。
- `PQ-AGG-INV-004`：无输入的 worker **不得**为 top-level PQ final aggregate发送伪空行；所有 workers 无输入时，leader **必须**产生串行 SQL 规定的空输入结果。
- `PQ-AGG-INV-005`：NULL 过滤、empty-set 结果、decimal precision/scale、overflow 和 warning 处理**应**与对应串行 Item_sum 语义一致。
- `PQ-AGG-INV-006`：含 aggregate/grouping function 的 HAVING **必须**在 leader final aggregate 后评估；不得基于 worker partial value提前过滤。
- `PQ-AGG-INV-007`：被安全下推的 HAVING 在 leader **不得**再次作为 final HAVING 重复应用；未下推时 worker **不得**应用。
- `PQ-AGG-INV-008`：`COUNT(DISTINCT)` 的 leader **必须**按原始 distinct keys 做全局去重，不能相加各 worker distinct counts。
- `PQ-AGG-INV-009`：count-distinct batch key layout、tree key length 与 leader global tree **必须**一致；每个 batch **必须**恰好 release 一次。
- `PQ-ORDER-INV-001`：Exchange_sort 的每个 worker input **必须**按相同比较规则局部有序。
- `PQ-ORDER-INV-002`：全局 ORDER **必须**比较完整 sort key，包含 ASC/DESC、NULL、collation 和 varlen 规则；不能只比较首字段。
- `PQ-ORDER-INV-003`：需要 stable output 且 sort key 相等时**必须**以稳定 rowid 打破平局；rowid 必须来自同一 divided-table handler 语义。
- `PQ-ORDER-INV-004`：hash join/BKA 造成重排且无显式 ORDER 时，系统**不得**把 rowid merge 描述为 SQL 顺序保证。
- `PQ-LIMIT-INV-001`：leader **必须**应用最终 OFFSET/LIMIT；任何 worker local limit 都只能是语义安全的上游剪枝。
- `PQ-LIMIT-INV-002`：aggregate 或需完整 rebuilt-group 输入时，worker **不得**在聚合/分组前截断到 query LIMIT。
- `PQ-LIMIT-INV-003`：无 ORDER LIMIT 启用时只承诺基数与谓词正确，不承诺与串行物理扫描选择相同行；关闭开关时**必须**转串行。
- `PQ-LIMIT-INV-004`：leader 达到 LIMIT 后**必须**detach queues 再等待 workers，防止 sender 因 ring 满永久阻塞。
- `PQ-LIMIT-INV-005`：PQ worker **不得**各自执行全局 OFFSET skip；handler offset pushdown 与 PQ 同一 query block **不得**同时生效。

## 7. 错误、回退、重试与清理

| 场景 | 当前传播 | 回退/重试 | 清理要求 |
|---|---|---|---|
| Item clone/rebuild/refix 失败 | leader/worker plan build 失败 | 早期可 graceful fallback；leader plan深度改写后通常 abort，由上层整句 retry 决定 | 恢复/释放 ref slices、tmp tables、JOIN |
| worker/leader temp schema 不一致 | worker 发 error sentinel并置 `pq_error` | 当前 PQ 终止 | worker temp table、MQ、leader gather |
| aggregate add/finalize 错误 | worker 或 leader THD error | 当前 PQ 终止；可由全句 retry 策略处理 | aggregate/temp buffers、worker join |
| AVG extra count/field 保存错误 | 报 PQ error | 不得只用 sum 继续 | 清理 Item/field 状态并恢复 original metadata |
| empty-input field save 错误 | leader 转为 `ER_PARALLEL_QUERY_ERROR` | 当前 PQ 终止 | 清空临时 NULL flags/fields |
| Filesort/Sort_param/heap 分配失败 | gather 初始化或读阶段错误 | 初始化期可能回退；执行期终止 | sort buffers、Exchange、workers |
| count-distinct batch 分配/IO/read 错误 | error sentinel或 leader error | 当前 PQ 终止 | release buffer/slot，关闭 cached file |
| HAVING 表达式错误 | 所在 worker/leader THD error | 当前 PQ 终止 | diagnostics 合并、worker stop |
| leader 达 LIMIT | 正常 EOF，随后 detach | 正常提前终止 | detach 全 queues、join workers、释放 sort/aggregate |
| KILL | worker/leader error path | 不重用当前 PQ 状态 | 同上，并合并正确 kill diagnostics |

串行 retry 必须重新优化/执行完整语句，不能复用已经消费部分 MQ、partial aggregate 或 partially advanced LIMIT 状态。

## 8. 支持范围、限制与开关

| 能力 | 行为状态 | 证据 | 条件/限制 |
|---|---|---|---|
| `SUM/COUNT/AVG/MIN/MAX` | implemented | static-evidence | 支持类型还受 field/function 资格检查 |
| 隐式聚合 | implemented | static-evidence | `simple_aggregate` feature 默认开启；单表无 GROUP 的场景受该 flag gate |
| `GROUP BY` | implemented | static-evidence | worker partial + leader final；ordered-index/group-limit 有成本 gate |
| HAVING | partial | static-evidence | aggregate HAVING 留 leader；安全非聚合 HAVING 可下推 |
| `COUNT(DISTINCT)` | partial | static-evidence | `count_distinct` feature 默认关闭；特殊 batch pointer 协议 |
| `SUM/AVG(DISTINCT)` | serial-fallback | static-evidence | 资格检查拒绝 |
| SELECT DISTINCT | partial | static-evidence | 可能转换为 GROUP/leader temp 去重；复杂形态按支持矩阵判断 |
| ORDER BY | implemented | static-evidence | local sort/index + global merge，或 leader 后置 sort |
| stable output | partial | static-evidence | 依赖 rowid；hash/BKA 无 ORDER 时不强制 |
| LIMIT + ORDER | partial | static-evidence | local top-N 仅在安全形态；leader final limit |
| LIMIT 无 ORDER | partial | static-evidence | `parallel_limit_no_order_by`；结果行集合可非确定 |
| OFFSET | partial | static-evidence | leader skip；大 offset 可选择 handler pushdown并禁用 PQ |
| window/ROLLUP | serial-fallback | static-evidence | 上层资格检查拒绝 |
| `GROUP_CONCAT`/JSON/UDF/variance/bit aggregate | serial-fallback | static-evidence | 聚合类型表拒绝 |

`pq_support_features_switch` 当前默认集合包含 `simple_aggregate`，不包含 `count_distinct`。`parallel_limit_no_order_by` 默认 true，`op_over_pq_offset_threshold` 默认 1000。

## 9. 可观测性

- optimizer trace 记录 PQ 不适合原因、DOP 和 merge_sort 选择；EXPLAIN 可见 local sort、global gather、aggregate 层。
- `pq_unsuite_info` 能区分 feature switch、unsupported aggregate、count-distinct temp 限制、无 ORDER LIMIT、offset priority 等前置拒绝。
- worker timing/examined rows 在结束时汇总到 template/leader；diagnostics conditions 也被合并。
- 对结果差分，应分别比较：无序 multiset、有序 row sequence、NULL/empty、decimal/warning、LIMIT cardinality。无 ORDER LIMIT 不应直接文本顺序 diff。
- 运行诊断应记录 `pq_rebuilt_group`、`pq_last_sort_idx`、`pq_stable_sort`、`m_ordered_index_usage`、HAVING pushdown flag、worker/local limit 与 leader final limit。

## 10. 源码与测试映射

### 10.1 源码锚点

| 契约 | 源码锚点 |
|---|---|
| GROUP/ORDER/HAVING 保存恢复 | `sql/parallel_query/pq_optimizer.cc`：`save_optimized_vars()`、`restore_optimized_vars()`、`set_push_down_having()` |
| aggregate 支持表/feature gate | `sql/parallel_query/pq_optimizer.cc`：`NO_PQ_SUPPORTED_AGG_FUNC_TYPES`、`check_simple_agg()`、`check_count_distinct()` |
| AVG 标记/final rebuild | `sql/parallel_query/sql_parallel.cc`：`pq_replace_avg_func()`、`pq_build_sum_funcs()` |
| 具体 sum rebuild | `sql/parallel_query/pq_clone_item.cc`：`Item_sum_count::pq_rebuild_sum_func()`、`Item_sum_sum::pq_rebuild_sum_func()`、`Item_sum_avg::pq_rebuild_sum_func()`、`Item_sum_min::pq_rebuild_sum_func()`、`Item_sum_max::pq_rebuild_sum_func()` |
| worker/leader temp table | `sql/sql_select.cc`：`JOIN::make_worker_tmp_table()`、`make_leader_tmp_table()`、`make_tmp_tables_info()` |
| empty input | `sql/iterators/composite_iterators.cc`：`AggregateIterator::Read()` |
| AVG sum/count 计算 | `sql/item_sum.h`：`PqAvgType`；`sql/item_sum.cc`：`Item_sum_avg::val_extra()`、`Item_sum_avg::reset_field()`、`Item_sum_avg::update_field()`、`Item_avg_field::val_extra()` |
| count distinct tree/batch | `sql/item_sum.cc`：`Aggregator_distinct::save_distinct_to_mq()`、`merge_count_distinct_tree()` |
| worker record/batch encoding | `sql/parallel_query/query_result_mq.cc`：`send_data()`、`pq_build_mq_count_distinct_item()` |
| leader merge sort构造 | `sql/parallel_query/pq_iterators.cc`：`pq_make_filesort()`、`pq_init_record_gather()` |
| group desc/local sort/stable gate | `sql/parallel_query/sql_parallel.cc`：`mark_desc_groups()`、`pq_make_join_readinfo()`、`pq_check_stable_sort()` |
| global k-way merge | `sql/parallel_query/exchange_sort.cc`：`Exchange_sort::init()`、`Exchange_sort::build_heap()`、`Exchange_sort::read_mq_record()`、`heap_compare_records()` |
| worker LIMIT suppression | `sql/parallel_query/pq_clone.cc`：`pq_make_join()` 中 select limit 处理 |
| 无 ORDER LIMIT gate | `sql/parallel_query/pq_optimizer.cc`：`disable_pq_if_limit_without_orderby()` |
| OFFSET/PQ 优先级 | `sql/sql_select.cc`：offset pushdown 与 `op_over_pq_offset_threshold` 分支 |
| leader final OFFSET/LIMIT | `sql/iterators/composite_iterators.cc`：`LimitOffsetIterator::Init()`、`LimitOffsetIterator::Read()` |

### 10.2 现有测试资产

| 主题 | MTR 资产 | 证据 |
|---|---|---|
| SUM/COUNT/AVG/MIN/MAX/GROUP | `pq_group_by`、`pq_aggr_no_record`、`pq_clone_item` | test-present |
| NULL/empty/AVG precision/overflow | `pq_aggr_no_record`、`pq_group_by` 中 AVG/overflow 回归 | test-present |
| HAVING | `pq_group_by`、`pq_clone_item`、`BUG2023110202800` 所在回归资产 | test-present |
| count distinct/feature gate | `pq_agg_distinct`、`pq_support_features_switch` | test-present |
| SELECT DISTINCT | `pq_distinct` | test-present |
| ORDER/stable/reverse | `pq_order_by`、`pq_order_const`、`pq_reverse_index_scan`、`pq_range_scan_reverse` | test-present |
| LIMIT/ORDER/OFFSET | `pq_limit_no_order_by`、`pq_group_by` | test-present |
| error/resource | `pq_mq_error`、`pq_worker_error`、`pq_memory_limit` | test-present |
| hash join + order deadlock regression | `pq_hash_join` 中 ISSUE 627 场景 | test-present |

## 11. 已知缺口、变更影响与质量风险

### 11.1 当前质量风险

| ID | 风险 | 静态证据/影响 | 建议验证 |
|---|---|---|---|
| `PQ-AGG-RISK-001` | AVG sum/count layout 漂移 | worker/leader Item type、field pack length、extra count 多处协作 | decimal/real、NULL、空输入、group、overflow 的 PQ/NO_PQ 差分 |
| `PQ-AGG-RISK-002` | 空 worker 伪 partial row 回归 | 正确性依赖 AggregateIterator 的 PQ 专用 EOF 分支 | DOP 大于页/行数、谓词只命中单 worker、全空输入 |
| `PQ-AGG-RISK-003` | HAVING 错误下推 | aggregate 和隐式分组空行需要 leader final evaluation | aggregate/nonaggregate/grouping function/constant/error expression 组合 |
| `PQ-AGG-RISK-004` | 浮点 reduction 顺序差异 | partial sums 改变加法结合顺序 | 定义允许误差并做高动态范围、NaN/Inf、DOP 差分 |
| `PQ-AGG-RISK-005` | warnings/overflow 合并顺序不稳定 | worker diagnostics 按线程完成并发汇总 | strict mode、decimal overflow、truncation，核对 errno/warning count |
| `PQ-AGG-RISK-006` | count-distinct pointer/slot 泄漏 | error/detach 后 batch 可能未进入正常 merge-release | ASAN + KILL/early LIMIT/IO fault/slot=1 |
| `PQ-AGG-RISK-007` | ordered group 方向或局部单调性破坏 | reverse index、optimized-away group/order 和 merge sort 共同决定顺序 | ASC/DESC/NULL/collation、多 worker 同 key 边界 |
| `PQ-AGG-RISK-008` | release-only GROUP+LIMIT gate | 成本 gate 被 `NDEBUG` 条件包围 | 对 debug/release EXPLAIN 资格做一致性审计并记录预期差异 |
| `PQ-ORDER-RISK-001` | stable rowid 被错误用于 join shuffle | 无 ORDER hash/BKA 已显式关闭；未来 plan type 可能漏判 | 新 join iterator 接入时扩展 gate，跑死锁与顺序测试 |
| `PQ-LIMIT-RISK-001` | worker local LIMIT 下推形态漏判 | aggregate/rebuilt-group 有显式禁用，其余依赖 top-N 安全推理 | ORDER+ties、DISTINCT、derived/UNION、join、OFFSET 组合 |
| `PQ-LIMIT-RISK-002` | 无 ORDER LIMIT 差分误报或产品预期不清 | 默认允许任意 N 行 | 测试比较谓词和行数，不比较固定文本顺序；产品文档显式说明 |
| `PQ-LIMIT-RISK-003` | early detach 资源闭环 | leader 到 limit 时 workers 可能正阻塞 MQ/sort/aggregate | KILL/limit=0/limit=1、小 ring、慢 worker 的资源归零测试 |

### 11.2 变更影响清单

修改以下任一内容时必须重新审查本模块：

- Item_sum 的 result type、tmp-field layout、NULL/empty/overflow 语义；
- aggregate clone/rebuild 或 ref slice；
- worker/leader temp table schema；
- HAVING pushdown规则；
- GROUP ordered-index、streaming aggregation、reverse direction；
- Filesort、Exchange_sort、stable rowid；
- LIMIT/OFFSET access path 或 worker clone pushdown；
- count-distinct feature flag、Unique tree、Batch_buffer protocol；
- diagnostics 合并顺序或串行 retry。

变更审查必须同时核对 `PQ-AGG-INV-*`、`PQ-ORDER-INV-*`、`PQ-LIMIT-INV-*`、模块 06 协议和支持矩阵。
