########################################################################
# Binlog-related backup locks tests
########################################################################

--source include/have_innodb.inc
--source include/have_rds_binlog_lock_enable.inc
--source include/have_debug_sync.inc

SELECT @@binlog_format;

RESET MASTER;

SHOW VARIABLES LIKE 'rds_binlog_lock_enable';

--echo #-----------------------------------------------------------------------
--echo # Setup
--echo #-----------------------------------------------------------------------

# Save the initial number of concurrent sessions.
--source include/count_sessions.inc

CREATE TABLE t_innodb(a INT NOT NULL, KEY (a)) ENGINE=InnoDB;

CREATE VIEW v_innodb AS SELECT * FROM t_innodb;

--echo #-----------------------------------------------------------------------
--echo # Single-threaded tests
--echo #-----------------------------------------------------------------------

#
# LBFB + DML/DDL in the same connection
#

LOCK BINLOG FOR BACKUP;

DELETE FROM t_innodb;
INSERT INTO t_innodb VALUES(0);
UPDATE t_innodb SET a = 1;
REPLACE INTO t_innodb VALUES(1);
SELECT * from t_innodb;
HANDLER t_innodb OPEN;
HANDLER t_innodb READ a FIRST;
HANDLER t_innodb CLOSE;

CREATE TABLE tt_innodb(a INT NOT NULL) ENGINE=InnoDB;

DROP TABLE tt_innodb;

--error ER_BAD_TABLE_ERROR
DROP TABLE non_existing;

TRUNCATE TABLE t_innodb;

RENAME TABLE t_innodb TO tmp, tmp TO t_innodb;

ALTER TABLE t_innodb ADD COLUMN b CHAR(10) NOT NULL;
ALTER TABLE t_innodb DROP COLUMN b;

ALTER TABLE t_innodb ADD KEY tmp (a);
ALTER TABLE t_innodb DROP KEY tmp;

CREATE DATABASE test1;
DROP DATABASE test1;

delimiter |;

CREATE PROCEDURE p1()
BEGIN
  SELECT 1;
END|

DROP PROCEDURE p1|

CREATE FUNCTION f1() RETURNS INT DETERMINISTIC
BEGIN
  RETURN 1;
END|

DROP FUNCTION f1|

delimiter ;|

CREATE VIEW v1 AS SELECT * FROM t_innodb;
DROP VIEW v1;

INSERT INTO v_innodb VALUES(1);

UNLOCK BINLOG;

#
# Test that temporary tables are not affected by backup locks
#

CREATE TEMPORARY TABLE tt_innodb(a INT NOT NULL) ENGINE=InnoDB;
INSERT INTO tt_innodb VALUES(5);

LOCK BINLOG FOR BACKUP;

START TRANSACTION;
SELECT * FROM tt_innodb;
INSERT INTO tt_innodb VALUES(6);
COMMIT;

SELECT * FROM tt_innodb;

DROP TEMPORARY TABLE tt_innodb;

UNLOCK BINLOG;

#
# test that AUTOCOMMIT updates succeed with a BINLOG lock in the same connection with binlog enabled
#

SELECT @@log_bin;

LOCK BINLOG FOR BACKUP;

INSERT INTO t_innodb VALUES(1);

UNLOCK BINLOG;

#
# Log tables
#

SET @old_general_log = @@general_log;
SET @old_slow_query_log = @@slow_query_log;
SET @old_log_output = @@log_output;
SET @old_long_query_time = @@SESSION.long_query_time;

SET SESSION long_query_time = 0;
SET GLOBAL log_output = 'TABLE';
SET GLOBAL general_log = ON;
SET GLOBAL slow_query_log = ON;

LOCK BINLOG FOR BACKUP;

SELECT 1;

UNLOCK BINLOG;

SET SESSION long_query_time = @old_long_query_time;
SET GLOBAL log_output = @old_log_output;
SET GLOBAL slow_query_log = @old_slow_query_log;
SET GLOBAL general_log = @old_general_log;

--echo #-----------------------------------------------------------------------
--echo # Multi-threaded tests
--echo #-----------------------------------------------------------------------

--connect(con1,localhost,root,,)

