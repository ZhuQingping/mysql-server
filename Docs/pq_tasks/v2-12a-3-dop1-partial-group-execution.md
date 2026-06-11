# V2-12A-3 DOP1 Partial Group Execution

## 目标

实现一个默认关闭、显式 gate 控制、DOP=1 only 的 GROUP BY partial aggregation 最小执行路径。

本阶段目标不是完整 GROUP BY 并行能力，而是把 `AGGREGATE(TABLE_SCAN)` 子树替换点、iterator contract、typed partial state 和最终结果输出链路打通到可测试状态。所有不在支持范围内的查询必须继续 serial fallback。

## 核心结论

- 接入点固定在 `sql/join_optimizer/access_path.cc` 的 `AccessPath::AGGREGATE` 分支；
- 不从 `AccessPath::TABLE_SCAN` / `PQTableScanIterator` hook 硬塞 GROUP BY 语义；
- 不新增 `AccessPath` 类型；
- 不修改原生 `AggregateIterator`；
- 不修改 InnoDB `row0pread.*` 核心；
- 默认 OFF，需要新 experimental gate 显式开启；
- 第一段只做 DOP=1，并且只支持直接字段 group key 和直接字段 aggregate argument。

## 支持范围

第一版只允许：

- 单表 InnoDB clustered full scan；
- `GROUP BY` key 为同一 base table 的直接字段；
- group key 类型先限定为非 BLOB/TEXT 的整数类型；
- aggregate argument 为同一 base table 的直接字段，或 `COUNT(*)`；
- aggregate function：
  - `COUNT(*)`
  - `COUNT(col)`
  - `SUM(int_col)`
  - `MIN(int_col)`
  - `MAX(int_col)`
- DOP=1；
- `WHERE + GROUP BY` 可在后续子任务打开，首个版本可先要求无 WHERE 或仅复用已稳定的 leader-side predicate/projection boundary。

必须 fallback：

- DOP > 1；
- HAVING；
- ROLLUP；
- ORDER BY；
- DISTINCT aggregate；
- `AVG`、`GROUP_CONCAT`、JSON aggregate、bit aggregate、variance/stddev、UDF aggregate；
- group key 或 aggregate argument 是表达式、函数、子查询、非直接字段；
- BLOB/TEXT、复杂 charset/collation string key；
- secondary index / ICP / partition table；
- multi-table / join；
- worker-side Item/JOIN clone 需求。

## 设计要求

### 1. Experimental Gate

新增 session sysvar：

```text
parallel_query_experimental_groupby_dop1
```

要求：

- 默认 OFF；
- OFF 时显式 GROUP BY 继续走 `GROUP_BY_PARTIAL_AGG_UNSUPPORTED` fallback；
- ON 时也只允许严格白名单 shape；
- gate 不得影响现有 implicit aggregate threaded row stream；
- `pq_vars` 需要覆盖新变量默认值和 set/reset。

### 2. Eligibility

`pq_optimizer` 的显式 GROUP BY 检查需要拆成两层：

- 诊断层：继续保留 `GROUP_BY_ROLLUP`、`GROUP_BY_HAVING`、`GROUP_BY_UNSUPPORTED_EXPR`、`GROUP_BY_UNSUPPORTED_AGGREGATE`；
- 执行层：只有 gate ON 且 `pq_groupby_dop1_supported()` 通过时，才允许 query block 标记为 PQ eligible。

不要为了通过 eligibility 放宽 ORDER BY、DISTINCT、multi-table、partition 等既有 guard。

### 3. Iterator Factory

新增 factory，例如：

```c++
unique_ptr_destroy_only<RowIterator> TryCreatePQGroupAggregateIterator(
    THD *thd, MEM_ROOT *mem_root, JOIN *join, AccessPath *aggregate_path,
    unique_ptr_destroy_only<RowIterator> child);
```

接入位置：

- `AccessPath::AGGREGATE`；
- child iterator 创建后、创建原生 `AggregateIterator` 前；
- factory 返回 nullptr 时无条件保留原生 `AggregateIterator` 路径。

