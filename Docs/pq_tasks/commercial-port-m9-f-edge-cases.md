# M9-F Secondary / Ref Edge Cases Taskbook

## 状态

Status: M9-F0 negative guard matrix completed / Code/Task Review accepted。
M9-F1 MVI guard completed / Code/Task Review accepted。
No source edits outside MTR/docs。

M9-E2-0 已确认当前没有稳定 constant covering `REF + ICP` 正例，因此
E2-1/E2-2 不进入编码。M9-F 接手商用 secondary/ref/ICP 迁移中剩余的
边角能力，但不应一次性打开所有商用路径。

## 目标

M9-F 的目标是把商用实现中与 secondary/ref 执行相关、但尚未进入
M9-B/C/D/E 主线的边角能力拆成可独立验收的子阶段：

- MVI / multi-valued index unique filter 正确性边界；
- reverse range / reverse index / reverse ref scan；
- partition table full/range/ref/dependent-ref scan；
- secondary index MIN 特例；
- record buffer / prefetch 生命周期与性能路径。

## 设计结论

M9-F 不是单个编码任务。上述能力跨越 optimizer access shape、handler
scan direction、InnoDB cursor boundary、partition handler state、
record buffer ownership 和 MQ/result path。默认策略：

1. 先补负向护栏或 debug-only shape probe；
2. 对 MVI、partition、secondary MIN、record buffer 先确认是否保持串行；
3. reverse scan 先做 shape probe 和 guard，正向支持必须另经 review；
4. 最后才迁移 record buffer / prefetch 等性能优化；
5. 每个子阶段完成后单独 review，review accept 后再进入下一子阶段。

## 参考测试

参考仓测试入口：

- `pq_multi_value.test`：MVI / multi-valued index 正确性，覆盖
  `PQblockScanIterator` 和 `PQRefIterator`；
- `pq_range_scan_reverse.test`：reverse range scan，含 descending key 和
  range boundary 回归；
- `pq_reverse_index_scan.test`：reverse index scan / reverse_sorted index；
- `pq_ref_reverse_scan.test`：ref reverse scan；
- `pq_partition.test`：partition table full/range/ref/dependent-ref；
- `pq_sec_index_min.test`：secondary index MIN；
- `pq_record_buffer.test`：record buffer / prefetch。

当前仓测试分类中这些用例仍属于 later/V2，不能直接全量搬入并作为
当前阶段验收。

## 当前仓已知 guard

- MVI / multi-valued key：
  - `pq_optimizer.cc` 和 `pq_iterators.cc` 已拒绝
    `HA_MULTI_VALUED_KEY`；
  - 当前缺少专门 MTR 证明 MVI 不会误进 secondary/ref PQ。
- Reverse scan：
  - full scan handler 入口拒绝 reverse；
  - secondary range/ref factory 拒绝 `param.reverse`；
  - 当前缺少 reverse range/index/ref 的专门 MTR 护栏。
- Partition：
  - full-scan eligibility 拒绝 partitioned table；
  - secondary gate 拒绝 `table->part_info != nullptr`；
  - 当前缺少 partition full/range/ref/dependent-ref 的专门 MTR 护栏。
- Record buffer / prefetch：
  - 当前只有 leader-local `PQ_record_buffer_sink` deep-copy vector；
  - 尚无参考仓 handler native `Record_buffer` / `n_fetch_cached`
    生命周期支持。

## 子阶段拆分

### M9-F0: Edge-case Negative Guard Matrix

Status: coding/validation completed; Code/Task Review accepted。

目标：

- 用当前仓最小 SQL 覆盖 MVI、reverse、partition、secondary MIN、
  record buffer 相关 access shape；
- 证明当前未支持 shape 不会错误进入已打开的 B3/C2/D3d/E1c2a 路径；
- 不打开任何新执行路径。

允许修改：

- `mysql-test/suite/parallel_query/t/pq_commercial_ref_icp.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_ref_icp.result`
- `Docs/pq_tasks/commercial-port-m9-f-edge-cases.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/README.md`

禁止修改：

- `sql/`
- `storage/innobase/`

验收：

- targeted MTR record/replay 通过；
- unsupported windows 中 `Parallel_queries_executed`、
  `Parallel_workers_launched`、`Parallel_ranges_built`、
  `Parallel_ranges_dispatched`、`Parallel_secondary_rows_produced` 不增长；
- 不回退 M9-B3d/C2/D3d/E1c2a 已有正例。

