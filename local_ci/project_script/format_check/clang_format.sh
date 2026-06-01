#!/bin/bash
set +e

echo "hello clang format"

workdir=$1
codehubTargetBranch=$2
codehubTargetRepoHttpUrl=$3
codehubSourceRepoHttpUrl=$4
codehubSourceBranch=$5

source ${workdir}/mysql-server/local_ci/project_script/common/common_fun.sh
# Update code
echo "---------------------- Update code --------------------------------"
cd ${workdir}/mysql-server/local_ci/project_script/common
sh update_code.sh $workdir $codehubTargetBranch $codehubTargetRepoHttpUrl $codehubSourceRepoHttpUrl $codehubSourceBranch

echo "---------------------- start clang format check --------------------------------"
CODEHUB_CODE_PATH=${workdir}/mysql-server
exclude_check_dir=""

cd $CODEHUB_CODE_PATH
chmod u+x format.py
chmod u+x clang_check -R
echo "check code:"$CODEHUB_CODE_PATH
echo "exclude_check_dir:"$exclude_check_dir
exit_flag=0
if [ "$(git diff --diff-filter=AM --name-only origin/$codehubTargetBranch HEAD | grep -E '\.(h|cc|hpp|cpp)$' | grep -v 'storage\/dstore\/dstore')"x != ""x ]; then
  check_command="git diff --diff-filter=AM --name-only origin/$codehubTargetBranch HEAD | grep -E '\.(h|cc|hpp|cpp)$' | grep -v 'storage\/dstore\/dstore' | xargs python3 $CODEHUB_CODE_PATH/format.py"
  echo "check_command:"$check_command
  out_result=$(bash -c "$check_command" 2>&1)
  
  if [[ -n $out_result ]]; then
      echo "######## CHECKING CLANG-FORMAT #########"
      echo "ERROR. The following files have clang-format problems:"
      echo ""
      echo $"$out_result"
      echo ""
      echo "------------------------------------------------------"
      echo "Please run python format.py -fix to fix"
      exit_flag=1
  fi
fi
exit ${exit_flag}
