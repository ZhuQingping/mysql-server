# Worker ExecuteIteratorQuery Smoke 迁移任务书

## 状态

Readinfo production risk closure completed.

本任务书承接：

- `commercial-port-next-worker-row-pull-bridge.md` W2 typed pull bridge；
- `commercial-port-m11-worker-result-path.md` M11-B3c threaded
  `Query_result_mq` probe；
- 2026-06-24 SQL / InnoDB / MTR 三个只读 Explorer 的最新差异结论。

## 背景

当前分支已经具备以下前置能力：

- `handler::ha_pq_next()` legacy wrapper 已接近商用形态；
- `PQblockScanIterator::Init()/Read()` 已能通过 typed
  `PQ_Worker_context*` 调 `pq_worker_scan_init()` /
  `pq_worker_scan_next()`；
- `ha_innobase::pq_worker_scan_next(PQ_Worker_context*, ...)` 已有
  callback-backed typed pull bridge；
- `Query_result_mq` 已有 PQWR ROW / FINISH / ERROR frame、leader decode
  adapter、local wiring smoke；
- `run_query_result_mq_threaded_probe_smoke()` 已证明一个 debug worker
  thread 可以通过 `Query_result_mq` 向 leader 发送 ROW / FINISH。

仍缺的商用主路径是：

- 可执行的 worker JOIN clone；
- `make_pq_worker_plan()`；
- worker 通过 `JOIN::query_expression()->ExecuteIteratorQuery(thd)` 执行；
- worker plan 内真实构造 `PQ_BLOCK_SCAN` 并进入 `PQblockScanIterator`；
- leader 通过 worker `Query_result_mq` 消费真实 worker SQL 执行结果。

## 当前不能重复做的事项

以下能力已经由现有测试覆盖，不作为本任务的新目标：

- 只验证 `Query_result_mq` frame 编解码；
- 只验证 worker thread 能发送 synthetic `Item_int` 行；
- 只验证 typed worker pull bridge 能读出记录；
- 只验证 `pq_make_join()` 能创建非执行 JOIN shell。

## 小步编码目标

下一步只做 debug-only `single-table worker ExecuteIteratorQuery smoke`
的第一个源码切口，目标不是打开默认用户路径，而是证明执行链路中的一个新增事实：

1. guarded smoke 能创建 worker plan shell；
2. 能识别当前 shell 还不能安全 `ExecuteIteratorQuery()` 的具体阻断点；
3. 能把阻断点记录为 status / MTR 断言；
4. 不启动默认生产路径，不改变 visible PQ 执行选择。

如果实现过程中发现可以在不改 SQL 主干 hook、不改 handler/InnoDB 的前提下让
worker shell 真正执行到 `PQblockScanIterator`，则允许继续完成 positive smoke；
否则本阶段以“可观测阻断点”收口，下一阶段再补 clone/QEP_TAB。

## 允许修改

