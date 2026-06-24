# Worker ExecuteIteratorQuery Smoke 迁移任务书

## 状态

Readinfo smoke contract in progress / review pending.

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
