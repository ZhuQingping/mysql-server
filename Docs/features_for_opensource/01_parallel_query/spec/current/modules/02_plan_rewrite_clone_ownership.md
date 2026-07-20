# Parallel Query 计划改写、克隆与所有权契约

> 模块 ID：`PQ-MOD-002`
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

本模块负责在传统优化器已经接受 PQ 后：

- 为一个 simple query expression，或由 simple query block 组成的 UNION ALL，决定
  `SEQ_EXEC/PARL_EXEC/ABORT_EXEC`。
- 备份仍可执行的串行 leader 计划，在 PQ MEM_ROOT 上克隆 template THD、Query_block、
  JOIN、QEP_TAB、TABLE、Item 和 AccessPath 关系。
- 创建 `Gather_operator` 及 worker manager 槽位，改写 leader 的首个 QEP_TAB、临时表和
  access path，使 gather 以下由 worker 模板执行，以上继续由 leader 执行。
- 规定 graceful fallback 的明确终点，以及模板、改写对象、共享对象和借用指针的所有权。

本模块不负责 PQ 资格、DIV/CUT 选择和成本准入（`PQ-MOD-001`），不负责真正创建线程、
执行 worker、KILL 或 join（`PQ-MOD-003`），也不实现 handler 的物理切分和取行
（`PQ-MOD-004`）。它不承诺任意新增 Item 自动可克隆；每个受支持具体 Item 类型都必须
提供与其语义匹配的 `pq_clone()` 和必要的 `refix_fields()`。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 入口 | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan` | 对 query expression 逐个调用最终资格和 leader 改写，并在最后重建 unit access paths |
| 入口 | `sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | 为单个已选中 `DIV_TAB` 的 JOIN 克隆 template 并改写 leader |
| 入口 | `sql/parallel_query/sql_parallel.cc::make_pq_gather_operator` | 创建 template THD/JOIN、克隆 QEP_TAB、建立 readinfo 并分配 Gather/worker manager |
| 入口 | `sql/parallel_query/pq_clone.cc::pq_make_join` | 克隆 Query_block/Item/JOIN 图并执行 PQ prepare |
| 入口 | `sql/parallel_query/pq_clone.cc::pq_dup_tabs` | 克隆 QEP_TAB、POSITION、TABLE/Table_ref、ref/range/condition 等执行结构 |
| 输入 | 已准入的原始 `JOIN` | leader 主 MEM_ROOT 拥有；`idx_div_tab` 必须可由标记解析，CUT 可选 |
| 输入 | prepare 阶段保存的 GROUP/ORDER、WHERE/HAVING | 由 `PQ-MOD-001` 建立，供模板表达式解析和 refix 使用 |
| 输入 | `THD::pq_dop`、`parallel_graceful_fallback` | 决定 manager 数量和失败是否还能恢复串行计划 |
| 输出 | `PQ_exec_status` | `SEQ_EXEC` 保留串行；`PARL_EXEC` 交给执行器；`ABORT_EXEC` 要求语句错误/重试处理 |
| 输出 | `Gather_operator` 与 template JOIN | leader PQ MEM_ROOT 生命周期内有效，成功时挂到改写后的首个 QEP_TAB 和 `THD::pq_gathers` |
| 输出 | 改写后的 `qep_tab1/ref_items1/tmp_fields1` 与 access paths | leader PQ 执行图；成功后替代活动指针，串行副本仍由 `*0` 保存 |

`sql/parallel_query/sql_parallel.h::PQ_exec_status` 的当前值是
`SEQ_EXEC=0`、`PARL_EXEC`、`ABORT_EXEC`。它是计划阶段结果，不是 worker 完成状态。

## 3. 主流程

