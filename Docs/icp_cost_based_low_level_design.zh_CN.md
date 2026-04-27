# 基于代价的 ICP —— 底层设计

> 本文档是 `icp_cost_based_low_level_design.md` 的中文对照版本，内容与英文版保持一致。若两者出现不一致，以英文版为准。

## 0. 术语

**DP** 指 **dynamic programming（动态规划）**。在 join 优化语境下，**join-order DP** 指用 **dynamic programming** 对 **join 顺序的前缀**（已排好的一段表）做代价递推与复用的那类搜索。基于代价的 ICP 会改写某张表在某个前缀下的 `POSITION::read_cost`，从而改变 **dynamic programming** 看到的局部代价；当两侧基线差距很小时，就可能改变最终 join 顺序。

## 1. 开关与常量

### 1.1 `sql/sql_const.h`

新增 optimizer switch bit：

- `OPTIMIZER_SWITCH_ICP_COST_BASED`

并相应地下移 `OPTIMIZER_SWITCH_LAST`。

### 1.2 `sql/sys_vars.cc`

扩展 `optimizer_switch` 元数据：

- 注册开关名 `icp_cost_based`；
- 在 `Sys_optimizer_switch` 中加入对应描述。

**默认值为 OFF**（不在 `OPTIMIZER_SWITCH_DEFAULT` 中）。

## 2. 规划器侧的代价决策

文件：`sql/sql_planner.cc`

### 2.1 资格预检

函数 `can_consider_icp_cost(const JOIN_TAB *tab, uint keyno)` 依次检查：

- 需要 `optimizer_switch=icp_cost_based` 为 ON；
- 候选 key 必须存在（`keyno != MAX_KEY`）；
- 引擎必须支持 `HA_DO_INDEX_COND_PUSHDOWN`；
- ICP 开关与相关 hint 不禁止下推；
- 不是多表 UPDATE/DELETE；
- 不在"带 guarded 条件"的子查询中；
- 索引不包含虚拟生成列；
- 不是（InnoDB 风格的）聚簇主键；
- 不是可以仅用索引读（covering index）满足查询的场景。

### 2.2 代价模型

`should_enable_icp_by_cost(...)` 按以下方式估算两种方案的总代价：

- **无 ICP**：所有行都回表查询 + 在 SQL 层评估 WHERE；
- **有 ICP**：更少回表 + 在剩余行上评估 SQL 层 WHERE + 在索引项上评估一份代价更低的引擎侧谓词（代价系数为 `kIcpEvalCpuFactor`）。

判定规则：

- 当 `cost_if_with_icp < cost_if_no_icp` 时，返回 `true`；
- 否则返回 `false`；调用方会把这种情况解释为"**不写 ON 决策**"，而不是"**写 OFF 决策**"（见 2.3 / 第 4 节）。

#### 2.2.2 奖励幅度硬封顶

文件：`sql/sql_planner.cc`

`should_enable_icp_by_cost()` 与 `range_icp_preview()` 给出的原始 `adjustment` 在被写回任何 cost 字段之前，统一过一道"硬封顶"：

- 常量 `kIcpBenefitCapRatio = 0.5`（`sql_planner.cc` 文件局部）；
- helper `cap_icp_benefit_adjustment(adjustment, cost_if_no_icp, capped_out)`：
  - 约定 `adjustment <= 0`（调用方总是传 `cost_if_with_icp - cost_if_no_icp`）；
  - 当 `cost_if_no_icp <= 0` 或 `adjustment >= 0` 时，原样透传；
  - 否则按 `|adjustment| <= kIcpBenefitCapRatio * cost_if_no_icp` 夹住，置 `*capped_out = true`，返回夹后的值；
  - 未夹住时 `*capped_out = false`，返回原值。

该封顶在**三处**奖励落点同步应用，确保没有任何一条奖励通路可以绕过：

