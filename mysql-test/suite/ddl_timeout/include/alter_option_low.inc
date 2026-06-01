#
# Test DDL lock fast timeout for the alter_option part in the ALTER TABLE command.
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
--echo # prepare t1 with one row, empty t2 and t3
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


create table t2 (
a varchar(500),
b GEOMETRY NOT NULL SRID 0,
va char(3) generated always as (substr(a, 1, 3))
) engine=innodb;


create table t3 (
a varchar(500),
b GEOMETRY NOT NULL SRID 0,
va char(3) generated always as (substr(a, 1, 3)) stored,
primary key(a)
) engine=innodb;

--enable_result_log
--enable_query_log


--echo #
--echo # determine ddl command
--echo #
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


if ($ALTER_TYPE == 'ADD_COLUMN_AFTER')
{
--let $table= t1
--let $ddl_cmd_postfix= add column col4_5 varchar(20) not null after col4
}
if ($ALTER_TYPE == 'ADD_COLUMN')
{
--let $table= t1
--let $ddl_cmd_postfix= add column col9 varchar(20) not null
}
if ($ALTER_TYPE == 'CREATE_INDEX')
{
--let $table= t1
--let $ddl_cmd_prefix= create index idx2 on $table(col2)
}
if ($ALTER_TYPE == 'ADD_UNIQUE')
{
--let $table= t1
--let $ddl_cmd_postfix= add unique index idx2(col2)
}
if ($ALTER_TYPE == 'ADD_FULLTEXT')
{
--let $table= t1
--let $ddl_cmd_postfix= add fulltext index idx2(col2)
--let $LOCK_IN_USE = SHARED
}
if ($ALTER_TYPE == 'SET_DEFAULT')
{
--let $table= t1
--let $ddl_cmd_postfix= alter column col2 set default ''
}
if ($ALTER_TYPE == 'DROP_DEFAULT')
{
--let $table= t1
--let $ddl_cmd_postfix= alter column col5  drop default
}
if ($ALTER_TYPE == 'ALTER_COLUMN')
{
--let $table= t1
--let $ddl_cmd_postfix= alter column col2 set default ''
}
if ($ALTER_TYPE == 'CHANGE_COLUMN')
{
--let $table= t1
--let $ddl_cmd_postfix= change column col2 fourth varchar(30) not null after col3
}
if ($ALTER_TYPE == 'MODIFY_COLUMN')
{
--let $table= t1
--let $ddl_cmd_postfix= modify column col6 int not null first
}
if ($ALTER_TYPE == 'SET_NULL')
{
--let $table= t1
--let $ddl_cmd_postfix= modify column col6 int null
}
if ($ALTER_TYPE == 'CHARSET')
{
--let $table= t1
--let $ddl_cmd_postfix= character set latin1
}
if ($ALTER_TYPE == 'CONVERT_TO')
{
--let $table= t1
--let $ddl_cmd_postfix= convert to character set latin1
}
if ($ALTER_TYPE == 'DROP_COLUMN')
{
--let $table= t1
--let $ddl_cmd_postfix= drop column col4
}
if ($ALTER_TYPE == 'DROP_INDEX_ALTER')
{
--let $table= t1
--let $ddl_cmd_postfix= drop index idx6
}
if ($ALTER_TYPE == 'DROP_INDEX')
{
--let $table= t1
--let $ddl_cmd_prefix= drop index idx6 on $table
}
if ($ALTER_TYPE == 'DROP_PRIMARY')
{
--let $table= t3
--let $ddl_cmd_postfix= drop primary key
}
if ($ALTER_TYPE == 'FORCE')
{
--let $table= t1
--let $ddl_cmd_postfix= force
}
if ($ALTER_TYPE == 'RENAME_COLUMN')
{
--let $table= t1
--let $ddl_cmd_postfix= rename column col3 to col33
}
if ($ALTER_TYPE == 'RENAME_INDEX')
{
--let $table= t1
--let $ddl_cmd_postfix= rename index idx6 to idx66
}
if ($ALTER_TYPE == 'RENAME_TABLE')
{
--let $table= t1
--let $ddl_cmd_postfix= rename to tt1
--let $LOCK_IN_USE=
}
if ($ALTER_TYPE == 'ALTER_INDEX')
{
--let $table= t1
--let $ddl_cmd_postfix= alter index idx6 invisible
}
if ($ALTER_TYPE == 'VIRTUAL_INDEX')
{
--let $table= t2
--let $ddl_cmd_postfix= add index idx_va(va)
}
if ($ALTER_TYPE == 'ADD_SPATIAL')
{
--let $table= t2
--let $ddl_cmd_postfix= ADD SPATIAL INDEX(b)
--let $LOCK_IN_USE=
}
if ($ALTER_TYPE == 'ADD_PRIMARY')
{
--let $table= t2
--let $ddl_cmd_postfix= add primary key(a)
}
if ($ALTER_TYPE == 'ADD_VIRTUAL_COLUMN')
{
--let $table= t2
--let $ddl_cmd_postfix= add column v1 char(3) generated always as (substr(a, 1, 3))
}
if ($ALTER_TYPE == 'DROP_VIRTUAL_COLUMN')
{
--let $table= t2
--let $ddl_cmd_postfix= drop column va
}
if ($ALTER_TYPE == 'VALIDATION')
{
--let $table= t2
--let $ddl_cmd_postfix= add column v2 char(3) generated always as (substr(a, 1, 3)) stored, with validation
--let $LOCK_IN_USE=
}
if ($ALTER_TYPE == 'WITHOUT_VALIDATION')
{
--let $table= t2
--let $ddl_cmd_postfix= add column v3 char(3) generated always as (substr(a, 1, 3)) stored, without validation
--let $LOCK_IN_USE=
}
if ($ALTER_TYPE == 'DROP_STORED_COLUMN')
{
--let $table= t3
--let $ddl_cmd_postfix= drop column va
}

