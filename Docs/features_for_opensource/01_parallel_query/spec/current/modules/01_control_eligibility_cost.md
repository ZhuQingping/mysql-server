# Parallel Query 控制、资格与成本契约

> 模块 ID：`PQ-MOD-001`
> 状态：`current-contract`
> 适用提交：`1f6c4a5cbeab86dddcf8da7cdb3a3c29bb3509a9` (`stable_branch`)
> 快照清单：`Docs/features_for_opensource/01_parallel_query/spec/manifest.yaml`；`working_tree=clean`
> source_tree_sha256：`a44b667674af59160db9442dd779a608af349c88a67712b1f364ce4a8a5af90d`
> test_tree_sha256：`066ac7d1945da34e0d1418973542e5f6f94b33529b996185d0498d182331b9f6`
> 最后静态核对：2026-07-20
> 最后运行验证：见 `current/quality/verification_evidence.md`

哈希覆盖边界：`source_tree_sha256` 仅覆盖 manifest 定义的
`sql/parallel_query/**`，以及 `storage/innobase/include/row0pread_pq.h`、
`storage/innobase/row/row0pread_pq.cc`、`storage/innobase/handler/ha_innodb_pq.cc`。
它不单独 content-hash `sql/handler.cc`、`sql/handler.h`、`sql/table.cc`、
`sql/table.h`、`sql/sql_parse.cc`、`sql/sql_class.cc`、`sql/sql_class.h`、
`sql/sql_lex.h`、`sql/sql_select.cc` 或 TempTable 接入文件；这些和其他外部接入证据仅是
绑定 commit + bound PQ source/test tree 上的 `repository/path::QualifiedSymbol` 静态核对，不能据此
推断整个 workspace 或本模块全部 source evidence 具有可复现 content hash。

本文描述清单绑定 bound PQ source/test tree 的 as-is 契约。行为状态只使用 manifest 的
`status_vocabulary`；源码观察的证据为 `static-evidence`，已找到但未运行的测试证据为
`test-present`，运行验证为 `none`。

## 1. 职责与非目标

本模块负责把会话开关、PQ/NO_PQ hint、语句和查询块资格、传统优化器生成的访问路径、
资源准入、feature switch 与成本估算收敛为一个决定：保留串行计划，或把当前
`Query_block/JOIN` 标记为可进入 PQ 计划克隆和 leader 改写阶段。它还负责选择
`DIV_TAB`，必要时标记 `CUT_TAB`，并把拒绝原因写入 optimizer trace。

本模块不负责：

- 克隆 Item、THD、JOIN 或 AccessPath，也不拥有改写后的 leader 计划；这些属于
  `PQ-MOD-002`。
- 创建、调度、终止或 join worker；这些属于 `PQ-MOD-003`。
- 实现存储引擎的并行扫描、物理 range 切分或 `PQ_Ctx`；这些属于 `PQ-MOD-004`
  及 InnoDB/TempTable 实现。
- 证明查询运行结果正确或性能更优。当前成本模型只提供准入决策，运行验证不在本快照
  的证据范围内。

