#########################Test preparation#####################################
let $tmp=query_get_value(SHOW VARIABLES LIKE 'datadir',Value,1);
let $biglog_name_tmp=`SELECT CONCAT('$tmp','mysql-bin*','.tmp')`;
--exec rm -rf $biglog_name_tmp

SET @a=UNIX_TIMESTAMP("2020-01-21 15:32:22");
SET timestamp=@a;
RESET MASTER;

DROP DATABASE IF EXISTS mtest;
CREATE DATABASE mtest;

#########################Test execution#######################################
set  timestamp=@a+4;
FLUSH LOGS;

let $binlog_name=query_get_value(SHOW MASTER STATUS,File,1);

set timestamp=@a+2;
USE mtest;
CREATE TABLE test1(p0 INT,p1 CHAR(10));
INSERT INTO test1 VALUES(1,'aaa');
INSERT INTO test1 VALUES(2,'bbb');

SET timestamp=@a+4;

CREATE TABLE test2(p0 INT,p1 CHAR(10));
INSERT INTO test2 VALUES(1,'aaa');
INSERT INTO test2 VALUES(2,'bbb');

--sorted_result
SELECT * FROM test1;
--sorted_result
SELECT * FROM test2;

FLUSH LOGS;

--exec $MYSQL_BINLOG -vv --truncate --stop-datetime="2020-01-21 15:32:25"  $tmp$binlog_name -r /dev/null

let $biglog_name_tmp=`SELECT CONCAT('$tmp','$binlog_name','.tmp')`;

DROP TABLE test1;
DROP TABLE test2;
--exec $MYSQL_BINLOG  $biglog_name_tmp|$MYSQL

USE mtest;
--sorted_result
SELECT * FROM test1;
--error ER_NO_SUCH_TABLE
SELECT * FROM test2;

#########################Clean up the environment#############################
DROP DATABASE mtest;


