# Parallel Query Fault-Injection Release Gates

> 文档 ID：`PQ-QUALITY-FAULT-001`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 运行验证：见 `current/quality/verification_evidence.md`

> Snapshot authority：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`。该 manifest 绑定
> implementation commit `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9`、`stable_branch` 已提交基线、
> `source_tree_sha256=a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> 和 `test_tree_sha256=066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`。
> 两个 hash 只覆盖 manifest 定义的 PQ source/test file set，不覆盖或复现整个 bound PQ source/test tree。

## 1. 目的与证据规则

本门禁定义 PQ 在 allocation、clone、plan rewrite、read view、worker launch、handler、MQ、
final operator、KILL 和 cleanup 失败时必须提交的发布证据。当前工作树已有较多 DBUG/DEBUG
SYNC 和 MTR 资产，但本轮没有运行；因此本文只记录能力与缺口，不给任何 gate 标记通过。

每个 fault case 必须同时验证：

1. 注入点和实际 PQ 路径可证明；
2. 客户端 rows/error code/SQLSTATE/warning 符合契约；
3. graceful fallback、full retry 或 no-retry 决策唯一且符合 point-of-no-return；
4. 所有实际创建的 worker 终止并 join；
5. thread/memory/MQ/handler/read-view/temp/batch/hash/VFD/PTRC 恢复基线；
6. 同一连接后续查询可执行，必要时可再次选择 PQ；
7. general/slow/audit/PFS/digest/binlog 不重复、不丢失且不误报成功。

仅匹配预期错误文本、MTR 未超时或进程未 crash，均不足以通过 gate。

## 2. Point-of-no-return 分类

| 阶段 | 允许行为 | 禁止行为 |
|---|---|---|
| Eligibility/RBO/CBO/resource reject | 保留原 serial plan；记录 reason | 创建 worker、修改 client-visible side effect |
| Clone/refix 早期且 `parallel_graceful_fallback=ON` | 恢复当前 serial plan，继续同一 dispatch | 残留 clone/Item/table/DIV/CUT state |
| Leader rewrite 后期、尚未执行 | 抛 `ER_PARALLEL_FAIL_INIT`；满足条件时完整 serial retry | 在半改写 plan 上继续执行 |
| Shared input/MQ/handler/read-view 初始化 | 失败并清理；只有被批准为安全 init error 时允许 full retry | 已启动依赖对象后无证明地 retry |
| Worker launch/runtime | 传播 error/KILL，detach，join | 串行续跑、返回部分成功 |
| Client row 或 target DML side effect 已产生 | 传播当前结果/error 并 cleanup | 自动 retry 导致重复 row/写入/trigger/binlog |

## 3. Fault Stage Matrix

下列测试和注入点是静态门禁映射；已运行 suite 的范围见 `verification_evidence.md`，
但未运行的专用 fault path 仍不得标为 `runtime-verified`。