上游是 parser、resolver、prepare 和传统优化器；下游入口是
`sql/parallel_query/sql_parallel.cc::make_pq_unit_plan()`。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 入口 | `sql/parse_tree_hints.cc::PT_table_level_hint::contextualize` | 解析 table-level `PQ` hint；有参数时把 DOP 限制到 1024，无参数时使用 `parallel_default_dop`，并把 hint 状态写入 `Opt_hints_*` |
| 入口 | `sql/parse_tree_hints.cc::PT_qb_level_hint::contextualize` | `NO_PQ_HINT_ENUM` 把 `THD::no_pq` 置为 true |
| 入口 | `sql/sql_parse.cc::mysql_execute_command` | 在非只读命令且 `pq_master_enable=false` 时禁止 PQ；`force_parallel_execute` 仅在 DOP 为 0 时补默认 DOP，不覆盖 `no_pq` |
| 入口 | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query` | 执行语句级硬资格检查，输出 `PQUnsuiteInfo` |
| 入口 | `sql/parallel_query/pq_resolver.cc::Query_block::suite_for_parallel_query` | 执行查询块级语义资格检查 |
| 入口 | `sql/parallel_query/pq_resolver.cc::Query_block::check_suite_for_pq_after_prepare` | 在优化前保存 GROUP/ORDER 映射，并把 WHERE/HAVING 克隆到 PQ MEM_ROOT |
| 入口 | `sql/parallel_query/pq_optimizer.cc::JOIN::choose_parallel_tables` | 选择并可标记 `DIV_TAB/CUT_TAB` |
| 入口 | `sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | 按资源、表和 feature 顺序作最终准入 |
| 入口 | `sql/parallel_query/sql_parallel.cc::JOIN::is_suit_for_pq_based_cost` | 比较当前串行/并行 access-path 成本 |
| 输入 | `THD` PQ 会话变量和 hint 状态 | 由语句 THD 拥有，有效期覆盖 prepare、optimize、execute 和 statement cleanup |
| 输入 | 已完成传统优化的 `JOIN/QEP_TAB/AccessPath` | 本模块借用；只有传统优化结果满足访问类型限制时才可选择 division table |
| 输入 | `parallel_*` 系统变量 | 读取当前 global/session 值；其单位、默认值和范围属于准入契约 |
| 输出 | `Query_block::parallel_exec`、`THD::has_pq` | 仅最终准入成功时置为 true |
| 输出 | `QEP_TAB::pq_div_tab` 及 CUT 标记 | 供后续 gather 边界、handler 初始化和 worker 计划克隆消费 |
| 输出 | 全局/THD 线程预留计数 | `check_pq_running_threads()` 成功后保留到 statement cleanup |
| 输出 | `PQUnsuiteInfo` 与 `not_apply_pq_plan` trace | 串行回退原因；不是客户端错误 |

`THD::suite_for_parallel_query()` 当前明确拒绝：DOP 为 0、`no_pq`、非
SELECT/INSERT_SELECT/REPLACE_SELECT 命令、prepared statement、stored
procedure/function/trigger 内部 SQL、attachable transaction、SERIALIZABLE、非 InnoDB
实例和 hypergraph optimizer。顶层 SQL 直接调用 stored function 时，
`Item_func::FUNC_SP` 还会命中
`sql/parallel_query/pq_optimizer.cc::NO_PQ_SUPPORTED_FUNC_TYPES`。查询块检查还拒绝
`SQL_BUFFER_RESULT`、ROLLUP、window、多行 VALUES、不支持的 subquery、
INTERSECT/EXCEPT、系统 schema、相关 view/derived、table function、locking、部分临时表
以及不满足查询块或字段约束的 table list。

## 3. 主流程

```text
parse
  PQ hint -> pq_dop / Opt_hints_*
  NO_PQ   -> no_pq=true
    |
prepare / resolve
  THD::suite_for_parallel_query()
  Query_block::suite_for_parallel_query()
  pq_check_table_list()
  check_suite_for_pq_after_prepare()
    | reject: keep serial plan + PQUnsuiteInfo
    v
traditional optimize
  JOIN::choose_parallel_tables(false)        # 探测候选，不作最终标记
    v
final PQ admission
  JOIN::suite_for_parallel_query()
    1. 复核前置资格
    2. get_pq_memory_total() < parallel_memory_limit
    3. choose_parallel_tables(true)          # 标记 DIV/CUT
    4. check_pq_running_threads(dop, timeout)# 预留线程额度
    5. feature switch 检查
    6. is_suit_for_pq_based_cost()
    7. parallel_exec=true; has_pq=true
    | reject: not_apply_pq_plan trace + serial plan
    v
make_pq_unit_plan()                           # PQ-MOD-002
    v
THD::cleanup_after_query()
  release_pq_running_threads()
  clear PQ statement state / PQ MEM_ROOT
```

