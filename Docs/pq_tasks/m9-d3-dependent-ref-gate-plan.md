# M9-D3 User-visible Dependent Ref Gate Plan

## 状态

Design convergence taskbook created by Codex Orchestrator after M9-D1/D2.
No source code changes in this step.

Base commits:

```text
db5645cfd1d Add PQ M9D dependent ref negative guard
50c1be56dad Add PQ M9D ref key dispatch smoke
```

## 背景

M9-C2 已打开 single-table constant covering secondary `REF` 的用户可见
leader-local row sink bridge。M9-D1/D2 已补上 dependent ref 的负向守卫和
debug-only multi-key smoke。

D3 是第一次考虑 multi-table dependent ref 用户可见路径，不能直接把 C2
iterator 套到 join child 上，原因：

- dependent ref 的 `Index_lookup::key_buff` 会随 outer row 变化；
- inner ref iterator 会被 nested-loop 多次 `Init()` / `Read()`；
- fallback 只能在当前 outer probe 未对外返回 row 前发生；
- 返回行顺序必须与 serial nested-loop 完全一致；
- `Parallel_queries_executed` 必须是 per-statement 语义，不能每个 outer probe
  增长一次；
- 商用实现走 worker-side `PQRefIterator` / `pq_ref_build_ranges()` /
  `ha_pq_next()`，当前分支还没有 worker-side dependent ref clone/refix 合同。

## 设计决策

D3 不直接平移商用 worker-side dependent ref。先做一个更窄的
leader-local dependent ref bridge，并且只在 review accepted 后进入编码。

原因：

- 当前 C2 handler producer 已能在 leader handler 上按一个 copied ref key
  产生 covering secondary ref rows；
- D2 已证明多个 copied key 可以顺序调用同一 fail-closed producer；
- worker-side commercial route 需要 clone item/field refix、per-ref-key worker
  ctx dispatch 和 MQ row ownership，适合拆到 M9-D4 或后续商用路径；
- D3 的目标是验证 SQL iterator ownership 和 per-probe fallback 边界，不扩大到
  worker/MQ。

## D3 最小目标

仅允许极窄形态：

- two-table nested-loop join；
- inner child 是 InnoDB covering secondary `REF`；
- inner ref 是 dependent ref，`depend_map != 0`；
- no ICP；
- no worker / no `Query_result_mq`；
- no `REF_OR_NULL` / `EQ_REF` / `PUSHED_JOIN_REF`；
- no reverse / MVI / partition / generated/nullable/hidden fields，即只允许
  与 `RefIterator<false>` 对齐；
- projection/read_set 完全被 inner secondary key 覆盖；
- per-probe estimated rows <= 64；
- statement-level total buffered rows 需要有小上限，默认 1024。

## 禁止范围

D3 编码阶段禁止：

- 修改 commercial `PQRefIterator` skeleton 为用户可见 worker path；
- 接入 `PQ_REF_SCAN` access path；
- 修改 worker open / worker next / MQ；
- 修改 clone item / field refix；
- 对 unsupported probe 在已经返回 row 后重启 serial；
- 让 `Parallel_queries_executed` 按 outer probe 增长；
- 保存 `Index_lookup::key_buff` 或 outer row buffer 的临时指针。

## 建议拆分

### M9-D3a: Dependent Ref Gate Design Review

目标：

- review 本文；
- 明确 D3 是否先做 leader-local bridge；
- 明确编码阶段的文件边界和 fallback 边界。

输出：

- Design Review Agent `ACCEPT` 后才能编码；
- 若 review 认为必须等待 worker-side commercial route，则 D3 停止，转入
  M9-D4 worker-side taskbook。

### M9-D3b: Iterator Scaffold Only

目标：

- 新增一个 dependent ref iterator scaffold 或扩展现有 C2 factory 的
  dependent-ref guarded branch；