Implementation notes:

- 新增 MVI / multi-valued index、partition table、reverse secondary range、
  secondary MIN shortcut、BLOB read-set 五类小表和负向查询；
- 使用局部 counter window 验证
  `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced` 均不增长；
- F0 不修改 `sql/` 或 `storage/innobase/`。

Validation:

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- targeted record/replay passed；
- full `parallel_query` suite passed: 74/74；
- F0 local deltas:
  `f0_edge_executed_delta=0`、`f0_edge_workers_delta=0`、
  `f0_edge_ranges_built_delta=0`、
  `f0_edge_ranges_dispatched_delta=0`、
  `f0_edge_secondary_rows_delta=0`。

Review:

- Code/Task Review Agent returned `ACCEPT`；
- confirmed five edge shapes are covered；
- confirmed no `sql/` or `storage/innobase/` files were modified；
- confirmed existing B3/C2/D3d/E1c2a positive statistics remain stable。

### M9-F1: MVI Guard

Status: coding/validation completed; Code/Task Review accepted。

目标：

- 明确 current branch 是否应该直接拒绝 MVI key / functional array key；
- 若当前 gate 已拒绝，补 MTR negative guard；
- 若存在绕过点，只做 fail-closed 修复，不打开 MVI 正例。

候选源码：

- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/pq_iterators.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`

验收：

- MVI full/index/ref 查询结果保持串行正确；
- PQ secondary/ref counters 不增长；
- review 确认没有把 JSON/array field materialization 引入现有 fixed
  record path。

Implementation notes:

- 在 F0 MVI 单表 ref guard 基础上新增 MVI dependent-ref guard；
- 新增 outer table 驱动 `pq_ref_icp_mvi` 的 `mv_doc_id_b` key；
- 使用局部 counter window 验证 MVI ref/dependent-ref 不增长
  `Parallel_queries_executed`、`Parallel_workers_launched`、
  `Parallel_ranges_built`、`Parallel_ranges_dispatched`、
  `Parallel_secondary_rows_produced`。

Validation:

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --record pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_ref_icp
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1
```

Results:

- targeted record/replay passed；
- full `parallel_query` suite passed: 74/74；
- F1 local deltas:
  `f1_mvi_executed_delta=0`、`f1_mvi_workers_delta=0`、
  `f1_mvi_ranges_built_delta=0`、
  `f1_mvi_ranges_dispatched_delta=0`、
  `f1_mvi_secondary_rows_delta=0`。

Review:

- Code/Task Review Agent returned `ACCEPT`；
- confirmed constant MVI ref and dependent MVI ref are both covered；
- confirmed no `sql/` or `storage/innobase/` files were modified；
- confirmed F0 negative guards and existing B3/C2/D3d/E1c2a positive
  statistics remain stable；
- non-blocking note: the dependent-ref EXPLAIN is currently rejected as
  `MULTI_TABLE`; if a future path opens multi-table MVI shapes, add a more
  direct unsupported-reason probe for `HA_MULTI_VALUED_KEY`。

### M9-F2: Reverse Scan Guard / Shape Probe

目标：

- 用最小 MTR 覆盖 reverse range、reverse index、reverse ref；
- 只读或 debug-only 确认 reverse shape 在当前 AccessPath/QEP_TAB 中如何
  表示；
- 明确 `param.reverse`、`m_reversed_access`、`ha_set_reverse_scan()`、
  InnoDB reverse cursor boundary 的最小契约；
- 不打开用户可见 reverse PQ。

候选源码：

- `sql/parallel_query/pq_iterators.cc`
- `sql/parallel_query/pq_optimizer.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`

验收：

- debug-only probe 能证明 reverse shape 被观测或被明确拒绝；
- current user-visible paths 对 reverse 仍 fallback；
- 文档给出是否另开 reverse 正向支持子阶段的明确前置条件。

### M9-F3: Partition Guard / Contract Design

目标：

- 迁移 partition table 最小负向护栏，覆盖 full/range/ref/dependent-ref
  代表形态；
- 设计 partition table PQ 的安全边界；
- 明确是否复用 `ha_innopart` / partition handler 的 per-part state；
- 先 guard/design，不打开 partition PQ。

原因：

- partition table 涉及 per-part `m_prebuilt`、partition pruning、
  `read_range_first_in_part()` / `read_range_next_in_part()`、record buffer
  partition awareness；
- 当前 M9-B/C/D/E 正例都明确要求 non-partitioned table。

