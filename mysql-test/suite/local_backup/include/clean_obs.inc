# ==== Purpose ====
#
# Remove objects from obs.
#
# ==== Usage ====
#
#   [--let $mock_obs = 1]
#   --source suite/local_backup/include/clean_obs.inc
#
# Parameters:
#
#   $mock_obs
#     By default, 0 is configured which means using true obs interface. You can
#     configure 1 to mock obs interface by setting this variable.


--disable_result_log
--disable_query_log

--let $MYSQL_BASEDIR = `SELECT @@basedir`

if (!$mock_obs)
{
  --exec $MYSQL_BASEDIR/bin/backup_download -p `cat $MYSQLTEST_VARDIR/obs_prefix.txt`/ -c $MYSQLTEST_VARDIR/backup_download.config -r
}
if ($mock_obs)
{
  --exec $MYSQL_BASEDIR/bin/backup_download -p `cat $MYSQLTEST_VARDIR/obs_prefix.txt`/ -c $MYSQLTEST_VARDIR/backup_download.config -r --debug=d,mock_list_object,mock_delete_object
}

--enable_query_log
--enable_result_log
