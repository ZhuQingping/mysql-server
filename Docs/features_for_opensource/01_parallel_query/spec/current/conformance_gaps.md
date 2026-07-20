# Parallel Query Current Conformance Gaps

> 文档 ID：`PQ-CONFORMANCE-GAPS-001`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 运行验证：见 `current/quality/verification_evidence.md`

> Snapshot authority：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`。该 manifest 绑定
> implementation commit `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9`、`stable_branch` 已提交基线、
> `source_tree_sha256=a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> 和 `test_tree_sha256=066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`。
> 两个 hash 只覆盖 manifest 定义的 PQ source/test file set，不覆盖或复现整个 bound PQ source/test tree。

## 1. 目的与决策状态

本文集中记录历史设计/旧文档与当前源码之间已由静态证据确认的差异，以及当前 contract
提出但尚未被代码和运行证据闭环的 hardening 项。历史 WL 和 `PQ-Feature-Spec.md` 保留设计
价值，但不是 current authority。

决策状态：

| 状态 | 含义 |
|---|---|
| `accept-current-code` | 当前行为作为本快照事实；历史描述降级为 design-only |
| `documentation-fix-required` | 当前实现已明确，后续文档隔离任务需修正或加 legacy banner |
| `product-decision-required` | 接口/语义存在冲突，需要确定对外契约后再改代码或文档 |
| `hardening-required` | 当前路径存在可证明的 contract/test/observability 缺口 |
| `audit-candidate` | 静态风险尚未由复现和运行证据确认，不能称为已确认产品缺陷 |

## 2. 已确认的设计/文档—代码 Drift