验收：

- partition full/range/ref/dependent-ref 代表查询结果正确；
- PQ execution/secondary counters 不增长，或只保持既有 safe fallback；
- 输出 partition full/range/ref/dependent-ref 后续正向拆分；
- review accept 后再考虑编码。

### M9-F4: Secondary Index MIN Guard / Design

目标：

- 迁移 `pq_sec_index_min` 最小护栏；
- 确认商用 `pq_sec_index_min` 是否属于 M9 secondary path，
  还是更接近 aggregate / optimizer shortcut；
- 不混入 reverse/partition/record buffer。

验收：

- 结果与串行一致；
- 不误入已打开 secondary/ref PQ path；
- 明确是否保留串行、增加后续 guard，或另开 M7/MIN 聚合子阶段。

### M9-F5: Record Buffer / Prefetch Contract

目标：

- 设计 server `Record_buffer` 与 InnoDB `prebuilt->n_fetch_cached`、
  `fetch_cache_first`、`record_buffer->out_of_range` 的生命周期；
- 不作为 F0-F4 的前置；
- 不改变现有 leader-local row sink 语义。

验收：

- 明确是否能只用于性能优化；
- 明确与 ICP filtered rows、clustered lookup、reverse boundary、
  partition boundary 的交互；
- review accept 后再考虑编码。

### M9-F6: Optional Reverse Range Positive Gate

前置：

- F2 review explicitly recommends a positive reverse range gate。

目标：

- 只考虑 single-table、single secondary range、strict safe read_set、
  no ICP/no ref/no partition 的 reverse range；
- 复用或薄封装 InnoDB reverse cursor boundary；
- 不处理 reverse ref、reverse index scan、ORDER BY Gather Merge。

验收：

- 正例结果与串行一致；
- empty/out-of-range boundary 正确；
- workers/ranges/counters 与现有 leader-local secondary path 语义一致；
- 完整 `parallel_query` suite 通过。

### M9-F7: Commercial Breadth Backlog

目标：

- 记录但不阻塞首轮 M9-F 的商用广度回归：
  - `pq_record_buffer` 全量矩阵；
  - `pq_partition` subquery/semijoin/materialization；
  - reverse group merge / LIMIT / JOIN；
  - MVI positive unique filter。

## 推荐执行顺序

1. M9-F0 negative guard matrix；
2. M9-F1 MVI guard；
3. M9-F2 reverse scan guard / shape probe；
4. M9-F3 partition guard / contract design；
5. M9-F4 secondary index MIN guard / design；
6. M9-F5 record buffer / prefetch contract；
7. 只有 F2 review 明确建议时，才进入 M9-F6 optional reverse range
   positive gate；
8. M9-F7 commercial breadth backlog 不阻塞首轮。

串行规则：

- F0/F1/F2/F3/F4 可以并行调研，但提交和编码仍串行；
- F5/F6 不并行编码，因为它们共享 InnoDB cursor / handler state；
- partition 与 record buffer 必须在 reverse/secondary boundary 稳定后再动；
- MVI positive unique filter、partition positive、record buffer positive 都不在
  首轮 M9-F 编码范围。

## 风险点

- MVI key 可能产生 duplicate logical rows，需要 unique filter；在当前
  fixed row image / strict integer read_set 路线下直接打开风险高；
- reverse boundary 使用相反 end tuple 比较，容易 off-by-one 或漏读空区间；
- partition handler 的 `m_prebuilt`、blob heap、record buffer 和 part id
  状态不能被普通 InnoDB handler path 直接复用；
- record buffer 一旦引入，会改变 row_count、out-of-range、ICP filtered
  rows、fallback-after-buffer 的错误处理语义；
- secondary MIN 可能走 optimizer shortcut，不一定应归入 secondary range
  row production。

## Agent Review Prompt

请作为 M9-F Design Review Agent，只读审查本任务书：

1. 子阶段拆分是否覆盖商用 M9 secondary/ref 剩余关键边角；
2. MVI、reverse、partition、secondary MIN、record buffer 的先后顺序是否合理；
3. F0 negative guard 是否足以防止已有 M9-B/C/D/E 正例被边角 shape 误触发；
4. 哪些子阶段可以并行设计，哪些必须串行编码；
5. 是否存在应提前处理、否则会影响当前已提交功能正确性的 blocker。

输出：

- Verdict: ACCEPT 或 REVISE
- Blocking findings
- Non-blocking risks
- 建议下一步
