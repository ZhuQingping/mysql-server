# M9-C Constant JT_REF Minimal Positive Path

## 状态

M9-C0 design taskbook created by Codex Orchestrator. M9-C1 debug-only ref
equality endpoint bridge completed and accepted by Review Agent.

Base commit:

```text
233e8f039df Add PQ M9B3 secondary range row production gate
```

## 背景

M9-B3 已打开一个极窄的用户可见 covering secondary range gate：

- 单表 InnoDB；
- secondary non-primary `INDEX_RANGE_SCAN`；
- forward single range；
- covering fixed non-null integer keypart；
- no ICP；
- no partition；
- no reverse / spatial / MVI / descending / nullable / varlen；
- runtime `HA_ERR_UNSUPPORTED` 回退串行；
- 不启动 worker，不走 `Query_result_mq`。

M9-C 的目标是推进 `JT_REF` / `AccessPath::REF` 最小正例。商用实现有完整
`PQRefIterator -> pq_ref_build_ranges() -> ha_pq_next()` worker 路径，但当前
分支 worker pull-row 仍显式 disabled。因此 M9-C 不能直接搬商用执行链路，
第一步必须先做常量 ref 的保守桥接设计。

## 商用实现调研结论

参考仓库：`/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore`

关键路径：

- `sql/parallel_query/pq_iterators.h` / `.cc`
  - `PQRefIterator::Init()` 调 `pq_worker_scan_init()`；
  - `PQRefIterator::Read()` 首次 `construct_lookup()`，再
    `FindKeyBufferAndMap()`，设置 `pq_ref_key` / `pq_ref_depend`，调用
    `pq_ref_build_ranges()`，之后 `ha_pq_next()` 拉行；
  - dependent ref 在 `Read()` 中按每个 outer row 重新构造 ref key。
- `sql/parallel_query/sql_parallel.cc`
  - `SetupPQTab()` 为 `JT_REF` 设置 `pq_ref_key`、`pq_ref=true`；
  - path rewrite 中 dependent ref 走 `PQ_REF_SCAN`，部分常量 lookup 走
    `PQ_BLOCK_SCAN`。
- `storage/innobase/handler/ha_innodb_pq.cc`
  - `pq_ref_build_ranges()` 用 `index_read(..., HA_READ_KEY_EXACT)` 验证
    ref key 存在；
  - 再用 `HA_READ_KEY_OR_NEXT` / `HA_READ_AFTER_KEY` 取得 start/end tuple；
  - 构造 `PQ_Config` 后 `build_ranges()`。

当前分支已有基础：

- `AccessPath::PQ_REF_SCAN` 和 `NewPQrefScanAccessPath()` skeleton；
- `PQRefIterator` skeleton，但 `Init()`/`Read()` 仍 fail-closed；
- M9-B3 的 `pq_secondary_covering_range_produce()` 可复用为常量 ref 的
  covering secondary equality range producer；
- 当前 worker pull-row / `ha_pq_next` 商用路线不应在 M9-C 最小正例中打开。

## M9-C 分阶段建议

### M9-C0: Constant Ref Gate Design

本阶段只做设计和任务拆分，不改源码。

目标：

- 定义常量 `JT_REF` 最小正例；
- 明确哪些 shape 允许进入后续 C1/C2；
- 明确 fallback 和 counter 语义；
- 明确为什么不直接迁移商用 `PQRefIterator -> ha_pq_next`。

建议最小正例：

```sql
SELECT k FROM pq_ref_icp_t1 FORCE INDEX(k_idx) WHERE k = 20;
```

第一版只支持：

- 单表；
- `AccessPath::REF` / traditional `JT_REF` 对应常量 ref；
- InnoDB；
- secondary non-primary key；
- exact equality；
- covering output；
- 固定长度、非 nullable、非 hidden、非 generated、非 functional-index 的
  integer keypart；
