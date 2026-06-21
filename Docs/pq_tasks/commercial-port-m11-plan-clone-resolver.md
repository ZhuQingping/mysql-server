# M11-A Plan Clone / Resolver Contract Taskbook

## 状态

M11-A0 design-only completed / Docs-Design Review accepted。
M11-A1 Item base contract compile-only skeleton completed /
Code-Docs-Test Review accepted。
M11-A2 Query_block / JOIN clone-link skeleton completed /
Code-Docs-Test Review accepted。

## 目标

为商用 plan clone / resolver 平移建立合同图和任务边界。M11-A0 不改源码，
不打开任何新执行能力。后续 A1-A5 才逐步引入 compile-only skeleton、
preflight probe 和 MTR smoke。

## 当前事实

当前仓：

- `sql/parallel_query/pq_clone.cc` 中 `pq_make_join()` 返回 `nullptr`；
- `pq_dup_tabs()`、`pq_replace_base_item()`、resolver/base-ref helpers
  仍 fail-closed；
- `pq_clone_activation_probe()` 只记录
  `Parallel_clone_probe_attempts/fallback/success/unsupported`；
- `pq_clone_diagnostics` 已验证 clone probe 不启动 worker；
- `Parallel_clone_probe_success` 当前应保持 0。

商用仓：

- `pq_clone.cc`、`pq_clone_item.cc`、`pq_refix_fields_item.cc` 等包含
  大量完整 clone / refix / restore 逻辑；
- 商用 `Item` hierarchy 有 `pq_clone()`、`pq_copy_from()`、
  `refix_fields()`、`pq_restore()`、`replace_with_base_item()` 等契约；
- 商用 `Query_block` / `JOIN` 有 clone link、backup/restore、
  qep/tab 状态保存等完整生命周期；
- 商用 `pq_make_join()` 假设上述核心类契约已经存在。

结论：不能直接整体搬商用 `pq_make_join()`。M11-A 必须先补 contract
skeleton，并保持 worker-start 前 fail-closed。

## M11-A0 设计输出

本任务书即 M11-A0 的设计输出，必须被 review 后提交。后续编码任务按
本文件拆分执行。

## 后续任务拆分

### M11-A1: Item Base Contract Compile-only Skeleton

目标：

- 在必要的 `Item` base class 中增加最小 PQ clone virtual/default
  contract；
- 默认实现必须 fail-closed，不实现 subclass 大量 clone；
- 不从主路径调用 `pq_make_join()`；
- 不改变现有 SQL 执行行为。

允许文件：

- `sql/item*.h` narrow declarations；
- `sql/parallel_query/pq_clone_item.cc`；
- `Docs/pq_tasks/commercial-port-m11-plan-clone-resolver.md`。

禁止：

