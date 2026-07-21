# PQ Context 重构独立审核结论

> 文档 ID：`PQ-QUALITY-REVIEW-CTX-002`
> 审核日期：2026-07-21
> 审核对象：`f9970c78a249b486195293b94e8b69d4da3c2b91`
> 稳定对照基线：`1b9ffd755d4`
> 审核方式：三名独立 Agent 只读审查；未修改代码，未执行全量 MTR
> 总结建议：`ACCEPT WITH RISKS`

> 历史范围说明：本审核固定针对 `f9970c78a249` 的原始 context 重构。其后的
> `PQ_thd_context` 原子错误信号加固不改变本审核的历史结论；实现合同和验证证据分别见
> [`pq_context_refactor_design.md`](../../../../../pq_context_refactor_design.md) 与
> [`verification_evidence.md`](verification_evidence.md)。

## 1. 结论

当前 THD、Query_block、JOIN 的 Parallel Query（PQ）context 重构未发现已证实的
等价性回归或必须立即修复的代码缺陷。重构正确地把 PQ 状态按宿主对象的生命周期
聚合为按值持有的 `PQ_thd_context`、`PQ_query_block_context` 和
`PQ_join_context`。

该结论不等于“所有运行期组合均已验证”或“性能无劣化”。完整 Release/Debug/ASAN
矩阵和 stable/refactor 工作负载性能对照仍是待完成验收项。

## 2. 已审查并确认的等价性合同

| 范畴 | 审查结论 | 主要证据 |
| --- | --- | --- |
| THD 生命周期 | 保持初始化、析构、statement cleanup 与 retry 标记复位时序 | `sql_class.cc`、`PQ_thd_context::{initialize_mem_root,destroy_mem_root,cleanup_statement}` |
| Worker/status | worker 仍只复制既有 `has_pq`/DOP；found-rows 合并分支未改变 | `PQ_thd_context::{copy_from,merge_status}` |
| Clone 链接 | 原/clone 双向指针、assert、link/unlink 合同保持 | `PQ_query_block_context::{last_clone,clone_of,link_clone,unlink_clone}` |
| Fallback/restore | const table、where/having、group/order、QEP/ref/reverse-scan 恢复顺序保持 | `PQ_query_block_context::restore`、`PQ_join_context::restore_plan` |
| 错误/KILL | local error、leader killed、leader PQ error、leader THD error 的短路顺序保持 | `THD::is_pq_error()` |
| Debug 注入 | `skip_fetch_ctx` 的 worker 传播、cleanup 与 Temptable guard 均已迁移 | `sql_parallel.cc`、`pq_context.cc`、`handler_pq.cc` |
| 原成员访问 | 主仓内未发现已迁移 THD/Query_block/JOIN 原始成员的遗留访问 | 相对基线 diff 与定向 `rg` 审查 |

`THD::in_sp_trigger` 保留在 `THD`，没有被误迁移到 context；这是正确的，因为它是
通用 stored-program 生命周期状态，而不是 PQ 自身状态。

## 3. 性能审核结论

### 3.1 已确认

- macOS arm64 AppleClang Release（`-O3 -DNDEBUG`、`WITH_LTO=OFF`）下，
  `THD::is_pq_error()` 保持头内联。
- `mysqld` 符号表没有 `PQ_thd_context::is_error` wrapper；MQ send/receive 循环
  的反汇编没有调用该 wrapper。
- 原始 context 重构未引入 mutex、atomic、virtual dispatch、`shared_ptr` 或新的共享所有权；
  PQ MEM_ROOT、QEP、map 和临时表参数的分配为既有逻辑迁移。后续的原子错误信号加固是
  有意的窄例外：它只保护既存 worker-to-leader cancellation flag，并使用 relaxed load/store。
- Debug DWARF 对象布局：`THD` 为 `+8B`，`JOIN` 为 `+8B`，`Query_block` 不变。

### 3.2 尚不能下结论

不能仅根据静态代码和符号审计宣称“性能无劣化”或“性能更好”。对象布局与成员位置
发生变化，可能影响 cache locality 和每连接常驻内存；小型 clone accessor 从头内联
改为跨编译单元调用也可能影响优化/clone/fallback 控制面。

最低成本性能验收应在同一台空闲机器、相同 Release 配置和数据集上，对 stable/refactor
交替运行至少五次，记录原始数据、吞吐、median、P95/P99、CPU 与每连接 RSS。负载至少
覆盖 PQ scan/MQ-Gather 大结果集、range/index scan、hash join/aggregate、短 PQ 查询
并发和大量空闲连接。

## 4. 风险与处置

| 优先级 | 风险/机会 | 处置 |
| --- | --- | --- |
| P0 | MQ error predicate 回退为 `.cc` wrapper 将损害高频轮询路径 | 保持 `THD::is_pq_error()` 头内联，作为硬约束。 |
| P1 | 仓外 C++ 模块可能直接使用旧 `THD::PQ_CLONE_PHASE` 或 PQ 公共字段 | 检索并构建仓外插件/私有模块；如需要兼容，提供 deprecated 类型别名或窄 accessor，不能恢复可变 raw fields。 |
| P1 | 性能无对照数据 | 完成第 3.2 节基准后才能关闭性能验收。 |
| P1（可选） | 五个 Query_block clone accessor 现为跨编译单元小调用 | 先测量；若存在可重复回归，再恢复为内联，不改变 ownership 模型。 |
| P2 | context 公开大量可变字段，当前是“聚合”而非严格封装 | 在独立后续重构中按 lifecycle、topology、clone、plan-rewrite 分批收敛为窄 API。 |
| P2 | `pq_context.h` 为 `PQUnsuiteInfo` 引入 `pq_optimizer.h` | 提取轻量 `pq_types.h`，降低核心头文件的编译扇出。 |
| P2 | context 含借用指针和资源状态，但可隐式 copy/move | 在后续独立变更中显式禁止 copy/move，防止未来误复制。 |
| P2 | manifest 中 `implementation_commit` 实际是比较基线，语义可能误导 AI | 后续改为 `comparison_base_commit` 并单列 reviewed refactor commit。 |

## 5. 架构判断

本轮采用三个按宿主生命周期划分的 context 是合理的：THD 承载连接/worker 与资源状态，
Query_block 承载语义、资格与 clone 状态，JOIN 承载计划改写与恢复状态。不要把它们合并为
一个总 context，也不要为“封装”引入 pimpl、shared ownership、虚函数或额外锁；这些做法
会破坏当前按值存储和低热路径开销的优势。

下一阶段的收益不在于再次搬移字段，而在于以小步方式收敛状态转换 API、明确资源/reset
合同、降低头文件耦合，并以性能数据决定是否恢复极小 accessor 的内联。

## 6. 审核边界

- 已完成：静态等价性、生命周期、热路径、对象布局、设计文档一致性与提交隔离审查。
- 已有定向运行证据：10 个 Debug PQ lifecycle/error/clone/fallback 用例；详见
  [`verification_evidence.md`](verification_evidence.md)。
- 未完成：完整 Release/Debug/ASAN MTR 矩阵、仓外扩展兼容性构建、性能与内存对照。