- no ICP；
- no partition；
- no reverse；
- no `REF_OR_NULL`；
- no `EQ_REF`；
- no dependent ref；
- no multi-table；
- no ORDER BY / GROUP BY / HAVING；
- estimated rows bounded，例如不超过 64；
- `parallel_query=ON` 且 `parallel_query_experimental_threaded_dop=ON`；
- 非 EXPLAIN；
- 非 PQ worker。

### M9-C1: Ref Equality Endpoint Bridge

Status: completed by Codex. Review Agent result: `ACCEPT`.

建议先不接 `PQRefIterator`，只实现一个内部 helper / debug smoke：

- 输入 `Index_lookup` 或已构造的 `key_range`；
- 生成等值 secondary ref key：
  - start: `HA_READ_KEY_OR_NEXT`；
  - stop condition: same key prefix no longer matches；
  - key buffer 必须 deep copy；
- 可复用 B3d materialization、visibility、template restore 和 row sink 逻辑；
- 不能直接把 ref equality 的上界按 `HA_READ_AFTER_KEY` 传给当前 B3d
  `pq_secondary_covering_range_produce()`；
- 当前 B3d producer 只接受 `HA_READ_BEFORE_KEY` 上界，并且 InnoDB drain loop
  只保存 `dtuple_t *end`，没有保存 endpoint flag；
- 如果把 same-key tuple 当作 end tuple，`end->compare(rec) <= 0` 会在第一条
  等值记录前停止，无法返回 duplicate ref rows；
- 因此 C1 必须新增或扩展一个 ref equality producer/helper：
  - 用 ref key 定位第一条 `>= key` 的 secondary record；
  - 每条记录 materialize 前先验证 key prefix 仍等于 ref key；
  - key prefix 不相等时正常 EOF；
  - duplicates 必须完整返回；
  - empty match 必须返回 0 rows 且 success；
  - last-key boundary 必须有测试；
  - endpoint flag 语义必须保留在 helper 参数中，不能丢成裸
    `dtuple_t *end`；
- 只在 debug flag 下验证 row count；
- 普通 `JT_REF` SELECT 仍串行 fallback。

验收：

- `SELECT k ... WHERE k = 20` 的 debug smoke 可见 row count 为 2；
- 覆盖 empty match；
- 覆盖 last-key match；
- `Parallel_queries_executed` 不增长；
- `Parallel_secondary_rows_produced` 不增长，除非后续决定新增 ref smoke
  专用 counter；
- non-covering / ICP / multi-table / nullable / varlen / dependent ref 负例
  仍 fail-closed。

实现结果：

- 新增 `pq_secondary_covering_ref_smoke()` handler API 和 InnoDB override；
- 新增 `InnoDB_pq_scan_ctx::materialize_secondary_ref_for_smoke()`；
- 新增 optimizer debug hook，严格由
  `DBUG_EXECUTE_IF("pq_secondary_covering_ref_smoke")` 触发；
- `JT_REF` 普通路径仍返回 `NON_FULL_TABLE_SCAN` fallback；
- MTR 覆盖 duplicate (`k=20`)、empty (`k=999`)、last-key (`k=50`) 和
  non-covering fail-closed。