```text
optimize complete / PQ-MOD-001 admitted
    |
make_pq_unit_plan(unit, leader_thd)
    +-- simple query block
    |     JOIN::suite_for_parallel_query()
    |       -> make_pq_leader_plan()
    +-- UNION ALL of simple query blocks
    |     repeat the same operation for each eligible JOIN
    +-- INSERT/REPLACE SELECT
          temporarily remove target table from name-resolution context
          normal non-ABORT tail restores context before return
          ABORT_EXEC from make_pq_leader_plan returns before that restore block
    |
make_pq_leader_plan(join, thd)
    1. backup_leader_plan()                 # graceful 模式才做完整 backup/digest
    2. switch thd->mem_root to pq_mem_root
    3. resolve DIV/CUT and mark handler::do_parallel_scan
    4. make_pq_gather_operator()
         pq_new_thd(leader)                 # template THD
         pq_make_join(template_thd, join)
         pq_dup_tabs(template_join, join, true)
         setup tmp info / pq_make_join_readinfo()
         unlink temporary Query_block clone links
         allocate Gather + DOP managers
    5. point of no return:
         graceful_fallback=false
         exec_code=ABORT_EXEC
    6. JOIN::make_leader_rewritten_tab()
    7. first rewritten tab->gather = gather; push THD::pq_gathers
    8. restore optimized vars / leader tmp table / tmp info
    9. JOIN::create_access_paths()
   10. invalidate and mark plan cache uncacheable
    |
make_pq_unit_plan(): unit->create_access_paths(thd)
    |
PARL_EXEC -> PQ-MOD-003
```

`pq_make_join()` 内部通过 PQ 专用 select prepare 路径调用 `pq_replace_base_item()`、
`setup_fields()`、GROUP/WHERE/ORDER 解析和具体 `Item::*::refix_fields()`。当前源码没有一个
统一的 `pq_refix_fields` 函数；契约应引用 `pq_select_prepare` 和各 Item 的
`refix_fields()` 实现。`Item::pq_clone()` 的基类默认返回 null，具体受支持类型必须覆盖。

成功创建 Gather 后，源码立即关闭 graceful fallback，再调用
`JOIN::make_leader_rewritten_tab()`。这条赋值边界是当前明确的 point of no return：边界前
在启用 graceful 的情况下允许恢复串行，边界后任何失败都走 `ABORT_EXEC`。

对 INSERT/REPLACE SELECT，当前静态控制流只证明到达函数尾部的非 ABORT 路径会恢复
target table 和 `Name_resolution_context_state`。simple/UNION 分支在
`make_pq_leader_plan()` 返回 `ABORT_EXEC` 时立即 return，位于 restore block 之前；该路径
是否由更外层 cleanup 完整恢复目前是 `unknown`，属于 `audit-candidate`，本文不把它表述为
已运行复现的缺陷。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | worker-local/shared/borrowed | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|
| 原始 `qep_tab0/ref_items0/tmp_fields0` | 传统优化器 | leader 主 MEM_ROOT/JOIN | leader-local，fallback 借用 | 原 JOIN/主 MEM_ROOT | graceful rollback 完成前不得被不可逆修改 |
| 改写 `qep_tab1/ref_items1/tmp_fields1` | `JOIN::make_leader_rewritten_tab` 等 | leader PQ MEM_ROOT/JOIN | leader-local | JOIN destroy/PQ root cleanup | 从 point of no return 到执行结束保持一致 |
| serial backup 字段 | `Query_block::pq_backup`、`JOIN::pq_backup` | 原 Query_block/JOIN | leader-local | `pq_restore` 或 JOIN cleanup | 只在 graceful 模式有恢复意义 |
| template `THD` 对象 | `pq_new_thd(leader)` | 对象内存位于 leader PQ MEM_ROOT；自身资源由其析构路径拥有 | leader 与 worker 构图阶段 shared template | `pq_free_thd` | 必须晚于所有 worker plan/线程销毁 |
| template Query_block/JOIN/Item/Table 图 | `pq_make_join`、`pq_dup_tabs` | template THD 的内存根与 JOIN | shared template；worker 克隆源 | `pq_free_join` | worker plan 创建期间保持只读语义模板 |
| `Gather_operator` | `make_pq_gather_operator` | leader PQ MEM_ROOT；leader QEP_TAB 挂接 | leader-owned/shared coordinator | `pq_free_gather`，成功路径由 `JOIN::destroy` 触发 | 必须覆盖 handler ctx、全部 worker 和 exchange 生命周期 |
| `PQ_worker_manager[DOP]` | `make_pq_gather_operator` | Gather/leader PQ MEM_ROOT | 每槽协调对象，运行时 leader/worker shared | `pq_free_gather` | 线程退出并 join 后才可析构 |
| `m_pq_hash_join_shared_context` | Gather prepare/执行准备 | Gather 的 destroy-only unique owner，分配于 leader PQ MEM_ROOT | workers 借用内部 raw pointer | Gather destroy | 所有 hash worker/iterator 停止后才能销毁 |
| `m_uncorrelated_subqueries` 中的 Item 指针 | 从原 leader Query_block 收集 | 原 leader Item 图 | borrowed by Gather | 原 leader Item owner | worker 启动前由 leader 求值；指针不得越过语句生命周期 |
| Query_block 原件/克隆双向链接 | `Query_block::pq_link_clone` | 两个 Query_block 临时共同维护 | borrowed raw links | clone 调用 `pq_unlink_clone` | 仅在 clone/readinfo 对照期间有效，之后必须断链 |
| template diagnostics area | Gather | Gather | leader/worker shared under later synchronization | Gather destroy | worker 合并诊断前保持有效 |

