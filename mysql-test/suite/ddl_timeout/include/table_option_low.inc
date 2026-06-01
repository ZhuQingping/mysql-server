#
# Test DDL lock fast timeout for the [table_option] part in the ALTER TABLE command.
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
--echo # prepare table t1 with one row
--echo #

--disable_result_log
--disable_query_log

create table t1 (
col1 int not null auto_increment primary key,
col2 varchar(30) not null,
col3 varchar (20) not null,
col4 varchar(4) not null,
col5 enum('PENDING', 'ACTIVE', 'DISABLED') not null default 'PENDING',
col6 int not null,
to_be_deleted int,
key idx6(col6)) engine=innodb;

insert into t1 values (2,4,3,5,"PENDING",1,7);

--enable_result_log
--enable_query_log

--echo #
--echo # determine ddl command
--echo #
--let $ddl_timeout_err = 1


# Specify proper lock for algorithm
--let $LOCK_IN_USE= $LOCK


if ($ALTER_TYPE == 'AUTO_INCREMENT')
{
--let $table= t1
--let $ddl_cmd_postfix= AUTO_INCREMENT 10
}
if ($ALTER_TYPE == 'CHARACTER_SET')
{
--let $table= t1
--let $ddl_cmd_postfix= CHARACTER SET latin1
}
if ($ALTER_TYPE == 'CHECKSUM')
{
--let $table= t1
--let $ddl_cmd_postfix= CHECKSUM 1
}
if ($ALTER_TYPE == 'COLLATE')
{
--let $table= t1
--let $ddl_cmd_postfix= COLLATE latin1_bin
}
if ($ALTER_TYPE == 'COMMENT')
{
--let $table= t1
--let $ddl_cmd_postfix= COMMENT 'string'
}
if ($ALTER_TYPE == 'COMPRESSION')
{
--let $table= t1
--let $ddl_cmd_postfix= COMPRESSION 'ZLIB'
}
if ($ALTER_TYPE == 'CONNECTION')
{
--let $table= t1
--let $ddl_cmd_postfix= CONNECTION 'connect_string'
}
if ($ALTER_TYPE == 'DATA_DIRECTORY')
{
--let $table= t1
--let $ddl_cmd_postfix= DATA DIRECTORY '/tmp'
}
if ($ALTER_TYPE == 'ENCRYPTION')
{
--let $table= t1
--let $ddl_cmd_postfix= ENCRYPTION 'Y'
}
if ($ALTER_TYPE == 'ENGINE')
{
--let $table= t1
--let $ddl_cmd_postfix= ENGINE=Innodb
}
if ($ALTER_TYPE == 'ROW_FORMAT')
{
--let $table= t1
--let $ddl_cmd_postfix= ROW_FORMAT = REDUNDANT
}
if ($ALTER_TYPE == 'STATS_AUTO_RECALC')
{
--let $table= t1
--let $ddl_cmd_postfix= STATS_AUTO_RECALC = 1
}
if ($ALTER_TYPE == 'STATS_PERSISTENT')
{
--let $table= t1
--let $ddl_cmd_postfix= STATS_PERSISTENT = 1
}
if ($ALTER_TYPE == 'STATS_SAMPLE_PAGES')
{
--let $table= t1
--let $ddl_cmd_postfix= STATS_SAMPLE_PAGES = 14
}
if ($ALTER_TYPE == 'PASSWORD')
{
--let $table= t1
--let $ddl_cmd_postfix= PASSWORD = 'string'
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

--eval insert into $table values (3,4,3,5,"ACTIVE",1,7)
--eval $select_all
--eval update $table set col5='DISABLED' where col1 = 2
--eval $select_all
--eval delete from $table where col5='DISABLED'
--eval $select_all
disconnect conn3;


--echo # conn2: DDL
connection conn2;
if ($ddl_timeout_err == 1)
{
--error ER_LOCK_WAIT_TIMEOUT
}
--reap
let $create = query_get_value(SHOW CREATE TABLE t1, Create Table, 1);
if ($create != $create_template)
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

eval SET GLOBAL lock_wait_timeout = $lock_wait_timeout_orig;
eval SET GLOBAL rds_ddl_lock_wait_timeout = $rds_ddl_lock_wait_timeout_orig;
--source include/wait_until_count_sessions.inc