1. `best_access_path()` 的 scan/range 奖励（见 2.3）：夹后的值才写进 `best_read_cost`；
2. `find_best_ref()` 的 ref 擂台奖励（见 2.4）：夹后的值才写进 `cur_ref_cost`；
3. `range_icp_preview()` 的 range 候选预演（见 2.5）：用镜像常量 `kRangeIcpBenefitCapRatio = 0.5`，把 `effective_cost` 底线锁在 `(1 - kRangeIcpBenefitCapRatio) * original_cost`。

封顶触发时，对应 trace 点打 `icp_cost_adjustment_capped = true`（三处 trace 字段列表见 2.3 / 2.4 / 2.5）。

**为什么选 50%**：实测上被很好利用的 ICP 谓词能节省 30%–80% 的回表；50% 足以让真正赢家的优势继续成立，又窄到让单次 `get_filtering_effect()` 失真不能"独自把某条路径打到接近 0"去重塑 join-order DP。后续若 telemetry 表明这个门槛偏宽/偏窄，只需改这两个常量并重录基线；如果要放宽，前提是先证明估算器比现在更可靠。

#### 2.2.1 range 扫描被选中时的基线修正

当 range 扫描在 `best_access_path()` 的 scan-vs-ref 比较中胜出，规划器会：

- 用 `rows_after_filtering` 覆盖 `rows_fetched`；
- 把 `filter_effect` 重新定义为
  `min(1, found_records * full_filter / rows_after_filtering)`。

而此时 `rows_after_filtering` 本身在开启 `COND_FANOUT_FILTER` 时就等于 `found_records * full_filter`，于是 `filter_effect` **天生被压成 ~1.0**。如果直接把这对值喂给 ICP 代价模型，会得到 `filtered_out_rows == 0`，模型**永远判定"不划算"**——相对社区默认下推出现明显回退。

为此，调用方（见 2.3）在调用 `should_enable_icp_by_cost` **之前**重新构建基线：

- `icp_rows_fetched = tab->found_records`——**索引扫描的原始输出行数**，这才是 ICP 真正工作的基数；
- `icp_filter_effect = min(1, rows_fetched / icp_rows_fetched)`——在新基线上才有意义的"索引层过滤比例"。

该修正**仅当 range 扫描是被选中的访问方法**时启用（即 `best_ref == nullptr && tab->range_scan() != nullptr`），且重建后的基线必须**严格大于**原基线。

### 2.3 规划集成

`best_access_path()` 中：

- 从已选访问路径推导候选 key；
- 跑资格预检；
- 若是 range 扫描被选中，按 2.2.1 重建 ICP 基线，然后调用 `should_enable_icp_by_cost`；
- 打印 trace 字段（完整列表见高层设计文档 3.3 节）：
  `icp_cost_based`、`icp_cost_keyno`、`icp_rows_fetched`、
  `icp_filter_effect`、`icp_cost_if_disabled`、`icp_cost_if_enabled`、
  `icp_enabled`；
- **如果模型判定 ICP 有收益**：
  - 设置 `POSITION::icp_decision_made = true`、
    `POSITION::use_cost_based_icp = true`、
    `POSITION::icp_keyno = icp_keyno`；
  - 把原始差值 `cost_if_with_icp - cost_if_no_icp` 经过 `cap_icp_benefit_adjustment(..., cost_if_no_icp, &capped)`（见 2.2.2）得到夹后的奖励，再加到 `best_read_cost` 上；
  - 打印 `icp_cost_adjustment` 与 `icp_adjusted_read_cost`；若封顶触发，额外打 `icp_cost_adjustment_capped = true`；
- **如果模型未能证明 ICP 有收益**：
  - 保持 `POSITION::icp_decision_made = false`；
  - 打印 `icp_fallback_to_default = true`；
  - **不**调整 `best_read_cost`。

这样做的理由：当前代价模型尚不足以可靠地"反向关闭"社区默认下推（参见 2.2.1 中的 range-scan 病理、以及 TODO 中的校准项）。仅写 ON 决策能保证该特性是**严格改进**——最坏情况是不给奖励，不会再出现"悄悄把 ICP 关掉导致变慢"。