`JOIN::choose_parallel_tables(bool do_mark)` 先应用基础成本、lateral、字段数、ROLLUP 和
访问类型限制。显式 table PQ hint 优先；否则 `parallel_rows_threshold > 0` 时选择 QEP
顺序中首个达到阈值的候选，阈值为 0 时选择估算行数最大的候选。`do_mark=true` 才写入
`pq_div_tab` 并调用 `MarkCutTable`。

成本判断由 `JOIN::is_suit_for_pq_based_cost()` 完成。当前实现对不可信 row count 直接
准入；重写 access path 成本后，`parl_cost < 0`、`seq_cost < 0`，以及加入 tuple/setup
成本之前的 `parl_cost > seq_cost` 分支也直接准入。其余路径将
`exchange_rows * parallel_tuple_cost + parallel_setup_cost` 加入并行成本，只有串行成本
不低于并行成本才准入。这些是源码现状，不代表已用运行数据证明其经济性。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | worker-local/shared/borrowed | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| `THD::pq_dop/no_pq/has_pq/retry_without_pq` | parser、dispatch、准入逻辑 | statement THD | leader-local | `THD::cleanup_after_query` 重置 | 必须覆盖整条语句及可能的串行重试 |
| `PQUnsuiteInfo` | eligibility 调用者 | 当前调用栈/trace consumer | leader-local | 调用者 | 拒绝原因必须在返回串行路径前可读取 |
| `Query_block::m_suite_for_pq` 与保存的 GROUP/ORDER/谓词 | prepare 检查 | `Query_block`；克隆对象位于 PQ MEM_ROOT | leader-local | PQ MEM_ROOT cleanup | 优化器改写原对象后仍可供计划克隆使用 |
| `JOIN/QEP_TAB` 的 DIV/CUT 标记 | `choose_parallel_tables(true)` | 原始 leader JOIN | leader-local，后续模块借用 | JOIN/statement cleanup | 标记成功后须与后续 gather 边界解释一致 |
| PQ 线程全局计数 | `check_pq_running_threads` | 进程级 PQ 资源统计 | shared | `release_pq_running_threads` 递减 | 一次成功预留对应一次语句清理释放 |
| `THD::pq_threads_running` | 同上 | statement THD | leader-local 记账 | statement cleanup | 记录本语句应返还的额度 |
| PQ memory bucket | PQ 分配/释放路径 | 进程级资源统计 | shared | 各对象销毁及 root cleanup | `get_pq_memory_total()` 只提供准入时快照，不是内存预分配 |

prepare 阶段克隆的 WHERE/HAVING 和映射位于 PQ MEM_ROOT；本模块只建立可克隆前提，
实际模板和改写对象的所有权见 `02_plan_rewrite_clone_ownership.md`。

## 5. 并发、锁和状态机

资格判断和计划标记在 leader 线程内串行进行。线程配额是本模块唯一直接的跨语句共享
并发状态：

- `sql/parallel_query/sql_parallel.cc::check_pq_running_threads()` 在
  `LOCK_pq_threads_running` 下检查/等待并增加全局计数，同时写入当前 THD 的预留数。
- `COND_pq_threads_running` 连接等待者与释放者；释放也必须在同一锁保护下修改计数并唤醒。
- timeout 代码按毫秒拆分为秒和纳秒，而系统变量帮助文字写“microseconds”。在单位统一前，
  文档不能把该变量对外宣称为已经验证的微秒契约。
- 成功预留不等于 worker 已创建；配额直到 `THD::cleanup_after_query()` 才释放。

逻辑状态不是源码 enum，但当前合法转换可归纳为：

```text
UNASSESSED
  -> ELIGIBILITY_REJECTED -> SERIAL
  -> PREPARED_ELIGIBLE
       -> NO_DIVISION_CANDIDATE -> SERIAL
       -> CANDIDATE_MARKED
            -> RESOURCE_REJECTED -> SERIAL
            -> FEATURE_REJECTED  -> SERIAL
            -> COST_REJECTED     -> SERIAL
            -> ADMITTED          -> PQ-MOD-002
```

