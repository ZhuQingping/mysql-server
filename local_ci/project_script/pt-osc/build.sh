#!/bin/bash
set +e

workdir=$1
build_type=${2:-debug}
test_type=${3:-run_pt_osc}
source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
sh ${workdir}/mysql-server/local_ci/project_script/common/down_code_dstore.sh
#start mtr
echo "---------------------- start mtr --------------------------------"
cd ${workdir}/mysql-server/local_ci/script
sh start_ci.sh -t ${build_type} -o no_format,${test_type} 2>&1 | tee ci_pt_osc.log

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