验证结果：

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp pq_stats` passed；
- 关键结果：`c1_ref_materialized_smoke_delta=3`；
- C1 不打开用户可见 `JT_REF` PQ：`executed_delta=1` 和
  `secondary_rows_produced_runtime_delta=3` 仍来自既有 M9-B3d covering range
  positive path。

Review result:

- Review Agent: `ACCEPT`；
- No blocking findings；
- Residual risk: C1 仍只证明 fast-path secondary visibility；deleted-row /
  full MVCC 行为、ref-specific smoke counter 留待后续阶段。

### M9-C2: User-visible Constant Covering Ref Gate

在 C1 review 通过后，再接用户可见路径：

- 优先在 `AccessPath::REF` factory 增加 guarded factory；
- 或新增最小 `PQRefIterator` 但内部仍复用 C1 ref equality producer；
- handler 返回 `HA_ERR_UNSUPPORTED` 时必须回退串行 `RefIterator`；
- 成功路径才设置 `PQ_execution_state::EXECUTED`；
- 成功路径增长 `Parallel_queries_executed` 和
  `Parallel_secondary_rows_produced`；
- 不启动 worker，不写 `Query_result_mq`。

验收：

- `SELECT k FROM pq_ref_icp_t1 FORCE INDEX(k_idx) WHERE k = 20` 返回
  `20, 20`；
- runtime counter delta：
  - `executed_delta = 1`；
  - `secondary_rows_produced_runtime_delta = 2`；
  - `workers_delta = 0`；
  - `ranges_built_delta = 0`；
  - `ranges_dispatched_delta = 0`；
- empty ref key 正例或负例必须有测试；
- unsafe keypart / non-covering / ICP / multi-table / dependent ref 不增加
  executed/produced。

## 禁止范围

M9-C 不做：

- dependent ref；
- multi-table ref；
- `REF_OR_NULL`；
- `EQ_REF`；
- ICP；
- non-covering clustered lookup materialization；
- worker pull-row / `ha_pq_next`；
- `Query_result_mq`；
- MVI unique filter；
- reverse ref scan；
- partition table；
- nullable / varlen / string / collation sensitive key；
- generated / hidden / functional-index keypart。

## 风险点

- `Index_lookup` key buffer 生命周期必须 deep copy，不能保存临时 pointer；
- `construct_lookup()` 与 `impossible_null_ref()` 的行为必须与
  `RefIterator` 一致；
- 当前 conservative eligibility 会把 `JT_REF` 标为 `NON_FULL_TABLE_SCAN`，
  用户可见 gate 若绕过 `join->pq_eligible`，必须像 B3d 一样有自己的极窄
  shape gate；
- `AccessPath::REF` 可能出现在 multi-table plan 或 wrapper child 中，
  不能误替换 dependent child；
- C2 如果使用 `PQRefIterator`，必须保证 fallback 串行 iterator ownership
  清晰；
- C1/C2 不应复用商用 worker `pq_ref_build_ranges()` 的 stateful per-ref-key
  map，避免把 dependent ref 过早带入。

## M9-C0 Review Gate

Design Review Agent 必须确认：

- C0/C1/C2 拆分合理；
- 不直接搬商用 worker ref 路线是正确的；
- C1 先做 debug-only ref-to-range bridge 能降低风险；
- C2 的用户可见 gate 应复用 B3d 的 covering range producer，而不是打开
  full `PQRefIterator -> ha_pq_next`；
- 所有 dependent ref / ICP / non-covering / multi-table 风险均后置。

## M9-C0 Review Result

Design Review Agent Lovelace returned `ACCEPT` with one medium documentation
finding.

Finding:

- 原 C1 文档建议把 ref equality 上界设为 `HA_READ_AFTER_KEY` 后直接调用
  B3d `pq_secondary_covering_range_produce()`；
- 但当前 B3d producer 只接受 `HA_READ_BEFORE_KEY`，且 row loop 只保存
  `dtuple_t *end`，无法表达 `HA_READ_AFTER_KEY` 的 after-key 语义；
- 若用 same-key tuple 作为 end，`end->compare(rec) <= 0` 会在第一条等值记录
  前停止。

Resolution:

- C1 已改为 `Ref Equality Endpoint Bridge`；
- C1 必须新增或扩展 ref equality producer/helper；
- stop condition 改为 per-record key prefix equality check；
- C1 必须覆盖 duplicate rows、empty match 和 last-key boundary；
- C1 不直接把 `HA_READ_AFTER_KEY` 上界传入当前 B3d producer。

Accepted split:

- C1 debug-only ref equality endpoint bridge；
- C2 user-visible constant covering ref gate；
- dependent ref、ICP、non-covering、multi-table、worker/MQ 后置。

## 当前结论

Codex Orchestrator 建议：

1. 提交 M9-C0 design taskbook；
2. 进入 M9-C1 debug-only ref equality endpoint bridge；
3. C1 完成后启动 code/task Review Agent；
4. C1 review `ACCEPT` 后，再做 C2 用户可见 constant covering ref gate。