### 2.4 ref 擂台 ICP 预演（issue-1 ref 形式）

在 `find_best_ref()`（`sql/sql_planner.cc`）对每个候选 key 的循环里，**在算出 `cur_ref_cost` 之后、进入擂台比较之前**，对当前候选做一次 ICP 收益预演，并把收益反馈回 `cur_ref_cost`：

1. **Gate 1 —— 特性启用且允许下推**。若 `icp_cost_based=off`，或当前 key 是 FULLTEXT，或 `can_consider_icp_cost(tab, key)` 否决，则跳过。此外，若没有 WHERE 条件（`tab->join()->where_cond == nullptr`）或 `table->cond_set` 整体为空（完全无谓词），同样跳过——没有东西可以过滤。

2. **Gate 2 —— 该 key 存在未绑定的 keypart**。计算
   `has_unbound_keyparts = (found_part != LOWER_BITS(actual_key_parts(keyinfo)))`；
   若当前 ref 已经把所有 keypart 都绑定了，**索引里没有剩余"尾部"给 ICP 去评估**，跳过奖励。

3. **Gate 3 —— 剩余 WHERE 列落在当前 key 上**。基于 `table->s->fields` 维护三个列位图：
   - `key_columns`：当前 key 全部 keypart 对应的字段集合（通过 `table->tmp_set` 复用）；
   - `ref_bound_cols`：已经被当前 ref 用 `found_part` 绑定的字段集合（用一个局部 `MY_BITMAP`，保持 `tmp_set` 干净）；
   - `remaining_cond_cols = table->cond_set \ ref_bound_cols`——"被 WHERE 引用、但 ref 没绑定"的列。

   若 `remaining_cond_cols` 为空（所有谓词都被 ref 吸收了）或 `bitmap_is_subset(&remaining_cond_cols, &key_columns)` 为假（有剩余列不在本 key 上，引擎无法下推），跳过奖励。

4. **selectivity 估算**。所有 gate 通过后，用
   `Item::get_filtering_effect(thd, tab_map, /*read_tables=*/0, &ref_bound_cols, tab->records())`
   估算"剩余谓词"的整体过滤比例。把 `ref_bound_cols` 作为"忽略集合"传进去，避免把已经被 ref 消化掉的谓词重复计数。若结果不在 `(0, 1)` 开区间，则估算不可用，直接跳过。

5. **cost 决策**。调用 `should_enable_icp_by_cost(tab, key, prefix_rowcount, cur_fanout, remaining_filter, &no, &with)`；若返回 `true`，把原始差值 `with - no` 经 `cap_icp_benefit_adjustment(..., no, &capped)`（见 2.2.2）得到夹后的奖励，再加到 `cur_ref_cost` 上。同时逐候选打印 trace 字段：`icp_cost_based`、`icp_rows_fetched`、`icp_filter_effect`、`icp_cost_if_disabled`、`icp_cost_if_enabled`、`icp_enabled`、`icp_cost_adjustment`、`icp_adjusted_ref_cost`；当本候选的封顶真正触发时，额外打 `icp_cost_adjustment_capped = true`。

离开该代码块前，**务必清空 `tmp_set`**，防止后续迭代或别的调用方看到脏位图。

**保证**：

- 该预演**只会降低候选代价，永不抬高**；任意一道 gate 不通过时 `cur_ref_cost` 与旧行为**逐字节一致**，对不能从 ICP 受益的候选完全透明。
- 对例子 `WHERE a = 4 AND c = 6`，`idx_a` / `idx_ab` 因为 `{c}` 不是它们 keypart 集合的子集，直接被 Gate 3 拒绝，不做任何调整。
- 预演只作用在**候选循环内部**，不写 `POSITION` 任何字段——那仍然由 2.3 在"最终赢家"身上完成。