| GAP-ID | 主题 | 历史/声明 | 当前源码事实 | 主要证据 | 决策状态 | 关闭条件 |
|---|---|---|---|---|---|---|
| `PQ-GAP-CONF-001` | DIV_TAB 选择 | 部分旧资料称第一张非 const 主表 | table hint → 第一张超过 rows threshold；threshold=0 时选择 `rows_fetched` 最大候选 | `pq_optimizer.cc::JOIN::choose_parallel_tables` | `accept-current-code` + `documentation-fix-required` | 历史文档显式标 design-only；current support/optimizer spec 保持一致 |
| `PQ-GAP-CONF-002` | Partition | WL008 描述跨多个 used partitions 并行 | `TABLE::suite_for_pq_division` 只允许 `num_partitions_used()==1`；在单个 partition 内切 B+Tree | `sql/table.cc`、`ha_innodb_pq.cc`、`pq_partition` | `accept-current-code` + `documentation-fix-required` | WL 加历史状态；多 partition 若实现需新 contract/test |
| `PQ-GAP-CONF-003` | Worker scheduling | WL010/WL018 声称经 thread pool task queue | 主路径直接 `mysql_thread_create(key_thread_parallel_query, ...)` | `pq_iterators.cc::pq_launch_worker` | `accept-current-code` + `documentation-fix-required` | 当前文档统一 `design-only`；未来集成重新定义 admission/join/PFS |
| `PQ-GAP-CONF-004` | Queue timeout 单位 | sysvar help 写 microseconds，旧资料单位不一致 | `check_pq_running_threads(... timeout_ms)` 按 `/1000` 秒和余数×1,000,000 纳秒换算，即 milliseconds | `sql/sys_vars.cc`、`sql_parallel.cc` | `product-decision-required` | 选定单一单位，同步代码/help/测试/用户文档并加边界测试 |
| `PQ-GAP-CONF-005` | MQ spin 变量名 | 历史资料使用 `pq_msg_queue_spin_count` | 当前变量是 `pq_msg_queue_spin_lock` | `sql/sys_vars.cc`、current codewalk | `accept-current-code` + `documentation-fix-required` | 历史文档不再作为变量参考 |
| `PQ-GAP-CONF-006` | 资源/CBO 默认值 | 历史资料常见 memory 256MB、max threads 128、cost 10000、tuple 0.1、setup 1000 | 当前分别为 100MB、64、1000、1.5、250 | `sql/sys_vars.cc` | `accept-current-code` + `documentation-fix-required` | current docs 只引用源码值；历史表标版本/状态 |
| `PQ-GAP-CONF-007` | Graceful fallback | 部分旧资料描述默认关闭或覆盖全部阶段 | 当前默认开启，只覆盖 clone/refix 早期；后期通过 `ER_PARALLEL_FAIL_INIT` full retry | `sql/sys_vars.cc`、`sql_parallel.cc::make_pq_leader_plan`、`sql_parse.cc::retry_without_parallel_query` | `accept-current-code` + `documentation-fix-required` | 历史描述隔离；point-of-no-return 测试矩阵完成 |
| `PQ-GAP-CONF-008` | Secondary index | InnoDB PQ 源码残留“不支持”注释 | 当前实现包含 secondary range、ICP 和 clustered lookup 路径 | `row0pread_pq.cc::find_visible_record`、`pq_range_sec`、`pq_icp` | `documentation-fix-required` | 清理陈旧注释并以 current support matrix 为准 |
| `PQ-GAP-CONF-009` | Correlated subquery | WL007 表述不支持，WL014 表述支持 | 当前只支持 feature-switch 开启、simple EXISTS/single-row、位置和嵌套 shape 均受限的 worker-multiple 路径 | `sql_union.cc::subquery_suite_for_parallel_query`、`pq_subquery_correlated` | `accept-current-code` + `documentation-fix-required` | 历史 WL 标版本；current shape/frequency matrix 可机检 |
| `PQ-GAP-CONF-010` | COUNT DISTINCT 数据流 | WL018 描述 group reshuffle | 当前 worker 发送 `Batch_buffer`，leader 合并 distinct tree | `query_result_mq.cc`、`item_sum.cc`、`pq_agg_distinct` | `accept-current-code` + `documentation-fix-required` | 历史机制降级；current aggregation spec 固化 batch/tree owner |
| `PQ-GAP-CONF-011` | Record buffer sysvar | WL017 声称 `parallel_read_buffer_size` 可配置 | 当前源码未发现该 PQ sysvar，record buffer 走当前 handler/iterator 配置 | 全仓静态 symbol 检索、`WL017` | `accept-current-code` + `documentation-fix-required` | WL 标 design-only；不得在 current docs 暴露不存在变量 |
| `PQ-GAP-CONF-012` | Command scope | 早期总体描述常概括为 SELECT-only | 当前 command gate还允许 INSERT SELECT/REPLACE SELECT 候选，但受 switch、ROW binlog 和锁约束 | `pq_resolver.cc::THD::suite_for_parallel_query`、`check_pq_suite_for_insert_select` | `accept-current-code` + `documentation-fix-required` | current matrix 保持 `partial`，历史材料区分版本 |
| `PQ-GAP-CONF-013` | Read-view 初始化顺序 | 原 codewalk 总流程图把 leader read view 放在 handler/Gather init 前 | 当前直接顺序为 `pq_init_record_gather` → `m_gather->init` → `pq_create_innodb_snapshot` → `pq_launch_worker` | `pq_iterators.cc::ParallelScanIterator::Init` | `accept-current-code` + `documentation-fix-required` | Task 5 修正原 guide；保留两类 read-view 角色说明 |
| `PQ-GAP-CONF-014` | Stored Function | 原 guide/部分历史总结只明确 SP/trigger context | `FUNC_SP` 在 unsupported function list，包含该 Item 的相关 query block 整体 serial-fallback | `pq_optimizer.cc::NO_PQ_SUPPORTED_FUNC_TYPES`、current support matrix | `accept-current-code` + `documentation-fix-required` | 所有 current/legacy 路由统一 PS/SP/FUNC_SP/trigger 四类状态 |
| `PQ-GAP-CONF-015` | Plan cache | 历史 feature 总结没有稳定说明 PQ plan cache 边界 | 当前 plan cache 不支持 PQ plan；PQ 胜出后 invalidate serial cached plan 并 set uncacheable | `sql_parallel.cc::make_pq_leader_plan` | `accept-current-code` + `documentation-fix-required` | current integration spec 保持 rejected；补 cache/reprepare 测试 |
| `PQ-GAP-CONF-016` | 规模/测试数量 | 旧 README/规格记录 28 个 SQL 文件、98 个 PQ tests 等快照 | `stable_branch` 当前为 33 个 PQ 源文件、98 个 tracked test/result-pq；精确值以 commit-bound generated inventory 为准 | 静态 inventory、codewalk | `accept-current-code` | 修改源码或测试时重新生成 inventory，不长期手写数量 |

## 3. 当前 Contract—实现/证据 Hardening Gap

这些条目不是历史文档误差；它们是 current contract 要求与当前静态实现/测试证据之间的
差距。