| Stage ID | 阶段 | 当前注入/同步证据 | 代表测试 | 必须补充的断言/缺口 | 优先级 |
|---|---|---|---|---|---|
| `PQ-FI-001` | THD/Gather/template allocation | `dup_thd_abort`、`pq_gather_error1..4` | `pq_worker_error`、fallback 类资产 | 每个 allocation owner、serial restore digest、reservation/memory delta | P0 |
| `PQ-FI-002` | Item/table/JOIN clone | `pq_clone_error2`、`pq_dup_tabs_error0`、`dup_select_abort1/2`、`dup_join_abort` | `pq_clone_item`、`pq_refactor_clone`、`refactor_fix_fields` | clone 类型清单、leader pointer isolation、OOM 覆盖 | P0 |
| `PQ-FI-003` | Refix/replace-base | `name_const_refix_error`、`item_func_like_refix_error`、`item_cond_refix_fields_error`、`arg_comparator_clone_error` | `refactor_fix_fields`、`pq_fallback` | 所有 ref slice/outer ref、恢复后 serial result、无半修复 Item | P0 |
| `PQ-FI-004` | Leader rewrite/temp/access path | `pq_leader_abort1/2/3`、`after_pq_leader_plan` sync | `pq_fallback`、`pq_auto_retry_failed_parallel_query`、`pq_optimizer_trace` | 逐点区分 graceful/full retry/no-retry；plan/materialization state digest | P0 |
| `PQ-FI-005` | Shared materialization/scalar subquery | 现有通用 init error 与高级 SQL regression | `pq_subquery`、`pq_divide_derived`、`pq_fallback` | shared temp/scalar 恰好一次、部分初始化、KILL、owner cleanup 注入 | P0 |
| `PQ-FI-006` | MQ allocation/protocol | `pq_mq_error1..6` | `pq_mq_error` | ring wrap、length 已写/payload 中断、receiver/sender detach race、ERROR_MSG ordering | P0 |
| `PQ-FI-007` | Leader read view/worker clone | `pq_skip_fetch_ctx`、`pq_launch_worker_1` sync；无直接 clone-fail 注入 | `pq_read_view`、customer purge/restore probes | read-view create/clone failure、purge protection、各 isolation、cleanup order | P0 |
| `PQ-FI-008` | Handler init/next/end | `ha_pq_next_deadlock`；InnoDB init sync `pq_init_01` | `pq_worker_error`、scan regression assets | init/end error、unsupported handler fail-closed、partial-open/duplicate End | P0 |
| `PQ-FI-009` | 第 0 个/部分 worker create | `pq_worker_error8/9`、`pq_worker_error7`、`pq_wait_launch_worker` | `pq_worker_error`、`pq_hash_join_error` | created-count、thread id validity、unlaunched barrier participant、join/resource exact delta | P0 |
| `PQ-FI-010` | Worker THD/JOIN/result/execute | `pq_worker_abort1/2`、`pq_worker_error1/2/6/10` | `pq_worker_error`、`pq_hash_join_error` | INIT/READY/OVER/ERROR/COMPELET 唯一终态、diagnostics owner、connection reuse | P0 |
| `PQ-FI-011` | Sort/exchange final operator | `pq_msort_error1..4/8/9` | `pq_worker_error`、`pq_order_by` | LIMIT early detach、partial heap/batch state、stable-order oracle | P0 |
| `PQ-FI-012` | Aggregate batch/temp file | `bat_buf_is_null`、low-memory distinct injection | `pq_agg_distinct` | cached-file open/write/read/close error、memory/file baseline、warning equivalence | P0 |
| `PQ-FI-013` | Hash barrier/spill/VFD | worker error series、`nested_loop_init_error`、`force_bloom_filter_miss` | `pq_hash_join_error`、`pq_hash_join` | participant drop、chunk/VFD I/O error、lock order、temp file exact cleanup | P0 |
| `PQ-FI-014` | KILL QUERY/CONNECTION/disconnect | `pq_wait_kill`、launch sync | `pq_kill`、`pq_kill_query`、`pq_mdl_lock` | before READY、MQ blocked、handler/barrier blocked、disconnect、overall bounded join | P0 |
| `PQ-FI-015` | Cursor restart/purge/page mutation | `move_block_yield`；当前没有 restore-window DEBUG SYNC | `pq_read_record_crash`、`pq_restart_after_select`、reverse tests | forward/reverse × delete/split/merge/purge，checksum 不重不漏 | P0 |
| `PQ-FI-016` | Retry side effects/logging | `pq_leader_abort1/2` | `pq_auto_retry_failed_parallel_query`、`pq_slow_log` | target DML/trigger/binlog exact once，audit/PFS/digest/general/slow consistency | P0 |
| `PQ-FI-017` | Thread/memory admission | `no_available_idle_threads`、配置阈值 | `pq_variables`、`pq_memory_limit` | timeout unit、KILL wait、limit boundary、overflow、multi-session fairness | P1 |
| `PQ-FI-018` | Cleanup re-entry/duplicate End | 无统一直接注入 | 零散 worker/MQ/temp regression | 对每类半初始化对象调用 End/cleanup 两次并证明安全或明确禁止第二次 | P0 |
| `PQ-FI-019` | PTRC/plan cache integration | PTRC clone/cleanup 代码；无专用注入 | `pq_bugfix` 单个 PQ+PTRC case | cache key、KILL/error/fallback/retry、worker cleanup、memory baseline | P1 |
| `PQ-FI-020` | Audit/PFS/replication integration | 通用 audit/log/PSI/binlog path | `pq_audit_log`、`pq_replica_enable` | event 内容/次数、worker identity、GTID/binlog/replay、restart | P1 |

