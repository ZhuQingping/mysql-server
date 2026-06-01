# Name: Testcase to create some connections and query on a table in background. 
#
# Description:
#   This testcase creates some connections table and run dml querys.
#   USE example:
#   --let $bg_bench_name = oltp_write_only
#   --let $bg_bench_tables = 4 
#   --let $bg_bench_threads = 2
#   --let $bg_bench_time = 60
#   --let $bg_bench_rows = 1000
#   --let $bg_query_db_name = sbtest
#   --source suite/local_backup/include/start_bg_sysbench.inc
#   It will create background sysbench

--disable_query_log
--disable_warnings

# Add user for sysbench to connect using plain old native password
CREATE USER sbuser IDENTIFIED WITH mysql_native_password;
GRANT ALL ON sbtest.* TO sbuser;

if ($bg_bench_threads=="") {
  --let $bg_bench_threads=2
}

if ($bg_bench_tables=="") {
  --let $bg_bench_tables=4
}

if ($bg_bench_time=="") {
  --let $bg_bench_time=60
}

if ($bg_bench_rows=="") {
  --let $bg_bench_rows=1000
}


if ($bg_query_db_name=="") {
  --let $bg_query_db_name=sbtest
}

let $sysbench_path = /home/ci/mregress/sysbench-1.1;
if ($bg_bench_name=="") {
  --let $bg_bench_name=/usr/share/sysbench/oltp_read_write.lua
}

# Recreate database if it already exist
--connection default
eval DROP DATABASE if exists $bg_query_db_name;
eval CREATE DATABASE $bg_query_db_name;

let $args = --benchmark=$bg_bench_name;

# Benchmark parameters
if ($bg_bench_tables) {
  let $args = $args --tables=$bg_bench_tables;
}
if ($bg_bench_threads) {
  let $args = $args --threads=$bg_bench_threads;
}
if ($bench_secondary_index) {
  let $args = $args --secondary=$bench_secondary_index;
}
if ($bench_args) {
  let $args = $args $bench_args;
}

let $UNIQUE_ID = `select uuid()`;
let $PID_FILE = $MYSQLTEST_VARDIR/tmp/child_pid_$UNIQUE_ID;
let $args = $args --pid_file="$PID_FILE";
#echo args: $args;
#

let $env_vars = env CRUNCH_ENGINE=Dstore CRUNCH_ROWS=$bg_bench_rows CRUNCH_TIME=$bg_bench_time;
#let $env_vars = env CRUNCH_ENGINE=Dstore CRUNCH_ROWS=$bg_bench_rows CRUNCH_TIME=$bg_bench_time LUA_PATH="$sysbench_path/?.lua;$LUA_PATH" PATH="$sysbench_path:$PATH" LD_LIBRARY_PATH="$sysbench_path:$LD_LIBRARY_PATH";
let $cmds = $env_vars perl $MYSQL_TEST_DIR/suite/ndbcrunch/sb_run.pl $args;
#echo cmds: $cmds;

exec mkdir -p $MYSQLTEST_VARDIR/tmp;
exec_in_background $env_vars perl $MYSQL_TEST_DIR/suite/local_backup_bench/include/sb_run.pl $args;

--sleep 3

--let $read_from_file = $PID_FILE
--source include/read_file_to_var.inc

let $bg_sysbench_pid = $result;
#echo bg_sysbench_pid in start: $bg_sysbench_pid;

--connection default
--enable_query_log
--enable_warnings