if ($LOCK_IN_USE == 'NONE')
{
if ($ALTER_TYPE == 'ADD_FULLTEXT')
{
--let $LOCK_IN_USE = SHARED
}
if ($ALTER_TYPE == 'CONVERT_TO')
{
--let $LOCK_IN_USE = SHARED
}
}


# Make the ddl command
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


if ($ALTER_TYPE == 'CREATE_INDEX')
{
if ($LOCK_IN_USE != '')
{
--let $ddl_cmd_prefix= $ddl_cmd_prefix LOCK=$LOCK_IN_USE
}
if ($ALGO != '')
{
--let $ddl_cmd_prefix= $ddl_cmd_prefix ALGORITHM=$ALGO
}
--let $ddl_cmd= $ddl_cmd_prefix
}

if ($ALTER_TYPE == 'DROP_INDEX')
{
if ($LOCK_IN_USE != '')
{
--let $ddl_cmd_prefix= $ddl_cmd_prefix LOCK=$LOCK_IN_USE
}
if ($ALGO != '')
{
--let $ddl_cmd_prefix= $ddl_cmd_prefix ALGORITHM=$ALGO
}
--let $ddl_cmd= $ddl_cmd_prefix
}

--let $select_all= select * from $table
if ($table == 't2')
{
--let $select_all= select a, va from $table
}
if ($table == 't3')
{
--let $select_all= select a from $table
}

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

if ($table == 't1')
{
let $create = query_get_value(SHOW CREATE TABLE t1, Create Table, 1);
if ($create != $create_template)
{
  --echo # Table definition is not as expected! 
}
--eval insert into $table values (3,4,3,5,"ACTIVE",1,7)
--eval $select_all
--eval update $table set col5='DISABLED' where col1 = 2
--eval $select_all
--eval delete from $table where col5='DISABLED'
--eval $select_all
let $create = query_get_value(SHOW CREATE TABLE t1, Create Table, 1);
if ($create != $create_template)
{
  --echo # Table definition is not as expected!
}
}
if ($table == 't2')
{
let $create = query_get_value(SHOW CREATE TABLE t2, Create Table, 1);
if ($create != $create_template)
{
  --echo # Table definition is not as expected!
}
--eval insert into $table (a, b) values ('1', ST_GeomFromText('POINT(1 1)'))
--eval $select_all
--eval update $table set a='11' where a = 1
--eval $select_all
--eval delete from $table where va = '11'
--eval $select_all
let $create = query_get_value(SHOW CREATE TABLE t2, Create Table, 1);
if ($create != $create_template)
{
  --echo # Table definition is not as expected!
}
}
if ($table == 't3')
{
let $create = query_get_value(SHOW CREATE TABLE t3, Create Table, 1);
if ($create != $create_template)
{
  --echo # Table definition is not as expected!
}
--eval insert into $table (a, b) values ('1', ST_GeomFromText('POINT(1 1)'))
--eval $select_all
--eval update $table set a='11' where a = 1
--eval $select_all
--eval delete from $table where va = '11'
--eval $select_all
let $create = query_get_value(SHOW CREATE TABLE t3, Create Table, 1);
if ($create != $create_template)
{
  --echo # Table definition is not as expected!
}
}

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
let $create = query_get_value(SHOW CREATE TABLE $table, Create Table, 1);
if ($create_template != $create)
{
  --echo # Table definition is not as expected!
}
disconnect conn1;

--echo #
--echo # teardown
--echo #
connection default;
drop table t1;
drop table t2;
drop table t3;

eval SET GLOBAL lock_wait_timeout = $lock_wait_timeout_orig;
eval SET GLOBAL rds_ddl_lock_wait_timeout = $rds_ddl_lock_wait_timeout_orig;
--source include/wait_until_count_sessions.inc
