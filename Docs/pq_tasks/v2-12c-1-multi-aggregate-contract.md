# V2-12C-1 Multi Aggregate Contract

## 状态

Design Completed。

本任务只做只读调研和编码任务书，不修改源码。

## 目标

在 V2-12B 已完成的 DOP2/DOP4 单 aggregate partial GROUP BY 基础上，定义多
aggregate 的最小可实现契约，作为 V2-12C-2 编码输入。

首段目标 shape：

```sql
SELECT grp, COUNT(*), SUM(val) FROM t GROUP BY grp;
SELECT grp, COUNT(nullable_val), SUM(val) FROM t GROUP BY grp;
SELECT grp, MIN(val), MAX(val) FROM t GROUP BY grp;
```

仍然只支持：

- 单表 InnoDB clustered full scan；
- DOP=2 / DOP=4；
- 单个 signed integer NOT NULL group key；
- aggregate 参数为同一 base table 的 signed integer field；
- explicit experimental gates。

## 只读调研结论

### 当前白名单边界

`pq_groupby_dop1_temp_shape_supported()` 当前要求：

- `temp_table_param->precomputed_group_by == false`；
- `temp_table_param->group_parts == 1`；
- `temp_table_param->sum_func_count == 1`；
- `table->group->next == nullptr`；
- `join->sum_funcs[0] != nullptr && join->sum_funcs[1] == nullptr`。

因此多 aggregate 第一处变更是新增独立白名单 helper，不能直接放宽原 helper。

### 当前 DOP partial path 边界

`PQTemptableGroupAggregateIterator::can_use_dop_partial_path()` 当前输出：

- 一个 `source_table`；
- 一个 `group_field_index`；
- 一个 `value_field_index`；
- 一个 `PQ_partial_group_agg_kind`。

`InitDopPartialPath()` 当前按一个 `Item_sum *sum = m_join->sum_funcs[0]` 写一个
`sum->get_result_field()`。

多 aggregate 必须把这个契约扩展为 aggregate descriptor array，否则无法可靠写入多个
result field。

### 当前 worker/leader merge 边界

`PQ_partial_group_mq_sink` 当前 worker 本地状态已经使用
`PQ_partial_group_merge_slot_v1`，该 slot 同时保存：

- `count_star`；
- `count_value`；
- `sum`；
- `min`；
- `max`。

`PQ_partial_group_payload_v1` 也同时携带这些字段。`agg_kind` 当前主要用于 leader
选择最终写哪个 result field，而不是限制 payload 只能保存一种统计量。

因此 V2-12C-2 不需要引入 payload v2。更低风险的做法是：

- 保持 `PQ_partial_group_payload_v1` 不变；
- worker 对每个 group 继续一次性累计 count/sum/min/max；
- leader merge 后根据 aggregate descriptor array，把同一个 merge slot 的不同字段写入不同 result field。

### MySQL temp-table result field 映射

原生 temp-table aggregate 路径通过：

- `init_tmptable_sum_functions(m_join->sum_funcs)` 初始化所有 aggregate；
- `update_tmptable_sum_func(m_join->sum_funcs, table())` 更新所有 aggregate；
- 每个 `Item_sum` 使用自己的 `get_result_field()` 写 temp-table result field。

`Temp_table_param::sum_func_count` 表示 aggregate 数量，`join->sum_funcs` 是 nullptr
结尾数组。

PQ multi aggregate path 应该沿用这个映射：

- 遍历 `join->sum_funcs[i]`，直到 nullptr；
- 每个 supported aggregate 生成一个 descriptor；
- 每个 descriptor 保存 `Item_sum *sum`、`Field *result_field`、`kind`、可选 `value_field_index`；
- 写 output temp table 时按 descriptor 顺序写对应 `result_field`。

不要通过 temp-table field 顺序猜测 aggregate result field。

## 推荐契约

### 新增内部 descriptor

在 `pq_group_aggregate_iterator.cc` 内部新增局部结构，先不放到公共头文件：

```cpp
struct PQ_partial_group_agg_desc {
  Item_sum *sum{nullptr};
  Field *result_field{nullptr};
  uint32 value_field_index{0};
  PQ_partial_group_agg_kind kind{PQ_partial_group_agg_kind::COUNT};
  bool count_nullable_arg{false};
};
```

约束：

- `sum` 和 `result_field` 必须非空；
- `kind` 仅允许 `COUNT` / `SUM` / `MIN` / `MAX`；
- `COUNT(*)` 可复用 group field 作为 `value_field_index`，但写结果必须取 `count_star`；
- `COUNT(nullable field)` 写结果取 `count_value`；
- `SUM/MIN/MAX` 写结果取 `sum/min/max`，无非 NULL 值时写 NULL。

### descriptor 数量

首段只支持 2 个 aggregate：

- `COUNT(*) + SUM(field)`；
- `COUNT(nullable_field) + SUM(field)`；
- `MIN(field) + MAX(field)`。

暂不支持 3 个及以上 aggregate，即使 payload 内部已有字段，也先 fallback。

原因：

- 2 个 aggregate 足够验证 result field mapping；
- worker sink 仍然只有一个 `value_field_index`，首段可要求多个非 COUNT aggregate 使用同一 value field；
- 避免把 `COUNT(*) + SUM(a) + MIN(b)` 这类 value field 多样性提前引入。

