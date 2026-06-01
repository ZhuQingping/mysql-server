#!/bin/bash

include=""
exclude="*/Dist/*,*/build/*,*/cmake/*,*/extra/*,*/mysql-test/*,*/packing/*,*/unittest/*,*/dstore/ut/src/*"

cd ~

local_lcov=".lcovrc"

if [ -f "/etc/lcovrc" ]; then
  \cp /etc/lcovrc ${local_lcov}
elif [ -f "/usr/local/etc/lcovrc" ]; then
  \cp /usr/local/etc/lcovrc ${local_lcov}
else
  touch ${local_lcov}
fi

function replace_lcovrc()
{
  key="$1"
  value="$2"
  line_count=`grep "^${key}" ${local_lcov}`
  if [ "$line_count" = "" ]; then
    echo "$key = $value" >> ${local_lcov}
  else
    sed -i "s/^$key.*$/$key = $value/g" ${local_lcov}
  fi
}

replace_lcovrc geninfo_no_exception_branch 1
echo "FuxiCovOrigin=${FuxiCovOrigin}"
if [ "$FuxiCovOrigin" != "Pipeline" ]; then
  ls -l $WORKSPACE/
  cat $WORKSPACE/webHookMessageForFuxiCov.txt
  MR_JSON=`cat $WORKSPACE/webHookMessageForFuxiCov.txt`
  MR_JSON=${MR_JSON:1}
  MR_JSON=${MR_JSON%\"}
  which jq
  echo $MR_JSON | jq "."
  echo $MR_JSON | jq ".object_attributes.description"
  
  cd $WORKSPACE
  files=$(git diff --diff-filter=AM --name-only origin/${TARGET_BRANCH} HEAD)
  if [[ -n $files ]]; then
      for j in ${files[@]};do
          echo $WORKSPACE/$j >> $WORKSPACE/merge_git_diff
          echo "增量文件："$j
      done
  fi
  echo "+++++++++++++++ merge_git_diff ++++++++++++++++"
  cat merge_git_diff
  
  while read element
  do
    echo ${element} | awk -F '/' '{print $NF}' >> $WORKSPACE/change_files
  done < $WORKSPACE/merge_git_diff
  echo "+++++++++++++++ merge_git_diff end++++++++++++++++"
fi
env
whoami
git config --global --list
netstat -anp | grep LISTEN
if [ "$FuxiCovOrigin" != "Pipeline" ]; then
  MR_DESC=`echo $MR_JSON | jq ".object_attributes.description"`
  echo -e $MR_DESC'\n'
  echo $MR_DESC | grep '#dstore_branch='
  if [ $? -eq 0 ]; then
    IFS='#'; arrIN=($MR_DESC); unset IFS;
    for i in "${arrIN[@]}"; do
      echo $i | grep dstore_branch=
      if [ $? -eq 0 ]; then
        dstore_branch=${i//dstore_branch=/}
        echo "dstore_branch:" $dstore_branch
        sh $WORKSPACE/local_ci/project_script/common/download_cmc.sh ${dstore_branch}/dstore
      fi
    done
  else
    sh $WORKSPACE/local_ci/project_script/common/download_cmc.sh
  fi
fi

# Download Huawei OBS codes.
echo -e "\033[32m[OBS] Update Huawei OBS code.\033[0m"
sh $WORKSPACE/local_ci/project_script/common/download_obs.sh

cd $WORKSPACE/local_ci/script
sh build_package.sh debug 0 1 0

sed -i 's|build_path|install_path/mysql|g' run_mtr.sh
# UT
cd $WORKSPACE/local_ci/script
sh run_ut.sh debug

platform=`arch`
obs_dep_arch="linux"
if [ "$platform" == "aarch64" ]; then
  obs_dep_arch="arm"
fi
cp $WORKSPACE/../dstore/huaweicloud-sdk-c-obs/build/script/Provider/build/${obs_dep_arch}/openssl-1.1.1w/lib/libssl.so.1.1 /home/ci/install/mysql/lib/
cp $WORKSPACE/../dstore/huaweicloud-sdk-c-obs/build/script/Provider/build/${obs_dep_arch}/openssl-1.1.1w/lib/libcrypto.so.1.1 /home/ci/install/mysql/lib/

# MTR
cd $WORKSPACE/
df -h
free -h
if [ "$FuxiCovOrigin" != "Pipeline" ]; then
  tests=()
  innodb_tests=()
  suites=()
  innodb_suites=()
  echo $MR_DESC | grep '#FUXI_COV_MTR_TESTS=\|#FUXI_COV_MTR_SUITE=\|#FUXI_COV_MTR_INNODB_TESTS=\|#FUXI_COV_MTR_INNODB_SUITE=\|#FUXI_COV_MTR_PQ_TESTS=\|#FUXI_COV_MTR_PQ_SUITE='
  if [ $? -eq 0 ]; then
    IFS='#'; arrIN=($MR_DESC); unset IFS;
    for i in "${arrIN[@]}"; do
      echo $i | grep FUXI_COV_MTR_TESTS=
      if [ $? -eq 0 ]; then
        tests=${i//FUXI_COV_MTR_TESTS=/}
        tests=(${tests//,/ })
        echo "FUXI_COV_MTR_TESTS:" ${#tests[@]}
        for t in ${tests[@]}; do
          echo $t >> /home/ci/install/mysql/mysql-test/tmp_test_list
        done
        cat /home/ci/install/mysql/mysql-test/tmp_test_list
      fi

      echo $i | grep FUXI_COV_MTR_INNODB_TESTS=
      if [ $? -eq 0 ]; then
        innodb_tests=${i//FUXI_COV_MTR_INNODB_TESTS=/}
        innodb_tests=(${innodb_tests//,/ })
        echo "FUXI_COV_MTR_INNODB_TESTS:" ${#innodb_tests[@]}
        for t in ${innodb_tests[@]}; do
          echo $t >> /home/ci/install/mysql/mysql-test/tmp_innodb_test_list
        done
        cat /home/ci/install/mysql/mysql-test/tmp_innodb_test_list
      fi

      echo $i | grep FUXI_COV_MTR_PQ_TESTS=
      if [ $? -eq 0 ]; then
        pq_tests=${i//FUXI_COV_MTR_PQ_TESTS=/}
        pq_tests=(${pq_tests//,/ })
        echo "FUXI_COV_MTR_PQ_TESTS:" ${#pq_tests[@]}
        for t in ${pq_tests[@]}; do
          echo $t >> /home/ci/install/mysql/mysql-test/tmp_pq_test_list
        done
        cat /home/ci/install/mysql/mysql-test/tmp_pq_test_list
      fi

      echo $i | grep FUXI_COV_MTR_SUITE=
      if [ $? -eq 0 ]; then
        suites=${i//FUXI_COV_MTR_SUITE=/}
        suites=(${suites//,/ })
        echo "FUXI_COV_MTR_SUITE:" ${#suites[@]}
      fi

      echo $i | grep FUXI_COV_MTR_INNODB_SUITE=
      if [ $? -eq 0 ]; then
        innodb_suites=${i//FUXI_COV_MTR_INNODB_SUITE=/}
        innodb_suites=(${innodb_suites//,/ })
        echo "FUXI_COV_MTR_INNODB_SUITE:" ${#innodb_suites[@]}
      fi

      echo $i | grep FUXI_COV_MTR_PQ_SUITE=
      if [ $? -eq 0 ]; then
        pq_suites=${i//FUXI_COV_MTR_PQ_SUITE=/}
        pq_suites=(${pq_suites//,/ })
        echo "FUXI_COV_MTR_INNODB_SUITE:" ${#pq_suites[@]}
      fi
    done
  fi

  export LD_LIBRARY_PATH=/home/ci/install/mysql/lib:$LD_LIBRARY_PATH

  #sh run_mtr.sh
  if [ ${#tests[@]} -gt 0 ]; then
    cd $WORKSPACE/local_ci/script
    sh run_mtr.sh debug mtr_cov 0 
  fi

  if [ ${#innodb_tests[@]} -gt 0 ]; then
    cd $WORKSPACE/local_ci/script
    sh run_mtr.sh debug mtr_cov_innodb 0
  fi

  if [ ${#pq_tests[@]} -gt 0 ]; then
    cd $WORKSPACE/local_ci/script
    sh run_mtr.sh debug mtr_cov_pq 0
  fi

  if [ ${#suites[@]} -gt 0 ]; then
    for s in ${suites[@]}; do
      cd $WORKSPACE/local_ci/script
      sh run_mtr.sh debug mtr_cov 0 ${s}
    done
  fi

  if [ ${#innodb_suites[@]} -gt 0 ]; then
    for s in ${innodb_suites[@]}; do
      cd $WORKSPACE/local_ci/script
      sh run_mtr.sh debug mtr_cov_innodb 0 ${s}
    done
  fi

  if [ ${#pq_suites[@]} -gt 0 ]; then
    for s in ${pq_suites[@]}; do
      cd $WORKSPACE/local_ci/script
      sh run_mtr.sh debug mtr_cov_pq 0 ${s}
    done
  fi

  # Remove useless .gcno and .gcda files
  cd $WORKSPACE/
  echo "mysqld.cc" >> $WORKSPACE/change_files
  echo "++++++++change_files++++++++++++++"
  cat $WORKSPACE/change_files
  find -name *.gcno | grep -vf $WORKSPACE/change_files | xargs rm -f
  find -name *.gcda | grep -vf $WORKSPACE/change_files | xargs rm -f
else
  cd $WORKSPACE/local_ci/script
  sh run_mtr.sh debug mtr_at 0
fi
# Generate coverage report
cd $WORKSPACE/
rm -rf report_cppcoco
mkdir report_cppcoco
cd report_cppcoco
curl -k -O http://100.95.189.218:80/droplet-engine2/obs/droplet-cppcoco/droplet-cppcoco.sh
export PATH=/opt/hw/gcc-10.3/bin:$PATH
sed -i "s/curl -k -O https/curl -k -O http/g" droplet-cppcoco.sh
sh droplet-cppcoco.sh -p $WORKSPACE -i "$include" -e "$exclude"

