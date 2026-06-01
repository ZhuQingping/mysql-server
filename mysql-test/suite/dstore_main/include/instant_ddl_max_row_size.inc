##########################################################################
# Test script to test ALTER TABLE ADD/DROP COLUMN ... ALGORITHM=INSTANT
# when INSTANT ADD causes max row size to go beyond permissible limit
#
# This test is to be run in debug mode only and with debug flag
##########################################################################

--echo # ------------------------------------------------------------
--echo # Create a table with 3 columns. [id, c1, c2]
--echo # ------------------------------------------------------------
--eval CREATE TABLE t1(id INT PRIMARY KEY, c1 VARCHAR(4000), c2 VARCHAR(4000)) ROW_FORMAT=$row_format;

--let $table_name=t1
--source suite/dstore_main/include/print_instant_metadata.inc

--echo # ------------------------------------------------------------
--echo # Add a new column. It causes max_row_size > permissible size
--echo # ------------------------------------------------------------
# Error with ALGORITHM=INSTANT
ALTER TABLE t1 ADD COLUMN c3 VARCHAR(500) NOT NULL DEFAULT 'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd', ALGORITHM=INSTANT;

# Pass with DEFAULT ALGORITHM

--let $table_name=t1
--source suite/dstore_main/include/print_instant_metadata.inc

# This INSERT succeeds
INSERT INTO t1 (id, c1, c2) VALUES(1, repeat('a', 4), repeat('b', 4));

SELECT id, LEFT(c1 , 10), LEFT(c2, 10), LEFT(c3, 10) FROM t1 ORDER BY id;

# Now this table is already in a state where a possible row could go beyond
# permissible size. So no new column could be added with ALGORITHM=INSTANT
ALTER TABLE t1 ADD COLUMN c4 INT DEFAULT -2147483648, ALGORITHM=INSTANT;
SELECT id, LEFT(c1 , 10), LEFT(c2, 10), LEFT(c3, 10), c4 FROM t1 ORDER BY id;

# Now this table is already in a state where a possible row could go beyond
# permissible size. So no column could be dropped with ALGORITHM=INSTANT
ALTER TABLE t1 DROP COLUMN c4, ALGORITHM=INSTANT;

SELECT id, LEFT(c1 , 10), LEFT(c2, 10), LEFT(c3, 10) FROM t1 ORDER BY id;

--echo ###########
--echo # CLEANUP #
--echo ###########
DROP TABLE t1;