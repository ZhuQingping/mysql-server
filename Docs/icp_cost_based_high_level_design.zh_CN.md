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
- **有界奖励（bounded adjustment）**：任何一次 ICP 成本奖励都不得超过"无 ICP 基线"的一个固定比例（详见"ICP 奖励硬封顶"一节）。这样即便 `Item::get_filtering_effect()` 对某种谓词形态严重低估了存活行数，单次估算偏差也不会让某条路径的成本被"打到接近 0"，进而独自支配 join-order DP。

## 特性开关

新增一个 `optimizer_switch` 选项：

- `icp_cost_based=off`（默认值）：走社区老逻辑；
- `icp_cost_based=on`：启用规划阶段的基于代价的 ICP 决策。

## 执行流程

0. `find_best_ref()` 中：**在** `best_access_path()` 的访问方法擂台选出 ref 赢家**之前**，对每个 ref 候选预演一次 ICP 收益，并反馈到该候选自己的代价里——防止最窄的那把索引凭空赢下擂台（**issue-1 ref 形式**，详见后文"ref 候选的基于代价 ICP"一节）。

0b. `get_key_scans_params()` 中：**range 优化器在擂台选出胜出的 range 候选过程中**，对每个候选预演一次 ICP 收益，用"**ICP 调整后的有效代价**"进行胜负比较（而不是用 handler 原始代价）。这解决 **issue-1 的 range 形式**：对于 `WHERE a BETWEEN X AND Y AND c = K` 这种形态，所有 range 候选的 handler 原始代价都会高于 table-scan 基线，于是 range 优化器返回 nullptr，ICP 连"上场机会"都拿不到。详见后文"range 候选的基于代价 ICP"一节。

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

## ref 候选的基于代价 ICP（issue-1 ref 形式）

**动机**：当多把索引共享相同的起始键（例如 `idx_a(a)`、`idx_ab(a, b)`、`idx_abc(a, b, c)`），`find_best_ref()` 原本的擂台只按 fanout 和索引宽度比较。对于 `WHERE a = 4 AND c = 6`，三把索引在 `a = 4` 上都产出相同的 fanout，最窄的 `idx_a` 就赢下擂台——**只有 `idx_abc` 才可能把 `c = 6` 交给引擎用 ICP 过滤**，但这份收益完全被现有擂台忽视。上一节 `best_access_path()` 里的 `icp_cost_based=on` 奖励**来得太晚**，赢家已经选完，救不回来。

**设计**：在 `find_best_ref()` 的每个候选循环里，**在 `cur_ref_cost` 算出之后、进入擂台比较之前**，对这个具体候选预演它的 ICP 收益：

- 若 `icp_cost_based=off`，或 `can_consider_icp_cost()` 否决，跳过；
- 若该 key 没有未绑定 keypart（ref 已经用完所有 keypart），跳过；
- 计算"**被 WHERE 引用但未被当前 ref 绑定**"的列集合；若该集合**不是**当前候选 keypart 集合的子集（说明有列根本不在这把索引上，ICP 无法下推），跳过；
- 否则通过 `Item::get_filtering_effect()` 估算剩余谓词的过滤比例（注意要排除已经被 ref 绑定的列，避免双重计数），传入 `should_enable_icp_by_cost()` 以 `rows_fetched = cur_fanout`、`filter_effect = remaining_filter` 做正式判定；判定为有收益时，从 `cur_ref_cost` 中减去 `cost_if_with_icp − cost_if_no_icp`（一个负值）。

**安全性保证**：预演**只会降低候选代价，永不抬高**。任何一道 gate 不通过时，`cur_ref_cost` 与旧行为逐字节一致，所以拿不到 ICP 收益的候选（比如上面例子里的 `idx_a` / `idx_ab`，因为 `{c}` 不是它们 keypart 集合的子集）自动走"不变"分支。

**适用范围**：这轮改动只修复 issue 1 的 **ref 子树**。range 子树由下面的 range 优化器预演单独解决。

## range 候选的基于代价 ICP（issue-1 range 形式）

**动机**：`get_key_scans_params()` 对每个 range 候选用 handler 上报的 `check_quick_select()` 代价与 `cost_est`（来自 `best_access_path()` 的 table-scan 基线）比较。handler 代价只反映"原始回表成本"，对"SQL 层稍后会不会下推"一无所知。对于 `SELECT * FROM t WHERE a BETWEEN 2 AND 4 AND c = 50`（索引 `(a)`、`(a, b)`、`(a, b, c)`），**三把索引的 range 代价都是基于 `a` 范围估算**，大表场景下三把索引 **全部** 都会高于 table-scan 基线。后果：range 优化器返回 nullptr，`best_access_path()` 回落到 Table scan，ICP 从头到尾没有上场机会。

