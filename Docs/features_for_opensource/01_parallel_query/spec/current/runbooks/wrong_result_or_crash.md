# Runbook：Parallel Query 错结果或崩溃

> 文档 ID：`PQ-RUN-002`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 运行验证：见 `current/quality/verification_evidence.md`；本文不把已有测试文件等同于当前提交已通过

## 1. 首要原则

先保护现场并证明“同一快照、同一 SQL 语义下串并结果是否等价”。不要先扩大 DOP、
反复重跑或修改数据；这些操作会破坏 MVCC、range 边界和并发时序证据。

若发生进程崩溃，保存 core、错误日志、符号化栈、目标二进制 build-id/commit、
崩溃线程及所有 PQ worker 线程栈。不要只截取 leader 栈。

## 2. 建立可比基线

在隔离数据副本上执行两组会话，显式固定所有会影响语义和计划的变量：

```sql
-- Serial oracle
SET SESSION force_parallel_execute=OFF;
SET SESSION parallel_default_dop=0;
<target statement>;

-- Parallel candidate；DOP 从 1、2、目标值逐级复现
SET SESSION force_parallel_execute=ON;
SET SESSION parallel_default_dop=1;
<target statement>;
```

结果比较应覆盖：行集合、重复行、NULL、排序键、warning/error、affected rows、
`FOUND_ROWS`/examined rows，以及 DML 的目标表与 binlog 条件。无 `ORDER BY` 的结果行序
不是稳定契约；但多行/少行、重复/遗漏、值差异仍是错误。浮点聚合应同时做精确输出
和容差分析，不能把所有差异都归因于浮点重排。

## 3. 按症状缩小模块

| 症状 | 高概率边界 | 应核对的不变量 |
|---|---|---|
| 少行/多行/重复行 | InnoDB range、cursor restore、MVI 去重 | range 覆盖全集且互斥；resume 不跳过/重读 |
| 仅并发 DML/purge/page split 时出错 | MVCC、persistent cursor、leader view 生命周期 | 所有 worker 可见性一致；view 覆盖执行期 |
| 二级索引/ICP 才出错 | ICP、回表、delete-mark、record image | ICP 前后语义一致；聚簇记录版本正确 |
| 反向扫描才出错 | reverse range 边界、restore direction | stored position 与消费方向一致 |
| GROUP BY/聚合才出错 | partial/final aggregation、receive temp table | 每行恰好贡献一次；NULL/DISTINCT 状态可合并 |
| ORDER BY/LIMIT 才出错 | `Exchange_sort`、稳定键、limit pushdown | 每 worker 有序；k-way merge 与最终 LIMIT 正确 |
| join 才出错 | DIV/CUT 选择、hash shared context、outer/semi 语义 | build/probe 所有权与 unmatched row 语义正确 |
| subquery/derived/UNION 才出错 | clone/refix、共享 materialization | leader/worker Item 绑定正确；共享对象只初始化一次 |
| 随 DOP 增大崩溃 | MQ 生命周期、worker cleanup、共享对象、资源计数 | 单一所有者；终态唯一；关闭顺序一致 |

## 4. 关键取证点

1. 保存串行与并行 `EXPLAIN FORMAT=TREE`；确认实际进入 `PARALLEL_SCAN`。
2. 保存 optimizer trace、`SHOW WARNINGS`、error log 和 PQ status 前后差值。
3. 记录 DOP=1/2/N 的首个失败值，以及 full/index/range/ref、正向/反向、聚簇/二级索引。
4. 记录隔离级别、显式事务、并发 writer/purge、表是否 partitioned、used partition 数。
5. 若可复现，使用已有 DBUG 注入点做单变量实验；不要在生产实例启用注入。
6. 崩溃时检查 `PQ_Ctx::read_record`、persistent cursor restore、MQ detach、
   worker `READY/OVER/ERROR/COMPELET` 和 handler end 的完整调用链。

核心源码入口：

- `sql/parallel_query/pq_iterators.cc`：leader Init/Read/End、worker scan iterator。
- `sql/parallel_query/sql_parallel.cc`：计划克隆、worker 执行、状态合并。
- `sql/parallel_query/msg_queue.cc`、`exchange*.cc`、`query_result_mq.cc`：记录协议。
- `storage/innobase/handler/ha_innodb_pq.cc`：handler 与 read view。
- `storage/innobase/row/row0pread_pq.cc`：range、cursor、MVCC、ICP/回表。

## 5. Retry 不能掩盖运行期错误

`parallel_fail_retry` 只对 `ER_PARALLEL_FAIL_INIT`、DML 命令且尚未完成执行的初始化失败
进行整句串行重试。Worker 已开始输出后的错误不能安全地从中间切换到串行，否则可能
重复结果或副作用。发现 warning“query was restarted without parallel query”时，必须保留
首轮错误证据；最终串行成功不代表 PQ 路径正确。

## 6. 修复后的证明集合

修复至少需要：

- 失败形状的最小结果等价性用例；
- DOP=1、2、较高 DOP；
- 边界行数 0/1、跨页、空 range、重复键、NULL；
- 若涉及 cursor/MVCC，加入并发 DML、purge、page split/merge、正反向扫描；
- 若涉及生命周期，加入 worker create/init/read 错误、KILL 和重复清理；
- feature-off 串行回归，以及 EXPLAIN/trace/status 断言。

代表性静态测试入口包括 `pq_range*.test`、`pq_range_scan_reverse.test`、
`pq_reverse_index_scan.test`、`pq_icp.test`、`pq_group_by.test`、
`pq_order*.test`、`pq_worker_error.test`。实际选取以
`requirements_test_traceability.md` 和目标改动影响面为准。