**暂不覆盖**：非 ref 的半连接物化路径、hypergraph 优化器。这些都作为 follow-up 单独跟进（见第 6 节）。range 候选走自己的预演路径，见 2.5。

### 2.5 range 候选的 ICP 预演（issue-1 range 形式）

文件：`sql/range_optimizer/index_range_scan_plan.cc`。

`get_key_scans_params()` 用每个 range 候选的 handler `check_quick_select()` 代价和 `cost_est`（来自 `best_access_path()` 的 table-scan 基线）做擂台比较。handler 代价只反映"原始回表成本"，不知道"SQL 层稍后还能不能下推"，于是**凡是"如果做了 ICP 会很便宜"的候选全部以 `cause: cost` 被拒**，整个 range 选项被折叠成 `nullptr`。range 形式修复通过**本地 ICP 预演**，仅影响"擂台比较"这一步。

**静态 helper**（定义在 `index_range_scan_plan.cc` 的匿名 namespace）：

1. `range_icp_preview_gates(thd, table, keynr)`：以下全部成立时返回 true：
   - `optimizer_switch=icp_cost_based=on`；
   - `keynr != MAX_KEY`；
   - 引擎对该 key 声明 `HA_DO_INDEX_COND_PUSHDOWN`；
   - `hint_key_state(thd, table->pos_in_table_list, keynr, ICP_HINT_ENUM, OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN)` 允许下推；
   - 命令**不是** `SQLCOM_UPDATE_MULTI` / `SQLCOM_DELETE_MULTI`；
   - 索引**不含**虚拟生成列；
   - 不是聚簇主键；
   - 不是覆盖索引（即 `table->covering_keys.is_set(keynr) && !table->no_keyread` 为假）。

2. `range_icp_preview(thd, table, keynr, where_cond, found_records, original_cost, trace_idx)`：返回"有效代价"：
   - 当 `where_cond == nullptr`、`found_records` 为 0 或 `HA_POS_ERROR`、或 (1) 中的 gate 失败时，直接返回 `original_cost`；
   - 读取 `bound_keyparts = table->quick_key_parts[keynr]`；若 `bound_keyparts == 0` 或 `bound_keyparts >= total_keyparts`，返回 `original_cost`（没有剩余 keypart 可下推，或完全没被 range 绑定）；
   - 基于 `table->s->fields` 构建两个位图：
     - `key_cols` = 当前 key 所有 keypart 对应的字段；
     - `range_bound_cols` = 前 `bound_keyparts` 个 keypart 对应的字段；
   - 通过 `Item::add_field_to_cond_set_processor` 遍历 `where_cond` 把列写进 `table->cond_set`。由于 `choose_table_order` 会在 `best_access_path()` 之前重新清空并回填 `cond_set`，此处"临时写"是安全的；helper 在退出时若 `cond_set` 原本为空会恢复为空；
   - 计算 `remaining_cond_cols = cond_set \ range_bound_cols`：若为空（已无谓词可下推）或不是 `key_cols` 的子集（有剩余列不在本索引上），返回 `original_cost`；
   - 估算 `remaining_filter = where_cond->get_filtering_effect(thd, table_map, read_tables=0, &range_bound_cols, found_records)`；要求 `0 < remaining_filter < 1`，否则返回 `original_cost`；
   - 计算
     `cost_if_with_icp = original_cost * remaining_filter + cost_model->row_evaluate_cost(found_records * kRangeIcpEvalCpuFactor)`，
     其中 `kRangeIcpEvalCpuFactor = 0.25`（文件局部常量，对应 `should_enable_icp_by_cost()` 里 `kIcpEvalCpuFactor` 的思想）；
   - 只有当 `cost_if_with_icp` **严格小于** `original_cost` 才返回 `cost_if_with_icp`，否则返回 `original_cost`；
   - 应用硬封顶（`kRangeIcpBenefitCapRatio = 0.5`，见 2.2.2）：若候选的 `cost_if_with_icp` 跌破 `(1 - kRangeIcpBenefitCapRatio) * original_cost`，返回值被夹到该下限（`effective_cost = 最低允许值`），并记录一个"已封顶"的内部标记；
   - 给出奖励时，打印 trace 字段 `icp_cost_based`、`icp_rows_fetched`、`icp_filter_effect`、`icp_cost_if_disabled`、`icp_cost_if_enabled`、`icp_enabled`、`icp_cost_adjustment`、`icp_adjusted_range_cost`；当封顶触发时，额外打 `icp_cost_adjustment_capped = true`。

