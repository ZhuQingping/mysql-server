CREATE TABLE t1(id int, name varchar(10), primary key(id));
INSERT INTO t1 VALUES(1,'AAAA');
INSERT INTO t1 VALUES(2,'BBBB');
INSERT INTO t1 VALUES(3,'CCCC');

#------------ TEST LOCATE_FUNC ------#
SELECT LOCATE("A", name, 2) FROM t1;

#------------ TEST parallel_threads_running ---------#
set @saved_parallel_max_threads = @@parallel_max_threads;
SET GLOBAL parallel_max_threads = 5;

DELIMITER //;
CREATE PROCEDURE pro_insert(cnt int)
BEGIN
DECLARE i int DEFAULT 0;
START TRANSACTION;
WHILE i<cnt
DO
INSERT INTO t1 VALUES(i+4,'Tilay');
set i=i+1;
end WHILE;
COMMIT;
end//
DELIMITER ;//
CALL pro_insert(20000);

connect (con1, localhost, root);
connect (con2, localhost, root);

--connection con1
set session parallel_queue_timeout=1;
--send SELECT sum(id) from t1;

--connection con2
set session parallel_queue_timeout=1;
--send SELECT sum(id) from t1;

--connection con1
--reap

--connection con2
--echo ----con2 parallel query-----
--reap

--connection default
disconnect con1;
disconnect con2;

#----------- TEST pq_gather_error1 ------
SET debug="+d,pq_gather_error1";
SELECT sum(id) from t1;
SET debug="-d,pq_gather_error1";

#----------- TEST pq_gather_error2 ------
SET debug="+d,pq_gather_error2";
SELECT sum(id) from t1;
SET debug="-d,pq_gather_error2";

#----------- TEST pq_gather_error3 -------
SET debug="+d,pq_gather_error3";
SELECT sum(id) from t1;
SET debug="-d,pq_gather_error3";

#----------- TEST pq_gather_error4 -------
SET debug="+d,pq_gather_error4";
SELECT sum(id) from t1;
SET debug="-d,pq_gather_error4";

#----------- TEST simulate_item_clone_attr_copy_error -------
SET debug="+d,simulate_item_clone_attr_copy_error";
SELECT id FROM t1 ORDER BY id LIMIT 100,10;
SELECT HEX(LTRIM(name)) FROM t1 ORDER BY id LIMIT 100,10;
SELECT sum(id+id) from t1;
SET debug="-d,simulate_item_clone_attr_copy_error";

#----------- TEST simulate_item_clone_error -------
SET debug="+d,simulate_item_clone_error";
SELECT id FROM t1 ORDER BY id LIMIT 100,10;
SELECT HEX(LTRIM(name)) FROM t1 ORDER BY id LIMIT 100,10;
SELECT sum(id+id) from t1;
SET debug="-d,simulate_item_clone_error";

#----------- TEST simulate_no_item_copy_function -------
SET debug="+d,simulate_no_item_copy_function";
SELECT id from t1 ORDER BY id LIMIT 100,10;
SELECT HEX(LTRIM(name)) from t1 ORDER BY id LIMIT 100,10;
SELECT sum(id+id) from t1;
SET debug="-d,simulate_no_item_copy_function";

--disable_query_log
CALL mtr.add_suppression("\\[Warning\\] .*deep copy method is not implemented, will not use parallel query, SQL=.*");
--enable_query_log

#----------- TEST simulate_item_type_mismatch -------
SET debug="+d,simulate_item_type_mismatch";
SELECT id from t1 ORDER BY id LIMIT 100,10;
SET debug="-d,simulate_item_type_mismatch";

--disable_query_log
CALL mtr.add_suppression("\\[Warning\\] .*is not equals to this class type .*, will not use parallel query, SQL=.*");
--enable_query_log

#----------- TEST simulate_item_rebuild_attr_copy_error -------
SET debug="+d,simulate_item_rebuild_attr_copy_error";
SELECT count(id) from t1 group by name;
SET debug="-d,simulate_item_rebuild_attr_copy_error";

