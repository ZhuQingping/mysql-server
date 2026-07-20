# Parallel Query 可观测性与集成当前契约

> 模块 ID：`PQ-MOD-012`
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

本模块定义如何证明 PQ 被选择、被拒绝、发生 fallback/retry 或运行失败，并记录 PQ 与以下
子系统的当前边界：

- Traditional/TREE/JSON EXPLAIN 和 EXPLAIN ANALYZE；
- optimizer trace 和全局 status；
- Performance Schema thread/memory/statement instrumentation；
- plan cache、Partial Result Cache（PTRC）；
- general log、slow log、audit log；
- binlog/replication 和 read-only replica；
- thread pool。

本模块不把测试文件存在解释为已验证，也不把 `force_parallel_execute=ON`、PQ Hint、
slow log `PQ_executed` 或 global `PQ_stmt_executed` 解释为 worker 已实际运行。前两项是
候选输入，后两项只证明 leader PQ plan 已构造/采用。证明实际 worker 运行还需要
created/effective workers、launch/READY 和 row-consumption 等指标；当前没有完整稳定产品接口。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| TREE plan | `sql/join_optimizer/explain_access_path.cc` 的 `PARALLEL_SCAN/PQ_BLOCK_SCAN/PQ_REF_SCAN` 分支 | 展示 Gather、DOP、parallel table/index/ref scan |
| PQ timing | `sql/parallel_query/explain_pq_access_path.cc`、`CollectWorkerIterTimingInfo` | EXPLAIN ANALYZE 汇总 template/worker timing |
| Traditional/JSON | `sql/opt_explain.cc`、`sql/opt_explain_json.cc` | 展示 parallel execute、DIV/CUT 和 plan attributes |
| Rejection trace | `sql/parallel_query/pq_optimizer.cc::add_reason_to_trace` | 写入 `not_apply_pq_plan`、select number 和 reason |
| Global status | `sql/mysqld.cc` status table、`sql/parallel_query/pq_resource_stat.cc` | 暴露 thread/memory/refused/statement 全局计数 |
| Leader-plan marker | `sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | worker launch 前设置 `THD::pq_executed` 并递增 `pq_stmt_executed`；表示 leader PQ plan 已构造/采用，不表示 worker 已运行 |
| Plan cache | `sql_parallel.cc::plan_cache::invalidate_cached_plan/set_uncacheable` | PQ 胜出时作废此前决定的 serial cached plan；当前不缓存 PQ plan |
| PTRC clone | `sql/item_subselect.cc` PQ key mapping、`sql/parallel_query/pq_clone.cc` | 将原 PTRC key table 映射到 clone query graph |
| PTRC cleanup | `sql/parallel_query/sql_parallel.cc::pq_free_thd` | worker THD 独立调用 `ptrc::cleanup` |
| Slow log | `sql/log.cc` 的 `PQ_executed` 输出 | SELECT/INSERT SELECT/REPLACE SELECT 记录 leader-plan marker；不是 worker-run marker |
| Retry logging/PSI | `sql/sql_parse.cc::retry_without_parallel_query` | 抑制第二次 general-log SQL，重启 statement PSI，重置 digest |
| Audit error | `sql_parallel.cc::pq_worker_exec` diagnostics 合并路径 | worker error 向 leader/audit 报告时保护 rewritten query 访问 |
| Replication gate | `pq_optimizer.cc::check_pq_suite_for_insert_select` | 只有当前 statement 为 ROW binlog 时允许 parallel INSERT/REPLACE SELECT |
| PFS keys | `sql/mysqld.cc::key_thread_parallel_query`、`sql/psi_memory_key.cc::key_memory_pq_mem_root` | 注册 worker thread 与 PQ MEM_ROOT instrumentation |

## 3. 证明路径

### 3.1 采用 PQ

```text
SQL
  -> EXPLAIN TREE/JSON/Traditional shows Gather or parallel scan
  -> execute statement
  -> statement-specific result/error/warning oracle
  -> correlate leader-plan marker with created/effective worker, launch/READY,
     row-consumption and cleanup evidence
