# M3 Commercial Plan Clone And Resolver Activation Taskbook

## 状态

Planned。

## 目标

迁移商用 plan clone / resolver 的最小 activation probe 和 diagnostic。M3 不直接搬商用 `pq_make_join()` 全链路，不生成可执行 worker plan；真正 worker plan 只在 Item/JOIN/QEP_TAB clone contract 完整后打开。

## 允许修改

- `sql/sql_class.h`
- `sql/sql_lex.h`
- `sql/sql_optimizer.h`
- `sql/sql_optimizer.cc`
- `sql/parallel_query/pq_clone.*`
- `sql/parallel_query/pq_clone_item.cc`
- `sql/parallel_query/pq_resolver.*`
- `sql/parallel_query/pq_refix_fields_item.cc`
- `sql/parallel_query/pq_replace_base_item.cc`
- `sql/parallel_query/pq_optimizer.*`
- `Docs/pq_tasks/commercial-port-m3-plan-clone-resolver.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 禁止修改

- `storage/innobase/**`
- `sql/handler.h`
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/msg_queue.*`
- `sql/parallel_query/exchange.*`
- `mysql-test/suite/parallel_query/t/*`，除非只新增 M3 fallback/diagnostic 用例
- `mysql-test/suite/parallel_query/r/*`，除非只新增 M3 fallback/diagnostic 预期

## 设计要求

- M3 流程是 `eligible -> clone_preflight_attempt -> pq_make_join probe -> diagnostic -> cleanup -> serial plan`；
- clone 失败必须 worker-start 前 fallback；
- worker THD 不允许递归开启 PQ；
- 不打开 subquery / UNION / derived / semijoin / partition / secondary index；
- 不把 `PARALLEL_SCAN` 接入真实执行；
- 不迁移大面积 `Item::pq_clone()/refix_fields()/pq_restore()` virtual contract；
- `pq_dup_tabs()`、`pq_dup_order()`、`pq_replace_base_item()`、resolver/base-ref helpers 继续 fail-closed；
- 不创建 `Gather_operator`、不分配 worker、不调用 handler/InnoDB `pq_leader_scan_init()`；
- 新增 diagnostic 必须区分 clone attempts/fallback/unsupported shape；
- review Agent 达成一致后才提交。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m3 --tmpdir=/tmp/pqt_m3
```

## Completion Report

Pending.
