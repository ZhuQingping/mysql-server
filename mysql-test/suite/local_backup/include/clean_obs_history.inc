# ==== Purpose ====
#
# Remove history objects from obs.
#
# ==== Usage ====
#
#   [--let $mock_obs = 1]
#   --source suite/local_backup/include/clean_obs_history.inc
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
--let $clean_timestamp = `SELECT FLOOR(UNIX_TIMESTAMP() / 100000 - 7)`

--let $counter = 3
--let $inner_counter = 10
while ($counter)
{
  if ($counter == 1)
  {
    --let $inner_counter = 3
  }
  while ($inner_counter)
  {
    if (!$mock_obs)
    {
      --let $bd_command = $MYSQL_BASEDIR/bin/backup_download -p $prefix_key/$clean_timestamp -c $MYSQLTEST_VARDIR/backup_download.config -r
    }
    if ($mock_obs)
    {
      --let $bd_command = $MYSQL_BASEDIR/bin/backup_download -p $prefix_key/$clean_timestamp -c $MYSQLTEST_VARDIR/backup_download.config -r --debug=d,mock_list_object,mock_delete_object
    }
    --exec $bd_command
    --dec $clean_timestamp
    --dec $inner_counter
  }
  --let $inner_counter = 10
  --let $clean_timestamp = `SELECT FLOOR($clean_timestamp / $inner_counter)`
  --dec $counter
}

--enable_query_log
--enable_result_log