KILL 不在资格状态机内；若准入期间仍在 condition wait，是否立即响应 KILL 需由该等待实现
和上层语句状态共同保证，本文没有运行证据。重复 cleanup 必须避免二次返还同一额度；当前
静态证据是 cleanup 读取并清理 THD 记账，未做故障注入验证。

## 6. 规范性不变量

- `PQ-CTRL-INV-001`：`THD::no_pq=true` MUST 阻止 PQ；`force_parallel_execute` MUST NOT
  覆盖显式 NO_PQ 或 master-disable 产生的禁止状态。
- `PQ-CTRL-INV-002`：最终接受 PQ MUST 同时满足 THD、Query_block、table/access、资源、
  feature 和成本检查；只有接受路径可设置 `parallel_exec` 与 `has_pq`。
- `PQ-CTRL-INV-003`：prepare 阶段 MUST 在传统优化器破坏或替换表达式映射之前保存
  GROUP/ORDER，并在 PQ MEM_ROOT 克隆需要的 WHERE/HAVING；失败 MUST 保持串行。
- `PQ-CTRL-INV-004`：`DIV_TAB` MUST 是 `pq_check_not_support_tab()` 和
  `TABLE::suite_for_pq_division()` 接受的当前访问路径/引擎组合。
- `PQ-CTRL-INV-005`：线程额度的检查与增加 MUST 在 `LOCK_pq_threads_running` 下原子完成；
  成功预留 MUST 在该语句 cleanup 中返还。
- `PQ-CTRL-INV-006`：任何准入拒绝 SHOULD 保留可执行的串行计划，并 MUST NOT 作为
  worker 运行时错误返回客户端。
- `PQ-CTRL-INV-007`：最终拒绝原因 MUST 通过 `PQ_UNSUITABLE_INFO` 写入
  `not_apply_pq_plan` trace，使“未进入 PQ”与“进入后失败”可区分。
- `PQ-CTRL-INV-008`：成本比较 MUST 使用同一传统计划快照；无效/不可信成本的 fail-open
  分支在改变前 MUST 通过差异测试和回归测试重新定约。
- `PQ-CTRL-INV-009`：`get_pq_memory_total()` MUST 被解释为共享资源的准入快照，
  MUST NOT 被解释为已为本语句保留 `parallel_memory_limit` 容量。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | 能否 fallback/retry | 必须清理的对象 |
