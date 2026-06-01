#
# Test DDL lock fast timeout for partition scenarios.
#
# Main flow:
#	- an operation to block DDL
#	- a DDL operation with fast timeout
#   - several DMLs
# Check points
#   - DDL rolls back
#   - DMLs success
--source include/count_sessions.inc

--echo #
--echo # common settings
--echo #
LET $lock_wait_timeout_orig=`select @@lock_wait_timeout`;
LET $rds_ddl_lock_wait_timeout_orig=`select @@rds_ddl_lock_wait_timeout`;
SET GLOBAL lock_wait_timeout=10;
# To reduce the running time, set to 1
SET GLOBAL rds_ddl_lock_wait_timeout=1;

--echo #
--echo # prepare t1 with two rows
--echo #

--disable_result_log
--disable_query_log

CREATE TABLE t1 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(50) DEFAULT '',
  purchased DATE DEFAULT '1992-1-25',
  KEY(id))
PARTITION BY RANGE( YEAR(purchased) ) (
PARTITION p0 VALUES LESS THAN (1990),
PARTITION p1 VALUES LESS THAN (1995),
PARTITION p2 VALUES LESS THAN (2000),
PARTITION p3 VALUES LESS THAN (2005));

INSERT INTO t1 VALUES (1, 'desk organiser', '2003-10-15'), (2, 'CD player', '1993-11-05');

--enable_result_log
--enable_query_log

--let $ddl_timeout_err = 1

# Specify proper lock for algorithm
--let $LOCK_IN_USE= $LOCK
if ($ALGO == 'INSTANT')
{
# only default lock is supported in instant
--let $LOCK_IN_USE= DEFAULT
}
if ($ALGO == 'COPY')
{
--let $LOCK_IN_USE= SHARED
}


if ($PARTITION_TYPE == 'ADD')
{
--let $table = t1
--let $ddl_cmd_postfix = ADD PARTITION (PARTITION p4 VALUES LESS THAN (4000))
}
if ($PARTITION_TYPE == 'DROP')
{
--let $table = t1
--let $ddl_cmd_postfix = DROP PARTITION p1
}
if ($PARTITION_TYPE == 'TRUNCATE')
{
--let $table = t1
--let $ddl_cmd_postfix = TRUNCATE PARTITION p1
}
if ($PARTITION_TYPE == 'COALESCE')
{
--disable_result_log
--disable_query_log
CREATE TABLE t2 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(50) DEFAULT '',
  purchased DATE DEFAULT '1992-1-25',
  KEY(id))
PARTITION BY HASH( YEAR(purchased) ) PARTITIONS 4;
--enable_result_log
--enable_query_log

--let $table = t2
--let $ddl_cmd_postfix = COALESCE PARTITION 2
--let $LOCK_IN_USE= 
}
if ($PARTITION_TYPE == 'REORGANIZE')
{
--let $table = t1
--let $ddl_cmd_postfix = REORGANIZE PARTITION p2,p3 INTO (PARTITION p9 VALUES LESS THAN (2005))
--let $LOCK_IN_USE= 
}
if ($PARTITION_TYPE == 'EXCHANGE')
{
--disable_result_log
--disable_query_log
CREATE TABLE t2 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(50) DEFAULT '',
  purchased DATE DEFAULT '1992-1-25',
  KEY(id));
--enable_result_log
--enable_query_log

--let $table = t1
--let $ddl_cmd_postfix = EXCHANGE PARTITION p1 WITH TABLE t2
}
if ($PARTITION_TYPE == 'REBUILD')
{
--let $table = t1
--let $ddl_cmd_postfix = REBUILD PARTITION p1
--let $LOCK_IN_USE=
}
if ($PARTITION_TYPE == 'REMOVE_PARTITION')
{
--let $table = t1
--let $ddl_cmd_postfix = REMOVE PARTITIONING
--let $ALGO=
--let $LOCK_IN_USE=
}
if ($PARTITION_TYPE == 'ANALYZE')
{
--let $table = t1
--let $ddl_cmd_postfix = ANALYZE PARTITION p1
--let $ddl_timeout_err = 0
}
if ($PARTITION_TYPE == 'CHECK')
{
--let $table = t1
--let $ddl_cmd_postfix = CHECK PARTITION p1
--let $ddl_timeout_err = 0
}
if ($PARTITION_TYPE == 'OPTIMIZE')
{
--let $table = t1
--let $ddl_cmd_postfix = OPTIMIZE PARTITION p1
--let $ddl_timeout_err = 0
}
if ($PARTITION_TYPE == 'REPAIR')
{
--let $table = t1
--let $ddl_cmd_postfix = REPAIR PARTITION p1
--let $ddl_timeout_err = 0
}


if ($ALGO != '')
{
--let $ddl_cmd_postfix= ALGORITHM=$ALGO, $ddl_cmd_postfix
}


if ($LOCK_IN_USE == '')
{
--let $ddl_cmd = alter table $table $ddl_cmd_postfix
}
if ($LOCK_IN_USE != '')
{
--let $ddl_cmd = alter table $table LOCK=$LOCK_IN_USE, $ddl_cmd_postfix
}


--let $select_all= select * from $table

let $create_template = query_get_value(SHOW CREATE TABLE $table, Create Table, 1);

--echo #
--echo # Testing
--echo #
connect(conn1,localhost,root,,);
connect(conn2,localhost,root,,);
connect(conn3,localhost,root,,);


--echo # conn1: a ddl blocking operation
connection conn1;
START TRANSACTION;
--eval $select_all


--echo # conn2: DDL
connection conn2;
--send_eval $ddl_cmd


--echo # conn3: DMLs
connection conn3;
# Wait until the above DDL is blocked
--let $wait_condition= select count(*) = 1 from information_schema.processlist where state = "Waiting for table metadata lock" and info = "$ddl_cmd";
--source include/wait_condition.inc

--eval insert into $table (name) values ('foo')
--eval $select_all
--eval update $table set purchased='1992-1-1' where name like 'foo'
--eval $select_all
--eval delete from $table where purchased='1992-1-1'
--eval $select_all
disconnect conn3;


--echo # conn2: DDL
connection conn2;
if ($ddl_timeout_err == 1)
{
--error ER_LOCK_WAIT_TIMEOUT
}
--reap
let $create = query_get_value(SHOW CREATE TABLE $table, Create Table, 1);
if ($create_template != $create)
{
  --echo # Table definition is not as expected!
}
disconnect conn2;


--echo # conn1: commit
connection conn1;
COMMIT;
--eval $select_all
disconnect conn1;

--echo #
--echo # teardown
--echo #
connection default;
drop table t1;
drop table if exists t2;

eval SET GLOBAL lock_wait_timeout = $lock_wait_timeout_orig;
eval SET GLOBAL rds_ddl_lock_wait_timeout = $rds_ddl_lock_wait_timeout_orig;
--source include/wait_until_count_sessions.inc
