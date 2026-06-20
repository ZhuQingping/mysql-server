# M9-E ICP Pushdown Plan

## 状态

Design taskbook created by Codex Orchestrator after M9-D3d.
Design Review Agent accepted the tightened M9-E0 scope.
M9-E0 coding completed by Codex Orchestrator; Code/Task Review accepted.
M9-E1a leader-local ICP contract design accepted.

M9-E 当前只进入设计与负向护栏阶段。两个只读调研 Agent 的结论一致：
当前分支不适合直接打开用户可见 ICP，更不能直接平移商用 worker-side
ICP lifecycle。第一步必须先确认 `pushed_idx_cond` 存在时，现有
secondary range/ref/dependent-ref PQ gate 继续 fail-closed，并补齐缺失的
MTR 观测。

## 背景

商用实现中的 ICP 支持不是简单复用 leader handler 上的
`pushed_idx_cond` 指针，而是一个跨 SQL clone/refix、handler、InnoDB
template、worker iterator 的完整生命周期：

- SQL 优化期 `QEP_TAB::push_index_cond()` 调用
  `make_cond_for_index()` / `idx_cond_push()` / `make_cond_remainder()`；
- worker plan 准备阶段对 `qep_tab->pq_cond` 再执行一次 ICP 拆分和下推；
- `TABLE::pq_copy()` clone `pushed_idx_cond` 并做 field refix；
- InnoDB `idx_cond_push()` 保存 `pushed_idx_cond` /
  `pushed_idx_cond_keyno`；
- `build_template(false)` 在 active index 匹配时设置
  `prebuilt->idx_cond` 与 ICP template；
- `row_search_idx_cond_check()` 会先把 ICP 字段写入
  `handler::record[0]`，再执行 `pushed_idx_cond->val_int()`；
- secondary scan 中 ICP 发生在 visibility / clustered lookup /
  MySQL record materialization 的精细边界上；
- dependent ref 的 `pq_ref_build_ranges()` 阶段还需要避免用 ICP 误判
  当前 ref key 不存在，否则后续 outer row 会丢记录。

当前分支的实际状态不同：

- `PQblockScanIterator` / `PQRefIterator` worker-side path 仍 fail-closed；
- 用户可见 secondary range/ref/dependent-ref 路径是 leader-local covering
  bridge；
- SQL iterator gate 和 InnoDB producer 都显式拒绝
  `pushed_idx_cond` / `prebuilt->idx_cond`；
- 当前 secondary producer 只允许 strict covering integer secondary
  fast-path，不支持 ICP template、non-covering clustered lookup、
  worker-side Item clone/refix。

因此 M9-E 必须拆小推进。

## 当前代码边界

当前分支已确认：

- `sql/parallel_query/pq_iterators.cc`：
  - covering secondary range gate 拒绝 `table->file->pushed_idx_cond`；
  - constant covering ref gate 拒绝 `table->file->pushed_idx_cond`；
  - dependent covering ref gate 拒绝 `table->file->pushed_idx_cond`；
  - covering read_set gate 只允许完整、安全、非 nullable 整型 keypart。
- `storage/innobase/handler/ha_innodb_pq.cc`：
  - `pq_secondary_covering_ref_produce()` 拒绝 `m_prebuilt->idx_cond`；
  - `pq_secondary_covering_range_produce()` 拒绝 `m_prebuilt->idx_cond`。
- `storage/innobase/row/row0pread_pq.cc`：
  - secondary visibility/materialization helpers 拒绝 `prebuilt->idx_cond`；
  - 用户可见 range/ref producer 只走 fast-path visibility；
  - non-covering clustered lookup materialization 不在当前用户可见 gate 内。
- upstream ICP API：
  - `make_cond_for_index()` / `make_cond_remainder()` 是
    `sql/sql_select.cc` 内部 static helper；
  - `ha_innobase::idx_cond_push()` 只保存 Item 指针和 keyno；
  - `innobase_index_cond()` 依赖 `pushed_idx_cond->val_int()` 和
    `handler::record[0]`。