#----------- TEST simulate_item_rebuild_error -------
SET debug="+d,simulate_item_rebuild_error";
SELECT count(id) from t1 group by name;
SET debug="-d,simulate_item_rebuild_error";

#----------- TEST simulate_no_item_rebuild_function -------
SET debug="+d,simulate_no_item_rebuild_function";
SELECT count(id) from t1 group by name;
SET debug="-d,simulate_no_item_rebuild_function";

--disable_query_log
CALL mtr.add_suppression("\\[Warning\\] .*rebuild sum method is not implemented, will not use parallel query, SQL=.*");
--enable_query_log

#----------- clear -----------
SET GLOBAL parallel_max_threads = @saved_parallel_max_threads;
SET debug="";
DROP PROCEDURE pro_insert;
DROP TABLE t1;

CREATE TABLE t1 (i INT, d1 DECIMAL(9,2), d2 DECIMAL(9,2));
INSERT INTO t1 VALUES (1, 101.40, 21.40), (1, -80.00, 0.00),
(2, 0.00, 0.00), (2, -13.20, 0.00), (2, 59.60, 46.40),
(2, 30.40, 30.40), (3, 37.00, 7.40), (3, -29.60, 0.00),
(4, 60.00, 15.40), (4, -10.60, 0.00), (4, -34.00, 0.00),
(5, 33.00, 0.00), (5, -25.80, 0.00), (5, 0.00, 7.20),
(6, 0.00, 0.00), (6, -51.40, 0.00);

SELECT i, d1 AS a, d2 AS b FROM t1 HAVING a <> b;
SELECT i, SUM(d1) AS a, SUM(d2) AS b FROM t1 GROUP BY i HAVING a <> b;
drop table t1;

#----------- TEST Item_field is substituted with Item_string -------
create table t2(c0 smallint(5) unsigned zerofill);
create table t3(c0 smallint(5) unsigned zerofill);
insert into t2 values(0);
insert into t3 values(0);
explain SELECT t2.c0,t3.c0 FROM t3, t2 WHERE (NOT (t3.c0 XOR CAST(t2.c0 AS SIGNED) OR (t2.c0)));
SELECT t2.c0,t3.c0 FROM t3, t2 WHERE (NOT (t3.c0 XOR CAST(t2.c0 AS SIGNED) OR (t2.c0)));
drop table t2,t3;

#----------- TEST Item_default_value -------
create table t1(str varchar(10) default 'def',strnull varchar(10),intg int default '10',rel double default '3.14');
insert into t1 values ('','',0,0.0);
analyze table t1;
explain select default(str), default(strnull), default(intg), default(rel) from t1;
select default(str), default(strnull), default(intg), default(rel) from t1;
drop table t1;

## clone Item_typecast_date error
CREATE TABLE t (col_datetime datetime, col_date date, col_time time, col_char char);
insert into t values ('2013-03-15 18:35:20', '2013-03-15', '18:35:20','L'),
                     ('2003-01-10 00:00:23', '2003-01-10', '00:00:23', NULL);
SELECT col_char <> col_date FROM t;
DROP table t;


## clone Item_typecast_real error
create table t1(
 a double not null,
 b double not null
);
insert into t1 (a,b) values(1.1,1.2),(1.3,1.4),(1.5,1.6);
ANALYZE TABLE t1;
explain select *from t1 where a=CAST('1.1' AS double) AND b=CAST('1.2' AS double);
select *from t1 where a=CAST('1.1' AS double) AND b=CAST('1.2' AS double);
drop table t1;

#----------- TEST pq_dup_tabs_error0 ------
create table t(a int, b int);
insert into t values(1,10),(2,20);
set optimizer_switch="block_nested_loop=off";
SET session debug="+d,pq_dup_tabs_error0";
let query=
 select t1.*, (select count(*)
               from t t2
               left join t t3 on t3.a>t2.a-t1.a)
 from t t1;
eval explain format=tree $query;
eval $query;
SET session debug="-d,pq_dup_tabs_error0";
set optimizer_switch=default;
drop table t;