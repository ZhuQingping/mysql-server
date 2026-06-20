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

#### M9-E1b: Leader-local Covering Secondary Range ICP Coding Taskbook

状态：

- Coding taskbook drafted；Design Review accepted；
- 编码入口探测后 blocked；
- 不进入 worker/MQ/clone；
- 不支持 ref/dependent ref/non-covering；
- 当前不提交源码改动。

目标：

- 在 M9-B3d 已有 user-visible leader-local covering secondary range gate 上，
  只对 strict covering integer secondary forward range 打开真实 ICP；
- 支持形态：
  - single-table simple SELECT；
  - root `INDEX_RANGE_SCAN`；
  - InnoDB secondary index；
  - `pushed_idx_cond_keyno == range keyno`；
  - no partition / no reverse / no geometry / one range；
  - read_set 和 ICP 所需字段均可由当前 secondary index 安全覆盖；
- 保持 `Parallel_workers_launched == 0`，不打开 worker thread 或 MQ；
- 对 unsupported 形态在 visible row 发送前 serial fallback；
- 一旦已有 visible row 发送，后续错误不能 serial restart。

建议实现策略：

1. SQL gate：
   - 在 `TryCreatePQSecondaryCoveringRangeIterator()` 中拆分无 ICP 与 ICP
     两条 narrow gate；
   - 继续保留 root range、simple query、DOP experimental gate、estimated row
     cap、keypart safety、read_set safety；
   - ICP path 必须额外要求：
     - `table->file->pushed_idx_cond != nullptr`；
     - `table->file->pushed_idx_cond_keyno == param.index`；
     - no ref/dependent/ref path；
     - no outer refs / multi-table；
   - 不改变 constant ref / dependent ref 的 ICP fallback。
2. Handler/InnoDB gate：
   - 保持 `handler` public virtual contract 和
     `ha_innobase::pq_secondary_covering_range_produce()` 签名不变；
   - 不修改 `sql/handler.h` / `storage/innobase/handler/ha_innodb.h`；
   - 若需要区分 ICP，必须只在 allowed source file 内新增 private/internal
     helper，或在现有 implementation 内按现有 handler state 分支；
   - 不能删除现有 non-ICP guard；ICP 入口必须是显式窄分支；
   - non-ICP path 行为必须保持不变；
   - ICP path 必须在 `build_template(false)` 后确认：
     - `m_prebuilt->idx_cond == true`；
     - `m_prebuilt->idx_cond_n_cols > 0`；
     - `pushed_idx_cond_keyno == keyno`；
     - `m_prebuilt->index == innobase_get_index(keyno)`；
     - `m_prebuilt->need_to_access_clustered == false`；
     - materialization template 仍满足 strict covering safety。
3. InnoDB row producer：
   - 新增窄 helper，语义等价于 serial `row_search_idx_cond_check()` 的
     covering-secondary 子集；
   - 不允许直接裸调 `pushed_idx_cond->val_int()`；
   - 因 `row_sel_store_mysql_field()` 是 `row0sel.cc` 内部 static，E1b
     必须二选一：
     - 在 `row0sel.cc` 暴露一个非常窄的 PQ-only wrapper；或
     - 将必要字段转换逻辑以受控 helper 形式移动到可复用位置；
   - helper 必须返回并处理：
     - `ICP_NO_MATCH`：跳过当前 secondary record，不发送 row，不增长
       produced counter；
     - `ICP_MATCH`：继续 visibility + materialization + send；
     - `ICP_OUT_OF_RANGE`：结束当前 range；
   - helper 必须在调用 `innobase_index_cond()` 前按 ICP template 写好
     `handler/table->record[0]`。
4. State restore：
   - 保存并恢复现有 prebuilt/template 状态；
   - handler-level state 必须默认只读：
     - `pushed_idx_cond`；
     - `pushed_idx_cond_keyno`；
     - `end_range`；
   - 如任何实现触碰上述 handler-level state，必须保存/恢复并在
     Completion Report 中说明原因；
   - prebuilt-level restore 清单至少包括：
     - `m_prebuilt->idx_cond`；
     - `m_prebuilt->idx_cond_n_cols`；
     - `m_prebuilt->mysql_template`；
     - `m_prebuilt->n_template`；
     - `m_prebuilt->need_to_access_clustered`；
     - `m_prebuilt->index`；
     - `m_prebuilt->read_just_key`；
     - `m_prebuilt->m_end_range`；
   - 新增保存/恢复 `idx_cond` 相关状态时不得破坏 serial handler；
   - close read view / pcur / clust_pcur / heap cleanup 必须覆盖所有 exit。

禁止范围：

- 不迁移 commercial worker-side `pq_cond` clone/refix；
- 不修改 `QEP_TAB::push_index_cond()`；
- 不修改 `make_cond_for_index()` / `make_cond_remainder()`；
- 不打开 `PQblockScanIterator` / `PQRefIterator` worker path；
- 不支持 constant ref / dependent ref ICP；
- 不支持 non-covering clustered lookup；
- 不支持 nullable、CHAR/VARCHAR、BLOB、virtual/gcol、MVI、spatial、
  descending keypart、partition table、reverse scan、join cache/BKA。

Allowed Files:

- `sql/parallel_query/pq_iterators.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/include/row0pread_pq.h`（仅新增窄 helper 声明时）
- `storage/innobase/row/row0sel.cc`（仅新增窄 PQ ICP wrapper 或迁移
  最小字段转换 helper 时）
- `storage/innobase/include/row0sel.h`（仅暴露窄 wrapper/helper 时）
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/m9-e-icp-pushdown.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`

Forbidden Files:

- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/sql_executor.cc`
- `sql/handler.cc`
- `sql/handler.h`
- `storage/innobase/handler/ha_innodb.h`
- `sql/parallel_query/pq_clone*`
- `sql/parallel_query/pq_refix_fields_item.cc`
- worker/MQ/exchange files

测试要求：

- `pq_commercial_ref_icp`：
  - 新增 user-visible ICP range 正例；
  - ICP-on `EXPLAIN` 必须含 `Using index condition`；
  - ICP-on PQ result 与 ICP-off serial baseline 一致；
  - `Parallel_queries_executed` 增长 1；
  - `Parallel_workers_launched`、`Parallel_ranges_built`、
    `Parallel_ranges_dispatched` 不增长；
  - `Parallel_secondary_rows_produced` 只等于 ICP 过滤后的输出行数，
    被 ICP 过滤的 secondary record 不计入 produced；
  - unsupported ICP shapes 必须有独立 status window，至少覆盖
    non-covering ICP、constant ref ICP、dependent ref ICP：
    - serial result matches baseline；
    - `Parallel_queries_executed` 不增长；
    - `Parallel_secondary_rows_produced` 不增长；
    - `Parallel_workers_launched` 不增长；
    - `Parallel_ranges_built` 不增长；
    - `Parallel_ranges_dispatched` 不增长。