## 目标拆分

### M9-E0: ICP Negative Guard And DBUG Smoke

目标：

- 保持所有用户可见 ICP query fallback 或 serial；
- 补齐 secondary range ICP 以及 adjacent ref/dependent-ref 边界负向 MTR；
- secondary range ICP-on case 的 `EXPLAIN` 必须稳定观测到
  `Using index condition`；
- constant ref / dependent ref 在当前测试表结构下不会稳定产生剩余可下推的
  index condition，E0 只把它们作为 adjacent boundary guard；
- ICP-off baseline 必须与 ICP-on query 结果一致；
- ICP negative counter window 必须证明没有进入 PQ execution；
- 可选增加 debug-only smoke，证明 producer 侧遇到 `prebuilt->idx_cond`
  时不会产出 PQ row；
- 不调用 `pushed_idx_cond->val_int()`；
- 不迁移 `make_cond_for_index()` / `make_cond_remainder()`；
- 不修改 SQL condition remainder 语义。

建议实现：

- E0 优先只改 MTR，不改源码；
- 三类负向用例必须写死 SQL 形态，避免测成无意义普通 WHERE：
  - secondary range ICP：
    - 使用能稳定产生 `pushed_idx_cond` 的 secondary index/query；
    - `EXPLAIN` 必须显示 `Using index condition`；
    - 同形态 ICP-off baseline 结果必须一致。
  - constant ref boundary：
    - 必须覆盖当前 M9-C2 constant covering ref 正例路径会被
      ICP/secondary WHERE 形态干扰时仍不会误启 PQ；
    - 当前 `k = const` 完全消耗 ref key 后，`pq_ref_icp_t1` 没有稳定
      的剩余 pushed index condition；不要把无 ICP 的 EXPLAIN 写成 ICP；
    - 计数窗口证明 `Parallel_queries_executed = 0` 且
      `Parallel_secondary_rows_produced = 0`。
  - dependent ref boundary：
    - 必须覆盖当前 M9-D3d dependent ref gate 会被
      adjacent ICP/secondary WHERE 形态干扰时仍不会误启 PQ；
    - 建议使用 two-table `STRAIGHT_JOIN`，inner table
      `FORCE INDEX(k_idx)`，join on `inner.k = outer.k` 并加
      `inner.v > 150`；
    - 当前 dependent ref key 完全消耗后没有稳定 pushed index condition；
      不要求 inner table `Using index condition`；
    - serial result 必须保留 repeated/missing outer-key multiplicity。
- MTR 增加独立 counter window：
  - ICP on/off 结果一致性；
  - `Parallel_queries_executed` 和 `Parallel_secondary_rows_produced`
    在 ICP 负向窗口内不增长；
  - `Parallel_workers_launched`、`Parallel_ranges_built`、
    `Parallel_ranges_dispatched` 在 ICP 负向窗口内不增长。
- 可选 debug-only smoke：
  - 只能使用现有 producer/guard；
  - 不新增 handler virtual API；
  - 不改任何 header；
  - 不改变 runtime gate；
  - 用 DBUG hook 在 producer safe window 内证明 `prebuilt->idx_cond`
    gate 返回 `HA_ERR_UNSUPPORTED`；
  - 只计 unsupported/fallback 或 materialized smoke counter；
  - 不产生用户可见 PQ row。

验收：

- build 通过；
- `pq_commercial_ref_icp` record/replay 通过；
- `pq_stats` 如有新增 counter 需 record/replay；
- 完整 `parallel_query` suite 通过；
- Completion Report 必须列出 ICP negative window 的实际 delta：
  - `executed = 0`；
  - `secondary_rows_produced = 0`；
  - `workers = 0`；
  - `ranges_built = 0`；
  - `ranges_dispatched = 0`；
  - secondary range ICP-on `EXPLAIN` 确认包含 `Using index condition`；
  - constant ref / dependent ref EXPLAIN 未稳定产生 ICP，作为 adjacent
    boundary guard 保留；
