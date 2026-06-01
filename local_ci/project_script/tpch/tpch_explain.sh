#!/bin/bash
set +e

echo "hello tpch"

workdir=$1
use_innodb_engine=$2
build_type=${3:-release}
codehubTargetBranch=$4
codehubSourceRepoHttpUrl=$5
codehubSourceBranch=$6
CODEHUB_CODE_PATH=${workdir}/mysql-server

source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
# Update code
echo "---------------------- Update code --------------------------------"
cd ${workdir}/mysql-server/local_ci/project_script/common
#sh update_code.sh $workdir $codehubTargetBranch $codehubSourceRepoHttpUrl $codehubSourceBranch

#start tpch explain
echo "---------------------- start build run tpch explain --------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format --run_tpch_explain $use_innodb_engine 2>&1 | tee ci_tpch_explain.log

exit_flag=0
if [ -n "`grep "Compile failed" ${workdir}/mysql-server/local_ci/script/logs/compile.log`" ]; then
  exit_flag=1
elif [ -f ${workdir}/mysql-server/local_ci/script/logs/run_tpch_explain_${use_innodb_engine}.log ]; then
  if [ -n "`grep "run tpch fail" ${workdir}/mysql-server/local_ci/script/logs/run_tpch_explain_${use_innodb_engine}.log`" ]; then
    exit_flag=1
  fi
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_tpch_explain.log

exit ${exit_flag}