- `pq_stats` 仅在新增 status variable 时 record。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_not_support pq_stats
perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --parallel=1
```

Completion Report 必须列出：

- changed files；
- ICP helper/wrapper 的确切边界；
- 为什么没有裸调 `pushed_idx_cond->val_int()`；
- public handler virtual signature 是否保持不变；
- state restore 清单，必须覆盖 handler-level read-only state 与
  prebuilt-level restored state；
- user-visible ICP 正例 delta；
- fallback 形态 delta；
- build/MTR 结果；
- Code/Task Review Agent 结论。

#### Agent Task Prompt: M9-E1b Coding Taskbook Review

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1b Coding Taskbook Review。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- sql/parallel_query/pq_iterators.cc
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/row/row0pread_pq.cc
- storage/innobase/row/row0sel.cc
- storage/innobase/handler/ha_innodb.cc

检视目标：
1. 判断 E1b 任务书是否足够窄、可编码、可验证；
2. 检查 Allowed/Forbidden Files 是否合理；
3. 检查是否应暴露 row0sel.cc 窄 wrapper，或还有更安全方案；
4. 检查测试 delta 是否足以证明 ICP 过滤发生在 row send 前；
5. 给出 `ACCEPT` 或 `REVISE`。

禁止：
- 不改文件；
- 不运行破坏性命令。
```

Review result:

- First Coding Taskbook Review Agent returned `REVISE`；
- fixed public handler virtual signature scope by keeping `sql/handler.h` and
  `storage/innobase/handler/ha_innodb.h` forbidden；
- fixed unsupported fallback tests to require independent counter windows；
- fixed lifecycle checklist to include handler-level and prebuilt-level ICP
  state；
- re-review found one M9 taskbook allowed-list inconsistency；qualified the
  M9-wide allowed list as historical and made E1b-specific files authoritative；
- final re-review returned `ACCEPT`。

Coding entry result:

- Codex attempted the minimum source path locally:
  - keep public handler virtual signature unchanged；
  - expose a narrow `row0sel.cc` ICP wrapper；
  - branch inside user-visible secondary range producer；
  - handle `ICP_NO_MATCH` before `row_sink->send_row()`；
- build passed during the local attempt；
- before keeping source changes, Codex probed stable SQL shapes in
  `pq_commercial_ref_icp` and confirmed:
  - non-covering `FORCE INDEX(k_idx)` + `v > ...` prints
    `Using index condition` but violates E1b non-covering prohibition；
  - covering `FORCE INDEX(k_v_idx)` / `FORCE INDEX(k_pad_idx)` shapes print
    `Using where; Using index` and do not produce `Using index condition`；
  - narrower projections such as `SELECT k ... FORCE INDEX(k_v_idx)` also do
    not produce `Using index condition`；
- therefore E1b has no stable user-visible strict-covering ICP positive MTR in
  the current schema/optimizer behavior；
- unverified source changes were removed；
- `pq_commercial_ref_icp --record` passed after removing probes；
- result: E1b coding is blocked until the scope changes to one of:
  - allow non-covering secondary ICP with clustered lookup contract；or
  - accept a debug-only ICP smoke；or
  - find a stable upstream optimizer shape that is both covering and
    `Using index condition`。
- Blocked-result Review Agent returned `ACCEPT`。

#### M9-E1c: Non-covering Secondary Range ICP + Clustered Lookup Contract

状态：

- Design-only drafted；Design Review accepted；
- 不改源码；
- 不直接打开用户可见 non-covering ICP。

为什么需要 E1c：

- E1b strict covering ICP 无稳定 SQL 正例；
- 当前唯一稳定 `Using index condition` 正例是
  `FORCE INDEX(k_idx)` + `v > ...`，这是 non-covering secondary range；
- 当前 user-visible range producer 明确拒绝
  `prebuilt->idx_cond` 与 `prebuilt->need_to_access_clustered`；
- 当前 clustered lookup helper 只服务 visibility/delete-mark 判断，不支持
  non-covering row materialization 后继续 drain；
- 因此下一步必须先定义 non-covering secondary ICP 的 clustered lookup
  contract，而不是直接编码。

当前分支已有能力：

- `pq_row_sel_get_clust_rec_for_mysql()` wrapper 可在 active mtr 下按
  secondary record 找 clustered record；
- `validate_secondary_visibility_with_cluster_lookup()` 可作为 visibility
  fallback；
- B3a/B3c 文档已明确：当前 clustered lookup 后不允许继续 secondary
  drain，避免 latch/order/lifetime 风险；
- user-visible producer 当前只从 secondary record
  `row_sel_store_mysql_rec()` 并 `row_sink->send_row()`。

商用实现目标序：

- 对 secondary record 执行 ICP；
- `ICP_NO_MATCH`：跳过，不做 clustered lookup，不发送 row；
- `ICP_OUT_OF_RANGE`：结束 range；
- `ICP_MATCH`：继续 visibility；
- 若需要 clustered lookup，则取 visible clustered record；
- 若 extra clustered latch / old version 存在，必须明确是否可以继续当前
  secondary scan；不能复用 B3c fast-path-only drain 假设；
- materialization 对 non-covering 列必须来自 clustered record 或正确版本。

E1c 必须回答的问题：

- 是否允许 user-visible producer 在 clustered lookup 后继续扫描；
- 如果不允许，是否需要 bookmark/restart 或 one-record-at-a-time 协议；
- clustered record materialization helper 应放在哪里，是否复用
  `row_sel_store_mysql_rec()` 的 clustered record 路径；
- ICP 在 visibility 前还是后执行：商用路径是先 ICP 再 clustered lookup；
- `prebuilt->need_to_access_clustered`、`read_just_key`、
  `idx_cond_n_cols`、`m_end_range`、`mysql_template` 如何构建和 restore；
- `Parallel_secondary_rows_produced` 只统计最终发送 row，还是统计
  ICP_MATCH 后 visible clustered rows；
- visible row 后发生 unsupported/cluster lookup error 是否必须报错，
  不允许 serial restart。

建议 E1c 拆分：