#
# Test that updates temporary tables
# would write into the binary log are blocked by the binlog lock.
#
--connection con1
--echo # connection con1

CREATE TEMPORARY TABLE tt_innodb(a INT NOT NULL) ENGINE=InnoDB;
INSERT INTO tt_innodb VALUES(5);

--connection default
--echo # connection default
LOCK BINLOG FOR BACKUP;

--let $orig_binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)

--connection con1
--echo # connection con1

SET SESSION lock_wait_timeout = 1;

START TRANSACTION;

# Updates to temporary tables are not binlogged in the ROW mode
INSERT INTO tt_innodb VALUES(6);

SELECT * FROM tt_innodb;

if (`SELECT @@binlog_format = 'STATEMENT'`)
{
   --error ER_LOCK_WAIT_TIMEOUT
   COMMIT;
if (!`SELECT @@binlog_format = 'STATEMENT'`)
{
   COMMIT;
}
}

--connection default
--echo # connection default

--let $binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)
--disable_query_log
--eval SELECT $orig_binlog_pos = $binlog_pos AS must_be_1
--enable_query_log

UNLOCK BINLOG;

--connection con1
--echo # connection con1

COMMIT;

DROP TEMPORARY TABLE tt_innodb;

--connection default
--echo # connection default

LOCK BINLOG FOR BACKUP;

--let $orig_binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)

--connection con1
--echo # connection con1

#
# test that AUTOCOMMIT updates are blocked with a BINLOG lock in another
# connection with binlog enabled
#

SET SESSION lock_wait_timeout = 1;

--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t_innodb VALUES(1);

SET SESSION lock_wait_timeout = DEFAULT;

--connection default
--echo # connection default

--let $binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)
--disable_query_log
--eval SELECT $orig_binlog_pos = $binlog_pos AS must_be_1
--enable_query_log

UNLOCK BINLOG;

#
# LTFB + DDL / DML in another connection
#

--connection default
--echo # connection default

LOCK BINLOG FOR BACKUP;

--let $orig_binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)

--connection con1
--echo # connection con1

SET SESSION lock_wait_timeout = 1;

--error ER_LOCK_WAIT_TIMEOUT
DELETE FROM t_innodb;
--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO t_innodb VALUES(0);
--error ER_LOCK_WAIT_TIMEOUT
UPDATE t_innodb SET a = 2;
--error ER_LOCK_WAIT_TIMEOUT
REPLACE INTO t_innodb VALUES(2);
SELECT * from t_innodb;
HANDLER t_innodb OPEN;
HANDLER t_innodb READ a FIRST;
HANDLER t_innodb CLOSE;

START TRANSACTION;
SELECT * from t_innodb;
DELETE FROM t_innodb;
INSERT INTO t_innodb VALUES(0);
UPDATE t_innodb SET a = 1;
REPLACE INTO t_innodb VALUES(1);
--error ER_LOCK_WAIT_TIMEOUT
COMMIT;

SELECT * FROM t_innodb;
COMMIT;

--error ER_LOCK_WAIT_TIMEOUT
CREATE TABLE tt_innodb(a INT NOT NULL) ENGINE=InnoDB;

--error ER_BAD_TABLE_ERROR
DROP TABLE non_existing;

--error ER_TABLE_EXISTS_ERROR
CREATE TABLE t_innodb(a INT NOT NULL) ENGINE=InnoDB;

--error ER_LOCK_WAIT_TIMEOUT
TRUNCATE TABLE t_innodb;

--error ER_LOCK_WAIT_TIMEOUT
RENAME TABLE t_innodb TO tmp, tmp to t_innodb;

--error ER_LOCK_WAIT_TIMEOUT
ALTER TABLE t_innodb ADD COLUMN b CHAR(10);

--error ER_LOCK_WAIT_TIMEOUT
ALTER TABLE t_innodb ADD KEY (a);

--error ER_LOCK_WAIT_TIMEOUT
CREATE DATABASE test1;

--error ER_LOCK_WAIT_TIMEOUT
CREATE PROCEDURE p1() SELECT 1;

--error ER_LOCK_WAIT_TIMEOUT
CREATE VIEW v1 AS SELECT * FROM t_innodb;

--error ER_LOCK_WAIT_TIMEOUT
INSERT INTO v_innodb VALUES(1);

