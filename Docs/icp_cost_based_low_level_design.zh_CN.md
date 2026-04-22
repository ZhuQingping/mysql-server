# 基于代价的 ICP —— 底层设计

> 本文档是 `icp_cost_based_low_level_design.md` 的中文对照版本，内容与英文版保持一致。若两者出现不一致，以英文版为准。

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
  - 给 cost 奖励：`best_read_cost += (cost_if_with_icp - cost_if_no_icp)`；
  - 打印 `icp_cost_adjustment` 与 `icp_adjusted_read_cost`；
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

5. **cost 决策**。调用 `should_enable_icp_by_cost(tab, key, prefix_rowcount, cur_fanout, remaining_filter, &no, &with)`；若返回 `true`，按
   `cur_ref_cost += (with - no)`（负值 → 降 cost）调整本候选的 ref cost，同时逐候选打印 trace 字段：`icp_cost_based`、`icp_rows_fetched`、`icp_filter_effect`、`icp_cost_if_disabled`、`icp_cost_if_enabled`、`icp_enabled`、`icp_cost_adjustment`、`icp_adjusted_ref_cost`。

离开该代码块前，**务必清空 `tmp_set`**，防止后续迭代或别的调用方看到脏位图。

**保证**：

- 该预演**只会降低候选代价，永不抬高**；任意一道 gate 不通过时 `cur_ref_cost` 与旧行为**逐字节一致**，对不能从 ICP 受益的候选完全透明。
- 对例子 `WHERE a = 4 AND c = 6`，`idx_a` / `idx_ab` 因为 `{c}` 不是它们 keypart 集合的子集，直接被 Gate 3 拒绝，不做任何调整。
- 预演只作用在**候选循环内部**，不写 `POSITION` 任何字段——那仍然由 2.3 在"最终赢家"身上完成。

**暂不覆盖**：range 候选、非 ref 的半连接物化路径、hypergraph 优化器。这些都作为 follow-up 单独跟进（见第 6 节）。

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

由于当前规划器只会写 ON 决策（见 2.3），`has_cost_based_icp_decision_for(keyno) == true` **目前等价于** `use_cost_based_icp() == true`。之所以仍然把两者分开保留，是为了日后校准更准的代价模型时，可以直接"打开"基于代价的 OFF 能力，而不需要再改数据结构。

### 3.2 `sql/sql_optimizer.cc`

在 `JOIN::get_best_combination()` 中，把 `POSITION` 的决策拷贝到 `JOIN_TAB`。

## 4. 执行期的强制校验

文件：`sql/sql_select.cc`

`QEP_TAB::push_index_cond(...)` 中：

- 若当前 `keyno` 上已存在决策且为 OFF：
  - 打印 trace `not_pushed_due_to_icp_cost = true`；
  - 直接返回，不下推 ICP；
- 否则保留社区默认的下推流程。

根据 2.3，**OFF 分支目前处于空转状态**。对应地，MTR 用例用"`not_pushed_due_to_icp_cost` 不应出现"作为反向断言，保护曾经中招的 range-scan 形态。

## 5. 测试覆盖

`mysql-test/t/icp_cost_based.test`：

- **Case 1** —— `icp_cost_based=off`，`FORCE INDEX` + ref 访问：
  验证老行为不变、新 trace 字段不出现。`EXPLAIN FORMAT=TREE` 结果断言计划形态为
  `Index lookup on t1 using idx_abc ..., with index condition: ...`。

- **Case 2** —— `icp_cost_based=on`，`FORCE INDEX` + ref 访问：
  验证新 trace 字段被打印出来，且计划仍然是 `Index lookup` 并带 `index condition: ...` 注解。

- **Case 3（issue-2 回归保护）** —— `icp_cost_based=on`，`FORCE INDEX` + leading 列 range + 尾列等值：
  验证规划器**不会**悄悄关掉 ICP。`EXPLAIN FORMAT=TREE` 断言
  `Index range scan on t3 using idx_ab ..., with index condition: ...`，
  trace 层再用 `icp_suppressed_by_cost_model = 0` 保证没有出现 `not_pushed_due_to_icp_cost`。

