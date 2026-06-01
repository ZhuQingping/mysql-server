--echo # aggregate with single Query

create table t1(a int primary key, b varchar(10), c decimal(5,2));
create table t2(a int primary key, b varchar(10), c decimal(5,2));
insert into t1 values (1, 'The', '3.14');
insert into t1 values (2, 'quick', '4.25');
insert into t1 values (3, 'brown', '5.36');
insert into t1 values (4, 'fox', '6.47');
insert into t1 values (5, 'jumps', '3.14');
insert into t1 values (6, 'over', '4.25');
insert into t1 values (7, 'the', '5.36');
insert into t1 values (8, 'lazy', '6.47');
insert into t1 values (9, 'dog', '7.58');
insert into t2 select * from t1 where a > 3;
analyze table t1;
analyze table t2;

explain select count(*) from t1;
explain select count(*), sum(c) from t1;
explain select count(*) from t1 where c > 5.0;
explain select count(*) as cnt from t1 order by cnt;
explain select count(*) as g from t1 order by a limit 1;
explain select count(*) from t1 group by(b);
explain select a, count(*) from t1 group by a order by b;
explain select count(*) from t1 join t2 on t1.a = t2.a;
explain select distinct count(*) from t1;
# having condition
explain select a from t1 having a > 3 order by a limit 10;
explain select sum(a+2) as SUM from t1 having SUM > 3;
explain select count(a) from t1 having sum(a) > 3 order by a limit 10;
explain select sum(distinct t1.a) from t1 where t1.a > 3;
drop table t1,t2;

--echo # subquery
## scalar uncorrelated subquery
create table t1 (id int, a int);
create table t2 (b int);
insert into t1 values (1, 1), (2, 1), (0, 2), (1, 3), (3, 3), (0, 2);
insert into t2 values (1), (3), (2), (0), (1), (2), (1);
analyze table t1, t2;

# scalar subquery in field list
explain select id, (select avg(b) from t2) as t from t1;

# scalar subquery in group-by
explain select sum(t1.a) from t1, t2 group by (select avg(t1.id) from t1) + t1.a;

# scalar subquery in order-by
explain select t1.a from t1 order by (select avg(t1.id) from t1) + t1.a;

# scalar subquery in where condition
explain select * from t1 where a > (select avg(b) from t2);

# scalar subquery in aggregation
explain select id, sum((select avg(b) from t2)) as t from t1 group by id order by id;

# scalar subquery in having
explain SELECT SUM(a), (SELECT SUM(b) FROM t2) AS scalar FROM t1 HAVING SUM(a) > scalar;

## scalar-aggregation
explain SELECT * from t1 JOIN (SELECT SUM(a) as c1, (SELECT sum(b) FROM t2) AS c2 FROM t1  HAVING SUM(a) > c2) as dt;
explain SELECT * from t1 JOIN (SELECT SUM(a) as c1, (SELECT sum(b) FROM t2) AS c2 FROM t1 group by t1.a HAVING SUM(a) > c2) as dt;

## scalar subquery runs in PQ while the outer query does not
explain select sum(distinct t1.id) from t1 where t1.id > (select t1.a from t1 limit 1);

drop tables t1, t2;

# exists
create table t1(a int not null, b char(10) not null);
create table t2(a int not null, b char(10) not null);
insert into t1 values (1, 'a'), (2, 'b'), (3, 'c'), (3, 'c'), (4, 'd'), (5, 'f'), (6, 'e'),(8,'h'),(0,'j');
insert into t2 values (3, 'c'), (4, 'd'), (5, 'f'), (6, 'e'), (1, 'a'), (2, 'b'), (7, 'g'),(0,'i');
analyze table t1,t2;

explain select * from t1 where exists(select * from t2 where t2.b=t1.b);
explain select * from t1 where exists(select count(*) from t2);
explain select count(*) from t1 where exists(select count(*) from t2);
drop table t1,t2;

--echo # view

create table small (id INTEGER not null, PRIMARY KEY (id));
create table big (id INTEGER not null, PRIMARY KEY (id));
insert into small VALUES (1), (2);
insert into big VALUES (1), (2), (3), (4);
create view small_view AS SELECT *, IF (id % 2 = 1, 1, 0) AS is_odd FROM small;
create view big_view AS SELECT big.*, small_view.id AS small_id, small_view.is_odd FROM big LEFT JOIN small_view ON small_view.id = big.id;

# to preserve plans:
--source include/turn_off_join_elimination.inc
explain select count(*) from small_view;
explain select count(*) from big_view;
--source include/restore_opt_switch_after_turn_off_join_elimination.inc

drop table small, big;
drop view small_view, big_view;

--echo # insert select
set pq_support_features_switch='insert_select=on';
create table t0 (a int);
create table t1 (a int);
insert into t0 values (1),(2),(3);
insert into t1 values (1),(2),(3);
explain insert into t1 select * from t0;
explain insert into t1 select count(*) from t0;
drop table t0,t1;

--echo # PQ_HINT
create table t1(a int primary key, b varchar(10), c decimal(5,2));
explain select/*+ PQ(2) */ sum(a) from t1;
explain select/*+ PQ(2) */ a from t1;
explain select /*+ PQ(t1 2)*/ sum(a) from t1;
explain select /*+ PQ(t1 2)*/ a from t1;
drop table t1;
