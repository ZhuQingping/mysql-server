#
# Clean backup dirs
#
# Usage:
#   --source suite/local_backup/include/clean_backup.inc
#


--exec rm -rf $MYSQLTEST_VARDIR/backup_output
--exec rm -rf $MYSQLTEST_VARDIR/full_local_backup
--exec rm -rf $MYSQLTEST_VARDIR/local_backup_meta
--exec rm -rf $MYSQLTEST_VARDIR/local_backup_restore_meta
--exec rm -rf $MYSQLTEST_VARDIR/wal_archive
--exec rm -rf $MYSQLTEST_VARDIR/obs

--exec rm -rf $MYSQLTEST_VARDIR/backup_config.py
--exec rm -rf $MYSQLTEST_VARDIR/backup_ctrl.py

# Remove objects from obs if use obs.
--let $rds_local_backup_use_obs = `SELECT @@rds_local_backup_use_obs`
if ($rds_local_backup_use_obs == '1') {
  --source suite/local_backup/include/clean_obs.inc
  --disable_result_log
  --disable_query_log
  --eval SET GLOBAL rds_full_local_backup_path = "$original_rds_full_local_backup_path"
  --eval SET GLOBAL dstore_local_backup_meta_path = "$original_rds_full_local_backup_path/meta"
  --eval SET GLOBAL rds_local_backup_obs_access_key = "$original_rds_local_backup_obs_access_key"
  --eval SET GLOBAL rds_local_backup_obs_secret_key = "$original_rds_local_backup_obs_secret_key"

  --exec rm -f $MYSQLTEST_VARDIR/obs_prefix.txt

  --enable_query_log
  --enable_result_log
}

--exec rm -rf $MYSQLTEST_VARDIR/backup_download.config
--exec rm -rf $MYSQLTEST_VARDIR/download_trace.json