**设计**：在 `get_key_scans_params()` 的每候选循环中，在 `check_quick_select()` 算出 `(found_records, cost)` 后，**对该候选单独算一份"有效代价"（effective cost）**——只用在擂台比较里：

- gate：`icp_cost_based=on`、引擎支持 `HA_DO_INDEX_COND_PUSHDOWN`、ICP 相关 hint/switch 没禁掉下推、非聚簇主键、非覆盖索引读、非虚拟生成列索引、非多表 UPDATE/DELETE；
- 要求 range 只绑定了索引的**前缀**：`bound_keyparts > 0` 并且 `bound_keyparts < user_defined_key_parts`（至少还有一个尾列可用来下推）；
- 计算"剩余 WHERE 列"（WHERE 列减去已被 range 绑定的列），要求其**是当前 key 所有 keypart 列的子集**（否则有列根本不在本索引上，引擎无法评估）；
- 通过 `Item::get_filtering_effect()` 估算剩余谓词的 selectivity，把 range 已绑定的列作为"已消化集合"传进去；
- 按
  `effective_cost = original_cost * filter + row_eval(found_records * k)`
  计算，其中 `k` 是一个很小的"每索引项评估因子"；只有当 `effective_cost < original_cost` 严格成立，才把 `effective_cost` 用于擂台。

在循环内部：将原来的胜负比较 `read_cost > cost.total_cost()` 改为 `read_cost > effective_cost`；同时用 `best_original_cost` 单独跟踪**当前赢家的 handler 原始代价**。循环结束后，`AccessPath::cost` 写的是 `best_original_cost`，**而不是** ICP 调整后的值——下游所有 cost 计算（`calculate_scan_cost`、EXPLAIN、join 顺序 DP）看到的都是与改动前完全一致的数字，这份预演只影响"擂台赢家选择"，不影响"赢家的 cost 度量"。

**安全性保证**：预演只会**降低**候选的有效代价。任一 gate 不通过时，helper 返回 `original_cost`，`read_cost > cost.total_cost()` 的老比较逻辑字字保留。覆盖索引、聚簇主键、全 keypart 绑定、剩余列跨索引等无法真正从 ICP 获益的形态，都会被 gate 拦掉。index-merge 构造（`ror_only = true`）完全跳过：index-merge 选"rowid 有序的候选"走的是另一套标准，和"range vs table scan"无关。

**与步骤 1 的关系**：若 range 优化器因为这次预演而挑出了之前被 table-scan 挤掉的候选，后续流程照常运转：`best_access_path()` 收到的 `AccessPath` 携带 handler 原始代价；步骤 1 的 scan-path 奖励仍可能再叠一轮；`push_index_cond()` 照常执行真正的下推。**不需要改数据结构。**

## ICP 奖励硬封顶

`Item::get_filtering_effect()` 是"尽力而为"的选择性估算器。遇到复合谓词、索引表达式边界、相关列这些形态，它可能显著**低估**存活行数——这会放大上面三处预演的 ICP 奖励。若不加约束，一次估算偏差就能让某条路径的奖励后成本降到接近 0，独自主导 join-order DP，把"轻微选择性失真"放大成"执行计划结构级的跳变"。

**规则**：任何一次奖励调整必须满足
`|adjustment| <= kIcpBenefitCapRatio * cost_if_no_icp`
其中 `kIcpBenefitCapRatio = 0.5`。换句话说，**单次 ICP 奖励最多抹去原路径一半的成本**，剩下的那一半仍然会被 join-order 规划看到。一旦预演得到的奖励会超过这条上限，就按 50% 夹住——候选依然在擂台里以一个"有界的优势"参与胜负比较。

**三处落点**（覆盖所有奖励通道，任何一处都不能绕过）：

- **scan/range 路径奖励**（`best_access_path()`）：在把 `cost_if_with_icp - cost_if_no_icp` 写进 `best_read_cost` 之前夹住。
- **ref 擂台奖励**（`find_best_ref()`）：在把同一差值写进 `cur_ref_cost` 之前夹住。
- **range 候选预演**（`range_icp_preview()`）：`effective_cost` 的下界为 `(1 - kRangeIcpBenefitCapRatio) * original_cost`（range 形式用的是镜像常量，以便两条通路的预算等价）。

