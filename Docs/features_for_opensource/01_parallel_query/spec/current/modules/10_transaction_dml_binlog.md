# Parallel Query 事务、DML 与 Binlog 当前契约

> 模块 ID：`PQ-MOD-010`
> 状态：`commit-bound-current`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 最后静态核对：2026-07-20
> 最后运行验证：见 `current/quality/verification_evidence.md`

> Snapshot authority：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`。该 manifest 绑定
> implementation commit `1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9`、`stable_branch` 已提交基线、
> `source_tree_sha256=a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> 和 `test_tree_sha256=066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`。
> 两个 hash 只覆盖 manifest 定义的 PQ source/test file set，不覆盖或复现整个 bound PQ source/test tree。

## 1. 职责与非目标

本模块定义 PQ 与事务、锁、一致性读、INSERT/REPLACE SELECT、binlog 和完整串行 retry
之间的当前契约。核心目标是防止以下三类语义被混淆：

1. worker 对 select side 的一致性读；
2. leader 对 target table 的 DML、唯一键、trigger 和 binlog 副作用；
3. 计划初始化失败后，在尚无外部副作用时重新解析并串行执行。

本模块不证明 InnoDB cursor/range 的底层 MVCC 实现；该部分属于
`modules/05_innodb_range_cursor_mvcc.md`。通用 worker error/cleanup 属于
`modules/03_worker_lifecycle_error_retry.md`。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| Command/transaction gate | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query` | 只允许 SELECT/INSERT SELECT/REPLACE SELECT 候选；拒绝 attachable transaction、SERIALIZABLE、PS 和 stored-program context |
| Lock gate | `sql/parallel_query/pq_resolver.cc::Query_block::pq_check_table_list` | 显式 table lock 和 locking clause 不进入 PQ |
| DML/binlog gate | `sql/parallel_query/pq_optimizer.cc::check_pq_suite_for_insert_select` | INSERT/REPLACE SELECT 需要 feature switch 开启且当前 statement 使用 ROW binlog format |
| Leader snapshot | `storage/innobase/handler/ha_innodb_pq.cc::pq_create_innodb_snapshot` | 启动 worker 前确保 leader consistent-read view 可供 clone |
| Worker snapshot | `storage/innobase/handler/ha_innodb_pq.cc::pq_clone_innodb_snapshot` | 为 worker trx 克隆 leader read view |
| Scan context visibility | `storage/innobase/row/row0pread_pq.cc::PQ_Scan_ctx::find_visible_record` | DIV/CUT scan 使用 scan context transaction/read view 判断可见性 |
| Target DML | `sql/sql_insert.cc::Query_result_insert` 及 INSERT/REPLACE command path | leader 消费 Gather 行并执行 target table 写入、唯一键和 trigger 语义 |
| Retry | `sql/sql_parse.cc::retry_without_parallel_query` | 仅对指定初始化错误、未完成执行且未 KILL 的 DML 完整重解析并禁用 PQ |
| Statement cleanup | `sql/sql_class.cc::THD::cleanup_after_query` | 释放 PQ 配额和 MEM_ROOT，恢复 statement/replication/PTRC 相关状态 |

输入包括 leader THD 的 transaction/isolation/binlog 状态、SELECT side 计划、target DML
command、feature switch 以及当前 statement binlog format。输出是：

- 一次只读 PQ SELECT；
- select side 为 PQ、target side 仍由 leader 执行的 INSERT/REPLACE SELECT；
- 明确的串行回退；
- 初始化失败后的完整串行 retry；
- 运行期错误且不得 retry 的失败结果。

## 3. 主流程

```text
command dispatch
  -> THD / Query_block eligibility
     -> reject SERIALIZABLE / attachable trx / explicit locking read
  -> traditional DML/SELECT optimize
  -> check_pq_suite_for_insert_select()
     -> feature switch off: serial-fallback
     -> current statement not ROW binlog: serial-fallback
  -> PQ plan rewrite
  -> ParallelScanIterator::Init()
     -> materialize/scalar inputs
     -> Gather/handler init for DIV/CUT
     -> pq_create_innodb_snapshot(leader)
     -> launch workers
        -> pq_clone_innodb_snapshot(worker, leader)
        -> worker reads SELECT side and sends rows through MQ
  -> leader consumes rows
     -> SELECT: client output/final operators
     -> INSERT/REPLACE SELECT: target write, constraints, trigger, binlog
  -> normal statement commit/rollback and PQ cleanup
```

当前调用顺序必须保持为：

```text
pq_init_record_gather()
  -> m_gather->init()
  -> pq_create_innodb_snapshot()
  -> pq_launch_worker()