**在 `get_key_scans_params()` 里的集成**：每个候选循环中维护两个运行值：

- `read_cost`：当前擂台最好"有效代价"（可能是 ICP 调整后的值），仅用于比较；
- `best_original_cost`：当前赢家的 handler 原始代价，用于 `AccessPath::cost` 以及下游所有消费方；

流程：

- `check_quick_select()` 算出 `(found_records, cost)` 之后，只有在 `!ror_only` 时从 `param->query_block->where_cond()` 取 `where_cond`（index-merge 构造完全不参与这次预演），调用
  `effective_cost = range_icp_preview(thd, param->table, keynr, where_cond, found_records, cost.total_cost(), &trace_idx)`；
- 擂台比较改为 `read_cost > effective_cost`（原为 `read_cost > cost.total_cost()`）；
- 新赢家出现时同时更新 `read_cost = effective_cost` 和 `best_original_cost = cost.total_cost()`；
- 循环结束时：`path->cost = best_original_cost`（**不是** `read_cost`）。

**保证**：

- `AccessPath::cost` 与社区实现字字相同——ICP 调整只影响 `get_key_scans_params()` 内部的**比较**，不影响"赢家的 cost 度量"。`calculate_scan_cost`、EXPLAIN、join 顺序 DP 看到的依旧是原来的数字。
- index-merge 构造（`ror_only = true`）强制 `where_cond = nullptr`，本次预演完全不介入——经典的"候选是否 rowid 有序、能否参与 ROR intersect"判据保持不变。
- 覆盖索引、聚簇主键、全 keypart 绑定、剩余列跨索引等不应从预演获益的形态，全部在 helper 内返回 `original_cost`，`read_cost > cost.total_cost()` 的老比较逻辑字字保留。

## 3. 数据传递

### 3.1 `sql/sql_select.h`

`POSITION` 携带：

- `icp_decision_made`
- `use_cost_based_icp`
- `icp_keyno`
- `cost_if_no_icp`
- `cost_if_with_icp`

`JOIN_TAB` 运行期持有拷贝的决策：

- `m_icp_decision_made`
- `m_use_cost_based_icp`
- `m_icp_keyno`

以及辅助 API：

- `has_cost_based_icp_decision_for(keyno)`
- `use_cost_based_icp()`
- `set_cost_based_icp_decision(...)`

由于当前规划器只会写 ON 决策（见 2.3），`has_cost_based_icp_decision_for(keyno) == true` **目前等价于** `use_cost_based_icp() == true`。这些字段仅作为规划器决策的记录；执行期 ICP 下推当前不会消费它们来关闭 ICP。

### 3.2 `sql/sql_optimizer.cc`

在 `JOIN::get_best_combination()` 中，把 `POSITION` 的决策拷贝到 `JOIN_TAB`。

## 4. 执行期下推

文件：`sql/sql_select.cc`

`QEP_TAB::push_index_cond(...)` 有意保留社区默认的下推流程。基于代价的
ICP 特性只在规划阶段奖励 ICP-capable 访问路径，不在执行计划精修阶段抑制
下推。

根据 2.3，规划器不会持久化 OFF 决策。只要 `push_index_cond()` 根据已有的
引擎能力、hint、guarded condition、key-only read、聚簇主键等检查判断可以
合法下推，就应该继续下推。

