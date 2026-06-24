# Worker QueryExpression Root Iterator Contract

Last synced: 2026-06-24

## 状态

Root iterator result-bound Read coding in progress.

## 背景

当前 worker execute smoke 已经完成以下事实验证：

- worker THD / worker TABLE / worker handler 独立打开；
- worker QEP_TAB skeleton、TABLE/Table_ref attach、TABLE/QEP_TAB scalar copy；
- worker-owned `PQ_BLOCK_SCAN` `AccessPath` 可直接被
  `CreateIteratorFromAccessPath()` 消费；
- worker JOIN root 可临时挂载 `PQ_BLOCK_SCAN`，再通过
  `worker_join->root_access_path()` 构造 iterator；
- root path 在 scope 内恢复，仍不调用 `ExecuteIteratorQuery()`。

剩余关键缺口是：真实商用 worker 执行最终需要 worker
`Query_expression` 拥有 root iterator，但当前分支不能安全直接调用
`Query_expression::force_create_iterators()`，因为它读取的是
`Query_expression::m_root_access_path`，不是 `JOIN::root_access_path()`。

## 只读调研结论

### 商用接口差异

商用 `taurusdbondstore` 中：

- `Query_expression::create_iterator_from_accesspath(THD*)` 是一个窄包装；
- 包装内部根据 `is_set_operation()` / materialized set operation 选择
  `JOIN*`，再调用 `CreateIteratorFromAccessPath()`；
- 商用还把 `Query_expression::create_access_paths(THD*)` 改为 public 且返回
  `bool`；
- 商用大量公开 `JOIN` PQ 状态字段，但这些不是本阶段必要条件。

当前分支中：

- 已有 `JOIN::root_access_path()` / `JOIN::set_root_access_path()`；
- 已有 `Query_expression::root_access_path()`、`root_iterator()`、
  `release_root_iterator()`、`clear_root_access_path()`；
- `Query_expression::create_access_paths(THD*)` 仍是 private `void`；
- 没有 `Query_expression::create_iterator_from_accesspath(THD*)`；
- `Query_expression::m_root_access_path` / `m_root_iterator` 仍是 private。

### 生命周期风险

当前分支正常执行路径中，simple query 的
`Query_expression::m_root_access_path = join->root_access_path()` 只发生在
`Query_expression::create_access_paths(THD*)`。`force_create_iterators()` 不是
复制点，它假设 `m_root_access_path` 已经有效。

如果 PQ smoke 过早把临时 worker root path 或 iterator 写入
`Query_expression`，容易破坏：

- `JOIN::root_access_path()` 与 `Query_expression::m_root_access_path` 的别名
  一致性；
- `m_root_iterator` 的 `unique_ptr_destroy_only<RowIterator>` ownership；
- worker TABLE/QEP_TAB detach 与 iterator 析构顺序；
- worker `Query_result_mq` 绑定/恢复顺序；
- `ExecuteIteratorQuery()` 中 `join->join_free()`、EOF、examined rows 等普通
  用户路径清理语义。

## 设计决策

下一步不直接迁移商用
`Query_expression::create_iterator_from_accesspath(THD*)`，也不新增
`Query_expression` root setter。

原因：

- 商用 wrapper 的真实语义是“从当前 `m_root_access_path` 写入
  `m_root_iterator`”，而当前 worker smoke 还没有安全设置 unit root path；
- 本阶段若只为 smoke 写入 `m_root_iterator`，会制造一个看似可执行但 cleanup
  ownership 不完整的 worker unit；
- 当前最需要证明的是 root iterator 的 `Init()` / cleanup 合同，而不是
  `Query_expression` ownership。

推荐下一小步：新增一个 smoke-local root iterator contract helper，只接收显式
`THD* + AccessPath* + JOIN*`，构造局部 iterator，并在同一作用域内完成
`Init()` 探测，依赖局部 `RowIterator` 析构完成具体 iterator cleanup。`Read()`
作为后续独立切口处理，避免本阶段新 root iterator 和现有手写
`PQblockScanIterator` Read smoke 竞争同一个 worker scan context。

