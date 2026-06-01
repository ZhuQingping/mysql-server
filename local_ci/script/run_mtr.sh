#!\bin\bash
build_type=$1
mtr_type=$2
asan_option=$3
suite_name=${4:-0}

source ./common_conf.sh
sanitize_option=""
if [ "$asan_option"x == "asan"x ]; then
  sanitize_option="--sanitize"
fi

if [ "$asan_option"x == "tsan"x ]; then
  export TSAN_OPTIONS="suppressions=${mysql_root_path}/mysql-test/tsan.supp:history_size=3:deadlock_check=0:detect_deadlocks=0:report_thread_leaks=0"
fi
engine_type="--dstore"
pq_option=""
test_list="tmp_test_list"
if [ "$mtr_type"x == "mtr_cov_innodb"x ]; then
  mtr_type="mtr_cov"
  engine_type=""
  test_list="tmp_innodb_test_list"
fi
if [ "$mtr_type"x == "mtr_AT_innodb_pq"x ]; then
  mtr_type="mtr_AT_innodb"
  pq_option="--pq"
fi
if [ "$mtr_type"x == "mtr_cov_pq"x ]; then
  mtr_type="mtr_cov"
  engine_type=""
  test_list="tmp_pq_test_list"
  pq_option="--pq"
fi
if [ "$platform" == "aarch64" ]; then
  jobs_num=16
  disabled_list="./collections/disabled-dstore-arm.def"
else
  jobs_num=16
  disabled_list="./collections/disabled-dstore.def"
fi
#pkg test base part mtr now
innodb_suites=(main audit_log audit_null auth_sec binlog binlog_gtid binlog_nogtid clone collations component_keyring_file connection_control engines/funcs engines/iuds engines/rr_trx encryption federated funcs_1 funcs_2 gcol gcol_ndb gis group_replication innodb innodb_fts innodb_gis innodb_stress innodb_undo innodb_zip information_schema interactive_utilities json json_ndb jp large_tests lock_order max_parts memcached ndb ndb_big ndb_binlog ndbcluster ndb_ddl ndb_opt ndb_rpl ndbcrunch network_namespace opt_trace parts perfschema query_rewrite_plugins rpl rpl_gtid rpl_ndb rpl_nogtid secondary_engine service_status_var_registration service_sys_var_registration service_udf_registration test_service_sql_api special stress sysschema sys_vars test_services x ptrc threadpool dbms_package parallel_query recyclebin)
# Keep same with DEFAULT_SUITES in mysql-test/mysql-test-run.pl,
# if following dstore_suites changes, DEFAULT_SUITES needs to be updated accordingly.
dstore_suites=(main audit_log binlog binlog_gtid binlog_nogtid dstore_main dstore_binlog dstore_binlog_gtid dstore_binlog_nogtid dstore_rpl dstore_rpl_gtid dstore_rpl_nogtid dstore collations connection_control engines/funcs engines/iuds engines/rr_trx encryption federated funcs_1 funcs_2 gcol innodb innodb_fts innodb_gis innodb_stress innodb_undo innodb_zip information_schema interactive_utilities json jp local_backup network_namespace opt_trace perfschema query_rewrite_plugins rpl rpl_gtid rpl_nogtid service_status_var_registration service_sys_var_registration service_udf_registration test_service_sql_api stress sysschema sys_vars x threadpool dbms_package dstore_inplace_ddl dstore_rpl_wal recyclebin dstore_parts dstore_sys_vars dstore_sysschema dstore_x)

innodb_suites1="audit_log,innodb,auth_sec,binlog_gtid,innodb_zip,collations,federated,test_service_sql_api,connection_control,opt_trace,engines/funcs,funcs_2,gis,ptrc,threadpool"
innodb_suites2="main,component_keyring_file,binlog_nogtid,stress,engines/iuds,innodb_gis,gcol,json,perfschema"
innodb_suites3="group_replication,innodb_undo,rpl_nogtid,sys_vars,parts,sysschema,information_schema,encryption,service_sys_var_registration,secondary_engine,service_udf_registration,memcached,rpl_gtid"
innodb_suites4="rpl,clone,innodb_fts,binlog,large_tests,funcs_1,jp,test_services,innodb_stress,query_rewrite_plugins,audit_null,service_status_var_registration,x,dbms_package"
if [ "$mtr_type" == "mtr_innodb_gate1" ]; then
  mtr_type="mtr_innodb_gate"
  suites_name=$innodb_suites1
elif [ "$mtr_type" == "mtr_innodb_gate2" ]; then
  mtr_type="mtr_innodb_gate"
  suites_name=$innodb_suites2
elif [ "$mtr_type" == "mtr_innodb_gate3" ]; then
  mtr_type="mtr_innodb_gate"
  suites_name=$innodb_suites3