## 4. P0 Release Gates

### `PQ-GATE-P0-001`：Clone/Rewrite 可恢复

必须覆盖 `PQ-FI-001` 至 `PQ-FI-004`：

- graceful fallback ON/OFF；
- DOP 1 和 DOP>1；
- simple SELECT、subquery/derived、UNION、aggregate/order/join 各至少一个 shape；
- 最终 serial rows/error/warning 与 PQ OFF 等价；
- plan、MEM_ROOT、thread reservation 和 materialization state 恢复基线。

### `PQ-GATE-P0-002`：Worker 与 MQ 必定终止

必须覆盖 `PQ-FI-006`、`PQ-FI-009`、`PQ-FI-010`、`PQ-FI-014`、`PQ-FI-018`：

- 第 0 个 worker 创建失败、创建一半、偶数 worker 失败；
- KILL before READY、READY 后、阻塞 MQ send/receive、LIMIT early detach；
- 每个实际创建 worker 只有一个最终状态并被 join；
- 测试使用有界 DEBUG SYNC timeout，不依赖固定 sleep 判断正确性。

### `PQ-GATE-P0-003`：Read View 与 Scan 正确性

必须覆盖 `PQ-FI-007`、`PQ-FI-008`、`PQ-FI-015`：

- leader view create、worker clone、handler init/read/end failure；
- RC/RR、autocommit/显式 transaction、并发 DML/purge/DDL；
- clustered/secondary、forward/reverse、covering/non-covering、ICP；
- PQ OFF/DOP 1/2/4/8 checksum，证明 range/cursor 不重不漏。

### `PQ-GATE-P0-004`：SQL Final Operator 不返回部分成功

必须覆盖 `PQ-FI-005`、`PQ-FI-011`、`PQ-FI-012`、`PQ-FI-013`：

- shared materialization、scalar subquery、sort/merge、aggregate batch、hash spill；
- worker 任意阶段 error 后客户端不得收到 silent partial success；
- temp/batch/chunk/VFD 和 shared owner 恢复基线；
- 同一连接后续 query 正常，必要时再次进入 PQ。

### `PQ-GATE-P0-005`：DML Retry 无重复副作用

必须覆盖 `PQ-FI-016`：

- SELECT、INSERT SELECT、REPLACE SELECT 分开；
- plain、IGNORE、unique conflict、target trigger；
- ROW binlog、rollback、GTID 和 replica checksum；
- retry 前后 general/slow/audit/PFS/digest/status 记录；
- target row、trigger side effect 和 binlog event 恰好一次。

### `PQ-GATE-P0-006`：执行上下文拒绝一致

Prepared Statement execute、Stored Procedure 内部 SQL、Stored Function 内部 SQL 和 Trigger
内部 SQL 必须由 stored-program/execute context gate 保持 `serial-fallback`。顶层 SQL 中的
`FUNC_SP` 则由 unsupported-function gate 使包含该 Item 的相关 query block 整体
`serial-fallback`。两类 gate 都不得被 force/hint 绕过，并必须在 text/SQL PREPARE/
binary PS/reprepare/stored-program/triggered DML 组合中分别验证。

