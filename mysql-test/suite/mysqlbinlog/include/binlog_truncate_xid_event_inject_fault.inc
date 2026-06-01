#########################Test preparation#####################################
let $tmp=query_get_value(SHOW VARIABLES LIKE 'datadir',Value,1);
let binlog_name_tmp=`SELECT CONCAT('$tmp','$binlog_name','.tmp')`;
--exec rm -rf $binlog_name_tmp

SET @a=UNIX_TIMESTAMP("2020-01-21 15:32:20");
SET timestamp=@a;
RESET MASTER;

DROP DATABASE IF EXISTS mtest;
CREATE DATABASE mtest;

#########################Test execution#######################################
let $binlog_name=query_get_value(SHOW MASTER STATUS,File,1);
USE mtest;
CREATE TABLE test2(p0 INT,p1 CHAR(10));
INSERT INTO test2 VALUES(1,'aaa');
BEGIN;
INSERT INTO test2 VALUES(2,'bbb');
SET timestamp=@a+2;
INSERT INTO test2 VALUES(3,'ccc');
SET timestamp=@a+4;

UPDATE test2 SET p1='ddd' WHERE p0=2;
INSERT INTO test2 VALUES(4,'eee');
SET timestamp=@a+2;
COMMIT;

SET timestamp=@a+6;
BEGIN;
INSERT INTO test2 VALUES(5,'fff');
UPDATE test2 SET p1='ggg' WHERE p0=1;
COMMIT;
FLUSH LOGS;
--exec $MYSQL_BINLOG -vv -#d,'truncate_enter_xid_event' --truncate --stop-datetime="2020-01-21 15:32:22" $tmp$binlog_name -r /dev/null
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_write_io_cache_fail1' --truncate --stop-datetime="2020-01-21 15:32:22" $tmp$binlog_name -r /dev/null

#########################Clean up the environment#############################
--exec rm -rf $binlog_name_tmp
DROP DATABASE mtest;