shape 检查：

- `path->type == AccessPath::AGGREGATE`；
- `param.rollup == false`；
- `param.child->type == AccessPath::TABLE_SCAN`；
- child table 与 `join` 中 PQ eligibility 记录一致；
- `join->pq_eligible == true`；
- `parallel_query_experimental_groupby_dop1 == true`；
- `parallel_default_dop == 1`。

### 4. PQGroupAggregateIterator

新增文件：

```text
sql/parallel_query/pq_group_aggregate_iterator.h
sql/parallel_query/pq_group_aggregate_iterator.cc
```

职责：

- `Init()`：在安全窗口内准备 DOP=1 执行；
- 内部扫描输入 table，构建 leader-local group table；
- `Read()`：逐个输出最终 group row；
- 发生不支持或 worker 启动前错误时允许返回 factory nullptr 或 Init fallback；
- 一旦进入 started 状态，不允许 silent fallback，必须 error/abort。

第一阶段可以选择同步 DOP=1 scan，不强制马上启动 worker thread；但代码命名和状态必须为后续 worker partial group 留出边界。

### 5. Typed Partial State

现有 `PQ_partial_agg` 是 double-based，只能作为历史 infrastructure，不能直接承载生产 GROUP BY 语义。

新增 typed state，建议先覆盖：

- signed/unsigned 64-bit integer group key；
- `COUNT` 使用 `uint64`；
- `SUM` 区分 signed/unsigned，第一版明确溢出策略；
- `MIN/MAX` 使用 field native integer value；
- NULL group key 和 nullable aggregate argument 需要显式记录。

DECIMAL、DOUBLE、string collation、DATE/TIME、AVG 放到 V2-12A-5。

### 6. 输出 Row 构造

这是本阶段最大风险点。

要求：

- 最终输出必须写入 MySQL 上层期望的 item/tmp/result buffer；
- 不允许只算出 hash table 后绕过 SQL 层结果协议；
- 如果无法稳定复用 `AggregateIterator` 的输出 buffer，先做 debug/smoke iterator 和 counter，不打开真实 SQL result；
- 不修改 `sql/item_sum.*` 和原生 `AggregateIterator`。

## 状态变量

建议新增：

- `Parallel_groupby_dop1_attempts`
- `Parallel_groupby_dop1_executed`
- `Parallel_groupby_dop1_fallback`
- `Parallel_groupby_partial_groups_built`
- `Parallel_groupby_groups_output`

计数规则：

- gate ON 且 factory 尝试创建 iterator，attempts +1；
- 成功完整输出结果，executed +1；
- factory 拒绝但仍 serial fallback，fallback +1 或沿用既有 fallback 语义，需在实现说明中固定；
- worker/iterator started 后错误不得计为 fallback。

## MTR 计划

### 正向

新增：

```text
pq_groupby_dop1_count
pq_groupby_dop1_sum_min_max
```

关键断言：

- gate OFF 时仍 fallback；
- gate ON + DOP=1 时结果与 serial 一致；
- `Parallel_queries_executed` 或 groupby executed counter 增量符合预期；
- fallback delta 为 0；
- groups output counter 等于结果 group 数；
- 测试结束恢复所有 session vars。

### 负向

新增或扩展：

```text
pq_groupby_dop1_gate_negative
pq_groupby_dop1_unsupported
```

覆盖：

- gate OFF；
- DOP=2；
- HAVING；
- ORDER BY；
- ROLLUP；
- DISTINCT aggregate；
- expression group key；
- expression aggregate argument；
- BLOB/TEXT group key。

## 验证