- **Case 4（issue-1 ref 形式）** —— `icp_cost_based=on`，**不带任何 index hint**，三把索引（`idx_a`、`idx_ab`、`idx_abc`）共享起始键 `a`，查询为 `WHERE a = 4 AND c = 6`：
  OFF 基线为 `Filter: (tt.c = 6) -> Index lookup on tt using idx_a (a=4)`；
  ON 后计划必须切换为 `Index lookup on tt using idx_abc (a=4), with index condition: (tt.c = 6)`。
  配套一条 `FORCE INDEX(idx_abc)` 对照说明：只要把优化器指向对的那把索引，执行层一直都能跑出这个 plan——问题纯粹出在"访问方法选择"这一步。

- **Case 4a（feature-gate 保护）** —— 同一条查询，把 `icp_cost_based` 切回 `off` 后必须恢复 OFF 基线 plan。这条用例把"开关"钉死，防止哪天因为默认值变 ON 或优化器状态残留悄悄回退。

- **Case 4b（ref 绑满全部 keypart，预演必须不触发）** —— `WHERE a = 4 AND b = 5 AND c = 6`，`icp_cost_based=on`：
  `idx_abc` 的所有 keypart 都被 ref 占用，Gate 3 的 `remaining_cond_cols` 为空，预演应直接跳过；此时期望看到的 plan 是社区行为 `idx_ab` lookup + server-side Filter on `c`，与基线一致。

- **Case 4c（剩余列不在索引上，预演必须不触发）** —— `WHERE a = 4 AND d = 7`：
  `d` 不是任何索引的 keypart，Gate 3 的 `remaining_cond_cols ⊆ key_columns` 子集检查对所有候选都失败；因此 OFF / ON 必须产生完全相同的 plan。

- **Case 5（issue-1 range 形式，已知限制）** —— `WHERE a BETWEEN 2 AND 4 AND c = 50`，不带 hint：
  目前仍回落到 table scan，因为 range 路径还没接入 ref 擂台预演机制；`.result` **故意**把这个"坏"计划钉住——未来 range 形式的修复会直接在 `.result` diff 里冒出来，起到"进度信号"作用。

所有 `EXPLAIN FORMAT=TREE` 输出用 `--replace_regex` 将 `cost=...`、`rows=...`、`(actual time=...)` 等易变数值归一化，保证结果在不同平台与代价调参下稳定。

预期输出存放在 `mysql-test/r/icp_cost_based.result`。

**回归稳健性**：

- `main.1st` 继续绿；
- 广义 ICP 用例集绿：`innodb_icp`、`innodb_icp_all`、`innodb_icp_none`、`range_icp`、`func_in_icp`、`null_key_icp_innodb`。

## 6. 后续工作（follow-up）

- **issue-1 的 range 形式**：把预演机制扩展到 range 候选，让"leading 列 range + 尾列等值"这样的形态（Case 5）也能倾向于"能做 ICP 的更宽索引"，而不是 table scan 或更窄的 range。
- **Hypergraph 优化器**：当前预演只挂在经典的 `find_best_ref()` 路径上；hypergraph 路径另有一套访问方法选择的代码流，需要单独接入一次。
- **selectivity 校准**：`Item::get_filtering_effect()` 偏保守。若上线后的 trace 数据显示在某些谓词形态上系统性地给多/给少奖励，可以在 gate 逻辑不动的前提下把估算器换成更好的；接口约束是"对 `(0,1)` 开区间的单一浮点数 selectivity 作答"。
- **基于代价的 OFF 决策**：2.3 节出于谨慎目前不写 OFF 决策；等代价模型校准更稳后可以打开——执行期（第 4 节）的 OFF 分支已经就位，无需再改数据结构。
