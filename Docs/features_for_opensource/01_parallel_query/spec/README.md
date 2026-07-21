# Parallel Query 当前规格入口

> 状态：`refactor-commit-ready`
> 稳定对照基线：`1b9ffd755d4` (`stable_branch`)；当前分支：`pq-local-refactor-clean`
> MySQL 基线：8.0.41
> 文档用途：AI 特性开发、代码走读、故障诊断、质量加固
> 运行验证：见 [`current/quality/verification_evidence.md`](current/quality/verification_evidence.md)

本目录是 Parallel Query（PQ）的技术规格入口。AI 或开发者不得把同级目录中的
演示材料、历史 patch、旧 Feature Spec 或旧 Worklog 自动解释为当前实现事实。

## 1. Source of truth

不同类型的事实使用不同权威来源：

| 事实类型 | 权威来源 | 冲突处理 |
|---|---|---|
| 当前代码实际行为 | `manifest.yaml` 所绑定快照中的产品源码 | 记录到 `current/conformance_gaps.md`，不能静默改写为设计目标 |
| 预期产品契约 | `current/` 下状态为 `current-contract` 的文档 | 若源码不满足，标记 `gap`，不能声称已实现 |
| 已验证行为 | 与目标提交绑定的构建、MTR、GUnit、DBUG 或压力证据 | 没有运行证据时只能写 `static-evidence` |
| 历史动机和未来方案 | `PQ-Feature-Spec.md`、`specs/WL*.md`、patch 和演示材料 | 只作背景；默认不加载，不得指导当前改码 |

发生冲突时，优先保留冲突本身，并按以下格式登记：

```text
GAP-ID
设计/预期契约
当前代码行为
静态或运行证据
风险
决策状态
```

## 2. 状态词汇

| 状态 | 含义 |
|---|---|
| `implemented` | 当前快照中存在明确代码路径，但不等于已经运行验证 |
| `verified` | 在绑定的构建上存在可复现通过证据 |
| `partial` | 只支持明确列出的子集或约束组合 |
| `serial-fallback` | SQL 正常执行，但 PQ 资格检查失败或主动保留串行计划 |
| `rejected` | 命令或计划被明确拒绝，可能返回错误或稳定拒绝原因 |
| `design-only` | 历史或未来设计，当前代码不能据此推断支持 |
| `deprecated` | 仍可能存在接口或变量，但不再作为推荐能力 |
| `unknown` | 当前证据不足，必须继续核对，禁止 AI 自行补全 |

## 3. 当前文档地图

### 必读入口

1. [`manifest.yaml`](manifest.yaml)：稳定对照基线、受控 PQ 源码快照、文档状态和生成顺序。
2. [`current/00_architecture_overview.md`](current/00_architecture_overview.md)：整体架构、主流程和跨模块不变量。
3. [`current/01_current_support_matrix.md`](current/01_current_support_matrix.md)：当前支持、部分支持和串行回退契约。

### 模块规格

| 模块 | 文档 | 负责的问题 |
|---|---|---|
| 控制与选择 | [`modules/01_control_eligibility_cost.md`](current/modules/01_control_eligibility_cost.md) | Hint、变量、Resolver、RBO、CBO、资源准入 |
| 计划与所有权 | [`modules/02_plan_rewrite_clone_ownership.md`](current/modules/02_plan_rewrite_clone_ownership.md) | 串行/Leader/Template/Worker 计划、clone、refix |
| Worker 生命周期 | [`modules/03_worker_lifecycle_error_retry.md`](current/modules/03_worker_lifecycle_error_retry.md) | 启动、状态、KILL、错误、fallback、retry、cleanup |
| Handler 契约 | [`modules/04_handler_capability_contract.md`](current/modules/04_handler_capability_contract.md) | SQL/引擎边界、capability、fail-closed |
| InnoDB 扫描 | [`modules/05_innodb_range_cursor_mvcc.md`](current/modules/05_innodb_range_cursor_mvcc.md) | B+Tree 切分、cursor、MVCC、ICP、回表 |
| MQ 与 Exchange | [`modules/06_mq_exchange_record_protocol.md`](current/modules/06_mq_exchange_record_protocol.md) | 消息格式、背压、detach、归并 |
| 聚合排序 | [`modules/07_aggregation_order_limit.md`](current/modules/07_aggregation_order_limit.md) | partial/final aggregation、ORDER、LIMIT |
| Join | [`modules/08_join_execution.md`](current/modules/08_join_execution.md) | Nested Loop、dependent ref、Hash/Semi/Anti/Outer Join |
| 高阶 SQL | [`modules/09_subquery_union_derived.md`](current/modules/09_subquery_union_derived.md) | Subquery、UNION、Derived、CTE、materialization |
| 事务与 DML | [`modules/10_transaction_dml_binlog.md`](current/modules/10_transaction_dml_binlog.md) | 隔离级别、锁、INSERT/REPLACE SELECT、binlog |
| 资源 | [`modules/11_resource_accounting.md`](current/modules/11_resource_accounting.md) | 线程、内存、MQ、临时表、VFD、资源归还 |
| 可观测和集成 | [`modules/12_observability_integrations.md`](current/modules/12_observability_integrations.md) | EXPLAIN、trace、status、plan cache、PTRC、日志 |
| Context 所有权重构 | [`current/13_context_ownership_refactor.md`](current/13_context_ownership_refactor.md) | THD、Query_block、JOIN 的 PQ 状态边界、等价性和验收合同 |

