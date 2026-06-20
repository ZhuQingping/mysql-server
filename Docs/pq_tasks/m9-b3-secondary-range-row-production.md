# M9-B3 Secondary Range Callback Row Production Design

## 状态

Design taskbook created by Codex Orchestrator. Review Agent requested revision.
This revision records the concrete B3a design answers. No source code changes in
this step.

M9-B3a coding entry was attempted by Codex Orchestrator and stopped before
source edits because the current upstream `Parallel_reader` secondary visibility
path is still unsupported.

Base commit:

```text
72ecd443852f3323b9c30a67207164d4e73b963a
```

## 目标

M9-B3 的目标是从 M9-B2 的 secondary range partition smoke 向真实 row production 推进，但第一步必须保持极窄正例，避免一次性打开完整 secondary/ref/ICP 商用路径。

本阶段优先设计并实现 **covering secondary range callback row production 的最小正例**：

- 单表 InnoDB；
- 单 secondary `INDEX_RANGE_SCAN`；
- forward range；
- non-partition table；
- covering secondary index；
- 无 ICP；
- 无 reverse scan；
- 无 MVI / spatial / descending keypart；
- 无 `JT_REF` / dependent ref；
- 默认不扩大 boundary 语义，仅复用 M9-B2 已验证的 half-open forward range：`k >= a AND k < b`。

如果实现前确认 covering secondary row materialization 仍需要较大 SQL record/field 协议改造，则必须拆成：

- M9-B3a: covering secondary row production 设计与 smoke；
- M9-B3b: non-covering secondary range cluster lookup；
- M9-B3c: 用户可见 secondary range PQ gate。

## 当前前置状态

M9-B2 已完成并提交：

- `sql/handler.h` 新增 debug-only `pq_secondary_range_partition_smoke()`；
- SQL 层从 `QUICK_RANGE` endpoint deep-copy key buffer；
- InnoDB 用 `row_sel_convert_mysql_key_to_innobase()` 构造 start/end tuple；
- `InnoDB_pq_scan_ctx::partition(split_level, start, end)` 可用 explicit secondary tuple 构造 exported ranges；
- 只接受 `Parallel_reader::Scan_range(start,end)` 可精确表达的 half-open forward range；
- 普通 secondary range/ref/ICP SELECT 仍 fallback；
- `Parallel_secondary_ranges_built` 只在 debug smoke 中增长；
- `Parallel_secondary_rows_produced` 保持 0；
- 完整当前 `parallel_query` suite 74 项通过。

当前 row production 关键限制：

- `InnoDB_pq_scan_ctx::produce_callback_rows_for_range()` 仍要求 `m_index->is_clustered()`；
- 当前 `store_callback_record()` 路线已服务 clustered full scan record image；
- secondary index tuple 是否能直接 materialize MySQL record，取决于 covering index 字段布局、null/varlen 字段、virtual/generated column 和 hidden PK 字段处理；
- non-covering secondary range 需要 cluster lookup，不能混入 B3 最小正例；
- ICP pushdown 需要 worker 侧 Item 生命周期，不能混入 B3 最小正例。

## 设计问题

M9-B3 编码前必须先回答以下问题。当前结论如下，B3a Code Agent 必须按这些
规则实现；如果实现中发现规则不可执行，停止编码并回到设计。

1. Covering 判定在哪里做？
   - B3a 只允许 debug-only smoke，不放开用户可见 `INDEX_RANGE_SCAN` PQ gate；
   - debug smoke 的 covering 判定在 SQL 层完成，基于 `TABLE::read_set` 里的被读列；
   - `read_set` 中每个列必须能在目标 secondary index 的 user-defined key parts 中找到同一 `Field::field_index()`；
   - B3a 不把 hidden primary key / hidden clustered key 计入 covering 输出列；
   - `read_set` 为空、包含 virtual/generated column、blob/text、geometry、JSON、multi-valued key 相关字段，或任何无法映射到 secondary key part 的列，必须 fail-closed；
   - WHERE residual 若引用 range key 之外的列，必须 fail-closed；B3a 不做 worker-side residual condition eval；
   - Nullable 和 varlen 字段第一版不作为正例支持；后续如要支持，必须追加 MTR 并明确 null bitmap/length bytes 写入规则；
   - InnoDB row production 不允许临时猜测 covering。

2. secondary tuple 如何写入 MySQL record？
   - B3a 只允许固定长度、非 nullable、非虚拟、非 generated 的 key columns；
   - conversion helper 必须只从 secondary index tuple 中读取已确认 covering 的 user-defined key parts；
   - conversion helper 必须写入 `TABLE::record[0]` 对应 Field 的 storage，不写入不在 `read_set` 中的列；
   - 不允许从 hidden PK/cluster key 填充用户可见列；
   - 若当前 `Parallel_reader::Ctx` 无法提供 secondary tuple 到 MySQL Field 的稳定转换信息，B3a 不得实现 row smoke，应将状态改为 blocked/design-only；
   - 不允许复用 clustered-only `store_callback_record()` 作为 secondary record conversion，除非先拆出明确支持 secondary tuple 的新 helper。

3. MVCC / deleted-mark 如何处理？
   - 必须复用已验证的 leader-pinned read view；
   - callback 只发送 visible row；
   - B3a 不允许为了可见性执行 clustered lookup；任何 clustered access 都后置到 B3c；
   - B3a 只有在当前 8.0.46 `Parallel_reader` 对 secondary index scan 能保证 callback record 已经过 read-view visibility 与 delete-mark 过滤时才能实现；
   - 如果 `Parallel_reader` secondary callback 只提供物理 secondary record，不能证明 MVCC/deleted-mark 语义正确，则 B3a 必须停留在 design-only，不做 row smoke；
   - purge/old-version 语义不能由 SQL 层补救，不能发送未确认 visible 的 secondary tuple；
   - 后续 non-covering 或 visibility 需要 clustered lookup 时另开 B3c。

4. range boundary 是否扩大？
   - M9-B3 不扩大 M9-B2 边界；
   - 只接受 start absent/`HA_READ_KEY_OR_NEXT` 与 end absent/`HA_READ_BEFORE_KEY`；
   - `BETWEEN`、`>`、`<=`、`=`、inclusive upper、reverse range 继续 fallback；
   - 扩大边界必须另立 M9-B4，并设计 index_read/cursor-positioning helper。

5. worker cleanup 如何保证？
   - worker started 后不得 silent serial fallback；
   - `KILL QUERY`、leader abort、worker ERROR 必须经 typed MQ / row sink 返回；
   - `pq_worker_scan_callback_produce()` 与 secondary row producer 必须共用已有 abort / FINISH / ERROR 语义；
   - row production 中途失败不得留下 active cursor、dirty `m_prebuilt` 或未 join worker。

6. 是否接用户可见执行入口？
   - B3 第一提交建议先做 debug-only smoke，证明 rows produced 可控增长；
   - 用户可见 secondary range PQ gate 需要单独 review；
   - 若打开用户可见 gate，必须保留默认 OFF 或极窄 eligibility，并新增实际 SELECT 结果正确性 MTR。

7. `Parallel_secondary_rows_produced` 何时增长？
   - B3a 中该 counter 只能在 row 已通过 visibility/delete-mark 检查、已成功 materialize 到 MySQL record、并且 `row_sink->send_row()` 成功返回之后增长；
   - 若 row 被过滤、materialize 失败、sink abort、worker error 或 KILL 中断，不能增长；
   - debug smoke MTR 必须同时验证 rows produced delta 和普通 fallback SELECT 不增长该 counter。

## 实现拆分建议

### M9-B3 Design

只读确认与文档更新：

- 对比当前 `store_callback_record()` / `produce_callback_rows_for_range()`；
- 对比商用 `row0pread_pq.cc` secondary row production；
- 明确 covering-only 是否可行；
- 明确哪些 field type 第一版支持；
- 明确是否需要新的 handler API，而不是复用 `pq_secondary_range_partition_smoke()`。

### M9-B3a Covering Secondary Row Smoke

建议只做 debug-only smoke：

- 新增 InnoDB helper，例如 `pq_secondary_range_callback_smoke()`；
- 复用 M9-B2 cloned half-open range endpoint；
- 在 debug flag 下 callback produce 覆盖索引 row；
- 增长 `Parallel_secondary_rows_produced`；
- 不修改 `access_path.cc`；
- 普通 secondary range SELECT 仍 fallback。

Current coding gate result:

- 当前 `storage/innobase/include/row0pread.h` 仍明确写着
  `Secondary index scans are not supported currently`；
- 当前 `Parallel_reader::check_visibility()` 在 `trx->read_view != nullptr`
  且 `!index->is_clustered()` 时直接 `ut_error`；
- 当前 `InnoDB_pq_scan_ctx::produce_callback_rows_for_range()` 也显式要求
  `m_index->is_clustered()`；
- 因此 B3a 不能直接调用 `Parallel_reader` 读取 secondary callback rows；
- 按本文件已接受的设计规则，若不能证明 secondary callback 已经过
  MVCC/delete-mark 过滤，必须停止，不实现 row smoke。