- Code/Task Review Agent 返回 `ACCEPT`。

### M9-E1: Leader-local Covering Secondary Range ICP

目标：

- 只支持当前 B3d 的 strict covering integer secondary forward range；
- 只允许 index-only ICP condition；
- 保持 no worker / no MQ；
- 在 producer 侧安全评估 ICP 前，必须证明 template/record[0] lifetime；
- 对 unsupported 在 visible row 前 serial fallback；
- visible row 后错误不 serial restart。

前置条件：

- 明确 ICP Item 是否复用 leader handler 上的 `pushed_idx_cond`，还是需要
  clone/refixed copy；
- 明确 `row_search_idx_cond_check()` 是否可对 leader-local producer 安全
  暴露，或需要窄 wrapper；
- 明确 `make_cond_remainder()` 已由 upstream `QEP_TAB::push_index_cond()`
  处理，PQ 不重复切条件。

禁止：

- 不支持 non-covering；
- 不支持 clustered lookup；
- 不支持 dependent ref；
- 不支持 worker-side clone/refix。

#### M9-E1a: Leader-local ICP Contract Design

状态：

- Design-only；不改源码；
- 两个只读 Explorer Agent 均建议 E1 先做 contract/blocking design，
  不直接编码；
- Design Review Agent 返回 `ACCEPT`；
- 当前结论：现有 leader-local covering secondary range producer 不能安全地
  直接调用 `pushed_idx_cond->val_int()`，也不能仅删除
  `pushed_idx_cond` / `prebuilt->idx_cond` guard。

已确认事实：

- SQL 层当前 secondary range gate 在
  `TryCreatePQSecondaryCoveringRangeIterator()` 中拒绝
  `table->file->pushed_idx_cond != nullptr`；
- constant ref gate 和 dependent ref scaffold 也拒绝 pushed ICP；
- `ha_innobase::pq_secondary_covering_range_produce()` 拒绝
  `m_prebuilt->idx_cond`；
- `InnoDB_pq_scan_ctx::produce_secondary_range_for_user_gate()` 拒绝
  `prebuilt->idx_cond`；
- 当前 producer 用 `row_sel_store_mysql_rec()` 写入
  `m_prebuilt->m_mysql_table->record[0]`，SQL sink 再 deep-copy 到
  iterator 内部 `m_rows`，`Read()` 时再拷回 `table()->record[0]`；
- serial InnoDB ICP 的正常路径是 `row_search_idx_cond_check()`：
  先按 ICP template 把字段写入 MySQL record，再调用
  `innobase_index_cond()`，后者依赖 handler 上的
  `pushed_idx_cond` / `pushed_idx_cond_keyno` 并调用
  `pushed_idx_cond->val_int()`；
- 当前 PQ producer 虽然会 `build_template(false)` 并 restore prebuilt
  template state，但现有安全条件显式要求 `idx_cond == false`，尚未证明
  ICP template ordering、`idx_cond_n_cols`、end-range `ICP_OUT_OF_RANGE`、
  active key 与 pushed key 一致性。

商用实现对照：

- 商用真实 PQ ICP 是 worker-context lifecycle：
  `QEP_TAB::push_index_cond()` 保存 PQ condition，worker setup 重新
  `make_cond_for_index()` / `make_cond_remainder()`，再对 worker handler
  `idx_cond_push()`；
- commercial `TABLE::pq_copy()` / clone path 会 deep-copy 并 refix
  `pq_cond` / `pushed_idx_cond`；
- InnoDB worker read path 通过 `row_search_idx_cond_check()` 评估 ICP；
- ref range boundary build 阶段不能用 ICP 判断 key 是否存在，ICP 必须在
  worker/row read 阶段执行。

E1a 设计结论：

- 不建议直接编码 E1 用户可见 ICP；
- 下一步应先定义 `M9-E1b` 的最小安全契约，若无法满足则继续保持
  E0 negative guard；