### 质量与诊断

- [`current/quality/requirements_test_traceability.md`](current/quality/requirements_test_traceability.md)
- [`current/quality/fault_injection_release_gates.md`](current/quality/fault_injection_release_gates.md)
- [`current/quality/verification_evidence.md`](current/quality/verification_evidence.md)
- [`current/runbooks/pq_not_selected.md`](current/runbooks/pq_not_selected.md)
- [`current/runbooks/wrong_result_or_crash.md`](current/runbooks/wrong_result_or_crash.md)
- [`current/runbooks/hang_kill_resource_leak.md`](current/runbooks/hang_kill_resource_leak.md)
- [`current/conformance_gaps.md`](current/conformance_gaps.md)

### 生成物

- [`generated/inventory.md`](generated/inventory.md)：从当前工作树生成的文件、测试和基线清单。
- [`generated/parallel_query_ai_handbook.md`](generated/parallel_query_ai_handbook.md)：按 manifest 顺序拼装的单文件版本。

生成物带有 `GENERATED — DO NOT EDIT` 标志；修改必须发生在 `current/` 源文档中。

## 4. AI 读取路线

### 初次理解 PQ

```text
AGENTS.md
  -> spec/README.md
  -> manifest.yaml
  -> 00_architecture_overview.md
  -> 01_current_support_matrix.md
```

### 开发一个支持场景

```text
overview
  -> support matrix
  -> 涉及的模块规格
  -> requirements-test traceability
  -> 对应源码与 MTR
```

### 排查 PQ 没有选中

```text
runbooks/pq_not_selected.md
  -> modules/01_control_eligibility_cost.md
  -> support matrix
  -> pq_optimizer_trace / EXPLAIN
```

### 排查 wrong result 或 crash

```text
runbooks/wrong_result_or_crash.md
  -> plan/clone ownership
  -> InnoDB range/cursor/MVCC
  -> 对应 SQL operator
  -> test traceability
```

### 排查 hang、KILL 或资源泄漏

```text
runbooks/hang_kill_resource_leak.md
  -> worker lifecycle
  -> MQ/exchange
  -> resource accounting
```

## 5. 历史资料规则

下列内容默认 `ai_default: false`：

- `../PQ-Feature-Spec.md`
- `../specs/WL001-PQ-Core-Architecture.md` 至 `WL018-*`
- `../patches/`
- `../working/`
- `../community_communication/`
- `../deliverables/`
- presentation、speaker script、Q&A、性能宣传材料

只有解释历史动机、比较设计与实现差异或准备社区材料时才加载它们。历史资料中
已经确认存在 DIV_TAB 选择、多分区、thread pool、COUNT DISTINCT、worker 状态和
变量默认值等漂移。

## 6. 修改规则

涉及 `sql/parallel_query/`、PQ 接入点、InnoDB PQ 文件或 PQ MTR 的变更，应同时检查：

1. `01_current_support_matrix.md` 是否需要更新。
2. 受影响模块的接口、所有权、状态机和失败语义是否改变。
3. Requirement → Source → Test 映射是否改变。
4. `conformance_gaps.md` 中的 gap 是否关闭、扩大或失效。
5. 重新生成 inventory 和 Handbook。
6. 更新 `verification_evidence.md` 中的目标范围、命令和结果；没有定向运行证据时不得写
   `verified`。

模块文档格式见 [`MODULE_TEMPLATE.md`](MODULE_TEMPLATE.md)。