### worker sink 契约

V2-12C-2 可以保留 `PQ_partial_group_mq_sink` 的一个 `value_field_index`：

- `COUNT(*) + SUM(val)`：`value_field_index = val`；
- `COUNT(nullable_val) + SUM(val)`：首段要求 `nullable_val == val`，否则 fallback；
- `MIN(val) + MAX(val)`：`value_field_index = val`。

这会牺牲一部分 shape 覆盖，但能避免 worker sink 同时读取多个 value field。

后续若要支持 `COUNT(nullable_col) + SUM(other_col)`，再把 worker sink 扩展为 value
descriptor array。

### leader 写 temp table 契约

新增 helper：

```cpp
bool write_dop_partial_group_result(
    const PQ_partial_group_merge_slot_v1 &slot,
    const PQ_partial_group_agg_desc *descs,
    uint32 desc_count);
```

行为：

- 先 `empty_record(table())`；
- 写 group key；
- 遍历 descriptor；
- 按 `kind` 从 slot 写对应 `result_field`；
- 任一 `result_field == nullptr` 直接 error；
- `SUM/MIN/MAX` 在 `slot.has_value == false` 时写 NULL；
- `COUNT(*)` 写 `count_star`；
- `COUNT(nullable field)` 写 `count_value`。

### fallback 契约

以下 shape 必须 serial fallback：

- `sum_func_count == 0` 或 `sum_func_count > 2`；
- `DISTINCT` aggregate；
- `AVG`；
- expression aggregate；
- DECIMAL/DOUBLE/string/temporal aggregate；
- nullable group key；
- multiple group key；
- hash group key；
- `COUNT(nullable_col) + SUM(other_col)`；
- `COUNT(DISTINCT ...)`；
- `MIN(a) + MAX(b)`；
- `COUNT(*) + MIN/MAX` 首段先不支持；
- `SUM(a) + SUM(a)` 首段先不支持。

fallback 必须发生在 worker started 之前。

## V2-12C-2 编码任务书

### 任务名称

V2-12C-2 Multi Aggregate First Implementation。

### 任务目标

实现 DOP2/DOP4 GROUP BY partial aggregate 的两个 aggregate result path：

- `COUNT(*) + SUM(val)`；
- `COUNT(nullable_val) + SUM(nullable_val)`；
- `MIN(val) + MAX(val)`。

### 允许修改

- `sql/parallel_query/pq_group_aggregate_iterator.cc`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/sql_parallel.h`
- `mysql-test/suite/parallel_query/t/*`
- `mysql-test/suite/parallel_query/r/*`
- `Docs/pq_tasks/*`

首段不建议修改 `exchange.h` / `exchange.cc`，除非测试证明 payload v1 无法满足。

### 禁止修改

- 原生 `AggregateIterator` / `TemptableAggregateIterator`；
- InnoDB `Parallel_reader` 核心；
- handler/InnoDB scan range 逻辑；
- `pq_worker_scan_next()` disabled 状态；
- 默认 OFF 行为。

### 推荐实现步骤

1. 新增 RED MTR：`pq_groupby_dop_partial_multi_agg`。
2. 先覆盖 DOP2：
   - `COUNT(*) + SUM(val)`；
   - `COUNT(nullable_val) + SUM(nullable_val)`；
   - `MIN(val) + MAX(val)`。
3. 新增 unsupported fallback cases：
   - `COUNT(nullable_col) + SUM(other_col)`；
   - `SUM(a) + SUM(a)`；
   - `COUNT(*) + MIN(val)`。
4. 新增 descriptor helper 和多 aggregate 白名单。
5. 扩展 `can_use_dop_partial_path()` 或新增 `can_use_dop_multi_partial_path()`。
6. 扩展 `InitDopPartialPath()` 参数，支持 descriptor array。
7. leader merge 写 temp table 时遍历 descriptor 写多个 `result_field`。
8. 补 DOP4 同一测试或在同一 MTR 中切换 DOP。
9. 跑 targeted MTR。
10. 跑完整 `parallel_query` suite。

### 必须验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop_partial_multi_agg \
  pq_groupby_dop_partial_aggregates \
  pq_groupby_dop_partial_fallback \
  --parallel=1 --vardir=/tmp/pqv_v212c2_target \
  --tmpdir=/tmp/pqt_v212c2_target
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_v212c2_full \
  --tmpdir=/tmp/pqt_v212c2_full
```

### 完成标准

- DOP2/DOP4 multi aggregate positive cases 正确；
- unsupported multi aggregate cases serial fallback；
- `Parallel_groupby_dop_partial_selected` 只在 supported shape 增长；
- worker error / external KILL 既有测试仍通过；
- 完整 `parallel_query` suite 通过；
- 提交只包含 V2-12C-2 相关源码、MTR、任务文档。

## 后续不在 V2-12C-2 做

- payload v2；
- 多 value field descriptor；
- 三个及以上 aggregate；
- ORDER BY same group key；
- HAVING；
- AVG；
- DECIMAL/DOUBLE/string/date；
- nullable group key；
- multiple group key。

## 验收

本设计任务完成条件：

- 本文档提交；
- README 总看板更新；
- 不修改源码；
- V2-12C-2 可以直接按本文档进入编码。