| GAP-ID | 当前事实 | 风险 | 决策状态 | 关闭条件 |
|---|---|---|---|---|
| `PQ-GAP-HARD-001` | Handler PQ 虚函数默认路径存在 `assert(false)` 后返回成功值 | Release 下 unsupported engine 可能不能 fail closed | `hardening-required`；产品缺陷结论仍需 reachability 证据 | 明确错误返回 + Debug/Release fake-handler/component test |
| `PQ-GAP-HARD-002` | `pq_memory_used[]/total` 是 `uint`，allocation length 是 `size_t` | 截断/回绕使 memory admission 失真 | `hardening-required` + `audit-candidate` | 宽计数实现或边界证明；>4GiB/concurrency tests |
| `PQ-GAP-HARD-003` | Memory gate只看当前 PQ MEM_ROOT global total，未预留预计新增量，且多类 runtime allocation 不计入 | `parallel_memory_limit` 不是完整 hard limit | `product-decision-required` | 定义准入信号还是硬限制；同步 sysvar help、accounting 和 tests |
| `PQ-GAP-HARD-004` | Thread reservation 在 feature-switch/CBO 前发生，失败后依赖 statement cleanup 归还 | 长串行 statement 期间可能虚占 PQ quota | `hardening-required` + `audit-candidate` | 定向并发状态测试；决定提前归还或接受并文档化 |
| `PQ-GAP-HARD-005` | `wait_for_status()` 5 秒循环无整体 deadline；kill 传播/状态条件有阶段限制 | partial launch/error 下可能无界等待 | `hardening-required` + `audit-candidate` | 正式状态机 + before READY/MQ/barrier/handler KILL tests + bounded wait |
| `PQ-GAP-HARD-006` | `pq_stmt_executed` 在 `make_pq_leader_plan` 中于 worker launch 前按采用的 query block 直接 `++`；UNION/多 query block statement 可增加多次，且计数为 global `uint` | 被误解为逻辑 statement 或 worker-run 计数；并发统计可能丢失，整数宽度有限 | `hardening-required` + `audit-candidate` | 将 leader-plan adoption 与 created/effective/READY/row-consumption 指标分离；验证 UNION 和并发语义；采用原子/宽计数或明确 best-effort 语义 |
| `PQ-GAP-HARD-007` | Optimizer rejection 是字符串，多个上下文合并一个 reason | 自动诊断和支持矩阵无法稳定映射 | `hardening-required` | 稳定 reason code + query block/stage + compatibility test |
| `PQ-GAP-HARD-008` | PTRC 与 PQ 有 clone key/cleanup 交叉，但无专用支持矩阵 | 错误结果、cache lifetime、memory cleanup 风险 | `hardening-required` | OFF/ON 四象限、subquery/view、KILL/error/fallback tests |
| `PQ-GAP-HARD-009` | `pq_audit_log` 不断言 event 内容/次数；replica test主要检查 read-only SELECT | retry/worker error/binlog/replay 集成不可证明 | `hardening-required` | audit exact-once + ROW binlog/GTID/replica apply/restart matrix |
| `PQ-GAP-HARD-010` | 104 个 workspace MTR 主题广，但没有统一 Requirement→path→oracle→cleanup run report | 测试存在被误当作发布验证 | `hardening-required` | traceability 每行获得 commit/build-bound runtime artifact |
| `PQ-GAP-HARD-011` | `disabled-pq.def` 存在数十项跨套件禁用 | PFS/main/InnoDB 等回归盲区无法量化 | `hardening-required` | 每项 owner、原因、替代覆盖、到期条件和清零计划 |
| `PQ-GAP-HARD-012` | PS/SP/FUNC_SP/trigger 当前均为相关上下文/query block serial-fallback，但测试与 reason taxonomy 不完整 | force/hint、reprepare 或 nested execution 可能缺回归保护 | `hardening-required` | text/SQL PREPARE/binary PS/reprepare/SP/FUNC_SP/trigger 矩阵 |

## 4. 决策优先级

### P0

- `PQ-GAP-HARD-001`、`005`：fail-closed 和 worker bounded termination；
- `PQ-GAP-HARD-008`：PTRC × clone/shared lifecycle；
- `PQ-GAP-HARD-009`：DML retry/audit/binlog exact-once；
- `PQ-GAP-HARD-010`、`011`：发布追踪与跨套件回归证据；
- `PQ-GAP-HARD-012`：PS/SP/FUNC_SP/trigger 拒绝不可绕过。

### P1

- `PQ-GAP-CONF-004`、`PQ-GAP-HARD-002` 至 `004`：timeout、memory、thread fairness；
- `PQ-GAP-HARD-006`、`007`：统计和 reason taxonomy；
- 历史文档 drift 的 legacy banner 与 current router 收敛。

## 5. 维护规则

1. 关闭 GAP 时必须附 source change 或明确的 product decision，以及绑定 commit/build 的测试
   artifact。
2. `audit-candidate` 在没有复现前不得写成 confirmed product bug。
3. 历史设计若重新实现，必须新建/更新 current contract，不能直接把旧 WL 状态改为 current。
4. PS、SP、Stored Function (`FUNC_SP`) 和 Trigger 的 current 状态统一为相关执行上下文或
   query block `serial-fallback`；任何扩大范围必须先更新 support matrix 和 release gates。
5. 测试文件存在只能关闭“无静态资产”缺口，不能关闭运行验证缺口。
