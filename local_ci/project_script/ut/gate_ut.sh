#!/bin/bash
set +e

workdir=$1
codehubTargetBranch=$2
codehubTargetRepoHttpUrl=$3
codehubSourceRepoHttpUrl=$4
codehubSourceBranch=$5
CODEHUB_CODE_PATH=${workdir}/mysql-server
# Add build_type similar to how it is implemented in local_ci/script/start_ci.sh
build_type="${6:-release}"
mergerequestid=${7:-0}

source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
# Update code
echo "---------------------- Update code --------------------------------"
cd ${workdir}/mysql-server/local_ci/project_script/common
sh update_code.sh $workdir $codehubTargetBranch $codehubTargetRepoHttpUrl $codehubSourceRepoHttpUrl $codehubSourceBranch
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh -m ${mergerequestid}
#start ut
echo "---------------------- start build run ut --------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,run_ut,clean 2>&1 | tee ci_ut.log

exit_flag=0
if [ ! -f ${workdir}/mysql-server/local_ci/script/logs/compile.log ]; then
  exit_flag=1
elif [ -n "`grep "Compile failed" ${workdir}/mysql-server/local_ci/script/logs/compile.log`" ]; then
  exit_flag=1
elif [ -f ${workdir}/mysql-server/local_ci/script/logs/run_ut.log ]; then
  if [ -n "`grep "run ut fail" ${workdir}/mysql-server/local_ci/script/logs/run_ut.log`" ]; then
    exit_flag=1
  fi
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_ut.log

exit ${exit_flag}

