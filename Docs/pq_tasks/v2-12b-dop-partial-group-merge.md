# V2-12B DOP Partial Group Merge

## 状态

In Progress。

## 目标

在 V2-12A DOP1 typed-state temp-table 输出的基础上，推进 DOP>1 GROUP BY partial aggregation：

- worker 端按 assigned range 扫描并构建 partial group state；
- worker 通过 MQ 发送 typed partial groups；
- leader merge worker partial state；
- leader 最终写 MySQL temp table rows，并由现有 table iterator 输出。

本阶段仍保持默认关闭，不把普通 GROUP BY DOP>1 直接暴露为默认执行路径。

## 当前基础

已完成：

- DOP1 `PQTemptableGroupAggregateIterator` 接管 temp-table aggregate iterator；
- DOP1 typed-state 输出支持：
  - `COUNT(*)` / `COUNT(non_nullable_expr)`；
  - signed integer `SUM`；
  - signed integer `MIN` / `MAX`；
- SUM 对 BIGINT/unsigned/表达式/DISTINCT 保持非 typed SUM path；
- `PARTIAL_GROUP` typed MQ message 已有 synthetic smoke；
- DOP2/DOP4 threaded row stream、range dispatch、worker error/kill/cleanup 已有测试覆盖。

## 非目标

- 不在本阶段支持复杂 GROUP BY：
  - HAVING；
  - ORDER BY；
  - ROLLUP；
  - DISTINCT aggregate；
  - 多 aggregate；
  - 字符串/decimal/double group key；
  - join / subquery / derived table；
  - secondary index / ICP / partition table；
- 不引入 worker-side `JOIN` / `Item` 深拷贝；
- 不修改原生 `AggregateIterator` / `TemptableAggregateIterator`；
- 不修改 InnoDB `Parallel_reader` 核心。

## 支持范围

第一段只支持：

- 单表 InnoDB clustered full scan；
- DOP=2 先行，DOP=4 后续打开；
- 单个 signed integer NOT NULL group key；
- 单个 aggregate：
  - `COUNT(*)` 或不可 NULL 参数的 `COUNT(expr)`；
  - signed integer `SUM`，拒绝 BIGINT/unsigned/表达式/DISTINCT；
  - signed integer `MIN` / `MAX`；
- 默认 OFF，由 experimental gate 控制；
- 所有 unsupported shape 必须 serial fallback 或继续走 DOP1 path，不得产错结果。

## 任务拆分

### V2-12B-1 Contract And Counters

目标：

- 明确 DOP partial group path 的状态机和 fallback 边界；
- 新增 counters：
  - `Parallel_groupby_dop_partial_attempts`
  - `Parallel_groupby_dop_partial_selected`
  - `Parallel_groupby_dop_partial_worker_groups`
  - `Parallel_groupby_dop_partial_merged_groups`
  - `Parallel_groupby_dop_partial_fallback`
- 新增 MTR 验证 counters 存在且默认不增长。

验收：

- `mysqld` build 通过；
- `pq_stats` 更新；
- 完整 `parallel_query` suite 通过。

### V2-12B-2 Real Partial Group Wire Smoke

目标：

- 复用现有 `PARTIAL_GROUP` message；
- 增加真实 typed state encode/decode helper，不依赖纯 synthetic fixed payload；
- 用 DOP=2 smoke 构造两个 worker partial groups，leader merge 后验证 group count/sum/min/max。

验收：

- 新增 smoke MTR；
- 不接 SQL GROUP BY 执行路径；
- 完整 suite 通过。

### V2-12B-3 Worker Local Partial State

目标：

- 在 worker producer 内增加可选 partial group accumulation 模式；
- worker 不发送 base ROW，而是在 worker EOF 时发送 partial groups；
- empty worker 仍发送 FINISH；
- worker ERROR/KILL/abort 复用现有 threaded cleanup。

验收：

- debug-only 或 experimental gate 下运行；
- 新增 worker partial group counters；
- 覆盖 empty worker、worker error、external kill。

### V2-12B-4 Leader Merge To Temp Table

目标：

- leader 收到所有 workers FINISH 后，把 merged state 写入 temp table；
- 输出路径复用 V2-12A DOP1 typed-state temp-table 写法；
- SQL result 与原生 GROUP BY 一致。

验收：

- DOP=2 `COUNT/SUM/MIN/MAX` 正向 MTR；
- 不支持 shape 的 fallback MTR；
- 完整 suite 通过。

### V2-12B-5 Gate Expansion And Hardening

目标：

- DOP=2 gate 默认 OFF，显式变量开启；
- DOP=4 在 DOP=2 稳定后打开；
- 增加 LIMIT、read-view、MDL、kill、large row count 回归。

验收：

- no-debug 下显式开启可执行；
- 默认行为不变；
- 完整 suite 通过。

## 风险点

- GROUP BY worker partial path 不能复用 raw row stream 直接交给上层 aggregate，否则没有 partial aggregation 收益；
- worker 端不能直接执行复杂 `Item` 表达式，第一版必须使用 base `Field` 读写；
- SUM 只允许 int64 安全范围，overflow 必须 fallback 或 error，不能输出截断结果；
- leader 必须等所有 workers FINISH 后才能输出结果；
- worker started 后不能 serial fallback，只能走 abort/error cleanup。

## 当前下一步

先执行 V2-12B-1：

- 补 counters；
- 补 `pq_stats`；
- 增加默认不增长的 status MTR；
- 不打开真实 DOP partial group execution。

## 调研结论

