# V2-12C GROUP BY Shape Expansion

## 状态

Design Created。

## 目标

在 V2-12B 已完成的 DOP2/DOP4 单 group key、单 aggregate typed partial
GROUP BY 基础上，评估并拆分更复杂 GROUP BY shape 的后续扩展。

本阶段先做设计拆分和风险排序，不直接修改源码。

## 当前基础

已完成：

- DOP2/DOP4 worker partial group path；
- 单表 InnoDB clustered full scan；
- 单个 signed integer NOT NULL group key；
- 单个 aggregate result path：
  - `COUNT(*)`；
  - `COUNT(non_nullable_field)`；
  - `COUNT(nullable_field)`；
  - signed integer `SUM(field)`；
  - signed integer `MIN(field)` / `MAX(field)`；
- DOP2/DOP4 unsupported shape fallback；
- DOP2/DOP4 worker ERROR 和 external KILL cleanup；
- 默认 OFF，由 explicit experimental gates 控制；
- 完整 `parallel_query` suite 69 项通过。

关键提交：

- `57e1273f7b0` Extend PQ DOP2 partial group aggregates
- `ee436109324` Cover PQ DOP2 partial group fallback
- `64ac3f25a0d` Harden PQ DOP2 partial group cleanup
- `80c929360ac` Enable PQ DOP4 partial group aggregates
- `f0e72858529` Cover PQ DOP4 partial group cleanup
- `b710fed764b` Support PQ DOP partial count star
- `b312bc497d0` Support PQ DOP partial nullable count

## 设计原则

- 继续保持默认 OFF，必须显式开启 experimental gates；
- unsupported shape 必须 serial fallback，不允许输出错误结果；
- worker started 后不得 serial fallback，只能返回 error/kill 并完整 cleanup；
- 不引入 worker-side `JOIN` / `Item` 深拷贝；
- 不修改原生 `AggregateIterator` / `TemptableAggregateIterator`；
- 优先扩展 typed state 和 leader temp-table 写入能力，再扩大 SQL shape；
- 每个 shape 必须先有 RED MTR，再实现最小代码。

## 候选方向与优先级

| Task | Priority | Recommendation | Why |
|------|----------|----------------|-----|
| V2-12C-1 多 aggregate 设计 | P0 | 先做设计 | 当前 payload v1 只承载单 aggregate，需要先定义 payload v2 或多 payload 策略 |
| V2-12C-2 多 aggregate 实现第一段 | P0 | 设计确认后做 | `COUNT(*) + SUM(field)` 收益高，且可复用现有 count/sum state |
| V2-12C-3 `ORDER BY same group key` 设计 | P1 | 先设计，暂不编码 | 当前 optimizer 全局提前拒绝 `ORDER BY`，需要确认 ORDER/GROUP 等价和输出排序语义 |
| V2-12C-4 HAVING 设计 | P1 | 后置 | 需要 leader merge 后过滤，并处理 hidden aggregate / alias 引用 |
| V2-12C-5 多 group key | P2 | 后置 | 需要扩展 group key payload schema、merge key 比较和 temp-table 写入 |
| V2-12C-6 非整数类型 | P2 | 后置 | DECIMAL/DOUBLE/date/string/collation 语义需要 typed state 扩展 |
| V2-12C-7 secondary index / ICP / partition | P2 | 单独阶段 | 跨 optimizer、handler、InnoDB range/predicate 路径，不能混入 GROUP BY shape 扩展 |

## 推荐推进顺序

### 1. V2-12C-1 Multi Aggregate Contract

目标：

- 确认多 aggregate 的 wire protocol：
  - 方案 A：payload v2 单消息承载多个 aggregate state；
  - 方案 B：同一 group key 发送多条 payload v1，每条 payload 对应一个 aggregate slot；
  - 方案 C：worker 本地 group state 扩展为固定小数组，leader merge 后直接写 temp table。
- 推荐先采用方案 C 的最小实现，内部状态支持多 aggregate，wire 层仍保持 v1 风格或小幅扩展；
- 第一段只支持两个 aggregate：
  - `COUNT(*) + SUM(field)`；
  - `COUNT(nullable_field) + SUM(field)`；
  - `MIN(field) + MAX(field)`；
- 禁止 DISTINCT、表达式 aggregate、AVG、DECIMAL/DOUBLE、string MIN/MAX。

原因：

- 多 aggregate 是当前功能边界最自然的下一步；
- SQL 用户常见查询是 `COUNT(*) + SUM(...)`；
- 当前 worker partial state 已同时维护 count/sum/min/max，内部能力已具备；
- 风险主要在 temp-table result field mapping，而不是 InnoDB scan。

