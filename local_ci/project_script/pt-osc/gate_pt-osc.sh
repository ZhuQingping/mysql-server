#!/bin/bash
set +e

workdir=$1
codehubTargetBranch=$2
codehubTargetRepoHttpUrl=$3
codehubSourceRepoHttpUrl=$4
codehubSourceBranch=$5
build_type=${6:-debug}
source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
# Update code
echo "---------------------- Update code --------------------------------"
cd ${workdir}/mysql-server/local_ci/project_script/common
sh update_code.sh $workdir $codehubTargetBranch $codehubTargetRepoHttpUrl $codehubSourceRepoHttpUrl $codehubSourceBranch
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh
#start cde mtr ci
echo "---------------------- start mtr gate ci--------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,run_pt_osc,clean 2>&1 | tee ci_pt_osc.log

exit_flag=0
if [ -n "`grep "Compile failed" ${workdir}/mysql-server/local_ci/script/logs/compile.log`" ]; then
  echo "Failure Cause: Compile failed"
  exit_flag=1
elif [ -f ${workdir}/mysql-server/local_ci/script/logs/run_pt_osc.log ]; then
  if [ -n "`grep -E "Data validation failed" ${workdir}/mysql-server/local_ci/script/logs/run_pt_osc.log`" ]; then
    echo "Failure Cause: Not all test cases are executed."
    exit_flag=1
  fi
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_pt_osc.log

exit ${exit_flag}

