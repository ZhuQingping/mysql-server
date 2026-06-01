#!/bin/bash
set +e

workdir=$1
codehubTargetBranch=$2
codehubTargetRepoHttpUrl=$3
codehubSourceRepoHttpUrl=$4
codehubSourceBranch=$5
CODEHUB_CODE_PATH=${workdir}/mysql-server
build_type=${6:-debug}
mtr_type=${7:-run_mtr_gate}
mergerequestid=${8:-0}
source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
# Update code
echo "---------------------- Update code --------------------------------"
cd ${workdir}/mysql-server/local_ci/project_script/common
sh update_code.sh $workdir $codehubTargetBranch $codehubTargetRepoHttpUrl $codehubSourceRepoHttpUrl $codehubSourceBranch
#sh ${workdir}/mysql-server/local_ci/project_script/common/download_cmc.sh
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh -m ${mergerequestid}
#start cde mtr ci
echo "---------------------- start mtr gate ci--------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,${mtr_type},clean 2>&1 | tee ci_mtr.log

exit_flag=0
if [ ! -f ${workdir}/mysql-server/local_ci/script/logs/compile.log ]; then
  exit_flag=1
elif [ -n "`grep "Compile failed" ${workdir}/mysql-server/local_ci/script/logs/compile.log`" ]; then
  log ERROR "Failure Cause: Compile failed"
  exit_flag=1
elif [ -f ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log ]; then
  if [ -n "`grep -E "Disabled test * could not be located" ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log`" ]; then
    log ERROR "Failure Cause: Failed to run the mysql-test-run command."
    exit_flag=1
  elif [ -z "`grep -E "Completed:" ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log`" ]; then
    log ERROR "Failure Cause: Not all test cases are executed."
    exit_flag=1
  elif [ -n "`grep -E "Failing test" ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log`" ]; then
    if [ -n "`grep "Failing test(s): shutdown_report" ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log`" ]; then
      log ERROR "Failure Cause: The shutdown_report failed."
      exit_flag=1
    elif [ ! -f ${workdir}/mysql-server/local_ci/script/logs/MTR_retry_result.out ]; then
      log ERROR "Failure Cause: The test case failed."
      exit_flag=1
    elif [ -n "`grep -E "Failing test" ${workdir}/mysql-server/local_ci/script/logs/MTR_retry_result.out`" ]; then
      log ERROR "Failure Cause: The test case retry-failed."
      exit_flag=1
    fi
  fi
fi

# Generated result.xml
if [ -f ${workdir}/mysql-server/local_ci/script/logs/MTR_retry_result.out ]; then
  output_report ${workdir}/mysql-server/local_ci/script/logs/MTR_retry_result.out ${workdir}/mysql-server/local_ci/script/logs
else
  output_report ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log ${workdir}/mysql-server/local_ci/script/logs
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_mtr.log

exit ${exit_flag}