1. E1c-0 design/read-only：
   - 对照 commercial `find_visible_record()` 和当前 B3a/B3c helper；
   - 写清楚 latch/mtr/clustered lookup continuation contract；
   - 不改源码。
2. E1c-1 debug-only one-record smoke：
   - 只验证 ICP -> clustered lookup -> materialize one row；
   - 不继续 drain；
   - 不 user-visible。
3. E1c-2 user-visible non-covering secondary range：
   - 只有 E1c-0/E1c-1 review 通过后再考虑；
   - 必须有 serial baseline、ICP positive MTR、KILL/error cleanup 和 fallback
     counter window。

M9-E1c Design Review Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1c Non-covering Secondary Range ICP + Clustered Lookup
Contract Review。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/m9-b3-secondary-range-row-production.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- storage/innobase/row/row0pread_pq.cc
- storage/innobase/include/row0sel.h
- storage/innobase/row/row0sel.cc
- storage/innobase/handler/ha_innodb_pq.cc
- /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/storage/innobase/row/row0pread_pq.cc

检视目标：
1. 判断 E1c 选择 non-covering ICP + clustered lookup contract 是否是合理下一步；
2. 检查是否仍应 design-only，不进入编码；
3. 检查当前问题清单是否覆盖 latch/mtr、clustered lookup continuation、
   materialization、ICP ordering、counter semantics、fallback/error；
4. 给出 `ACCEPT` 或 `REVISE`。

禁止：
- 不改文件；
- 不运行破坏性命令。
```

Review result:

- Design Review Agent returned `ACCEPT`；
- no blocking findings；
- confirmed E1c is the right next design step after E1b blocked；
- confirmed current branch has clustered lookup visibility plumbing but lacks
  non-covering materialization / drain continuation contract；
- confirmed E1c-0/E1c-1/E1c-2 split is reasonable。

#### M9-E1c-0: Non-covering ICP Clustered Lookup Detailed Contract

状态：

- Contract drafted；Design Review accepted；
- 只改文档，不改源码；
- 不打开用户可见 non-covering ICP；
- 下一步进入 E1c-1 debug-only one-record smoke 任务书。

目标：

- 对齐商用 non-covering secondary range ICP 的最小正确顺序；
- 明确当前分支哪些 helper 可复用、哪些能力缺失；
- 给 E1c-1/E1c-2 留出清晰入口，避免直接把 non-covering ICP 接到
  user-visible gate。

商用路径 contract：

- `find_visible_record()` 在 secondary index 分支先执行 ICP：
  - `ICP_NO_MATCH` 返回 `DB_NOT_FOUND`；
  - `ICP_OUT_OF_RANGE` 返回 `DB_END_OF_RANGE`；
  - `ICP_MATCH` 继续 visibility / clustered lookup。
- ICP 执行后，如果 read view 无法直接看到 secondary page `max_trx_id`，
  或 `prebuilt->need_to_access_clustered` 为 true，则构建/复用
  `prebuilt->sel_graph`，通过 `pq_row_sel_get_clust_rec_for_mysql()` 取
  clustered record，并设置 `mtr_has_extra_clust_latch = true`。
- clustered record 不存在或 delete-mark 时返回 `DB_NOT_FOUND`；可见时返回
  `DB_SUCCESS`。
- materialization 必须跟随 record 来源：
  - `mtr_has_extra_clust_latch == true`：从 `clust_rec` 调
    `row_sel_store_mysql_rec(..., rec_clust=true, clust_index,
    prebuilt->index, clust_offsets, ...)`；
  - 否则从 secondary/clustered 当前 `rec` 调
    `row_sel_store_mysql_rec(..., config.m_index->is_clustered(), index,
    prebuilt->index, offsets, ...)`。
- 如果做过 clustered lookup，继续扫描前必须：
  - `pcur->store_position(&mtr)`；
  - `mtr.commit()` 释放 extra clustered latch；
  - `mtr.start()`；
  - `mtr.set_log_mode(MTR_LOG_NO_REDO)`；
  - `pcursor.restore_position()`；
  - 清空 `mtr_has_extra_clust_latch`；
  - 移动 cursor 前执行 boundary check。
- offsets / record buffer 生命周期必须与 cursor restore 一起定义：
  - heap 清空后必须重置 `offsets` / `clust_offsets`；
  - server record buffer 的 `out_of_range`、`n_fetch_cached`、
    `fetch_cache_first`、`n_rows_fetched` 不能与未提交 mtr 或旧 cursor
    position 脱节；
  - generated clustered row id 必须基于实际 materialized record 写回
    `prebuilt`。
- `DB_NOT_FOUND` 表示 invisible 或 ICP filtered row，不能推进输出 buffer，
  但可以继续当前 range 的下一条 secondary record。
- `DB_END_OF_RANGE` 表示当前 range 结束；如果已有 cached rows，则先返回
  cached rows 并标记 out-of-range。

当前分支可复用能力：

- `pq_row_sel_get_clust_rec_for_mysql()` 已存在，且文档明确 returned
  clustered record 只在 active mtr 生命周期内有效。
- `validate_secondary_visibility_with_cluster_lookup()` 已能基于 secondary
  record 做 clustered lookup visibility 判定，但当前只返回
  `DB_SUCCESS` / `DB_NOT_FOUND` / `DB_UNSUPPORTED`，不返回 clustered record
  给 materialization。
- `pq_secondary_covering_range_produce()` 已保存并恢复一批
  `row_prebuilt_t` template 状态：
  `index`、`read_just_key`、`template_type`、`n_template`、
  `null_bitmap_len`、`need_to_access_clustered`、`templ_contains_blob`、
  `templ_contains_fixed_point`、`mysql_prefix_len`、`idx_cond_n_cols`、
  `keep_other_fields_on_keyread`、`in_fts_query`、`m_end_range`、
  `mysql_template`。
- `produce_secondary_range_for_user_gate()` 已有 start/end cursor、abort、
  row sink、`Parallel_secondary_rows_produced` 增长位置的基本框架。

当前分支缺口：

- user-visible secondary range producer 仍硬拒绝：
  `prebuilt->idx_cond`、`prebuilt->need_to_access_clustered`、
  `!prebuilt->read_just_key`。
- 当前 producer 只走 covering fast-path：
  `validate_secondary_visibility_fast_path()` 后直接从 secondary `rec`
  `row_sel_store_mysql_rec()`，不支持从 clustered record materialize。
- 当前 clustered lookup helper 不暴露 `clust_rec`、`clust_offsets`、
  `mtr_has_extra_clust_latch` 给调用方，也没有 continuation / restore
  协议。
- `row_search_idx_cond_check()` 仍是 `row0sel.cc` 内部 static；E1c-1 若要
  复用 serial ICP，必须新增窄 wrapper，不允许裸调
  `pushed_idx_cond->val_int()`。
- 当前 SQL 层还没有为 non-covering ICP 定义 `read_just_key=false`、
  `need_to_access_clustered=true`、`idx_cond_n_cols`、`m_end_range`、
  `mysql_template` 的构建/恢复 contract。
- fallback atomicity 仍未定义：当前 user-visible iterator 允许 handler
  `HA_ERR_UNSUPPORTED` 时回到 serial，但一旦已经有 row 进入
  `PQ_record_buffer_sink`，后续 unsupported 不能再 silent serial fallback。
  E1c-2 前必须定义 commit point：发送任何 row 后发生 error/unsupported
  只能报错或中止 PQ path，不能混合 serial 输出。

E1c-1 debug-only one-record smoke contract：

- 入口必须是 debug-only 或 DBUG-only，不接 user-visible optimizer gate；
- 只验证一条记录的链路：
  secondary rec -> ICP -> clustered lookup -> clustered materialization；
- 允许 `DB_NOT_FOUND` 后继续找下一条 secondary record，但成功 materialize
  一条后立即停止，不做长 range drain；
- 不增长 user-visible `Parallel_queries_executed`；如需观测，只使用
  scoped debug/status counter，并在 taskbook 中写清 delta；不得增长
  `Parallel_secondary_rows_produced`。
- helper 必须返回/管理：
  - ICP result；
  - `clust_rec` / `clust_offsets`；
  - `mtr_has_extra_clust_latch`；
  - 是否需要 commit/restart/restore cursor；
  - materialization record 来源。
- 任何 clustered lookup error、template mismatch、cursor restore failure、
  KILL/abort 都必须返回错误或 unsupported，不允许 silent serial restart。
- 如需要复用 existing smoke counter，应优先使用
  `Parallel_secondary_rows_materialized_smoke` 或新增 debug-only counter；
  E1c-1 不是 user-visible PQ execution。

E1c-2 user-visible non-covering range gate 前置条件：

- E1c-1 debug-only smoke 通过 Code/Task Review；
- SQL `EXPLAIN` 必须稳定出现 `Using index condition`，且查询形态为当前已知
  non-covering positive：`FORCE INDEX(k_idx)` + `v > ...`；
- 必须有 serial baseline 对照、ICP positive result correctness、
  fallback-after-boundary counter window、KILL/error cleanup；
- `Parallel_secondary_rows_produced` 只在 row 已通过 ICP、visibility、
  clustered materialization，并且 `row_sink->send_row()` 成功后增长；
- ICP filtered、invisible、delete-mark、out-of-range、unsupported、abort
  都不能增长该 counter；
- 如果后续引入 record buffer cache，`ICP_OUT_OF_RANGE` 必须正确处理
  “已有 cached rows 先返回、range 完结状态留给下一轮”的语义，不得漏行、
  重复或提前推进下一 range；
- worker clone / worker-side `idx_cond_push` / dependent ref ICP 继续禁止；
- 若 visible row 已经开始进入 PQ path，后续 unsupported 必须报错或中止
  当前 PQ path，不允许返回 serial executor 混合输出。

E1c-0 Design Review Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1c-0 Non-covering ICP Clustered Lookup Detailed Contract
Review。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- storage/innobase/row/row0pread_pq.cc
- storage/innobase/include/row0sel.h
- storage/innobase/row/row0sel.cc
- storage/innobase/handler/ha_innodb_pq.cc
- /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/storage/innobase/row/row0pread_pq.cc

检视目标：
1. 判断 E1c-0 是否准确描述商用 ICP -> clustered lookup -> materialization
   -> mtr commit/restart/restore cursor 顺序；
2. 检查当前分支可复用能力与缺口是否完整；
3. 检查 E1c-1 debug-only 和 E1c-2 user-visible 的进入条件是否足够保守；
4. 给出 `ACCEPT` 或 `REVISE`，如 REVISE 请列出必须修改项。

禁止：
- 不改文件；
- 不运行破坏性命令。
```

