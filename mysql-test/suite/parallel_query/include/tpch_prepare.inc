########################################################################
#
# SUMMARY
#
#   Create and populate table(s) using the OLAP schema
#
# USAGE
#   --source suite/parallel_query/include/tpch_prepare.inc
#
########################################################################

--disable_query_log
--disable_result_log
--echo # Step 1, prepare tpch tables
--echo # Step 1-1, create tables
CREATE TABLE nation  ( N_NATIONKEY  INTEGER NOT NULL,
                       N_NAME       CHAR(25) NOT NULL,
                       N_REGIONKEY  INTEGER NOT NULL,
                       N_COMMENT    VARCHAR(152),
                       PRIMARY KEY (N_NATIONKEY));

CREATE TABLE region  ( R_REGIONKEY  INTEGER NOT NULL,
                       R_NAME       CHAR(25) NOT NULL,
                       R_COMMENT    VARCHAR(152),
                       PRIMARY KEY (R_REGIONKEY));

CREATE TABLE part  ( P_PARTKEY     INTEGER NOT NULL,
                     P_NAME        VARCHAR(55) NOT NULL,
                     P_MFGR        CHAR(25) NOT NULL,
                     P_BRAND       CHAR(10) NOT NULL,
                     P_TYPE        VARCHAR(25) NOT NULL,
                     P_SIZE        INTEGER NOT NULL,
                     P_CONTAINER   CHAR(10) NOT NULL,
                     P_RETAILPRICE DECIMAL(15,2) NOT NULL,
                     P_COMMENT     VARCHAR(23) NOT NULL,
                     PRIMARY KEY (P_PARTKEY));

CREATE TABLE supplier ( S_SUPPKEY     INTEGER NOT NULL,
                        S_NAME        CHAR(25) NOT NULL,
                        S_ADDRESS     VARCHAR(40) NOT NULL,
                        S_NATIONKEY   INTEGER NOT NULL,
                        S_PHONE       CHAR(15) NOT NULL,
                        S_ACCTBAL     DECIMAL(15,2) NOT NULL,
                        S_COMMENT     VARCHAR(101) NOT NULL,
                        PRIMARY KEY (S_SUPPKEY));

CREATE TABLE partsupp ( PS_PARTKEY     INTEGER NOT NULL,
                        PS_SUPPKEY     INTEGER NOT NULL,
                        PS_AVAILQTY    INTEGER NOT NULL,
                        PS_SUPPLYCOST  DECIMAL(15,2)  NOT NULL,
                        PS_COMMENT     VARCHAR(199) NOT NULL,
                        PRIMARY KEY (PS_PARTKEY,PS_SUPPKEY));

CREATE TABLE customer ( C_CUSTKEY     INTEGER NOT NULL,
                        C_NAME        VARCHAR(25) NOT NULL,
                        C_ADDRESS     VARCHAR(40) NOT NULL,
                        C_NATIONKEY   INTEGER NOT NULL,
                        C_PHONE       CHAR(15) NOT NULL,
                        C_ACCTBAL     DECIMAL(15,2)   NOT NULL,
                        C_MKTSEGMENT  CHAR(10) NOT NULL,
                        C_COMMENT     VARCHAR(117) NOT NULL,
                        PRIMARY KEY (C_CUSTKEY));

CREATE TABLE orders  ( O_ORDERKEY       BIGINT NOT NULL,
                       O_CUSTKEY        INTEGER NOT NULL,
                       O_ORDERSTATUS    CHAR(1) NOT NULL,
                       O_TOTALPRICE     DECIMAL(15,2) NOT NULL,
                       O_ORDERDATE      DATE NOT NULL,
                       O_ORDERPRIORITY  CHAR(15) NOT NULL,
                       O_CLERK          CHAR(15) NOT NULL,
                       O_SHIPPRIORITY   INTEGER NOT NULL,
                       O_COMMENT        VARCHAR(79) NOT NULL,
                       PRIMARY KEY (O_ORDERKEY));

