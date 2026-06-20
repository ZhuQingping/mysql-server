# M9-D Dependent Ref / Per-ref-key Dispatch Design

## 状态

Design-only taskbook created by Codex Orchestrator. First Design Review Agent
returned `CHANGES REQUESTED`; counter, repeated-key, fallback ownership, and D2
forbidden-scope contracts were revised. Re-review returned `ACCEPT`. No source
code changes in this step.

Base commit:

```text
e65817cc693 Add PQ M9C covering ref gate
```

## 背景

M9-C 已完成常量 covering secondary `JT_REF` 的最小用户可见正例：

- 单表 InnoDB；
- root / FILTER-through-root `REF`；
- constant ref；
- covering fixed non-null integer secondary key；
- no ICP / no worker / no `Query_result_mq`；
- unsupported runtime 回退标准 `RefIterator`；
- `parallel_query` suite 74/74 通过。

M9-D 面向商用实现中的 dependent ref。商用路径不是常量 ref 的简单扩展：

- SQL iterator 会在每个 outer row 上重新 `construct_lookup()`；
- handler 侧使用 `pq_ref_depend`、`pq_ref_key`；
- InnoDB worker dispatch 依赖 per-ref-key state / `pq_key_map`；
- clone/worker 侧还依赖 item/field refix 能力；
- 当前分支 worker pull-row 仍是受控路线，secondary ref 当前是 leader-local
  row sink bridge。

因此 M9-D 必须先设计和拆分，不直接打开 dependent ref。

## 商用实现观察

参考仓库：`/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore`

关键点：

- `sql/parallel_query/pq_iterators.cc`
  - `PQRefIterator::Read()` 在 dependent ref 下每次重新构造 key；
  - 设置 `pq_ref_key.key` / `pq_ref_key.length`；
  - 设置 `pq_ref_depend=true`；
  - 调用 `pq_ref_build_ranges()`；
  - 后续由 `ha_pq_next()` / worker ctx dispatch。
- `storage/innobase/handler/ha_innodb_pq.cc`
  - `pq_ref_build_ranges()` 在 dependent ref 下维护 `pq_key_map`；
  - `pq_worker_scan_next()` 根据 `pq_ref_info` dispatch 对应 ctx；
  - ref key 变化会触发 ctx 切换。
- `sql/parallel_query/pq_clone.cc` / `pq_clone_item.cc` /
  `pq_refix_fields_item.cc`
  - dependent ref 能力依赖 clone 后 field/item 关系修正。

## M9-D 不变量

本阶段默认不打开以下能力：

- multi-table dependent ref 用户可见 PQ；
- worker-side secondary ref；
- ICP；
- non-covering secondary ref cluster lookup；
- `REF_OR_NULL` / `EQ_REF`；
- reverse / MVI / partition；
- clone item refix 正式启用。

任何实现阶段必须保持：

- unsupported -> serial fallback；
- no worker/MQ unless a later reviewed design explicitly打开；
- no partial row loss；
- no hidden change to normal `RefIterator` semantics；
- no dependency on temporary `Index_lookup::key_buff` lifetime without deep copy；
- no child/composite path replacement beyond reviewed root gate。

Dependent ref 额外不变量：

- row order 必须与 serial nested-loop ref 完全一致；
- repeated outer keys 不能被去重，例如 outer keys `20,20,999,30,20` 必须按
  outer-row 顺序产生 `2+2+0+1+2` inner rows；
- 每个 outer probe 的 ref key buffer 必须 deep copy，不能保存
  `Index_lookup::key_buff` 的临时地址；
- `construct_lookup()` 和 `impossible_null_ref()` 的顺序必须匹配
  `RefIterator`；
- per-probe unsupported fallback 只允许发生在该 probe 对外返回任何 row 之前；
- 一旦当前 probe 已经对外返回 row，后续错误必须向上报错，禁止重启 serial
  probe 造成重复行；
- `Parallel_queries_executed` 必须是 per-statement 语义，不能按 outer probe
  重复增长；
- ref probe 数、ref produced row 数如需观测，应新增 ref-specific counters，
  不能用 per-query counter 承载 per-probe 语义。

## 建议拆分

### M9-D0: Design / Contract Review

目标：

- 决定 M9-D 是否继续沿用 leader-local row sink bridge，还是转入商用 worker
  `PQRefIterator` 路线；
- 明确 dependent ref 的 stop condition 和 fallback 边界；
- 明确是否需要新增 ref-specific status counters；
- 明确 clone/item refix 是否作为前置任务。

输出：

- 本文档 review 通过；
- 后续编码阶段列表；
- 每个阶段的允许/禁止文件。