`sql/icp_cost_based.cc` 中的规划期 gate 有意复用执行计划精修前可见的稳定
检查，并应随 `push_index_cond()` 中稳定合法性检查的变化同步更新。目前仍有两类
检查尚未完全闭合：

- **range 预演阶段不可得**：`get_key_scans_params()` 有 `TABLE`，但没有
  `JOIN_TAB`，因此 range 候选擂台不知道 guarded condition 状态。ref 预演和
  最终 scan/range 预演有 `JOIN_TAB`，会把 guarded condition 状态传入公共 gate。
- **访问路径擂台之后才确定**：`JOIN_TAB::reversed_access` 以及 BKA/BNL
  join-cache 细节可能在 ref/range 候选已经获得 ICP 奖励之后才确定。这些情况下，
  最终 `push_index_cond()` 可能拒绝或避免下推，但规划期 cost 已经包含 ICP 收益。
  这是当前已知限制；后续应在发放奖励前让这些限制可见，或在后期限制确定后撤销奖励。

## 5. 测试覆盖

`mysql-test/t/icp_cost_based.test`：

- **Case 1** —— `icp_cost_based=off`，`FORCE INDEX` + ref 访问：
  验证老行为不变、新 trace 字段不出现。`EXPLAIN FORMAT=TREE` 结果断言计划形态为
  `Index lookup on t1 using idx_abc ..., with index condition: ...`。

- **Case 2** —— `icp_cost_based=on`，`FORCE INDEX` + ref 访问：
  验证新 trace 字段被打印出来，且计划仍然是 `Index lookup` 并带 `index condition: ...` 注解。

- **Case 3（issue-2 回归保护）** —— `icp_cost_based=on`，`FORCE INDEX` + leading 列 range + 尾列等值：
  验证当代价模型无法证明 ICP 有收益时，会回落社区默认下推路径，而不是关闭
  ICP。`EXPLAIN FORMAT=TREE` 断言
  `Index range scan on t3 using idx_ab ..., with index condition: ...`。

- **Case 4（issue-1 ref 形式）** —— `icp_cost_based=on`，**不带任何 index hint**，三把索引（`idx_a`、`idx_ab`、`idx_abc`）共享起始键 `a`，查询为 `WHERE a = 4 AND c = 6`：
  OFF 基线为 `Filter: (tt.c = 6) -> Index lookup on tt using idx_a (a=4)`；
  ON 后计划必须切换为 `Index lookup on tt using idx_abc (a=4), with index condition: (tt.c = 6)`。
  配套一条 `FORCE INDEX(idx_abc)` 对照说明：只要把优化器指向对的那把索引，执行层一直都能跑出这个 plan——问题纯粹出在"访问方法选择"这一步。

- **Case 4d（执行期后置限制，反向 ref 访问）** —— `WHERE a = 4 AND c = 6 ORDER BY b DESC LIMIT 5`：
  ref 擂台奖励了 `idx_abc`，但后续 ORDER BY 优化把访问变成 reverse iterator。
  `push_index_cond()` 对 `reversed_access` 拒绝 ICP，最终 plan 是反向 index lookup +
  server-side Filter。optimizer trace 断言 cost preview/reward 已经发生，plan 断言记录
  没有 `with index condition`，作为当前规划/执行不一致的 guardrail。

- **Case 4a（feature-gate 保护）** —— 同一条查询，把 `icp_cost_based` 切回 `off` 后必须恢复 OFF 基线 plan。这条用例把"开关"钉死，防止哪天因为默认值变 ON 或优化器状态残留悄悄回退。

- **Case 4b（ref 绑满全部 keypart，预演必须不触发）** —— `WHERE a = 4 AND b = 5 AND c = 6`，`icp_cost_based=on`：
  `idx_abc` 的所有 keypart 都被 ref 占用，Gate 3 的 `remaining_cond_cols` 为空，预演应直接跳过；此时期望看到的 plan 是社区行为 `idx_ab` lookup + server-side Filter on `c`，与基线一致。