## 下一阶段任务：Root Iterator Local Contract Helper

### Goal

把当前“root AccessPath 可构造 iterator”的 smoke 推进到“root iterator
可局部 `Init()` 并由作用域析构清理”的 smoke，但仍不写入
`Query_expression`。

### Allowed Files

- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/mysqld.cc`（仅新增 status var）
- `mysql-test/suite/parallel_query/t/pq_worker_execute_iterator_smoke.test`
- `mysql-test/suite/parallel_query/r/pq_worker_execute_iterator_smoke.result`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- 本任务书

### Forbidden Files

- `sql/sql_lex.h`
- `sql/sql_union.cc`
- `sql/sql_optimizer.*`
- `sql/sql_select.cc`
- `sql/sql_executor.*`
- `sql/join_optimizer/access_path.*`
- `sql/handler.*`
- `storage/innobase/**`

### Design Requirements

- helper 必须 smoke/local-only，不暴露为生产 API；
- helper 不得写 `Query_expression::m_root_access_path`；
- helper 不得写 `Query_expression::m_root_iterator`；
- helper 不得调用 `force_create_iterators()`；
- helper 不得调用 `ExecuteIteratorQuery()`；
- root path attach 后必须先恢复，再 cleanup worker plan；
- iterator 必须在 worker QEP_TAB/TABLE detach 前析构；
- root iterator `Read()` 成为本 smoke 的唯一 row-read 路径，不再同时运行
  hand-written `PQblockScanIterator` Read；
- 失败路径必须只增加 blocked counter 并 fail-closed；
- 默认用户可见 PQ gate 不变。

### Suggested Counters

- `Parallel_worker_execute_iterator_smoke_root_init_success`
- `Parallel_worker_execute_iterator_smoke_blocked_root_init`
- `Parallel_worker_execute_iterator_smoke_root_read_success`
- `Parallel_worker_execute_iterator_smoke_blocked_root_read`

公开 status 名必须控制在 PFS 可见长度内。若名称接近 64 字符，应采用短名，
例如 `Parallel_worker_execute_iterator_smoke_root_read_ok`。

### Validation

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_worker_execute_iterator_smoke pq_clone_diagnostics pq_stats \
  --parallel=1 --vardir=/tmp/pq-worker-root-iterator-local-vardir \
  --tmpdir=/tmp/pq-worker-root-iterator-local-tmpdir
```

### Review Focus

- 是否绕过了本任务的 forbidden files；
- 是否把 local smoke iterator 写进 `Query_expression`；
- root path / QEP_TAB / TABLE / result 绑定恢复顺序是否安全；
- destructor 是否早于 worker TABLE close 和 worker THD destroy；
- status/MTR 是否证明 root iterator local `Init()` / destructor cleanup
  lifecycle，而不是重复上一阶段 construction smoke。

## 后续商用 wrapper 条件

只有在以下条件同时满足后，才进入商用
`Query_expression::create_iterator_from_accesspath(THD*)` 迁移：

- worker `Query_expression::m_root_access_path` 有安全、单一的设置路径；
- worker `m_root_iterator` ownership 与 cleanup 顺序已设计并验证；
- worker `Query_result_mq` 已能承接 `ExecuteIteratorQuery()` 的 metadata/data
  /EOF/error；
- worker `JOIN` ownership 和 `join_free()` 语义已明确；
- review agent 接受触碰 `sql/sql_lex.h` / `sql/sql_union.cc` 的设计。

## Init/destructor-cleanup 实现记录

本阶段选择更窄的 Init/destructor-cleanup 切口：

- 在现有 worker JOIN root attach scope 内复用 factory-created root iterator；
- root Init smoke 放在既有手写 `PQblockScanIterator` single-row Read smoke
  之后；
- `CreateIteratorFromAccessPath()` 成功后调用 root iterator `Init()`；
- `Init()` 成功后在同一 scope 内销毁 iterator；
- 不调用 root iterator `Read()`；
- 不写 `Query_expression::m_root_access_path`；
- 不写 `Query_expression::m_root_iterator`；
- 不调用 `force_create_iterators()`；
- 不调用 `ExecuteIteratorQuery()`。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_root_init`
- `Parallel_worker_execute_iterator_smoke_root_init_ok`

本阶段有意保留现有手写 `PQblockScanIterator` single-row Read smoke，避免
factory-created root iterator 和手写 iterator 在同一小步内都读取同一个 worker
scan context。`RowIterator` 基类没有 `End()` 接口；具体
`PQblockScanIterator` 的 `End()` 由析构函数调用。

MTR 首轮验证发现：若 root iterator `Init()` 放在手写 Read smoke 之前，会占用
同一个 worker scan context 并导致后续手写 Read/result binding 断言失败。因此
本阶段固定顺序为：先完成既有 hand-written iterator Read/End，再验证 factory
root iterator Init/destructor cleanup。

## Read 实现记录

本阶段继续在同一个 factory-created root iterator scope 内推进一次 `Read()`。
验证中发现 factory root iterator 和 hand-written `PQblockScanIterator` 不能在同
一个 worker scan context 上顺序各读一次：无论 root Init 放在前面，还是
hand-written Read 放在前面，都会让另一路径观察到已被占用/关闭的 worker
context。因此本阶段将 smoke 切换为 root iterator 唯一 row-read 路径。

- factory root iterator `Init()` 成功后调用一次 `Read()`；
- `Read()` 返回 `0` 视为 root iterator row production smoke 成功；
- `Read()` 不负责发送 `Query_result_mq`，只验证 root iterator 能把 row 放入
  worker table record buffer；
- hand-written `PQblockScanIterator` direct Init/Read counter 在本 smoke 中保持
  不增加，用于证明不再重复消费同一个 worker scan context；
- 不写 `Query_expression::m_root_access_path`；
- 不写 `Query_expression::m_root_iterator`；
- 不调用 `force_create_iterators()`；
- 不调用 `ExecuteIteratorQuery()`。

新增诊断：

- `Parallel_worker_execute_iterator_smoke_blocked_root_read`
- `Parallel_worker_execute_iterator_smoke_root_read_ok`

## Result-bound Read 实现记录

本阶段继续把 worker result 合同前移到 root iterator `Read()` 之前：

- 在 factory root iterator `Init()/Read()` 前绑定 worker-owned
  `Query_result_mq`；
- root iterator `Read()` 在 worker query expression / query block result 已经
  指向 `Query_result_mq` 的状态下运行；
- 新增 `Parallel_worker_execute_iterator_smoke_result_bound_before_read`，
  MTR 断言该 counter 与 root read success 同时增加；
- cleanup 仍通过 `PQ_worker_execute_smoke_plan::cleanup()` 统一恢复原始 result，
  且恢复发生在 worker JOIN destroy 之前；
- 仍不调用 `ExecuteIteratorQuery()`，也不发送 Query_result_mq data frame；
- 默认用户可见 PQ gate 不变。

## Result MQ row/eof smoke 实现记录

本阶段继续补齐 root `Read()` 后的 worker result frame 闭环：

- root iterator `Read()` 成功后，构造 smoke-only worker-owned `Item_field`
  列表，字段来源是 `worker->m_open_ctx.worker_table->field[]`；
- 不使用 worker query block fields，也不使用 leader fields，因为当前 worker
  query shell 还没有完整 Item clone / refix / replace-base-item；
- 使用已绑定的 `Query_result_mq` 执行一次 `send_data()` 和一次 `send_eof()`；
- 立即从 worker `MQueue_handle` 解码 ROW/FINISH frame，证明发送链路可用；
- 新增诊断：
  - `Parallel_worker_execute_iterator_smoke_blocked_result_send`
  - `Parallel_worker_execute_iterator_smoke_result_row_sent`
  - `Parallel_worker_execute_iterator_smoke_result_eof_sent`
- 仍不调用 `ExecuteIteratorQuery()`；
- 仍不声明真实 SELECT 输出正确性；正式路径还需要完整 worker Item clone /
  refix / replace-base-item。