## 5. P1 Release Gates

### `PQ-GATE-P1-001`：资源边界与公平性

- thread wait/reject、KILL during wait、CBO reject after reservation；
- memory limit−1/limit/limit+1、大于 4GiB 计数审计、多 session 并发；
- MQ 大行/backpressure、Batch/hash spill、VFD pressure；
- statement 后所有指标/文件恢复基线，普通 OLTP 不发生未批准的饥饿。

### `PQ-GATE-P1-002`：Observability 与集成

- EXPLAIN/TREE/JSON/ANALYZE 与实际 created/effective worker 对齐；
- optimizer reason 可归因；
- plan cache hit/invalidation/reprepare；
- PTRC × PQ OFF/ON 四象限；
- audit/PFS/slow/general 与 retry、worker error 一致；
- binlog/replica apply/restart 等价。

## 6. 确定性要求

- Concurrency 测试 SHOULD 使用 DEBUG SYNC signal/wait 和明确 timeout；固定 sleep 只能用于
  辅助观察，不能作为唯一同步条件。
- 每个 fault case MUST 在注入前证明 query 会进入目标 PQ path，避免“注入未命中却通过”。
- 每个 error case MUST 在清除 DBUG 后运行同一连接健康检查。
- Resource case MUST 保存和恢复修改过的 GLOBAL/SESSION variable，并比较前后基线。
- 无 ORDER BY 的 rows oracle 使用 multiset/checksum；有 ORDER BY 使用严格顺序。
- Error oracle 至少包含 success/error、error code、SQLSTATE、warning 和外部副作用。
- 超时、hang 或 server crash 视为 gate 失败；不得用扩大 timeout 掩盖未解释的等待。

## 7. Release Evidence Artifact

每次 gate 运行报告必须记录：

| 字段 | 要求 |
|---|---|
| Source | 完整 commit SHA、dirty/clean 状态和 patch 标识 |
| Build | Debug/Release/ASAN 等 build 类型、编译器和关键 CMake 开关 |
| Command | build-ninja 下的完整 MTR/GUnit/压力命令 |
| Case | Requirement、FI stage、注入 key、SQL shape、DOP、transaction/resource 维度 |
| Path proof | EXPLAIN/trace/status/DEBUG SYNC 命中证据 |
| Oracle | rows/order/error/warning/side effects |
| Cleanup | workers、threads、memory、MQ、handler、read view、temp/spill/VFD、PTRC 基线 |
| Result | pass/fail/blocked；失败 artifact 和复现命令 |

没有该 artifact 的静态 MTR 文件只能标为 `test-present`。

## 8. 当前门禁状态

| Gate | 当前状态 | 原因 |
|---|---|---|
| `PQ-GATE-P0-001` | `partial-runtime-evidence` | 全套 MTR 已通过，但 clone/fallback 的逐阶段 fault oracle 仍不完整 |
| `PQ-GATE-P0-002` | `not-runtime-verified` | 有 worker/MQ/KILL 资产，缺统一终态/join/resource assertion |
| `PQ-GATE-P0-003` | `not-runtime-verified` | 有 read-view/cursor regression，缺 isolation×concurrency 完整矩阵 |
| `PQ-GATE-P0-004` | `not-runtime-verified` | 有 operator error tests，缺 shared temp/spill 精确 cleanup 证明 |
| `PQ-GATE-P0-005` | `not-runtime-verified` | retry MTR 存在，DML/trigger/binlog/audit exact-once 未闭环 |
| `PQ-GATE-P0-006` | `not-runtime-verified` | context gate 有静态代码，binary PS/reprepare/FUNC_SP/trigger 矩阵不足 |
| `PQ-GATE-P1-001` | `not-runtime-verified` | 资源 limit/fairness/overflow/OLTP 影响缺完整报告 |
| `PQ-GATE-P1-002` | `not-runtime-verified` | PTRC、audit、PFS、replication 集成覆盖不足 |
