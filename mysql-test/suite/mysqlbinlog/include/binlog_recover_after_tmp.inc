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
USE mtest;
let $binlog_name=query_get_value(SHOW MASTER STATUS,File,1);
CREATE TABLE test1(p0 INT,p1 CHAR(10));
INSERT INTO test1 VALUES(1,'aaa');
INSERT INTO test1 VALUES(2,'bbb');
SET timestamp=@a+2;
INSERT INTO test1 VALUES(3,'ccc');

FLUSH LOGS;
let $binlog_name2=query_get_value(SHOW MASTER STATUS,File,1);
INSERT INTO test1 VALUES(4,'ddd');
FLUSH LOGS;

--sorted_result
SELECT * FROM test1;

--exec $MYSQL_BINLOG -vv --truncate --stop-datetime="2020-01-21 15:32:23"  $tmp$binlog_name -r /dev/null 
let $biglog_name_tmp=`SELECT CONCAT('$tmp','$binlog_name','.tmp')`;
let $biglog_name=`SELECT CONCAT('$tmp','$biglog_name')`;

DROP DATABASE mtest;
--exec $MYSQL_BINLOG  $biglog_name_tmp|$MYSQL
--exec $MYSQL_BINLOG  $tmp$binlog_name2|$MYSQL

USE mtest;
--sorted_result
SELECT * FROM test1;

#########################Clean up the environment#############################
DROP DATABASE mtest;
