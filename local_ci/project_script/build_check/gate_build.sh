#!/bin/bash
set +e

workdir=$1
codehubTargetBranch=$2
codehubTargetRepoHttpUrl=$3
codehubSourceRepoHttpUrl=$4
codehubSourceBranch=$5
build_type=${6:-release}
mergerequestid=${7:-0}
CODEHUB_CODE_PATH=${workdir}/mysql-server

source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
# Update code
echo "---------------------- Update code --------------------------------"
cd ${workdir}/mysql-server/local_ci/project_script/common
sh update_code.sh $workdir $codehubTargetBranch $codehubTargetRepoHttpUrl $codehubSourceRepoHttpUrl $codehubSourceBranch
cd ${workdir}/mysql-server/local_ci/project_script/common
sh down_code_dstore.sh -m ${mergerequestid}

#start cde mtr ci
echo "---------------------- start mtr gate ci--------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,clean 2>&1 | tee ci_build.log

exit_flag=0
if [ ! -f ${workdir}/mysql-server/local_ci/script/logs/compile.log ]; then
  exit_flag=1
elif [ -n "`grep "Compile failed" ${workdir}/mysql-server/local_ci/script/logs/compile.log`" ]; then
  exit_flag=1
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_build.log

exit ${exit_flag}

