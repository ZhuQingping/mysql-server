--echo ############################################################
--echo # Uninstall the plugins, reset the variables, and restore the original master-replica replication relationship.
--echo ############################################################

if (!$current_primary)
{
  --let $current_primary = $node1
}

if (!$current_standby)
{
  --let $current_standby = $node2
}

--echo #
--echo # 1. Shutdown the mysqld instances of the master and replica servers launched with the new data directory.
--echo #
--connection $current_standby
STOP REPLICA;
RESET REPLICA ALL FOR CHANNEL 'dstore_wal_replication_channel';

--connection $current_primary
--source include/stop_wal_dump_threads.inc
SET GLOBAL rpl_semi_sync_source_enabled_wal = 0;
SET GLOBAL rpl_semi_sync_replica_enabled_wal = 0;
UNINSTALL PLUGIN rpl_semi_sync_source;
UNINSTALL PLUGIN rpl_semi_sync_replica;
use mtr;

--connection $current_standby
SET GLOBAL rpl_semi_sync_source_enabled_wal = 0;
SET GLOBAL rpl_semi_sync_replica_enabled_wal = 0;
UNINSTALL PLUGIN rpl_semi_sync_source;
UNINSTALL PLUGIN rpl_semi_sync_replica;
use mtr;

--echo #
--echo # 2. Shutdown the mysqld instances of the master and replica servers launched with the new data directory.
--echo #
--let $rpl_server_number= 1
--source include/rpl_stop_server.inc
--let $rpl_server_number= 2
--source include/rpl_stop_server.inc

--echo # Delete the directories of the primary and standby servers.
--exec rm -rf $MY_MASTER_DATADIR/
--exec rm -rf $MY_SLAVE_DATADIR/

--echo #
--echo # 3. Restart the mysqld instances of the master and replica servers using the configurations from the opt file.
--echo #

# Set the value of rpl_server_parameters to override the existing configuration; otherwise, the previously set values will remain in effect.
--let $rpl_server_parameters = --loose-dstore_enable_standby_role=OFF
--let $rpl_server_number = 1
--source include/rpl_start_server.inc
# Delete the database from the original directory.
--connection $node1
show databases;
drop database copy_database;
use mtr;

--let $rpl_server_number = 2
--let $rpl_server_parameters = --loose-dstore_enable_standby_role=ON
--source include/rpl_start_server.inc
use mtr;