Review result:

- Design Review Agent returned `ACCEPT`；
- no blocking findings；
- confirmed commercial order is accurately captured:
  ICP -> clustered lookup -> materialization -> mtr commit/restart/restore
  cursor；
- confirmed current branch reuse points and gaps are complete；
- confirmed E1c-1 debug-only and E1c-2 user-visible entry conditions are
  conservative enough；
- confirmed README / M9 taskbook / E1c-0 document status is consistent。

#### M9-E1c-1: Debug-only Non-covering ICP One-record Smoke Taskbook

状态：

- Coding taskbook drafted；Taskbook Review accepted；
- 设计目标是 debug-only one-record smoke；
- Coding completed；Code/Task Review accepted。

目标：

- 用最小 debug-only 入口验证 non-covering secondary range ICP 的关键链路：
  secondary record -> serial-equivalent ICP -> clustered lookup ->
  clustered-record materialization。
- 不接 user-visible optimizer gate；
- 不做 range drain；
- 不增长 user-visible PQ execution counters；
- 不支持 worker clone / worker-side ICP / dependent ref / constant ref ICP。

允许修改：

- `storage/innobase/include/row0sel.h`
- `storage/innobase/row/row0sel.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/handler/ha_innodb.h`
- `sql/handler.h`
- `sql/parallel_query/pq_optimizer.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/m9-e-icp-pushdown.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/pq_iterators.h`
- `sql/join_optimizer/access_path.cc`
- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_clone.*`
- worker-side Item clone / refix 逻辑；
- 用户可见 secondary range / ref eligibility gate；
- public handler execution API 的已有语义。

实现要求：

1. 新增窄 ICP wrapper：
   - 必须复用 serial `row_search_idx_cond_check()` 语义；
   - 不允许直接调用 `pushed_idx_cond->val_int()`；
   - wrapper 输入必须包含 `row_prebuilt_t *`、secondary `rec`、
     secondary `offsets`、MySQL `record[0]` buffer；
   - 必须准确返回 `ICP_NO_MATCH` / `ICP_OUT_OF_RANGE` / `ICP_MATCH`
     或等价的 PQ-local enum；
   - wrapper 必须留在 InnoDB 内部窄接口，不扩大 SQL public handler API。
2. 新增 debug-only InnoDB one-record helper：
   - 只处理 single-table、secondary index、forward half-open range；
   - 要求 active read view、`LOCK_NONE`、非 intrinsic table；
   - 允许 `prebuilt->idx_cond != nullptr`；
   - 允许 `prebuilt->need_to_access_clustered == true`；
   - 必须从 secondary record 先执行 ICP；
   - `ICP_NO_MATCH` 可以继续找下一条 secondary record；
   - `ICP_OUT_OF_RANGE` 结束 smoke，不能 materialize；
   - `ICP_MATCH` 后做 clustered lookup；
   - clustered record 不存在或 delete-mark 时继续找下一条 secondary record；
   - 成功 materialize 一条 clustered record 后立即停止，不继续 drain。
3. materialization contract：
   - non-covering 输出必须从 `clust_rec` materialize；
   - 必须使用 `row_sel_store_mysql_rec(..., rec_clust=true, clust_index,
     prebuilt->index, clust_offsets, ...)` 等价路径；
   - clustered record lifetime 只在 active mtr 内有效；
   - 如果 helper 在 materialize 后需要继续扫描，必须先设计
     store/commit/restart/restore cursor；E1c-1 不允许继续 drain，因此可以
     materialize 后停止并提交 mtr。
4. debug 入口：
   - 新增 DBUG 名建议：
     `pq_secondary_noncovering_icp_one_record_smoke`；
   - 触发方式可沿用 `pq_optimizer.cc` 的 secondary range smoke 风格；
   - 只允许在 `EXPLAIN SELECT * FROM pq_ref_icp_t1 FORCE INDEX(k_idx)
     WHERE k BETWEEN 20 AND 40 AND v > 150` 这类 non-covering ICP 形态中触发；
   - 普通执行不受影响。
5. counters：
   - 可以复用 `Parallel_secondary_rows_materialized_smoke`，成功
     materialize 一条 clustered row 后增长 1；
   - 不得增长 `Parallel_secondary_rows_produced`；
   - 不得增长 `Parallel_queries_executed`；
   - fallback negative window 中 `executed/workers/ranges/secondary_rows`
     必须保持 0。
6. prebuilt / template 状态：
   - 必须保存并恢复 `index`、`read_just_key`、
     `need_to_access_clustered`、`idx_cond`、`idx_cond_n_cols`、
     `m_end_range`、
     `mysql_template`、`n_template`、`template_type`、`null_bitmap_len` 等
     handler template 状态；
   - 不能依赖 `reset_template()` 隐式处理 ICP 状态，因为它会清理
     `idx_cond` / `idx_cond_n_cols`；E1c-1 必须显式保存并恢复二者；
   - helper 结束后必须关闭 cursor、commit mtr、释放 heap；
   - 不得留下 dirty `pcur` / `clust_pcur` / read view。
7. fail-closed：
   - template mismatch、clustered lookup error、ICP wrapper error、
     cursor open/restore error、OOM、KILL/abort 均返回 unsupported/error；
   - debug smoke 不能 silent 开启 user-visible PQ path；
   - 普通 ICP SELECT 仍保持 E0 negative guard。

验证要求：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
```

