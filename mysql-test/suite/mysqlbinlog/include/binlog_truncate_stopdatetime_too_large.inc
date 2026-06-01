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

SET timestamp=@a+2;

CREATE TABLE test2(p0 INT,p1 CHAR(10));
INSERT INTO test2 VALUES(2,'bbb');

--sorted_result
SELECT * FROM test1;
--sorted_result
SELECT * FROM test2;

FLUSH LOGS;

--exec $MYSQL_BINLOG -vv --truncate --stop-datetime="2021-12-12 16:42:35"   $tmp$binlog_name -r /dev/null 
let $biglog_name_tmp=`SELECT CONCAT('$tmp','$binlog_name','.tmp')`;

#the two binlog file shoule be same
--diff_files $biglog_name_tmp $tmp$binlog_name 

#########################Clean up the environment#############################
DROP DATABASE mtest;