### M9-D1: Dependent Ref Negative Guard

目标：

- 对 multi-table dependent ref 增加更明确的 runtime counter guard；
- 证明当前 C2 root-only gate 不会替换 join child ref；
- 不打开新执行路径。

允许修改：

- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- 必要的 docs。

验收：

- multi-table ref SELECT 结果正确；
- `Parallel_queries_executed` / `Parallel_secondary_rows_produced` 不因
  dependent ref 增长；
- full suite 通过。

### M9-D2: Ref-key Dispatch Smoke

目标：

- 只做 debug-only smoke，验证多个 ref key 可按 key buffer deep copy 后依次
  调用 current C2 producer；
- 不接 join child；
- 不改变用户可见 execution。

禁止范围：

- 禁止修改 access-path factory；
- 禁止用户可见 iterator replacement；
- 禁止 worker / `Query_result_mq`；
- 禁止增长 `Parallel_queries_executed`；
- 禁止保存未 deep-copy 的 key buffer pointer。

允许修改：

- SQL debug smoke helper；
- existing handler ref producer only if needed；
- MTR。

验收：

- debug smoke 能证明 key=20、key=30、key=999 的 row_count 分别为 2、1、0；
- 每个 key 都使用独立 deep-copy buffer；
- 普通 dependent ref 仍 serial fallback。

### M9-D3: User-visible Dependent Ref Gate Candidate

前置条件：

- D1/D2 review accepted；
- 明确不使用 worker/MQ，或已有单独 worker/ref dispatch 设计 accepted；
- 有可回退的 serial iterator ownership。

目标：

- 仅在极窄 multi-table covering dependent ref 上尝试；
- 明确这是 leader-local nested-loop ref materialization，不是商用
  worker-side dependent `PQRefIterator` / `ha_pq_next` 路线；
- 对每个 outer row 使用 leader-local C2 ref producer；
- unsupported 只允许在当前 outer probe 尚未对外返回任何 row 时回退 serial
  nested-loop ref path；
- 当前 outer probe 已经对外返回 row 后，后续错误必须报错，禁止重新串行读取
  该 probe；
- `Parallel_queries_executed` 只能按 statement 增长一次；
- ref probe / ref row 观测应使用 ref-specific counters 或明确文档化为
  `Parallel_secondary_rows_produced` row-level 语义。

风险：

- join iterator ownership / outer row interaction；
- repeated producer cost；
- `Index_lookup::key_copy` / `construct_lookup()` error propagation；
- row order 与普通 nested loop 的一致性；
- repeated outer-key multiplicity，不能 key-level dedup；
- duplicate key 与 empty match 混合场景。

## Required Tests

- constant ref C2 regression；
- multi-table dependent ref remains serial in D1；
- debug-only multi-key dispatch in D2；
- if D3 opens user-visible path:
  - outer key 20 -> two inner rows；
  - outer key 30 -> one inner row；
  - outer key missing -> zero inner rows；
  - repeated/interleaved outer keys `20,20,999,30,20` -> inner row counts
    `2,2,0,1,2` in outer-row order；
  - non-covering / ICP / nullable / reverse / partition all fallback；
  - fallback before any row for a probe returns serial-equivalent rows；
  - error after any row for a probe is externally visible propagates instead of
    restarting serial；
  - `Parallel_queries_executed` grows once per statement, not once per outer
    probe；
  - `workers_delta=0`；
  - `ranges_built_delta=0` and `ranges_dispatched_delta=0`；
  - counters distinguish ref rows from range rows；
  - full `parallel_query` suite。

## Review Gate

Before any M9-D coding, launch a Design Review Agent to answer:

- Is leader-local dependent ref bridge acceptable, or must M9-D wait for worker
  `PQRefIterator` migration?
- Are D1/D2 enough before considering user-visible D3?
- Are there missing invariants around row order, key buffer lifetime, or
  fallback ownership?
- Is the per-statement vs per-probe counter contract precise enough?
- Should M9-D defer ICP and clone/item refix entirely to M9-E / later?

## Review Result

Design Review Agent returned `ACCEPT` after revisions.

Accepted points:

- D1 negative guard and D2 debug-only multi-key dispatch should precede any D3
  user-visible dependent ref；
- D2 forbidden scope is explicit；
- D3 is correctly described as leader-local nested-loop ref materialization,
  not commercial worker `PQRefIterator` / `ha_pq_next`；
- per-probe fallback boundary is explicit；
- `Parallel_queries_executed` is per-statement, and ref probe/row observability
  must use ref-specific counters or documented row-level semantics；
- tests now cover repeated/interleaved keys, fallback-before-row,
  error-after-row, workers/ranges staying zero, and per-statement counters.
