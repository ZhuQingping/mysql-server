# 基于代价的 ICP —— 高层设计

> 本文档是 `icp_cost_based_high_level_design.md` 的中文对照版本，内容与英文版保持一致。若两者出现不一致，以英文版为准。

## 背景

社区版 MySQL 中，ICP（Index Condition Pushdown，索引条件下推）的启停决定是在执行计划精修阶段（`QEP_TAB::push_index_cond()`）做的，而不是在 join 顺序/访问路径规划阶段。这就可能漏掉本可以借助 ICP 显著变快的执行计划——优化器根本没有把 ICP 的收益算进 plan 选择里。

## 目标

让优化器在**规划阶段**就能把 ICP 的收益纳入考虑，同时保留安全性与回滚能力：

- 当启用 ICP 的访问路径确实更便宜时，提升 plan 质量；
- 通过保守的代价检查，避免性能回退；
- 通过 `optimizer_switch` 在运行时可回滚。

## 设计原则

- **默认行为必须保持与社区路径兼容**。
- **必须由一个显式开关控制**是否启用新行为。
- **执行期的安全校验必须保留**。
- **严格改进（strict improvement）**：当代价模型**不能明确证明** ICP 有收益时，必须回落到社区默认的下推流程——可以不奖励有利 plan，但绝不允许"悄悄关掉本该下推的 ICP"。

## 特性开关

新增一个 `optimizer_switch` 选项：

- `icp_cost_based=off`（默认值）：走社区老逻辑；
- `icp_cost_based=on`：启用规划阶段的基于代价的 ICP 决策。

## 执行流程

1. `best_access_path()` 中：
   - 确定候选索引（`ref` 用到的 key，或者被选中的 range key）；
   - 跑一遍资格预检；
   - **构建一份用于 ICP 收益估算的基线**（见下文"基线说明"），然后估算 `cost_if_disabled` 与 `cost_if_enabled`；
   - 若模型判定 ICP 有收益：在 `POSITION` 上写入 ON 决策，并从 `best_read_cost` 中减去估算的收益，好让 join 规划阶段更倾向于这个路径；
   - 若模型无法证明 ICP 有收益：**不写决策**，让后续阶段回落到社区默认下推流程。

   > **基线说明**：当 range 扫描胜出成为所选访问方法时，`best_access_path()` 此刻的 `rows_fetched` / `filter_effect` 已经是"应用过完整 WHERE 过滤"之后的值（`rows_fetched = rows_after_filtering`，并且 `filter_effect ≈ 1.0`）。直接把这两个值喂给 ICP 模型，会让 `filtered_out_rows` 恒为 0，决策永远变成"不划算"——即便 ICP 实际上能消除绝大部分回表。因此，**对 range 扫描路径，我们用 `tab->found_records` 重新构建一份索引扫描原始输出行数的基线**。

2. `get_best_combination()` 中：
   - 把 `POSITION` 里记录的决策拷贝到 `JOIN_TAB`。

3. `push_index_cond()` 中：
   - 若当前 key 上**已存在**决策，按决策执行；
   - 否则（包括上文"未写决策"的情况），保留社区老 ICP 流程。

   目前规划器**只会写 ON 决策**，所以 "存在决策且为 OFF" 这一分支在当前实现中是"空转"的。保留该分支的代码，是为了将来校准更准的代价模型时可以直接启用"基于代价关闭 ICP"，不需要再动数据结构。

## 可观测性

当 `icp_cost_based=on` 时，optimizer trace 中会出现以下字段：

- `icp_cost_based`：是否进入了基于代价的 ICP 评估；
- `icp_cost_keyno`：评估基于的候选 key；
- `icp_rows_fetched`、`icp_filter_effect`：模型使用的行数基线与过滤比例；
- `icp_cost_if_disabled`、`icp_cost_if_enabled`：两种方案的估算代价；
- `icp_enabled`：代价模型自己的判定结果；
- `icp_cost_adjustment` / `icp_adjusted_read_cost`：**仅在 ON 分支**（即给了成本奖励）时出现；
- `icp_fallback_to_default`：**仅在模型无法证明收益**、规划器主动回落到社区默认路径时出现。

当 `icp_cost_based=off` 时，上述字段都不会出现。

`push_index_cond()` 这一层仍然会在"检测到已写入 OFF 决策并因此跳过下推"时输出 `not_pushed_due_to_icp_cost`。由于当前规划器不会写入 OFF 决策，这个 token 在实际运行中**不应出现**，MTR 用例据此做反向断言以作为回归保护。

## 验证

使用 `main.icp_cost_based` 用例验证：

- **OFF 路径**：完全与社区行为一致（新字段都不出现，`EXPLAIN` 计划与社区基线相同）；
- **ON 路径 / ref 访问**：新 trace 字段被打印出来，并且 `EXPLAIN FORMAT=TREE` 中仍然保留 `with index condition: ...` 注解；
- **ON 路径 / range 扫描（issue-2 回归保护）**：`FORCE INDEX` + leading 列 range + 尾列等值的组合必须仍然产生
  `Index range scan on ... with index condition: ...`；同时断言 `icp_suppressed_by_cost_model = 0`，防止本特性悄悄关掉该形态的 ICP。

同时保证 `main.1st` 以及更广的 ICP 用例集绿灯：
`innodb_icp`、`innodb_icp_all`、`innodb_icp_none`、`range_icp`、`func_in_icp`、`null_key_icp_innodb`。
