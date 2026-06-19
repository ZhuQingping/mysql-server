# M10 Commercial Test Suite Gap Closure Taskbook

## 状态

Planned。

## 目标

对齐 taurusdbondstore `mysql-test/suite/parallel_query` 商用测试套，完成缺口分类、迁移、skip/fallback 说明和当前分支测试闭环。

推荐拆分：

- M10-A: 测试 manifest：enabled / adapted / deferred；
- M10-B: 基础商用套件名称对齐，如 `pq_variables` vs 当前 `pq_vars`；
- M10-C: 按功能启用 aggregation、ORDER BY、ref/ICP，其他 deferred；
- M10-D: 完整 `parallel_query` suite clean run。

## 允许修改

- `mysql-test/suite/parallel_query/**`
- `Docs/pq_tasks/commercial-port-m10-test-suite-gap-closure.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 先生成测试差异清单；
- V1/M1-M9 已支持能力迁移为 active tests；
- 未支持能力必须记录 skip 原因和目标阶段；
- 不把商用 `.result-pq` 原样复制成错误预期；
- enabled / adapted / deferred 分类必须可追踪到 capability；
- 完整 suite 必须可重复运行。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m10 --tmpdir=/tmp/pqt_m10
```

## Completion Report

Pending.
