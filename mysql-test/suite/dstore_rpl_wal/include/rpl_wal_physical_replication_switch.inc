--echo ############################################################
--echo switchover_1
--echo ############################################################
 
--echo # Demote the source to replica.
 
--connection $old_primary
SET GLOBAL read_only = ON;
SET GLOBAL event_scheduler = OFF;
call rpl_wal.pdb_demote();
--echo #
--echo # dstore_enable_standby_role should show as true, indicating that the current node is the replica.
--echo #
SHOW VARIABLES LIKE 'server_id';
SHOW VARIABLES LIKE 'dstore_enable_standby_role';
 
--echo #
--echo # Promote the replica to source.
--echo #
--connection $old_standby
STOP REPLICA;
RESET REPLICA ALL FOR CHANNEL 'dstore_wal_replication_channel';
call rpl_wal.pdb_promote();
--echo #
--echo # dstore_enable_standby_role should show as false, indicating that the current node is the source.
--echo #
SHOW VARIABLES LIKE 'server_id';
SHOW VARIABLES LIKE 'dstore_enable_standby_role';
 
 
--echo #
--echo # Clean the primary and standby servers.
--echo #
--connection $old_primary
--source include/stop_wal_dump_threads.inc
 
--echo #
--echo # Re-establish primary-standby relationship.
--echo #
--connection $old_primary
--disable_warnings
--replace_result $old_standby_port old_standby_port
--eval CHANGE REPLICATION SOURCE TO SOURCE_HOST='127.0.0.1', SOURCE_PORT=$old_standby_port, SOURCE_USER='root', SOURCE_HEARTBEAT_PERIOD = 100 FOR CHANNEL $rpl_channel_name;
--enable_warnings
--let $assert_text=Channel name should be $rpl_channel_name
--let $assert_sql=SELECT CHANNEL_NAME FROM performance_schema.replication_connection_configuration WHERE CHANNEL_NAME = $rpl_channel_name
--let $assert_cond=COUNT(*) = 1
--source include/assert.inc

START REPLICA;
--echo #
--echo # Slave_IO_Running should be ON.
--echo #
select rcs.SERVICE_STATE as Slave_IO_Running from performance_schema.replication_connection_status rcs;
 
--echo #
--echo # Disable read-only mode on the new source.
--echo #
--connection $old_standby
SET GLOBAL read_only = OFF;
SET GLOBAL event_scheduler = ON;