两个只读调研已完成，结论一致：

- DOP2/DOP4 threaded row stream 已存在，但显式 GROUP BY 在 optimizer eligibility 和 GROUP aggregate factory 层均被 DOP1-only gate 拦截；
- worker partial aggregation 的最小 hook 应放在 worker row producer / `PQ_row_sink` 侧构建 partial state，leader 只 merge `PARTIAL_GROUP`；
- 不建议让 leader 复用 raw row stream 后再聚合，因为这仍传输全量 row，leader 仍承担全量 group build，不是真正 partial aggregation；
- 现有 `PARTIAL_GROUP` 只是 smoke payload，没有正式 wire schema，也没有 leader 按 group key merge 多 worker partial 的 helper；
- 下一步应先正式化 payload v1，再做 leader in-memory merge smoke，之后才接 worker DOP2 debug smoke 和 SQL GROUP BY planner gate。

关键代码边界：

- DOP threaded gate：`PQTableScanIterator::should_enter_threaded_read_shadow_path()`；
- worker producer：`Gather_operator::run_worker_callback_threaded_producer()` / `pq_run_callback_limited_producer_task()`；
- InnoDB callback producer：`ha_innobase::pq_worker_scan_callback_produce()`；
- PARTIAL_GROUP smoke：`Exchange_nosort::run_partial_group_smoke()`；
- GROUP BY DOP>1 eligibility reject：`pq_check_query_block_eligible()` 中 `GROUP_BY_PARTIAL_AGG_UNSUPPORTED`；
- DOP1 GROUP BY factory：`TryCreatePQTemptableGroupAggregateIterator()`。

### V2-12B-1 Contract And Counters

状态：Completed。

实现：

- 新增 DOP partial GROUP BY counters：
  - `Parallel_groupby_dop_partial_attempts`
  - `Parallel_groupby_dop_partial_selected`
  - `Parallel_groupby_dop_partial_worker_groups`
  - `Parallel_groupby_dop_partial_merged_groups`
  - `Parallel_groupby_dop_partial_fallback`
- 新增 `pq_groupby_dop_partial_counters`，验证 counters 可见且当前 DOP1 GROUP BY / DOP2 普通 threaded aggregate 不误增长；
- `pq_stats` 更新 Parallel 状态变量数量为 42；
- 不打开任何真实 DOP partial GROUP BY 执行路径。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_dop_partial_counters pq_stats \
  --parallel=1 --vardir=/tmp/pqv_dop_partial_verify \
  --tmpdir=/tmp/pqt_dop_partial_verify
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_dop_partial_counters \
  --tmpdir=/tmp/pqt_full_dop_partial_counters
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted counters suite 通过；
- 完整 `parallel_query` suite 通过，共 59 项。

下一步：

- V2-12B-2：正式化 `PARTIAL_GROUP` payload v1 schema；
- 增加 payload encode/decode smoke、leader in-memory merge smoke、malformed payload counter；
- 继续不打开 SQL GROUP BY DOP>1 执行路径。

### V2-12B-2 Partial Group Payload V1 Smoke

状态：Completed。

实现：

- 新增正式 `PQ_partial_group_payload_v1` wire struct；
- 新增 `PQ_PARTIAL_GROUP_PAYLOAD_MAGIC` / `PQ_PARTIAL_GROUP_PAYLOAD_VERSION`；
- 新增 `PQ_partial_group_agg_kind`，当前 schema 覆盖：
  - 单 signed integer group key；
  - 单 aggregate kind；
  - `count_star` / `count_value`；
  - `sum` / `min` / `max` typed state；
- `Exchange_nosort::run_synthetic_partial_group_smoke()` 从临时局部 payload 切到 payload v1；
- smoke 现在会在 leader 侧按 group key 做 in-memory merge 校验；
- 不修改 SQL GROUP BY DOP>1 eligibility；
- 不增长 V2-12B DOP partial execution counters。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_partial_group_smoke pq_groupby_dop_partial_counters pq_stats \
  --parallel=1 --vardir=/tmp/pqv_partial_payload_v1 \
  --tmpdir=/tmp/pqt_partial_payload_v1
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted payload v1 suite 通过。

后续补充：

- 已增加 malformed payload smoke 和 payload error counter；
- 已将 payload v1 validation helper 从 smoke 内部抽出；
- 后续仍需将 merge helper 进一步抽出，供 worker partial producer 复用。

### V2-12B-2b Malformed Payload Smoke

状态：Completed。

实现：

- 新增 `Parallel_groupby_partial_payload_errors`；
- 新增 `Exchange_nosort::run_synthetic_partial_group_malformed_smoke()`；
- 抽出 `pq_validate_partial_group_payload_v1()`；
- malformed smoke 覆盖 bad magic、bad version、short payload；
- `pq_groupby_partial_group_smoke` 验证 payload error counter 增长；
- `pq_stats` 更新 Parallel 状态变量数量为 43。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_groupby_partial_group_smoke pq_stats \
  --parallel=1 --vardir=/tmp/pqv_payload_error_verify \
  --tmpdir=/tmp/pqt_payload_error_verify
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full_payload_error \
  --tmpdir=/tmp/pqt_full_payload_error
```

结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- targeted payload error suite 通过；
- 完整 `parallel_query` suite 通过，共 59 项。

下一步：

- V2-12B-3：抽出 leader in-memory merge helper；
- 先用 synthetic payload v1 做多 worker same-key/different-key merge smoke；
- 继续不打开 SQL GROUP BY DOP>1 执行路径。