- E1b 最小允许范围只能是 leader-local、single-threaded、single-table、
  strict covering integer secondary forward range、no worker/no MQ/no clone、
  no clustered lookup、no ref/dependent ref、no outer refs、no reverse、
  no partition；
- E1b 必须使用 InnoDB ICP 等价路径或窄 wrapper，不允许裸调
  `pushed_idx_cond->val_int()`；
- E1b 必须明确处理 `ICP_NO_MATCH`、`ICP_MATCH`、`ICP_OUT_OF_RANGE`；
- E1b 必须保证 unsupported 在任何 visible row 发送前可回退 serial，
  visible row 后只能报错/中止，不能 serial restart；
- E1b 必须证明所有 handler/prebuilt/template/end-range 状态 restore
  完整。

E1b 编码前硬门槛：

- E1a 设计 review `ACCEPT`；
- 明确是否新增窄 InnoDB helper 来复用 `row_search_idx_cond_check()` 语义；
- 明确 `pushed_idx_cond_keyno == keyno` 的校验点；
- 明确 SQL 层 read_set / covering gate 是否必须包含 ICP 用到的字段；
- 明确 MTR 如何证明：
  - ICP-on `EXPLAIN` 含 `Using index condition`；
  - PQ path 执行并产出过滤后的行；
  - ICP-off serial baseline 结果一致；
  - filtered-out rows 不增长 `Parallel_secondary_rows_produced`；
  - unsupported 形态仍保持 E0 fallback。

#### Agent Task Prompt: M9-E1a Design Review

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1a Leader-local ICP Contract Design Review。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- sql/parallel_query/pq_iterators.cc
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/row/row0pread_pq.cc
- storage/innobase/row/row0sel.cc
- storage/innobase/handler/ha_innodb.cc

检视目标：
1. 判断 E1a “不直接编码，先定义 E1b 最小安全契约” 是否合理；
2. 检查当前文档列出的 SQL/InnoDB ICP blocking points 是否准确；
3. 检查 E1b 最小允许范围是否足够窄；
4. 检查是否遗漏必须在编码前确认的生命周期、record[0]、template、
   pushed key、end-range、fallback/error 语义；
5. 给出 `ACCEPT` 或 `REVISE`。

禁止：
- 不改文件；
- 不运行破坏性命令；
- 不建议直接删除 `pushed_idx_cond` / `prebuilt->idx_cond` guard。
```

Review result:

- Design Review Agent returned `ACCEPT`；
- no blocking findings；
- optional E1b follow-ups:
  - keep existing root-range/simple-query gates explicit；
  - require a narrow InnoDB helper or wrapper around serial ICP semantics；
  - assert rows filtered by ICP do not increment
    `Parallel_secondary_rows_produced`。

### M9-E2: Constant Covering Ref ICP

目标：

- 扩展 M9-C2 constant covering secondary ref；
- 支持 `k = const AND extra_index_col predicate` 这类覆盖索引 ICP；
- 保持 dependent ref、non-covering、outer-table ICP fallback。

风险：

- ref equality producer 当前按 copied key 读取全部 matching rows；
- ICP filter 后 row count / empty probe counter 需要重新定义。

### M9-E3: Dependent Ref ICP Contract

目标：

- 只在 D3d 的 narrow dependent ref gate 上考虑 ICP；
- 先迁移 commercial `pq_ref_build_ranges()` 禁用 idx_cond 的保护语义；
- 覆盖 ref key 重复、missing outer key、outer-dependent ICP 防丢行。

禁止：

- 未证明 ref range build 阶段不会误用 ICP 前，不打开用户可见 dependent
  ref ICP。

### M9-E4: Worker-side ICP Clone/Refix

目标：

- 在 `PQblockScanIterator` / `PQRefIterator` worker-side path 真正启用后，
  再迁移 commercial worker ICP lifecycle；
- 包括 `pq_cond`、`pushed_idx_cond` clone/refix、worker `idx_cond_push()`、
  remainder condition、worker handler/prebuilt template、KILL/ERROR cleanup。

前置条件：

- worker handler/prebuilt/trx/read-view 生命周期已完整；
- worker-side range/ref row stream 已用户可见；
- clone/refix 对 ICP Item 支持完整；
- MTR 覆盖 `pq_icp`、`pq_ref_build_range`、`pq_record_buffer` 类回归。

## 禁止范围

M9-E0 禁止：

- 调用 `pushed_idx_cond->val_int()`；
- 暴露 `row_search_idx_cond_check()` 给 PQ producer；
- 放开 `prebuilt->idx_cond` gate；
- 迁移 `make_cond_for_index()` / `make_cond_remainder()`；
- 修改 `QEP_TAB::push_index_cond()`；
- 修改 worker/MQ；
- 支持 non-covering、clustered lookup、nullable、CHAR/VARCHAR、BLOB、
  virtual/gcol、MVI、spatial、descending keypart、partition table；
- 打开 worker-side `PQRefIterator` 或 `PQblockScanIterator`。

M9-E1/E2/E3 编码前必须另做设计 review。

## M9-E0 Completion Report

M9-E0 completed by Codex Orchestrator.

Changed files:

- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/m9-e-icp-pushdown.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`

