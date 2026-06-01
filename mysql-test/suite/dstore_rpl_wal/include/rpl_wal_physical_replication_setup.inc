--echo ############################################################
--echo Establish a master-replica replication relationship based on physical replication.
--echo ############################################################

--disable_warnings
--source include/master-slave.inc
--enable_warnings

--echo #
--echo # 1. Initialize the required variables, then disconnect the subsequent master-replica replication connections.
--echo #
--let $UNIQUE = `SELECT REPLACE(UUID(), '-', '')`
--let $MY_MASTER_DATADIR=$MYSQL_TMP_DIR/data_master_$UNIQUE
--let $MY_SLAVE_DATADIR=$MYSQL_TMP_DIR/data_slave_$UNIQUE

--source include/rpl_connection_master.inc
--let $node1 = $CURRENT_CONNECTION
--source include/rpl_connection_slave.inc
--let $node2 = $CURRENT_CONNECTION

--connection $node1
--let $datadir1 = `SELECT @@global.datadir`
--let $log_error1 = `SELECT @@global.log_error`

--connection $node2
--let $datadir2 = `SELECT @@global.datadir`
--let $log_error2 = `SELECT @@global.log_error`

# Since include/master-slave.inc has already set up binlog-based master-replica replication for us, stop the replication first.
--connection $node2
STOP REPLICA;
RESET REPLICA ALL;

--echo #
--echo # 2. Since physical replication does not support DDL operations at present, execute all required DDL statements first.
--echo #
--connection $node1
create database copy_database;
use copy_database;
CREATE TABLE copy_table (
  id INT PRIMARY KEY,
  data VARCHAR(100)
) engine = dstore;
--sleep 2

--echo #
--echo # 3. Shutdown the primary and standby instances.
--echo #
--let $rpl_server_number= 1
--source include/rpl_stop_server.inc
--let $rpl_server_number= 2
--source include/rpl_stop_server.inc