- 默认返回 `nullptr` 或 serial fallback；
- 不调用 handler producer；
- 只增加 compile-level helper 和 MTR negative guard。

验收：

- dependent ref D1 negative guard 仍 0；
- C2 constant ref 不回退；
- full `parallel_query` suite 通过。

### M9-D3c: Single-probe Buffering Smoke

目标：

- 在 debug-only hook 下，对一个 dependent ref probe 调用 C2 handler producer；
- 不替换用户可见 join child；
- 验证 copied key lifetime、row count、unsupported fallback-before-row。

验收：

- repeated/missing outer key MTR 仍 serial；
- debug smoke 可观测 key=20 -> 2、key=30 -> 1、key=999 -> 0；
- `Parallel_queries_executed` 不增长。

### M9-D3d: User-visible Leader-local Gate

目标：

- 仅在 reviewed narrow join shape 下替换 inner `REF` child；
- 每次 inner iterator `Read()` 的 first-record path 必须对齐
  `RefIterator<false>`：先执行 `impossible_null_ref()`，再执行
  `construct_lookup()`，再 deep-copy `key_buff`；
- 调用 leader-local C2 handler producer；
- 把当前 probe 的 rows 缓存在 SQL-owned buffer；
- `Read()` 按 buffer 顺序返回，空 probe 返回 EOF；
- unsupported 且当前 probe 未返回任何 row 时回退 serial inner ref；
- 当前 probe 已返回 row 后的错误向上返回，禁止重启 serial probe。

计数合同：

- `Parallel_queries_executed` 每 statement 最多 +1；
- `Parallel_secondary_rows_produced` 按真实返回 inner row 数增长；
- `Parallel_workers_launched` 保持 0；
- `Parallel_ranges_built` / `Parallel_ranges_dispatched` 保持 0；
- D3d 前必须先有 ref-specific observability，至少能区分 probe attempts、
  empty probes、fallback-before-row probes 和 produced rows；
- probe 级观测禁止复用 `Parallel_queries_executed`。

验收：

- outer keys `20,20,999,30,20` 返回 inner multiplicity `2,2,0,1,2`；
- 结果顺序与 serial `STRAIGHT_JOIN` 一致；
- D1 negative guard 改为 positive guard 时，必须同步更新 counter 预期；
- C2 constant ref regression 通过；
- non-covering / ICP / nullable / reverse / partition fallback；
- full `parallel_query` suite 通过。

## 主要风险

- `NestedLoopIterator` 对 inner child `Init()` / `Read()` 的调用顺序；
- `construct_lookup()` / `impossible_null_ref()` 顺序和错误语义是否与
  `RefIterator<false>` 完全一致；
- fallback serial inner iterator 的生命周期和已经可见 row 的边界；
- statement-level counter 只计一次的实现位置；
- outer row buffer 变化后 copied key 是否仍独立；
- row order、duplicate outer key multiplicity、empty probe 的 EOF 语义；
- root-only C2 gate 与 child dependent ref gate 的互斥关系。

## Review Questions

- D3 是否接受 leader-local bridge 作为商用实现平移前的中间路径？
- D3b/D3c/D3d 是否还需要进一步拆分？
- statement-level `Parallel_queries_executed` 应放在 iterator 首次成功 probe
  还是 iterator Init 阶段？
- fallback serial inner ref 是否需要独立 wrapper，还是复用
  `RefIterator<false>` 足够？
- ref-specific counters 作为 D3d 前置，建议在 D3b/D3c 期间落地并由 MTR
  验证。

## Review Result

Design Review Agent returned `ACCEPT`.

Accepted points:

- leader-local bridge 是 D3 的合理中间阶段，风险低于直接打开
  worker/MQ commercial route；
- D3b/D3c/D3d 拆分足够小；
- per-probe fallback 边界和 per-statement/per-row counter 语义清楚；
- D3d 前应补 ref-specific counters，用于观测 probe attempts、empty probes、
  fallback-before-row probes 和 produced rows。