--connection default
--echo # connection default

--let $binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)
--disable_query_log
--eval SELECT $orig_binlog_pos = $binlog_pos AS must_be_1
--enable_query_log

UNLOCK BINLOG;

--connection con1
--echo # connection con1

# DDLs are not transactional, so previous DDL statements against t_innodb will
# be executed despite the error
SHOW CREATE TABLE t_innodb;

DROP TABLE t_innodb;
CREATE TABLE t_innodb(a INT NOT NULL) ENGINE=InnoDB;

--connection default
--echo # connection default

#
# Log tables
#

SET @old_general_log = @@general_log;
SET @old_slow_query_log = @@slow_query_log;
SET @old_log_output = @@log_output;

SET GLOBAL log_output = 'TABLE';
SET GLOBAL general_log = ON;
SET GLOBAL slow_query_log = ON;

LOCK BINLOG FOR BACKUP;

--let $orig_binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)

--connection con1
--echo # connection con1

SET @old_long_query_time = @@SESSION.long_query_time;
SET SESSION long_query_time = 0;

SELECT 1;

--connection default
--echo # connection default

SELECT 1;

--connection con1
--echo # connection con1

SET SESSION long_query_time = @old_long_query_time;

--connection default
--echo # connection default

--let $binlog_pos= query_get_value(SHOW MASTER STATUS, Position, 1)
--disable_query_log
--eval SELECT $orig_binlog_pos = $binlog_pos AS must_be_1
--enable_query_log

UNLOCK BINLOG;

SET GLOBAL log_output = @old_log_output;
SET GLOBAL slow_query_log = @old_slow_query_log;
SET GLOBAL general_log = @old_general_log;
truncate table mysql.general_log;
truncate table mysql.slow_log;

#
# test information_schema, performance_schema and processlist
#
--echo # test information_schema, performance_schema and processlist

--connect(con2,localhost,root,,)

--connection con1
--echo # connection con1
set session lock_wait_timeout = DEFAULT;
SET DEBUG_SYNC= "binlog_protection_acquired WAIT_FOR released";
--send INSERT INTO t_innodb VALUES(5)

--connection default
--echo # connection default
--sleep 3
--send LOCK BINLOG FOR BACKUP

--connection con2
--echo # connection con2
--sleep 3
#select LOCK_STATUS,LOCK_MODE,LOCK_DURATION from information_schema.metadata_lock_info where LOCK_TYPE = 'Binlog lock' order by LOCK_MODE;
select LOCK_TYPE,LOCK_DURATION,LOCK_STATUS from performance_schema.metadata_locks where OBJECT_TYPE = 'BINLOG' order by LOCK_TYPE;
select OBJECT_TYPE, OBJECT_SCHEMA,OBJECT_NAME, INDEX_NAME from performance_schema.events_waits_current where OBJECT_TYPE='BINLOG';
SET DEBUG_SYNC= "now SIGNAL released";

--connection con1
--echo # connection con1
--reap

--connection default
--echo # connection default
--reap

--connection con1
--echo # connection con1
--send INSERT INTO t_innodb VALUES(6)

--connection con2
--echo # connection con2
--sleep 3
#select LOCK_STATUS,LOCK_MODE,LOCK_DURATION from information_schema.metadata_lock_info where LOCK_TYPE = 'Binlog lock' order by LOCK_MODE;
select LOCK_TYPE,LOCK_DURATION,LOCK_STATUS from performance_schema.metadata_locks where OBJECT_TYPE = 'BINLOG' order by LOCK_TYPE;
select OBJECT_TYPE, OBJECT_SCHEMA,OBJECT_NAME, INDEX_NAME from performance_schema.events_waits_current where OBJECT_TYPE='BINLOG';
--connection default
--echo # connection default
UNLOCK BINLOG;

--connection con1
--echo # connection con1
--reap
SELECT * FROM t_innodb;

--echo #-----------------------------------------------------------------------
--echo # Cleanup
--echo #-----------------------------------------------------------------------

--disconnect con1
--disconnect con2

--connection default
DROP VIEW v_innodb;
DROP TABLE t_innodb;

--source include/wait_until_count_sessions.inc