建议 MTR 断言：

- 开启 `SET SESSION debug="d,pq_secondary_noncovering_icp_one_record_smoke"`；
- `EXPLAIN SELECT * FROM pq_ref_icp_t1 FORCE INDEX(k_idx)
  WHERE k BETWEEN 20 AND 40 AND v > 150`；
- `Parallel_secondary_rows_materialized_smoke` delta 为 1；
- 同一窗口内 `Parallel_secondary_rows_produced` delta 为 0；
- `Parallel_queries_executed` delta 为 0；
- `Parallel_workers_launched` delta 为 0；
- `Parallel_ranges_built` delta 为 0；
- `Parallel_ranges_dispatched` delta 为 0；
- debug 关闭后同一 ICP SELECT 仍 fallback，E0 negative guard 不变；
- 非 ICP / covering ICP 候选不触发该 smoke。

Agent task prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1c-1 Debug-only Non-covering ICP One-record Smoke。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- storage/innobase/row/row0pread_pq.cc
- storage/innobase/include/row0pread_pq.h
- storage/innobase/include/row0sel.h
- storage/innobase/row/row0sel.cc
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/handler/ha_innodb.h
- sql/handler.h
- sql/parallel_query/pq_optimizer.cc
- mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test

任务目标：
1. 新增 debug-only one-record smoke，验证 secondary ICP -> clustered lookup
   -> clustered materialization；
2. 保持 user-visible ICP SELECT fallback；
3. 不增长 `Parallel_secondary_rows_produced` 或
   `Parallel_queries_executed`；
4. 编译并运行 targeted MTR。

必须遵守本 taskbook 的 Allowed / Forbidden Files。

完成后：
1. 在本文件 Completion Report 填写 changed files、实现说明、验证结果、
   风险点；
2. 不要提交 commit。
```

Taskbook Review Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Taskbook Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1c-1 Debug-only Non-covering ICP One-record Smoke
Taskbook Review。

只读任务：不改文件。

请检查：
1. allowed / forbidden files 是否能支撑 debug-only smoke 且不会误开
   user-visible gate；
2. ICP wrapper、clustered lookup、materialization、counter、prebuilt restore
   要求是否足够完整；
3. MTR 断言是否能证明 debug-only one-record smoke，同时保持 E0 negative
   guard；
4. 给出 `ACCEPT` 或 `REVISE`。
```

Taskbook Review result:

- First review returned `REVISE`；
- fixed requirements:
  - explicitly save/restore `prebuilt->idx_cond` as well as
    `idx_cond_n_cols`；
  - assert same-window `Parallel_workers_launched`、
    `Parallel_ranges_built`、`Parallel_ranges_dispatched` deltas are 0；
  - add `sql/handler.h` and `storage/innobase/handler/ha_innodb.h` to the
    Code Agent read list；
- second review returned `ACCEPT`。

Completion Report:

- Changed files:
  - `sql/handler.h`
  - `sql/parallel_query/pq_optimizer.cc`
  - `storage/innobase/handler/ha_innodb.h`
  - `storage/innobase/handler/ha_innodb_pq.cc`
  - `storage/innobase/include/row0pread_pq.h`
  - `storage/innobase/row/row0pread_pq.cc`
  - `storage/innobase/include/row0sel.h`
  - `storage/innobase/row/row0sel.cc`
  - `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
  - `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- Implementation:
  - added a debug-only handler hook
    `pq_secondary_noncovering_icp_one_row_smoke()`；
  - added an InnoDB narrow ICP wrapper over serial
    `row_search_idx_cond_check()`；
  - added a one-record scan_ctx smoke helper that evaluates ICP on secondary
    records, fetches clustered record only after `ICP_MATCH`, materializes one
    clustered record, then stops；
  - added optimizer DBUG gate
    `pq_secondary_noncovering_icp_one_record_smoke`；
  - added MTR assertions proving materialized smoke delta is 1 while
    user-visible PQ execution / workers / ranges / secondary rows stay 0。
