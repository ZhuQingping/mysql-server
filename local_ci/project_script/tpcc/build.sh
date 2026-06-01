#!/bin/bash
set +e

echo "hello tpcc"

workdir=$1
build_type=${2:-debug}
warehouses=$3
running_time=$4
warmup_time=$5
connections=$6
report_interval=$7
CODEHUB_CODE_PATH=${workdir}/mysql-server

source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh
#start tpcc
echo "---------------------- start build run tpcc --------------------------------"
cd ${workdir}/mysql-server/local_ci/script
echo "warehouses=$warehouses
running_time=$running_time
warmup_time=$warmup_time
connections=$connections
report_interval=$report_interval
" > tpcc_param
sh start_ci.sh -t ${build_type} -o no_format,run_tpcc,clean 2>&1 | tee ci_tpcc.log

exit_flag=0
if [ -n "`grep "Compile failed" ${workdir}/mysql-server/local_ci/script/logs/compile.log`" ]; then
  exit_flag=1
elif [ -f ${workdir}/mysql-server/local_ci/script/logs/run_tpcc.log ]; then
  if [ -n "`grep "run tpcc fail" ${workdir}/mysql-server/local_ci/script/logs/run_tpcc.log`" ]; then
    exit_flag=1
  fi
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_tpcc.log

exit ${exit_flag}

