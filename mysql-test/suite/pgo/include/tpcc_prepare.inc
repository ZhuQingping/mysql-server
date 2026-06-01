########################################################################
#
# SUMMARY
#
#   Create and populate table(s) using the TPC-C schema
#
# USAGE
#   --source suite/pgo/include/tpcc_prepare.inc
#
########################################################################

--disable_query_log
--disable_result_log
--disable_abort_on_error

--exec unzip -qo $MYSQLTEST_VARDIR/std_data/tpcc10.zip -d $MYSQL_TMP_DIR

SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0;
SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0;

DROP DATABASE IF EXISTS tpcc10;

CREATE DATABASE tpcc10;
USE tpcc10;
--echo # load tpcc data
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/customer.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/district.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/history.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/item.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/new_orders.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/order_line.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/orders.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/stock.sql
--exec $MYSQL --show-warnings --user=root tpcc10 < $MYSQL_TMP_DIR/tpcc10/warehouse.sql

--exec rm -rf $MYSQL_TMP_DIR/tpcc10

SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;
SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS;

--echo # analyze tables
ANALYZE TABLE customer;
ANALYZE TABLE district;
ANALYZE TABLE history;
ANALYZE TABLE item;
ANALYZE TABLE new_orders;
ANALYZE TABLE order_line;
ANALYZE TABLE orders;
ANALYZE TABLE stock;
ANALYZE TABLE warehouse;

--enable_result_log
--enable_query_log
--enable_abort_on_error

--echo # check table
SHOW CREATE TABLE customer;
SHOW CREATE TABLE district;
SHOW CREATE TABLE history;
SHOW CREATE TABLE item;
SHOW CREATE TABLE new_orders;
SHOW CREATE TABLE order_line;
SHOW CREATE TABLE orders;
SHOW CREATE TABLE stock;
SHOW CREATE TABLE warehouse;

SELECT COUNT(*) FROM customer;
SELECT COUNT(*) FROM district;
SELECT COUNT(*) FROM history;
SELECT COUNT(*) FROM item;
SELECT COUNT(*) FROM new_orders;
SELECT COUNT(*) FROM order_line;
SELECT COUNT(*) FROM orders;
SELECT COUNT(*) FROM stock;
SELECT COUNT(*) FROM warehouse;