**50% 怎么来的**：实测上，一个被很好利用的 ICP 谓词大约能节省 30%–80% 的回表；50% 足以让真正的赢家保住赢的幅度，又窄到让"单次选择性估算失真"不能独自重塑整个 plan。未来若线上 telemetry 显示这个门槛偏宽/偏窄，只需调整这两个常量再重录基线；想放宽上限，前提是先证明估算器本身更可信。

**可观测性**：真正触发 50% 夹子时，对应 trace 点会额外打一条 `icp_cost_adjustment_capped: true`，与通常的 `icp_cost_adjustment` / `icp_adjusted_*_cost` 并列。这个字段是"ICP 拿到的奖励为什么比原始估算小"的主要排查入口——如果某个按直觉该翻盘的计划最终没翻，看到这个 flag 就说明估算器 + 封顶一起守住了底线。

## 可观测性

当 `icp_cost_based=on` 时，optimizer trace 中会出现以下字段：

- **扫描路径判定**（由 `best_access_path()` 输出）：
  - `icp_cost_based`：是否进入了基于代价的 ICP 评估；
  - `icp_cost_keyno`：评估基于的候选 key；
  - `icp_rows_fetched`、`icp_filter_effect`：模型使用的行数基线与过滤比例；
  - `icp_cost_if_disabled`、`icp_cost_if_enabled`：两种方案的估算代价；
  - `icp_enabled`：代价模型自己的判定结果；
  - `icp_cost_adjustment` / `icp_adjusted_read_cost`：**仅在 ON 分支**（即给了成本奖励）时出现；
  - `icp_cost_adjustment_capped`：**仅当 50% 硬封顶真的夹住了本次奖励时**出现（参见"ICP 奖励硬封顶"一节）；
  - `icp_fallback_to_default`：**仅在模型无法证明收益**、规划器主动回落到社区默认路径时出现。
- **ref 擂台预演**（由 `find_best_ref()` 对每个候选 key 输出）：
  - `icp_cost_based`；
  - `icp_rows_fetched`、`icp_filter_effect`；
  - `icp_cost_if_disabled`、`icp_cost_if_enabled`、`icp_enabled`；
  - `icp_cost_adjustment` / `icp_adjusted_ref_cost`：**仅当该候选被打了折扣时**才出现；
  - `icp_cost_adjustment_capped`：**仅当 50% 硬封顶真的夹住了本候选的奖励时**出现。
- **range 擂台预演**（由 `get_key_scans_params()` 对每个候选 key 输出，嵌在 `analyzing_range_alternatives` 之内）：
  - `icp_cost_based`；
  - `icp_rows_fetched`、`icp_filter_effect`；
  - `icp_cost_if_disabled`、`icp_cost_if_enabled`、`icp_enabled`；
  - `icp_cost_adjustment` / `icp_adjusted_range_cost`：**仅当该候选被打了折扣时**才出现；
  - `icp_cost_adjustment_capped`：**仅当 50% 硬封顶真的夹住了本候选的奖励时**出现。

当 `icp_cost_based=off` 时，上述字段都不会出现。若某个 ref 候选没有通过预演 gate（例如没有未绑定的 keypart，或剩余 WHERE 列不在这把索引上），则该候选不会输出 ref 预演那一组字段——这也是 gate 命中情况的可观测信号。

`push_index_cond()` 这一层仍然会在"检测到已写入 OFF 决策并因此跳过下推"时输出 `not_pushed_due_to_icp_cost`。由于当前规划器不会写入 OFF 决策，这个 token 在实际运行中**不应出现**，MTR 用例据此做反向断言以作为回归保护。

## 验证

使用 `main.icp_cost_based` 用例验证：