`pq_free_gather()` 的销毁顺序是 template JOIN、各 manager、Gather、template THD，并在
切换 `current_thd` 后恢复 leader globals。成功路径中 `sql/sql_select.cc::JOIN::destroy`
从 `qep_tab1[i].gather` 进入该释放函数。任何新增 raw pointer 都必须归入这个顺序，而不能
只依赖 MEM_ROOT 批量释放掩盖析构要求。

template THD 对象本身分配在来源 THD 的 `pq_mem_root`，但新 THD 构造后拥有自己的资源和
PQ root；后续 worker THD 对象又分配在 template THD 的 PQ root。这个嵌套关系不表示
父 MEM_ROOT 自动执行所有 C++ 析构，因此 `pq_free_join/pq_free_thd/pq_free_gather` 的显式
顺序是契约的一部分。

## 5. 并发、锁和状态机

计划克隆和 leader 改写发生在 worker 创建之前，当前由 leader 线程串行执行；此阶段没有
worker 与其并发访问 template。`Query_block::pq_link_clone()` 建立双向 raw pointer，
`pq_make_join_readinfo()` 完成后由 clone 调用 `pq_unlink_clone()`；该链接没有锁，合法性的
前提就是单线程构图和短生命周期。

计划阶段状态机：

```text
SERIAL_READY
  -> SERIAL_BACKED_UP                  # graceful=true
  -> CLONING_TEMPLATE
       -> clone/gather failure
            graceful=true  -> RESTORE_SERIAL -> SEQ_EXEC
            graceful=false -> ABORT_EXEC + ER_PARALLEL_FAIL_INIT
       -> GATHER_READY
            -> POINT_OF_NO_RETURN      # graceful=false; exec_code=ABORT_EXEC
                 -> LEADER_REWRITTEN
                      -> ACCESS_PATH_READY -> PARL_EXEC
                 -> any failure -> ABORT_EXEC + ER_PARALLEL_FAIL_INIT
```

`PQ_exec_status` 与 worker 状态机无共享取值；`PARL_EXEC` 只表示执行图已经可交给执行器。
KILL、MQ detach 和线程终态见 `PQ-MOD-003`。若计划失败，必须在任何 worker 可观察 Gather
之前完成 restore/free；当前架构通过“构图先于 launch”建立该 happens-before。