```

`m_gather->init()` 可能在构造 DIV/CUT scan context 时触发 leader transaction read view；
随后的 `pq_create_innodb_snapshot()` 负责确保 worker 计划中的其他普通 InnoDB access 也有
可 clone 的 view。这两个角色相关但不等价。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | 分类 | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| Leader THD transaction | 客户连接 | leader THD | leader-local | 普通 transaction/connection lifecycle | 覆盖整个 statement 和最后一个 worker access |
| Leader read view | InnoDB leader trx | leader trx/MVCC | shared source | InnoDB transaction cleanup | worker clone 与 DIV/CUT scan 使用期间不得提前关闭 |
| Worker trx/read view | `pq_clone_innodb_snapshot` | worker THD/trx | worker-local clone | worker THD/InnoDB cleanup | 逻辑快照必须与 leader scan context 相容 |
| `PQ_Scan_ctx::m_trx` | InnoDB leader scan init | scan context 借用 leader trx | borrowed shared | leader scan end | 最后一个 `PQ_Ctx` 读取结束前有效 |
| Target table handler/locks | INSERT/REPLACE command | leader THD | leader-local | DML/statement cleanup | worker 不得直接执行 target write |
| Binlog statement/row cache | server binlog layer | leader transaction | leader-local | commit/rollback | retry 前必须无已提交或不可逆记录 |
| Trigger/SP execution state | stored-program runtime | leader substatement | leader-local | substatement cleanup | trigger 内部 SQL 设置 `in_sp_trigger`，不得递归进入 PQ |
| Retry parser/digest/PSI state | `retry_without_parallel_query` | leader THD | leader mutable | second dispatch cleanup | second attempt 使用原 SQL，PQ 必须禁用 |

## 5. 并发、锁和状态机

### 5.1 锁语义

- `SELECT ... FOR UPDATE`、`FOR SHARE`/`LOCK IN SHARE MODE` 和显式 `LOCK TABLES`
  不属于 PQ consistent read，必须串行回退。
- INSERT/REPLACE SELECT 的 select-from table 在 statement-based replication 下需要锁语义，
  而 PQ worker 没有对应 locking-read 实现，因此只有当前 statement 被判定为 ROW format
  才能进入候选路径。
- Target table 的 write lock、unique-key conflict、auto-increment 和 trigger 仍由 leader 的
  普通 DML 路径负责。
- 并发 DML、purge 和 DDL 不能改变同一 statement 中不同 worker 的逻辑快照。

### 5.2 Retry 状态

```text
PQ planning/init
  -> no error                         -> execute once
  -> graceful-fallback eligible      -> restore serial plan in same dispatch
  -> ER_PARALLEL_FAIL_INIT
       and DML
       and non-fatal
       and not killed
       and !lex->is_exec_completed()
       -> clear first error
       -> close/start statement PSI
       -> reset digest and parser state
       -> suppress duplicate general-log entry
       -> no_pq=true / cost threshold=max
       -> dispatch original SQL once serially
  -> any runtime/side-effect stage error -> propagate; no retry