Conclusion:

- M9-B3a covering secondary row smoke 暂时 blocked；
- 下一步必须先做 M9-B3a-0 Secondary Visibility Helper Design/Implementation；
- M9-B3a-0 目标是从商用 `PQ_Scan_ctx::find_visible_record()` 平移最小
  secondary visibility/delete-mark 逻辑，明确是否允许 clustered lookup；
- 在 M9-B3a-0 通过 review 前，不允许实现 `pq_secondary_range_callback_smoke()`。

### M9-B3a-0 Secondary Visibility Helper

目标：

- 只做 InnoDB secondary visibility/delete-mark 前置 helper；
- 对齐商用 `find_visible_record()` 的最小语义；
- 明确 covering secondary range 是否仍需要 clustered lookup 来验证可见性；
- 若需要 clustered lookup，则 B3a 原来的“no clustered lookup”约束必须调整，
  并把 row smoke 后置；
- 不产生 MySQL record；
- 不增长 `Parallel_secondary_rows_produced`；
- 不修改 SQL iterator factory；
- 普通 secondary range SELECT 继续 fallback。

建议允许修改：

- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `Docs/pq_tasks/m9-b3-secondary-range-row-production.md`

禁止修改：

- `sql/join_optimizer/access_path.cc`
- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_iterators.*`
- `sql/parallel_query/pq_clone.*`
- 用户可见 PQ execution gate

验收：

- helper 不触发 `Parallel_reader::check_visibility()` 的 secondary `ut_error`；
- helper 对 delete-mark、old version、read view 不做不安全假设；
- 如果 helper 必须 cluster lookup，文档明确 B3a row smoke 需要拆成 B3a-1；
- build 通过；
- targeted smoke/negative MTR 保持 ordinary secondary range fallback。

### M9-B3a-1 Secondary Visibility Fast-path Design

Review 后结论：

- M9-B3a-1 不直接平移商用 `find_visible_record()` 的完整 clustered
  lookup 分支；
- 当前 8.0.46 仓库没有商用新增的
  `pq_row_sel_get_clust_rec_for_mysql()` public wrapper；
- 当前 `row_search_idx_cond_check()` 仍是 `row0sel.cc` 内部 static；
- B3a 仍不做 ICP，因此无需在 B3a-1 暴露 ICP helper；
- clustered lookup 只为 visibility 使用是商用正确方向，但它需要单独
  B3a-2 设计/实现；
- B3a-1 只定义 covering secondary visibility fast-path：
  - input: secondary `rec`、`offsets`、`mtr`、worker/handler `prebuilt`；
  - active read view 必须存在；
  - 不能复用 `has_active_read_view()` 作为 dereference read view 的条件；
  - 必须显式要求 `m_trx != nullptr && m_trx->read_view != nullptr &&
    MVCC::is_view_active(m_trx->read_view)`；
  - `srv_read_only_mode` 但没有 active read view 的情况在 B3a-1 unsupported；
  - index 必须是 secondary index；
  - `prebuilt->idx_cond == false`；
  - `prebuilt->need_to_access_clustered == false`；
  - `page_get_max_trx_id(page_align(rec))` 必须被 read view sees；
  - `rec_get_deleted_flag(rec, compact)` 必须为 false；
  - 满足以上条件返回 `DB_SUCCESS`；
  - 任一条件不满足返回 `DB_UNSUPPORTED`；
  - 对 `!view->sees(page_max_trx_id)` 的 uncertain page，必须 whole-smoke
    fail-closed，不能 skip row 后继续，因为那会产生不完整结果；
  - 不产生 MySQL record，不调用 row sink，不增长
    `Parallel_secondary_rows_produced`；
  - B3a-1 新增 counter：
    `Parallel_secondary_visibility_supported` 表示 fast-path contract
    satisfied；
  - `Parallel_secondary_visibility_unsupported` 表示 fast-path contract
    failed closed，包括 uncertain page。

后续拆分：

- M9-B3a-1: 实现 fast-path helper 和 smoke counter，只验证 helper 可以
  fail-closed 或在受控场景返回 supported；
- M9-B3a-2: 设计/迁移 clustered lookup for visibility，新增
  `pq_row_sel_get_clust_rec_for_mysql()` wrapper 或等价接口；
- M9-B3a-3: covering secondary row materialization smoke；
- M9-B3b: debug-only covering secondary one-record materialization smoke；
- M9-B3c/B3d: 后续再评估用户可见 covering secondary range PQ gate。

M9-B3a-1 允许修改：

- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/m9-b3-secondary-range-row-production.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`

M9-B3a-1 禁止修改：

- `storage/innobase/row/row0sel.cc`
- `storage/innobase/include/row0sel.h`
- `sql/join_optimizer/access_path.cc`
- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_iterators.*`
- `sql/parallel_query/pq_clone.*`
- 用户可见 PQ execution gate

M9-B3a-1 Review gate:

- Review Agent 必须确认 fast-path 不会产生不完整结果；
- Review Agent 必须确认 uncertain page whole-smoke fail-closed；
- Review Agent 必须确认 clustered lookup 后置到 B3a-2；
- Review Agent 必须确认普通 secondary range/ref/ICP SELECT 仍 fallback。

### M9-B3a-2 Clustered Lookup For Visibility Design

目标：

- 为 secondary visibility helper 增加 clustered lookup fallback；
- 只服务 visibility/delete-mark 判定，不产生 MySQL record；
- 不接 ICP，不接 row sink，不增长 `Parallel_secondary_rows_produced`；
- 不打开用户可见 execution gate；
- 让 B3a-3 row materialization smoke 可以在 uncertain page 上安全失败或安全
  验证 visibility。

设计结论：

- B3a-2 需要暴露一个窄 wrapper，等价商用
  `pq_row_sel_get_clust_rec_for_mysql()`；
- 允许修改 `storage/innobase/include/row0sel.h` 和
  `storage/innobase/row/row0sel.cc`，但只新增 wrapper，不修改主
  `row_search_mvcc()` 流程；
- wrapper 签名沿用商用实现：
  - input: `row_prebuilt_t *prebuilt`, `dict_index_t *sec_index`,
    secondary `rec`, `que_thr_t *thr`, `mtr_t *mtr`；
  - output: `const rec_t **out_rec`, `ulint **offsets`,
    `mem_heap_t **offset_heap`, optional `const dtuple_t **vrow`；
  - mtr contract: caller must hold the secondary record latch in `mtr`; wrapper
    may use the same `mtr` to access the clustered index；
  - latch contract: returned clustered record is valid only while `mtr` remains
    active；
  - ownership: old-version memory / offsets use `offset_heap` and remain valid
    until caller frees/empties the heap；
- B3a-2 helper in `row0pread_pq.cc` must call the wrapper only when:
  - active read view exists；
  - secondary index；
  - `prebuilt->index == m_index == sec_index`；
  - table is not intrinsic；
  - `prebuilt->idx_cond == false`；
  - `prebuilt->select_lock_type == LOCK_NONE`；
  - `prebuilt->sel_graph` exists or can be built with `row_prebuild_sel_graph()`；
  - caller provides active `mtr_t *`；
- If `view->sees(page_max_trx_id)` and `prebuilt->need_to_access_clustered` is
  false, fast-path can return `DB_SUCCESS` without clustered lookup；
- fast-path success must still check `rec_get_deleted_flag(rec, compact) ==
  false` before returning `DB_SUCCESS`；
- If page is uncertain or `prebuilt->need_to_access_clustered` is true,
  clustered lookup is allowed only for visibility；
- clustered lookup result handling:
  - wrapper error -> `DB_UNSUPPORTED` for B3a-2 smoke；
  - `clust_rec == nullptr` -> `DB_NOT_FOUND` / row invisible；
  - delete-marked clustered record -> `DB_NOT_FOUND`；
- visible clustered record -> `DB_SUCCESS`；
- if clustered lookup takes an extra clustered latch, B3a-2 callers must either:
  - validate exactly one record and stop before advancing the secondary cursor；
  - or commit/restart `mtr` before continuing scan traversal；
- B3a-2 implementation must use the first option only: one-record smoke /
  one-record validation and stop after the visibility decision；
- continuous scan with commit/restart is deferred to B3a-3/B3b；
- B3a-2 smoke may count supported/invisible/unsupported visibility decisions,
  but must not enqueue rows or materialize MySQL record；
- uncertain page must no longer be guessed; it must either be resolved by
  clustered lookup or fail closed.

建议新增/使用 counters：

- keep `Parallel_secondary_visibility_attempts`；
- keep `Parallel_secondary_visibility_supported`；
- keep `Parallel_secondary_visibility_unsupported`；
- optionally add `Parallel_secondary_visibility_invisible` only if MTR needs to
  distinguish invisible rows from unsupported helper failures。

B3a-2 允许修改：

- `storage/innobase/include/row0sel.h`
- `storage/innobase/row/row0sel.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- this taskbook and progress docs

B3a-2 禁止修改：