elif [ "$mtr_type" == "mtr_innodb_gate4" ]; then
  mtr_type="mtr_innodb_gate"
  suites_name=$innodb_suites4
fi

retry_regular_failed_tests() {
    if [ -n "`grep -E "Failing test" $build_path/mysql-test/MTR_result.out`" ]; then
    grep "Failing test(s):" $build_path/mysql-test/MTR_result.out | awk -F': ' '{print $2}' | tr ' ' '\n' | sed 's/ //g' > failed_test_list
    if [ -n "`grep -E "shutdown_report" $build_path/mysql-test/failed_test_list`" ]; then
      echo "Engine cases cannot be executed again"
      exit 1
    fi
    ./mtr $sanitize_option --parallel=1 $engine_type $pq_option --big-test --do-test-list=failed_test_list --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb 2>&1 | tee -a $build_path/mysql-test/MTR_retry_result.out
  fi
}
retry_pq_failed_tests() {
  if [ -n "`grep -E "Failing test" $build_path/mysql-test/MTR_result_pq.out`" ]; then
    grep "Failing test(s):" $build_path/mysql-test/MTR_result_pq.out | awk -F': ' '{print $2}' | tr ' ' '\n' | sed 's/ //g' > failed_test_list
    if [ -n "`grep -E "shutdown_report" $build_path/mysql-test/failed_test_list`" ]; then
      echo "Engine cases cannot be executed again"
      exit 1
    fi
    ./mtr $sanitize_option --parallel=1 --pq --big-test --do-test-list=failed_test_list --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb 2>&1 | tee -a $build_path/mysql-test/MTR_retry_result.out
  fi
}

declare -A GATE_SUITES=(
    ["mtr_gate1"]="$engine_type $pq_option --do-test-list=mtr_schedule_ci_default"
    ["mtr_gate2"]="$engine_type $pq_option --big-test --suite=dstore_main"
    ["mtr_gate3"]="$engine_type $pq_option --big-test --suite=json"
    ["mtr_gate4"]="$engine_type $pq_option --big-test --suite=threadpool"
    ["mtr_gate5"]="$engine_type $pq_option --big-test --suite=audit_log"
    ["mtr_gate6"]="$engine_type $pq_option --big-test --suite=dstore_inplace_ddl"
    ["mtr_gate7"]="$engine_type $pq_option --big-test --suite=dstore_parts"
    ["mtr_gate8"]="$engine_type $pq_option --big-test --suite=local_backup"
    ["mtr_gate9"]="--pq --big-test --suite=parallel_query"
    ["mtr_gate10"]="$engine_type $pq_option --big-test --suite=dstore_rpl_wal"
)

declare -A XML_REPORTS=(
    ["mtr_gate1"]="mtr-report.xml"
    ["mtr_gate2"]="dstore_mtr-report.xml"
    ["mtr_gate3"]="json_mtr-report.xml"
    ["mtr_gate4"]="threadpool_mtr-report.xml"
    ["mtr_gate5"]="audit_log_mtr-report.xml"
    ["mtr_gate6"]="json_mtr-report.xml"
    ["mtr_gate7"]="json_mtr-report.xml"
    ["mtr_gate8"]="local_backup_mtr-report.xml"
    ["mtr_gate9"]="parallel_query_mtr-report.xml"
    ["mtr_gate10"]="json_mtr-report.xml"
)

OUTPUT_FILE="MTR_result.out"
[ "$mtr_type" == "mtr_gate9" ] && OUTPUT_FILE="MTR_result_pq.out"

# run mtr test cases in install directory
cd $build_path/mysql-test
mkdir engines
echo "mtr_type:"$mtr_type
if [ "$mtr_type"x == "mtr_at"x ]; then
  for s in ${dstore_suites[@]}; do
    echo "runing ${s}"
    ./mtr $sanitize_option --parallel=16 $engine_type $pq_option --big-test --suite=${s} --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=./collections/disabled-dstore.def --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/${s}_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  done
