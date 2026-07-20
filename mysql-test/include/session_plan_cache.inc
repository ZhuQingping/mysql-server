--source include/no_ps_protocol.inc
--source include/count_sessions.inc
--source include/elide_costs.inc

# We will be displaying global status variables which FLUSH STATUS does not reset (Cached_plan_invalidations is of type SHOW_FUNC in mysqld.cc, and SHOW_FUNC variables are not reset, see reset_status_vars()); if mysqld has already executed some other mtr tests and they used plan cache, this status will contain effects from those other tests, so restart:
--source include/restart_mysqld.inc

# This SET below helps with two things:
# - there is OPTIMIZE TABLE in this test; if the auto-update runs at the same time we can get a deadlock error (as in BUG2025090204090)
# - the plan in explain_distinct_ranges is unstable, probably due to the auto-update.
set global dstore_use_histogram_auto_update=off;

# PLAN_CACHE_PORT dstore does not support index merge, partitioning, so we have an include file which is run twice, once for innodb, once for dstore.

connect (con_analyze,localhost,root,,test,,); # Will be used to test ANALYZE TABLE
connection default;
select @@global.rds_plan_cache;

set @old_plan_cache = @@rds_plan_cache;
set rds_plan_cache=on;

create table t1
(
  a int primary key,
  b char(10)
);
insert into t1 values (1,'one');
insert into t1 values (2,'two');
insert into t1 values (3,'three');
insert into t1 values (4,'four');
analyze table t1;

# basic functionality
set @a=2;
--echo "test const table"
--replace_column 10 #
explain select * from t1 where a = 2;
prepare stmt from 'select * from t1 where a = ?';
show session status like "cached_plan%";
execute stmt using @a;
execute stmt using @a;
execute stmt using @a;
show session status like "cached_plan%";
flush status;
--echo "test system table"
create table s_table(a int, b int) engine=heap;
insert into s_table values(1,2);
--replace_column 10 #
explain select * from s_table;
prepare stmt from 'select * from s_table';
execute stmt;
execute stmt;
execute stmt;
show session status like "cached_plan%";
insert into s_table values(1,2);
--replace_column 10 #
explain select * from s_table;
execute stmt;
show session status like "cached_plan%";
execute stmt;
show session status like "cached_plan%";
flush status;
drop table s_table;
--echo "test full table scan"
set @a='two';
let $param = 'two';
let $query=select * from t1 where b = $param;
--echo # In EXPLAIN it may show that parallel query is used (making plan
--echo # caching impossible), but when we use a prepared statement for the
--echo # real SELECT, PQ is not used, and plan caching can thus be done.
--replace_column 10 #
eval explain $query;
let $param = ?;
let $query=select * from t1 where b = $param;
eval prepare stmt from '$query';
execute stmt using @a;
execute stmt using @a;
execute stmt using @a;
show session status like "cached_plan%";
flush status;
--echo "test ref table scan"
alter table t1 add index idx(b);
let $param = 'two';
let $query=select * from t1 where b = $param;
--replace_column 10 #
eval explain $query;
let $param = ?;
let $query=select * from t1 where b = $param;
eval prepare stmt from '$query';
execute stmt using @a;
execute stmt using @a;
execute stmt using @a;
show session status like "cached_plan%";
flush status;
--echo "test range table scan"
let $param = 't';
let $query=select * from t1 where b > $param;
--replace_column 10 #
eval explain $query;
let $param = ?;
let $query=select * from t1 where b > $param;
eval prepare stmt from '$query';
set @a='t';
execute stmt using @a;
execute stmt using @a;
execute stmt using @a;
show session status like "cached_plan%";

# PLAN_CACHE_PORT changed ANALYZE to OPTIMIZE. Grep for table_ref_version in sql_plan_cache.cc for more info

--echo "Test that OPTIMIZE TABLE in another connection invalidates the plan"
connection con_analyze;
set rds_plan_cache=on;
optimize table t1;
connection default;
flush status;
execute stmt using @a;
show session status like "cached_plan%";
show global status like "cached_plan%";
disconnect con_analyze;
flush status;
--echo "test index merge intersect"
alter table t1 add column c int default 2;
alter table t1 add index idx1(c);
insert into t1 values(5,'five',3);
insert into t1 values(6,'two',3);
insert into t1 values(7,'two',5);
let $param1 = 'two';
let $param2 = 3;
let $query=select * from t1 where b = $param1 and c = $param2;
# Sometimes it reports intersect(idx,idx1) sometimes intersect(idx1,idx)

--disable_query_log
--disable_result_log
analyze table t1 update histogram on b,c;
analyze table t1;
--enable_result_log
--enable_query_log
 
--replace_column 7 # 8 # 10 # 12 #
eval explain $query;
let $param1 = ?;
let $param2 = ?;
let $query=select * from t1 where b = $param1 and c = $param2;
set @a = 'two';
set @b = 3;
eval prepare stmt from "$query";
execute stmt using @a,@b;
execute stmt using @a,@b;
execute stmt using @a,@b;
show session status like "cached_plan%";
flush status;
--echo "test index merge union"
let $param1 = 'two';
let $param2 = 5;
let $query=select * from t1 where b = $param1 or c = $param2;
--replace_column 10 #
eval explain $query;
let $param1 = ?;
let $param2 = ?;
let $query=select * from t1 where b = $param1 or c = $param2;
set @a = 'two';
set @b = 5;
eval prepare stmt from "$query";
execute stmt using @a,@b;
execute stmt using @a,@b;
execute stmt using @a,@b;
show session status like "cached_plan%";
flush status;
--echo "test index skip scan"
# PLAN_CACHE_PORT: in the original version using pq_clone: skip-scan access method had a non-working stub for pq_clone(), so plan cache could not work with it.
--echo "Skip scan is supported as we do not use pq_clone anymore"
create table t(a int, b int, d int, e int, key(b, d));
let $param1 = 2;
let $query=SELECT /*+ skip_scan(t) */b, d FROM t WHERE d < $param1;
--replace_column 10 #
eval explain $query;
let $param1 = ?;
let $query=SELECT /*+ skip_scan(t) */b, d FROM t WHERE d < $param1;
set @a = 2;
eval prepare stmt from '$query';
execute stmt using @a;
drop table t;
show session status like "cached_plan%";
flush status;
--echo "test group by"
set @a = 'two';
set @b = 3;
prepare stmt from "select sum(a) from t1 where c > 1 group by b";
--sorted_result
execute stmt;
--sorted_result
execute stmt;
show session status like "cached_plan%";
flush status;
--echo "don't support with-rollup"
prepare stmt from "select sum(a) from t1 where c > 1 group by b with rollup";
--sorted_result
execute stmt;
--sorted_result
execute stmt;
show session status like "cached_plan%";
--echo "test aggregation without GROUP BY"
prepare stmt from "select sum(b + ?), count(*) from t1";
execute stmt using @b;
execute stmt using @b;
execute stmt using @b;
show session status like "cached_plan%";
flush status;
--echo "test window function"
prepare stmt from 'select a, c, sum(a+c+?) over (rows unbounded preceding) foo from t1 order by foo desc';
--sorted_result
execute stmt using @b;
--sorted_result
execute stmt using @b;
--sorted_result
execute stmt using @b;
show session status like "cached_plan%";
flush status;
prepare stmt from 'select a, c, sum(a+c) over (rows between unbounded preceding and current row) foo from t1';
--sorted_result
execute stmt;
--sorted_result
execute stmt;
--sorted_result
execute stmt;
show session status like "cached_plan%";
flush status;
prepare stmt from 'select a, min(c), sum(c), sum(c) over (rows unbounded preceding) wf from t1 group by (a) order by wf desc';
execute stmt;
execute stmt;
execute stmt;
show session status like "cached_plan%";
flush status;
--echo "test order by"
drop table t1;
create table t1 (a int, b int);
insert into t1 (a,b) values (2,8),(1,9),(3,7);
prepare stmt from "select * from t1 order by ?";
set @a=NULL;
execute stmt using @a;
--echo "Reprepare will invalidate cached plan. So here cached_plan_hits is 0"
set @a=1;
execute stmt using @a;
show session status like "cached_plan%";
set @a=2;
execute stmt using @a;
show session status like "cached_plan%";
flush status;
deallocate prepare stmt;

--echo "test some special cases"
create table t2 (a int);
insert into t2 values (0);

# parameter is NULL
set @arg00=NULL;
prepare stmt from 'select 1 FROM t2 where a=?';
execute stmt using @arg00;
execute stmt using @arg00;
execute stmt using @arg00;
show session status like "cached_plan%";
flush status;

drop table t1,t2;
deallocate prepare stmt;

--echo "test limit offset"
create table t1 (a int);
insert into t1 (a) values (1), (2), (3), (4), (5), (6), (7), (8), (9), (10);
prepare stmt from "select * from t1 limit ?, ?";
set @offset=0, @limit=1;
execute stmt using @offset, @limit;
execute stmt using @offset, @limit;
execute stmt using @offset, @limit;
drop table t1;
show session status like "cached_plan%";
flush status;

--echo "test FOUND_ROWS"
create table t1 (id int);
insert into t1 values (1),(2),(3),(4),(5),(6);
# PLAN_CACHE_PORT we added 6 rows, so that the next added one will represent less than 20%, to not cause an invalidation
prepare stmt from "select SQL_CALC_FOUND_ROWS * from t1";
# Expect 6 rows
execute stmt;
execute stmt;
select found_rows();
show session status like "cached_plan%";
insert into t1 values (1);
# Expect 7 rows
execute stmt;
select found_rows();
show session status like "cached_plan%";
flush status;
deallocate prepare stmt;
drop table t1;

--echo "test LIKE operator"
create table t1 (a varchar(10));
insert into t1 (a) values ('yahoo');
set character_set_connection=latin1;
prepare stmt from 'select a from t1 where a like ?';
set @var='google';
execute stmt using @var;
execute stmt using @var;
execute stmt using @var;
show session status like "cached_plan%";
flush status;
deallocate prepare stmt;
drop table t1;

--echo "test BETWEEN ... AND optimization."
create table t1 ( a bigint );
insert into t1 values(1),(2),(3);
prepare stmt from 'select a from t1 where a between ? and ?';
set @a=1;
execute stmt using @a, @a;
execute stmt using @a, @a;
show session status like "cached_plan%";
set @b=3;
execute stmt using @a, @b;
execute stmt using @a, @b;
show session status like "cached_plan%";
flush status;
--echo "test BETWEEN ... AND with bigint"
set @a='111e9999';
set @b='1';
execute stmt using @a, @b;
execute stmt using @a, @b;
show session status like "cached_plan%";
flush status;
drop table t1;
deallocate prepare stmt;

--echo "test RAND function"
create table t1 (a int);
insert into t1 (a) values (1), (2), (3), (4);
prepare stmt from
"select rand(),
        cast(rand(10)*10000000000 as unsigned integer),
        cast(rand(?)*10000000000 as unsigned integer) from t1";
set @var=1;
--replace_column 1 - 2 - 3 -
execute stmt using @var;
set @var=2;
--replace_column 1 - 2 - 3 -
execute stmt using @var;
drop table t1;
show session status like "cached_plan%";
flush status;
deallocate prepare stmt;

create table t1 (a int);
prepare stmt from "select * from t1 where 1 > (SELECT 0)";
execute stmt;
execute stmt;
show session status like "cached_plan%";
flush status;
--echo "block subquery"
prepare stmt from "select * from t1 where 1 > (1 in (SELECT * FROM t1))";
execute stmt;
execute stmt;
show session status like "cached_plan%";
drop table t1;
deallocate prepare stmt;

--echo "Only support one table"
create table t1(a int);
create table t2(b int);
create view v1 as select b from t1, t2;

prepare stmt from 'select 1 from v1';
execute stmt;
execute stmt;

prepare stmt from 'select 1 from t1, t2';
execute stmt;
execute stmt;
show session status like "cached_plan%";
drop view v1;
drop table t1, t2;
deallocate prepare stmt;

# requires partitioning
if (!$missing_features)
{
--echo "test partitioned table"
create table t1 (a int)
  partition by list (a)
  (partition p0 values in (1,2,3), partition p1 values in (4,5,6));

insert into t1 values (1),(2),(3),(4),(5),(6);
--replace_column 10 #
explain select * from t1 where a in (2,3);
set @a=2;
set @b=3;
prepare stmt from "select * from t1 where a in (?,?)";
execute stmt using @a, @b;
--echo "Cached plan uses table records to check whether number of records"
--echo " is changed. If prune table occurs, records estimated might be different"
--echo " before and after prune table. Causes invalidation."
execute stmt using @a, @b;
show session status like "cached_plan%";
--echo "Change the range so that pruning does not occur => no invalidation."
flush status;
set @a=3;
set @b=4;
execute stmt using @a, @b;
execute stmt using @a, @b;
show session status like "cached_plan%";
flush status;
drop table t1;
deallocate prepare stmt;
}