Implementation notes:

- 新增独立 ICP negative counter window；
- secondary range shape 使用 `SELECT * ... FORCE INDEX(k_idx)
  WHERE k BETWEEN 20 AND 40 AND v > 150`，`EXPLAIN` 稳定显示
  `Using index condition; Using where; Not parallel NON_FULL_TABLE_SCAN`；
- 同一 range shape 在 `index_condition_pushdown=off` 下结果一致；
- constant ref shape `WHERE k = 20 AND v > 150` 和 dependent ref
  `STRAIGHT_JOIN` shape 保留为 adjacent boundary guard：当前 MySQL
  优化器在这些形态中不会稳定生成 `Using index condition`，E0 不伪造
  ICP 证据；
- counter window 结果：
  - `e0_icp_executed_delta = 0`；
  - `e0_icp_workers_delta = 0`；
  - `e0_icp_ranges_built_delta = 0`；
  - `e0_icp_ranges_dispatched_delta = 0`；
  - `e0_icp_secondary_rows_produced_delta = 0`。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_read_threaded_dop2_multirange
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --parallel=1
```

Result:

- `mysqld` build passed；
- `pq_commercial_ref_icp` record/replay passed；
- first full suite run hit pre-existing/order-sensitive
  `pq_read_threaded_dop2_multirange` empty-worker-range expectation
  (`0 -> 1`)；the same test passed when rerun alone；
- second full `parallel_query` suite passed，74 tests successful。

Review:

- Code/Task Review Agent first returned `REVISE` because the embedded task
  prompt still claimed all three ref/range/dependent-ref shapes must prove
  `Using index condition`；
- stale prompt/review residue was corrected to match the implemented boundary；
- Code/Task Re-review Agent returned `ACCEPT`。

Next:

- submit M9-E0；
- M9-E1/E2/E3/E4 均需重新设计 review，不从 E0 直接放开用户可见 ICP。

## 风险点

- ICP Item 依赖 `handler::record[0]`，而当前 leader-local producer 会把
  row 写入内部 SQL-owned buffer，再由 iterator 复制给父执行器；
- InnoDB ICP template 由 `build_template(false)` 设置，当前 producer 的
  temporary prebuilt state restore 未证明能安全叠加 ICP；
- SQL 层 `make_cond_for_index()` 会设置 Item marker，
  `make_cond_remainder()` 会删除已下推条件，重复实现容易导致漏过滤或重复过滤；
- dependent ref 的 ICP 不能参与 ref range build，否则可能因某个 outer key
  的条件结果误判 range 不存在；
- `compare_key_icp(end_range)` 和 reverse/range boundary 在 PQ producer 中
  尚未建模。

## Allowed Files

M9-E0 允许修改：

- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`（仅新增 counter 时）
- `sql/parallel_query/pq_iterators.cc`（仅可选 debug-only hook；优先不改）
- `sql/parallel_query/sql_parallel.h`（仅新增 status counter 时；优先不改）
- `sql/mysqld.cc`（仅新增 status counter 时；优先不改）
- `storage/innobase/handler/ha_innodb_pq.cc`（仅可选 debug-only smoke；
  不新增 API；优先不改）
