#!/bin/bash
workdir=$1
codehubTargetBranch=$2
codehubTargetRepoHttpUrl=$3
codehubSourceRepoHttpUrl=$4
codehubSourceBranch=$5
CODEHUB_CODE_PATH=${workdir}/mysql-server
source ${CODEHUB_CODE_PATH}/local_ci/project_script/common/common_fun.sh
mysql_branch=$codehubTargetBranch

# update code
cd ${CODEHUB_CODE_PATH} && git clean -dfx
update_code ${CODEHUB_CODE_PATH} ${mysql_branch}
# merge code
cd ${CODEHUB_CODE_PATH}
merge_code