- **Case 1**：OFF + FORCE INDEX + ref —— 与社区行为一致，新字段都不出现。
- **Case 2**：ON + FORCE INDEX + ref —— 新 trace 字段被打印，并且 `EXPLAIN FORMAT=TREE` 中仍然保留 `with index condition: ...`。
- **Case 3**（issue-2 回归保护）：ON + FORCE INDEX + leading 列 range + 尾列等值 —— 必须仍然产生 `Index range scan on ... with index condition: ...`，同时 `icp_suppressed_by_cost_model = 0`，防止特性悄悄关掉本形态的 ICP。
- **Case 4**（issue-1 ref 形式）：ON + **不带任何 index hint**、三把索引共享起始键 `a` —— 计划必须从 OFF 基线 `Filter(c=6) + Index lookup on idx_a` 切换为 `Index lookup on idx_abc, with index condition: (tt.c = 6)`。配套的 `FORCE INDEX(idx_abc)` 对照说明执行层自始至终都支持这个 plan，问题一直只在"访问方法选择"层面。
- **Case 4a**（feature-gate 保护）：同样的查询，把 `icp_cost_based` 切回 `off`，必须恢复 OFF 基线 plan——防止"默认变 ON"或"优化器状态残留"造成意外回归。
- **Case 4b**（全 keypart 被 ref 绑定，预演必须**不触发**）：`WHERE a = 4 AND b = 5 AND c = 6` —— `idx_abc` 的所有 keypart 都被 ref 占用，没有剩余 WHERE 列可让 ICP 吸收；Gate 3 的 `remaining_cond_cols` 为空，预演应直接跳过，plan 保持与社区一致（`idx_ab` lookup + server-side Filter）。
- **Case 4c**（剩余列不在任何索引上，预演必须**不触发**）：`WHERE a = 4 AND d = 7` —— `d` 不是任何索引的 keypart，`remaining_cond_cols ⊆ key_columns` 子集检查对所有候选都失败；OFF 与 ON 产生完全相同的 plan。
- **Case 5**（issue-1 range 形式）：ON + 不带 hint，`WHERE a BETWEEN 2 AND 4 AND c = 50`，索引 `idx_a`/`idx_ab`/`idx_abc`。OFF 基线为 Table scan（每个 range 候选 handler 原始代价都超过 table-scan 基线）。ON 后 plan 必须切换为
  `Index range scan on tt using idx_abc over (2 <= a <= 4), with index condition: ((tt.c = 50) and (tt.a between 2 and 4))`——range 候选预演奖励了唯一能把 `c = 50` 下推进引擎的 `idx_abc`。
- **Case 5a**（range + 全 keypart 绑定，预演必须**不触发**）：`WHERE a BETWEEN 2 AND 4 AND b = 3 AND c = 50` —— range 已经把优化器能用到的所有 keypart 全部绑定，`bound_keyparts == user_defined_key_parts`，gate 拒绝奖励；plan 必须和社区行为一致，**不因 feature flag 变化**。
- **Case 5b**（剩余列不在任何索引上，预演必须**不触发**）：`WHERE a BETWEEN 2 AND 4 AND d = 7` —— `d` 不是任何索引的 keypart，`remaining_cond_cols ⊆ key_cols` 子集检查对所有候选都失败；plan 必须与 OFF 基线同为 Table scan。
- **Case 6a**（多表 guardrail，STRAIGHT_JOIN）：`tj_driver STRAIGHT_JOIN tt WHERE d.id <= 3 AND tt.c = 50`，`SELECT tt.payload` 强制非覆盖扫描。ON 时 `tt` 的访问方式可能从 `idx_a`（OFF）升级到 `idx_abc + index condition: (tt.c = 50)`（ON），但 `STRAIGHT_JOIN` 固定了驱动表，plan 拓扑（`d` 驱动、`tt` inner）在 OFF/ON 之间保持一致。
- **Case 6b**（多表 guardrail，自由 join order）：同样两张表，不加 `STRAIGHT_JOIN`。奖励允许升级 `tt` 的访问方式，但**最终选中的 JOIN ORDER（小的 `tj_driver` 驱动、`tt` inner）必须在 OFF/ON 之间不变**。否则就意味着 `rows_fetched` / `filter_effect` 串进了 join-order DP；我们在奖励里**显式不写**这两个字段，就是为了守这条线。
- **Case 6c**（多表 guardrail，EXISTS 子查询，**预期策略变化**）：`EXISTS (SELECT 1 FROM tt WHERE tt.a = d.a AND tt.c = 50 AND tt.payload <> '')`。这条用例**故意**记录一个 **预期行为变化**：`advance_sj_state()` / `fix_semijoin_strategies_for_picked_join_order` 做 semijoin 策略选择时会读 `pos->read_cost`，而 `pos->read_cost` 是**带奖励值**的。OFF 时优化器选 `MaterializeLookup`（对 `tt` 做一次 table scan + 哈希 probe）；ON 时由于 `idx_abc + ICP` 让每次 probe 成本足够低，`FirstMatch` 跑赢了 Materialize。两者都是正确 plan，但运维上线这个特性时**要预期到**：EXISTS / IN 子查询的 semijoin 策略可能发生迁移。

同时保证 `main.1st` 以及更广的 ICP / semijoin 用例集绿灯：
- ICP：`innodb_icp`、`innodb_icp_all`、`innodb_icp_none`、`range_icp`、`func_in_icp`、`null_key_icp_innodb`；
- Semijoin：`subquery_sj_firstmatch`、`subquery_sj_mat`、`subquery_sj_loosescan`。