每个子提交至少执行：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_diagnostics pq_groupby_partial_group_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_base \
  --tmpdir=/tmp/pqt_groupby_base
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby --tmpdir=/tmp/pqt_full_groupby
```

新增正向执行后必须追加 targeted MTR。

## 禁止修改

- `sql/iterators/composite_iterators.*`
- `sql/item_sum.*`
- `storage/innobase/row/row0pread.*`
- `sql/join_optimizer/access_path.h`
- handler / InnoDB virtual API，除非先更新任务书并单独 review。

## 推荐拆分

### V2-12A-3.1 Task Contract And Gate

- 新增任务书；
- 新增 sysvar；
- 保持 GROUP BY 仍 fallback；
- `pq_vars` 覆盖新变量；
- 完整 suite 通过。

### V2-12A-3.2 Factory Shape Guard

- 新增 `TryCreatePQGroupAggregateIterator()` skeleton；
- 接入 `AccessPath::AGGREGATE`；
- factory 默认返回 nullptr 或 gated skeleton；
- 不改变现有结果。

### V2-12A-3.3 Typed State Smoke

- 新增 typed group state；
- synthetic input 验证 count/sum/min/max merge；
- 不接 SQL result。

### V2-12A-3.4 DOP1 SQL Result Smoke

- gate ON；
- 支持 `GROUP BY int_col COUNT(*)`；
- 结果与 serial 一致；
- 成功后再扩展 `SUM/MIN/MAX`。

## 当前状态

状态：V2-12A-3.4f temp-table shape helper Completed；真实 GROUP BY SQL result 接管仍未打开。

已完成：

- 新增默认 OFF 的 `parallel_query_experimental_groupby_dop1` session/global sysvar；
- `pq_vars` 覆盖新变量默认值、set/reset 和 global 可见性；
- GROUP BY eligibility 和执行路径保持不变，显式 GROUP BY 继续 fallback。
- 新增 `TryCreatePQGroupAggregateIterator()` skeleton；
- 在 `AccessPath::AGGREGATE` 分支接入 factory；
- factory 当前不接管 child iterator ownership，严格返回 `nullptr`，原生 `AggregateIterator` 行为保持不变。
- 新增 in-memory typed GROUP BY state smoke，覆盖 integer group key、`COUNT(*)`、`COUNT(col)`、`SUM`、`MIN`、`MAX` 和 NULL aggregate argument。
- 在 `AccessPath::TEMPTABLE_AGGREGATE` 分支接入 factory skeleton；
- 新增 `Parallel_groupby_dop1_factory_attempts` / `Parallel_groupby_dop1_factory_fallback`，验证 gate ON + DOP=1 时 GROUP BY factory hook 可观测；
- 新增 `pq_groupby_dop1_factory_observable`，确认 factory hook 仍返回 `nullptr` 并保持原生 GROUP BY 结果正确。
- `TryCreatePQGroupAggregateIterator()` / `TryCreatePQTemptableGroupAggregateIterator()` 已接收 child iterator ownership carrier 指针，后续真实 iterator 可在通过白名单后显式 `std::move()` 接管；当前仍不移动 ownership。
- `TryCreatePQTemptableGroupAggregateIterator()` 已接收 `Temp_table_param`、output `TABLE` 和 `ref_slice`，后续真实 iterator 可复用原生 temp-table 输出链路；当前仍返回 `nullptr`。
- 新增 temp-table shape helper，识别 `GROUP BY` 单个整数 key + 单个 `COUNT(*)` 的候选形态，并通过 status counter 观察；当前仍不放开 eligibility、不移动 ownership、不接管输出。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars pq_groupby_diagnostics \
  --parallel=1 --vardir=/tmp/pqv_groupby_gate \
  --tmpdir=/tmp/pqt_groupby_gate
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_diagnostics pq_groupby_partial_group_smoke pq_vars \
  --parallel=1 --vardir=/tmp/pqv_groupby_factory \
  --tmpdir=/tmp/pqt_groupby_factory
TMPDIR=/tmp ./mtr --suite=parallel_query pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_typed \
  --tmpdir=/tmp/pqt_groupby_typed
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_typed_state_smoke pq_groupby_partial_group_smoke pq_stats \
  --parallel=1 --vardir=/tmp/pqv_groupby_typed \
  --tmpdir=/tmp/pqt_groupby_typed
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_stats pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_obs \
  --tmpdir=/tmp/pqt_groupby_obs
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_typed \
  --tmpdir=/tmp/pqt_full_groupby_typed
```