- `sql/join_optimizer/access_path.cc`
- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_iterators.*`
- `sql/parallel_query/pq_clone.*`
- 用户可见 PQ execution gate

B3a-2 Review gate:

- Review Agent 必须确认 wrapper 不改变现有 `row_search_mvcc()` 行为；
- Review Agent 必须确认 mtr/latch lifetime 被写清楚；
- Review Agent 必须确认 B3a-2 is one-record-and-stop and does not continue a
  secondary scan after clustered lookup；
- Review Agent 必须确认 fast-path preserves secondary delete-mark check；
- Review Agent 必须确认 `prebuilt->index == sec_index/m_index` hard gate；
- Review Agent 必须确认 clustered lookup 只用于 visibility；
- Review Agent 必须确认 intrinsic table rejected；
- Review Agent 必须确认 no row materialization / no row sink / no execution
  gate；
- Review Agent 必须确认普通 secondary range/ref/ICP SELECT 仍 fallback。

Design review:

- Review Agent Chandrasekhar returned `ACCEPT WITH RISKS`；
- Required implementation constraints:
  - do not use `has_active_read_view()` alone before dereferencing read view；
  - require explicit active `m_trx->read_view`；
  - read-only mode without active read view is unsupported in B3a-1；
  - B3a-1 must keep `Parallel_secondary_rows_produced` at 0；
  - whole-smoke fail-closed conditions should return `DB_UNSUPPORTED`；
  - extend or rename the B3a-0 helper instead of silently changing a comment
    that says all secondary indexes are unsupported。

### M9-B3b Debug-only Covering Materialization Smoke

仅当 B3a 通过 review 后再做：

- 不放开用户可见 covering secondary PQ eligibility；
- 不修改 `access_path.cc`、iterator、row sink、`Query_result_mq` 或 worker
  secondary path；
- 只在 DBUG smoke 中验证一个 visible covering secondary record 可以
  materialize 到 MySQL record buffer 后立即丢弃；
- `Parallel_secondary_rows_produced` 必须保持 0。

### M9-B3c Non-covering Cluster Lookup

后置：

- secondary tuple 定位 clustered record；
- cluster lookup visible version；
- hidden PK / generated column / blob 边界；
- 与 ICP、ref path 明确隔离。

## 允许修改

M9-B3 Design-only 允许修改：

- `Docs/pq_tasks/m9-b3-secondary-range-row-production.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

M9-B3a 实现阶段建议允许修改：

- `sql/handler.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/mysqld.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- 本任务文档和总看板

## 禁止修改

M9-B3 Design-only 禁止源码修改。

M9-B3a 默认禁止：

- `sql/join_optimizer/access_path.cc`
- `sql/sql_select.cc`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_iterators.*`
- `sql/parallel_query/pq_clone.*`
- `sql/range_optimizer/*`
- 非 `pq_commercial_ref_icp` 的大规模测试迁移

如必须修改上述文件，停止编码，先更新设计并启动 Review Agent。

## 验收标准

Design-only 验收：

- 明确 B3 是否先做 covering-only；
- 明确 row materialization 路线；
- 明确 MVCC/deleted-mark 依据；
- 明确 boundary 不扩大；
- 明确 counter 增长时机；
- 明确 debug smoke 与用户可见 gate 的分界；
- Review Agent 同意后再进入 B3a 编码。

B3a 实现验收建议：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --record pq_commercial_ref_icp pq_stats
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a --tmpdir=/tmp/pqt_m9b3a
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a_full --tmpdir=/tmp/pqt_m9b3a_full
```

预期：

- B3a-1 debug smoke 下 `Parallel_secondary_visibility_supported` 可增长；
- B3a-1 仍要求 `Parallel_secondary_rows_produced` 保持 0；
- 普通 secondary range/ref/ICP SELECT 继续 fallback，除非任务明确进入 B3b；
- `Parallel_queries_executed` / `Parallel_workers_launched` 不因普通 fallback 用例增长；
- 完整当前 `parallel_query` suite 通过。

## Code Agent 任务书

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M9-B3 Design / M9-B3a Covering Secondary Range Row Smoke。

请确认当前 worktree 包含最新 baseline：
git log --oneline -5
git status --short

必读文件：
- AGENTS.md
- CLAUDE.md
- Docs/pq_tasks/README.md
- Docs/pq_tasks/commercial-port-gap-analysis.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- Docs/pq_tasks/m9-b0-secondary-range-design.md
- Docs/pq_tasks/m9-b2-secondary-range-partition.md
- Docs/pq_tasks/m9-b3-secondary-range-row-production.md
- storage/innobase/row/row0pread_pq.cc
- storage/innobase/handler/ha_innodb_pq.cc
- sql/parallel_query/pq_optimizer.cc
- 商用参考：/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/storage/innobase/row/row0pread_pq.cc
- 商用参考：/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/storage/innobase/handler/ha_innodb_pq.cc

任务目标：
1. 先完成 M9-B3 设计确认，不直接扩大执行入口；
2. 若设计确认 covering-only 可行，再实现 debug-only M9-B3a smoke；
3. 只支持 half-open forward covering secondary range；
4. 普通 secondary range/ref/ICP SELECT 继续 fallback；
5. 不接 JT_REF、dependent ref、ICP、reverse、partition table、non-covering cluster lookup。
6. 只支持固定长度、非 nullable、非 virtual/generated 的 covering key columns；
7. 若不能证明 secondary callback 已经过 MVCC/delete-mark 过滤，停止并回报，不实现 row smoke。

允许修改：
- Docs/pq_tasks/m9-b3-secondary-range-row-production.md
- Docs/pq_tasks/commercial-port-m9-ref-icp.md
- Docs/pq_tasks/commercial-port-gap-analysis.md
- Docs/pq_tasks/README.md
- sql/handler.h
- sql/parallel_query/pq_optimizer.cc
- sql/parallel_query/sql_parallel.h
- sql/parallel_query/sql_parallel.cc
- sql/mysqld.cc
- storage/innobase/handler/ha_innodb.h
- storage/innobase/handler/ha_innodb_pq.cc
- storage/innobase/include/row0pread_pq.h
- storage/innobase/row/row0pread_pq.cc
- mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test
- mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result
- mysql-test/suite/parallel_query/r/pq_stats.result

禁止修改：
- sql/join_optimizer/access_path.cc
- sql/sql_select.cc
- sql/sql_optimizer.cc
- sql/parallel_query/pq_iterators.*
- sql/parallel_query/pq_clone.*
- sql/range_optimizer/*
- unrelated docs/tests/build artifacts

验证：
- cmake --build build-ninja --target mysqld -j 16
- cd build-ninja/mysql-test
- TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a --tmpdir=/tmp/pqt_m9b3a
- TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a_full --tmpdir=/tmp/pqt_m9b3a_full

停止条件：
- covering-only 无法安全 materialize MySQL record；
- 无法证明 secondary callback 的 MVCC/delete-mark 语义；
- 需要扩大 range boundary 语义；
- 需要修改 access_path/sql_select/sql_optimizer 才能完成 smoke；
- 需要引入 non-covering cluster lookup；
- 需要 worker-side ICP Item 生命周期；
- 测试或 build 失败且无法定位到本任务范围。

完成后：
1. 把完成报告写入 Docs/pq_tasks/m9-b3-secondary-range-row-production.md；
2. 输出 changed files、实现说明、构建/测试结果、风险点；
3. 生成 patch 到主仓库，不要 commit。
```

## Review Agent 任务书

```text
请先阅读 AGENTS.md 和 CLAUDE.md。

你的角色是 Review Agent。
主控 Agent 是 Codex。
当前任务是 M9-B3 secondary range row production review。

Review 范围：
- 只检查 M9-B3 / M9-B3a diff；
- 对照 Docs/pq_tasks/m9-b3-secondary-range-row-production.md 的允许/禁止文件；
- 重点检查 covering 判定、secondary tuple materialization、MVCC/deleted-mark、half-open boundary、worker abort/error cleanup、普通 fallback 是否保持；
- 确认没有打开 JT_REF、ICP、reverse、partition、non-covering cluster lookup；
- 确认 `Parallel_secondary_rows_produced` 只在明确 smoke 或明确 gate 中增长。

输出：
- APPROVE 或 REQUEST_CHANGES；
- blocker 列表；
- 可后置风险；
- 已核对的验证命令与结果。
```

## Completion Report

Design revision completed by Codex Orchestrator after Review Agent Turing
returned `REVISE`.

Review findings addressed:

- Covering 判定从建议改为可执行 fail-closed 规则；
- B3a field type 收窄为固定长度、非 nullable、非 virtual/generated key columns；
- 明确 B3a 不允许 clustered lookup，即使只为 visibility；
- 明确若不能证明 `Parallel_reader` secondary callback 已处理 MVCC/delete-mark，B3a 停留 design-only；
- 明确 `Parallel_secondary_rows_produced` 只在 visibility passed、materialization succeeded、sink enqueue succeeded 后增长；
- 将 `sql/parallel_query/sql_parallel.cc` 加入 B3a 允许文件，匹配 row sink cleanup 语义。