- **Case 4c（剩余列不在索引上，预演必须不触发）** —— `WHERE a = 4 AND d = 7`：
  `d` 不是任何索引的 keypart，Gate 3 的 `remaining_cond_cols ⊆ key_columns` 子集检查对所有候选都失败；因此 OFF / ON 必须产生完全相同的 plan。

- **Case 5（issue-1 range 形式）** —— `WHERE a BETWEEN 2 AND 4 AND c = 50`，不带 hint。`icp_cost_based=off` 时由于所有 range 候选 handler 原始代价都高于 table-scan 基线，计划为 Table scan。`icp_cost_based=on` 时 range 候选预演（见 2.5）奖励了 `idx_abc`（唯一能把 `c = 50` 下推给引擎的索引），计划切换为
  `Index range scan on tt using idx_abc over (2 <= a <= 4), with index condition: ((tt.c = 50) and (tt.a between 2 and 4))`。

- **Case 5a（range 绑满 keypart，预演必须不触发）** —— `WHERE a BETWEEN 2 AND 4 AND b = 3 AND c = 50`，`icp_cost_based=on`：
  range 已经把 `idx_abc` 所有 keypart 绑定了，`bound_keyparts == total_keyparts`，helper 返回 `original_cost`，plan 与社区行为一致。

- **Case 5b（剩余列不在任何索引上，预演必须不触发）** —— `WHERE a BETWEEN 2 AND 4 AND d = 7`：
  `d` 不是任何索引的 keypart，`remaining_cond_cols ⊆ key_cols` 子集检查对所有候选都失败；ON / OFF 都得到一样的 Table-scan 基线。

- **Case 6a（多表 guardrail，STRAIGHT_JOIN）** —— `tj_driver STRAIGHT_JOIN tt`：`STRAIGHT_JOIN` 固定驱动表，ON/OFF 两侧顶层 plan（`d` 驱动，`tt` 被驱动）必须一致；`tt` 的访问方式在 ON 时可以从 `idx_a + SQL Filter` 升级到 `idx_abc + index condition: (tt.c = 50)`。

- **Case 6b（多表 guardrail，自由 join order）** —— 同两张表，无 `STRAIGHT_JOIN`：即便 ON 时 `tt` 看似更便宜，优化器**必须**仍然选择 `tj_driver`（5 行）作为驱动表。这正是靠"`pos->rows_fetched` / `pos->filter_effect` 在奖励后**不被改写**"（`sql_planner.cc:1496-1497`）来守护的——join-order DP 两侧看到的是同样的 fanout 估计。

- **Case 6c（多表 guardrail，semijoin 策略变化，预期）** —— `EXISTS (SELECT 1 FROM tt WHERE tt.a = d.a AND tt.c = 50 AND tt.payload <> '')`：OFF 选 `MaterializeLookup`（对 `tt` 做一次 table scan + 哈希 probe）；ON 选 `FirstMatch` on `idx_abc + ICP`。这是**预期**行为：`advance_sj_state()` / `fix_semijoin_strategies_for_picked_join_order` 在做 semijoin 策略选择时**会读 `pos->read_cost`**，而 `pos->read_cost` 是**带奖励**的。guardrail 把两边精确 plan 字符串记录下来，一旦策略再漂移会立即被 `--record` 对比出来；运维上线该特性时应在 release notes / 运维文档里提示：EXISTS / IN 子查询的 semijoin 策略可能发生迁移。

所有 `EXPLAIN FORMAT=TREE` 输出用 `--replace_regex` 将 `cost=...`、`rows=...`、`(actual time=...)` 等易变数值归一化，保证结果在不同平台与代价调参下稳定。

预期输出存放在 `mysql-test/r/icp_cost_based.result`。

**回归稳健性**：