结果：完整 `parallel_query` suite 通过，共 54 项。

下一步：

- V2-12A-3.4 DOP1 SQL result smoke 需要先确认当前传统优化器下 `GROUP BY` 的实际 iterator access path。

## V2-12A-3.4 预研记录

只读 Agent 建议的最小路径：

- 只支持 `GROUP BY int_col, COUNT(*)`；
- 使用 `Item_sum_count::make_const(count)` 输出每组 count；
- 使用 `StoreFromTableBuffers()` / `LoadIntoTableBuffers()` 保存和恢复 group key 代表行；
- 不修改 `AggregateIterator` 和 `item_sum.*`。

本地验证结论：

- 已尝试实现 gated SQL result smoke；
- `SELECT val, COUNT(*) FROM t1 GROUP BY val` 和 `SELECT id, COUNT(*) FROM t1 GROUP BY id` 结果正确，但 `Parallel_queries_executed` 未增长，说明当前查询没有进入 `AccessPath::AGGREGATE` factory；
- 这意味着当前接入点对传统优化器 GROUP BY 形态不足，可能实际走 `TEMPTABLE_AGGREGATE` 或其他 legacy aggregation 路径；
- 未提交该实验代码，避免引入不可观测或不可验证的执行路径。

后续要求：

- 先做只读 path tracing，确认 GROUP BY 在当前 optimizer/executor 下的实际 access path；
- 如果是 `TEMPTABLE_AGGREGATE`，需要单独设计接入点，不应把 3.4 强塞到 `TABLE_SCAN` hook；
- 保持 `sql/iterators/composite_iterators.*` 和 `sql/item_sum.*` 不修改。

### V2-12A-3.4 Path Tracing 结论

只读 Agent 结论：

- `SELECT val, COUNT(*) FROM t1 GROUP BY val` 在 `val` 无有序索引时通常走 `AccessPath::TEMPTABLE_AGGREGATE`，创建 `TemptableAggregateIterator`；
- `SELECT id, COUNT(*) FROM t1 GROUP BY id` 在主键有序输入下可能走 `INDEX_SCAN + AccessPath::AGGREGATE`，但当前 factory 只允许 `AGGREGATE(TABLE_SCAN)`，因此也不会接管；
- gate ON 只影响 PQ eligibility 诊断，不改变 optimizer plan；
- 当前显式 GROUP BY 仍在 eligibility 阶段被 `GROUP_BY_PARTIAL_AGG_UNSUPPORTED` 拒绝。

3.4 正确拆分：

1. `V2-12A-3.4a`：只读/可观测 path tracing，新增 debug/status 或 MTR 证明目标 SQL 的 access path；
2. `V2-12A-3.4b`：为 `TEMPTABLE_AGGREGATE` 设计 gated factory skeleton，先返回 nullptr；
3. `V2-12A-3.4c`：仅在明确白名单下尝试 `GROUP BY int_col COUNT(*)` SQL result smoke。

### V2-12A-3.4b TEMPTABLE_AGGREGATE Factory Skeleton

状态：Completed。

目标：

- 在 `AccessPath::TEMPTABLE_AGGREGATE` 分支建立 PQ GROUP BY factory hook；
- factory 当前仍返回 `nullptr`；
- 不接管 `subquery_path` / `table_path` child ownership；
- 不改变原生 `TemptableAggregateIterator` 行为。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_diagnostics pq_groupby_partial_group_smoke \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_temp_factory \
  --tmpdir=/tmp/pqt_groupby_temp_factory
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_temp_factory \
  --tmpdir=/tmp/pqt_full_temp_factory