Validation:

- Design-only；无源码改动；
- 未运行 build/MTR。

Second review:

- Review Agent Turing returned `ACCEPT WITH RISKS`；
- Critical findings: none；
- Previous blocker closed；
- Design is sufficient to enter M9-B3a as a debug-only implementation attempt；
- Remaining MVCC/deleted-mark uncertainty is intentionally a B3a hard stop
  condition；
- Note: this taskbook must be committed or otherwise preserved before Code Agent
  handoff, because it is a new file.

M9-B3a coding attempt:

- Codex checked current upstream `Parallel_reader` visibility path before source
  edits；
- `storage/innobase/row/row0pread.cc::Parallel_reader::check_visibility()`
  still calls `ut_error` for secondary indexes under active read view；
- `storage/innobase/include/row0pread.h` still documents secondary index scans
  as unsupported；
- `storage/innobase/row/row0pread_pq.cc::produce_callback_rows_for_range()`
  remains clustered-only；
- Per accepted design, B3a must stop if secondary callback visibility/delete-mark
  cannot be proven；
- No source edits were made for B3a row smoke；
- Next task: M9-B3a-0 Secondary Visibility Helper。

Blocked-result review:

- Review Agent Raman returned `ACCEPT`；
- It confirmed current upstream `Parallel_reader` secondary visibility is
  unsupported and unsafe for B3a row smoke；
- It confirmed there is no smaller safe bypass: a page-max-trx-id covering fast
  path would still require a new secondary visibility helper and fail-closed
  contract；
- Open design question for B3a-0: whether clustered lookup is allowed only for
  visibility, and whether uncertain pages should fail the whole smoke rather
  than skip rows.

M9-B3a-0 implementation:

- Added debug-only handler API `pq_secondary_visibility_smoke()`；
- Added `InnoDB_pq_scan_ctx::validate_secondary_visibility_contract()`；
- The helper intentionally returns `DB_UNSUPPORTED` for secondary indexes under
  an active read view until the commercial secondary visibility/delete-mark
  helper is migrated；
- Added `Parallel_secondary_visibility_attempts` and
  `Parallel_secondary_visibility_unsupported` status variables；
- `pq_optimizer.cc` triggers the visibility smoke only under
  `pq_secondary_visibility_smoke` DBUG flag and only for the same narrow
  secondary `INDEX_RANGE_SCAN` shape used by M9-B2 smoke；
- MTR verifies attempts +1, unsupported +1, rows produced 0；
- Ordinary secondary range/ref/ICP SELECT remains serial fallback。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --record pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a0 --tmpdir=/tmp/pqt_m9b3a0` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a0_full --tmpdir=/tmp/pqt_m9b3a0_full` passed, 74 tests successful。

Review:

- Review Agent Dewey returned `APPROVE`；
- Critical findings: none；
- Important findings: none；
- Minor packaging note: ensure this new taskbook is explicitly included in the
  final patch/commit and unrelated dirty files are excluded；
- Non-blocking wording note: visibility smoke does not re-check half-open range
  endpoint flags, but this is acceptable for B3a-0 because the helper reads no
  rows and always fails closed。

M9-B3a-1 design:

- Codex compared the current 8.0.46 InnoDB APIs with the commercial
  `find_visible_record()` implementation；
- Current branch lacks commercial `pq_row_sel_get_clust_rec_for_mysql()` public
  wrapper and keeps `row_search_idx_cond_check()` static；
- B3a-1 therefore selects a secondary visibility fast-path only；
- `view->sees(page_get_max_trx_id(page_align(rec)))` is required；
- uncertain pages fail the whole smoke closed instead of skipping rows；
- clustered lookup for visibility is deferred to B3a-2；
- no source code for B3a-1 implementation has been written yet。

Review:

- Review Agent Chandrasekhar returned `ACCEPT WITH RISKS`；
- Required constraints were written back before coding。

M9-B3a-1 implementation:

- Added `InnoDB_pq_scan_ctx::validate_secondary_visibility_fast_path()`；
- The helper explicitly requires `m_trx->read_view != nullptr` and
  `MVCC::is_view_active(m_trx->read_view)`；
- `srv_read_only_mode` without an active read view remains unsupported；
- The helper requires secondary index, `LOCK_NONE`, no ICP, no clustered access,
  matching prebuilt index, `view->sees(page_get_max_trx_id(page_align(rec)))`,
  and non delete-marked record；
- Uncertain pages return `DB_UNSUPPORTED` so a future smoke must fail the whole
  attempt instead of skipping rows；
- Added `Parallel_secondary_visibility_supported`；
- Current debug smoke has no safe secondary record source, so supported remains
  0 and unsupported remains 1；
- `Parallel_secondary_rows_produced` remains 0。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --record pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a1 --tmpdir=/tmp/pqt_m9b3a1` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a1_full --tmpdir=/tmp/pqt_m9b3a1_full` passed, 74 tests successful。

Review:

- Review Agent Curie returned `APPROVE`；
- Critical findings: none；
- Important findings: none；
- Scope hygiene: final patch/commit must exclude unrelated dirty/untracked
  files；
- Open question for B3a-2: decide whether
  `validate_secondary_visibility_fast_path()` or the clustered lookup helper
  should carry an explicit `mtr_t *` / latch contract instead of documenting a
  latched secondary record only in text。

M9-B3a-2 design:

- Codex selected the commercial wrapper approach for clustered lookup
  visibility；
- This requires a narrow `row0sel.h/.cc` wrapper equivalent to
  `pq_row_sel_get_clust_rec_for_mysql()`；
- The wrapper must not alter `row_search_mvcc()` behavior；
- The mtr/latch contract is explicit: caller provides active `mtr_t *` that
  protects the secondary record; returned clustered record is valid only while
  that mtr remains active；
- Clustered lookup is allowed only for visibility/delete-mark validation；
- No MySQL record materialization, no row sink, no execution gate。

Design review:

- Review Agent Hooke returned `REVISE`；
- Design revised to require secondary delete-mark check before fast-path
  success；
- Design revised to require `prebuilt->index == sec_index/m_index`；
- Design revised to reject intrinsic tables；
- Design revised to make B3a-2 one-record-and-stop only after clustered lookup；
- Continuous scan with mtr commit/restart is deferred。

Review:

- Review Agent Hooke returned `ACCEPT` on re-review；
- Design is ready to enter coding with the documented gates enforced in
  implementation。

M9-B3a-2 implementation:

- Added narrow `pq_row_sel_get_clust_rec_for_mysql()` wrapper in
  `row0sel.h/.cc`；
- Added `InnoDB_pq_scan_ctx::validate_secondary_visibility_with_cluster_lookup()`；
- The helper requires active read view, secondary index, `LOCK_NONE`, no ICP,
  matching `prebuilt->index == m_index`, matching `prebuilt->trx == m_trx`,
  non-intrinsic table, non-null `prebuilt->clust_pcur`, active `mtr_t *`, and
  caller-provided secondary record/offsets；
- Fast-path success still requires secondary delete-mark check；
- If fast-path is uncertain, the helper may perform clustered lookup only for
  visibility/delete-mark validation；
- Invisible or delete-marked clustered records return `DB_NOT_FOUND`；
- Unsupported or unsafe states return `DB_UNSUPPORTED`；
- The helper does not materialize MySQL records, does not enqueue rows, and does
  not open any secondary range execution gate；
- `pq_secondary_visibility_smoke()` now also fails closed for intrinsic tables。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a2_r2 --tmpdir=/tmp/pqt_m9b3a2_r2` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a2_full_r2 --tmpdir=/tmp/pqt_m9b3a2_full_r2` passed, 74 tests successful。

Review:

- Review Agent Parfit returned `APPROVE`；
- Critical findings: none；
- Important findings: none；
- Minor finding fixed: document the enforced `prebuilt->clust_pcur != nullptr`
  gate。

M9-B3a-3 one-record secondary visibility smoke design:

Goal:

- Prove the B3a-1/B3a-2 visibility helpers can be called with a real latched
  secondary record；
- Keep the path debug-only and contract-only；
- Do not materialize MySQL rows, do not enqueue rows, and do not open secondary
  range execution。

Scope:

- Add a debug-only one-record smoke path under a new DBUG flag, for example
  `pq_secondary_visibility_one_record_smoke`；
- Reuse the same narrow `INDEX_RANGE_SCAN` SQL-layer shape gates as M9-B2/B3a；
- Use an independent DBUG flag and handler API so the existing fail-closed
  `pq_secondary_visibility_smoke()` contract remains unchanged；
- Pass copied start/end key endpoints to InnoDB so the smoke is tied to the
  optimizer's chosen range；
- In InnoDB, convert endpoints to dtuple exactly like
  `pq_secondary_range_partition_smoke()`；
- Open a secondary cursor under one active `mtr_t` at the range start；
- Inspect at most one user record；
- Check end boundary before visibility validation；
- Build offsets for the secondary record；
- Call `validate_secondary_visibility_fast_path()` first and
  `validate_secondary_visibility_with_cluster_lookup()` only when the fast path
  is unsupported；
