########################################################################
#
# SUMMARY
#
#   Drop tables created by tpcc_prepare.inc
#
# USAGE
#
#   --source suite/pgo/include/tpcc_cleanup.inc
########################################################################

SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0;
SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0;

--disable_query_log
# Drop tpcc tables
DROP TABLE customer;
DROP TABLE district;
DROP TABLE history;
DROP TABLE item;
DROP TABLE new_orders;
DROP TABLE order_line;
DROP TABLE orders;
DROP TABLE stock;
DROP TABLE warehouse;
DROP DATABASE tpcc10;
--enable_query_log

SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;
SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS;
