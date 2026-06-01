#########################Test preparation#####################################
let $tmp=query_get_value(SHOW VARIABLES LIKE 'datadir',Value,1);
let $binlog_name_tmp=`SELECT CONCAT('$tmp','mysql-bin*','.tmp')`;
--exec rm -rf $binlog_name_tmp

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
FLUSH LOGS;
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_open_file_fail' --truncate --stop-datetime="2020-01-21 15:32:23" $tmp$binlog_name -r /dev/null
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_write_io_cache_fail' --truncate --stop-datetime="2020-01-21 15:32:23" $tmp$binlog_name -r /dev/null
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_write_io_cache_fail1' --truncate --stop-datetime="2020-01-21 15:32:23" $tmp$binlog_name -r /dev/null
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_logname_too_long' --truncate --stop-datetime="2020-01-21 15:32:23" $tmp$binlog_name -r /dev/null
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_tmp_logname_is_null' --truncate --stop-datetime="2020-01-21 15:32:23" $tmp$binlog_name -r /dev/null
##all the events are after the specified time
--error 1
--exec $MYSQL_BINLOG -vv -#d,'truncate_delete_tmp_logname_fail' --truncate --stop-datetime="2020-01-21 15:31:23" $tmp$binlog_name -r /dev/null



#########################Clean up the environment#############################
let biglog_name_tmp=`SELECT CONCAT('$tmp','$binlog_name','.tmp')`;
--exec rm -rf $biglog_name_tmp
DROP DATABASE mtest;