```

结果：完整 `parallel_query` suite 通过，共 53 项。

禁止事项：

- 不强行改变 optimizer，让 temp-table group 变成 streaming aggregate；
- 不修改 `setup_tmptable_write_func()`、`make_group_fields()` 或 `streaming_aggregation` 语义；
- 不全局放开显式 GROUP BY eligibility；
- 不绕过 `TemptableAggregateIterator` 的 spill/slice/sum-func 语义，除非 PQ iterator 完整复制并有独立验证。

### V2-12A-3.4c Factory Observability

状态：Completed。

目标：

- 在不接管 SQL result 的前提下，使 DOP=1 GROUP BY factory hook 可观测；
- gate OFF 时不增加 GROUP BY factory counters；
- gate ON + DOP=1 时，`TEMPTABLE_AGGREGATE` / `AGGREGATE` factory 尝试可通过 status counter 验证；
- factory 继续返回 `nullptr`，原生 iterator 继续负责 GROUP BY 结果输出。

实现：

- 新增 `Parallel_groupby_dop1_factory_attempts`；
- 新增 `Parallel_groupby_dop1_factory_fallback`；
- `TryCreatePQGroupAggregateIterator()` / `TryCreatePQTemptableGroupAggregateIterator()` 在 experimental gate 命中后递增 attempts，在返回 `nullptr` 前递增 fallback；
- 新增 `pq_groupby_dop1_factory_observable`。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_stats pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_obs \
  --tmpdir=/tmp/pqt_groupby_obs
```

结果：targeted suite 通过。完整 suite 需随提交前验证。

下一步：

- V2-12A-3.4d 进入 `TEMPTABLE_AGGREGATE` SQL result smoke 设计/实现；
- 需要先扩展 factory 接口，使其能接收 `subquery_iterator`、`table_iterator`、`Temp_table_param`、output `TABLE` 和 `ref_slice`；
- 真实接管必须复用临时表输出链路，不直接向客户端发送 row。

### V2-12A-3.4d Ownership Carrier

状态：Completed。

目标：

- 在 `AccessPath::AGGREGATE` factory 中接收 child iterator ownership carrier；
- 在 `AccessPath::TEMPTABLE_AGGREGATE` factory 中接收 `subquery_iterator` 和 `table_iterator` ownership carrier；
- 本步骤不移动 child ownership，不创建真实 PQ group iterator，不改变原生执行结果；
- 后续只有在 shape 白名单完全通过后，factory 才能 `std::move()` child iterator 并返回真实 iterator。

实现：

- `TryCreatePQGroupAggregateIterator(..., unique_ptr_destroy_only<RowIterator> *child_iterator)`；
- `TryCreatePQTemptableGroupAggregateIterator(..., unique_ptr_destroy_only<RowIterator> *subquery_iterator, unique_ptr_destroy_only<RowIterator> *table_iterator)`；
- `access_path.cc` 在原生 iterator 创建前传入 `job.children[]` 地址；
- factory 继续返回 `nullptr`，原生 `AggregateIterator` / `TemptableAggregateIterator` 保持唯一执行路径。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_carrier \
  --tmpdir=/tmp/pqt_groupby_carrier
```

下一步：

- 扩展 factory 参数读取 `Temp_table_param`、output `TABLE` 和 `ref_slice`；
- 新增只读/diagnostic 白名单 helper，确认 `GROUP BY int_col COUNT(*)` 的 temp-table shape；
- 仍不修改 `TemptableAggregateIterator` 和 `item_sum.*`。

### V2-12A-3.4e Temp-table Carrier

状态：Completed。

目标：

- 将 `AccessPath::TEMPTABLE_AGGREGATE` 的 temp-table output contract 传入 PQ factory；
- 暴露 `Temp_table_param`、output `TABLE`、`ref_slice` 给后续真实 PQ group iterator；
- 本步骤不读取/写入 temp table，不移动 child ownership，不改变原生执行路径。

实现：

- `TryCreatePQTemptableGroupAggregateIterator(..., Temp_table_param *temp_table_param, TABLE *table, int ref_slice)`；
- `access_path.cc` 传入 `param.temp_table_param`、`param.table`、`param.ref_slice`；
- factory 做 null/ref_slice guard 后仍返回 `nullptr`。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_temp_carrier \
  --tmpdir=/tmp/pqt_groupby_temp_carrier
```

下一步：

- 新增只读 shape helper，识别 `GROUP BY int_col COUNT(*)` 的 temp-table 参数布局；
- 在 helper 可观测后，再决定是否创建真正的 PQ temp-table group iterator。

