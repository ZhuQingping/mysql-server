# D30 pq_support_features_switch 变量契约

## 目标

对齐商用实现中的 `pq_support_features_switch` flagset 变量，为后续
simple aggregate、count distinct、correlated subquery、hash join spill 和
insert-select 等场景迁移提供配置契约。

本任务只迁移变量、默认值、SET/DEFAULT/SET_VAR 可见性和 THD helper，不把
这些 flag 接入 optimizer eligibility 或执行路径。

## 范围

允许修改：

- `sql/parallel_query/pq_optimizer.h`
- `sql/sys_vars.cc`
- `sql/system_variables.h`
- `sql/sql_class.h`
- `mysql-test/suite/parallel_query/t/pq_commercial_support_features_switch.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_support_features_switch.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

禁止修改：

- PQ eligibility 判定逻辑
- worker / Exchange / InnoDB 执行路径
- hash join / subquery / insert-select 行为

## 设计

- flag 名称按商用实现保留：
  `simple_aggregate`、`count_distinct`、`correlated_subquery`、
  `hash_join_spill_to_disk`、`insert_select`、`default`。
- 默认值按商用实现保留：
  `simple_aggregate=on`、`correlated_subquery=on`，其余 OFF。
- 新增 `THD::pq_support_features_switch_flag()`，为后续迁移使用商用同名
  helper 预留源码契约。

## 验证计划

1. RED：新增 `pq_commercial_support_features_switch` 后先运行 MTR，确认因
   unknown system variable 失败。
2. GREEN：实现 sysvar 后运行：
   - `cmake --build build-ninja --target mysqld -j 8`
   - `cd build-ninja/mysql-test && ./mtr --suite=parallel_query pq_commercial_support_features_switch pq_vars`
3. 独立 review agent 检视代码、测试和商用资料对齐。

## 完成报告

- RED：`pq_commercial_support_features_switch` 在实现前因
  `Unknown system variable 'pq_support_features_switch'` 失败。
- GREEN：`mysqld` build 通过；targeted MTR
  `pq_commercial_support_features_switch pq_vars` 通过。
- Review：独立 review agent 已 ACCEPT。提交前补充了 review 建议的
  `SET GLOBAL` 新 session 继承覆盖，以及 numeric sentinel/overflow 边界覆盖。
