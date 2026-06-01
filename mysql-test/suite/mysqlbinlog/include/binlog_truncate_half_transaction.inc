#########################Test preparation#####################################
let $tmp=query_get_value(SHOW VARIABLES LIKE 'datadir',Value,1);
let $biglog_name_tmp=`SELECT CONCAT('$tmp','mysql-bin*','.tmp')`;
--exec rm -rf $biglog_name_tmp

SET @a=UNIX_TIMESTAMP("2020-01-21 15:32:20");
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
BEGIN;
INSERT INTO test1 VALUES(2,'bbb');
INSERT INTO test1 VALUES(3,'ccc');
SET timestamp=@a+4;

UPDATE test1 SET p1='ddd' WHERE p0=2;
INSERT INTO test1 VALUES(4,'eee');
SET timestamp=@a+2;
COMMIT;


SET timestamp=@a+6;
BEGIN;
INSERT INTO test1 VALUES(5,'fff');
UPDATE test1 SET p1='ggg' WHERE p0=1;
COMMIT;

--sorted_result
SELECT * FROM test1;

FLUSH LOGS;

# test truncate half transaction
--exec $MYSQL_BINLOG -vv --truncate --stop-datetime="2020-01-21 15:32:23" $tmp$binlog_name -r /dev/null

let biglog_name_tmp=`SELECT CONCAT('$tmp','$binlog_name','.tmp')`;

DROP DATABASE mtest;
--exec $MYSQL_BINLOG  $biglog_name_tmp|$MYSQL

USE mtest;

--sorted_result
SELECT * FROM test1;

# the truncate time-point happens to be at the end of a transaction
--exec rm -r $biglog_name_tmp

--exec $MYSQL_BINLOG -vv --truncate --stop-datetime="2020-01-21 15:32:24"  $tmp$binlog_name -r /dev/null

DROP DATABASE mtest;
--exec $MYSQL_BINLOG  $biglog_name_tmp|$MYSQL

USE mtest;

--sorted_result
SELECT * FROM test1;
#########################Clean up the environment#############################
DROP DATABASE mtest;