--echo "test special cases"
create table t1(a int, b int, primary key(a));
insert into t1 values(1,2), (3,4);
prepare stmt from "select * from t1 where a = ? and b > ?";
set @a=3;
set @b=2;
execute stmt using @a,@b;
set @a=1;
execute stmt using @a,@b;
show session status like "cached_plan%";
flush status;
drop table t1;
select now() into @a;
create table t1(a timestamp, primary key(a));
insert into t1 values(@a + interval 1 second);
--replace_column 10 #
explain select count(*) from t1 where a = @a + interval 1 second;
prepare stmt from "select count(*) from t1 where a = ? + interval 1 second";
execute stmt using @a;
execute stmt using @a;
select sleep(1);
select now() into @a;
execute stmt using @a;
show session status like "cached_plan%";
flush status;
drop table t1;

--echo More tests of "system" and "const" tables
create table t1(a int, b int) engine=myisam;
insert into t1 values(3,4);
--replace_column 10 #
explain select * from t1 where a=3 and b=4;
prepare s from 'select * from  t1 where a=3 and b=4';
execute s;
update t1 set a=3,b=5;
execute s;
show status like "cached_plan%";
drop table t1;
flush status;
create table t1(a int primary key, b int);
insert into t1 values(3,4);
--replace_column 10 #
explain select * from t1 where a=3 and b=4;
prepare s from 'select * from  t1 where a=3 and b=4';
execute s;
update t1 set a=3,b=5;
execute s;
show status like "cached_plan%";
drop table t1;
flush status;

--echo "block temporary table"
create temporary table if not exists t1 (a1 int);
prepare stmt from "select  * from t1";
--enable_warnings
execute stmt;
execute stmt;
show session status like "cached_plan%";
drop temporary table t1;
deallocate prepare stmt;

--echo "test derived table"
create table t1(a int);
prepare stmt from "select * from (select * from t1) as tt";
execute stmt;
execute stmt;
show session status like "cached_plan%";
prepare stmt from "select * from (select a from t1 group by a) as tt";
execute stmt;
execute stmt;
show session status like "cached_plan%";
flush status;
drop table t1;
deallocate prepare stmt;

--echo "block stored function"
create table t1(c1 int);
insert into t1 values(1),(2);
create function f1() returns int deterministic return 1;
prepare stmt from "select c1 from t1 where c1 = f1()";
execute stmt;
execute stmt;
show session status like "cached_plan%";
drop function f1;
deallocate prepare stmt;
drop table t1;

--echo "don't support UNION"
create table t1(a int);
insert into t1 values (2), (3), (1);

prepare stmt from
  '(select a from t1) union (select a+10 from t1) order by a';

execute stmt;
execute stmt;
show session status like "cached_plan%";

prepare stmt from
  '(select a from t1) union all (select a+10 from t1) order by a';
execute stmt;
execute stmt;
show session status like "cached_plan%";
deallocate prepare stmt;

--echo "invalid cached plan if table is changed"
prepare stmt from 'select * from t1 order by 1';
execute stmt;
execute stmt;
show session status like "cached_plan%";
alter table t1 add column b int;
execute stmt;
show session status like "cached_plan%";

--echo "invalid cached plan if table records are changed sharply"
execute stmt;
show session status like "cached_plan%";
select count(*) from t1;
--echo "from 3 rows to 5 is a high increase"
insert into t1 select * from t1 where a<3;
select count(*) from t1;
execute stmt;
show session status like "cached_plan%";

--echo "when rds_plan_cache_allow_change_ratio is 0, plan cache work even if table records are changed sharply"
select @@rds_plan_cache_allow_change_ratio;
set rds_plan_cache_allow_change_ratio = 0;
execute stmt;
show session status like "cached_plan%";
--echo "table rows change from 5 rows to 20, plan cache work"
insert into t1 select * from t1;
insert into t1 select * from t1;
execute stmt;
show session status like "cached_plan%";
set rds_plan_cache_allow_change_ratio = default;
select @@rds_plan_cache_allow_change_ratio;
flush status;

drop table t1;

--echo "fixed issue#42: main.const_folding has an incorrect result"
create table tinytbl(cola tinyint unsigned);
insert into tinytbl values (0), (1),
       (0),(1),(3),
       (254), (255),
       (null);
analyze table tinytbl;
prepare stmt from 'select * from tinytbl where cola >= ?';
set @maxint=255;
set @minint=0;
execute stmt using @maxint;
execute stmt using @minint;
show session status like "cached_plan%";
flush status;

deallocate prepare stmt;
drop table tinytbl;

--echo "issue#58: Block user and system variables"
create table t1(a int);
insert into t1 values (2), (3), (1);
prepare stmt from 'select @a from t1';
set @a=1;
execute stmt;
show session status like "cached_plan%";

flush status;
drop table t1;

--echo "BUG2022061800209: Ref scan with multiple Key parts can result in crash"
show session status like "cached_plan%";
create table t1(a int, b int, c int, key(a, b, c));
insert into t1 values(1,2,3),(1,2,4),(3,2,6),(3,4,5);
--replace_column 10 #
explain select * from t1 where a = 1 and b = 2;
prepare stmt from 'select * from t1 where a = ? and b = ?';
set @a = 1, @b = 2;
execute stmt using @a, @b;
execute stmt using @a, @b;
set @a = 3, @b = 2;
execute stmt using @a, @b;
drop table t1;

--echo "test optimizer trace for plan cache"
show session status like "cached_plan%";
create table t1(a int, b int);
insert into t1 values(1,2),(3,4);
prepare stmt from 'select * from t1 where a > ?';
set @a = 1;
set optimizer_trace='enabled=on';
execute stmt using @a;
--replace_regex /  \"cost\".*// /  \"cost_for_plan\".*//
select * from information_schema.optimizer_trace;
show session status like "cached_plan%";
execute stmt using @a;
show session status like "cached_plan%";
select * from information_schema.optimizer_trace;
insert into t1 select * from t1;
analyze table t1;
execute stmt using @a;
insert into t1 select * from t1;
execute stmt using @a;
--replace_regex /  \"cost\".*// /  \"cost_for_plan\".*// /  \"cached table records\".*/\  "cached table records\"/ /  \"current table records\".*/  \"current table records\"/
select * from information_schema.optimizer_trace;
set optimizer_switch='mrr=off';
execute stmt using @a;
--replace_regex /  \"cost\".*// /  \"cost_for_plan\".*//
select * from information_schema.optimizer_trace;
set optimizer_switch=default;

# PLAN_CACHE_PORT: here there was a piece which required a debug binary (used "set debug="); so in Taurus the entire test file was skipped if using a release binary (someone inserted a "have_debug" requirement at the top). It is bad to have zero testing of a feature in a release binary, so this little section is moved to a debug-specific mtr test instead.

set optimizer_trace='enabled=off';
drop table t1;

--echo BUG2022072201617: Plan cache should not cache SELECT FOR SHARE/UPDATE (PLAN_CACHE_PORT: now it does!)
create table t1(a int, b int);
insert into t1 values(1,2),(3,4);
prepare stmt from 'select * from t1 for update';
execute stmt;
execute stmt;
show session status like "cached_plan%";
if (!$missing_features)
{
prepare stmt from 'select * from t1 lock in share mode';
execute stmt;
execute stmt;
show session status like "cached_plan%";
}
drop table t1;
# This is tested more later in this file.

--echo # BUG2023032903203
flush status;
CREATE TABLE t0 (c0 INT, c1 INT);
show session status like "cached_plan%";
PREPARE p1 FROM 'SELECT 1 FROM t0 GROUP BY t0.c0, t0.c1';
EXECUTE p1;
EXECUTE p1;
show session status like "cached_plan%";
DROP PREPARE p1;
drop table t0;

--echo BUG2023060701013: Plan cache causes a incorrect result
flush status;
CREATE TABLE t1 (decimal_col decimal(25,5) NOT NULL, time_col time NOT NULL);
insert into t1 values(99999999999999999999.99999, '08:59:59');
prepare stmt from "SELECT decimal_col, time_col FROM t1 WHERE time_col BETWEEN '06:59:59' AND decimal_col limit 1;";
execute stmt;
show session status like "cached_plan%";
execute stmt;
show session status like "cached_plan%";
DROP TABLE t1;
deallocate prepare stmt;

--echo BUG2023061900127: Virtual column cause a server crash for plan cache
# PLAN_CACHE_PORT due to how the new impl works (grep for
# hidden_items_from_optimization in sql_plan_cache.cc for more), the
# plan is not cached.
CREATE TABLE datalink_dataLink_0046 (
 $createTime datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
 $json json NOT NULL,
 $updateTime datetime NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
 active bit(1) GENERATED ALWAYS AS ((json_extract($json,_utf8mb4'$."active"') = true)) VIRTUAL NOT NULL,
 code varchar(100) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."code"'))) VIRTUAL NOT NULL,
 corporationId varchar(50) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."corporationId"'))) VIRTUAL NOT NULL,
 entityId varchar(60) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."entityId"'))) VIRTUAL NOT NULL,
 id varchar(60) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
 name varchar(300) CHARACTER SET gb18030 COLLATE gb18030_chinese_ci GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."name"'))) VIRTUAL NOT NULL,
 masterId varchar(60) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."masterId"'))) VIRTUAL,
 sourceId varchar(40) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."sourceId"'))) VIRTUAL,
 visible bit(1) GENERATED ALWAYS AS ((json_extract($json,_utf8mb4'$."visible"') = true)) VIRTUAL NOT NULL,
 ownerId varchar(100) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."ownerId"'))) VIRTUAL,
 json_createTime bigint GENERATED ALWAYS AS (json_unquote(json_extract($json,_utf8mb4'$."createTime"'))) VIRTUAL NOT NULL,
 PRIMARY KEY (id),
 UNIQUE KEY by_corporationId_code_entityId (corporationId,code,entityId),
 KEY idx_updateTime ($updateTime),
 KEY by_corporationId_entityId_json_createTime (corporationId,entityId,json_createTime),
 KEY by_sourceId_corporationId_entityId (sourceId,corporationId,entityId),
 KEY by_corporationId_active_entityId (corporationId,active,entityId),
 KEY by_entityId_ownerId (entityId,ownerId),
 KEY by_corporationId_visible (corporationId,visible),
 KEY by_masterId_corporationId (masterId,corporationId),
 KEY by_entityId_updateTime_corporationId (entityId,$updateTime,corporationId)
);
flush status;
prepare stmt from "SELECT t0.$json FROM datalink_dataLink_0046 AS t0 WHERE t0.corporationId = 'wYsa4Mg1j86800' AND (t0.id IN ('wYsa4Mg1j86800::', 'wYsa4Mg1j86800:31010123:1120346342', 'wYsa4Mg1j86800:31010123:1120347655', 'wYsa4Mg1j86800:31010123:1120397694', 'wYsa4Mg1j86800:31010123:1120691803', 'wYsa4Mg1j86800:31010123:1113531223')) ORDER BY JSON_EXTRACT(t0.$json, '$.\"createTime\"') DESC";
execute stmt;
show session status like "cached_plan%";
execute stmt;
show session status like "cached_plan%";
DROP TABLE datalink_dataLink_0046; 
deallocate prepare stmt;
set rds_plan_cache = @old_plan_cache;

--echo #
--echo # BUG2023110302419 The interaction between features plan cache and NDP causes crash.
--echo #

# PLAN_CACHE_PORT had to disable the lines which set non-existent NDP
# variables. So the test's goal is void, but still it is kept.

# Expect normal execution without crashing.
CREATE TABLE `sbtest` (
  `id` INT UNSIGNED NOT NULL DEFAULT '0',
  `k` INT UNSIGNED NOT NULL DEFAULT '0',
  `c` CHAR(120) NOT NULL DEFAULT '',
  `pad` CHAR(60) NOT NULL DEFAULT ''
) DEFAULT CHARSET=utf8mb4;
INSERT INTO `sbtest` VALUES (1,49929,'83868641912-28773972837-60736120486-75162659906-27563526494-20381887404-41576422241-93426793964-56405065102-33518432330','67847967377-48000963322-62604785301-91415491898-96926520291'),
(2,50198,'38014276128-25250245652-62722561801-27818678124-24890218270-18312424692-92565570600-36243745486-21199862476-38576014630','23183251411-36241541236-31706421314-92007079971-60663066966');
# SET @old_ndp_row_filter_factor = @@ndp_row_filter_factor;
# SET @old_ndp_minimum_scan_page = @@ndp_minimum_scan_page;
# SET @old_ndp_mode = @@ndp_mode;
SET rds_plan_cache = ON;
# SET ndp_row_filter_factor = 1;
# SET ndp_minimum_scan_page = 0;
PREPARE stmt FROM 'SELECT c FROM sbtest WHERE id BETWEEN ? AND ? ORDER BY c';
SET @var1 = 1;
SET @var2 = 2;
# SET ndp_mode = ON;
EXECUTE stmt USING @var1,@var2;
# SET ndp_mode = off;
EXECUTE stmt USING @var1,@var2;
# SET ndp_mode = ON;
EXECUTE stmt USING @var1,@var2;
show status like "cached_plan%";
DEALLOCATE PREPARE stmt;
SET rds_plan_cache = @old_plan_cache;
# SET ndp_row_filter_factor = @old_ndp_row_filter_factor;
# SET ndp_minimum_scan_page = @old_ndp_minimum_scan_page;
# SET ndp_mode = @old_ndp_mode;
DROP TABLE `sbtest`;