```

`lex->is_exec_completed()` 是当前 retry 防线，但发布契约必须更强：能产生
`ER_PARALLEL_FAIL_INIT` 的所有位置都必须位于 client rows、target rows、trigger、binlog 和
外部审计副作用之前。

## 6. 规范性不变量与 Requirement

### `PQ-TRX-REQ-001` / `PQ-TRX-INV-001`：一致性快照

同一 statement 的 DIV/CUT scan、worker 普通 InnoDB access 和 leader final access MUST
观察相容的逻辑快照；read view 和 purge protection MUST 覆盖最后一个相关 worker。

### `PQ-TRX-REQ-002` / `PQ-TRX-INV-002`：锁读取拒绝

PQ MUST NOT 执行 locking read。任何 locking clause 或显式 table lock MUST 在 worker
启动前串行回退，且 trace MUST 给出拒绝证据。

### `PQ-TRX-REQ-003` / `PQ-TRX-INV-003`：DML 副作用单一所有者

INSERT/REPLACE SELECT 的 SELECT side 可以由 workers 产生行，但 target write、constraint、
trigger、affected rows 和 binlog side effect MUST 只由 leader 普通 DML 路径执行。

### `PQ-TRX-REQ-004` / `PQ-TRX-INV-004`：ROW binlog gate

Parallel INSERT/REPLACE SELECT MUST 仅在 feature switch 开启且
`is_current_stmt_binlog_format_row()` 为真时采用 PQ；否则 MUST 保持串行锁与复制语义。

### `PQ-TRX-REQ-005` / `PQ-TRX-INV-005`：Retry 无副作用

完整串行 retry MUST 只发生在客户端未收到结果、target table 未写入、trigger 未产生不可逆
副作用且 binlog 未形成可提交事件时。进入 worker runtime 或 target DML 后 MUST NOT retry。

### `PQ-TRX-REQ-006` / `PQ-TRX-INV-006`：错误、warning 与 IGNORE 等价

PQ OFF/ON 对 error code、SQLSTATE、warning、affected rows、last insert id 和
INSERT IGNORE/REPLACE 冲突处理 MUST 等价。

### `PQ-TRX-REQ-007` / `PQ-TRX-INV-007`：日志和 instrumentation 只计一次逻辑语句

Retry 后 general log MUST 不重复写 SQL；slow log、audit、statement PSI、digest、status 和
binlog MUST 能表达一次逻辑 statement 及其 retry 事实，不得产生两个成功副作用记录。

### `PQ-TRX-REQ-008` / `PQ-TRX-INV-008`：拒绝上下文不可绕过

SERIALIZABLE、attachable transaction、Prepared Statement execute、Stored Program/Trigger
内部 SQL 的拒绝 MUST 不受 PQ Hint、force 变量或 retry 影响。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | 能否 fallback/retry | 必须清理的对象 |
|---|---|---|---|---|
| Eligibility/locking/binlog gate | resolver/optimizer | 正常串行执行 | serial-fallback | 不得建立 worker、read-view clone 或 target PQ state |
| Clone/refix 早期失败 | leader planner | 串行执行或初始化错误 | point-of-no-return 前 graceful fallback | leader plan、DIV/CUT、materialization、线程预留 |
| Leader plan/init 抛 `ER_PARALLEL_FAIL_INIT` | leader | 原错误清除后串行重试，或变量关闭时返回错误 | 仅满足 `retry_without_parallel_query` 全部条件时 | 第一 attempt 的 plan、PSI、digest、PQ state |
| Leader/worker read-view 创建或 clone 失败 | InnoDB/PQ runtime | 初始化错误 | 只有最终错误被定义为安全 init error 时可 retry；当前缺专项契约 | worker trx/view、leader scan、MQ、已创建 workers |
| Worker SELECT side runtime error | worker→leader | statement error | 禁止 retry | 全部 worker、MQ、read view、handler |
| Leader target DML/trigger/binlog error | leader DML | 普通 DML error/rollback | 禁止因 PQ 自动 retry | target locks、transaction/binlog cache、trigger state |
| KILL/disconnect | leader/workers | cancel/error | 禁止 retry | workers join、read views、target statement、MQ |

## 8. 支持、限制和 feature switch

| 场景 | 当前状态 | 当前契约 |
|---|---|---|
| Autocommit consistent-read SELECT | `implemented` | 仍需通过全部 PQ gate |
| READ COMMITTED | `partial` | 有 read-view clone 路径，缺完整并发矩阵 |
| REPEATABLE READ | `partial` | 有静态路径和零散测试，显式事务/purge 生命周期未形成发布证明 |
| READ UNCOMMITTED | `unknown` | 当前规范不声明组合支持 |
| SERIALIZABLE | `serial-fallback` | THD gate 硬拒绝 |
| 显式事务 | `partial` | isolation、并发 DML、commit/rollback 需组合验证 |
| XA session | `unknown` | handler worker init 使用 InnoDB trx registration 不等于已批准 XA 支持 |
| Locking read/explicit lock | `serial-fallback` | Query block gate 拒绝 |
| INSERT SELECT | `partial` | `insert_select` switch 默认 off；当前 statement 必须 ROW format |
| REPLACE SELECT | `partial` | 同一 gate；unique conflict/trigger/affected rows 需独立等价验证 |
| INSERT/REPLACE IGNORE | `partial` | error-to-warning 和 scalar subquery 容忍规则存在专门路径 |
| Statement/Mixed 最终未采用 ROW 的 DML | `serial-fallback` | 保持普通串行锁与 binlog 语义 |
| Read-only replica 上 SELECT | `partial` | 存在 read-only/replica 静态测试资产，不代表复制应用线程已验证 |
| 普通 INSERT/UPDATE/DELETE | `serial-fallback` | 不属于 PQ 主 command |

## 9. 可观测性

- Optimizer trace 必须区分 command/transaction/lock 拒绝与 `SWITCH_INSERT_SELECT`。
- EXPLAIN 只能证明 SELECT side 的 Gather；它不能证明 target write、trigger 或 binlog 已被
  并行化，后者本来就应由 leader 执行。
- Slow log 通过 `sql/log.cc` 对 SELECT/INSERT SELECT/REPLACE SELECT 输出
  leader-plan marker `PQ_executed`；该 marker 在 worker launch 前设置，不证明 worker 已运行或
  target DML 已产生副作用。
- Retry 第二次 dispatch 临时设置 `OPTION_LOG_OFF`，用于避免 general log 重复；同时重启
  statement PSI 并重置 digest。
- 当前没有稳定产品字段记录 `graceful-fallback`、`full retry`、首次错误阶段和 target rows
  是否已开始，因此 release gate 必须通过 DBUG、warning、log 和副作用联合判断。

## 10. 代码与测试映射

下列测试是静态 Requirement 映射；suite 运行范围见 `current/quality/verification_evidence.md`，不得据此推导组合级 `verified`。

| Requirement/Invariant | 源码 symbol | 正向测试 | 负向/故障测试 | 证据状态 |
|---|---|---|---|---|
| `PQ-TRX-REQ-001` / `INV-001` | `pq_create_innodb_snapshot`、`pq_clone_innodb_snapshot`、`PQ_Scan_ctx::find_visible_record` | `pq_read_view`、`pq_rec_visible` | customer purge/restore probes | `static-partial` |
| `PQ-TRX-REQ-002` / `INV-002` | `Query_block::pq_check_table_list` | `pq_optimizer_trace` | `parallel_insert_select`、`parallel_replace_select` locking clauses | `static-partial` |
| `PQ-TRX-REQ-003` / `INV-003` | `Query_result_insert`、MQ/Gather path | `parallel_insert_select`、`parallel_replace_select` | behavior-change tests with concurrent DML | `static-partial` |
| `PQ-TRX-REQ-004` / `INV-004` | `check_pq_suite_for_insert_select` | `parallel_insert_select*`、`parallel_replace_select*` | feature-switch off/locking tests | `static-partial`; binlog-format matrix limited |
| `PQ-TRX-REQ-005` / `INV-005` | `retry_without_parallel_query`、`make_pq_leader_plan` | `pq_auto_retry_failed_parallel_query` | `pq_fallback`、`pq_optimizer_trace` DBUG branches | `static-partial` |
| `PQ-TRX-REQ-006` / `INV-006` | worker IGNORE handler、leader DML path | correlated subquery INSERT IGNORE case、parallel DML tests | unique/conflict/concurrent DML cases | `static-partial` |
| `PQ-TRX-REQ-007` / `INV-007` | retry PSI/digest/general-log reset、`sql/log.cc` | `pq_slow_log` | `pq_auto_retry_failed_parallel_query` | `static-gap`; audit/binlog exact-once 未证明 |
| `PQ-TRX-REQ-008` / `INV-008` | `THD::suite_for_parallel_query` | `pq_optimizer_trace` | `pq_sp_trigger`、`pq_prepare` | `static-partial` |

## 11. 已知缺口和变更影响

- `PQ-GAP-TRX-001`：缺少 RU/RC/RR/SERIALIZABLE × autocommit/显式事务 ×
  full/range/ref/reverse × 并发 DML/purge/DDL 的发布矩阵。
- `PQ-GAP-TRX-002`：leader read view、scan-context borrowed trx、worker cloned view 和 purge
  protection 的完整所有权证明尚未闭环。
- `PQ-GAP-TRX-003`：XA 为 `unknown`；worker 内部启动 InnoDB trx 的代码不能作为 XA
  statement 支持证明。
- `PQ-GAP-DML-001`：INSERT/REPLACE SELECT 的 ROW/MIXED/STATEMENT、`sql_log_bin`、GTID、
  rollback、unique conflict、auto-increment 和 trigger 组合缺统一 oracle。
- `PQ-GAP-DML-002`：`ER_PARALLEL_FAIL_INIT` 的所有 raise site 缺机器可检查的“target side
  尚无副作用”证明。
- `PQ-GAP-RETRY-001`：general log 有去重实现，slow log、audit、PFS、digest、binlog 与 status
  的逻辑语句计数仍缺 retry 专项断言。
- `PQ-GAP-LOCK-001`：parallel INSERT/REPLACE SELECT 有意改变 selected-from table 锁行为，
  需要对外支持契约和隔离级别矩阵，而不能只依赖 behavior-change MTR 注释。

修改事务、read view、DML、binlog 或 retry 时，必须联动检查模块 03/05/09/12、普通串行
DML 路径、复制与日志集成，以及质量门禁。
