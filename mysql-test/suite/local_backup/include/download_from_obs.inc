# ==== Purpose ====
#
# Download from obs.
#
# ==== Usage ====
#
#   [--let $mock_obs = 1]
#   --source suite/local_backup/include/download_from_obs.inc
#
# Parameters:
#
#   $mock_obs
#     By default, 0 is configured which means using true obs interface. You can
#     configure 1 to mock obs interface by setting this variable.


--disable_result_log
--disable_query_log

--let $MYSQL_BASEDIR = `SELECT @@basedir`
--let $prefix_key = `SELECT @@rds_full_local_backup_path`
--let $bd = $MYSQL_BASEDIR/bin/backup_download

if (!$mock_obs)
{
  --exec $bd -p $prefix_key/ -c $MYSQLTEST_VARDIR/backup_download.config -t $MYSQLTEST_VARDIR/download_trace.json -w 4

  --exec mv $MYSQLTEST_VARDIR/obs/meta/* $MYSQLTEST_VARDIR/local_backup_meta/

  --exec $MYSQL_BASEDIR/bin/backup_monitor $MYSQLTEST_VARDIR/local_backup_meta -l | grep '\"timePoint\"' | tail -n 1 | awk "{print \$2}" | sed "s/.\$//" > $MYSQLTEST_VARDIR/timePoint.txt
  --exec date -d @`cat $MYSQLTEST_VARDIR/timePoint.txt` "+%YY%mM%dD%HH%MM%SS" > $MYSQLTEST_VARDIR/backupDir.txt

  --exec mv $MYSQLTEST_VARDIR/obs/full_backup_* $MYSQLTEST_VARDIR/full_local_backup/`cat $MYSQLTEST_VARDIR/backupDir.txt`

  --exec rm -f $MYSQLTEST_VARDIR/timePoint.txt
  --exec rm -f $MYSQLTEST_VARDIR/backupDir.txt
}
if ($mock_obs)
{
  --exec $bd -p $prefix_key/ -c $MYSQLTEST_VARDIR/backup_download.config -t $MYSQLTEST_VARDIR/download_trace.json -w 4 --debug=d,mock_list_object,mock_get_object_meta,mock_get_object
}

--enable_query_log
--enable_result_log