```

EXPLAIN 中以下标记是当前计划证据：

- `Gather: N workers, parallel scan on ...`；
- `Parallel table scan`、parallel index/range/ref lookup；
- AccessPath 类型 `PARALLEL_SCAN`、`PQ_BLOCK_SCAN`、`PQ_REF_SCAN`；
- Traditional/JSON 中的 parallel execute 和 DIV/CUT 信息。

EXPLAIN 是规划证据，不保证 worker 实际成功启动、获得 context 或完成。
`PQ_executed`/`PQ_stmt_executed` 也在 worker launch 之前更新，只能与计划证据一起证明 leader
PQ plan 已构造/采用。实际 worker 运行必须另有 created/effective count、launch/READY、
row-consumption 和 cleanup 证据；当前此证据链是 observability gap。

### 3.2 拒绝、fallback 和 retry

```text
eligibility/RBO/CBO/resource reject
  -> serial plan
  -> optimizer trace: not_apply_pq_plan + select_number + reason

clone/refix graceful fallback
  -> serial plan in same dispatch
  -> trace/warning/path evidence required

ER_PARALLEL_FAIL_INIT full retry
  -> first statement PSI ended
  -> digest/parser reset
  -> second dispatch with no_pq
  -> no duplicate general-log SQL
  -> final error/warning/log + leader-plan marker evidence required;
     marker alone does not prove worker run

worker runtime error
  -> error propagation + worker join
  -> no serial retry
