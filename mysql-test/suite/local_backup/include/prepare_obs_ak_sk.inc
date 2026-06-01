# ==== Purpose ====
#
# Prepare ak/sk for obs interface.
#
# ==== Usage ====
#
#   --source suite/local_backup/include/prepare_obs_ak_sk.inc


--disable_result_log
--disable_query_log

if ($original_rds_local_backup_obs_access_key == "")
{
  --exec echo $OBS_AK > $MYSQLTEST_VARDIR/obs_ak.txt
  --exec echo $OBS_SK > $MYSQLTEST_VARDIR/obs_sk.txt
  --let $include_silent = 1
  --let $read_from_file = $MYSQLTEST_VARDIR/obs_ak.txt
  --source include/read_file_to_var.inc
  --let $obs_ak = $result
  --let $read_from_file = $MYSQLTEST_VARDIR/obs_sk.txt
  --source include/read_file_to_var.inc
  --let $obs_sk = $result
  --let $include_silent = 0
  --eval SET GLOBAL rds_local_backup_obs_access_key = "$obs_ak"
  --eval SET GLOBAL rds_local_backup_obs_secret_key = "$obs_sk"
  --exec sed -i 's#^    "ak".*#    "ak": "$obs_ak",#g' $MYSQLTEST_VARDIR/backup_download.config
  --exec sed -i 's#^    "sk".*#    "sk": "$obs_sk",#g' $MYSQLTEST_VARDIR/backup_download.config
  --exec rm -rf $MYSQLTEST_VARDIR/obs_ak.txt
  --exec rm -rf $MYSQLTEST_VARDIR/obs_sk.txt
}

--enable_query_log
--enable_result_log