## 6. 规范性不变量

- `PQ-PLAN-INV-001`：`make_pq_leader_plan()` MUST 仅接收已选定有效 DIV_TAB 的 JOIN；
  CUT_TAB 若存在 MUST 与 template/leader 使用相同边界解释。
- `PQ-PLAN-INV-002`：graceful 模式在 point of no return 之前 MUST 只把新对象分配到
  PQ MEM_ROOT，MUST 保持主 MEM_ROOT 中串行计划可恢复。
- `PQ-PLAN-INV-003`：成功 `make_pq_gather_operator()` 后、调用
  `make_leader_rewritten_tab()` 前 MUST 将 fallback 状态切换为 `ABORT_EXEC`；边界后
  MUST NOT 返回 `SEQ_EXEC`。
- `PQ-PLAN-INV-004`：每个可进入 worker 图的具体 Item MUST 提供语义等价的 clone，
  并在 `pq_select_prepare` 中完成所需 refix；基类 null 返回 MUST 被当作构图失败。
- `PQ-PLAN-INV-005`：原 Query_block 与 clone 的双向链接 MUST 在 readinfo 完成后断开，
  MUST NOT 留下指向已销毁 clone 的 `pq_last_clone/pq_is_clone_of`。
- `PQ-PLAN-INV-006`：template、worker clone 和 leader 改写对象 MUST 使用各自约定的
  MEM_ROOT；borrowed pointer 的 owner MUST 比最后一个使用者活得更久。
- `PQ-PLAN-INV-007`：`Gather_operator` MUST 在所有 worker 退出并且 handler/MQ 不再使用
  其共享对象后销毁；manager 和 template THD MUST NOT 提前释放。
- `PQ-PLAN-INV-008`：计划构造错误在返回 `ABORT_EXEC` 时 MUST 暴露
  `ER_PARALLEL_FAIL_INIT`；`make_pq_unit_plan()` MUST 把最后的 access-path 构造错误
  归一化为该错误。
- `PQ-PLAN-INV-009`：成功 PQ 改写 MUST 使 plan cache 失效并标记 uncacheable，
  MUST NOT 缓存带 statement-root raw pointer 的 PQ 执行图。
- `PQ-PLAN-INV-010`：`PQ_stmt_executed` 只 SHOULD 被解释为 leader PQ 计划已生成，
  MUST NOT 被解释为整条查询成功完成。
