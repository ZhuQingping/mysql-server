##########################################################################
# Test script to test UPDATE on table having INSTANT ADD/DROP columns.
#
# $row_format is to be set to the row_format on which test is to be run.
##########################################################################

--eval CREATE TABLE t1 (c1 char(20), c2 char(20)) ROW_FORMAT=$row_format
INSERT INTO t1 values ("row1_c1", "row1_c2");

SHOW CREATE TABLE t1;
--let $table_name=t1
--source suite/dstore_main/include/print_instant_metadata.inc

--echo # ------------------------------------------------------------
--echo # ADD AN INSTANT COLUMN At the end [c1, c2, +c3]
--echo # ------------------------------------------------------------
ALTER TABLE t1 ADD COLUMN c3 char(20) default "c3_def", ALGORITHM=INSTANT;
--let $table_name=t1
--source suite/dstore_main/include/print_instant_metadata.inc
SELECT * FROM t1 ORDER BY c1;

INSERT INTO t1 values ("row2_c1", "row2_c2", "row2_c3");
SELECT * FROM t1 ORDER BY c1;

--echo # INPLACE UPDATE
UPDATE t1 SET c1="r1_c1" where c1="row1_c1";
SELECT * FROM t1 ORDER BY c1;

--echo # NOT INPLACE UPDATE
UPDATE t1 SET c3="row1_c3" where c1="r1_c1";
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # DROP A COLUMN at the end [c1, c2, -c3]
--echo # ------------------------------------------------------------
ALTER TABLE t1 DROP COLUMN c3, ALGORITHM=INSTANT;
--let $table_name=t1
--source suite/dstore_main/include/print_instant_metadata.inc
SELECT * FROM t1 ORDER BY c1;

INSERT INTO t1 values ("row3_c1", "row3_c2");
SELECT * FROM t1 ORDER BY c1;

--echo # INPLACE UPDATE
UPDATE t1 SET c1="r3_c1" where c1="row3_c1";
SELECT * FROM t1 ORDER BY c1;

--echo # NOT INPLACE UPDATE
UPDATE t1 SET c2="r1_c2" where c1="r1_c1";
SELECT * FROM t1 ORDER BY c1;


--echo # ------------------------------------------------------------
--echo # Rebuild table [c1, c4]
--echo # ------------------------------------------------------------
ALTER TABLE t1 FORCE;

--let $table_name=t1
--source suite/dstore_main/include/print_instant_metadata.inc
SELECT * FROM t1 ORDER BY c1;

INSERT INTO t1 values ("row4_c1", "row4_c2");
SELECT * FROM t1 ORDER BY c1;

--echo ###########
--echo # CLEANUP #
--echo ###########
DROP TABLE t1;