- Validation:
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl
    --suite=parallel_query --record pq_commercial_ref_icp` passed；
  - `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl
    --suite=parallel_query pq_commercial_ref_icp` passed；
  - `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl
    --suite=parallel_query --parallel=1` passed 74/74。
- Observed MTR delta:
  - `e1c1_materialized_smoke_delta = 1`；
  - `e1c1_secondary_rows_produced_delta = 0`；
  - `e1c1_executed_delta = 0`；
  - `e1c1_workers_delta = 0`；
  - `e1c1_ranges_built_delta = 0`；
  - `e1c1_ranges_dispatched_delta = 0`。
- Risks:
  - still debug-only；does not prove safe continued secondary drain after
    clustered lookup；
  - user-visible non-covering ICP remains disabled until E1c-2；
  - worker-side ICP clone/refix remains out of scope。
- Review:
  - Code/Task Review Agent returned `ACCEPT`；
  - no blocking findings；
  - confirmed debug-only gate, serial ICP wrapper reuse, clustered lookup after
    `ICP_MATCH`, one-record stop, state restore, counters, MTR and completion
    report are acceptable。

#### M9-E1c-2: User-visible Non-covering Secondary Range ICP Gate Design

状态：

- Design taskbook drafted；Design Review accepted；
- 不改源码；
- 目标是从 E1c-1 one-record smoke 进入 user-visible gate 前的实现契约。

目标：

- 定义最小 user-visible non-covering secondary range ICP path；
- 复用 E1c-1 已验证的 ICP -> clustered lookup -> clustered materialization；
- 新增安全的 continued secondary drain / cursor restore contract；
- 只打开 single-table、single range、forward、non-partition、
  non-covering secondary ICP 正例；
- worker-side ICP clone/refix、ref/dependent ref ICP、reverse/partition/MVI
  继续禁止。

候选 SQL 正例：

```sql
SELECT * FROM pq_ref_icp_t1 FORCE INDEX(k_idx)
 WHERE k >= 20 AND k < 40 AND v > 250;
```

Coding note: E1c-2a 实现时将用户可见正例收窄为
`pq_ref_icp_t4 FORCE INDEX(k_icp_idx)` + `icp_col >= 250` + non-covering
integer `payload`，避免首个 gate 同时承诺 nullable/CHAR materialization。

测试数据前置：

- 当前 `pq_ref_icp_t1` 的 `k >= 20 AND k < 40` 覆盖
  `(20,200)`、`(20,210)`、`(30,300)`；
- `v > 250` 必须稳定产生：
  - in-range `ICP_NO_MATCH`：`k=20` 两行被 ICP 过滤；
  - in-range `ICP_MATCH`：`k=30, v=300` 通过 ICP 并做 clustered
    materialization；
- 若后续调整测试数据，必须继续保留至少一个 in-range ICP filtered row 和
  至少一个 in-range ICP matched row。

进入条件：

- `EXPLAIN` 稳定显示 `Using index condition`；
- 访问路径是单表 `JT_RANGE` / `INDEX_RANGE_SCAN`；
- `range_scan->index_range_scan().num_ranges == 1`；
- key 是 secondary index，非 primary，非 partition；
- range endpoint 仅支持 M9-B3 已验证的 half-open forward range：
  start `HA_READ_KEY_OR_NEXT`，end `HA_READ_BEFORE_KEY`；
- `table->file->pushed_idx_cond != nullptr` 且
  `pushed_idx_cond_keyno == keyno`；
- 必须新增显式正向 predicate 判断“non-covering but clustered-materializable
  and safe”，不能使用 `!pq_secondary_covering_read_set_is_safe()`：
  - read_set 至少包含一个目标 secondary index 不覆盖的列；
  - 所有 read_set 字段必须是 clustered record 可 materialize 的普通 stored
    base column；
  - 第一版只允许 fixed-length、non-null、非 virtual/generated、非 blob/text/
    json/geometry、非 multi-valued/functional key 相关字段；
  - WHERE residual / ICP 中引用的非 key 列必须可从 clustered record
    materialize；
  - 不支持 partial prefix、nullable、varlen 或需要外部 LOB 的字段；
  - 该 predicate 返回 false 时只是“unsupported”，不能被解释为
    non-covering safe；
- 不允许 GROUP BY、HAVING、ORDER BY gather merge、JOIN、dependent ref、
  locking read、reverse scan、geometry/MVI/spatial/FTS、virtual/generated
  output列、BLOB/TEXT/JSON first path。

实现契约：

1. SQL gate：
   - 不能修改 existing covering range gate 的安全边界；
   - 建议新增独立
     `TryCreatePQSecondaryNoncoveringIcpRangeIterator()` 或同等清晰入口；
   - ordinary unsupported 情况继续 serial fallback；
   - 一旦 handler 已经向 row sink 发送 row，后续 error/unsupported 必须
     返回错误，不能 serial fallback 混合输出。
2. Handler / prebuilt：
   - public handler API 可新增窄 user-visible hook，但不得改已有 hook
     语义；
   - 必须保存并恢复 `active_index`、`pushed_idx_cond`、
     `pushed_idx_cond_keyno`、`prebuilt->idx_cond`、
     `idx_cond_n_cols`、`need_to_access_clustered`、`read_just_key`、
     `m_end_range`、`mysql_template`、`n_template`、`template_type`、
     `null_bitmap_len` 等状态；
   - build template 必须等价于 serial secondary ICP non-covering path：
     `read_just_key=0`、secondary `prebuilt->index`、pushed key 等于 keyno、
     `need_to_access_clustered=true`；
   - `reset_template()` 会清理 ICP 状态，不能依赖隐式恢复。
3. InnoDB scan loop：
   - ICP 必须先在 secondary record 上执行；
   - `ICP_NO_MATCH`：不做 clustered lookup，不发送 row，继续下一条；
   - `ICP_OUT_OF_RANGE`：结束当前 range；
   - `ICP_MATCH`：做 clustered lookup；
   - clustered record missing/delete-mark：不发送 row，继续下一条；
   - visible clustered record：从 clustered record materialize，再
     `row_sink->send_row()`；
   - `Parallel_secondary_rows_produced` 只在 send 成功后增长。
4. continued drain / cursor restore：
   - 每次 clustered lookup 后，若继续扫描，必须保存 secondary pcur
     position，commit mtr 释放 clustered latch，restart mtr，restore
     secondary position，清空 extra-latch 标记，再做 boundary check 后移动；
   - heap reset 后必须重置 `offsets` / `clust_offsets`；
   - 不允许持有 clustered latch 后直接移动 secondary cursor；
   - cursor restore 失败必须返回 error/unsupported，不得 silent fallback。
5. row sink / counters：
   - user-visible path 成功时可增长 `Parallel_queries_executed`；
   - 不 launch workers，不 build/dispatch worker ranges；
   - `Parallel_secondary_rows_produced` 等于最终发送行数；
   - ICP filtered / invisible / delete-mark / out-of-range / abort 均不增长；
   - `row_count` 必须和 SQL row sink buffered rows 一致。

验证要求：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --parallel=1
```

