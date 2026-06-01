
--echo #
--echo # Scenario 1:
--echo # Create a normal table, rebuild and truncate will clear the instant
--echo # information
--echo #

--eval CREATE TABLE t1 (a INT NOT NULL AUTO_INCREMENT PRIMARY KEY, b INT) ROW_FORMAT=$row_format

INSERT INTO t1 VALUES(0, 1), (0, 2), (0, 3), (0, 4), (0, 5);


let $new_cols = 1;
let $instant_add_column = ALTER TABLE t1 ADD COLUMN c1 INT DEFAULT 10;

--source ../mysql-test/suite/dstore_main/include/instant_add_column_exec_and_verify.inc

# An ALTER TABLE which requires rebuild
let $new_cols = 0;
let $instant_add_column = ALTER TABLE t1 ADD COLUMN c2 INT DEFAULT 20, ADD KEY(c2);

--source ../mysql-test/suite/dstore_main/include/instant_add_column_exec_and_verify.inc

# Should be empty now
# SELECT count(*) FROM information_schema.innodb_tables WHERE instant_cols != 0;
# SELECT count(*) FROM information_schema.innodb_columns WHERE has_default = 1;


INSERT INTO t1(a, b) VALUES(0, 1), (0, 2), (0, 3), (0, 4), (0, 5);

let $new_cols = 1;
let $instant_add_column = ALTER TABLE t1 ADD COLUMN c3 INT DEFAULT 10;

--source ../mysql-test/suite/dstore_main/include/instant_add_column_exec_and_verify.inc

# TRUNCATE TABLE should rebuild the table too
let $new_cols = 0;
let $instant_add_column = TRUNCATE TABLE t1;

--source ../mysql-test/suite/dstore_main/include/instant_add_column_exec_and_verify.inc

# Should be empty now
# SELECT count(*) FROM information_schema.innodb_tables WHERE instant_cols != 0;
# SELECT count(*) FROM information_schema.innodb_columns WHERE has_default = 1;


CHECK TABLE t1;

SHOW CREATE TABLE t1;

DROP TABLE t1;