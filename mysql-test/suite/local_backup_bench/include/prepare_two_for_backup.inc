# ==== Purpose ====
#
# Prepare for backup, create dir and replace config.
#
# ==== Usage ====
#
#   --source suite/local_backup_bench/include/prepare_two_for_backup.inc

#--let $rpl_debug= 1
#--let $rpl_topology= 1, 2
--let $rpl_skip_start_slave= 1
--let $rpl_skip_reset_master_and_slave= 1
--let $rpl_skip_change_master= 1
--source include/master-slave.inc


--echo # restart Master
--let $rpl_server_number= 1
--let $rpl_server_parameters= --skip-log-bin
--source include/rpl_restart_server.inc

--exec sh $MYSQL_TEST_DIR/suite/local_backup/include/create_dir.sh

--connection master
--let $MYSQL_BASEDIR = `SELECT @@basedir`
--let $backup_config_py = $MYSQLTEST_VARDIR/backup_config.py
--exec cp $MYSQL_TEST_DIR/suite/local_backup/include/backup_config.py $backup_config_py
--exec cp $MYSQL_TEST_DIR/suite/local_backup/include/backup_ctrl.py $MYSQLTEST_VARDIR/

--connection slave
--let $MYSQLD_DATADIR = `SELECT @@datadir`

--exec sed -i 's#^    "backup_monitor".*#    "backup_monitor": "$MYSQL_BASEDIR/bin/backup_monitor",#g' $backup_config_py
--exec sed -i 's#^    "backup_data_path".*#    "backup_data_path": "$MYSQLTEST_VARDIR/full_local_backup",#g' $backup_config_py
--exec sed -i 's#^    "backup_meta_path".*#    "backup_meta_path": "$MYSQLTEST_VARDIR/local_backup_meta",#g' $backup_config_py
--exec sed -i 's#^    "backup_wal_path".*#    "backup_wal_path": "$MYSQLTEST_VARDIR/wal_archive",#g' $backup_config_py
--exec sed -i 's#^    "backup_output_path".*#    "backup_output_path": "$MYSQLTEST_VARDIR/backup_output",#g' $backup_config_py
--exec sed -i 's#^    "copy_back_data_path".*#    "copy_back_data_path": "$MYSQLD_DATADIR",#g' $backup_config_py
--exec sed -i 's#^    "copy_back_meta_path".*#    "copy_back_meta_path": "$MYSQLTEST_VARDIR/local_backup_restore_meta"#g' $backup_config_py

--echo # stop slave
--let $rpl_server_number= 2
--source include/rpl_stop_server.inc

--connection master