- `storage/innobase/row/row0pread_pq.cc`（仅可选 debug-only smoke；
  不新增 API；优先不改）
- `Docs/pq_tasks/m9-e-icp-pushdown.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`

## Forbidden Files

M9-E0 禁止修改：

- `sql/join_optimizer/access_path.cc`
- `sql/sql_executor.cc`
- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/handler.cc`
- `sql/handler.h`
- `sql/parallel_query/pq_iterators.h`
- `sql/parallel_query/pq_optimizer.cc`
- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0sel.cc`
- `sql/parallel_query/pq_clone*`
- `sql/parallel_query/pq_refix_fields_item.cc`
- worker/MQ/exchange files

## Validation

M9-E0 目标验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query
```

如新增 status variable：

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_stats
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_stats
```

## Agent Task Prompt: M9-E0

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M9-E0 ICP Negative Guard And DBUG Smoke。

请阅读：
- Docs/pq_tasks/README.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- Docs/pq_tasks/m9-e-icp-pushdown.md
- sql/parallel_query/pq_iterators.cc
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/row/row0pread_pq.cc
- mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test

任务目标：
1. 保持 secondary range ICP 以及 adjacent ref/dependent-ref boundary 用户查询
   走 serial 或 fallback；
2. 补齐 secondary range + ICP 的负向 MTR，并补齐 constant ref、
   dependent ref 的 adjacent boundary MTR；
3. 如需要，添加 debug-only smoke 证明 producer 遇到 prebuilt->idx_cond
   不产出 PQ row；
4. 不调用 pushed_idx_cond->val_int()；
5. 不迁移 make_cond_for_index / make_cond_remainder；
6. 不打开 worker/MQ 路径。

允许修改：
- 见 m9-e-icp-pushdown.md 的 M9-E0 Allowed Files。

禁止修改：
- 见 m9-e-icp-pushdown.md 的 M9-E0 Forbidden Files。

验证：
- cmake --build build-ninja --target mysqld -j 16
- TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
- TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
- TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query

完成后：
1. 更新 m9-e-icp-pushdown.md Completion Report；
2. 写明 changed files、实现说明、测试结果、风险点；
3. Completion Report 必须列出：
   - secondary range ICP-on EXPLAIN 是否包含 `Using index condition`；
   - constant ref / dependent ref 是否仅作为 adjacent boundary guard，
     且未声明真实 ICP 覆盖；
   - secondary range 的 ICP-off baseline 与 ICP-on query 是否结果一致；
   - ICP negative window 的 `executed=0`、
     `secondary_rows_produced=0`、`workers=0`、`ranges_built=0`、
     `ranges_dispatched=0`；
   - 是否完全未改源码；若改源码，说明为什么 MTR-only 不够。
4. 不要提交。
```

## Completion Report

Design completed by Codex Orchestrator.

Review:

- First Design Review Agent returned `REVISE`；
- tightened E0 SQL shapes, `Using index condition` requirement, ICP-off
  baseline requirement, negative counter deltas, Forbidden Files, and optional
  DBUG smoke constraints；
- Design Re-review Agent returned `ACCEPT`；
- Non-blocking risks:
  - M9 taskbook still has broad M9-level allowed files, but M9-E0 scoped
    Allowed/Forbidden Files in this document must be treated as authoritative；
  - actual MTR record must confirm secondary range ICP-on EXPLAIN contains
    `Using index condition`；
  - constant ref and dependent ref must remain documented as adjacent boundary
    guards until a later design proves stable real ICP shapes；
  - E0 coding Completion Report must list the required zero deltas。