MTR 必须覆盖：

- serial baseline 与 PQ result correctness：
  - ICP on/off 对照；
  - 返回行数与内容一致；
  - 必须覆盖 in-range `ICP_NO_MATCH` 跳过行。E1c-2a 实际使用
    `pq_ref_icp_t4(k, icp_col, payload)` 中的 `icp_col >= 250`，过滤
    `k=20` 两行并保留 `k=30` 一行；
- positive counter window：
  - `Parallel_queries_executed` delta = 1；
  - `Parallel_secondary_rows_produced` delta = expected output rows；
  - `Parallel_workers_launched`、`Parallel_ranges_built`、
    `Parallel_ranges_dispatched` delta = 0；
- fallback windows：
  - covering ICP candidate 不误入该 path；
  - unsafe non-covering read_set 不能因
    `!pq_secondary_covering_read_set_is_safe()` 被误判为 eligible；
  - multi-range / reverse / partition / primary / ref / dependent ref /
    no ICP 继续 fallback；
  - fallback windows 的 `executed/workers/ranges/secondary_rows` 均为 0；
- error/abort:
  - 如本阶段不实现 KILL/error injection，必须在 taskbook 中显式说明
    deferred，并保持 max_rows cap fail-closed。

Design Review Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1c-2 User-visible Non-covering Secondary Range ICP Gate
Design Review。

只读任务：不改文件。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- sql/parallel_query/pq_iterators.cc
- sql/parallel_query/pq_optimizer.cc
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/row/row0pread_pq.cc
- mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test

检视目标：
1. 判断 user-visible E1c-2 gate 是否应该以该设计进入 coding；
2. 检查 SQL gate、handler/prebuilt、continued drain、counter、MTR 约束是否
   足够保守；
3. 检查是否遗漏必须先完成的安全前置；
4. 给出 `ACCEPT` 或 `REVISE`。
```

Design Review result:

- First review returned `REVISE`；
- blocking findings addressed:
  - replaced `!pq_secondary_covering_read_set_is_safe()` with a required
    positive “non-covering but clustered-materializable and safe” predicate；
  - changed positive SQL shape to `v > 250` over current test data, so the
    range has both in-range `ICP_NO_MATCH` rows and an `ICP_MATCH` row；
- coding later replaced the test shape with `pq_ref_icp_t4/k_icp_idx` and
  `icp_col >= 250` to keep the first user-visible gate integer-only while
  preserving in-range ICP misses plus one ICP match；
- second review returned `ACCEPT`。

#### M9-E1c-2a: User-visible Non-covering ICP Range Coding Taskbook

状态：

- Coding taskbook drafted；Taskbook Review accepted；
- 下一步进入 coding。

目标：

- 打开最小 user-visible non-covering secondary range ICP path；
- 只支持 single-table、single forward half-open secondary range；
- ICP 在 secondary record 上先执行；
- `ICP_MATCH` 后 clustered lookup 并从 clustered record materialize；
- clustered lookup 后安全继续 secondary drain；
- 不 launch worker，不 build/dispatch worker ranges。

允许修改：

- `sql/handler.h`
- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/pq_iterators.h`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/m9-e-icp-pushdown.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/join_optimizer/access_path.cc`
- `sql/sql_optimizer.cc`
- `sql/sql_select.cc`
- `sql/parallel_query/pq_clone.*`
- worker-side Item clone / refix；
- ref / dependent ref ICP path；
- M9-E1c-1 debug smoke 语义；
- E1c-2 设计之外的 range boundary 扩展。

实现要求：

1. SQL gate / iterator：
   - 新增独立 non-covering ICP range iterator 或清晰分支，不复用 covering
     name 造成语义混淆；
   - eligibility 必须要求：
     - single table；
     - `JT_RANGE` / `INDEX_RANGE_SCAN`；
     - secondary key；
     - `num_ranges == 1`；
     - non-reverse, non-geometry, non-partition；
     - `pushed_idx_cond != nullptr` and `pushed_idx_cond_keyno == keyno`；
     - half-open forward endpoint only；
     - positive non-covering-safe predicate accepted；
   - handler 在发送任何 row 前返回 `HA_ERR_UNSUPPORTED` 可以 fallback；
   - handler 一旦发送过 row，后续 error/unsupported 必须返回 error，不得
     serial fallback。
2. Positive non-covering-safe predicate：
   - 不得使用 `!pq_secondary_covering_read_set_is_safe()`；
   - read_set 必须包含至少一个 secondary key 不覆盖的列；
   - 所有 read_set 字段必须是 stored base column，fixed-length，
     non-null，非 virtual/generated/blob/text/json/geometry/MVI/functional；
   - 第一版可以只允许 `INT NOT NULL` 这类简单字段；
   - unsafe 返回仅代表 unsupported。
3. Handler / prebuilt：
   - 新增 user-visible handler hook；
   - 必须显式保存恢复 `active_index`、`pushed_idx_cond`、
     `pushed_idx_cond_keyno`、`prebuilt->idx_cond`、
     `idx_cond_n_cols`、`need_to_access_clustered`、`read_just_key`、
     `m_end_range`、`mysql_template`、`n_template`、`template_type`、
     `null_bitmap_len`；
   - build template 使用 serial-equivalent non-covering secondary ICP state：
     `read_just_key=0`、secondary `prebuilt->index`、
     `pushed_idx_cond_keyno=keyno`、`need_to_access_clustered=true`。
4. InnoDB scan loop:
   - start/end endpoint 与 B3 half-open 语义一致；
   - per record:
     - compute secondary offsets；
     - check end boundary；
     - run `pq_row_search_idx_cond_check()`；
     - `ICP_NO_MATCH`: continue；
     - `ICP_OUT_OF_RANGE`: finish；
     - `ICP_MATCH`: clustered lookup；
     - missing/delete-mark: continue；
     - visible: materialize from clustered record and `row_sink->send_row()`；
   - after clustered lookup, if continuing:
     - store secondary pcur position；
     - commit mtr；
     - restart mtr；
     - restore secondary pcur；
     - reset offsets heap state；
     - boundary check before moving cursor；
   - cursor restore failure must return error/unsupported；
   - after any row has been sent to the SQL row sink, subsequent
     error/unsupported must not silently fallback to serial execution；
   - no clustered latch may be held while advancing secondary cursor。
5. Counters:
   - successful path:
     - `Parallel_queries_executed += 1`；
     - `Parallel_secondary_rows_produced += sent_rows`；
     - `Parallel_workers_launched/ranges_built/ranges_dispatched` unchanged；
   - ICP filtered / invisible / delete-mark / abort do not increment
     produced rows。
6. MTR:
   - positive query uses `pq_ref_icp_t4/k_icp_idx` and `icp_col >= 250` to
     force two in-range ICP misses and one ICP match；
   - expected result is one row: id 3 / k 30 / payload 3000；
   - compare serial ICP on/off result；
   - assert counters；
   - assert fallback for no ICP, covering candidate, unsafe non-covering
     read_set, primary, ref, dependent ref, multi-range/reverse if expressible；
   - each fallback boundary must use an independent counter window and assert
     `Parallel_queries_executed`、`Parallel_workers_launched`、
     `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
     `Parallel_secondary_rows_produced` deltas are 0。