elif [ "$mtr_type"x == "mtr_gate"x ]; then
  # run mtr test cases in build directory
  cd $build_path/mysql-test
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --do-test-list=mtr_schedule_ci_default --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=dstore_main --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/dstore_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=json --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/json_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=threadpool --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/threadpool_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  # run audit log suite.
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=audit_log --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/audit_log_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  # run dstore_inplace_ddl suite.
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=dstore_inplace_ddl --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/json_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  # run dstore_parts suite.
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=dstore_parts --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/json_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  # run local_backup suite with binlog
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=local_backup --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/local_backup_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  ./mtr $sanitize_option --parallel=${jobs_num} --pq --big-test --suite=parallel_query --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/parallel_query_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result_pq.out
  # run dstore_rpl_wal suite.
  ./mtr $sanitize_option --parallel=4 $engine_type $pq_option --big-test --suite=dstore_rpl_wal --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/json_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  # 重试失败用例
  if [ -n "`grep -E "Failing test" $build_path/mysql-test/MTR_result.out`" ]; then
    grep "Failing test(s):" $build_path/mysql-test/MTR_result.out | awk -F': ' '{print $2}' | tr ' ' '\n' | sed 's/ //g' > failed_test_list
    if [ -n "`grep -E "shutdown_report" $build_path/mysql-test/failed_test_list`" ]; then
      echo "Engine cases cannot be executed again"
      exit 1
    fi
    ./mtr $sanitize_option --parallel=1 $engine_type $pq_option --big-test --do-test-list=failed_test_list --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb 2>&1 | tee -a $build_path/mysql-test/MTR_retry_result.out
  fi
  if [ -n "`grep -E "Failing test" $build_path/mysql-test/MTR_result_pq.out`" ]; then
    grep "Failing test(s):" $build_path/mysql-test/MTR_result_pq.out | awk -F': ' '{print $2}' | tr ' ' '\n' | sed 's/ //g' > failed_test_list
    ./mtr $sanitize_option --parallel=1 --pq --big-test --do-test-list=failed_test_list --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb 2>&1 | tee -a $build_path/mysql-test/MTR_retry_result.out
  fi
elif [ "$mtr_type"x == "mtr_cov"x ]; then
  export MTR_START_TIMEOUT=600
  if [ "$suite_name"x == "0"x ]; then
    ./mtr $sanitize_option $engine_type $pq_option --parallel=4 --big-test --do-test-list=${test_list} --timer --testcase-timeout=30 --shutdown-timeout=600 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=./collections/disabled-dstore.def --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  else
    ./mtr $sanitize_option $engine_type $pq_option --parallel=4 --suite=${suite_name} --timer --testcase-timeout=30 --shutdown-timeout=600 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=./collections/disabled-dstore.def --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  fi
elif [ "$mtr_type"x == "mtr_innodb_gate"x ]; then
  ./mtr $sanitize_option $pq_option --suite=${suites_name} --parallel=16 --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  if [ -n "`grep -E "Failing test" $build_path/mysql-test/MTR_result.out`" ]; then
    grep "Failing test(s):" $build_path/mysql-test/MTR_result.out | awk -F': ' '{print $2}' | tr ' ' '\n' | sed 's/ //g' > failed_test_list
    if [ -n "`grep -E "engines/" $build_path/mysql-test/failed_test_list`" -o -n "`grep -E "shutdown_report" $build_path/mysql-test/failed_test_list`" ]; then
      echo "Engine cases cannot be executed again"
      exit 1
    fi
    ./mtr $sanitize_option $pq_option --parallel=1 --do-test-list=failed_test_list --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations --exclude-platform=windows --skip-ndb 2>&1 | tee -a $build_path/mysql-test/MTR_retry_result.out
  fi
elif [ "$mtr_type"x == "mtr_AT_innodb"x ]; then
  for s in ${innodb_suites[@]}; do
    echo "runing ${s}"
    if [ "$s"x == "parallel_query"x ]; then
      pq_option="--pq"
    fi
    ./mtr $sanitize_option $pq_option --parallel=16 --suite=${s} --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/${s}_mtr-report.xml 2>&1 | tee -a $build_path/mysql-test/MTR_result.out
  done
elif [ "$mtr_type"x == "mtr_gate10"x ]; then
  ./mtr $sanitize_option --parallel=${jobs_num} $engine_type $pq_option --big-test --suite=dstore_rpl_wal --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/${XML_REPORTS[$mtr_type]} 2>&1 | tee -a $build_path/mysql-test/${OUTPUT_FILE}
  retry_regular_failed_tests
  retry_pq_failed_tests
else
  ./mtr $sanitize_option --parallel=${jobs_num} ${GATE_SUITES[$mtr_type]} --timer --testcase-timeout=30 --suite-timeout=400 --force --max-test-fail=0 --nounit-tests --report-unstable-tests --skip-combinations  --skip-test-list=${disabled_list} --exclude-platform=windows --skip-ndb --xml-report=$build_path/mysql-test/${XML_REPORTS[$mtr_type]} 2>&1 | tee -a $build_path/mysql-test/${OUTPUT_FILE}
  retry_regular_failed_tests
  retry_pq_failed_tests
fi

if [ ! -d $log_dir ]; then
  mkdir -p $log_dir
fi
cp $build_path/mysql-test/*mtr-report.xml $log_dir/
cp $build_path/mysql-test/engines/*mtr-report.xml $log_dir/
cp $build_path/mysql-test/MTR*.out $log_dir/