- `PQ-PLAN-INV-011`（hardening target，当前行为 `unknown`）：INSERT/REPLACE SELECT 的
  target table 与 `Name_resolution_context_state` MUST 在 `make_pq_unit_plan()` 的所有返回
  分支恢复，包括 `ABORT_EXEC` 早退；当前源码静态路径尚未证明该约束已满足。

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | 能否 fallback/retry | 必须清理的对象 |
|---|---|---|---|---|
| template THD/JOIN/Item/QEP 克隆失败，graceful 开启 | `sql/parallel_query/sql_parallel.cc::make_pq_gather_operator`、`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | 恢复并执行串行计划 | 原地 `SEQ_EXEC` fallback | 部分 template JOIN/THD、manager、Gather、clone link、PQ root 临时对象 |
| 同一阶段，graceful 关闭 | `sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | `ER_PARALLEL_FAIL_INIT` | 可由 `PQ-MOD-003` 的整语句 retry 策略处理 | 同上，但不得假装恢复串行计划 |
| Gather 成功后的 leader rewrite/tmp table 失败 | `sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | `ER_PARALLEL_FAIL_INIT` | 不可原地 graceful fallback；可能整语句串行 retry | Gather/template/managers、派生物化状态、active pointers、PQ root |
| INSERT/REPLACE SELECT 的 leader plan 返回 `ABORT_EXEC` | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan` | 返回初始化错误并可能由上层判定 retry | name-resolution context 恢复为 `unknown`；只记录审计风险 | target table list、`Name_resolution_context_state`、第一次尝试的 PQ 对象 |
| unit access path 重建失败 | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan` | 归一化为 `ER_PARALLEL_FAIL_INIT` | 可能整语句串行 retry | 已生成的 PQ 图最终由语句/JOIN 销毁路径回收 |
| PARL_EXEC 后 worker/handler 错误 | 执行模块 | 见具体运行错误 | 不是本模块的原地 fallback | 见 `PQ-MOD-003/004` |

错误路径先恢复 `thd->mem_root` 和 derived materialize 状态，必要时
`pq_free_gather()`、清空已挂接的 `tab->gather`、恢复 `join->fields`，再调用
`restore_leader_plan()`。只有 `graceful_fallback` 仍为 true 时，该函数才执行完整
`Query_block::pq_restore()` 与 `JOIN::pq_restore()`；否则只把活动数组指针指回 `*0` 并
返回错误。

整语句 retry 是重新 parse/optimize 的第二次执行，不是从改写中点继续，详见
`03_worker_lifecycle_error_retry.md`。

## 8. 支持、限制和 feature switch

| 能力 | 行为状态 | 当前边界 | 证据 | 缺口/Hardening |
|---|---|---|---|---|
| simple query expression | `implemented` | 仍要求相应 JOIN 通过 `PQ-MOD-001` | `static-evidence` | `none` |
| UNION ALL of simple query blocks | `partial` | 逐 query block 改写；其他 set operation 被拒绝 | `static-evidence` | `none` |
| INSERT_SELECT / REPLACE_SELECT context：正常非 ABORT 路径 | `implemented` | 临时移除 target table，到达函数尾部时恢复 name-resolution context | `static-evidence` | `none` |
| INSERT_SELECT / REPLACE_SELECT context：`ABORT_EXEC` 早退 | `unknown` | early return 位于 context restore block 之前；更外层恢复效果未验证 | `static-evidence` | `audit-candidate`；缺专用 DBUG/故障测试 |
| concrete Item clone/refix | `partial` | 多种 Item 有覆盖，但基类默认返回 null；没有静态完备性证明 | `static-evidence` | clone 完备性 `audit-candidate` |
| graceful fallback | `partial` | 仅 clone/refix/Gather 完成前；leader rewrite 开始后明确不可原地恢复 | `static-evidence` | `PQ-GAP-CONF-007` |
| plan cache | `rejected` | 成功 PQ 计划会 invalidate 并设为 uncacheable | `static-evidence` | `PQ-GAP-CONF-015` |
| hypergraph PQ access path | `rejected` | PQ 当前依赖传统 optimizer/QEP_TAB | `static-evidence` | `none` |
| worker 运行期 fallback | `rejected` | 已开始 worker 执行后不能切回原串行 iterator 继续 | `static-evidence` | `none` |
| shared hash-join context | `partial` | 最多一个 parallel-aware hash join；使用 raw borrowed pointer 和同步 | `static-evidence` | ownership `audit-candidate` |

`parallel_graceful_fallback` 控制 point of no return 前的恢复能力；它不改变后续
`ER_PARALLEL_FAIL_INIT` 的错误归一化。`parallel_fail_retry` 属于 dispatch/retry 模块，
不扩大此处可原地恢复的范围。

## 9. 可观测性

- optimizer trace 的 `make_parallel_query_plan`/detail 可定位 Gather 或 leader rewrite 的
  构图失败；DBUG 注入点提供更细粒度的静态测试入口。
- `PQ_stmt_executed` 和 `THD::pq_executed` 在 leader access path 构造后设置，只证明
  `PARL_EXEC` 计划已建立。
- `ER_PARALLEL_FAIL_INIT` 表示 PQ 初始化/计划阶段失败并可进入整语句 retry 判定；它与
  worker 执行期 `ER_PARALLEL_QUERY_ERROR` 不同。
- Debug 构建在 graceful backup/restore 周围比较主 MEM_ROOT digest；这是开发期一致性
  assertion，不是 release 运行观测接口。
- EXPLAIN 是否展示 Gather/PQ 路径可辅助证明改写成功，但不能证明 worker 已启动或结果
  已完整返回。

## 10. 代码与测试映射

下表逐项列出真实测试输入和结果路径；这些文件存在，但本任务没有执行它们。

| Requirement/Invariant | repository/path::QualifiedSymbol | 正向测试资产 | 负向/故障测试资产 | 行为状态 | 证据 | 缺口/Hardening |
|---|---|---|---|---|---|---|
| Item clone 等价性（INV-004） | `sql/parallel_query/pq_clone_item.cc::Item::pq_clone`、`sql/parallel_query/pq_clone.cc::pq_make_join` | `mysql-test/suite/parallel_query/t/pq_clone_item.test`、`mysql-test/suite/parallel_query/r/pq_clone_item.result-pq`、`mysql-test/suite/parallel_query/t/pq_refactor_clone.test`、`mysql-test/suite/parallel_query/r/pq_refactor_clone.result-pq` | `mysql-test/suite/parallel_query/t/refactor_fix_fields.test`、`mysql-test/suite/parallel_query/r/refactor_fix_fields.result-pq` | `partial` | `static-evidence`、`test-present` | clone 完备性 `audit-candidate` |
| refix 与 graceful restore（INV-002/003） | `sql/parallel_query/pq_clone.cc::pq_select_prepare`、`sql/parallel_query/pq_refix_fields_item.cc::Item::refix_fields`、`sql/parallel_query/pq_refix_fields_item.cc::Item_field::refix_fields`、`sql/parallel_query/sql_parallel.cc::restore_leader_plan` | `mysql-test/suite/parallel_query/t/pq_fallback.test`、`mysql-test/suite/parallel_query/r/pq_fallback.result-pq` | `mysql-test/suite/parallel_query/t/refactor_fix_fields.test`、`mysql-test/suite/parallel_query/r/refactor_fix_fields.result-pq`、`mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `partial` | `static-evidence`、`test-present` | `PQ-GAP-CONF-007` |
| leader 改写数组和首 tab（INV-001/003） | `sql/parallel_query/pq_optimizer.cc::JOIN::make_leader_rewritten_tab`、`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | `mysql-test/suite/parallel_query/t/pq_check_first_rewritten_tab.test`、`mysql-test/suite/parallel_query/r/pq_check_first_rewritten_tab.result-pq` | `mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `implemented` | `static-evidence`、`test-present` | `none` |
| template/Gather 所有权（INV-005/006/007） | `sql/parallel_query/sql_parallel.cc::make_pq_gather_operator`、`sql/sql_lex.h::Query_block::pq_unlink_clone`、`sql/parallel_query/sql_parallel.cc::pq_free_gather` | `mysql-test/suite/parallel_query/t/pq_refactor_clone.test`、`mysql-test/suite/parallel_query/r/pq_refactor_clone.result-pq` | `mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `partial` | `static-evidence`、`test-present` | ownership `audit-candidate` |
| 初始化错误归一化（INV-008） | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan`、`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | `mysql-test/suite/parallel_query/t/pq_fallback.test`、`mysql-test/suite/parallel_query/r/pq_fallback.result-pq` | `mysql-test/suite/parallel_query/t/pq_auto_retry_failed_parallel_query.test`、`mysql-test/suite/parallel_query/r/pq_auto_retry_failed_parallel_query.result-pq`、`mysql-test/suite/parallel_query/t/pq_abort.test`、`mysql-test/suite/parallel_query/r/pq_abort.result-pq` | `partial` | `static-evidence`、`test-present` | `none` |
| INSERT/REPLACE SELECT 的 ABORT context restore（INV-011） | `sql/parallel_query/sql_parallel.cc::make_pq_unit_plan`、`sql/parallel_query/sql_parallel.cc::make_pq_leader_plan` | `none` | `none`；缺少用 `dup_thd_abort` 或 `pq_leader_abort1/pq_leader_abort3` 触发 INSERT/REPLACE SELECT ABORT 并检查 context/retry 的故障测试 | `unknown` | `static-evidence` | `audit-candidate` |
| plan cache 禁用（INV-009） | `sql/parallel_query/sql_parallel.cc::plan_cache::invalidate_cached_plan`、`sql/parallel_query/sql_parallel.cc::plan_cache::set_uncacheable` | `mysql-test/suite/parallel_query/t/pq_prepare.test`、`mysql-test/suite/parallel_query/r/pq_prepare.result-pq` | `mysql-test/suite/parallel_query/t/pq_prepare.test`、`mysql-test/suite/parallel_query/r/pq_prepare.result-pq` | `rejected` | `static-evidence`、`test-present` | `PQ-GAP-CONF-015` |

## 11. 已知缺口和变更影响

当前 `conformance_gaps.md` 已登记与本模块相关的 drift 和证据缺口：

- `PQ-GAP-CONF-007` 固化 graceful fallback 只覆盖 clone/refix 早期、point of no return
  之后依赖 `ER_PARALLEL_FAIL_INIT` full retry 的当前边界。
- `PQ-GAP-CONF-015` 固化 PQ plan 不进入 plan cache，成功改写后必须 invalidate 串行 cached
  plan 并设置 uncacheable。
- `PQ-GAP-HARD-010` 要求把现有测试资产提升为绑定 commit/build 的运行报告；本任务所有
  clone/rewrite 测试仍只是 `test-present`。
- INSERT/REPLACE SELECT ABORT context restore — 行为状态 `unknown`；证据
  `static-evidence`；缺口类型为 `audit-candidate`。需要对 INSERT/REPLACE SELECT 使用
  `dup_thd_abort` 或 `pq_leader_abort1/pq_leader_abort3` 注入，并验证 target table、
  `Name_resolution_context_state` 和 retry 后续语句；当前精确测试资产为 `none`。
- 未单独登记的 clone 完备性审计项 — 行为状态 `partial`；证据 `static-evidence`；缺口
  类型为 `audit-candidate`。没有机器可检查的“所有可达 concrete Item 类型
  到 `pq_clone/refix_fields` 实现与字段保持”的完备映射；现有测试是代表性覆盖，新增
  Item/字段可能静默落入 null clone 或漏拷贝；若成为发布门禁，应在集中 gap 表分配 ID。
- 未单独登记的 ownership 审计项 — 行为状态 `partial`；证据 `static-evidence`；缺口类型为
  `audit-candidate`。Query_block 临时双向链接、worker 对 template/
  leader 的 raw borrowed pointer 依赖构图顺序和 statement-root 生命周期；断链后缺少覆盖
  全图的运行时 ownership assertion，需用失败点和销毁顺序测试验证并集中登记。

### 关联文件（读者导航；下列完整文件路径不构成 stable mapping）

- `sql/parallel_query/sql_parallel.h`、`sql/parallel_query/sql_parallel.cc`、
  `sql/parallel_query/pq_clone.h`、`sql/parallel_query/pq_clone.cc`、
  `sql/parallel_query/pq_clone_item.cc`、`sql/parallel_query/pq_refix_fields_item.cc`、
  `sql/parallel_query/pq_optimizer.cc`、`sql/sql_lex.h`、`sql/sql_select.cc`。
- `00_architecture_overview.md`、`01_current_support_matrix.md`、本目录其他模块文档和
  `conformance_gaps.md`。引用 refix 时应使用当前真实入口，不延续不存在的统一 symbol。
- 至少重跑第 10 节列出的全部精确 `.test` 输入并核对对应 `.result-pq`；新增 Item 必须
  同时增加成功 clone、故障 fallback、销毁和结果一致性覆盖。
