#!/bin/bash
set +e

workdir=$1
build_type=${2:-debug}
mtr_type=${3:-run_mtr}
source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh
#start mtr
echo "---------------------- start mtr --------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,${mtr_type} 2>&1 | tee ci_mtr.log

echo "---------------------- Check build results ---------------------------"
exit_flag=1
if [ ! -f ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log ]; then
  exit_flag=1
elif [ -z "`grep "==ERROR" ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log`"  ]; then
  exit_flag=0
  echo "===============NO ERROR================="
else
  echo "==============Stack Analysis====================="
  cd ${workdir}/mysql-server/local_ci/project_script/asan
  sh filtering_error.sh ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log 2>&1 | tee ${workdir}/mysql-server/local_ci/script/logs/asan_report
  if [ -z "`grep "==ERROR" ${workdir}/mysql-server/local_ci/script/logs/asan_report`"  ]; then
    exit_flag=0
    echo "===============NO ERROR================="
  fi
fi

# Generated result.xml
output_report ${workdir}/mysql-server/local_ci/script/logs/run_mtr.log ${workdir}/mysql-server/local_ci/script/logs

# Cleaning up the container
rm_docker ${workdir}/mysql-server/local_ci/script/ci_mtr.log
exit ${exit_flag}

