# Name: Testcase to wait the query end. 
#
# Description:
#   This testcase will wait the query end.
#   USE example:
#   --let $bg_sysbench_pid = pid of background
#   --source suite/local_backup_bench/include/wait_bg_sysbench.inc
#   It will wait the query that created by start_bg_dml.inc to end.

--disable_query_log
--disable_warnings

if ($bg_sysbench_pid=="") {
  echo There is no sysbench pid, please check bg_sysbench_pid;
}
if ($bg_sysbench_pid!="") {
  exec kill -KILL $bg_sysbench_pid|| true;
}

--connection default
--enable_query_log
--enable_warnings