- subclass 大面积 override；
- resolver/fix_fields 行为改变；
- `pq_make_join()` positive path。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
```

Implementation notes:

- add only the `Item::pq_clone()` / `Item::pq_copy_from()` base virtual
  declarations needed by later commercial Item clone migration；
- default `Item::pq_clone()` returns `nullptr`；
- default `Item::pq_copy_from()` returns `true`；
- do not add commercial `origin_item` / `cloned_origin_item` state fields yet；
- do not add subclass overrides；
- do not call these helpers from `pq_make_join()` or any optimizer/runtime path。

Status: coding/validation completed / Code-Docs-Test Review accepted。

### M11-A2: Query_block / JOIN Clone-link Skeleton

目标：

- 增加 clone link / backup / restore 的 compile-only shell；
- 保持 no-op 或 fail-closed；
- 不保存可执行 cloned JOIN，不创建 `Gather_operator`。

允许文件：

- `sql/sql_lex.h` narrow declarations；
- `sql/sql_optimizer.h` narrow declarations；
- `sql/parallel_query/pq_clone.*`；
- `Docs/pq_tasks/commercial-port-m11-plan-clone-resolver.md`。

禁止：

- `sql/sql_optimizer.cc` 大面积逻辑迁移；
- `sql/sql_select.cc`；
- AccessPath rewrite；
- worker launch。

Implementation notes:

- add only `Query_block` clone-link accessors and no-op backup/restore shell；
- add only `JOIN` clone/restore shell declarations and fail-closed/no-op
  definitions；
- `JOIN::pq_copy_from()` / `JOIN::setup_tmp_table_info()` /
  `JOIN::restore_optimized_vars()` return `true`；
- `Query_block::pq_backup()` / `Query_block::pq_restore()` /
  `JOIN::pq_restore()` are no-op；
- `pq_make_join()` remains fail-closed and returns `nullptr`；
- `pq_clone_activation_probe()` must not increment
  `Parallel_clone_probe_success` or store an executable cloned JOIN。

Status: coding/validation completed / Code-Docs-Test Review accepted。

### M11-A3: Resolver Helper Compile-only Subset

目标：

- 迁入可独立编译的 resolver/base-item helper 子集；
- helper 默认只服务 preflight，不参与真实执行；
- complex derived/subquery/order/group mapping 继续 unsupported。

允许文件：

- `sql/parallel_query/pq_resolver.*`；
- `sql/parallel_query/pq_refix_fields_item.cc`；
- `sql/parallel_query/pq_replace_base_item.cc`。

### M11-A4: Clone Contract Preflight Probe

目标：

- 新增 `pq_clone_contract_preflight()`；
- `pq_clone_activation_probe()` 可以调用 preflight；
- 只记录更细诊断，不创建 executable cloned JOIN；
- `Parallel_clone_probe_success` 不能被误用为 worker plan 可执行。

验证：

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_clone_diagnostics \
  --parallel=1 --vardir=/tmp/pqv_m11a_clone --tmpdir=/tmp/pqt_m11a_clone
```

### M11-A5: Clone Preflight MTR Smoke

目标：

- 扩展或新增 MTR；
- 验证 simple SELECT 仍 serial/fallback；
- clone preflight counters 增长；
- `Parallel_workers_launched` 不增长；
- 不声明 cloned JOIN positive execution。

## M11-A 总禁止范围

- `storage/innobase/**`；
- `sql/handler.h`；
- `sql/sql_select.cc`；
- `sql/join_optimizer/access_path.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/msg_queue.*`；
- `sql/parallel_query/exchange.*`；
- subquery / UNION / derived / semijoin / partition / secondary ICP positive
  path；
- 任何 worker-start 后 fallback 语义变化。

## 风险

- `Item::refix_fields()` 触碰 resolver/fix_fields，不可大面积一次性迁移；
- `Query_block::pq_backup()/pq_restore()` 必须完整恢复 fallback 前状态；
- `pq_make_join()` 依赖 `pq_mem_root`、table clone、QEP_TAB clone、ORDER/GROUP、
  temporary table、derived/subquery 等大量上下文；
- 在 cleanup contract 未证明前，`Parallel_clone_probe_success` 保持 0 是
  安全护栏。

## Review 要求

M11-A0 只接受 docs/design review。A1 以后每个源码子任务必须单独启动
Code-Docs-Test Review Agent，review accepted 后才提交。

## Review

Docs-Design Review Agent accepted this A0 taskbook after confirming it keeps
`pq_make_join()` fail-closed, does not create cloned JOIN, does not launch
workers, and keeps AccessPath/InnoDB out of scope.

M11-A1 Code-Docs-Test Review Agent accepted the minimal Item base contract
skeleton. The accepted scope adds only `Item::pq_clone()` /
`Item::pq_copy_from()` declarations plus fail-closed defaults, does not add
commercial `origin_item` / `cloned_origin_item` state, does not add subclass
overrides, and does not touch resolver/fix_fields/restore/`pq_make_join()` or
worker paths.

M11-A2 Code-Docs-Test Review Agent first returned REVISE because
`pq_try_clone_item` exceeded the clone-link shell scope and belongs to later
Item/resolver work. The revised diff removed that field. Re-review accepted the
remaining `Query_block` clone-link shell and `Query_block` / `JOIN` no-op or
fail-closed methods, with `pq_make_join()` still returning `nullptr` and clone
probe success still not incremented.
