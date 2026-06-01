#!\bin\bash

# Prepare hosts
build_type=$1
asan_option=$2
with_cov=${3:-0}
with_dstore=${4:-1}
with_pgo=${5:-0}
with_lto=${6:-0}
with_bolt=${7:-0}
train_sysbench=${8:-1}
train_tpcc=${9:-1}
train_mtr=${10:-1}
build_pagedump_waldump=${11:-0}

source ./common_conf.sh
if [ "$asan_option"x == "asan"x ]; then
  export ASAN_OPTIONS="detect_leaks=1:log_path=$log_dir/asan.log:verbosity=1:atexit=1:dedup_token_length=3"
  asan_option="--asan"
fi
if [ "$asan_option"x == "tsan"x ]; then
  asan_option="--tsan"
fi

cov_option=""
if [ "$with_cov"x == "1"x ]; then
  sh ${mysql_root_path}/local_ci/project_script/cov/lcovrc_conf.sh
  cov_option="--gcov"
fi
dstore_option=""
if [ "$with_dstore"x == "1"x ]; then
  dstore_option="--dstore"
  cd ${mysql_root_path}/local_ci/script
  sh build_utils.sh ${build_type}
fi
opt_option=()
if [[ "$with_pgo" == "1" ]];then
  opt_option+=("--pgo")
fi
if [[ "$with_lto" == "1" ]];then
  opt_option+=("--lto")
fi
if [[ "$with_bolt" == "1" ]];then
  opt_option+=("--bolt")
fi
train_option=()
if [[ "$train_sysbench" == "1" ]];then
  train_option+=("--pgo_sysbench")
fi
if [[ "$train_tpcc" == "1" ]];then
  train_option+=("--pgo_tpcc")
fi
if [[ "$train_mtr" == "1" ]];then
  train_option+=("--pgo_mtr")
fi
pagedump_waldump_option=""
if [[ "$build_pagedump_waldump" == "1" ]];then
  pagedump_waldump_option="--pagedumpWaldump"
fi

echo ${opt_option[@]}
echo ${train_option[@]}

cd ${mysql_root_path}
if [[ "${build_type}" = "debug" ]]; then
  sh +x ${mysql_root_path}/BUILD/build_debug.sh  --workdir=${mysql_root_path}/hwsql-builder --result=${mysql_root_path}/artis --install=${install_path}/mysql --jobs=$(nproc) --enable-install $asan_option $cov_option $dstore_option ${opt_option[@]} ${train_option[@]} $pagedump_waldump_option
else
  sh +x ${mysql_root_path}/BUILD/build_release.sh  --workdir=${mysql_root_path}/hwsql-builder --result=${mysql_root_path}/artis --install=${install_path}/mysql --jobs=$(nproc) --enable-install $asan_option $cov_option $dstore_option ${opt_option[@]} ${train_option[@]} $pagedump_waldump_option
fi
if [ $? -ne 0 ];then
  echo "Compile failed"
  exit 1
fi

if [ "$with_dstore"x == "1"x ]; then
  export LD_LIBRARY_PATH=$install_path/mysql/lib/dstore:$GCC/lib:$GCC/lib64:${dstore_root_path}/local_libs/openssl/lib:$LD_LIBRARY_PATH
fi