7. Error/abort scope:
   - E1c-2a does not need to add KILL/error injection if the taskbook records
     this as deferred；
   - if KILL/error injection is deferred, the implementation must keep a
     bounded `max_rows` cap and fail closed on cap hit；
   - cap hit before any row is sent may return unsupported and fallback；
   - cap hit after any row is sent must return error/abort, not fallback。

验证命令：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --parallel=1
```

Taskbook Review Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Taskbook Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-E1c-2a User-visible Non-covering ICP Range Coding Taskbook
Review。

只读任务：不改文件。

请阅读：
- Docs/pq_tasks/m9-e-icp-pushdown.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- sql/parallel_query/pq_iterators.cc
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/row/row0pread_pq.cc
- mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test

检视目标：
1. 判断 allowed/forbidden files 是否足以实现且范围不外溢；
2. 检查 positive non-covering-safe predicate 是否写清楚；
3. 检查 continued drain / cursor restore 是否足以进入 coding；
4. 检查 MTR 与 counters 是否能证明 user-visible path 和 fallback 边界；
5. 给出 `ACCEPT` 或 `REVISE`。
```

Taskbook Review result:

- First review returned `REVISE`；
- blocking findings addressed:
  - carried forward cursor restore failure and post-row no-silent-fallback
    requirements；
  - required independent zero-growth counter windows for every fallback
    boundary；
  - recorded KILL/error injection as deferrable only with bounded max_rows
    fail-closed semantics；
- second review returned `ACCEPT`。

Completion Report:

- Pending coding。

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

## M9-E1c-2a Completion Report

Coding completed by Codex Orchestrator; Code/Task Review accepted.

Changed files:

- `sql/handler.h`
- `sql/parallel_query/pq_iterators.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/m9-e-icp-pushdown.md`

Implementation notes:

- Added a positive SQL predicate for non-covering but clustered-materializable
  secondary range ICP; it is not implemented as `!covering_safe`。
- Added `PQSecondaryNoncoveringIcpRangeIterator` as a separate leader-local
  iterator. It buffers bounded row images, falls back only before any row is
  buffered, and treats unsupported/error after buffering as fatal。
- Added handler/InnoDB hook
  `pq_secondary_noncovering_icp_range_produce()` and scan_ctx helper
  `produce_secondary_icp_range_for_user_gate()`。
- InnoDB order is ICP on secondary record -> clustered lookup for
  `ICP_MATCH` -> clustered record materialization -> secondary cursor
  restore -> continue drain。
- Scope remains single-table, single range, InnoDB, non-partitioned,
  non-reverse, no worker/MQ, no ref/dependent-ref ICP, max 64 produced rows。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp
```

Target MTR assertions:

- Positive query uses `pq_ref_icp_t4 FORCE INDEX(k_icp_idx)` with
  `WHERE k >= 20 AND k < 40 AND icp_col >= 250` and selects non-covering
  integer `payload`；
- `EXPLAIN` contains `Using index condition`；
- result row is `(id=3, k=30, payload=3000)`；
- `e1c2_executed_delta=1`；
- `e1c2_secondary_rows_produced_delta=1`；
- `e1c2_workers_delta=0`；
- `e1c2_ranges_built_delta=0`；
- `e1c2_ranges_dispatched_delta=0`；
- ICP-off fallback window has `executed=0`、`fallback=0`、`workers=0`、
  `ranges_built=0`、`ranges_dispatched=0`、`secondary_rows=0`；
- unsafe read-set fallback window has `executed=0`、`fallback=0`、
  `workers=0`、`ranges_built=0`、`ranges_dispatched=0`、
  `secondary_rows=0`。

Risk notes:

- This is still leader-local and buffered; it does not migrate worker-side ICP
  clone/refix。
- The user-visible positive is intentionally limited to non-null integer
  projected/predicate fields. Nullable, CHAR/VARCHAR, BLOB, JSON, geometry,
  partition, reverse, ref/dependent-ref, and worker/MQ paths remain out of
  scope。
- Range endpoint support remains conservative; this stage does not add
  `HA_READ_AFTER_KEY` semantics。

Review:

- First Code/Task Review returned `REVISE`；
- fixed `ICP_OUT_OF_RANGE` to use common success finalization so `row_count`
  remains consistent with buffered rows；
- expanded no-ICP and unsafe-read-set fallback windows to assert zero
  `executed/fallback/workers/ranges_built/ranges_dispatched/secondary_rows`；
- clarified the actual `pq_ref_icp_t4/k_icp_idx/icp_col/payload` positive
  shape in taskbook history；
- second Code/Task Review returned `ACCEPT`。