- Commit the `mtr_t` immediately after the one-record check and return；
- Do not continue scanning after clustered lookup because B3a-2 explicitly uses
  a one-record-and-stop latch contract。

Required gates:

- active read view with `trx->read_view != nullptr` and
  `MVCC::is_view_active(trx->read_view)`；
- secondary non-clustered, usable, non-corrupted index；
- `LOCK_NONE`；
- no ICP；
- `prebuilt->need_to_access_clustered` may use clustered lookup, but the smoke
  must still not materialize MySQL row data；
- `prebuilt->index == m_index`；
- `prebuilt->trx == m_trx`；
- non-intrinsic table；
- non-null `prebuilt->pcur` and `prebuilt->clust_pcur` before cursor work；
- start endpoint absent or `HA_READ_KEY_OR_NEXT`；
- end endpoint absent or `HA_READ_BEFORE_KEY`；
- no reverse scan and no inclusive upper-bound smoke；
- index-native end-boundary check before visibility validation；
- one-record-and-stop after any clustered lookup。

Return/counter semantics:

- handler success means "one-record visibility path executed" and SQL layer
  increments `Parallel_secondary_visibility_supported`；
- visible first record, no user record in range, and invisible/delete-marked
  first record are all success for B3a-3 smoke because the path executed and no
  row is produced；
- unsafe state, conversion failure, cursor error, or unsupported shape: handler
  returns unsupported and SQL layer increments
  `Parallel_secondary_visibility_unsupported`；
- `Parallel_secondary_rows_produced` remains 0 in all B3a-3 cases。

Forbidden in B3a-3:

- no `Query_result_mq` integration；
- no worker thread secondary range execution；
- no ICP check migration；
- no `row_sel_store_mysql_rec()`；
- no row sink callback；
- no repeated page/cursor traversal；
- no reuse of an mtr after clustered lookup for continued secondary scanning。

Validation plan:

- Add/extend MTR debug smoke in `pq_commercial_ref_icp` for a deterministic
  visible first secondary record；
- Assert visibility attempts increases by both the fail-closed smoke and the
  one-record smoke when both DBUG flags are enabled；
- Assert visibility supported +1 for the one-record smoke；
- Assert visibility unsupported +1 from the existing fail-closed
  `pq_secondary_visibility_smoke` when both DBUG flags are enabled；
- Assert `Parallel_secondary_rows_produced` remains 0；
- Run targeted `pq_commercial_ref_icp pq_stats pq_not_support`；
- Run full `parallel_query` suite。

Design review:

- Review Agent Descartes returned `ACCEPT WITH RISKS`；
- Important findings fixed before coding: clarify supported counter semantics
  and make range/cursor boundary gates explicit；
- Minor finding fixed: range boundary is now in the required gates list。

M9-B3a-3 implementation:

- Added handler API `pq_secondary_visibility_one_record_smoke()`；
- Added SQL-layer DBUG hook `pq_secondary_visibility_one_record_smoke`；
- The new hook reuses the same narrow secondary `INDEX_RANGE_SCAN` shape gates
  and copied endpoint handling as M9-B2/B3a；
- Added
  `InnoDB_pq_scan_ctx::validate_one_secondary_record_for_smoke()`；
- InnoDB opens at most one secondary cursor position, checks exclusive end
  boundary before visibility, calls B3a-1/B3a-2 visibility helpers, closes
  cursors, commits the mtr, and returns；
- Handler temporarily binds `m_prebuilt->index` to the target secondary index
  while the debug smoke runs, then restores it；
- No MySQL row materialization, no row sink, no `Query_result_mq`, no worker
  secondary execution gate；
- `Parallel_secondary_visibility_supported` means the one-record visibility
  path executed；
- `Parallel_secondary_rows_produced` remains 0。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --record pq_commercial_ref_icp`
  passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_ref_icp pq_stats pq_not_support --parallel=1 --vardir=/tmp/pqv_m9b3a3 --tmpdir=/tmp/pqt_m9b3a3` passed；
- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m9b3a3_full --tmpdir=/tmp/pqt_m9b3a3_full` passed, 74 tests successful。

Review:

- Review Agent Mencius returned `APPROVE`；
- Critical findings: none；
- Important findings: none；
- Minor finding fixed: align validation-plan wording with the MTR that enables
  both the fail-closed and one-record visibility smoke flags。

M9-B3b covering secondary one-record materialization design:

Goal:

- Prove a visible secondary index record can be converted into MySQL row format
  for a covering secondary scan；
- Keep the first production step debug-only and one-record-only；
- Do not enable worker-thread secondary range execution or normal optimizer PQ
  eligibility for secondary/ref/ICP plans。

Why covering-only first:

- Current B3a helpers prove visibility for a latched secondary record；
- Current branch still lacks commercial `PQ_PCursor` continuous secondary
  traversal, mtr commit/restart protocol, and row cache handling；
- `row_sel_store_mysql_rec()` can materialize a secondary record only when the
  query is covering and no clustered columns are needed；
- Non-covering secondary row production must wait until the clustered record
  selected by visibility lookup can be safely used for MySQL record
  materialization under a documented mtr lifetime。

Scope:

- Add a new debug-only handler API and SQL DBUG flag, for example
  `pq_secondary_covering_one_row_smoke`；
- Reuse B3a-3 SQL gates: single `INDEX_RANGE_SCAN`, no reverse/geometry, no
  ICP, one range, non-primary secondary, non-partitioned table, unsupported key
  parts rejected；
- Require covering secondary scan:
  - `prebuilt->need_to_access_clustered == false`；
  - `prebuilt->read_just_key == true` or an equivalent existing covering
    signal must be proven before coding；
  - no BLOB/external-column materialization through clustered record；
- Reuse copied start/end endpoints and open at most one secondary record；
- Reuse B3a visibility helper before materialization；
- Call `row_sel_store_mysql_rec()` only after visibility succeeds；
- Use `rec_clust=false` and pass the secondary `m_index`/`prebuilt->index`
  consistently；
- Do not call row sink in B3b design unless the review explicitly accepts a
  debug-only in-memory sink；
- Prefer a smoke that materializes into `m_prebuilt->m_mysql_table->record[0]`
  and then discards the row；
- `Parallel_secondary_rows_produced` must remain 0 unless a row sink enqueue is
  introduced and succeeds in a later sub-stage。

Required gates:

- active read view；
- secondary non-clustered usable non-corrupted index；
- `LOCK_NONE`；
- no ICP；
- matching `prebuilt->index == m_index`；
- matching `prebuilt->trx == m_trx`；
- non-intrinsic table；
- non-null `prebuilt->pcur` and `prebuilt->clust_pcur`；
- covering-only, no clustered access；
- one-record-and-stop；
- end-boundary check before visibility/materialization；
- mtr committed immediately after the one-record check。

Forbidden in B3b:

- no continuous secondary range scan；
- no non-covering materialization from clustered record；
- no ICP migration；
- no normal execution gate for secondary/ref/ICP plans；
- no worker-thread secondary execution；
- no `Query_result_mq` production path；
- no `Parallel_secondary_rows_produced` growth unless a later reviewed
  sub-stage adds a real sink enqueue contract。

Acceptance tests:

- Add/extend a debug-only MTR case for a covering `SELECT k ... WHERE k >= ...
  AND k < ...`；
- Verify the smoke path succeeds with `Parallel_secondary_visibility_supported`
  or a new materialization-specific status counter；
- Verify ordinary secondary/ref/ICP SELECT remains serial fallback；
- Verify `Parallel_secondary_rows_produced` remains 0 for B3b；
- Run `pq_commercial_ref_icp pq_stats pq_not_support`；
- Run full `parallel_query` suite。

Resolved design decisions before coding:

- B3b remains debug-only. It must not change normal secondary/ref/ICP
  eligibility and must not modify `access_path.cc`, iterator execution,
  row sink, `Query_result_mq`, or worker secondary execution；
- Covering gate is a combined gate:
  - `prebuilt->index == m_index`；
  - `!m_index->is_clustered()`；
  - `prebuilt->trx == m_trx`；
  - `prebuilt->need_to_access_clustered == false`；
  - `prebuilt->idx_cond == false`；
  - `prebuilt->read_just_key == true`；
  - `prebuilt->n_template > 0`；
  - every `mysql_template[i]` is safe for the first smoke:
    `!is_virtual`, `!is_multi_val`, `rec_field_no != ULINT_UNDEFINED`, no
    prefix key field, `mysql_null_bit_mask == 0`,
    `mysql_type != DATA_MYSQL_TRUE_VARCHAR`, `mbminlen == mbmaxlen`, and no
    `DATA_LARGE_MTYPE(type)` / `DATA_GEOMETRY_MTYPE(type)` column。
- `row_sel_store_mysql_rec()` call contract is fixed:
  - call only after B3a visibility returns `DB_SUCCESS`；
  - `rec_clust=false`；
  - `rec_index == prebuilt_index == m_index`；
  - offsets must come from the same latched secondary record；
  - mtr must remain active for the whole call；
  - pass `prebuilt->blob_heap`, but B3b must fail closed before BLOB/TEXT/JSON
    templates so the blob path is not exercised in the first smoke。
- B3b may write `m_prebuilt->m_mysql_table->record[0]` and then discard it
  only inside the DBUG smoke. The smoke must restore `m_prebuilt->index`, close
  `pcur`/`clust_pcur`, commit mtr, and leave executed/workers counters
  unchanged；
- Add a materialization-specific smoke counter:
  `Parallel_secondary_rows_materialized_smoke`；
- `Parallel_secondary_rows_produced` remains 0 because no row sink enqueue
  exists in B3b；
- Explicit fail-closed exclusions: virtual/generated columns, nullable columns,
  varlen/prefix keyparts, BLOB/TEXT/JSON/geometry, descending/spatial/MVI keys,
  ICP, non-covering access, FTS doc id, and read-only mode without active read
  view。

Design review:

- Review Agent Harvey returned `REVISE`；
- Critical findings:
  - Earlier B3b wording still implied a user-visible covering gate, but the
    current B3b target must remain debug-only one-record materialization；
  - Covering gate and counter strategy must be decided in the design instead
    of left as open questions before coding；
- Important required revisions:
  - Covering gate must be a combination of matching secondary prebuilt/index,
    `need_to_access_clustered == false`, `idx_cond == false`,
    `read_just_key == true`, non-empty safe templates, and fail-closed
    template/key-part checks；
  - `row_sel_store_mysql_rec()` call contract must explicitly require
    `rec_clust=false`, `rec_index == prebuilt_index == m_index`, offsets from
    the same latched secondary record, and active mtr lifetime；
  - B3b should add a materialization-specific smoke counter instead of
    overloading visibility counters；
  - Writing `record[0]` is acceptable only inside the DBUG smoke and must not
    change execution state or increase executed/workers/rows-produced counters；
- Minor required revisions:
  - Explicitly reject virtual/generated, nullable, varlen/prefix keypart,
    BLOB/TEXT/JSON/geometry, descending/spatial/MVI, ICP, non-covering, FTS doc
    id, and read-only mode without active read view；
- Revision applied by Codex:
  - user-visible gate wording moved out of B3b；
  - covering gate resolved as a combined prebuilt/index/template gate；
  - `row_sel_store_mysql_rec()` contract resolved；
  - `Parallel_secondary_rows_materialized_smoke` selected as the B3b
    materialization counter；
  - fail-closed exclusions expanded；
- Current B3b status: revised design ready for Design Review Agent; no B3b
  coding started。

B3b design re-review:

- Review Agent Einstein returned `ACCEPT WITH RISKS`；
- Critical findings: none；
- Important findings: none；
- Minor findings: none；
- Residual risk accepted: the positive smoke can only increment the
  materialization counter if `prebuilt->mysql_template` and
  `prebuilt->read_just_key` are already ready for the covering scan at the
  debug hook point. If not, B3b must fail closed instead of relaxing gates；
- Current B3b status: design accepted for coding。

B3b coding status:

- Implemented debug-only `pq_secondary_covering_one_row_smoke` path；
- Added `Parallel_secondary_rows_materialized_smoke` status counter；
- SQL hook remains DBUG-only and does not change normal secondary/ref/ICP
  eligibility；
- Positive smoke is limited to one secondary range record and writes only to
  `m_prebuilt->m_mysql_table->record[0]` before discarding the row；
- `Parallel_secondary_rows_produced` remains 0 because B3b has no row sink
  enqueue contract。

Code review round 1:

- Review Agent Maxwell returned `REVISE`；
- Critical issue: the SQL covering read-set gate was not applied to the B3b
  covering smoke path, so non-covering DBUG cases could reach InnoDB；
- Important issue: generated/hidden/functional-index columns were not rejected
  by the SQL gate；
- Important issue: InnoDB handler temporarily rebuilt `m_prebuilt` template but
  restored only `index` and `read_just_key`, which was insufficient because
  `build_template(false)` mutates additional template fields。

Revisions applied:

- Moved `pq_secondary_covering_read_set_is_safe()` into
  `pq_maybe_run_secondary_covering_one_row_smoke()` and removed it from the
  one-record visibility smoke；
- Extended SQL field gate to reject generated, hidden, and functional-index
  fields；
- Handler now snapshots and restores `m_prebuilt->index`, `read_just_key`,
  `template_type`, `n_template`, `null_bitmap_len`,
  `need_to_access_clustered`, `templ_contains_blob`,
  `templ_contains_fixed_point`, `mysql_prefix_len`, `idx_cond_n_cols`,
  `keep_other_fields_on_keyread`, `in_fts_query`, `m_end_range`, and the
  `mysql_template` buffer contents/pointer；
- Added DBUG negative MTR cases for `SELECT *` and `SELECT v` through `k_idx`
  while the covering smoke flag is enabled。

Code review round 2:

- Review Agent Maxwell returned `REVISE`；
- Critical issue: the first revision still had the covering read-set gate in
  `pq_maybe_run_secondary_visibility_one_record_smoke()` instead of the actual
  B3b covering smoke；
- Important issue: the negative MTR cases still allowed ICP to mask the missing
  covering gate；
- Second revision moved the gate to
  `pq_maybe_run_secondary_covering_one_row_smoke()` and removed it from
  one-record visibility smoke；
- Second revision disables ICP around the DBUG non-covering `SELECT *` and
  `SELECT v` negative cases, proving the read-set covering gate itself。

Validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query --record
  pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query
  pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query` passed, 74/74；
- Key counter result in `pq_commercial_ref_icp` after round 2:
  `secondary_range_clone_attempts_delta=3`,
  `secondary_range_clone_failed_delta=0`,
  `secondary_ranges_built_delta=3`,
  `secondary_rows_materialized_smoke_delta=1`,
  `secondary_visibility_attempts_delta=9`,
  `secondary_visibility_supported_delta=3`,
  `secondary_visibility_unsupported_delta=5`。

Current B3b status:

- Code and tests are implemented；
- Review Agent Maxwell final re-review returned `ACCEPT`；
- Previous blockers closed:
  - covering read-set gate is now on the covering materialization smoke path；
  - one-record visibility smoke no longer has the covering read-set gate；
  - DBUG non-covering negative cases run with ICP off；
- M9-B3b is complete and ready for the next M9-B3 sub-stage。

### M9-B3c Debug-only Covering Secondary Range Materialization Drain

Design review:

- Review Agent Socrates returned `REVISE`；
- Critical finding: B3c cannot jump directly from B3b one-record
  materialization to continuous secondary cursor draining because the current
  branch does not yet define cursor advance, mtr commit/restart, reposition /
  bookmark semantics, offsets heap lifetime, and clustered lookup continuation
  rules；
- Required split: add `M9-B3c-0 Secondary Cursor Drain Contract` before any
  B3c source coding。

Current status:

- B3c is blocked for coding until B3c-0 is reviewed；
- No B3c source code changes are allowed yet。

### M9-B3c-0 Secondary Cursor Drain Contract

Goal:

- Define the minimum contract required before a debug-only multi-row secondary
  cursor drain can be implemented；
- Keep this subtask design-only unless a later review explicitly accepts a
  bounded helper implementation；
- Resolve the cursor/mtr/visibility continuation rules that B3b intentionally
  avoided by being one-record-and-stop。

B3c-0 mechanical contract:

- Cursor advance:
  - The first B3c implementation may use only `prebuilt->pcur` on the target
    secondary index；
  - It opens the cursor exactly like B3b: `open_on_user_rec(..., PAGE_CUR_GE,
    BTR_SEARCH_LEAF, &mtr, ...)` when a start tuple exists, otherwise
    `begin_leaf()` plus `move_to_next_user_rec()`；
  - After a record is accepted and materialized, the only allowed advance is
    `prebuilt->pcur->move_to_next_user_rec(&mtr)`；
  - End-boundary comparison is performed before every visibility check and
    before every materialization；
  - `DB_END_OF_INDEX` or first record beyond end boundary means normal success
    with the exact count already materialized。
- MTR lifetime:
  - B3c keeps one mtr open for the whole bounded fast-path-only drain；
  - B3c does not commit/restart the mtr during the drain；
  - Because there is no restart, B3c has no bookmark/reposition protocol；
  - This is only acceptable because B3c fast-path-only never calls
    `validate_secondary_visibility_with_cluster_lookup()` and therefore never
    takes the extra clustered latch called out by the B3a-2 one-record
    contract；
  - If a future design wants clustered lookup during a drain, it must stop
    scanning after that one record or define a separate continuation-safe
    mtr/reposition protocol。
- Reposition/bookmark semantics:
  - No restart means no bookmark or tuple restart is used in B3c；
  - B3c must not synthesize restart-after boundaries from the current secondary
    key tuple；
  - B3c must not copy or restore physical cursor positions；
  - If the current mtr cannot stay open for the configured bounded cap, B3c
    must fail closed rather than restart。