--echo # BUG2024030102055 : Incorrect execution result when join->zero_result_cause = nullptr
create table bsc_erc1155_owner (
  id bigint unsigned not null auto_increment,
  nft_unique_num varchar(20) not null,
  contract varchar(128) not null,
  token_id varchar(128) not null,
  address varchar(128) not null,
  amount varchar(256) not null,
  primary key (id),
  unique key idx_nft_unique_num (nft_unique_num,address)
);
insert into bsc_erc1155_owner(nft_unique_num,contract,token_id,address,amount)
  values('xv1e342cccc11qss','0x03a9037cf84abe90de5aea6052c9e2a0b933e2f6',
  '0x0000000000000000000000000000000000000000000000000000000000000002','tfs2fe56jmssstccccehRnADB8HU4C9eTl',1);

--connect(con1,localhost,root,,test)
--connect(con2,localhost,root,,test)

# JOIN::type() == JT_CONST
--connection con1
set rds_plan_cache=on;
prepare pd from "select id from bsc_erc1155_owner where nft_unique_num = ? and address =  ?";
set @p='xv1e342cccc11qss';
set @q='tfs2fe56jmssstccccehRnADB8HU4C9eTl';
execute pd using @p,@q;

set @p='xv1e342ccccq756';
set @q='tfs2fe56jmssstccccenADB8HU4C9eTl';
execute pd using @p,@q;

set @p='xv1e342cccc11qss';
set @q='tfs2fe56jmssstccccehRnADB8HU4C9eTl';
execute pd using @p,@q;

show status like "cached_plan%";

# Full table scan + Impossible where_cond
--connection default
alter table bsc_erc1155_owner drop index idx_nft_unique_num;

--connection con2
set rds_plan_cache=on;
prepare pd from "select id from bsc_erc1155_owner where nft_unique_num = ? and 1 = ?";
set @p='xv1e342cccc11qss';
set @q=1;
execute pd using @p,@q;

set @p='xv1e342ccccq756';
set @q=2;
execute pd using @p,@q;

set @p='xv1e342cccc11qss';
set @q=1;
execute pd using @p,@q;

show status like "cached_plan%";

--connection default
--disconnect con1
--disconnect con2
drop table bsc_erc1155_owner;

# PLAN_CACHE_PORT This 'JT_SYSTEM' section was in the pq_8.0.22 branch
# and not in the Taurus branch, is kept in case it is useful.

# JOIN::type() == JT_SYSTEM
create table t1 (
  id bigint unsigned not null auto_increment,
  col1 char(8),
  col2 char(8),
  primary key (id)) engine=memory;
insert into t1(col1,col2) values('fe','2');

prepare pd from "select * from t1 where col1 = ? or col2 = ?";
set @p='fe';
set @q='2';
execute pd using @p,@q;

set @p='f454';
set @q='3';
execute pd using @p,@q;

set @p='fe';
set @q='2';
execute pd using @p,@q;

show status like "cached_plan%";

drop table t1;

--echo # BUG2024031600480

set rds_plan_cache=on;
create table tbl1 (
  id_col int not null auto_increment,
  decimal_col decimal(25,5) default null,
  blob_col blob,
  tinytext_col tinytext,
  unique key id_col (id_col desc),
  key key_dec1 (decimal_col)
);
prepare stmt from "select blob_col from tbl1 a where a.decimal_col in (?,NULL) and a.tinytext_col in (?,'iiiiiiiiii1','iiiiiiiiii1')";
set @col1=-999.99;set @col2='iiiiiiiiii1';execute stmt using @col1,@col2;
set @col1=100.111;set @col2='jkj';execute stmt using @col1,@col2;
set @col1=NULL;set @col2='a';execute stmt using @col1,@col2;
show status like "cached_plan%";
drop table tbl1;

--echo # BUG2024031905199
set rds_plan_cache=on;
create table tbl1 (
  id_col int not null auto_increment,
  decimal_col decimal(25,5) default null,
  char_col char(4) DEFAULT NULL,
  unique key id_col (id_col desc),
  key key_dec1 (decimal_col)
);
insert into tbl1(decimal_col, char_col) values(99999999999999999999.99999, 'aaa2'),
  (-99999999999999999999.99999,'aaaa'),(-99999999999999999999.99999,'aabb'),
  (0.00000,'ab'),(0.00000,NULL),(-99999999999999999999.99999,'aacd'),
  (0.00000,'aabb');
prepare stmt from "select char_col, decimal_col from tbl1 A where A.char_col not like ? or A.decimal_col = ? order by 1,2";
set @col1='aacd%';set @col2=3.1415926;execute stmt using @col1,@col2;
set @col1='aabb%';set @col2=0.00000;execute stmt using @col1,@col2;
show status like "cached_plan%";
drop table tbl1;

--echo # BUG2024031905317
set rds_plan_cache=on;
CREATE TABLE tbl1 (
  id_col int NOT NULL DEFAULT '0',
  time_col time(6) DEFAULT NULL,
  KEY ndx_time_col (time_col)
);
insert into tbl1 values(1,'02:59:59.000000'),(2,'00:00:00.000000'),(3,'24:59:59.000000'),(4,'23:23:59.000000'),(4,NULL);

# If you want to study this test, first grep for BUG2024031905317 further down.
prepare stmt from "SELECT time_col FROM tbl1 WHERE time_col = ? limit 10";
set @col3='02:59:59.000000';execute stmt USING @col3;
# Note an invalidation here, because when we store this wrong TIME into the Index_lookup object, it causes an error.
set @col3='03:59:60.000000';execute stmt USING @col3;
show status like "cached_plan%";
drop table tbl1;

--echo # BUG2024030103702 Plan cache result in memory usage growth
set rds_plan_cache=ON;

# prepare table schema and data
create table sbtest1 (
  id int not null,
  k int not null,
  c char(128) not null,
  pad char(64) not null,
  primary key (id),
  key k_1 (k)
);