### V2-12A-3.4f Temp-table Shape Helper

状态：Completed。

目标：

- 在 factory 内只读识别 `GROUP BY int_col, COUNT(*)` 的 temp-table 聚合形态；
- 通过 status counter 区分 supported/unsupported shape；
- 不改变 eligibility，显式 GROUP BY 仍可因 `GROUP_BY_PARTIAL_AGG_UNSUPPORTED` 走原生路径；
- 不接管 child iterator ownership，不写 temp table。

实现：

- 新增 `Parallel_groupby_temp_shape_supported`；
- 新增 `Parallel_groupby_temp_shape_unsupported`；
- shape 条件：
  - `Temp_table_param::group_parts == 1`；
  - `Temp_table_param::sum_func_count == 1`；
  - `table->group` 单一 group key；
  - group key tmp field 为整数类型；
  - `join->sum_funcs` 中只有一个 `Item_sum::COUNT_FUNC`，且非 distinct。
- 扩展 `pq_groupby_dop1_factory_observable` 验证 gate ON 后可观察到 supported shape。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_stats pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_shape \
  --tmpdir=/tmp/pqt_groupby_shape
```

下一步：

- 增加 eligibility 白名单，但仍先返回 native fallback；
- 再实现真正的 `TEMPTABLE_AGGREGATE` PQ iterator，复用 temp table output contract。

### V2-12A-3.4g Eligibility Whitelist

状态：Completed。

目标：

- 对显式 `GROUP BY` 增加最小 eligibility 白名单，让 gate ON + DOP=1 的候选查询可以到达 GROUP BY factory 诊断；
- `parallel_query_experimental_groupby_dop1=OFF` 或 DOP>1 时继续 `GROUP_BY_PARTIAL_AGG_UNSUPPORTED`；
- 仅对这个显式 GROUP BY 候选允许 optimizer 已创建的 tmp table 继续通过 full-scan 检查；
- factory 仍返回 `nullptr`，真实结果仍由原生 `TemptableAggregateIterator` 输出。

实现约束：

- 不放宽 ORDER BY、DISTINCT、multi-table、partition、secondary index 等既有 guard；
- 不移动 child iterator ownership；
- 不写 temp table；
- 不修改 `TemptableAggregateIterator`、`AggregateIterator`、`item_sum.*` 或 InnoDB 核心；
- EXPLAIN 只用于确认候选状态，不能代表真实 GROUP BY PQ iterator 已执行。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_elig \
  --tmpdir=/tmp/pqt_groupby_elig
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_elig \
  --tmpdir=/tmp/pqt_full_groupby_elig
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted GROUP BY suite 通过：`pq_groupby_dop1_factory_observable`、`pq_groupby_diagnostics`、`pq_groupby_typed_state_smoke`；
- 完整 `parallel_query` suite 通过，共 55 项；
- 验证过程中发现 LIMIT/OFFSET early-stop 会暴露 worker/read-view 生命周期风险，已通过 `PQTableScanIterator::EndPSIBatchModeIfStarted()` 在 statement unlock 前清理并 join worker，保留 LIMIT threaded counter 预期。

下一步：

- 后续进入真正的 `TEMPTABLE_AGGREGATE` PQ iterator，实现可验证的 temp-table 写入和结果输出。

### V2-12A-3.5 Temp-table Native Delegate Wrapper

状态：Completed。

目标：

- 在 `AccessPath::TEMPTABLE_AGGREGATE` factory 中真正返回 PQ-owned iterator；
- PQ iterator 接管 `subquery_iterator` / `table_iterator` ownership；
- 第一小步内部委托原生 `temptable_aggregate_iterator::CreateIterator()` 完成 temp table 写入和读取；
- 验证 output temp table contract、ref slice、batch-mode forwarding 和结果输出链路可执行；
- 不实现 worker partial aggregation，不修改 `TemptableAggregateIterator` / `item_sum.*`。

实现约束：

- 仅在 gate ON、DOP=1、`join->pq_eligible`、temp shape supported 时 selected；
- unsupported shape 或非 eligible 继续 native fallback；
- selected 计数和 native delegate executed 计数只表示 PQ wrapper 接管，不表示 worker partial group 已执行；
- `Parallel_queries_executed` 仍只保留 threaded row-stream execution 语义，不被 native delegate wrapper 污染。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_stats pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_delegate \
  --tmpdir=/tmp/pqt_groupby_delegate
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_delegate \
  --tmpdir=/tmp/pqt_full_groupby_delegate
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted suite 通过：`pq_groupby_dop1_factory_observable`、`pq_stats`、`pq_groupby_diagnostics`、`pq_groupby_typed_state_smoke`；
- 完整 `parallel_query` suite 通过，共 55 项。

下一步：

- 再把 wrapper 的 `Init()` 从 native delegate 替换为 PQ-owned temp-table 写入循环。

### V2-12A-3.6 PQ-owned Temp-table Write Loop

状态：Completed。

目标：

- 移除 3.5 wrapper 内部的 native delegate；
- PQ wrapper 自己驱动 `subquery_iterator`，写入 output temp table，并通过 `table_iterator` 输出最终结果；
- 继续复用 MySQL helper：`copy_funcs()`、`init_tmptable_sum_functions()`、`update_tmptable_sum_func()`、`instantiate_tmp_table()`、`create_ondisk_from_heap()`；
- 不修改原生 `TemptableAggregateIterator` 和 `item_sum.*`；
- 不接 worker partial state，仍是 leader-local DOP=1 temp-table 聚合执行。

实现：

- `PQTemptableGroupAggregateIterator::Init()` 复用原生 temp-table aggregate contract：
  - 输入阶段切到 `REF_SLICE_SAVED_BASE`；
  - 创建/清空 output temp table；
  - 使用 index 0 查找已有 group；
  - group 命中时 `update_tmptable_sum_func()` + `ha_update_row()`；
  - 新 group 时切到 `ref_slice`，`copy_funcs()` + `init_tmptable_sum_functions()` + `ha_write_row()`；
  - 完成后 `table()->materialized = true` 并初始化 `table_iterator`。
- 新增 `Parallel_groupby_dop1_temp_table_executed`；
- 保留 `Parallel_groupby_dop1_native_delegate_executed` 作为 3.5 历史观测项，本步骤期望其 delta 为 0。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_stats pq_groupby_diagnostics \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_owned \
  --tmpdir=/tmp/pqt_groupby_owned
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_owned \
  --tmpdir=/tmp/pqt_full_groupby_owned
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted suite 通过：`pq_groupby_dop1_factory_observable`、`pq_stats`、`pq_groupby_diagnostics`、`pq_groupby_typed_state_smoke`；
- 完整 `parallel_query` suite 通过，共 55 项。

下一步：

- 扩展正向 MTR：COUNT/SUM/MIN/MAX、NULL、更多 group 数；
- 再把 leader-local temp-table 聚合内部替换为 PQ partial state merge。

### V2-12A-3.7 SUM/MIN/MAX Coverage

状态：Completed。

目标：

- 将 supported temp-table shape 从单 `COUNT` 放宽到单个整数聚合：
  - `COUNT`
  - `SUM(int_col)`
  - `MIN(int_col)`
  - `MAX(int_col)`
- 继续拒绝 DISTINCT aggregate、表达式参数、非整数参数和多 aggregate；
- 新增正向 MTR 覆盖 NULL 输入和多个 group。

实现：

- `pq_groupby_dop1_temp_shape_supported()` 支持 `Item_sum::SUM_FUNC`、`MIN_FUNC`、`MAX_FUNC`；
- SUM/MIN/MAX 要求唯一参数是 `Item_field` 且字段类型为整数；
- 新增 `pq_groupby_dop1_sum_min_max`，验证 selected、temp-table executed、fallback delta 和 shape supported counters。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_sum_min_max pq_groupby_dop1_factory_observable \
  pq_stats pq_groupby_diagnostics \
  --parallel=1 --vardir=/tmp/pqv_groupby_smm \
  --tmpdir=/tmp/pqt_groupby_smm
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_smm \
  --tmpdir=/tmp/pqt_full_groupby_smm
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted suite 通过；
- 完整 `parallel_query` suite 通过，共 56 项。

下一步：

- 增加负向 MTR：DOP=2、HAVING、ORDER BY、DISTINCT、表达式 group key、表达式 aggregate argument、非整数/BLOB/TEXT；
- 再推进 PQ partial state merge。

### V2-12A-3.8 Unsupported Shape Coverage

状态：Completed。

目标：

- 增加 GROUP BY DOP1 负向覆盖，确认 unsupported shape 不进入 PQ-owned temp-table execution；
- 覆盖 optimizer guard 和 factory shape guard 两类拒绝路径；
- 保证 selected/temp-table executed counters 不增长。

覆盖：

- DOP=2；
- HAVING；
- ORDER BY；
- DISTINCT aggregate；
- expression group key；
- expression aggregate argument；
- string group key。

验证：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_unsupported pq_groupby_dop1_sum_min_max \
  pq_groupby_dop1_factory_observable pq_stats \
  --parallel=1 --vardir=/tmp/pqv_groupby_neg \
  --tmpdir=/tmp/pqt_groupby_neg
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_neg \
  --tmpdir=/tmp/pqt_full_groupby_neg
```