CREATE TABLE lineitem ( L_ORDERKEY    BIGINT NOT NULL,
                        L_PARTKEY     INTEGER NOT NULL,
                        L_SUPPKEY     INTEGER NOT NULL,
                        L_LINENUMBER  INTEGER NOT NULL,
                        L_QUANTITY    DECIMAL(15,2) NOT NULL,
                        L_EXTENDEDPRICE  DECIMAL(15,2) NOT NULL,
                        L_DISCOUNT    DECIMAL(15,2) NOT NULL,
                        L_TAX         DECIMAL(15,2) NOT NULL,
                        L_RETURNFLAG  CHAR(1) NOT NULL,
                        L_LINESTATUS  CHAR(1) NOT NULL,
                        L_SHIPDATE    DATE NOT NULL,
                        L_COMMITDATE  DATE NOT NULL,
                        L_RECEIPTDATE DATE NOT NULL,
                        L_SHIPINSTRUCT CHAR(25) NOT NULL,
                        L_SHIPMODE     CHAR(10) NOT NULL,
                        L_COMMENT      VARCHAR(44) NOT NULL,
                        PRIMARY KEY (L_ORDERKEY,L_LINENUMBER));

--echo # Step 1-2, load data
--exec unzip -qo $MYSQLTEST_VARDIR/std_data/tpch10mb.zip -d $MYSQL_TMP_DIR
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/customer.tbl' INTO TABLE customer FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/orders.tbl' INTO TABLE orders FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/lineitem.tbl' INTO TABLE lineitem FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/nation.tbl' INTO TABLE nation FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/partsupp.tbl' INTO TABLE partsupp FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/part.tbl' INTO TABLE part FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/region.tbl' INTO TABLE region FIELDS TERMINATED BY '|';
--eval LOAD DATA LOCAL INFILE '$MYSQL_TMP_DIR/tpch10mb/supplier.tbl' INTO TABLE supplier FIELDS TERMINATED BY '|';
--exec rm -rf $MYSQL_TMP_DIR/tpch10mb

--echo # Step 1-3, add secondary index
CREATE INDEX region_fk ON nation (N_REGIONKEY);
CREATE INDEX supplier_fk1 ON supplier(S_NATIONKEY);
CREATE INDEX customer_fk1 ON customer(C_NATIONKEY);
CREATE INDEX partsupp_fk1 ON partsupp(PS_SUPPKEY);
CREATE INDEX partsupp_fk2 ON partsupp(PS_PARTKEY);
CREATE INDEX orders_fk1 ON orders(O_CUSTKEY);
CREATE INDEX lineitem_fk1 on lineitem(L_ORDERKEY);
CREATE INDEX lineitem_fk2 on lineitem(L_PARTKEY,L_SUPPKEY);

--echo # Step 1-4, analyze tables
# Increase the variable 'innodb_stats_persistent_sample_pages' to make the execution plan more stable
SET @saved_innodb_stats_persistent_sample_pages = @@innodb_stats_persistent_sample_pages;
ANALYZE TABLE customer;
ANALYZE TABLE orders;
ANALYZE TABLE lineitem;
ANALYZE TABLE nation;
ANALYZE TABLE partsupp;
ANALYZE TABLE part;
ANALYZE TABLE region;
ANALYZE TABLE supplier;
SET GLOBAL innodb_stats_persistent_sample_pages = @saved_innodb_stats_persistent_sample_pages;
--enable_result_log
--enable_query_log

--echo # Step 1-5, check table
SHOW CREATE TABLE customer;
SHOW CREATE TABLE orders;
SHOW CREATE TABLE lineitem;
SHOW CREATE TABLE nation;
SHOW CREATE TABLE partsupp;
SHOW CREATE TABLE part;
SHOW CREATE TABLE region;
SHOW CREATE TABLE supplier;
SELECT COUNT(*) FROM customer;
SELECT COUNT(*) FROM orders;
SELECT COUNT(*) FROM lineitem;
SELECT COUNT(*) FROM nation;
SELECT COUNT(*) FROM partsupp;
SELECT COUNT(*) FROM part;
SELECT COUNT(*) FROM region;
SELECT COUNT(*) FROM supplier;