```

当前 reason 主要是可读字符串而非稳定产品枚举；同一个
`PREPARE_TRIGER_PROCEDURE` 分类合并 PS、SP、trigger、attachable transaction 和
SERIALIZABLE。诊断工具不能假定该字符串永久稳定或能精确区分子场景。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | 分类 | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| `PARALLEL_SCAN` + Gather metadata | leader plan rewrite | leader/Gather | plan state | plan/statement cleanup | EXPLAIN 使用期间 template plan 有效 |
| Template worker timing | worker timing merge | Gather/template JOIN | shared aggregate | Gather cleanup | worker 退出前不得销毁；合并必须线程安全 |
| `THD::pq_executed` | leader plan adoption, before worker launch | leader THD | statement-level leader-plan marker | statement dispatch reset | slow log 写出后再复位；不推导 worker run |
| Global PQ counters | server global | server | shared | process lifetime | 并发更新不得丢失或回绕 |
| Optimizer trace object | leader THD | statement trace | leader-local | statement cleanup | 每个 rejected query block 保留 select number/reason |
| Cached serial plan | plan cache | query expression/cache | shared cache state | invalidation/destroy | PQ 计划胜出后不得继续复用该 serial cache entry |
| PTRC parameters/keys | PTRC + clone path | leader/template/worker THD | local or explicitly mapped | `ptrc::cleanup` | clone key 指向对应 clone TABLE；worker cleanup 独立执行 |
| Slow/general/audit event | logging/audit subsystem | leader logical statement | external record | subsystem | retry 不得伪装成两次成功语句；worker 不应产生独立客户端 command |
| PFS worker thread record | PSI thread API | server/PFS | per-worker | thread end | thread start/end 与实际 worker 生命周期一致 |

## 5. 并发与集成边界

- Worker 通过同一个 `key_thread_parallel_query` 注册，但当前 key 带 singleton flag；是否能
  准确表达多个并发 PQ workers 属于 instrumentation 审计项。
- `pq_stmt_executed` 当前是全局 `uint`，`make_pq_leader_plan` 在 worker launch 前直接 `++`。
  它可按被采用的 query block 递增；UNION/多 query block statement 因此可增加多次，
  不能解释为逻辑 statement 数或实际 worker-run 数。并发精确性同样未验证。
- EXPLAIN ANALYZE 将实际 worker timing 合并到 template plan；未创建、提前退出和错误 worker
  必须与实际 created DOP 对齐。
- Plan cache 不支持 PQ plan。普通优化阶段可能先决定 cache，PQ 胜出后必须 invalidate 并把
  query block 标为 uncacheable。
- PTRC 与 PQ 不是互斥的独立黑盒：subquery clone 会映射 PTRC keys，worker THD 还需独立
  cleanup；某些 aggregate context 遇到 PTRC path 会重置 pushed aggregate state。
- Audit command/query 事件属于 leader logical statement。Worker error 可合并 diagnostics，
  但不应被记录成额外客户端 SQL command。
- Worker 当前直接 `mysql_thread_create()`；thread pool 配置不是 PQ admission 或 scheduling
  证据。

## 6. 规范性不变量与 Requirement

### `PQ-OBS-REQ-001` / `PQ-OBS-INV-001`：采用路径可证明

每个声明使用 PQ 的测试或诊断 MUST 提供 Gather/PQ AccessPath 和 leader-plan marker，
并另行证明 created/effective workers、launch/READY、row consumption 以及结果/错误/cleanup。
force 变量、hint、用例文件名、slow log `PQ_executed` 或 global `PQ_stmt_executed`
MUST NOT 单独作为 worker 实际运行的充分证据。

### `PQ-OBS-REQ-002` / `PQ-OBS-INV-002`：拒绝原因可归因

每个稳定 eligibility/RBO/CBO/resource 拒绝 MUST 输出可归因到 query block 和阶段的 reason，
且拒绝后运行状态 MUST 显示未执行 PQ。

### `PQ-OBS-REQ-003` / `PQ-OBS-INV-003`：计数不丢失且语义明确

Global/per-statement 指标 MUST 区分 reservation、created/effective workers、actual PQ
execution、fallback 和 retry；并发更新 MUST NOT 丢失、回绕或把串行 fallback 计为成功 PQ。

### `PQ-OBS-REQ-004` / `PQ-OBS-INV-004`：Plan cache 不复用 PQ 不兼容状态

PQ leader plan 胜出后 MUST invalidate serial cached plan；Prepared Statement execute 当前
MUST serial-fallback，不能通过 cached/reprepared plan 绕过。

### `PQ-OBS-REQ-005` / `PQ-OBS-INV-005`：PTRC 语义与生命周期

PTRC key、cached result 和 cleanup MUST 绑定到正确的 clone query graph；PQ OFF/ON MUST
保持 rows/error/warning，KILL/error/fallback 后 MUST 不残留 worker PTRC state。

### `PQ-OBS-REQ-006` / `PQ-OBS-INV-006`：日志表达一次逻辑语句

General/slow/audit/PFS/digest MUST 对 graceful fallback 和 full retry 给出一致、无重复成功
副作用的记录；worker MUST NOT 伪装为独立客户端 command。

### `PQ-OBS-REQ-007` / `PQ-OBS-INV-007`：执行上下文状态一致

PS、SP 内部 SQL、Stored Function 相关 query block 和 Trigger 内部 SQL 的观测 MUST 与
`serial-fallback` 契约一致；其中 `FUNC_SP` 所在相关 query block 整体不得显示 Gather。

### `PQ-OBS-REQ-008` / `PQ-OBS-INV-008`：复制语义可验证

Parallel INSERT/REPLACE SELECT MUST 只在 ROW binlog gate 后执行，binlog event、GTID、
replica result 和 source affected rows MUST 与串行执行等价；read-only replica SELECT 与
replication apply thread 行为必须分别声明。

## 7. 错误、fallback、retry 和 cleanup

| 阶段 | 需要保留的观测 | 禁止行为 | Cleanup/重置 |
|---|---|---|---|
| Eligibility/RBO/CBO reject | query block + reason + serial plan | 计入成功 PQ statement | 清除候选 PQ state |
| Graceful fallback | 原拒绝/失败阶段、最终 serial plan | 伪装为从未尝试且无法诊断 | 恢复 cached/materialization/plan state |
| Full retry | 首次 init error、retry 标志、最终 serial 结果 | general log 重复 SQL、两次 binlog side effect | 重启 PSI/digest，复位 PQ flags |
| Worker runtime error | worker/phase、客户端 error、join/cleanup 状态 | 串行 retry、部分成功 | worker PTRC、PFS thread、MQ、status 合并 |
| EXPLAIN ANALYZE error/KILL | created workers 和 partial timing | 把 requested DOP 当全部完成 | template timing 和 worker records cleanup |
| Plan cache/PTRC setup failure | cache decision、fallback stage | 复用半改写 cache entry | invalidate/cleanup clone cache state |

## 8. 支持、限制和 feature switch

| 集成 | 当前状态 | 当前契约 |
|---|---|---|
| Traditional EXPLAIN | `implemented` | 显示 parallel execute/DOP/DIV/CUT |
| EXPLAIN TREE/JSON | `implemented` | 展示 Gather 和 PQ scan path |
| EXPLAIN ANALYZE | `implemented` | 有 worker timing 汇总；错误/partial worker 格式稳定性仍需验证 |
| Optimizer trace rejection | `implemented` | `not_apply_pq_plan` + reason；reason taxonomy 不够细 |
| Global status | `implemented` | 线程、内存、refused、statement；均为全局粒度 |
| Per-statement worker/range/MQ/skew | `unknown` | 当前没有完整稳定接口 |
| Plan cache PQ plan | `rejected` | PQ 胜出后 invalidate/uncacheable；当前不缓存 PQ plan |
| Prepared Statement PQ | `serial-fallback` | execute context gate 拒绝 |
| Stored Procedure/Function/Trigger 内部 SQL | `serial-fallback` | `in_sp_trigger` gate 拒绝 |
| 顶层 Stored Function 相关 query block | `serial-fallback` | `FUNC_SP` unsupported gate 拒绝整个相关 query block |
| PTRC × PQ | `partial` | 存在 clone key 和 worker cleanup 特殊路径，缺完整支持矩阵 |
| Slow log `PQ_executed` | `implemented` | 对 SELECT/INSERT SELECT/REPLACE SELECT 写 leader-plan marker；不证明 worker run |
| General log retry suppression | `implemented` | second dispatch 临时 `OPTION_LOG_OFF` |
| Audit log × PQ | `partial` | 有测试资产和 worker error 保护代码，缺内容/次数断言 |
| PFS thread/memory | `partial` | 注册 key；跨套件与多 worker 精度未完成发布证明 |
| ROW binlog parallel DML | `partial` | gate 存在；event/GTID/replica 等价矩阵不足 |
| Read-only replica SELECT | `partial` | 有静态 MTR 资产；不代表 apply-thread DML 支持 |
| Thread-pool dispatch | `design-only` | 当前直接创建 worker threads |

## 9. 最小观测组合

| 目标 | 必须采集 |
|---|---|
| 证明 leader PQ plan 已采用 | EXPLAIN TREE/JSON 中 Gather + slow log `PQ_executed` 或受控 `PQ_stmt_executed` delta，并说明 query-block/UNION 计数语义 |
| 证明 worker 实际运行 | created/effective worker count + launch/READY + row-consumption + statement result/error + join/cleanup；当前无完整稳定产品接口 |
| 证明 serial rejection | optimizer trace reason + 无 Gather 的最终 plan + 串行结果 |
| 证明 graceful fallback | 注入点/失败阶段 + 最终 serial plan + warning/error + 资源基线 |
| 证明 full retry | `ER_PARALLEL_FAIL_INIT` 注入 + retry warning/flag + general/slow/audit/PFS 记录 + 单次副作用 |
| 证明 runtime error | worker/handler/MQ 注入 + 客户端 error + 无 retry + workers/resources cleanup |
| 证明 PTRC 兼容 | PQ/PTRC OFF/ON 四象限 + plan cache/PTRC plan evidence + result/error + cleanup |
| 证明复制等价 | source plan + ROW binlog events/GTID + replica rows/checksum + source/replica error |

## 10. 代码与测试映射

下列测试是静态 Requirement 映射；suite 运行范围见 `current/quality/verification_evidence.md`，不得据此推导组合级 `verified`。

| Requirement/Invariant | 源码 symbol | 正向测试 | 负向/故障测试 | 证据状态 |
|---|---|---|---|---|
| `PQ-OBS-REQ-001` / `INV-001` | EXPLAIN PQ AccessPath branches、leader-plan marker | `pq_explain`、`pq_explain_tree/json/analyze` | `pq_optimizer_trace` | `static-gap`; 无完整 worker-run 证据链 |
| `PQ-OBS-REQ-002` / `INV-002` | `add_reason_to_trace`、`PQ_UNSUITABLE_INFO` | `pq_optimizer_trace`、`pq_opt_trace` | resource/unsupported cases in same tests | `static-partial` |
| `PQ-OBS-REQ-003` / `INV-003` | global status、`pq_stmt_executed` | `pq_variables`、`pq_examined_rows` | `pq_memory_limit` | `static-gap`; 并发精度/per-statement 缺失 |
| `PQ-OBS-REQ-004` / `INV-004` | `invalidate_cached_plan/set_uncacheable` | plan/cache-related regression assets | `pq_prepare`、`pq_optimizer_trace` | `static-partial`; PS protocol 未独立覆盖 |
| `PQ-OBS-REQ-005` / `INV-005` | `Item_subselect` PTRC mapping、`ptrc::cleanup` | `pq_bugfix` 的 PQ+PTRC case | none dedicated | `static-gap` |
| `PQ-OBS-REQ-006` / `INV-006` | retry log/PSI reset、`sql/log.cc` | `pq_slow_log` | `pq_auto_retry_failed_parallel_query`、`pq_audit_log` | `static-partial`; audit test 不断言事件内容 |
| `PQ-OBS-REQ-007` / `INV-007` | PS/SP gate、`FUNC_SP` unsupported list | `pq_sp_trigger` | `pq_optimizer_trace` | `static-partial`; reason 分类合并 |
| `PQ-OBS-REQ-008` / `INV-008` | ROW gate、binlog/replication path | `pq_replica_enable`、parallel insert/replace tests | non-ROW fallback cases | `static-gap`; event/GTID/replay 未形成矩阵 |

## 11. 已知缺口和变更影响

- `PQ-GAP-OBS-001`：缺 requested/created/effective DOP、per-worker rows/bytes/skew、range split、
  MQ wait/peak、fallback/retry stage 和 cleanup result 的 per-statement 接口。
- `PQ-GAP-OBS-002`：拒绝原因是字符串 taxonomy，且 PS/SP/trigger/transaction 合并到一个
  reason；自动诊断缺稳定 reason code。
- `PQ-GAP-OBS-003`：`THD::pq_executed` 和 `pq_stmt_executed` 在 `make_pq_leader_plan`
  中于 worker launch 前更新，只表示 leader PQ plan 已构造/采用；后者可按
  query block 递增，UNION/多 query block 逻辑 statement 可增加多次，且存在全局
  `uint`、非原子 `++` 的并发精度/宽度缺口。当前还没有完整稳定的
  created/effective worker、launch/READY、row-consumption 和 cleanup per-statement 指标，
  因此 slow log `PQ_executed`/global `PQ_stmt_executed` 不得当作实际 worker-run 证据。
- `PQ-GAP-INT-PLAN-001`：当前 plan cache 不支持 PQ plan；缺 cache hit/miss、invalidated plan、
  fallback/reprepare 的独立矩阵。
- `PQ-GAP-INT-PTRC-001`：PTRC 与 PQ 有真实 clone/cleanup 交叉，但只有零散 bug case；缺
  四象限、subquery/view/materialization、KILL/error/fallback 和 memory 生命周期测试。
- `PQ-GAP-INT-AUDIT-001`：`pq_audit_log` 运行 SQL 和连接，但没有静态可见的 audit event
  内容、次数、worker error 或 retry 去重断言。
- `PQ-GAP-INT-PFS-001`：PFS 相关跨套件有 disabled-pq 项，且 single key 对多 worker 的
  表达精度需要验证。
- `PQ-GAP-INT-REPL-001`：`pq_replica_enable` 主要检查 read-only replica 上 SELECT；未证明
  parallel DML binlog event、GTID、replica apply 和 crash/restart 等价。
- `PQ-GAP-INT-TP-001`：历史 thread-pool 设计与当前 direct thread creation 不一致；当前
  只能标为 `design-only`。

修改 EXPLAIN、trace、status、cache、PTRC、logging、audit、replication 或 worker 创建方式时，
必须同步模块 01/03/09/10/11、支持矩阵、traceability 和 release gates。