产出：

- 设计文档更新；
- 新增任务文档 `v2-12c-1-multi-aggregate-contract.md`；
- 不改源码。

### 2. V2-12C-2 Multi Aggregate First Implementation

目标：

- 支持 DOP2/DOP4：
  - `SELECT grp, COUNT(*), SUM(val) FROM t GROUP BY grp`;
  - `SELECT grp, MIN(val), MAX(val) FROM t GROUP BY grp`;
- 新增 MTR：
  - `pq_groupby_dop_partial_multi_count_sum`;
  - `pq_groupby_dop_partial_multi_min_max`;
  - unsupported multi aggregate fallback；
  - worker error / external kill 复用已有 cleanup 测试或增加组合覆盖。

允许修改：

- `sql/parallel_query/pq_group_aggregate_iterator.cc`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/exchange.*`
- `mysql-test/suite/parallel_query/t/*`
- `mysql-test/suite/parallel_query/r/*`
- `Docs/pq_tasks/*`

禁止修改：

- 原生 `AggregateIterator` / `TemptableAggregateIterator`；
- InnoDB `Parallel_reader` 核心；
- secondary index / ICP / partition 相关路径。

验收：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop_partial_multi_count_sum \
  pq_groupby_dop_partial_multi_min_max \
  pq_groupby_dop_partial_counters pq_stats \
  --parallel=1 --vardir=/tmp/pqv_groupby_multi_agg \
  --tmpdir=/tmp/pqt_groupby_multi_agg
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_groupby_multi_agg \
  --tmpdir=/tmp/pqt_full_groupby_multi_agg
```

### 3. V2-12C-3 ORDER BY Same Group Key Design

目标：

- 只评估：
  - `GROUP BY grp ORDER BY grp ASC`；
  - `GROUP BY grp ORDER BY grp DESC`；
  - `ORDER BY` 与 group key 完全相同，且无额外 order expression；
- 明确是否依赖 MySQL temp-table iterator 的 index order 输出；
- 明确是否需要 leader 写 temp table 后额外排序；
- 明确 `ORDER BY aggregate` 继续 fallback。

当前风险：

- `pq_check_query_block_eligible()` 在 GROUP BY 逻辑前全局拒绝
  `query_block->is_ordered()`；
- 直接放开 ORDER BY 可能让非 GROUP BY 查询绕过既有保护；
- 需要可靠比较 ORDER 和 GROUP 表达式等价，不能只按字段名字符串判断；
- 输出顺序必须由测试验证，不能依赖偶然 insert order。

推荐结论：

- 不作为下一个编码任务；
- 先写设计和只读调研；
- 等多 aggregate 完成后再评估。

### 4. V2-12C-4 HAVING Design

目标：

- 仅设计，不编码；
- 评估 leader merge 后应用 HAVING 的位置；
- 处理 visible aggregate、hidden aggregate、alias 引用；
- 第一段仅考虑 HAVING 引用已输出 aggregate 的简单比较。

推荐结论：

- 后置于多 aggregate 和 ORDER BY same key；
- 没有 hidden aggregate 处理前不打开。

## 当前不建议立即做的任务

- `AVG`：需要 `sum + count` result semantics，并处理 integer/decimal/double；
- DECIMAL / DOUBLE SUM：需要 MySQL 精确类型语义，不适合在当前 int64 path 上顺手扩展；
- string/date MIN/MAX：collation、temporal encoding、NULL 语义需要独立 typed state；
- nullable group key：需要 group key NULL bitmap 和 merge key 比较；
- multi key GROUP BY：需要 key tuple 编码和 temp-table field mapping；
- secondary index / ICP / partition：跨层风险高，应单独开阶段。

## 验收标准

V2-12C 设计阶段完成条件：

- 本文档提交；
- README 总看板指向 V2-12C；
- 明确下一步执行任务为 V2-12C-1 Multi Aggregate Contract；
- 不修改源码；
- 不影响当前 69 项 `parallel_query` suite 基线。

V2-12C 实现阶段通用完成条件：

- 每个 shape 都先有 RED MTR；
- `mysqld` build 通过；
- targeted GROUP BY suite 通过；
- 完整 `parallel_query` suite 通过；
- 默认 OFF 行为不变；
- unsupported shape fallback 不误增长 selected/executed counter。

## 下一步

创建并执行 `v2-12c-1-multi-aggregate-contract.md`：

- 只读调研当前 `Temp_table_param` / `Item_sum` / result field mapping；
- 给出 payload/state 方案选择；
- 生成 V2-12C-2 的编码任务书；
- 暂不改源码。
