# ==== Purpose ====
#
# Prepare for backup, create dir and replace config.
#
# ==== Usage ====
#
#   [--let $clean_obs_history = 1]
#   --source suite/local_backup/include/prepare_for_backup.inc
#
# Parameters:
#
#   $clean_obs_history
#     By default, 0 is configured which means do not clean obs history data. You can
#     configure 1 to clean obs history data by setting this variable.


--exec sh $MYSQL_TEST_DIR/suite/local_backup/include/create_dir.sh

# Remove objects from obs if use obs.
--let $rds_local_backup_use_obs = `SELECT @@rds_local_backup_use_obs`
--let $original_rds_full_local_backup_path = `SELECT @@rds_full_local_backup_path`
--let $original_rds_local_backup_obs_access_key = `SELECT @@rds_local_backup_obs_access_key`
--let $original_rds_local_backup_obs_secret_key = `SELECT @@rds_local_backup_obs_secret_key`
if ($rds_local_backup_use_obs == '1') {
  --source suite/local_backup/include/prepare_obs_ak_sk.inc
  if ($clean_obs_history)
  {
    --source suite/local_backup/include/clean_obs_history.inc
  }
  --let $timestamp_ms = `SELECT ROUND(UNIX_TIMESTAMP(NOW(6)) * 1000)`
  --let $uuid_suffix = `SELECT UUID()`
  --disable_result_log
  --disable_query_log
  --eval SET GLOBAL rds_full_local_backup_path = "$original_rds_full_local_backup_path/$timestamp_ms$uuid_suffix"
  --eval SET GLOBAL dstore_local_backup_meta_path = "$original_rds_full_local_backup_path/$timestamp_ms$uuid_suffix/meta"
  --let $obs_prefix = `SELECT @@rds_full_local_backup_path`
  --exec echo $obs_prefix > $MYSQLTEST_VARDIR/obs_prefix.txt
  --enable_query_log
  --enable_result_log
  --source suite/local_backup/include/clean_obs.inc
}

--let $MYSQL_BASEDIR = `SELECT @@basedir`
--let $MYSQLD_DATADIR = `SELECT @@datadir`
--let $backup_config_py = $MYSQLTEST_VARDIR/backup_config.py

--exec sed -i 's#^    "backup_monitor".*#    "backup_monitor": "$MYSQL_BASEDIR/bin/backup_monitor",#g' $backup_config_py
--exec sed -i 's#^    "backup_data_path".*#    "backup_data_path": "$MYSQLTEST_VARDIR/full_local_backup",#g' $backup_config_py
--exec sed -i 's#^    "backup_meta_path".*#    "backup_meta_path": "$MYSQLTEST_VARDIR/local_backup_meta",#g' $backup_config_py
--exec sed -i 's#^    "backup_wal_path".*#    "backup_wal_path": "$MYSQLTEST_VARDIR/wal_archive",#g' $backup_config_py
--exec sed -i 's#^    "backup_output_path".*#    "backup_output_path": "$MYSQLTEST_VARDIR/backup_output",#g' $backup_config_py
--exec sed -i 's#^    "copy_back_data_path".*#    "copy_back_data_path": "$MYSQLD_DATADIR",#g' $backup_config_py
--exec sed -i 's#^    "copy_back_meta_path".*#    "copy_back_meta_path": "$MYSQLTEST_VARDIR/local_backup_restore_meta",#g' $backup_config_py
--exec sed -i 's#^    "base_path".*#    "base_path": "$MYSQLTEST_VARDIR/obs"#g' $MYSQLTEST_VARDIR/backup_download.config
