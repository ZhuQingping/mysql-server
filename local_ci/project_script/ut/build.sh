#!/bin/bash
set +e

workdir=$1
build_type="${2:-release}"
CODEHUB_CODE_PATH=${workdir}/mysql-server
# Add build_type similar to how it is implemented in local_ci/script/start_ci.sh

source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh
#start ut
echo "---------------------- start build run ut --------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,run_ut,clean 2>&1 | tee ci_ut.log

exit_flag=1
if [ -f ${workdir}/mysql-server/local_ci/script/logs/run_ut.log ]; then
  if [ -n "`grep "run ut succeed" ${workdir}/mysql-server/local_ci/script/logs/run_ut.log`" ]; then
    exit_flag=0
  fi
fi

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_ut.log
exit ${exit_flag}