- `main.1st` 继续绿；
- 广义 ICP 用例集绿：`innodb_icp`、`innodb_icp_all`、`innodb_icp_none`、`range_icp`、`func_in_icp`、`null_key_icp_innodb`；
- Semijoin 套件在**默认** `optimizer_switch` 下绿：`subquery_sj_firstmatch`、`subquery_sj_mat`、`subquery_sj_loosescan`——即 `icp_cost_based=off`（默认）时社区基线不漂移。

## 6. 当前范围：仅经典优化器

本文档描述的实现有意限制在经典优化器路径内。所有写点和代价预演都依赖经典优化器的数据结构与调用点：

- `find_best_ref()`：在经典 ref 赢家选出前调整 ref 候选的擂台代价；
- `get_key_scans_params()`：在 range 候选选出前调整每个索引的有效擂台代价；
- `best_access_path()`：把最终基于代价的 ON 决策写入 `POSITION`；
  `JOIN::get_best_combination()` 再把它拷贝到 `JOIN_TAB`；
- `QEP_TAB::push_index_cond()`：保留社区下推合法性检查，不消费 cost-based 决策来关闭 ICP。

Hypergraph optimizer 的访问路径枚举和代价流不走这条完全相同的规划链路。
因此，只在 `find_best_ref()` 和 `get_key_scans_params()` 上加 hook，并不能让
Hypergraph 访问路径自动使用本文描述的 ICP 调整后有效代价来比较 ref/range
候选。本轮支持的明确场景是：

- 使用经典优化器，且当前查询未被 Hypergraph optimizer 规划；
- 使用 InnoDB 或其他声明 `HA_DO_INDEX_COND_PUSHDOWN` 的存储引擎；
- 显式打开 `optimizer_switch='icp_cost_based=on'`。

当查询由 Hypergraph optimizer 规划时，本特性应视为不在当前支持范围内：
选路不保证包含基于代价的 ICP 奖励，查询会回落到该优化器路径已有的 ICP
行为。后续可以在 Hypergraph optimizer 的访问路径代价层补等价的预演和
决策传递 hook，扩展支持新的优化器引擎。

## 7. 后续工作（follow-up）

- **Hypergraph 优化器**：当前两套预演分别挂在经典的 `find_best_ref()` 与 `get_key_scans_params()` 路径上；hypergraph 路径自己有另一套访问方法选择代码，ref 形式与 range 形式**都需要**额外接一次。
- **selectivity 校准**：`Item::get_filtering_effect()` 偏保守。若上线后的 trace 数据显示在某些谓词形态上系统性地给多/给少奖励，可以在 gate 逻辑不动的前提下把估算器换成更好的；接口约束是"对 `(0,1)` 开区间的单一浮点数 selectivity 作答"。range 形式目前用的是文件局部常量 `kRangeIcpEvalCpuFactor`，两边稳定一段时间后可考虑与规划器侧的 `kIcpEvalCpuFactor` 统一。一旦 `icp_cost_adjustment_capped` 的 telemetry 显示当前 50% 预算过紧/过松，可同步调整 `kIcpBenefitCapRatio` / `kRangeIcpBenefitCapRatio`（2.2.2）并重录基线。
- **基于代价的 OFF 决策**：2.3 节出于谨慎目前不写 OFF 决策；第 4 节也刻意让
  `push_index_cond()` 保持社区默认下推路径。若未来更准确的代价模型需要按代价关闭
  ICP，应单独补充明确的执行期设计，而不是隐式复用当前"只奖励"路径。
- **后期才发现不能 ICP 的路径**：reverse access 和部分 BKA/BNL 场景是在早期
  ref/range 擂台之后才决定的。Case 4d 记录了 reverse-ref 不一致。后续应让这些
  后期限制在发放奖励前可见，或在限制确定后扣回奖励。
- **非命中情况的 trace**：range 形式 helper 目前只在"被奖励"时输出 trace。后续可以把 skip 原因（`"covering"`、`"fully_bound"`、`"not_on_index"` 等）也补上，方便不读 C++ 也能定位 gate 用例行为。
