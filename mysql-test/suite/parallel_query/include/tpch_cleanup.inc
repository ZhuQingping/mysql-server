########################################################################
#
# SUMMARY
#
#   Drop tables created by olap_prepare.inc
#
# USAGE
#
#   --source suite/pgo/include/olap_cleanup.inc
########################################################################

--disable_query_log
# Drop tpch tables
DROP TABLE customer;
DROP TABLE orders;
DROP TABLE lineitem;
DROP TABLE nation;
DROP TABLE partsupp;
DROP TABLE part;
DROP TABLE region;
DROP TABLE supplier;
# DROP DATABASE tpch;
--enable_query_log
