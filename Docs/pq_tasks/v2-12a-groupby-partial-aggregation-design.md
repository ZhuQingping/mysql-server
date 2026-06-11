# V2-12A GROUP BY Partial Aggregation Design

## 结论

GROUP BY partial aggregation 应使用 worker partial aggregate state + leader merge 架构。

不要把 worker raw base row stream 直接交给上层 `AggregateIterator` 作为目标架构。当前 `Exchange_nosort` 不保证同组相邻，raw row 方案只能作为调试或过渡路径，无法提供真正的 GROUP BY 并行收益。

## 当前边界

当前 threaded row stream：

- worker 发送 fixed `record[0]` row image；
- leader 在 `PQTableScanIterator::Read()` 中通过 `Exchange_nosort::materialize_next_record_image_status()` 拷回 leader table record；
- 上层 MySQL iterator 消费完整 base row；
- implicit aggregate 可由上层原生聚合自然消费；
- 显式 GROUP BY 当前仍在 eligibility 阶段 fallback。

关键文件：

- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/exchange.*`
- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/pq_optimizer.*`
- `sql/iterators/composite_iterators.*`

## 推荐架构

新增 GROUP BY 专用 PQ 聚合路径，例如：

- `PQGroupAggregateIterator`
- 或 `PQPartialAggregateIterator`

职责边界：

- `PQTableScanIterator` 继续负责 raw row stream；
- group aggregate iterator 替代 scan + aggregate 子树；
- worker 端扫描 assigned range，在 worker-local group hash table 中聚合；
- worker EOF 时发送 `PARTIAL_GROUP` 消息；
- leader 收集所有 worker partial groups，merge 到 leader-local group table；
- leader 收到所有 FINISH 后逐行输出最终 group rows。

第一阶段支持范围：

- GROUP BY key 必须是同一 base table 的直接字段；
- aggregate argument 必须是直接字段或 `COUNT(*)`；
- 不引入 worker-side Item/JOIN clone；
- 默认 experimental gate OFF。

## Aggregate 函数边界

支持：

- `COUNT(*)`
- `COUNT(col)`，忽略 NULL
- `SUM(int/decimal/double)`
- `AVG(int/decimal/double)`，partial state 为 `sum + non_null_count`
- `MIN/MAX`，覆盖数值、日期时间、字符串字段

Fallback：

- `COUNT(DISTINCT)` / `SUM(DISTINCT)` / `AVG(DISTINCT)`
- `GROUP_CONCAT`
- JSON aggregate
- bit aggregate、variance/stddev、UDF aggregate
- window function
- `ROLLUP`
- `HAVING` 第一阶段继续 fallback
- group key 或 aggregate argument 含表达式、子查询、非确定函数
- BLOB/TEXT group key 或 aggregate argument
- ORDER BY / secondary index / ICP / partition table

## Typed Partial State

现有 `PQ_partial_agg` 的 double-based state 不适合作为生产语义。

需要 typed state：

- integer：区分 signed/unsigned，遵守 MySQL 溢出规则；
- decimal：使用 `my_decimal` 或 packed decimal；
- double：保留 double 语义；
- AVG：保存 `sum + count`；
- MIN/MAX string：按字段 collation 比较，不能直接 byte compare；
- nullable field：显式记录 NULL bitmap。

## MQ Protocol

扩展 typed message header，不破坏现有 `ROW` / `FINISH` / `ERROR` / `ABORT`：

- 新增 `PARTIAL_GROUP`；
- payload 包含：
  - group key count；
  - encoded group key fields；
  - aggregate count；
  - typed aggregate states；
- plan descriptor 固定字段类型、NULL bitmap、charset/collation，不在每条消息重复完整元数据；
- decode 失败视为 fatal worker error。

## Memory

- worker-local group table 必须受 `parallel_memory_limit` 或后续 per-query PQ memory tracker 约束；
- 第一版超限直接 OOM/fatal，不做 worker spill；
- leader merge table 可优先复用 MySQL tmp table spill 能力；
- worker spill 单独后续阶段；
- worker 启动前 OOM 可 fallback，`mark_pq_started()` 后只能 error。

## Error And Kill

- worker error：发送 `ERROR`，leader abort 其他 worker 并返回错误；
- leader kill：沿用 kill propagation，并确保 worker THD killed 与 MQ detach 唤醒；
- empty worker：必须发送 `FINISH`；
- leader 只有收到所有 worker FINISH 后才能输出最终结果；
- partial decode/merge 失败不允许继续输出部分结果。

## EXPLAIN And Fallback

Eligible EXPLAIN 建议显示：

```text
Parallel group aggregate, dop=N, partial=worker_hash, merge=leader_hash
```

新增或细分 fallback reason：