结果：

- targeted suite 通过；
- 完整 `parallel_query` suite 通过，共 57 项。

下一步：

- 推进 PQ partial state merge，逐步替换 leader-local temp-table update loop 的 aggregate state 来源。

### V2-12A-3.9 COUNT Typed-State Temp-Table Output

状态：Completed。

目标：

- 将最小安全范围的 `GROUP BY int_not_null, COUNT(*)` 从逐输入行更新 temp table，切到 typed-state 聚合后一次性写 temp table rows；
- 保留 SUM/MIN/MAX 现有 temp-table update loop，避免在本步骤引入 DECIMAL、overflow、all-NULL 语义风险；
- 增加可观测状态变量，确认新路径真实执行。

实现边界：

- 新增 `PQ_count_group_state`，按单个 NOT NULL signed integer group key 累计 COUNT；
- 新路径只在以下条件同时满足时启用：
  - 非 hash group key；
  - 单 group key；
  - key 临时表输出字段为 NOT NULL signed integer；
  - 单个 `COUNT` aggregate；
  - COUNT 参数不可为 NULL（覆盖 `COUNT(*)` / `COUNT(non_nullable_expr)`）；
- 写最终输出行时使用 `(*table->group->item)->get_tmp_table_field()` 写 group key，使用 `Item_sum::get_result_field()` 写 COUNT 结果；
- 新增 `Parallel_groupby_dop1_typed_count_executed` 状态变量；
- `pq_groupby_dop1_factory_observable` 验证 typed COUNT path delta；
- `pq_stats` 更新 Parallel 状态变量数量为 35。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_factory_observable pq_stats \
  --parallel=1 --vardir=/tmp/pqv_typed_count_verify \
  --tmpdir=/tmp/pqt_typed_count_verify
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop1_sum_min_max pq_groupby_dop1_unsupported \
  pq_groupby_typed_state_smoke \
  --parallel=1 --vardir=/tmp/pqv_groupby_typed_reg \
  --tmpdir=/tmp/pqt_groupby_typed_reg
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_typed_count \
  --tmpdir=/tmp/pqt_full_typed_count
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- COUNT typed path targeted suite 通过；
- GROUP BY SUM/MIN/MAX 与 unsupported 回归通过。
- 完整 `parallel_query` suite 通过，共 57 项。

下一步：

- 扩展 typed-state 到 MIN/MAX（先保持整数 Field::store 语义）；
- SUM 需要单独处理 DECIMAL/REAL result field 与 overflow/all-NULL 语义，不与 MIN/MAX 混在一个提交里推进；
- nullable group key 和 hash group key 继续保持保守路径或后续显式设计。
