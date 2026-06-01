--let $PARTITION_TYPE = ADD
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = DROP
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = TRUNCATE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = COALESCE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = REORGANIZE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = EXCHANGE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = ANALYZE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = CHECK
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = OPTIMIZE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = REBUILD
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = REPAIR
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_table_partition_low.inc


--let $PARTITION_TYPE = REMOVE_PARTITION
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_table_partition_low.inc