- `GROUP_BY_UNSUPPORTED_EXPR`
- `GROUP_BY_UNSUPPORTED_AGGREGATE`
- `GROUP_BY_HAVING`
- `GROUP_BY_ROLLUP`
- `GROUP_BY_DISTINCT_AGG`
- `GROUP_BY_MEMORY_SPILL_UNSUPPORTED`
- `GROUP_BY_BLOB_TEXT_UNSUPPORTED`

## 实现拆分

### V2-12A-1 Gate And Diagnostics

只改 eligibility / EXPLAIN / status。

- 显式 GROUP BY 仍 fallback；
- fallback reason 从笼统 `HAS_GROUP_BY` 细分；
- 新增 MTR 验证 fallback reason。

状态：Completed。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_diagnostics pq_not_support pq_error_paths \
  --parallel=1 --vardir=/tmp/pqv_group_diag4 --tmpdir=/tmp/pqt_group_diag4
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full18 --tmpdir=/tmp/pqt_full18
```

结果：完整 `parallel_query` suite 通过，共 48 项。

### V2-12A-2 Wire Protocol Smoke

- 扩展 MQ `PARTIAL_GROUP`；
- synthetic worker 发送 partial group；
- leader decode / merge smoke；
- 不接真实 execution。

状态：Completed。

实现：

- 新增 `MQMessageType::PARTIAL_GROUP`；
- `Exchange_nosort::read_mq_message()` 可原样返回 `PARTIAL_GROUP`；
- 新增 synthetic partial group smoke，不执行 merge，不接 SQL execution；
- 新增 `Parallel_exchange_partial_group_smoke_rows` / `Parallel_exchange_partial_group_smoke_finishes`；
- 新增 `pq_groupby_partial_group_smoke` MTR。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_partial_group_smoke pq_exchange_rows_dop1 \
  --parallel=1 --vardir=/tmp/pqv_partial_group2 \
  --tmpdir=/tmp/pqt_partial_group2
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full20 --tmpdir=/tmp/pqt_full20
```

结果：完整 `parallel_query` suite 通过，共 49 项。

### V2-12A-3 DOP1 Partial Group Execution

- 支持直接字段 group key；
- 支持 `COUNT/SUM/MIN/MAX` 基础类型；
- 默认 experimental gate OFF；
- DOP=1 结果与串行一致。

### V2-12A-4 DOP2/DOP4 Partial Merge

- 接入 range-aware worker；
- 多 worker 同 key merge；
- 多 worker 不同 key merge；
- empty worker FINISH。

### V2-12A-5 Type Semantics

- DECIMAL / DOUBLE；
- NULL-heavy；
- string collation；
- DATE/TIME；
- AVG 精确语义。

### V2-12A-6 Hardening

- worker ERROR；
- external `KILL QUERY`；
- memory limit exceeded；
- large cardinality；
- counters；
- EXPLAIN ANALYZE 可观测性。

## MTR Matrix

正向：

- `GROUP BY int_key COUNT(*)`
- `GROUP BY int_key COUNT(nullable_col)`
- `SUM/AVG` int、decimal、double
- `MIN/MAX` int、decimal、date、varchar collation
- NULL group key
- WHERE + GROUP BY
- DOP=1、DOP=2、DOP=4
- empty table、empty WHERE result
- high duplicate groups、high cardinality groups

负向 fallback：

- `COUNT(DISTINCT)`
- `SUM(DISTINCT)` / `AVG(DISTINCT)`
- `GROUP_CONCAT`
- `HAVING`
- `ROLLUP`
- `ORDER BY`
- `GROUP BY a+1`
- `SUM(a+b)`
- BLOB/TEXT group key 或 aggregate argument
- partition / secondary index / ICP

错误与资源：

- forced worker ERROR
- external `KILL QUERY`
- memory limit exceeded
- one worker empty range
- all workers empty range
- counters：executed / fallback / rows / workers / ranges / partial groups sent / partial groups merged

## 文件范围

预计实现会触碰：

- `sql/parallel_query/pq_optimizer.*`
- `sql/parallel_query/pq_aggregate.*`
- `sql/parallel_query/exchange.*`
- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/parallel_query/pq_iterator.*`
- optional new `sql/parallel_query/pq_group_aggregate_iterator.*`
- `sql/sql_executor.cc`
- `sql/join_optimizer/access_path.cc`
- `sql/opt_explain.cc`
- `mysql-test/suite/parallel_query/t/`
- `mysql-test/suite/parallel_query/r/`

## 当前状态

状态：V2-12A-2 Completed。

下一步建议：

1. 实施 V2-12A-3 DOP1 Partial Group Execution；
2. 继续保持 GROUP BY partial aggregation 实现串行推进；
3. ORDER BY / ICP / partition 继续设计先行，不与 GROUP BY 实现并行修改同一路径。