|---|---|---|---|---|
| hint/语句/查询块不合格 | resolver/eligibility | 正常执行串行计划 | serial-fallback | prepare 阶段已建的 PQ root 对象在语句 cleanup 清理 |
| prepare 映射或谓词克隆失败 | `sql/parallel_query/pq_resolver.cc::Query_block::check_suite_for_pq_after_prepare` | 当前实现记录 `INTER_ERROR` 原因并保留串行路径 | serial-fallback；不是自动重试 | 部分 PQ MEM_ROOT 分配 |
| 内存门限达到 | `sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | 串行执行，trace 为 `MEMORY_LIMIT` | serial-fallback | 尚无 worker；语句 PQ 状态 |
| 没有可用线程额度/等待超时 | `sql/parallel_query/sql_parallel.cc::check_pq_running_threads` | 串行执行，trace 为 `IDLE_THREAD` | serial-fallback | 未成功预留时无额度；已预留路径由 cleanup 返还 |
| feature switch 不允许 | `sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | 串行执行，trace 为 `DISABLED_IN_PQ_SUPPORT_FEATURES` | serial-fallback | 已做的 DIV/CUT 标记和线程记账随语句结束清理 |
| 并行成本更高 | `sql/parallel_query/sql_parallel.cc::JOIN::is_suit_for_pq_based_cost` | 串行执行，trace 为 `PQ_COST_HIGHER` | serial-fallback | 同上 |
| 后续计划克隆/改写失败 | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan` | 由 `PQ-MOD-002` 的 fallback/`ER_PARALLEL_FAIL_INIT` 契约决定 | 可能原地 fallback 或全语句串行 retry | 见 `PQ-MOD-002/003` |

本模块的正常“不合格”不是 SQL 错误，不触发 `parallel_fail_retry`。自动重试只处理后续模块
产生的精确错误 `ER_PARALLEL_FAIL_INIT`，不能被用来掩盖准入拒绝。

## 8. 支持、限制和 feature switch

| 能力 | 行为状态 | 当前边界 | 证据 | 缺口/Hardening |
|---|---|---|---|---|
| PQ/NO_PQ hint 与 DOP | `implemented` | DOP 0 禁止；hint 参数上限 1024；NO_PQ 优先 | `static-evidence` | `none` |
| SELECT | `implemented` | 还受查询块、table、access、feature 和成本限制 | `static-evidence` | `none` |
| INSERT_SELECT / REPLACE_SELECT | `partial` | 仅命令类型通过第一层检查，仍须满足其余限制和相应 feature switch | `static-evidence` | `none` |
| SQL/Binary Protocol prepared statement | `serial-fallback` | 执行阶段 `lex->in_execute_ps=true`，`PREPARE_TRIGER_PROCEDURE` 原因 | `static-evidence` | `PQ-GAP-HARD-012` |
| Stored Procedure/Function 内部 SQL、Trigger 内部 SQL | `serial-fallback` | 执行期间 `in_sp_trigger=true`，`PREPARE_TRIGER_PROCEDURE` 原因 | `static-evidence` | `PQ-GAP-HARD-012` |
| 顶层 SQL 直接调用 Stored Function | `serial-fallback` | `Item_func::FUNC_SP` 命中 `NO_PQ_SUPPORTED_FUNC_TYPES` | `static-evidence` | `PQ-GAP-CONF-014`、`PQ-GAP-HARD-012` |
| SERIALIZABLE、attachable transaction | `serial-fallback` | 语句级资格检查拒绝 | `static-evidence` | `none` |
| hypergraph optimizer | `serial-fallback` | 当前只绑定传统优化器 | `static-evidence` | `none` |
| InnoDB base table | `implemented` | 还要求允许的访问类型、单个 used partition、无 fulltext 等 | `static-evidence` | `none` |
| TempTable division | `partial` | 当前仅 full scan；详细 handler 边界见 `PQ-MOD-004` | `static-evidence` | `none` |
| ROLLUP/window/INTERSECT/EXCEPT 等 | `serial-fallback` | 由 Query_block 检查拒绝 | `static-evidence` | `none` |
| 简单聚合、相关子查询 | `partial` | 默认 support flags 包含这两项，仍有表达式和计划限制 | `static-evidence` | `none` |
| count distinct、hash spill、insert select 支持位 | `partial` | 当前默认关闭，开启并不绕过其他资格检查 | `static-evidence` | `none` |
| 成本准入 | `implemented` | 默认 `parallel_cost_threshold=1000`、tuple cost 1.5、setup cost 250 | `static-evidence` | 未登记的 CBO `audit-candidate` |
| 线程准入 | `partial` | 默认 max 64、queue timeout 0；timeout 单位存在静态不一致 | `static-evidence` | `PQ-GAP-CONF-004`、`PQ-GAP-HARD-004` |
| 内存准入 | `partial` | 默认 100 MiB；只检查全局统计快照 | `static-evidence` | `none` |

其他静态默认值：`pq_master_enable=true`、`force_parallel_execute=false`、
`parallel_default_dop=4`、`parallel_rows_threshold=10000`、
`parallel_graceful_fallback=true`、`parallel_fail_retry=true`。

## 9. 可观测性

- optimizer trace 的 `not_apply_pq_plan` 及 `PQ_UNSUITABLE_INFO` 是证明被拒绝阶段和原因的
  首选证据；只看到串行 EXPLAIN 不能区分资格、资源、feature 或成本原因。
- `Query_block::parallel_exec` 和 `THD::has_pq` 是内部接受标记；它们不是对客户端稳定的
  成功完成证明。
- `PQ_stmt_executed` 在 leader 计划构造成功时增加，不等价于 worker 已启动或查询已成功
  返回全部结果。
- PQ memory、running threads 等 status 只能辅助观察；判断泄漏需要比较语句前后并结合
  cleanup/fault-injection 测试。
- 当前测试资产包含 `.test` 与 `.result-pq`，但本快照未运行，不能标为 `verified`。

## 10. 代码与测试映射

下表逐项列出真实测试输入和结果路径；这些文件存在，但本任务没有执行它们。

| Requirement/Invariant | repository/path::QualifiedSymbol | 正向测试资产 | 负向/故障测试资产 | 行为状态 | 证据 | 缺口/Hardening |
|---|---|---|---|---|---|---|
| PQ/NO_PQ、DOP 与 master 开关（INV-001） | `sql/parse_tree_hints.cc::PT_table_level_hint::contextualize`、`sql/parse_tree_hints.cc::PT_qb_level_hint::contextualize`、`sql/sql_parse.cc::mysql_execute_command` | `mysql-test/suite/parallel_query/t/pq_variables.test`、`mysql-test/suite/parallel_query/r/pq_variables.result-pq` | `mysql-test/suite/parallel_query/t/pq_master_disable.test`、`mysql-test/suite/parallel_query/r/pq_master_disable.result-pq`、`mysql-test/suite/parallel_query/t/pq_not_support.test`、`mysql-test/suite/parallel_query/r/pq_not_support.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| 语句和查询块硬资格（INV-002/003） | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query`、`sql/parallel_query/pq_resolver.cc::Query_block::suite_for_parallel_query`、`sql/parallel_query/pq_resolver.cc::Query_block::check_suite_for_pq_after_prepare` | `mysql-test/suite/parallel_query/t/pq_prepare.test`、`mysql-test/suite/parallel_query/r/pq_prepare.result-pq` | `mysql-test/suite/parallel_query/t/pq_not_support.test`、`mysql-test/suite/parallel_query/r/pq_not_support.result-pq`、`mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`、`mysql-test/suite/parallel_query/r/pq_optimizer_trace.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| PS/SP/Stored Function/Trigger 串行回退（INV-001/002） | `sql/parallel_query/pq_resolver.cc::THD::suite_for_parallel_query`、`sql/parallel_query/pq_optimizer.cc::NO_PQ_SUPPORTED_FUNC_TYPES` | `mysql-test/suite/parallel_query/t/pq_sp_trigger.test`、`mysql-test/suite/parallel_query/r/pq_sp_trigger.result-pq` | Binary PS 协议级和 trigger restore fault 测试为 `none` | `serial-fallback` | `static-evidence`、`test-present` | `PQ-GAP-HARD-012` |
| DIV table 选择（INV-004） | `sql/parallel_query/pq_optimizer.cc::JOIN::choose_parallel_tables`、`sql/parallel_query/pq_optimizer.cc::pq_check_not_support_tab` | `mysql-test/suite/parallel_query/t/pq_variables.test`、`mysql-test/suite/parallel_query/r/pq_variables.result-pq`、`mysql-test/suite/parallel_query/t/pq_cbo.test`、`mysql-test/suite/parallel_query/r/pq_cbo.result-pq` | `mysql-test/suite/parallel_query/t/pq_not_support.test`、`mysql-test/suite/parallel_query/r/pq_not_support.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| 线程与内存准入（INV-005/009） | `sql/parallel_query/sql_parallel.cc::check_pq_running_threads`、`sql/parallel_query/pq_resource_stat.cc::get_pq_memory_total` | `mysql-test/suite/parallel_query/t/pq_variables.test`、`mysql-test/suite/parallel_query/r/pq_variables.result-pq` | `mysql-test/suite/parallel_query/t/pq_memory_limit.test`、`mysql-test/suite/parallel_query/r/pq_memory_limit.result-pq`、`mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`、`mysql-test/suite/parallel_query/r/pq_optimizer_trace.result-pq` | `partial` | `static-evidence`、`test-present` | `PQ-GAP-CONF-004`、`PQ-GAP-HARD-004` |
| feature switch | `sql/parallel_query/pq_optimizer.cc::JOIN::check_pq_support_features_switch`、`sql/parallel_query/pq_optimizer.cc::JOIN::suite_for_parallel_query` | `mysql-test/suite/parallel_query/t/pq_support_features_switch.test`、`mysql-test/suite/parallel_query/r/pq_support_features_switch.result-pq` | `mysql-test/suite/parallel_query/t/pq_support_features_switch.test`、`mysql-test/suite/parallel_query/r/pq_support_features_switch.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| CBO 决策（INV-008） | `sql/parallel_query/sql_parallel.cc::JOIN::is_suit_for_pq_based_cost` | `mysql-test/suite/parallel_query/t/pq_cbo.test`、`mysql-test/suite/parallel_query/r/pq_cbo.result-pq` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`、`mysql-test/suite/parallel_query/r/pq_optimizer_trace.result-pq` | `implemented` | `static-evidence`、`test-present` | 未登记的 CBO `audit-candidate` |
| 拒绝原因可观测（INV-006/007） | `sql/parallel_query/pq_optimizer.cc::add_reason_to_trace`、`sql/parallel_query/pq_optimizer.cc::PQ_UNSUITABLE_INFO` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`、`mysql-test/suite/parallel_query/r/pq_optimizer_trace.result-pq` | `mysql-test/suite/parallel_query/t/pq_optimizer_trace.test`、`mysql-test/suite/parallel_query/r/pq_optimizer_trace.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |

## 11. 已知缺口和变更影响

当前 `conformance_gaps.md` 已登记本模块的主要 drift 和 hardening 项；
`audit-candidate` 仍不能写成已运行复现缺陷。

- `PQ-GAP-CONF-004` — 行为状态 `partial`；证据 `static-evidence`；缺口类型为 contract
  drift。`parallel_queue_timeout` 的 sysvar 帮助文字写
  microseconds，而 `check_pq_running_threads()` 按 milliseconds 计算绝对超时；外部契约、
  实现和测试注释需要统一。
- `PQ-GAP-HARD-004` — 行为状态 `partial`；证据 `static-evidence`；缺口类型为
  `audit-candidate`。最终准入在
  `choose_parallel_tables(true)` 和线程额度预留之后仍可能因 feature/CBO 拒绝；标记与配额
  保留到 statement cleanup 的副作用和高并发公平性需要运行故障测试确认。
- `PQ-GAP-CONF-014` 与 `PQ-GAP-HARD-012` — 行为状态 `serial-fallback`；证据
  `static-evidence`。PS/SP/Stored Function/Trigger 的当前口径均为
  相关执行上下文或 query block `serial-fallback`；Binary PS、reprepare、nested execution
  和稳定 reason taxonomy 仍需矩阵验证。
- 未单独登记的成本审计项 — 行为状态 `unknown`；证据 `static-evidence`；缺口类型为
  `audit-candidate`。不可信/负成本以及初步
  `parl_cost > seq_cost` 的 fail-open 分支是否符合预期，需要基于 trace、结果一致性与性能
  样本重新验证；若保留为发布门禁，应先在 `conformance_gaps.md` 分配集中 GAP-ID。

### 关联文件（读者导航；下列完整文件路径不构成 stable mapping）

- `sql/parse_tree_hints.cc`、`sql/sql_parse.cc`、`sql/parallel_query/pq_resolver.cc`、
  `sql/parallel_query/pq_optimizer.cc`、`sql/parallel_query/sql_parallel.cc`、
  `sql/parallel_query/pq_resource_stat.h`、`sql/parallel_query/pq_resource_stat.cc` 和 PQ sysvar 定义。
- `00_architecture_overview.md`、`01_current_support_matrix.md` 与
  `conformance_gaps.md`；总览中历史 symbol 与当前实现不一致时应以静态核对后的当前 symbol
  为准并同步修订。
- 至少重跑第 10 节列出的全部精确 `.test` 输入并核对对应 `.result-pq`，并增加 timeout
  单位与晚期拒绝后配额返还的确定性验证。
