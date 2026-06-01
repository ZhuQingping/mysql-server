# ==== Purpose ====
#
# Skip testcases which don't have obs env.
#
# ==== Usage ====
#
#   --source suite/local_backup/include/have_obs_env.inc


--disable_result_log
--disable_query_log

--exec echo $OBS_AK > $MYSQLTEST_VARDIR/obs_ak.txt
--let $include_silent = 1
--let $read_from_file = $MYSQLTEST_VARDIR/obs_ak.txt
--source include/read_file_to_var.inc
--let $obs_ak = $result
--let $include_silent = 0
--exec rm -rf $MYSQLTEST_VARDIR/obs_ak.txt

--enable_query_log
--enable_result_log

if (!$obs_ak)
{
  --skip Test requires 'have_obs_env'
}