create table t1(n1 int, n2 int, s char(20), vs varchar(20), t text, ts timestamp);
insert into t1 (n1, n2, s, vs, t, ts) values (1, 11, 'one', 'eleven', 'eleven', '2018-01-01 00:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (1, 11, 'one', 'eleven', 'eleven', '2019-06-30 12:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (2, 11, 'two', 'eleven', 'eleven', '2018-01-01 00:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (2, 12, 'two', 'twelve', 'twelve', '2019-06-30 12:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (2, 13, 'two', 'thirteen', 'foo', '2018-01-01 14:30:30');
insert into t1 (n1, n2, s, vs, t, ts) values (2, 13, 'two', 'thirteen', 'foo', '2018-01-01 14:30:30');
insert into t1 (n1, n2, s, vs, t, ts) values (2, 13, 'two', 'thirteen', 'bar', '2018-01-01 15:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (NULL, 13, 'two', 'thirteen', 'bar', '2020-07-01 00:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (2, NULL, 'two', 'thirteen', 'bar', '2020-08-01 00:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (3, 13, NULL, 'thirteen', 'bar', '2018-12-31 00:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (3, 13, 'two', NULL, 'bar', '2018-12-31 00:00:00');
insert into t1 (n1, n2, s, vs, t, ts) values (3, 13, 'two', 'thirteen', NULL, '2020-08-01 00:00:00');

delimiter ||;
create procedure insert_data(in n int)
begin
declare i int default 1;
while (i <= n ) do
insert into sbtest1(id,k,c,pad) values(i,round(rand()*100),repeat(md5(i%5),4),repeat(md5(rand()),2));
set i=i+1;
end while;
end ||
delimiter ;||

call insert_data(1000);
analyze table sbtest1;

# execute select query
--disable_warnings
# test distinct
prepare explain_tree_distinct_ranges from "explain format=tree select distinct c from sbtest1 where id between ? and ? order by c";
prepare explain_distinct_ranges from "explain select distinct c from sbtest1 where id between ? and ? order by c";
prepare distinct_ranges from "select distinct c from sbtest1 where id between ? and ? order by c";
set @a=200;
set @b=400;
--replace_regex $elide_costs_and_rows
execute explain_tree_distinct_ranges using @a,@b;
--replace_column 10 #
execute explain_distinct_ranges using @a,@b;
execute distinct_ranges using @a,@b;
show status like "cached_plan%";
set @a=400;
set @b=600;
# PLAN_CACHE_PORT On second execution, "using temporary; using
# filesort" is present in EXPLAIN TRADITIONAL output (this was a bug
# in the initial impl; discovered, investigated and fixed during the
# porting).
--replace_regex $elide_costs_and_rows
execute explain_tree_distinct_ranges using @a,@b;
--replace_column 10 #
execute explain_distinct_ranges using @a,@b;
execute distinct_ranges using @a,@b;
set @a=600;
set @b=800;
--replace_regex $elide_costs_and_rows
execute explain_tree_distinct_ranges using @a,@b;
--replace_column 10 #
execute explain_distinct_ranges using @a,@b;
execute distinct_ranges using @a,@b;
show status like "cached_plan%";
# test group by
prepare group_by_stmt from 'select count(*), t from t1 where t = ? group by t order by t desc';
set @arg1 = 'eleven';
execute group_by_stmt using @arg1;
execute group_by_stmt using @arg1;
execute group_by_stmt using @arg1;
show status like "cached_plan%";
--enable_warnings

# cleanup
drop table sbtest1;
drop table t1;
drop procedure insert_data;

--echo # BUG2024033002828
set rds_plan_cache=ON;

create table sbtest1 (
  id_col int not null auto_increment,
  smallint_col smallint default null,
  time_col time(6) default null,
  varchar_col varchar(255) default null,
  binary_col binary(11) default null,
  unique key id_col (id_col desc),
  key ndx_id_time_col (id_col desc,time_col desc),
  key ndx_id_varchar_col (id_col desc,varchar_col(4) desc),
  key ndx_time_varchar_col (time_col desc,varchar_col(4) desc)
);
insert into sbtest1(smallint_col, time_col, binary_col) values(-32768, '05:59:59.000000', 0x6363636363636363636331);
insert into sbtest1(smallint_col, time_col, binary_col) values(-2635, '05:59:59.000000', 0x6363636363636363636331);
insert into sbtest1(smallint_col, time_col, binary_col) values(-32768, '05:59:59.000000', 0x6363636363636363636331);
insert into sbtest1(smallint_col, time_col, binary_col) values(878, '05:59:59.000000', 0x6363636363636363636331);
insert into sbtest1(smallint_col, time_col, binary_col) values(45, '05:59:59.000000', 0x6363636363636363636331);

prepare stmt from "select time_col, binary_col, smallint_col from sbtest1 A where A.time_col in (?,'14:00:30.000000',NULL) and A.smallint_col in (?,-2635,-32768,878) order by 1,2";
set @col1='05:59:59.000000';set @col2=45;execute stmt USING @col1,@col2;
# Note an invalidation here, because the previous execution had two ranges in the range scan for time_col (because the first IN has two non-NULL values), whereas this execution has only one range (the first IN has one non-NULL value only), and reinit_qep_tab_properties() compares the number of ranges.
set @col1=NULL;set @col2=32767;execute stmt USING @col1,@col2;
show status like "cached_plan%";
# An invalidation, as we go from one range to two ranges, again:
set @col1='03:59:60.000000';set @col2=-32768;execute stmt USING @col1,@col2;
show status like "cached_plan%";
drop table sbtest1;
deallocate prepare stmt;

--echo # PQ refactor fix_field process

# requires LONGTEXT
if (!$missing_features)
{

--echo # BUG2024082609113 : query'result with plan cache is incorrect.
create table test1 (
  id_col int not null auto_increment,
  time_col time(6) default null,
  longtext_col longtext,
  unique key id_key (id_col DESC)
);
insert into test1(time_col, longtext_col) values
('00:00:00.000000',"lll2"), ('12:00:00.000000',"l"),
('01:59:59.000000',"llllllllll1"), ('02:59:59.000000',"llllllllll1"),
('03:59:59.000000',"llllllllll1"), ('04:59:59.000000',"llllllllll1"),
('05:59:59.000000',"llllllllll1"), ('06:59:59.000000',"llllllllll1"),
('07:59:59.000000',"llllllllll1"), ('08:59:59.000000',"llllllllll1"),
('09:59:59.000000',"llllllllll1"), ('10:59:59.000000',"llllllllll1"),
('11:59:59.000000',"llllllllll1"), ('12:59:59.000000',"llllllllll1"),
('13:59:59.000000',"llllllllll1"), ('14:59:59.000000',"llllllllll1"),
('15:59:59.000000',"llllllllll1"), ('16:59:59.000000',"llllllllll1"),
('17:59:59.000000',"llllllllll1"), ('18:59:59.000000',"llllllllll1"),
('19:59:59.000000',"llllllllll1"), ('20:59:59.000000',"llllllllll1"),
('21:59:59.000000',"llllllllll1"), ('22:59:59.000000',"llllllllll1"),
('23:59:59.000000',"llllllllll1"), ('24:59:59.000000',"llllllllll1");

let $query =
prepare stmt from "select longtext_col, time_col from test1 A where A.longtext_col  = 'bbb' or A.time_col not between '01:59:59.000000' AND ? ";

set rds_plan_cache = 0;
eval $query;
set @col2='24:59:59.000000';
execute stmt USING @col2;
set @col2='12:59:59.000000';
execute stmt USING @col2;

set rds_plan_cache = 1;
eval $query;
set @col2='24:59:59.000000';
execute stmt USING @col2;
set @col2='12:59:59.000000';
execute stmt USING @col2;

show status like "cached_plan%";

deallocate prepare stmt;
drop table test1;
}

CREATE TABLE cols(
 s1 INTEGER, s2 INTEGER,
 u1 INTEGER, u2 INTEGER,
 i1 INTEGER, i2 INTEGER);
CREATE TABLE source(b1 INTEGER, b2 INTEGER);

INSERT INTO source VALUES(1, 1);
INSERT INTO cols(i1, s1, u1) VALUES(1, 1, 1);
INSERT INTO cols(i2, s2, u2) VALUES(1, 1, 1);
INSERT INTO cols(i1) VALUES((SELECT b2 FROM source));
INSERT INTO cols(i1) SELECT b1 FROM source;
INSERT INTO cols(i1) SELECT b2 FROM source;

set rds_plan_cache = 1;
prepare stmt_sh1 from 'SELECT 1 FROM cols HAVING SUM(s1)=1';
execute stmt_sh1;
execute stmt_sh1;
show status like "cached_plan%";
deallocate prepare stmt_sh1;

drop table cols, source;

# PLAN_CACHE_PORT: here there was a piece which required a debug binary (used "set debug="); so in Taurus the entire test file was skipped if using a release binary (someone inserted a "have_debug" requirement at the top). It is bad to have zero testing of a feature in a release binary, so this little section is moved to a debug-specific mtr test instead.

--echo # more tests following the porting to 8.0.41

# cleanup old PS, for clearer "show status" later
drop prepare pd;
drop prepare group_by_stmt;
drop prepare distinct_ranges;
drop prepare explain_distinct_ranges;
drop prepare explain_tree_distinct_ranges;
drop prepare s;

flush status;
show session status like "cached_plan%";

create table t (vc varchar(100),
 gc varchar(10) generated always as (reverse(vc)) stored,
 key (gc(3)));

# We have a prefix index, so that both the index-lookup Item and
# "using where" Item are tested.

insert into t(vc) values ('a'), ('abc'), ('abcabc'), ('abcdef');
analyze table t;

prepare s from "explain format=tree select * from t where reverse(vc) = 'cba'";
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";

# The traditional format shows less info about the condition attached
# to the table, but more about the JOIN's overall condition (which
# isn't executed, but still good to see)

prepare s from "explain select * from t where reverse(vc) = 'cba'";
flush status;
show session status like "cached_plan%";
--replace_column 10 #
execute s;
--replace_column 10 #
execute s;
show session status like "cached_plan%";

prepare s from "select * from t where reverse(vc) = 'cba'";
flush status;
show session status like "cached_plan%";
execute s;
execute s;
show session status like "cached_plan%";
drop table t;

# If GC is substituted in ORDER BY, we cannot cache. Note that, unlike
# in the previous case, we need a full-length index, otherwise there
# cannot be substitution in ORDER BY. Note also that EXPLAIN is
# misleading: it suggests that no substitution was done, but if we
# look at the optimizer trace we see 'gc' is used in the filesort
# stage.

create table t (vc varchar(100),
 gc varchar(10) generated always as (reverse(vc)) stored,
 key (gc));
insert into t(vc) values ('a'), ('abc'), ('abcabc'), ('abcdef');
analyze table t;

prepare s from "explain format=tree select * from t order by reverse(vc)";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";

# Test with WHERE on covering index

prepare s from "explain format=tree select gc from t where reverse(vc)='a'";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";

prepare s from "select gc from t where reverse(vc)='a'";
flush status;
execute s;
execute s;
show session status like "cached_plan%";

drop table t;

# Test of constant propagation; in '=', then in '<' (completely
# different code path in the optimizer; the first one using
# multiple-equal item).

create table t (a int, b int);
insert into t values (1,1),(1,2),(2,1),(2,2);
analyze table t;

# We can see that propagation is not lost when using the cached plan (change_item_tree() is not called, constant item 1 is put in "a=b" and stays)
prepare s from "explain format=tree select * from t where a=b and a=1";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";

prepare s from "explain select * from t where a=b and a=1";
flush status;
--replace_column 10 #
execute s;
--replace_column 10 #
execute s;
show session status like "cached_plan%";

prepare s from "select * from t where a=b and a=1";
flush status;
execute s;
execute s;
show session status like "cached_plan%";

# We can see that propagation is not lost when using the cached plan (change_item_tree() is called in first optimization, constant item 1 is put in "a<b", this is recorded in the cached plan, then is undone at end of execution, then on second execution we fetch the cachec plan and replay the change of "a" to "a" in "a<b")
prepare s from "explain format=tree select * from t where a<b and a=1";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";

prepare s from "explain select * from t where a<b and a=1";
flush status;
--replace_column 10 #
execute s;
--replace_column 10 #
execute s;
show session status like "cached_plan%";

prepare s from "select * from t where a<b and a=1";
flush status;
execute s;
execute s;
show session status like "cached_plan%";

# Same with non-comparison item '+' (<cache> is not lost).
# We turn this off as we want to see how Items in ICP condition are cached:
set @old_empty=@@rds_empty_redundant_check_in_range_scan;
set @@rds_empty_redundant_check_in_range_scan=false;

prepare s from "explain format=tree select * from t where (a+2)<b and a=1";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";

# If we then have a range access on 'b' during first execution (3<b),
# when we execute the cached plan the condition is still 3<b (has been
# reverted to (a+2)<b then rolled forward to 3<b), so the creation of
# the range access path still works. If we missed the rolling forward,
# this creation would fail (as 'a' is not in the index) and the plan
# would be invalidated.

# This query also proves the need for calling update_used_tables() in
# get_item_and_refresh(), in a quite complicated sequence of events.
# When rds_empty_redundant_check_in_range_scan is off, in the first
# optimization the Item which leads to the range condition, a+2<b
# (depends on 'a'), becomes 1+2<b (after optimize_cond()) (does not
# depend on 'a' anymore, the used_tables() of '+' is now correctly 0
# thanks to a call to update_used_tables()), then <cache>(1+2)<b
# (after cache_const_expr_transformer()). Then the condition is saved
# in the cached plan ('<' is a permanent item, not a clone ; though we
# make a clone for the Item_cache inside). When the second execution
# starts, before the cached plan is applied, WHERE is back to (a+2<b),
# restore_cmd_properties() sets the used_tables() of '+' back to the
# map of "t" (as the '+' depends on 'a' again). Then the cached plan
# is applied, we roll forward to <cache>(1+2)<b and the <cache> has
# used_tables() of 0 (just as when we made this clone, and it was not
# changed by restore_cmd_properties() as it was not part of permanent
# items). So when reinit_qep_tab_properties() calls the range
# optimizer on this condition, this succeeds in building a range
# ("constant < column" is fine). Now, let's see what changes when
# rds_empty_redundant_check_in_range_scan is on (the default), then it
# runs in reduce_cond_for_table() and eliminates 1+2<b from the
# table's condition, just before it goes into
# cache_const_expr_transformer(). So Item_cache is not created. It is
# thus not cloned when caching the plan. Then when the cached plan is
# applied, and we roll forward from 'a' to '1', the WHERE is (1+2)<b,
# and used_tables() of '+' is 1, and so the range cannot be
# made. The key difference is the presence or absence of a Item_cache
# with used_tables()==0. That is why it is important to run
# update_used_tables() on Items when applying the plan; in the latter
# case above, the '+' thus gets to know that its arguments are
# constant (1 and 2) so is constant itself.

alter table t add key(b);
prepare s from "explain format=tree select * from t force index (b) where (a+2)<b and a=1";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";
drop table t;
set @@rds_empty_redundant_check_in_range_scan=@old_empty;

# This exercices the item replacement done in change_cond_ref_to_const():
# "Y" is cloned and replaced into the '<' with another "Y" having the same collation as the column to compare to,
# and this calls change_item_tree(). We replay it.
create table t1 (s1 char(5) character set utf8 collate utf8_lithuanian_ci);
insert into t1 values ('I'),('K'),('Y');
prepare s from "explain format=tree select * from t1 where s1 < 'K' and s1 = 'Y'";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";
drop table t1;

# This tests that we clone injected CAST nodes

CREATE TABLE dt_t (dt DATETIME, i INT);
prepare s from "EXPLAIN FORMAT=TREE SELECT * from dt_t WHERE dt_t.dt = dt_t.i";
flush status;
--replace_regex $elide_costs_and_rows
execute s;
--replace_regex $elide_costs_and_rows
execute s;
show session status like "cached_plan%";
DROP TABLE dt_t;

# This tests a variant of BUG2024031905317 (see earlier above) ; the
# reason is: with the changes in the impl of plan cache for 8.0.41,
# the earlier test, using temporal values, was not exercising its
# related fix anymore; indeed the call to fix_fields() in the new
# clone_item(), calling convert_constant_arg(), created
# Item_time_with_ref to wrap Item_param, thus
# has_same_type_and_item_params() in cache_plan() noticed the type
# change (from Item_param to Item_time_with_ref) and thus turned the
# cache off ; thus the presence or absence of
# cmp_item_params_after_reduce_cond() in
# JOIN::finalize_table_conditions() (= the bugfix) didn't make any
# difference, anymore, nullifying the test. With this new test, which
# uses integers, we avoid the "*_with_ref" thing and exercise the code
# fix again: without the call to has_same_type_and_item_params(), we
# would get "255" for the search of 1000. The reason being: when
# optimizing when the parameter is 3, ref lookup is set up and the
# filter (Item_func_eq) is removed (as it's already "done" by the ref
# lookup, as noticed by reduce_cond_for_table() via test_if_ref());
# later when executing when the parameter is 1000, 1000 is stored into
# the ref, 1000 is capped to 255, then 255 is looked up; this is where
# the missing filter would be very much needed, to finally eliminate
# the wrong 255. With the check done by
# has_same_type_and_item_params(), the plan is not cached and the
# problem disappears. That is the fix made by: commit
# c859981a53afc30d7a590130780b180ed442c8fa Author: x00465057
# <xuemengjiao@huawei.com> Date: Tue Mar 26 17:16:18 2024 +0800
# BUG2024031905317 Result incorrect when rds_plan_cache = on BUT it
# introduces a new problem: any ref access similarly becomes non
# cachable (unless it's on a unique key, i.e. we have a const table).
# This is pretty severe. And some queries of TPC-C are concerned. Thus
# a different fix is made in this implementation:
# has_same_type_and_item_params() is not called anymore; instead, when
# we store 1000 we detect the truncation (in replace_cache_key()), and
# throw away the cached plan. This way, for a proper query where
# values fit into types (as in TPC-C) we get caching back. The queries
# below serve to exercise this new fix. And it is the undoing of the
# too-severe fix which explains why in the result file we get more
# cached plans (changes in SHOW STATUS output earlier in this file).

create table t2 (a tinyint unsigned, key(a));
insert into t2 values (0),(3),(255);
analyze table t2;
prepare s from "select * from t2 where a=?";
flush status;

# two in-bound values: there is caching

set @a=3;
execute s using @a;
show session status like "cached_plan%";
set @a=0;
execute s using @a;
show session status like "cached_plan%";

# starting from scratch again

prepare s from "select * from t2 where a=?";
flush status;
set @a=3;
execute s using @a;
show session status like "cached_plan%";

# out-of-bounds value 1000: cache is disabled permanently for this
# statement, even for future in-bounds values 3 and 0.

set @a=1000;
execute s using @a;
show session status like "cached_plan%";
set @a=1000;
execute s using @a;
show session status like "cached_plan%";
set @a=3;
execute s using @a;
show session status like "cached_plan%";
set @a=0;
execute s using @a;
show session status like "cached_plan%";
set @a=1000;
execute s using @a;
show session status like "cached_plan%";

drop table t2;

# Tests for Items created by constant folding

# Item_time_literal

create table t1 
(
  c1  tinyint, c2  smallint, c3  mediumint, c4  int,
  c5  integer, c6  bigint, c7  float, c8  double,
  c9  double precision, c10 real, c11 decimal(7, 4), c12 numeric(8, 4),
  c13 date, c14 datetime, c15 timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP 
  ON UPDATE CURRENT_TIMESTAMP, c16 time, c17 year,
  primary key(c1)
);

insert into t1
  ( c1, c13, c14, c15, c16, c17 )
values
  ( 20, '1991-01-01 01:01:01', '1991-01-01 01:01:01', '1991-01-01 01:01:01',
        '1991-01-01 01:01:01', '1991') ;
flush status;
prepare s from "select 'true' as found from t1 
where c1= 20 and c13= CAST('1991-01-01 01:01:01' AS DATE) and c14= '1991-01-01 01:01:01' and
  c15= '1991-01-01 01:01:01' and c16= '1991-01-01 01:01:01' and
  c17= '1991-01-01 01:01:01'" ;
EXECUTE s;
EXECUTE s;
show session status like "cached_plan%";

DROP TABLE t1;

# Item_func_lt (lower than)

CREATE TABLE t1 (a INT, b INT, KEY(a,b));
INSERT INTO t1 VALUES (1,1000), (1,1001), (1,2000),
(1,2001), (1,3000), (1,3002);
ANALYZE TABLE t1;
flush status;

select (@thread_id:=thread_id)*0 from performance_schema.threads where processlist_id=connection_id();
show session status like "cached_plan%";

PREPARE s FROM "SELECT a, MAX(b) FROM t1 WHERE a = 1 AND b <= 1999.5 GROUP BY a";

# This is a set of 3 queries to measure the memory usage of Plan Cache.
# This stores in variables the net amount of allocations and current number of bytes used.
let $measure_mem=
select (@delta_alloc:=(count_alloc-count_free))*0,
       (@bytes:=current_number_of_bytes_used)*0
       from performance_schema.memory_summary_by_thread_by_event_name
       where event_name="memory/sql/plan_cache_mem_root" and thread_id=@thread_id;
# This uses the variables above as reference, to verify that nothing has changed since they were set.
let $check_mem_unchanged=
select @delta_alloc=(count_alloc-count_free),
       @bytes=current_number_of_bytes_used
        from performance_schema.memory_summary_by_thread_by_event_name
        where event_name="memory/sql/plan_cache_mem_root" and thread_id=@thread_id;
# This checks that there are no allocations, no bytes used.
let $measure_zero_mem=
select count_alloc=count_free,
       current_number_of_bytes_used=0
       from performance_schema.memory_summary_by_thread_by_event_name
       where event_name="memory/sql/plan_cache_mem_root" and thread_id=@thread_id;

# initial state of plan cache (empty), and of memory (nothing used by cache):
show session status like "cached_plan%";
eval $measure_zero_mem;
EXECUTE s;
# Execute again and verify that it causes an invalidation:
show session status like "cached_plan%";
eval $measure_mem;
select @delta_alloc>0, @bytes>0;
EXECUTE s;
eval $measure_mem;
select @delta_alloc>0, @bytes>0;
show session status like "cached_plan%";

# We see a plan was cached but it was not used later (hit=0). This is because:

# - during the optimization, we pick a range access of type
# GROUP_INDEX_SKIP_SCAN, then we drop the GROUP BY clause as the WHERE
# clause ensures we have only one group (a=1).
# - so we cache a plan which has GROUP_INDEX_SKIP_SCAN and no GROUP BY.

# - on second EXECUTE, we apply the cached plan,
# reinit_qep_tab_properties() calls test_quick_select() to build the
# range access, but the key difference is that GROUP BY is now empty,
# so it does not make sense to use GROUP_INDEX_SKIP_SCAN, and instead
# it chooses INDEX_RANGE_SCAN (it's like if making a range access for
# "SELECT a, MAX(b) FROM t1 WHERE a = 1 AND b <= 1999.5"). Then
# reinit_qep_tab_properties() notices that the chosen range access is
# not like the initial one, and cautiously decides that it's not safe.
# So the cached plan is invalidated, and we optimize again, and this
# time we have a GROUP BY, so we'll make a GROUP_INDEX_SKIP_SCAN and
# cache it, again.

# As what happened in the second execution (get an inconsistent range
# access path, invalidate the cached plan and free its MEM_ROOT, make
# a new range access path, make a cached plan in a MEM_ROOT), gets
# repeated over and over below, the next-produced cached plans should
# not use more memory than the second one did. We check that below. We
# also check that the number of allocations-minus-free is also
# identical.

EXECUTE s;
eval $check_mem_unchanged;
show session status like "cached_plan%";
EXECUTE s;
eval $check_mem_unchanged;
show session status like "cached_plan%";
DROP PREPARE s;

# here we check that plan cache uses no memory anymore.
eval $measure_zero_mem;
show session status like "cached_plan%";

DROP TABLE t1;

# Item_func_isnotnull. This is interesting: as the column's range is
# [0,255], a condition like colA<256 is always true if colA is a
# number, but it is UNKNOWN if colA is NULL. MySQL rewrites it to
# WHERE colA IS NOT NULL.

CREATE TABLE t1 (colA TINYINT UNSIGNED );
INSERT INTO t1 VALUES (0), (1),
(0),(1),(3),
(254), (255),
(NULL);
ANALYZE TABLE t1;
flush status;
PREPARE s FROM "SELECT * FROM t1 WHERE colA < 256";
EXECUTE s;
EXECUTE s;
show session status like "cached_plan%";
DROP TABLE t1;

# Item_func_ne ( not equal )
CREATE TABLE t1(u TINYINT UNSIGNED);
INSERT INTO t1 VALUES (0), (3), (255), (NULL);
flush status;
PREPARE s FROM "SELECT * FROM t1 WHERE u=256 IS NOT NULL";
EXECUTE s;
EXECUTE s;
show session status like "cached_plan%";
DROP TABLE t1;

# Item_func_if. Result of the rewrite: "WHERE (i = if((i is
# null),NULL,1))". Why so complicated? because the comparison '<' is
# not "top-level" in WHERE, that is, it makes a difference if it
# returns UNKNOWN vs FALSE (see ignore_unknown() in code).

CREATE TABLE t1(i TINYINT);
INSERT INTO t1 VALUES (10),(100),(1),(0),(NULL);
ANALYZE TABLE t1;
PREPARE s FROM "SELECT * FROM t1 WHERE i = (i < 255)";
EXECUTE s;
EXECUTE s;
show session status like "cached_plan%";
DROP TABLE t1;

DROP PREPARE s;

if (!$missing_features)
{

# Test SELECT FOR UPDATE: when operating from a cached plan, does it put locks, does it respect others' locks?

CREATE USER test@localhost;
GRANT CREATE, SELECT, UPDATE on *.* to test@localhost;

CREATE USER test2@localhost;
GRANT CREATE, SELECT, UPDATE on *.* to test2@localhost;

CREATE TABLE t1 (a INT);
INSERT INTO t1 VALUES (1), (2), (3);

connect (con1, localhost, test, , test);

BEGIN;
# Put a row lock
SELECT * FROM t1 WHERE a = 2 FOR UPDATE ;

connect (con2,localhost,test2,,test);
BEGIN;

PREPARE s FROM "SELECT * FROM t1 WHERE a = 2 FOR UPDATE NOWAIT";
--error ER_LOCK_NOWAIT
EXECUTE s;
--error ER_LOCK_NOWAIT
EXECUTE s;
show session status like "cached_plan%";

connection con1;
# Release locks
COMMIT;

connection con2;
# Should work now
EXECUTE s;
# Again, and keep the lock:
BEGIN;
EXECUTE s;
show session status like "cached_plan%";

connection con1;
# Now it's its turn to wait
--error ER_LOCK_NOWAIT
SELECT * FROM t1 WHERE a = 2 FOR UPDATE NOWAIT;

connection default;
disconnect con1;
disconnect con2;
--source include/wait_until_count_sessions.inc

DROP TABLE t1;
DROP USER test@localhost;
DROP USER test2@localhost;
}

# One test of a scalar subquery

create table t1(a int, b int, c int, d int, e int, key(a,b,c,d DESC));
insert into t1 values(1,1,1,1,1),(2,2,2,2,2),(3,3,3,3,7);
insert into t1 values(1,1,1,10,50),(2,2,2,20,-30),(3,3,3,-3,8);
SELECT * FROM t1 WHERE a = 1 AND b = 1 AND c = 1 AND d = ( SELECT max(d) FROM t1 WHERE a = 1 AND b = 1 AND c = 1 );
--replace_column 10 #
explain SELECT * FROM t1 WHERE a = 1 AND b = 1 AND c = 1 AND d = ( SELECT max(d) FROM t1 WHERE a = 1 AND b = 1 AND c = 1 );
prepare s from "SELECT * FROM t1 WHERE a = ? AND b = ? AND c = ? AND d = ( SELECT max(d) FROM t1 WHERE a = ? AND b = ? AND c = ? )";
flush status;
set @a=1;
execute s using @a,@a,@a,@a,@a,@a;
set @a=2;
execute s using @a,@a,@a,@a,@a,@a;
set @a=3;
execute s using @a,@a,@a,@a,@a,@a;
set @a=1;
execute s using @a,@a,@a,@a,@a,@a;
# Both the top and sub-query are cached, so counters grow by 2 each time.
show session status like "cached_plan%";
drop prepare s;
drop table t1;

--echo #
--echo # Test of plan cache with engine condition pushdown
--echo #

create table t2 (pk int primary key , a int, b int) ;
prepare s from "EXPLAIN SELECT * FROM t2 WHERE a = ?";
set @a=1;
execute s using @a;
set @a=2;
execute s using @a;
set @a=3;
execute s using @a;
drop prepare s;
drop table t2;

--echo #
--echo # BUG2025071028786 while using windown function and order by together, after enable plan_cache, the query using cached plan will return a result set of incorrectly sorted
--echo #

CREATE TABLE t_general_test_pk (
  id int NOT NULL auto_increment primary key,
  name varchar(255) DEFAULT NULL,
  client char(4) DEFAULT NULL,
  bin_id binary(16) NOT NULL,
  t_bit16 bit(16) DEFAULT NULL,
  create_time datetime DEFAULT CURRENT_TIMESTAMP COMMENT '????',
  col1 decimal(10,2) DEFAULT NULL,
  text blob,
  tex text,
  sex enum('boy','girl','secret') DEFAULT 'secret'
) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

insert into t_general_test_pk values(1,null,null,123,0,'2025-08-04 18:42:00',0,'textsefwefwef','fwgfwergwegf','boy');
insert into t_general_test_pk values(2,null,null,123,0,'2025-08-04 18:42:00',0,null,null,'boy');
insert into t_general_test_pk values(3,'NULL','NULL',123,0,'2025-08-04 18:42:00',0,'textsefwefwef','fwgfwergwegf','boy');
insert into t_general_test_pk values(4,'NULL','NULL',123,0,'2025-08-04 18:42:00',0,'','','girl');
insert into t_general_test_pk values(5,'NULL','NULL',123,0,'2025-08-04 18:42:00',0,'textsefwefwef','fwgfwergwegf','boy');
insert into t_general_test_pk values(6,'NULL','NULL',123,0,'2025-08-04 18:42:00',0,'','','girl');
insert into t_general_test_pk values(7,'NULL','NULL',123,2,'2025-08-04 18:42:00',0,'textsefwefwef','fwgfwergwegf','boy');
insert into t_general_test_pk values(8,'NULL','NULL',123,2,'2025-08-04 18:42:00',0,'','','girl');
insert into t_general_test_pk values(9,'NULL','NULL',123,2,'2025-08-04 18:42:00',0,'','','secret');

insert into t_general_test_pk(name,client,bin_id,t_bit16,create_time,col1,text,tex,sex) select name,client,bin_id,t_bit16,create_time,col1,text,tex,sex from t_general_test_pk;
insert into t_general_test_pk(name,client,bin_id,t_bit16,create_time,col1,text,tex,sex) select name,client,bin_id,t_bit16,create_time,col1,text,tex,sex from t_general_test_pk;
insert into t_general_test_pk(name,client,bin_id,t_bit16,create_time,col1,text,tex,sex) select name,client,bin_id,t_bit16,create_time,col1,text,tex,sex from t_general_test_pk;

create index id_key on t_general_test_pk(id desc);
create index name_key on t_general_test_pk(name(20) desc, create_time);
create index decimal_key on t_general_test_pk(col1);
create index sex_key on t_general_test_pk(sex desc);

# used to lose ORDER BY

# create_time has the same value in every row.  Use id as an explicit
# tie-breaker so the window frame and the displayed order are deterministic.
set @sql_text1='select sex,create_time,id,id- LAG(id) over (order by create_time,id) as temp_diff_id from t_general_test_pk t order by sex,create_time,id;';
PREPARE stmt1 FROM @sql_text1;
EXECUTE stmt1;
EXECUTE stmt1;
EXECUTE stmt1;
show status like '%Cached_plan%';
DROP PREPARE stmt1;
 
set @sql_text1='select sex,create_time,id,avg(id) over (order by create_time,id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) as moving_avg_id from t_general_test_pk t order by sex,create_time,id;';
PREPARE stmt1 FROM @sql_text1;
EXECUTE stmt1;
EXECUTE stmt1;
EXECUTE stmt1;
show status like '%Cached_plan%';
DROP PREPARE stmt1;
DROP TABLE t_general_test_pk;

--echo #
--echo # BUG2025071124154 [plan cache][wrong result] enable plan_cache, a
--echo # query incluing a scalar subquery in select filed, and in the where
--echo # clause contains a condition judgment as length(BLOB)<?, the query
--echo # will return a wrong result set after using cached plan for several
--echo # times.
--echo #

CREATE TABLE t_general_test (
  id INT DEFAULT NULL,
  name VARCHAR(255) DEFAULT NULL,
  text BLOB,
  tex TEXT,
  sex ENUM('boy','girl','secret') DEFAULT 'secret'
) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO t_general_test VALUES
(1,'abc',   _binary 'abc',NULL,'secret'),
(2,'abc',   _binary 'abc',NULL,'secret'),
(3,'abc',   _binary 'abc',NULL,'secret'),
(100,NULL,  _binary 'aaaaaaaaaaaaaaaaaaaaaaaa',NULL,'secret'),
(101,NULL,  _binary 'aaaaaaaaaxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',NULL,'secret'),
(NULL,NULL, _binary 'textsefwefwef','fwgfwergwegf','boy'),
(NULL,NULL, NULL,NULL,'boy'),
(0,'NULL',  _binary 'textsefwefwef','fwgfwergwegf','boy'),
(0,'NULL',  '','','girl'),
(1,'abc',   _binary 'abc',NULL,'secret'),
(2,'abc',   _binary 'abc',NULL,'secret'),
(3,'abc',   _binary 'abc',NULL,'secret'),
(100,NULL , _binary 'aaaaaaaaaaaaaaaaaaaaaaaa',NULL,'secret'),
(101,NULL , _binary 'aaaaaaaaaxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',NULL,'secret'),
(NULL,NULL, _binary 'textsefwefwef','fwgfwergwegf','boy'),
(NULL,NULL, NULL,NULL,'boy'),
(0,'NULL' , _binary 'textsefwefwef','fwgfwergwegf','boy'),
(0,'NULL' , '','','girl'),
(255,'NULL',_binary 'textsefwefwef','fwgfwergwegf','boy'),
(100,'NULL','','','girl'),
(255,'NULL',_binary 'textsefwefwef','fwgfwergwegf','boy'),
(100,'NULL','','','girl');

set @sql_text='SELECT sex,
	              name,
		      LENGTH(text),
		      LENGTH(tex),
		      id,
		      (SELECT AVG(id) FROM t_general_test) AS total_avg_id,
                      id - (SELECT AVG(id) FROM t_general_test) AS id_diff_total
               FROM t_general_test
	       WHERE sex IS NOT NULL AND LENGTH(text) < ?
	       HAVING id > (SELECT AVG(id) FROM t_general_test)
	       ORDER BY sex DESC, id DESC';

set @val1=30;

SELECT @@rds_plan_cache;
PREPARE stmt1 FROM @sql_text;
EXECUTE stmt1 USING @val1;
EXECUTE stmt1 USING @val1;
EXECUTE stmt1 USING @val1;
SHOW STATUS LIKE '%Cached_plan%';

DROP PREPARE stmt1;
DROP TABLE t_general_test;

--echo # Second problem in BUG2025071124154 ('reverse' property of index access was lost)

CREATE TABLE tbl_1_all_type2index_desc (
  id_col INT NOT NULL AUTO_INCREMENT,
  bool_col TINYINT(1) DEFAULT NULL,
  bigint_col BIGINT NOT NULL,
  varchar_col VARCHAR(255)  DEFAULT NULL,
  UNIQUE KEY (id_col DESC),
  KEY ndx_bigint_varchar_col (bigint_col DESC, varchar_col(4) DESC)
) AUTO_INCREMENT=114 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO tbl_1_all_type2index_desc VALUES 
  (66 ,0, 9223372036854775807,'bbba2'),
  (103,0, 13                 ,NULL   );

PREPARE stmt FROM "WITH A AS (SELECT bool_col, bigint_col 
	     	  	      FROM tbl_1_all_type2index_desc AS A 
			      WHERE A.bool_col = ? and A.bigint_col > ?
			      ORDER BY 1,2) 
                   SELECT * FROM A";

set @col1='1';
set @col2=300;
EXECUTE stmt USING @col1,@col2;
set @col1='0';set @col2=-1000;
EXECUTE stmt USING @col1,@col2;
EXECUTE stmt USING @col1,@col2;
SHOW STATUS LIKE '%Cached_plan%';

DROP PREPARE stmt;
DROP TABLE tbl_1_all_type2index_desc;

--echo #
--echo # BUG2025072115809 [plan cache][wrong result] enable plan_cache, set 
--echo #                  differents values and execute a query which contains
--echo #                  conditions of time and timestamp type, the result 
--echo #                  returned for the second time is wrong.
--echo #

CREATE TABLE tbl_1_all_type2index_desc (
  id_col int NOT NULL AUTO_INCREMENT,
  bigint_col bigint NOT NULL,
  date_col date DEFAULT NULL,
  timestamp_col timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  time_col time(6) DEFAULT NULL,
  varchar_col varchar(255) CHARACTER SET utf8 COLLATE utf8_unicode_ci DEFAULT NULL,
  UNIQUE KEY (id_col desc),
  KEY ndx_timestamp_time_col (timestamp_col desc, time_col desc),
  KEY ndx_timestamp_varchar_col (timestamp_col desc, varchar_col(4) desc)
) AUTO_INCREMENT=114 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO tbl_1_all_type2index_desc VALUES 
(6, 1,'1998-12-03','2020-01-01 02:00:00.000000','23:23:59.000000','bbbbbbbbbb'),
(7, 1,'1998-12-03','2020-01-01 02:00:00.000000','23:23:59.000000','bbbbbbbbbb'),
(8, 1,'1998-12-03','2020-01-01 02:00:00.000000','23:23:59.000000','bbbbbbbbbb'),
(9, 1,'1998-12-03','2020-01-01 02:00:00.000000','23:23:59.000000','bbbbbbbbbb'),
(10,1,'1998-12-03','2020-01-01 02:00:00.000000','23:23:59.000000','bbbbbbbbbb'),
(11,1,'1998-11-03','2020-02-03 02:00:00.000000','10:59:59.000000','bbbbbbbbcd'),
(12,1,'1998-11-03','2020-02-03 02:00:00.000000','10:59:59.000000','bbbbbbbbcd'),
(13,1,'1998-11-03','2020-02-03 02:00:00.000000','10:59:59.000000','bbbbbbbbcd'),
(14,1,'1998-11-03','2020-02-03 02:00:00.000000','10:59:59.000000','bbbbbbbbcd'),
(15,1,'1998-11-03','2020-02-03 02:00:00.000000','10:59:59.000000','bbbbbbbbcd');

--echo #
--echo # PS query execution with caching off and on
--echo #
set rds_plan_cache=0;
PREPARE stmt FROM 
"WITH A AS (SELECT date_col, bigint_col FROM tbl_1_all_type2index_desc A
            WHERE A.time_col >= ? AND
	           A.timestamp_col BETWEEN '1995-09-02 12:20:00.000000' AND ?
		   ORDER BY 1,2)
 SELECT * FROM A";

set @col1='07:59:59.000000', @col2='1995-09-02 12:20:00.000000';
EXECUTE stmt USING @col1,@col2;
set @col1='01:59:59.000000', @col2='2020-03-02 10:00:00.000000';
EXECUTE stmt USING @col1,@col2;

set rds_plan_cache=1;
PREPARE stmt FROM 
"WITH A AS (SELECT date_col, bigint_col FROM tbl_1_all_type2index_desc A
            WHERE A.time_col >= ? AND
	           A.timestamp_col BETWEEN '1995-09-02 12:20:00.000000' AND ?
		   ORDER BY 1,2)
 SELECT * FROM A";

set @col1='07:59:59.000000', @col2='1995-09-02 12:20:00.000000';
EXECUTE stmt USING @col1,@col2;
set @col1='01:59:59.000000', @col2='2020-03-02 10:00:00.000000';
EXECUTE stmt USING @col1,@col2;
DROP PREPARE stmt;

# The plans with InnoDB are not stable (they vary randomly, sometimes table scan other times range scan), while with DStore they look stable. So, disabling the EXPLAIN if not DStore.

if ($missing_features)
{
--echo #
--echo # PS EXPLAIN with caching off and on
--echo #
set rds_plan_cache=0;

PREPARE stmt_explain FROM 
"EXPLAIN FORMAT=tree
 WITH A AS (SELECT date_col, bigint_col FROM tbl_1_all_type2index_desc A
            WHERE A.time_col >= ? AND
	           A.timestamp_col BETWEEN '1995-09-02 12:20:00.000000' AND ?
		   ORDER BY 1,2)
 SELECT * FROM A";

set @col1='07:59:59.000000', @col2='1995-09-02 12:20:00.000000';
--replace_regex $elide_costs_and_rows;
EXECUTE stmt_explain USING @col1,@col2;
set @col1='01:59:59.000000', @col2='2020-03-02 10:00:00.000000';
--replace_regex $elide_costs_and_rows;
EXECUTE stmt_explain USING @col1,@col2;

set rds_plan_cache=1;
PREPARE stmt_explain FROM 
"EXPLAIN FORMAT=tree
 WITH A AS (SELECT date_col, bigint_col FROM tbl_1_all_type2index_desc A
            WHERE A.time_col >= ? AND
	           A.timestamp_col BETWEEN '1995-09-02 12:20:00.000000' AND ?
		   ORDER BY 1,2)
 SELECT * FROM A";

set @col1='07:59:59.000000', @col2='1995-09-02 12:20:00.000000';
--replace_regex $elide_costs_and_rows;
EXECUTE stmt_explain USING @col1,@col2;
set @col1='01:59:59.000000', @col2='2020-03-02 10:00:00.000000';
--replace_regex $elide_costs_and_rows;
EXECUTE stmt_explain USING @col1,@col2;

DROP PREPARE stmt_explain;
}

--echo # Observe how the whole testcase has not increased the count of cached plan hits: it is because of the invalidation as each time the range optimizer fails to recreate an index range scan identical to the previously cached one, due to the constant hop between point-range and true-range.
SHOW STATUS LIKE '%Cached_plan%';
DROP TABLE tbl_1_all_type2index_desc;

--echo #
--echo # BUG2025071724338 while executing a prepared query including "order by int, tinytext", the second time using cached plan, mysqld will coredump.
--echo #

CREATE TABLE tbtest_part (
  id int NOT NULL,
  tinyint_field tinyint DEFAULT NULL,
  smallint_field smallint DEFAULT NULL,
  bigint_field bigint DEFAULT NULL,
  float_field float DEFAULT NULL,
  datetime_field datetime DEFAULT NULL,
  timestamp_field timestamp NULL DEFAULT NULL,
  char_field char(8) DEFAULT NULL,
  varchar_field varchar(10) DEFAULT NULL,
  text_field tinytext,
  PRIMARY KEY (id),
  UNIQUE KEY idx_uni (id,tinyint_field,smallint_field,bigint_field),
  KEY idx_f (char_field,varchar_field),
  KEY idx_n (float_field),
  KEY idx_d (datetime_field,timestamp_field)
) DEFAULT CHARSET=latin1;
 
PREPARE stmt0 FROM 'SELECT HIGH_PRIORITY STRAIGHT_JOIN SQL_SMALL_RESULT SQL_BUFFER_RESULT SQL_NO_CACHE id,tinyint_field,smallint_field,bigint_field,float_field,datetime_field,timestamp_field,char_field,varchar_field,text_field FROM tbtest_part as t1 WHERE (id=67401 or id=67501 or id=67601 or id=67701 or id=67801 or id=67901 or id=68001 or id=68101) GROUP BY id,text_field HAVING datetime_field like \'2018-08-08 00:00:00\' OR MAX(smallint_field) ORDER BY id,text_field LIMIT 2200000 OFFSET 0';

EXECUTE stmt0;
EXECUTE stmt0;
EXECUTE stmt0;
EXECUTE stmt0;
SHOW STATUS LIKE '%Cached_plan%';
DEALLOCATE PREPARE stmt0;
DROP TABLE tbtest_part;

--echo #
--echo # BUG2025071733156 while executing a prepared query including many aggregate functions in select field, the second time using cached plan, mysqld will coredump.
--echo #

flush status;
CREATE TABLE tbtest_part (
 id int NOT NULL,
 tinyint_field tinyint DEFAULT NULL,
 smallint_field smallint DEFAULT NULL,
 bigint_field bigint DEFAULT NULL,
 float_field float DEFAULT NULL,
 datetime_field datetime DEFAULT NULL,
 timestamp_field timestamp NULL DEFAULT NULL,
 char_field char(8) DEFAULT NULL,
 varchar_field varchar(10) DEFAULT NULL,
 text_field tinytext,
 PRIMARY KEY (id),
 UNIQUE KEY idx_uni (id,tinyint_field,smallint_field,bigint_field),
 KEY idx_f (char_field,varchar_field),
 KEY idx_n (float_field),
 KEY idx_d (datetime_field,timestamp_field)
);
 
PREPARE stmt0 FROM ' SELECT DISTINCT STRAIGHT_JOIN SQL_SMALL_RESULT SQL_BUFFER_RESULT AVG(smallint_field),AVG(distinct smallint_field),BIT_AND(tinyint_field),BIT_OR(tinyint_field),BIT_XOR(tinyint_field),COUNT(1),COUNT(*),COUNT(DISTINCT float_field),COUNT(DISTINCT tinyint_field,smallint_field,bigint_field),JSON_ARRAYAGG(char_field),JSON_ARRAYAGG(1+1),JSON_OBJECTAGG(char_field,varchar_field),MAX(smallint_field),MAX(distinct smallint_field),MIN(smallint_field),MIN(distinct smallint_field),STD(float_field),STDDEV(float_field),STDDEV_SAMP(float_field),SUM(smallint_field),SUM(distinct smallint_field),VAR_POP(bigint_field),VAR_SAMP(bigint_field),VARIANCE(bigint_field) FROM tbtest_part  WHERE (id=1280601 or id=1280701 or id=1280801 or id=1280901 or id=1281001 or id=1281101 or id=1281201 or id=1281301) LIMIT 2200000 OFFSET 0 ';
 
EXECUTE stmt0;
EXECUTE stmt0;
# The 3rd execution used to segfault, while reading a freed Item_sum from JOIN::sum_funcs
EXECUTE stmt0;
EXECUTE stmt0;
show status like '%Cached_plan%';
DEALLOCATE PREPARE stmt0;
DROP TABLE tbtest_part;

--echo #
--echo # BUG2025082626613 [plan_cache][asan]using the ASan pkg to start a DStore instance, find another new coredump scenerio including CTE query. (the coredump won't occur while using the release pkg)
--echo #

CREATE TABLE tbl_1_all_type2index_desc (
  bool_col tinyint(1) DEFAULT NULL,
  time_col time(6) DEFAULT NULL,
  blob_col blob,
  text_col text CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci,
  enum_col enum('aaa','bbb','ccc') CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci DEFAULT NULL
);
CREATE TABLE tbl_3_all_typeindex_desc (
  decimal_col decimal(25,5) DEFAULT NULL,
  datetime_col datetime(6) DEFAULT NULL,
  text_col text CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci

);
CREATE TABLE tbl_1_all_typeindex_desc (
  float_col float DEFAULT NULL,
  text_col text CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci
);

prepare stmt from "WITH cte1 AS ( SELECT text_col,time_col FROM tbl_1_all_type2index_desc WHERE  tbl_1_all_type2index_desc.enum_col NOT BETWEEN NULL AND ? OR tbl_1_all_type2index_desc.blob_col IN (?,NULL,'a','iiiiiiiiii1') XOR tbl_1_all_type2index_desc.bool_col <> ?), cte2 AS ( SELECT cte1.text_col, tbl_3_all_typeindex_desc.decimal_col, COUNT(tbl_3_all_typeindex_desc.datetime_col) FROM cte1 left join tbl_3_all_typeindex_desc ON cte1.text_col = tbl_3_all_typeindex_desc.text_col WHERE cte1.text_col < ANY (WITH cte3 AS ( SELECT *  FROM tbl_1_all_typeindex_desc WHERE tbl_1_all_typeindex_desc.float_col IS NOT UNKNOWN) SELECT cte3.text_col FROM cte3 )GROUP BY 1,2 ORDER BY 1,2,3)SELECT * FROM cte2 ORDER BY 1,3,2;";

flush status;
set @enum_col0='iii2';set @blob_col1='a';set @bool_col2='2020';execute stmt USING @enum_col0,@blob_col1,@bool_col2;
show status like '%Cached_plan%';
set @enum_col0='i';set @blob_col1='iii2';set @bool_col2='NULL';execute stmt USING @enum_col0,@blob_col1,@bool_col2;
show status like '%Cached_plan%';
# the crash used to happen at this execution:
set @enum_col0='NULL';set @blob_col1='a';set @bool_col2='1901';execute stmt USING @enum_col0,@blob_col1,@bool_col2;
show status like '%Cached_plan%';
set @enum_col0='iii2';set @blob_col1='a';set @bool_col2='2020';execute stmt USING @enum_col0,@blob_col1,@bool_col2;
show status like '%Cached_plan%';
set @enum_col0='i';set @blob_col1='iii2';set @bool_col2='NULL';execute stmt USING @enum_col0,@blob_col1,@bool_col2;
show status like '%Cached_plan%';
set @enum_col0='NULL';set @blob_col1='a';set @bool_col2='1901';execute stmt USING @enum_col0,@blob_col1,@bool_col2;
show status like '%Cached_plan%';
drop prepare stmt;
drop table tbl_1_all_type2index_desc,tbl_3_all_typeindex_desc,tbl_1_all_typeindex_desc;

--echo # BUG2025091110617 [plan_cache][wrong result] after enable plan_cache, a query including a where subclause like "tinyblob_col IS NOT UNKNOWN AND int_col NOT BETWEEN ? AND ?" return the result set with wrong sort

CREATE TABLE t (
  int_col INT DEFAULT NULL,
  tinyblob_col TINYBLOB,
  KEY (int_col)
) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO t VALUES (-2147483648,_binary 'eeeeeeeeee1'),(-2147483648,_binary 'eeeeeeeeee1'),(-2147483648,_binary 'eeeeeeeeee1'),(-2147483648,_binary 'eeeeeeeeee1'),(-2147483648,_binary 'eeeeeeeeee1'),(100,_binary 'eeeeeeeeee1'),(100,_binary 'eeeeeeeeee1'),(100,_binary 'eeeeeeeeee1'),(100,_binary 'eeeeeeeeee1'),(100,_binary 'eeeeeeeeee1'),(101,_binary 'eeeeeeeeee1'),(101,_binary 'eeeeeeeeee1'),(101,_binary 'eeeeeeeeee1'),(101,_binary 'eeeeeeeeee1'),(101,_binary 'eeeeeeeeee1'),(99,_binary 'eeeeeeeeee1'),(99,_binary 'eeeeeeeeee1'),(99,_binary 'eeeeeeeeee1'),(99,_binary 'eeeeeeeeee1'),(99,_binary 'eeeeeeeeee1'),(80,_binary 'eeeeeeeeee1'),(80,_binary 'eeeeeeeeee1'),(80,_binary 'eeeeeeeeee1'),(80,_binary 'eeeeeeeeee1'),(80,_binary 'eeeeeeeeee1'),(76,_binary 'eeeeeeeeee1'),(76,_binary 'eeeeeeeeee1'),(76,_binary 'eeeeeeeeee1'),(76,_binary 'eeeeeeeeee1'),(76,_binary 'eeeeeeeeee1'),(65,_binary 'eeeeeeeeee1'),(65,_binary 'eeeeeeeeee1'),(65,_binary 'eeeeeeeeee1'),(65,_binary 'eeeeeeeeee1'),(65,_binary 'eeeeeeeeee1'),(58,_binary 'eeeeeeeeee1'),(58,_binary 'eeeeeeeeee1'),(58,_binary 'eeeeeeeeee1'),(58,_binary 'eeeeeeeeee1'),(58,_binary 'eeeeeeeeee1'),(43,_binary 'eeeeeeeeee1'),(43,_binary 'eeeeeeeeee1'),(43,_binary 'eeeeeeeeee1'),(43,_binary 'eeeeeeeeee1'),(43,_binary 'eeeeeeeeee1'),(39,_binary 'eeeeeeeeee1'),(39,_binary 'eeeeeeeeee1'),(39,_binary 'eeeeeeeeee1'),(39,_binary 'eeeeeeeeee1'),(39,_binary 'eeeeeeeeee1'),(22,_binary 'eeeeeeeeee1'),(22,_binary 'eeeeeeeeee1'),(22,_binary 'eeeeeeeeee1'),(22,_binary 'eeeeeeeeee1'),(22,_binary 'eeeeeeeeee1'),(15,_binary 'eeeeeeeeee1'),(15,_binary 'eeeeeeeeee1'),(15,_binary 'eeeeeeeeee1'),(15,_binary 'eeeeeeeeee1'),(15,_binary 'eeeeeeeeee1'),(121,_binary 'eeeeeeeeee1'),(121,_binary 'eeeeeeeeee1'),(121,_binary 'eeeeeeeeee1'),(121,_binary 'eeeeeeeeee1'),(121,_binary 'eeeeeeeeee1'),(2147483647,_binary 'eee2'),(2147483647,_binary 'eee2'),(2147483647,_binary 'eee2'),(2147483647,_binary 'eee2'),(2147483647,_binary 'eee2'),(131,_binary 'e'),(131,_binary 'e'),(131,_binary 'e'),(100,_binary 'e'),(100,_binary 'e'),(100,_binary 'a'),(100,_binary 'a'),(100,_binary 'a'),(84,_binary 'a'),(84,_binary 'a'),(84,_binary 'e'),(84,_binary 'e'),(84,_binary 'e'),(67,NULL),(67,NULL),(67,NULL),(67,NULL),(67,NULL),(43,NULL),(43,NULL),(43,NULL),(43,NULL),(43,NULL),(24,NULL),(24,NULL),(24,NULL),(24,NULL),(24,NULL),(12,NULL),(12,NULL),(12,NULL),(12,NULL),(12,NULL),(11,NULL),(11,NULL),(11,NULL),(11,NULL),(11,NULL),(NULL,NULL),(NULL,NULL),(NULL,NULL),(NULL,NULL),(NULL,NULL);
ANALYZE TABLE t;
ANALYZE TABLE t UPDATE HISTOGRAM ON int_col, tinyblob_col;

SET optimizer_switch="mrr_cost_based=off";
# Verify that range access is used. Unfortunately, "Using MRR" is not shown, even though MRR is actually used (look for ET_USING_MRR in opt_explain.cc).
--replace_column 10 #
EXPLAIN SELECT int_col AS c1 FROM t C WHERE tinyblob_col IS NOT UNKNOWN AND int_col NOT BETWEEN 11 AND 43 GROUP BY 1 ORDER BY 1;

PREPARE stmt FROM 'SELECT int_col AS c1 FROM t C WHERE tinyblob_col IS NOT UNKNOWN AND int_col NOT BETWEEN 11 AND 43 GROUP BY 1 ORDER BY 1';
EXECUTE stmt;
EXECUTE stmt;
EXECUTE stmt;

SHOW STATUS LIKE '%Cached_plan%';
DROP PREPARE stmt;
DROP TABLE t;

--echo # BUG2025091102891 after enable plan_cache, In the scenario where subqueries are nested and subqueries of the year type are queried, for debug pkg: mysqld will crash; for release pkg:the result is incorrect when the plan_cache feature is used for the second time.

set sql_mode=0;

CREATE TABLE t (
  year_col year DEFAULT NULL,
  id_col int NOT NULL DEFAULT '0',
  KEY (year_col DESC)
) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

INSERT INTO t VALUES (1903,11),(1908,38),(2155,67),(NULL,85),(NULL,104),(1903,15),(1911,55),(2155,65),(2020,75),(1902,1),(1907,1),(1909,1),(2155,1),(2020,1),(NULL,1),(NULL,1),(1902,10),(1902,10),(1903,10),(1903,10),(1904,10),(1907,10),(1908,10),(1911,10),(1912,10),(2020,10),(2020,10),(NULL,10),(1902,20),(1904,20),(1911,20),(2020,20),(NULL,20),(NULL,20),(NULL,20),(NULL,20),(1903,30),(1904,30),(1906,30),(1907,30),(1908,30),(1908,30),(NULL,30),(NULL,30),(NULL,30),(NULL,30),(1905,40),(1906,40),(1912,40),(1912,40),(NULL,40),(1903,50),(1903,50),(1906,50),(1908,50),(1910,50),(NULL,50),(NULL,50),(NULL,50),(1902,60),(1903,60),(1904,60),(1906,60),(1907,60),(1912,60),(2155,60),(NULL,60),(1901,70),(1905,70),(1909,70),(1911,70),(1913,70),(2155,70),(2020,70),(NULL,70),(NULL,70),(1907,80),(1909,80),(1909,80),(1910,80),(2020,80),(NULL,80),(NULL,80),(NULL,80),(1903,90),(1908,90),(1912,90),(2020,90),(NULL,90),(1904,100),(1906,100),(1909,100),(2155,100),(2020,100),(NULL,100),(NULL,100),(NULL,100),(1905,1),(1907,1),(NULL,1),(NULL,1),(NULL,1),(1902,2),(1908,2),(1912,2),(2155,2),(2020,2),(NULL,2),(1904,3),(1904,3),(1905,3),(1908,3),(1910,3),(1913,3),(2020,3),(NULL,3),(1904,4),(1905,4),(1910,4),(2155,4),(NULL,4),(1901,5),(1906,5),(1908,5),(1910,5),(1911,5),(1907,6),(1913,6),(2020,6),(NULL,6),(NULL,6),(NULL,6),(NULL,6),(NULL,6),(1901,7),(1903,7),(1908,7),(1910,7),(1911,7),(2155,7),(2020,7),(2020,7),(1901,8),(1912,8),(1913,8),(2020,8),(NULL,8),(1902,9),(1906,9),(1908,9),(1909,9),(1912,9),(1913,9),(2155,9),(2020,9),(2020,9),(NULL,9),(1907,10),(1911,10),(2155,10),(NULL,10),(1907,11),(1909,11),(1913,11),(2020,11),(NULL,11),(NULL,11),(NULL,11),(1904,12),(1912,12),(2155,12),(NULL,12),(1902,13),(1903,13),(1904,13),(NULL,13),(NULL,13),(1904,14),(2020,14),(2020,14),(NULL,14),(1901,15),(1905,15),(1909,15),(NULL,15),(NULL,15),(1903,16),(1912,16),(2020,16),(NULL,16),(NULL,16),(1902,17),(NULL,17),(NULL,17),(1901,18),(1903,18),(1905,18),(1908,18),(1910,18),(1910,18),(1912,18),(2020,18),(NULL,18),(NULL,18),(NULL,18),(1901,19),(1903,19),(1907,19),(2155,19),(2155,19),(NULL,19),(NULL,19),(1902,20),(1904,20),(1905,20),(1908,20),(2155,20),(2020,20),(NULL,20),(NULL,20),(1902,21),(1902,21),(1903,21),(1905,21),(1908,21),(1909,21),(NULL,21),(NULL,21),(NULL,21),(NULL,21),(1901,22),(1904,22),(1906,22),(1907,22),(1910,22),(1912,22),(2020,22),(NULL,22),(1910,23),(2155,23),(NULL,23),(1903,24),(1905,24),(1907,24),(1911,24),(1912,24),(1912,24),(2020,24),(NULL,24),(NULL,24),(NULL,24),(1906,25),(1908,25),(1911,25),(2020,25),(NULL,25),(NULL,25),(1902,26),(1902,26),(1908,26),(1911,26),(2155,26),(NULL,26),(NULL,26),(NULL,26),(NULL,26),(1901,27),(1908,27),(1913,27),(1913,27),(NULL,27),(1902,28),(1905,28),(1912,28),(2020,28),(NULL,28),(NULL,28),(NULL,28),(1901,29),(1903,29),(1905,29),(1906,29),(1907,29),(1911,29),(1913,29),(1913,29),(2020,29),(1902,30),(1908,30),(1911,30),(1913,30),(2020,30),(NULL,30),(1901,31),(1905,31),(1906,31),(1909,31),(1909,31),(1912,31),(1912,31),(1912,31),(NULL,31),(NULL,31),(NULL,31),(1901,32),(1907,32),(2020,32),(2020,32),(NULL,32),(1902,33),(1903,33),(1904,33),(1904,33),(NULL,33),(NULL,33),(NULL,33),(1901,34),(1901,34),(1904,34),(1906,34),(1908,34),(1910,34),(1913,34),(2020,34),(2020,34),(2020,34),(NULL,34),(NULL,34),(NULL,34),(1902,35),(1904,35),(1908,35),(1909,35),(1912,35),(2020,35),(NULL,35),(NULL,35),(1903,36),(1908,36),(1910,36),(2155,36),(1908,37),(1909,37),(NULL,37),(1902,38),(1902,38),(1905,38),(1906,38),(1908,38),(NULL,38),(NULL,38),(1906,39),(2155,39),(2020,39),(NULL,39),(1901,40),(1903,40),(1906,40),(1913,40),(2020,40),(1901,41),(1903,41),(1905,41),(1908,41),(1908,41),(1911,41),(2155,41),(NULL,41),(NULL,41),(1902,42),(1904,42),(1905,42),(1906,42),(1907,42),(2020,42),(NULL,42),(NULL,42),(1902,43),(1911,43),(1912,43),(1913,43),(1913,43),(1902,44),(1904,44),(1907,44),(2020,44),(NULL,44),(NULL,44),(1903,45),(1904,45),(1907,45),(1910,45),(NULL,45),(NULL,45),(NULL,45),(1902,46),(1908,46),(1912,46),(2020,46),(NULL,46),(NULL,46),(NULL,46),(1903,47),(1906,47),(1911,47),(1913,47),(2020,47),(NULL,47),(2020,48),(NULL,48),(NULL,48),(1910,49),(1911,49),(1912,49),(2020,49),(NULL,49),(1904,50),(1907,50),(1909,50),(2020,50),(NULL,50),(NULL,50),(1901,51),(1906,51),(1907,51),(1911,51),(1911,51),(1911,51),(2020,51),(NULL,51),(NULL,51),(NULL,51),(NULL,51),(NULL,51),(1904,52),(1908,52),(1909,52),(2155,52),(NULL,52),(NULL,52),(1901,53),(1904,53),(1904,53),(1908,53),(1909,53),(1911,53),(2155,53),(2020,53),(2020,53),(NULL,53),(NULL,53),(1901,54),(1901,54),(1903,54),(1906,54),(1912,54),(1913,54),(NULL,54),(NULL,54),(NULL,54),(1904,55),(1906,55),(1909,55),(1909,55),(1911,55),(2020,55),(NULL,55),(NULL,55),(NULL,55),(NULL,55),(NULL,55),(1905,56),(1905,56),(1906,56),(1910,56),(1910,56),(1912,56),(2020,56),(NULL,56),(NULL,56),(NULL,56),(1902,57),(1902,57),(1905,57),(1909,57),(1910,57),(2020,57),(NULL,57),(NULL,57),(NULL,57),(NULL,57),(NULL,57),(1901,58),(1904,58),(1904,58),(1911,58),(2155,58),(2020,58),(NULL,58),(1901,59),(1907,59),(1907,59),(1907,59),(1912,59),(NULL,59),(NULL,59),(1903,60),(1904,60),(1905,60),(1908,60),(1909,60),(1909,60),(1912,60),(2155,60),(2020,60),(2020,60),(NULL,60),(1903,61),(1911,61),(2020,61),(2020,61),(2020,61),(NULL,61),(1903,62),(1903,62),(1904,62),(1910,62),(NULL,62),(NULL,62),(1904,63),(1906,63),(1909,63),(1913,63),(2020,63),(2020,63),(1902,64),(1904,64),(1906,64),(1906,64),(1907,64),(1911,64),(1912,64),(2155,64),(2020,64),(2020,64),(2020,64),(2020,64),(NULL,64),(1901,65),(1902,65),(1903,65),(1907,65),(1907,65),(1912,65),(2020,65),(NULL,65),(1903,66),(1903,66),(1905,66),(1909,66),(NULL,66),(NULL,66),(1905,67),(1908,67),(1912,67),(2155,67),(NULL,67),(1907,68),(1908,68),(1908,68),(NULL,68),(NULL,68),(1902,69),(1904,69),(1910,69),(NULL,69),(NULL,69),(1912,70),(1913,70),(2155,70),(2155,70),(NULL,70),(NULL,70),(NULL,70),(1901,71),(1909,71),(1910,71),(1911,71),(1912,71),(NULL,71),(NULL,71),(NULL,71),(NULL,71),(1902,72),(1904,72),(1906,72),(1906,72),(1907,72),(1910,72),(2020,72),(2020,72),(NULL,72),(NULL,72),(1902,73),(1905,73),(1906,73),(1907,73),(1911,73),(1912,73),(2155,73),(2020,73),(NULL,73),(1901,74),(1902,74),(1903,74),(1904,74),(1904,74),(1906,74),(1906,74),(1907,74),(1909,74),(2020,74),(1912,75),(1902,76),(1903,76),(1909,76),(1909,76),(1911,76),(2020,76),(2020,76),(NULL,76),(NULL,76),(1903,77),(1905,77),(1905,77),(1909,77),(2020,77),(2020,77),(NULL,77),(1901,78),(1903,78),(1904,78),(1906,78),(1909,78),(1913,78),(1906,79),(1908,79),(1909,79),(NULL,79),(1906,80),(1908,80),(1912,80),(1913,80),(2020,80),(NULL,80),(1907,81),(1910,81),(1913,81),(2155,81),(NULL,81),(NULL,81),(NULL,81),(NULL,81),(1901,82),(1903,82),(1910,82),(1911,82),(1913,82),(NULL,82),(NULL,82),(1901,83),(1902,83),(1906,83),(1908,83),(1912,83),(1913,83),(2155,83),(1903,84),(1912,84),(2155,84),(2020,84),(2020,84),(2020,84),(1902,85),(1907,85),(1911,85),(2155,85),(2020,85),(NULL,85),(NULL,85),(NULL,85),(1904,86),(1906,86),(1912,86),(2020,86),(2020,86),(NULL,86),(NULL,86),(NULL,86),(1901,87),(1903,87),(1904,87),(1906,87),(1908,87),(1910,87),(2020,87),(NULL,87),(NULL,87),(NULL,87),(1906,88),(1908,88),(1908,88),(1909,88),(1909,88),(2155,88),(2020,88),(NULL,88),(1902,89),(1903,89),(1904,89),(2155,89),(NULL,89),(NULL,89),(NULL,89),(1905,90),(1908,90),(1908,90),(1913,90),(2020,90),(NULL,90),(NULL,90),(NULL,90),(1904,91),(1906,91),(1911,91),(2020,91),(NULL,91),(1906,92),(1907,92),(1910,92),(2020,92),(NULL,92),(NULL,92),(NULL,92),(NULL,92),(1904,93),(1912,93),(1913,93),(NULL,93),(1913,94),(NULL,94),(NULL,94),(1901,95),(1903,95),(1904,95),(1907,95),(NULL,95),(NULL,95),(NULL,95),(1902,96),(1905,96),(1908,96),(1912,96),(1913,96),(2155,96),(NULL,96),(NULL,96),(1904,97),(2155,97),(1903,98),(1904,98),(1905,98),(1907,98),(1908,98),(1908,98),(1913,98),(1903,99),(1906,99),(1908,99),(1911,99),(NULL,99),(NULL,99),(NULL,99),(NULL,99),(1905,100),(1907,100),(1911,100),(1913,100),(2155,100),(NULL,100),(NULL,100),(NULL,100),(NULL,100),(1901,101),(1905,101),(1906,101),(1909,101),(1910,101),(1912,101),(NULL,101),(1908,102),(2155,102),(NULL,102),(1904,103),(1904,103),(1910,103),(2020,103),(1904,104),(1911,104),(NULL,104),(1906,105),(1909,105),(1911,105),(2020,105),(2020,105),(NULL,105),(1902,106),(1906,106),(1907,106),(NULL,106),(NULL,106),(NULL,106),(1903,107),(1907,107),(1909,107),(1911,107),(1912,107),(2155,107),(NULL,107),(NULL,107),(1904,108),(1906,108),(1907,108),(1912,108),(2020,108),(NULL,108),(NULL,108),(1902,109),(1903,109),(1904,109),(1905,109),(1905,109),(1907,109),(1910,109),(1911,109),(1913,109),(2155,109),(2020,109),(2020,109),(2020,109),(NULL,109),(NULL,109),(NULL,109),(NULL,109),(NULL,109),(1906,110),(1911,110),(2155,110),(2020,110),(NULL,110),(NULL,110),(NULL,110),(NULL,110),(NULL,110),(1901,111),(1902,111),(1905,111),(1906,111),(1907,111),(1907,111),(1910,111),(1911,111),(1912,111),(1913,111),(NULL,111),(1901,112),(1906,112),(1909,112),(1912,112),(1912,112),(2020,112),(NULL,112),(1906,113),(1912,113),(2155,113),(NULL,113);

# Verify that range access and MRR are used
--replace_column 10 #
EXPLAIN SELECT year_col, SUM(id_col) FROM t WHERE year_col BETWEEN 1910 AND 2020;

PREPARE stmt FROM 'SELECT year_col, SUM(id_col) FROM t WHERE year_col BETWEEN 1910 AND 2020';
EXECUTE stmt;
EXECUTE stmt;
SHOW STATUS LIKE '%Cached_plan%';
DROP PREPARE stmt;
DROP TABLE t;
set sql_mode=default;

# requires multi-valued index
if (!$missing_features)
{
# Test for a memory leak when using a dedicated MEM_ROOT (snippet from json.array_index)

create table t1 (f1 json, id varchar(255) as (f1->"$[0]") stored not null primary key );
create index i1 on t1((cast(f1->"$[*]" as unsigned array)));
insert into t1(f1) values
  (cast('[1,3]' as json)), (cast(2 as json)), (cast('[3,3,4,4,7]' as json)),
  (cast(4 as json)), (cast('[100,5,7]' as json)), (cast(6 as json)),
  (cast(7 as json)), (cast('[8,4,3,5]' as json)), (cast('[5,6,7]' as json)),
  (cast('[9,2,7]' as json));

prepare s from "select * from t1 force index(i1)
  where json_overlaps(f1->'$[*]', '[4,3,7]') order by id";

execute s;

# There is JSON data remaining in memory: in the open table and in the cached plan.

SELECT COUNT(*) FROM performance_schema.memory_summary_by_thread_by_event_name
WHERE thread_id=@thread_id AND
      event_name = 'memory/sql/JSON' AND
      (count_alloc <> count_free OR
       sum_number_of_bytes_alloc <> sum_number_of_bytes_free);

drop prepare s; # this frees the cached plan

drop table t1; # and this frees the open table

# Now there should be no more JSON in memory for this thread:
SELECT COUNT(*) FROM performance_schema.memory_summary_by_thread_by_event_name
WHERE thread_id=@thread_id AND
      event_name = 'memory/sql/JSON' AND
      (count_alloc <> count_free OR
       sum_number_of_bytes_alloc <> sum_number_of_bytes_free);
}

SET optimizer_switch=default;
set rds_plan_cache = @old_plan_cache;
set global dstore_use_histogram_auto_update=default;
