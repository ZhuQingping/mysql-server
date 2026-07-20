# PQ 模块规格模板

> 模块 ID：`PQ-MOD-NNN`
> 状态：`current-contract | commit-bound-current | design-only | deprecated`
> 适用提交：完整 commit SHA
> 最后静态核对：YYYY-MM-DD
> 最后运行验证：commit/build/test report，未验证写 `none`

## 1. 职责与非目标

- 模块负责什么。
- 明确不负责什么。
- 上下游模块和接口边界。

## 2. 入口、输入与输出

| 类型 | 路径与稳定 symbol | 契约 |
|---|---|---|
| 入口 | `path/file.cc::symbol` | 前置条件 |
| 输入 | 类型或状态 | 所有权与有效期 |
| 输出 | 类型或状态 | 后置条件 |

## 3. 主流程

使用最小流程图或调用链，区分 prepare、optimize、execute 和 cleanup 阶段。

## 4. 数据结构、所有权和生命周期

| 对象 | 创建者 | owner | worker-local/shared/borrowed | 销毁者 | 生命周期要求 |
|---|---|---|---|---|---|

## 5. 并发、锁和状态机

- 锁、原子变量、condition/event 和 happens-before。
- 合法状态与转换。
- KILL、error、detach 和重复 cleanup 行为。

## 6. 规范性不变量

每条不变量使用稳定 ID，并使用 MUST/MUST NOT/SHOULD：

```text
PQ-<MODULE>-INV-001
```

## 7. 错误、fallback、retry 和 cleanup

| 失败阶段 | 错误所有者 | 客户端行为 | 能否 fallback/retry | 必须清理的对象 |
|---|---|---|---|---|

## 8. 支持、限制和 feature switch

区分 `implemented`、`partial`、`serial-fallback`、`rejected`、`design-only` 和
`unknown`。不得用“支持”概括带有隐藏约束的能力。

## 9. 可观测性

- EXPLAIN / optimizer trace / status / error log。
- 如何证明实际进入或拒绝该路径。

## 10. 代码与测试映射

| Requirement/Invariant | 源码 symbol | 正向测试 | 负向/故障测试 | 证据状态 |
|---|---|---|---|---|

## 11. 已知缺口和变更影响

- 对应 `conformance_gaps.md` 的 GAP-ID。
- 修改该模块必须联动检查的文件和测试。
- 未经运行验证的推断必须明确标为 `unknown` 或 `audit-candidate`。