--echo #
--echo # 4. Copy the data directory of the primary to that of the standby.
--echo #
--exec mkdir -p $MY_MASTER_DATADIR
--exec mkdir -p $MY_SLAVE_DATADIR
--exec cp -r $datadir1/* $MY_MASTER_DATADIR/
--exec cp -r $datadir1/* $MY_SLAVE_DATADIR/

--echo #
--echo # 5. Start the primary and standby instances with new parameters for establishing a master-replica physical replication relationship.
--echo #
# Delete the auto.cnf file; the instance will automatically generate a new one when started, ensuring that the UUIDs of the primary and standby servers are different.
--exec rm -rf $MY_MASTER_DATADIR/auto.cnf
# The log-error path needs to be set to the original value; otherwise, the replica will fail to suppress exceptions.
--let $rpl_server_number = 1
--let $rpl_omit_print_server_parameters=1
--let $rpl_server_parameters = --datadir=$MY_MASTER_DATADIR --log-error=$log_error1 --loose-storage_engine_mode=ONLY_DSTORE --default_storage_engine=Dstore --default_tmp_storage_engine=Dstore --disabled-storage-engines=InnoDB,MyISAM,MEMORY,MRG_MYISAM,CSV --transaction-isolation=READ-COMMITTED
--source include/rpl_start_server.inc
--let $rpl_server_number = 2
--let $rpl_omit_print_server_parameters=1
--let $rpl_server_parameters = --datadir=$MY_SLAVE_DATADIR --log-error=$log_error2 --loose-storage_engine_mode=ONLY_DSTORE --default_storage_engine=Dstore --default_tmp_storage_engine=Dstore --disabled-storage-engines=InnoDB,MyISAM,MEMORY,MRG_MYISAM,CSV --transaction-isolation=READ-COMMITTED
--source include/rpl_start_server.inc

--echo #
--echo # 6. Confirm that the primary and standby servers start with the new data directory.
--echo #
--connection $node1
--let $assert_text=Database copy_database should exist (only one record)
--let $assert_sql=SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA WHERE SCHEMA_NAME = 'copy_database'
--let $assert_cond=COUNT(*) = 1
--source include/assert.inc

--let $assert_text=Table copy_table should exist in database copy_database (only one record)
--let $assert_sql=SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'copy_database' AND TABLE_NAME = 'copy_table'
--let $assert_cond=COUNT(*) = 1
--source include/assert.inc
use copy_database;
show tables;

--connection $node2
--let $assert_text=Database copy_database should exist (only one record)
--let $assert_sql=SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA WHERE SCHEMA_NAME = 'copy_database'
--let $assert_cond=COUNT(*) = 1
--source include/assert.inc

--let $assert_text=Table copy_table should exist in database copy_database (only one record)
--let $assert_sql=SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'copy_database' AND TABLE_NAME = 'copy_table'
--let $assert_cond=COUNT(*) = 1
--source include/assert.inc
use copy_database;
show tables;

--echo #
--echo # 7. Install the plugin on both the primary and standby servers, and establish a master-replica physical replication relationship.
--echo #
--replace_result $SEMISYNC_SOURCE_PLUGIN SEMISYNC_SOURCE_PLUGIN
--replace_result $SEMISYNC_REPLICA_PLUGIN SEMISYNC_REPLICA_PLUGIN

--echo #
--echo # Install the primary server's plugin and set enabled_wal to ON.
--echo #
--connection $node1
--eval INSTALL PLUGIN rpl_semi_sync_source SONAME '$SEMISYNC_SOURCE_PLUGIN'
--eval INSTALL PLUGIN rpl_semi_sync_replica SONAME '$SEMISYNC_REPLICA_PLUGIN'
SET GLOBAL rpl_semi_sync_source_enabled_wal = 1;
SET GLOBAL rpl_semi_sync_replica_enabled_wal = 1;
SET GLOBAL rds_physical_rpl_sender_packet_size = 1;

--echo #
--echo # dstore_enable_standby_role should show as false, indicating that the current node is the source.
--echo #
SHOW VARIABLES LIKE 'server_id';
SHOW VARIABLES LIKE 'dstore_enable_standby_role';

--echo #
--echo # Install the standby server's plugin and set enabled_wal to ON.
--echo #
--connection $node2
--eval INSTALL PLUGIN rpl_semi_sync_source SONAME '$SEMISYNC_SOURCE_PLUGIN'
--eval INSTALL PLUGIN rpl_semi_sync_replica SONAME '$SEMISYNC_REPLICA_PLUGIN'
SET GLOBAL rpl_semi_sync_source_enabled_wal = 1;
SET GLOBAL rpl_semi_sync_replica_enabled_wal = 1;
SET GLOBAL rds_physical_rpl_sender_packet_size = 1;

--echo #
--echo # dstore_enable_standby_role should show as true, indicating that the current node is the replica.
--echo #
SHOW VARIABLES LIKE 'server_id';
SHOW VARIABLES LIKE 'dstore_enable_standby_role';

--echo #
--echo # Configure the replica and check whether it is configured correctly.
--echo #
STOP REPLICA;
RESET REPLICA ALL;
--disable_warnings
--replace_result $SERVER_MYPORT_1 SERVER_MYPORT_1
--let $rpl_channel_name = 'dstore_wal_replication_channel'
--eval CHANGE REPLICATION SOURCE TO SOURCE_HOST='127.0.0.1', SOURCE_PORT=$SERVER_MYPORT_1, SOURCE_USER='root', SOURCE_HEARTBEAT_PERIOD = 100 FOR CHANNEL $rpl_channel_name;
--enable_warnings
--let $assert_text=Channel name should be $rpl_channel_name
--let $assert_sql=SELECT CHANNEL_NAME FROM performance_schema.replication_connection_configuration WHERE CHANNEL_NAME = $rpl_channel_name
--let $assert_cond=COUNT(*) = 1
--source include/assert.inc

--echo Slave_IO_Running should be OFF.
select rcs.SERVICE_STATE as Slave_IO_Running from performance_schema.replication_connection_status rcs;

START REPLICA;
# Wait for 2 seconds to prevent uncertain results displayed in subsequent views.
--sleep 2

--echo #
--echo # Slave_IO_Running should be ON.
--echo #
select rcs.SERVICE_STATE as Slave_IO_Running from performance_schema.replication_connection_status rcs;
