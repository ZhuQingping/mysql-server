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

状态：Task Contract Created。

下一步：

- 先执行 V2-12A-3.1，新增 gate 且不改变 GROUP BY fallback；
- 再进入 V2-12A-3.2 factory skeleton。