- Offsets/heap lifetime:
  - Offsets are recomputed for each candidate record with `rec_get_offsets()`
    from a per-record heap；
  - Offsets and any per-record heap allocations must not survive
    `move_to_next_user_rec()`；
  - The helper must call `mem_heap_empty(heap)` after each accepted or rejected
    candidate before advancing；
  - The heap is freed once at final cleanup；
  - If `rec_get_offsets()` fails or memory allocation fails, the whole smoke
    fails closed with drained count 0。
- Visibility:
  - B3c chooses fast-path-only drain；
  - Only rows passing `validate_secondary_visibility_fast_path()` may be
    materialized；
  - B3c must not call `validate_secondary_visibility_with_cluster_lookup()`；
  - Any `DB_UNSUPPORTED`, uncertain page, delete-mark, or condition that would
    require clustered lookup fails the whole smoke closed；
  - The safety rationale is that fast-path-only does not acquire a clustered
    latch, so the single secondary mtr can cover the bounded drain。
- Invisible rows:
  - `DB_NOT_FOUND` during a drain fails closed in B3c；
  - No partial counter increment is allowed after invisible, unsupported, OOM,
    cursor error, `row_sel_store_mysql_rec()` failure, or any unexpected
    handler/InnoDB error。
- Bounded cap:
  - Cap hit must return unsupported/fail closed；
  - Handler must set drained row count to 0 on cap hit, OOM, cursor error,
    `rec_get_offsets()` failure, `row_sel_store_mysql_rec()` failure,
    unsupported state, or any partial drain；
  - SQL layer must increment
    `Parallel_secondary_rows_materialized_smoke` only when handler returns
    success with an exact drained row count。
- State restoration:
  - B3c must snapshot and restore the same B3b `m_prebuilt` state set:
    `index`, `read_just_key`, `template_type`, `n_template`,
    `null_bitmap_len`, `need_to_access_clustered`, `templ_contains_blob`,
    `templ_contains_fixed_point`, `mysql_prefix_len`, `idx_cond_n_cols`,
    `keep_other_fields_on_keyread`, `in_fts_query`, `m_end_range`, and the
    `mysql_template` pointer/content；
  - Cursor cleanup must close both `pcur` and `clust_pcur` on every exit path；
  - mtr must be committed on every exit path。

Recommended B3c-0 conclusion:

- Choose fast-path-only drain for the first B3c implementation；
- Do not use `validate_secondary_visibility_with_cluster_lookup()` in a
  continuous drain；
- If any candidate row needs clustered lookup or returns `DB_NOT_FOUND`,
  fail the whole smoke closed；
- Keep one mtr open for the bounded drain, do not restart, do not bookmark, and
  recompute offsets per record；
- This deliberately limits B3c to stable read-view/page-max-trx-id cases, but
  avoids unsafe continuation after clustered latch acquisition。

B3c-0 acceptance output:

- Update this taskbook with the selected cursor/mtr/visibility policy；
- Start a Design Review Agent；
- Only after Review Agent returns `ACCEPT`, proceed to B3c coding。

B3c-0 design review:

- Review Agent Tesla returned `ACCEPT`；
- Critical findings: none；
- Important findings: none；
- Accepted coding constraints:
  - fast-path-only drain；
  - one bounded mtr；
  - no restart/bookmark/reposition；
  - no clustered lookup while draining；
  - per-record offsets/heap reset；
  - any unsupported/error/cap hit returns drained count 0 and no SQL counter
    increment。

Goal:

- Advance from B3b one-record materialization to a bounded multi-record drain
  for the same strict covering secondary range shape；
- Keep the path debug-only；
- Do not enable user-visible secondary/ref/ICP PQ execution；
- Do not enqueue rows to `Query_result_mq` or `PQ_row_sink` in this stage；
- Reuse `Parallel_secondary_rows_materialized_smoke` as a smoke-only
  materialized-row counter, this time increasing by the number of visible
  covering secondary records drained from the bounded range。

Scope:

- Add a new DBUG flag, for example
  `pq_secondary_covering_range_materialize_smoke`；
- Add a new handler API or extend the B3b handler API only if the output row
  count contract remains explicit；
- SQL hook uses the exact B3b gate:
  - single `INDEX_RANGE_SCAN`；
  - secondary non-primary key；
  - one forward range；
  - non-partitioned table；
  - no ICP；
  - no reverse/geometry；
  - no unsupported key parts；
  - `pq_secondary_covering_read_set_is_safe(table, keyno)` must pass；
- InnoDB loop opens the secondary cursor at the copied start endpoint, checks
  the end endpoint before each materialization, validates visibility, then
  calls `row_sel_store_mysql_rec()` for each visible row；
- The first implementation must be bounded, for example a small debug-only max
  row count such as 64. Hitting the cap must make the handler return
  unsupported with drained count 0；SQL must not increment any materialization
  counter on cap hit；
- Use one smoke result counter update at SQL layer after handler success:
  add the exact drained row count to
  `Parallel_secondary_rows_materialized_smoke`；
- Keep `Parallel_secondary_rows_produced` at 0。

Required InnoDB gates:

- Same gates as B3b: active read view, secondary non-clustered usable index,
  `LOCK_NONE`, no ICP, no non-covering clustered materialization,
  `need_to_access_clustered == false`, non-intrinsic table, matching
  `prebuilt->index`/`trx`, safe template, and non-null cursors；
- Handler must snapshot and restore the full B3b `m_prebuilt` state set named
  in B3c-0；
- Cursor/mtr/offset lifetime must remain valid for each
  `row_sel_store_mysql_rec()` call；
- If visibility returns uncertain/unsupported for any candidate row, the whole
  smoke fails closed and does not increment the materialized counter；
- Until B3c-0 accepts a continuation-safe clustered lookup protocol, B3c must
  use fast-path-only visibility and must not call clustered lookup while
  draining；
- If a candidate row is delete-marked, invisible, uncertain, or would require
  clustered lookup, the whole smoke fails closed with drained count 0；
- End boundary must be checked before visibility/materialization for each row。

Forbidden:

- No normal `INDEX_RANGE_SCAN` iterator factory changes；
- No `PQRefIterator`；
- No dependent ref；
- No ICP；
- No non-covering clustered record materialization；
- No reverse scan, descending keypart, MVI, spatial, partition table, virtual /
  generated / hidden fields, nullable or varlen fields；
- No worker thread secondary execution；
- No `Query_result_mq` send path；
- No user-visible result correctness claim。

Acceptance tests:

- Extend `pq_commercial_ref_icp` DBUG block with one covering range whose
  expected visible covering rows are deterministic, for example
  `SELECT k FROM pq_ref_icp_t1 FORCE INDEX(k_idx) WHERE k >= 20 AND k < 40`；
- Expected materialized smoke delta should include:
  - B3b one-record positive: `+1`；
  - B3c range-drain positive: `+3` for current fixture rows
    `(20,20,30)`；
  - DBUG non-covering negatives: `+0`；
- Must use separate before/after status windows for B3b-only and B3c-only smoke
  so one counter cannot mask the other；
- Non-covering B3c negative tests must run with ICP off, matching the B3b
  negative pattern；
- Add a cap-hit fail-closed test if the debug cap can be set low enough without
  adding a new user-visible variable；otherwise document cap-hit testing as
  deferred in the completion report and require code review to manually verify
  cap-hit returns unsupported with drained count 0 and SQL does not increment
  the materialization counter；
- Verify `Parallel_secondary_rows_produced` remains 0；
- Verify ordinary secondary/ref/ICP SELECT still reports
  `Not parallel NON_FULL_TABLE_SCAN` and does not execute PQ；
- Run:
  - `cmake --build build-ninja --target mysqld -j 16`；
  - `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query --record
    pq_commercial_ref_icp pq_stats`；
  - `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query
    pq_commercial_ref_icp pq_stats`；
  - full `parallel_query` suite。

Review gate before coding:

- Review Agent must confirm B3c does not accidentally become a user-visible
  secondary range execution gate；
- Review Agent must confirm the bounded drain cannot report partial success；
- Review Agent must confirm cursor/mtr/template state restoration is explicit；
- Review Agent must confirm tests distinguish one-record B3b from range-drain
  B3c and keep non-covering negatives fail-closed。

Current B3c status:

- Initial design/task split drafted, rejected once, then revised via B3c-0；
- B3c-0 contract review returned `ACCEPT`；
- Fast-path-only B3c coding completed by Codex Orchestrator；
- Code/task Review Agent Linnaeus returned `ACCEPT`；
- M9-B3c is complete。

B3c implementation notes:

- Added debug-only handler API `pq_secondary_covering_range_smoke()`；
- Added SQL DBUG flag `pq_secondary_covering_range_materialize_smoke`；
- SQL hook reuses the B3b strict covering read-set gate and increments
  `Parallel_secondary_rows_materialized_smoke` only when handler returns
  success with an exact row count；
