#########################Test preparation#####################################
let $tmp=query_get_value(SHOW VARIABLES LIKE 'datadir',Value,1);

SET @a=UNIX_TIMESTAMP("2020-01-21 15:32:22");
SET timestamp=@a;
RESET MASTER;

DROP DATABASE IF EXISTS mtest;
CREATE DATABASE mtest;
SET timestamp=@a+1;

#########################Test execution#######################################
USE mtest;
CREATE TABLE test2(p0 INT,p1 CHAR(10));
let $binlog_name=query_get_value(SHOW MASTER STATUS,File,1);
SET timestamp=@a+2;
CREATE TABLE test1(p0 INT,p1 CHAR(10));
INSERT INTO test1 VALUES(1,'aaa');
INSERT INTO test1 VALUES(2,'bbb');

SET timestamp=@a+4;

INSERT INTO test1 VALUES(3,'ccc');
INSERT INTO test1 VALUES(4,'ddd');

--sorted_result
SELECT * FROM test1;

FLUSH LOGS;

DROP DATABASE mtest;
CREATE DATABASE mtest;
# use --start-datetime and  --stop-datetime to recover
--exec $MYSQL_BINLOG --start-datetime="2020-01-21 15:32:24" --stop-datetime="2020-01-21 15:32:25" --database=mtest $tmp$binlog_name|$MYSQL

USE mtest;
--sorted_result
SELECT * FROM test1;
--error ER_NO_SUCH_TABLE
SELECT * FROM test2;

let $pos_722=query_get_value(SHOW BINLOG EVENTS IN '$binlog_name',Pos,10);
let $end_pos_1101=query_get_value(SHOW BINLOG EVENTS IN '$binlog_name',End_log_pos,15);
DROP DATABASE mtest;
CREATE DATABASE mtest;
# use --start-position and  --stop-position to recover
--exec $MYSQL_BINLOG --start-position=$pos_722  --stop-position=$end_pos_1101 --database=mtest $tmp$binlog_name|$MYSQL


USE mtest;
--sorted_result
SELECT * FROM test1;
--error ER_NO_SUCH_TABLE
SELECT * FROM test2;

#########################Clean up the environment#############################
DROP DATABASE mtest;