- `sql/parallel_query/pq_clone.h`
- `sql/parallel_query/pq_clone.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterators.h`
- `sql/parallel_query/pq_iterators.cc`
- `sql/mysqld.cc`（仅新增 status var）
- `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- 本任务书

## 禁止修改

- `storage/innobase/**`
- `sql/handler.*`
- `sql/sql_class.*`
- `sql/sql_optimizer.*`
- `sql/sql_select.cc`
- `sql/sql_executor.*`
- `sql/join_optimizer/access_path.cc`
- ORDER BY / GROUP BY / ref / ICP / partition 主路径

## 设计要求

- 默认路径必须保持不变；只允许通过 DBUG flag 或现有 smoke chain 触发；
- 不允许调用 production `ExecuteIteratorQuery()` 后再 silent serial fallback；
- 如果 worker started 后失败，必须作为 smoke 失败或显式阻断计数，不得改写用户结果；
- 不能绕过 typed worker context owner 校验；
- 不允许回退到商用 legacy `void *scan_ctx` InnoDB pull path；
- 每个新增 status counter 必须进入 `pq_stats`。

## 建议新增诊断计数

- `Parallel_worker_execute_iterator_smoke_attempts`
- `Parallel_worker_execute_iterator_smoke_blocked_clone`
- `Parallel_worker_execute_iterator_smoke_blocked_readinfo`
- `Parallel_worker_execute_iterator_smoke_blocked_execute`
- `Parallel_worker_execute_iterator_smoke_success`

如果本阶段只做到可观测阻断，则预期：

- attempts 增加；
- exactly one blocked counter 增加；
- success 不增加；
- worker-result synthetic counters 不作为本阶段成功依据。

## 目标验证

开发阶段只跑目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_typed_pull_next_smoke \
  pq_commercial_worker_result_adapter \
  --parallel=1 --vardir=/tmp/pq-worker-exec-smoke-vardir \
  --tmpdir=/tmp/pq-worker-exec-smoke-tmpdir
```

完整 `parallel_query` suite 留到本轮源码阶段收尾后统一跑。

## Review 要求

编码完成后启动独立 Review Agent，重点检查：

- 是否误改默认执行路径；
- 是否真实推进 worker ExecuteIterator 合同，而不是重复现有
  `Query_result_mq` synthetic smoke；
- worker started 后失败语义是否清楚；
- status / MTR 是否能定位当前阻断点；
- 是否遵守 allowed / forbidden files。

## 当前决策

本任务是小任务小提交。完成后单独 commit，不与 native Record_buffer、
range/ref/ICP、ORDER BY、GROUP BY 等改动混合。

## 实现记录

上一小步实现选择“可观测阻断点”收口，没有打开 worker
`ExecuteIteratorQuery()`：

- 新增 `Gather_operator::run_worker_execute_iterator_smoke()`；
- probe 创建并销毁非执行 `pq_make_join()` shell；
- 当前阻断点记录在 `pq_make_join_readinfo()`，对应
  `Parallel_worker_execute_iterator_smoke_blocked_readinfo`；
- 新增 5 个 status 变量：
  - `Parallel_worker_execute_iterator_smoke_attempts`
  - `Parallel_worker_execute_iterator_smoke_blocked_clone`
  - `Parallel_worker_execute_iterator_smoke_blocked_readinfo`
  - `Parallel_worker_execute_iterator_smoke_blocked_execute`
  - `Parallel_worker_execute_iterator_smoke_success`
- 新增 `pq_worker_execute_iterator_smoke`，断言当前能通过 clone shell，
  并在 readinfo 阶段阻断，尚未到达 execute/success；
- 同步更新 `pq_stats` 变量清单。

本次未改动：

- 默认生产执行路径；
- handler / InnoDB；
- optimizer / access path；
- `Query_result_mq` 协议；
- ORDER BY / GROUP BY / ref / ICP / partition 路径。

验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_typed_pull_next_smoke \
  pq_commercial_worker_result_adapter pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-exec-target2-vardir \
  --tmpdir=/tmp/pq-worker-exec-target2-tmpdir
```

结果：目标 MTR 5/5 通过（含 `shutdown_report`）。

## Readinfo Smoke Contract 记录

本次继续推进一个小步：把 `pq_make_join_readinfo()` 从恒失败改为
smoke-safe 最小成功合同。

实现边界：

- 仅当 `join != nullptr`、`gather != nullptr` 且 `div_tab == nullptr` 时返回
  success；
- 仍不创建 QEP_TAB、AccessPath、worker iterator 或 cloned worker plan；
- 不启动 worker，不调用 handler/InnoDB，不调用 `ExecuteIteratorQuery()`；
- 默认用户可见路径不变。

MTR 预期同步为：

- clone shell 不阻断；
- readinfo 不再阻断；
- execute 前仍阻断在
  `Parallel_worker_execute_iterator_smoke_blocked_execute`；
- success 仍为 0。

开发阶段目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_typed_pull_next_smoke \
  pq_commercial_worker_result_adapter pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-readinfo-target-vardir \
  --tmpdir=/tmp/pq-worker-readinfo-target-tmpdir
```

## Readinfo Interface Risk Closure

Review Agent 提醒：`pq_make_join_readinfo()` 是导出的生产形态接口，如果它对
`div_tab == nullptr` 返回 success，未来非 smoke 调用可能误判为已完成
readinfo 构建。

修复决策：

- `pq_make_join_readinfo()` 恢复 fail-closed；
- 新增内部 `pq_make_join_readinfo_smoke_contract()`，只供
  `run_worker_execute_iterator_smoke()` 使用；
- smoke 仍能推进到 execute gate；
- 生产调用在真实 QEP_TAB / AccessPath 构建迁移前不会得到假阳性 success。

提交与验证：

- `062e8fdfb7c` Advance PQ worker execute smoke to execute gate；
- `c15da6be890` Keep PQ worker readinfo production gate closed；
- 开发期验证已限定为相关目标：`git diff --check`、`mysqld` build、
  `pq_worker_execute_iterator_smoke` / `pq_worker_typed_pull_next_smoke` /
  `pq_commercial_worker_result_adapter` / `pq_stats`。

## 后续执行策略

- 每个新增迁移切口单独 commit；
- 开发阶段只跑该切口相关 MTR，完整 suite 留到阶段收尾；
- 下一步必须推进 worker plan / ExecuteIterator 主路径的新事实，不再重复
  synthetic `Query_result_mq`、typed pull bridge 或 readinfo smoke。

## Worker PQ_BLOCK_SCAN Iterator Construction Smoke

本次小步继续停留在 debug-only smoke 链路，但把阻断点从“readinfo 后直接
execute gate”推进到商用 worker plan 的下一层边界：

- 从 leader 当前单表 `QEP_TAB/TABLE` 构造最小 `PQ_BLOCK_SCAN`
  `AccessPath`；
- 调用既有 `CreateIteratorFromAccessPath()` 创建 `PQblockScanIterator`；
- 不调用 iterator `Init()` / `Read()`；
- 不调用 `ExecuteIteratorQuery()`；
- 不启动 production worker path，不进入 handler/InnoDB worker scan。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_access_path`
- `Parallel_worker_execute_iterator_smoke_blocked_iterator`
- `Parallel_worker_execute_iterator_smoke_iterator_constructed`

预期 MTR 行为：

- clone/readinfo/access-path/iterator 均不阻断；
- iterator constructed 至少增加 1；
- execute gate 仍阻断；
- success 仍为 0。

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_typed_pull_next_smoke \
  pq_commercial_worker_result_adapter pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-accesspath-smoke-vardir2 \
  --tmpdir=/tmp/pq-worker-accesspath-smoke-tmpdir2
```

结果：目标 MTR 5/5 通过。

## Worker Table Owned Iterator Construction Smoke

Review Agent 对上一小步的残余风险是：如果后续越过 iterator construction
boundary，不能继续借用 leader `TABLE/QEP_TAB`。本次小步先收敛该风险：

- 通过 `pq_create_worker_thd()` 创建 worker THD；
- 通过 `pq_open_worker_table()` 打开独立 worker `TABLE/handler`；
- 使用 worker THD 和 worker TABLE 构造 `PQ_BLOCK_SCAN` `AccessPath`；
- 使用 worker THD 调用 `CreateIteratorFromAccessPath()` 构造
  `PQblockScanIterator`；
- 仍不调用 iterator `Init()` / `Read()`；
- 仍不调用 `ExecuteIteratorQuery()`；
- 完成后关闭 worker table、销毁 worker THD，并恢复 leader globals。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_worker_open`
- `Parallel_worker_execute_iterator_smoke_worker_table_opened`

预期 MTR 行为：

- clone/readinfo/worker-open/access-path/iterator 均不阻断；
- worker table opened 至少增加 1；
- iterator constructed 至少增加 1；
- execute gate 仍阻断；
- success 仍为 0。

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_typed_pull_next_smoke \
  pq_commercial_worker_result_adapter pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-open-iterator-smoke-vardir \
  --tmpdir=/tmp/pq-worker-open-iterator-smoke-tmpdir
```

结果：目标 MTR 5/5 通过。

## Worker PQ_BLOCK_SCAN Init/End Smoke

本次小步继续推进到 `PQblockScanIterator::Init()` 边界：

- `run_worker_execute_iterator_smoke()` 显式接收 `PQ_Leader_context`，不再隐式
  依赖 gather 状态；
- 使用 worker THD + worker TABLE 构造 iterator 后调用 `Init()`；
- `Init()` 成功后立即离开 iterator 作用域，由 `PQblockScanIterator`
  析构执行 `End()`，再关闭 worker table / 销毁 worker THD；
- 不调用 `Read()`；
- 不调用 `ExecuteIteratorQuery()`；
- execute gate 仍保持阻断，`worker_execute_iterator_smoke_success` 仍为 0。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_init`
- `Parallel_worker_execute_iterator_smoke_init_success`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_dop1 \
  pq_worker_attach_contract_smoke pq_parallel_scan_iterator_row_values \
  pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-init-smoke-vardir3 \
  --tmpdir=/tmp/pq-worker-init-smoke-tmpdir3
```

结果：目标 MTR 6/6 通过。

## Worker PQ_BLOCK_SCAN Single Row Read Smoke

本次小步继续推进到 `PQblockScanIterator::Read()` 的单行读取边界：

- 保留上一小步的 factory construction smoke；
- 单行 Read 使用直接构造的 `PQblockScanIterator`，便于显式控制
  `Init()` / `Read()` / `End()` 生命周期；
- `Read()` 只调用一次，读到一行即停止，不读 EOF；
- `need_rowid=false`，不触发 `handler::position()` / rowid / stable ref；
- 不接 `Query_result_mq`；
- 不调用 `ExecuteIteratorQuery()`；
- execute gate 仍保持阻断，`worker_execute_iterator_smoke_success` 仍为 0。

实现过程中确认：

- 外层 `m_leader_ctx` 是 PROBE context，不能用于真实单行 Read；
- 必须在外层 PROBE context `cleanup_pq_resources(false)` 之后运行本 smoke，
  避免 caller 持有被同一 handler 重建时清理掉的 PROBE ctx；
- 本 smoke 内部创建局部 EXECUTE leader context，完成后在所有路径释放；
- worker table / worker THD / iterator ctx 均在局部 EXECUTE context 释放前清理。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_read`
- `Parallel_worker_execute_iterator_smoke_read_success`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_worker_typed_pull_next_smoke \
  pq_parallel_scan_iterator_row_values pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-read-smoke-vardir5 \
  --tmpdir=/tmp/pq-worker-read-smoke-tmpdir5
```

结果：目标 MTR 5/5 通过。

## Worker Query_result_mq Ownership Smoke

本次小步继续推进 worker ExecuteIterator 链路中“结果接收者归属”的边界：

- 在 worker THD mem_root 上创建 worker-owned `Query_result_mq`；
- 校验 `Query_result_mq` 持有的 `MQueue_handle` 与 worker MQ handle 一致；
- 将同一个 `Query_result_mq` 绑定到 worker
  `Query_expression` 和 `Query_block`；
- 校验两个 query result getter 均返回同一个 worker-owned result；
- 当前 `pq_make_join()` 仍共享 leader `Query_block`，因此绑定后必须在销毁
  worker THD 前恢复原始 result 指针；
- 不调用 `send_result_set_metadata()` / `send_data()` / `send_eof()`；
- 不调用 `ExecuteIteratorQuery()`；
- execute gate 仍保持阻断，`worker_execute_iterator_smoke_success` 仍为 0。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_result`
- `Parallel_worker_execute_iterator_smoke_result_bound`
- `Parallel_worker_execute_iterator_smoke_result_restored`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_commercial_worker_result pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-result-own-vardir \
  --tmpdir=/tmp/pq-worker-result-own-tmpdir
```

结果：目标 MTR 4/4 通过（含 `shutdown_report`）。

## Worker JOIN Scalar Shape Copy Smoke

本次小步把 worker-owned query shell 继续推进为带 JOIN 标量 shape 的非执行
worker shell。该步骤只复制安全标量字段和本地 PQ 标记，不复制 `QEP_TAB`、
`TABLE`、handler、AccessPath 或 Item/ORDER/GROUP 树，也不打开 worker
`ExecuteIteratorQuery()`。

实现边界：

- `pq_make_join()` 在创建 worker JOIN 后调用 `JOIN::pq_copy_from()`；
- `JOIN::pq_copy_from()` 分配 ref item slice，并把 active slice 指向 worker
  query block 的 `base_ref_items`；
- 设置 `query_block->join = worker_join`；
- 复制表数量、const/primary table 数量、plan state、limit、found rows 相关
  标量、rowcount 估算和 PQ 本地标记；
- worker offset 在 `parallel_exec` 场景下归零，避免 worker 单独应用 leader
  OFFSET；
- `qep_tab` 仍保持未复制，真实 worker plan 仍在后续 `pq_dup_tabs()` 切片；
- 新增诊断：
  - `Parallel_worker_join_shape_attempts`
  - `Parallel_worker_join_shape_success`
  - `Parallel_worker_join_shape_unsupported`
- `pq_worker_execute_iterator_smoke` 断言 shape copy 成功且 unsupported 不增加；
- execute gate 仍保持 fail-closed，`success` 仍为 0。

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-join-shape-vardir \
  --tmpdir=/tmp/pq-worker-join-shape-tmpdir
```

## Worker QEP_TAB Skeleton Smoke

本次小步迁移商用 `pq_dup_tabs()` 的第一段形状，但保持为显式 smoke-only
preflight，不接入 `pq_make_join()` 默认主路径，也不复制任何可执行对象。

实现边界：

- 新增 `pq_dup_tabs_skeleton_preflight(worker_join, leader_join)`；
- 仅在 `run_worker_execute_iterator_smoke()` 的 worker plan helper 中显式调用；
- 分配 worker-owned `QEP_shared[]` 和 `QEP_TAB[]`；
- 只设置每个 `QEP_TAB` 的 `QEP_shared`、`join` 和 `idx`；
- 明确校验 `table()`、`table_ref`、`condition()`、`range_scan()` 均为空；
- 不复制 `TABLE`、handler、AccessPath、condition、ref、filesort、tmp table；
- `JOIN::destroy()` 能清理 skeleton，不访问真实 table/handler；
- 现有 worker execute smoke 仍稳定停在 execute gate，`success` 仍为 0。

新增诊断：

- `Parallel_worker_qep_tab_skeleton_attempts`
- `Parallel_worker_qep_tab_skeleton_success`
- `Parallel_worker_qep_tab_skeleton_unsupported`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-qep-skeleton-vardir \
  --tmpdir=/tmp/pq-worker-qep-skeleton-tmpdir
```

## Worker QEP_TAB Table Bind Smoke

本次小步在 worker-owned `QEP_TAB` skeleton 基础上，临时绑定
`pq_open_worker_table()` 打开的 worker-local `TABLE` 和 `Table_ref`，验证后
立即解绑。该步骤对应商用 `pq_set_table_ref()` / `pq_dup_tabs()` 的最小
TABLE/Table_ref 绑定形状，但仍不迁移完整 table clone 或执行路径。

实现边界：

- 新增 `pq_bind_qep_tab_table_preflight(worker_join, worker_table,
  leader_table)`；
- 只在 `run_worker_execute_iterator_smoke()` 打开 worker table 后显式调用；
- 仅支持当前单 base table、非 const worker `QEP_TAB[0]`；
- 临时执行 `set_table(worker_table)` 和 `table_ref = worker_ref`；
- 验证 worker table、handler、record buffer 均与 leader 分离；
- 验证 `Table_ref::query_block` 可临时指向 worker query block；
- 验证 read/write bitmap 已由 `pq_open_worker_table()` 从 leader 同步；
- preflight 结束前恢复 `table_ref`、`Table_ref::query_block`、
  `TABLE::pos_in_table_list` 并 `set_table(nullptr)`；
- 不复制 `TABLE::pq_copy()`、condition、range scan、ref/keyuse、AccessPath、
  join cache、semijoin、tmp table 或 handler 状态；
- 后续 `NewPQblockScanAccessPath()` / `PQblockScanIterator` 仍显式使用
  `qep_tab=nullptr`，execute gate 仍保持 fail-closed。

新增诊断：

- `Parallel_worker_qep_tab_table_bind_attempts`
- `Parallel_worker_qep_tab_table_bind_success`
- `Parallel_worker_qep_tab_table_bind_unsupported`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-qep-table-bind-vardir \
  --tmpdir=/tmp/pq-worker-qep-table-bind-tmpdir
```

## Worker Range Scan Clone Helper

本次小步平移商用实现中的 `CopyRangeScanAccessPath()` 主体，先让当前仓库具备
worker-local range scan clone helper：

- 支持 `INDEX_RANGE_SCAN`、`INDEX_MERGE`、`ROWID_INTERSECTION`、
  `ROWID_UNION` 的递归复制；
- `INDEX_RANGE_SCAN` 的 `KEY_PART::field` 会重指向 worker `TABLE` 的
  `key_info` 字段；
- `QUICK_RANGE`、children array、CPK child 均分配在 worker THD 的
  `pq_mem_root`，缺失时回退到 `mem_root`；
- 新增 `pq_clone_range_scan_preflight()` 作为 smoke-only 诊断入口；
- 当前只在 `run_worker_execute_iterator_smoke()` 中发现
  `source_tab->range_scan()` 时调用，并且结果不阻断既有 smoke。
- 本步诊断入口只验证 plain `INDEX_RANGE_SCAN` 的字段重绑定；递归
  `INDEX_MERGE` / `ROWID_INTERSECTION` / `ROWID_UNION` clone helper 已实现，
  但要等后续专门 range/index-merge 用例覆盖后再作为验收门。

当前边界：

- 不把 cloned range scan 挂到 worker `QEP_TAB`；
- 不传入 `NewPQblockScanAccessPath()` 或 `PQblockScanIterator`；
- 不打开 production `ExecuteIteratorQuery()`；
- 如果 TABLE/Field 重绑定尚未满足要求，只记录
  `Parallel_worker_range_scan_clone_unsupported`。

新增诊断：

- `Parallel_worker_range_scan_clone_attempts`
- `Parallel_worker_range_scan_clone_success`
- `Parallel_worker_range_scan_clone_unsupported`

后续依赖：

- 完成 worker `TABLE::pq_copy()` / Field / record / bitmap ownership 迁移后，
  再把 range clone 从诊断提升为 `pq_dup_tabs()` 的真实复制步骤；
- 届时新增专门 range query MTR，断言 clone success，而不是只依赖
  fullscan execute smoke。

验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-range-clone-vardir \
  --tmpdir=/tmp/pq-worker-range-clone-tmpdir
```

## Worker Table_ref Clone Helper

本次小步平移商用实现中的 `Table_ref::pq_copy()`，为后续
`pq_set_table_ref()` / 完整 `pq_dup_tabs()` 打基础：

- 在 `Table_ref` 上新增 `pq_copy(THD*, Table_ref*)` 成员声明；
- 实现复制 `effective_algorithm`、`tableno`、derived column names；
- 对 `table_name`、`alias`、`db` 执行 worker THD `mem_root` 上的独立字符串复制；
- 新增 `pq_clone_table_ref_preflight()`，在 worker execute smoke 中独立验证
  cloned `Table_ref` 的 table number、db/table name、alias；
- 当前不把 cloned `Table_ref` 接入 worker `QEP_TAB` 或生产执行路径。

新增诊断：

- `Parallel_worker_table_ref_clone_attempts`
- `Parallel_worker_table_ref_clone_success`
- `Parallel_worker_table_ref_clone_unsupported`

当前边界：

- 不迁移 `pq_set_table_ref()`；
- 不迁移 `TABLE::pq_copy()`、`QEP_TAB::pq_copy()`、`Index_lookup::pq_copy()`；
- 不修改 optimizer / executor / handler / InnoDB 主路径；
- worker `QEP_TAB` 仍使用前一小步的临时 bind smoke，不持久引用新 clone。

验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-table-ref-clone-vardir \
  --tmpdir=/tmp/pq-worker-table-ref-clone-tmpdir
```

## Restricted Worker Plan Ownership Helper

本次小步不改变执行语义，只把 `run_worker_execute_iterator_smoke()` 里散落的
worker THD / JOIN shell / `Query_result_mq` bind-restore / cleanup 逻辑收敛
为受限 helper，贴近商用 `make_pq_worker_plan()` 的资源所有权形状。

实现边界：

- helper 负责创建 worker THD；
- helper 负责通过当前 `pq_make_join()` 创建非执行 JOIN shell；
- helper 负责执行 smoke-only readinfo contract；
- helper 负责 `Query_result_mq` 临时绑定和 cleanup 期间恢复原始 result；
- helper cleanup 按调用点决定是否关闭 worker table，保持现有 close 顺序；
- 当前仍稳定命中 ownership blocker；
- 仍不调用 `ExecuteIteratorQuery()`。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_plan_constructed`
- `Parallel_worker_execute_iterator_smoke_plan_cleaned`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_commercial_worker_result pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-plan-helper-vardir \
  --tmpdir=/tmp/pq-worker-plan-helper-tmpdir
```

结果：目标 MTR 4/4 通过（含 `shutdown_report`）。

## Worker Query Ownership Preflight Gate

两个只读 Explorer 对商用仓和当前仓的结论一致：商用
`make_pq_worker_plan()` 依赖 `pq_dup_select()` / `pq_dup_tabs()` /
`pq_make_join_readinfo()` 构造 worker-owned `Query_block`、`Query_expression`
和可执行 worker `JOIN`；当前仓的 `pq_make_join()` 仍只是
`new JOIN(worker_thd, leader_query_block)` shell。

本次小步把这个真实阻断点固化为 execute 前的 fail-closed preflight：

- 保留前一小步的 `Query_result_mq` bind/restore smoke；
- 新增 `pq_worker_join_ownership_preflight()`，拒绝共享 leader
  `Query_block` 或 `Query_expression` 的 worker JOIN；
- 当前预期稳定命中 ownership blocker；
- 仍不调用 `ExecuteIteratorQuery()`；
- 仍同步记录 `blocked_execute`，保持既有 smoke 语义连续；
- 下一阶段只有实现 worker-owned query shell 后，才能把该 gate 从 blocked
  推进到 success。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_ownership`

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_commercial_worker_result pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-ownership-gate-vardir \
  --tmpdir=/tmp/pq-worker-ownership-gate-tmpdir
```

结果：目标 MTR 4/4 通过（含 `shutdown_report`）。

## Worker-Owned Query Shell Smoke

本次小步把 `pq_make_join()` 从共享 leader `Query_block` 的 JOIN shell 推进为
worker-owned 顶层 `Query_block` / `Query_expression` shell，但仍不构造可执行
worker plan，也不调用 `ExecuteIteratorQuery()`。

实现边界：

- 仅在既有 `pq_clone_shell_supported()` 单表场景下创建 owned shell；
- 在 worker THD 的 `LEX` 上创建新的顶层 `Query_block`；
- 复制最小非执行元数据：`select_number`、active options、`parallel_exec`、
  `uncacheable`；
- 通过 `pq_link_clone()` 记录 leader -> worker clone 关系；
- `JOIN::pq_restore()` / `Query_block::pq_restore()` 在 cleanup 前 unlink clone；
- `pq_make_join()` 在 JOIN 分配失败时立即 rollback clone link；
- `PQ_worker_execute_smoke_plan::cleanup()` 销毁 worker JOIN 前恢复 clone link；
- ownership preflight 现在应通过；
- execute gate 仍保持 fail-closed，`success` 仍为 0。

说明：当前 `pq_clone_activation_probe()` 仍运行在 leader THD 上，本切片后会在
preflight 成功后更保守地 fallback，不再创建 leader-side shell；worker-owned shell
仅由 worker THD 的 smoke plan 路径验证。

开发期目标验证：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_commercial_worker_result pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-owned-shell-vardir \
  --tmpdir=/tmp/pq-worker-owned-shell-tmpdir
```

结果：目标 MTR 4/4 通过（含 `shutdown_report`）。