- InnoDB helper `materialize_secondary_range_for_smoke()` uses the B3c-0
  contract:
  - one bounded mtr；
  - no restart/bookmark/reposition；
  - `prebuilt->pcur->move_to_next_user_rec(&mtr)` is the only advance；
  - fast-path-only visibility；
  - no clustered lookup；
  - per-record offsets recomputation and `mem_heap_empty()` before advance；
  - cap/error/unsupported paths reset row count to 0；
- The B3c smoke cap is a hard-coded debug-only `kMaxSmokeRows = 64` in the
  handler；
- Cap-hit MTR is deferred because there is no user-visible or session debug
  variable to lower the cap without adding new surface area. Code review must
  manually verify cap-hit returns unsupported, row count 0, and SQL does not
  increment the materialization counter。

B3c validation:

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query --record
  pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query
  pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl mysql-test-run.pl --suite=parallel_query` passed, 74/74；
- Key MTR counters:
  - `b3b_materialized_smoke_delta = 1`；
  - `b3c_materialized_smoke_delta = 3`；
  - overall `secondary_rows_materialized_smoke_delta = 4`；
  - `secondary_rows_produced_delta = 0`。

B3c code/task review:

- Review Agent Linnaeus returned `ACCEPT`；
- Critical findings: none；
- Important findings: none；
- Minor finding: worktree has many unrelated dirty/untracked files, so a future
  commit/patch must include only intended PQ/debug-smoke files；
- Review confirmed B3c remains debug-only, does not open user-visible
  secondary/ref/ICP PQ execution, does not connect `Query_result_mq` /
  `PQ_row_sink`, uses fast-path-only visibility, keeps one mtr with no
  restart/bookmark, recomputes offsets per record, and resets row count to 0 on
  unsupported/error/cap paths。

## M9-B3d 用户可见 covering secondary range gate

状态：

- Codex Orchestrator 已实现；
- Review Agent 初审提出 1 个 HIGH、1 个 MEDIUM；第一次修复把 hook 收得过窄
  导致 FILTER-wrapped 正例被禁用；Codex Orchestrator 已改为显式 job 标记
  传播并重新验证；Review Agent 最终复核返回 `ACCEPT`；
- 本阶段只打开极窄用户可见正例，不打开 ref / ICP / non-covering
  secondary range；
- 运行时 InnoDB fast-path 不满足时回退串行 `IndexRangeScanIterator`，避免把
  fast-path uncertainty 暴露为用户错误。

目标：

- 将 B3c 的 covering secondary range materialization 从 debug-only smoke
  推进到一个最小用户可见查询；
- 正例为当前 fixture 中的：
  `SELECT k FROM pq_ref_icp_t1 FORCE INDEX(k_idx) WHERE k >= 20 AND k < 40`；
- 结果应为 `20, 20, 30`；
- `Parallel_queries_executed` 增长 1；
- `Parallel_secondary_rows_produced` 增长 3；
- `Parallel_workers_launched`、`Parallel_ranges_built`、
  `Parallel_ranges_dispatched` 不增长；
- `Parallel_secondary_rows_materialized_smoke` 在运行期不增长。

用户可见 gate：

- `parallel_query=ON`；
- `parallel_query_experimental_threaded_dop=ON`；
- 非 `EXPLAIN`；
- 非 PQ worker THD；
- 普通 `SQLCOM_SELECT`；
- simple query block；
- 单表计划；
- 无 `ORDER BY`、无显式 `GROUP BY`、无 `HAVING`；
- `AccessPath::INDEX_RANGE_SCAN`；
- 单个 forward range；
- 非 geometry / reverse；
- InnoDB；
- 非 partition table；
- secondary non-primary key；
- 无 `pushed_idx_cond`；
- 非 spatial / MVI / descending keypart；
- optimizer 估算行数不超过 64；
- SQL `read_set` 只包含目标 secondary index 的完整 user-defined keypart；
- 第一版只允许固定长度、非 nullable、非 hidden、非 generated、非
  functional-index 的整数列；
- nullable、varlen、blob/text、bit、prefix keypart、hidden/generated 字段、
  非覆盖列全部 fail-closed。

实现说明：

- `sql/join_optimizer/access_path.cc` 在普通 forward `INDEX_RANGE_SCAN`
  分支内调用 `TryCreatePQSecondaryCoveringRangeIterator()`；
- 该 factory 只允许顶层 ordinary range scan 或顶层 `FILTER` 直接包裹的
  ordinary range scan 调用；
- `access_path.cc` 用 `IteratorToBeCreated::allow_pq_secondary_range` 显式携带
  允许标记：
  - 顶层 job 为 true；
  - 只有 `FILTER` 向 child 传播；
  - `INDEX_MERGE` / `ROWID_INTERSECTION` / `ROWID_UNION` 等 composite parent 的
    child range scan 不传播，避免替换 `RowIDCapableRowIterator` child；
- geometry / reverse 分支保持原路径；
- factory 不复用 `join->pq_eligible`，因为当前 conservative eligibility 会把
  secondary range 标为 `NON_FULL_TABLE_SCAN`；B3d 用自身极窄 gate 控制；
- factory 同时校验：
  - 目标 secondary key 的全部 user-defined keypart 都是固定非空安全整型
    full keypart；
  - SQL `read_set` 中的字段也必须由目标 secondary key 覆盖；
  这避免 range 定位 keypart 未被投影时绕过字段安全检查；
- `PQSecondaryCoveringRangeIterator::Init()` 调用 handler
  `pq_secondary_covering_range_produce()`；
- handler 返回 `HA_ERR_UNSUPPORTED` 时，iterator 构造并初始化串行
  `IndexRangeScanIterator`，同时设置 `FALLBACK_SERIAL` 和 fallback counter；
- handler 成功时，SQL-owned `PQ_row_sink` 复制 `TABLE::record[0]` 的固定
  record image 到本地 buffer，`Read()` 再逐行拷回 leader table record；
- 成功路径计入 `EXECUTED`、`Parallel_queries_executed`、
  `Parallel_secondary_rows_produced` 和 `Parallel_rows_scanned`；
- InnoDB producer 复用 B3c fast-path-only drain 语义，仍然不做 clustered
  lookup，不做 ICP，不启动 worker，不写 `Query_result_mq`。

验证：

- `cmake --build build-ninja --target mysqld -j 16` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp pq_stats` passed；
- `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query` passed, 74/74。

Review fix validation:

- Review finding HIGH: generic `INDEX_RANGE_SCAN` hook might replace
  composite rowid/index-merge child range scan；
  - First fix used `job.destination == &ret` and was too strict because normal
    `WHERE` range may be wrapped by `FILTER`；
  - Final fix uses `allow_pq_secondary_range` job metadata: top-level true,
    `FILTER` propagates, composite children remain false；
- Review finding MEDIUM: unsafe range keypart that is not projected could skip
  `read_set` checks；
  - Fix: added whole-key `pq_secondary_key_parts_are_safe()`；
  - Test: added `k_pad_idx(k, pad)` negative query using unsafe `CHAR` keypart
    while projecting only `k`；
- Re-validation after fix:
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query --record pq_commercial_ref_icp pq_stats` passed；
  - `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_ref_icp pq_stats` passed；
  - `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query` passed, 74/74。
- Final result check after root/FILTER propagation fix:
  - runtime positive remains active:
    `executed_delta = 1` and `secondary_rows_produced_runtime_delta = 3`；
  - unsafe `k_pad_idx(k, pad)` negative does not add extra executed/produced
    counter deltas。

Final review:

- Review Agent Hume returned `ACCEPT`；
- Closed findings:
  - composite `INDEX_RANGE_SCAN` child replacement risk；
  - unsafe non-projected keypart safety gap；
- Review confirmed `allow_pq_secondary_range` only propagates through `FILTER`,
  composite rowid/index-merge child jobs remain false, the ordinary `WHERE`
  range positive still executes, and the unsafe `k_pad_idx(k, pad)` negative
  does not add extra executed/produced counters。

关键 MTR 观察：

- 运行期 covering range 正例返回 3 行：`20, 20, 30`；
- `executed_delta = 1`；
- `workers_delta = 0`；
- `ranges_built_delta = 0`；
- `ranges_dispatched_delta = 0`；
- `secondary_rows_produced_runtime_delta = 3`；
- `secondary_rows_materialized_runtime_delta = 0`；
- 全窗口 `secondary_rows_produced_delta = 3`；
- debug smoke 全窗口仍保持 `secondary_rows_materialized_smoke_delta = 4`；
- ref、ICP、primary range、non-covering、multi-table 路径仍由现有负例覆盖。

残余风险：

- 这是 bounded first user-visible gate，不是完整商用 secondary/ref/ICP
  实现；
- 结果集 cap 暂为 handler 内部 64 行；factory 同步用 optimizer
  estimated rows 做前置保护，真实超 cap 时 runtime 会回退串行；
- fast-path visibility 不满足时回退串行，不保证命中 PQ；
- 当前用户可见正例只覆盖固定整型 covering key column；
- 后续完整商用平移仍需继续迁移 clustered lookup visibility、non-covering
  materialization、ICP、ref/dependent-ref 和 worker/MQ row stream。
