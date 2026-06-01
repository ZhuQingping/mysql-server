#!/bin/bash

# These variables no need to change
mysql_root_path=$(dirname $(dirname $(dirname `readlink -f "$0"`)))
workdir=$(dirname $(dirname $(dirname $(dirname `readlink -f "$0"`))))
dstore_root_path=${workdir}/dstore
user_name=$USER
SHM_SIZE='40G'
base_path="/home/ci"
run_path="/home/ci/run"
install_path="/home/ci/install"

make_flag="true"
build_dstore=0
format_flag="true"
clean_flag="false"
coverage_flag="false"
mtr_flag="false"
build_type="relwithdebinfo"
root_docker_id=""
asan_option=0
with_cov=0
with_dstore=1
mtr_with_pkg=0
tpcc_flag="false"
mtr_type=""
tpch_explain_flag="false"
use_innodb_engine=0
platform=`arch`
start_port=8000
max_port=65535
# Define the long options
LONGOPTS="docker_id:,build_type:,options:,run_tpch:,run_tpch_explain:"

PARSED=$(getopt --options=d:t:o:p: --longoptions="$LONGOPTS" --name "$0" -- "$@")
echo "$PARSED"
eval set -- "$PARSED"

while true; do
    case "$1" in
        -d|--docker_id)
            root_docker_id="$2"
            shift 2
            ;;
        -t|--build_type)
            build_type="$2"
            shift 2
            ;;
        -o|--options)
            options="$2"
            shift 2
            ;;
        --run_tpch)
            tpch_scale="$2"
            tpch_flag="true"
            shift 2
            ;;
        --run_tpch_explain)
            use_innodb_engine="$2"
            tpch_explain_flag="true"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "Unknown parameter passed: $1"
            exit 1
            ;;
    esac
done

# No need to change unless necessary
#if [ "${user_name}"x == "root"x ]; then
#  echo "Use a non-root user."
#  exit 1
#fi
cd ${mysql_root_path}
echo -n -e "\033[32m[CI_INFO]:mysql_root_path:${mysql_root_path} "&& date "+%Y-%m-%d %H:%M:%S"

log_dir=$mysql_root_path/local_ci/script/logs
mkdir -p $log_dir
chown -R $user_name:$user_name $log_dir

function complie_build()
{
  local build_type=$1
  local need_clean=$2
  echo -n -e "\033[32m[CI_INFO]: Complie and build, build_type:$build_type, asan_option:$asan_option, need_clean:$need_clean, start time: \033[0m " && date "+%Y-%m-%d %H:%M:%S"
  if [ "$need_clean"x == "true"x ]; then
    if [ -d ${mysql_root_path}/hwsql-builder ]; then
      rm -rf ${mysql_root_path}/hwsql-builder
    fi
  fi
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh build_package.sh ${build_type} ${asan_option} ${with_cov} ${with_dstore}" 2>&1 | tee $log_dir/compile.log
  if [ -n "`grep "Compile failed" $log_dir/compile.log`" ]; then
    echo "stop local_ci"
    exit 1
  fi
}

function complie_build_dstore()
{
  local build_type=$1
  local need_clean=$2
  echo -n -e "\033[32m[CI_INFO]: Complie and build dstore, build_type:$build_type, need_clean:$need_clean, start time: \033[0m " && date "+%Y-%m-%d %H:%M:%S"
  if [ "$need_clean"x == "true"x ]; then
    if [ -d ${dstore_root_path}/tmp_build ]; then
      rm -rf ${dstore_root_path}/tmp_build
    fi
  fi
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh build_dstore.sh ${build_type} ${with_cov}" 2>&1 | tee $log_dir/dstore_compile.log
  if [ -n "`grep "Compile failed" $log_dir/dstore_compile.log`" ]; then
    echo "stop local_ci"
    exit 1
  fi
}

function find_available_port()
{
  local start=$1
  local port=$start
  while [ $port -le $max_port ]; do
    if ! (echo > /dev/tcp/127.0.0.1/$port 2>/dev/null) 2>/dev/null; then
      echo "$port"
      return 0
    fi
    port=$((port + 1))
  done
  return 1
}

function start_docker()
{
  # Start a new docker container
  local seq_str=$1
  local date_str=`date '+%Y%m%d_%H%M%S'`
  local docker_name="${user_name}_${date_str}"_"${seq_str}"
  echo "platform=$platform"
  if [ "$platform" == "aarch64" ]; then
    dockerfile=Dockerfile_arm
  elif [ "$platform" == "x86_64" ]; then
    dockerfile=Dockerfile
  fi
  # Check if docker image exist
  image_tag=$(grep "fuxi_build_image:" ${dockerfile} | awk -F ":" '{print $2}')
  image_name=local_ci_${image_tag}_${user_name}
  image_id=`docker images | grep -w "$image_name" | awk '{print $3}'`
  if [ -z $image_id ]; then
    echo -e "\033[32m[CI_INFO]: docker image is not exist, start build image\033[0m"
    echo -e "\033[32m[CI_INFO]: docker build --build-arg UID=$(id -u ${user_name}) --build-arg GID=$(id -g ${user_name}) --build-arg MYSQL_ROOT_PATH=${mysql_root_path}  --build-arg DSTORE_ROOT_PATH=${dstore_root_path} -t $image_name\033[0m"
    docker build -f ${dockerfile} --build-arg UID=$(id -u ${user_name}) --build-arg GID=$(id -g ${user_name}) --build-arg MYSQL_ROOT_PATH=${mysql_root_path} --build-arg DSTORE_ROOT_PATH=${dstore_root_path} -t $image_name .
    image_id=`docker images | grep -w "$image_name" | awk '{print $3}'`
  fi
  echo -e "\033[32m[CI_INFO]: image_id=${image_id}\033[0m"
  export OBS_AK=$([ -f /opt/obs_ak.txt ] && cat /opt/obs_ak.txt)
  export OBS_SK=$([ -f /opt/obs_sk.txt ] && cat /opt/obs_sk.txt)
  ci_port=$(find_available_port $start_port)
  if [ $? -ne 0 ]; then
    echo "Error: No available port found for ci_port between $start_port and $max_port" >&2
    exit 1
  fi
  ai_port=$(find_available_port $((ci_port + 1)))
  if [ $? -ne 0 ]; then
    echo "Error: No available port found for ai_port between $((ci_port + 1)) and $max_port" >&2
    exit 1
  fi
  docker run -h localhost -it -d -p ${ci_port}:22 -p ${ai_port}:8080  --shm-size=${SHM_SIZE} -e OBS_AK -e OBS_SK -v /var/crash/coredump:/var/crash/coredump -v $mysql_root_path:${mysql_root_path} -v $dstore_root_path:${dstore_root_path} \
            --name $docker_name --privileged $image_id > /dev/null

  new_docker_id=`docker ps | grep $docker_name | awk '{print $1}'`
  new_docker_id=${new_docker_id:0:12}
  docker exec $new_docker_id bash -c "mkdir -p $install_path"
  docker exec $new_docker_id bash -c "sudo chmod 777 /var/log -R; sudo chmod 777 /lib64"
  if [ "$platform" == "x86_64" ]; then
    docker exec $new_docker_id bash -c "sudo mount -t tmpfs -o size=8G tmpfs /tmp"
  fi
  docker exec $new_docker_id bash -c "sudo chown -R ci:ci /sys/fs/cgroup/cpu,cpuacct"
  docker exec $new_docker_id bash -c "mkdir /home/ci/.claude/; cp /opt/tools/settings.json /home/ci/.claude/"
  docker exec $new_docker_id bash -c "mkdir /home/ci/.claude-code-router/; cp /opt/tools/config.json /home/ci/.claude-code-router/"
  docker exec $new_docker_id bash -c "cp /opt/tools/.gdbinit /home/ci/"
  docker exec $new_docker_id bash -c "cp /opt/tools/mysql8_init.sql /home/ci/"
  docker exec $new_docker_id bash -c "echo '10.155.120.249 api.anthropic.rnd.huawei.com' | sudo tee -a /etc/hosts"
  docker exec $new_docker_id bash -c "echo '100.125.2.10 obs.cn-southwest-244.ulanqab.huawei.com' | sudo tee -a /etc/hosts"
  docker exec $new_docker_id bash -c "echo '100.125.1.4 obs.cn-southwest-244.myhuaweicloud.com' | sudo tee -a /etc/hosts"
  docker exec $new_docker_id bash -c "echo 'nameserver 10.129.0.84' | sudo tee -a /etc/resolv.conf"
  echo -e "\033[32m[CI_INFO]: start a docker container. image_id: $image_id, docker id: ${new_docker_id}"
  echo -e "\033[32m[CI_INFO]: Port Mapping: ${ci_port}:22, ${ai_port}:8080\033[0m"
  # Copy gdbint in order to enable gdb print STL container
}


function parallel_run_test()
{
  echo -n -e "\033[32m[CI_INFO]: run all mrt test, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh run_mtr.sh ${build_type} ${mtr_type} ${asan_option}" 2>&1 | tee $log_dir/run_mtr.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: MTR test finish success!\033[0m"
  fi
}

function run_ut()
{
  echo -n -e "\033[32m[CI_INFO]: run all unittest, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh run_ut.sh ${build_type} ${build_dstore}" 2>&1 | tee $log_dir/run_ut.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: UT test finish success!\033[0m"
  fi
}

function run_tpcc()
{
  echo -n -e "\033[32m[CI_INFO]: run tpcc, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh run_tpcc.sh ${build_type}" 2>&1 | tee $log_dir/run_tpcc.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: TPCC test finish success!\033[0m"
  fi
}

function run_pt_osc()
{
  echo -n -e "\033[32m[CI_INFO]: run pt_osc, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh start_mysqld.sh -startmysql; sh run_pt_osc.sh" 2>&1 | tee $log_dir/run_pt_osc.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: RUN PT_OSC finish success!\033[0m"
  fi
}

function run_gh_ost()
{
  echo -n -e "\033[32m[CI_INFO]: run gh_ost, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh start_mysqld.sh -startmysql; sh run_ghost.sh" 2>&1 | tee $log_dir/run_ghost.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: RUN GHOST finish success!\033[0m"
  fi
}

function run_sysbench()
{
  echo -n -e "\033[32m[CI_INFO]: run sysbench, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh start_mysqld.sh -startmysql; sh run_sysbench.sh" 2>&1 | tee $log_dir/run_sysbench.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: RUN SYSBENCH finish success!\033[0m"
  fi
}

function run_tpch()
{
  local scale_factor=$1
  echo -n -e "\033[32m[CI_INFO]: run tpch ${scale_factor}G, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; ./run_tpch.sh ${scale_factor}" 2>&1 | tee $log_dir/run_tpch_${scale_factor}G.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: TPCH ${scale_factor}G test finish success!\033[0m"
  fi
}

function run_tpch_explain()
{
  local use_engine=$1
  echo -n -e "\033[32m[CI_INFO]: run tpch explain 1G, engine ${use_engine}, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path}/local_ci/script; sh run_tpch_explain.sh ${build_type} ${use_innodb_engine}" 2>&1 | tee $log_dir/run_tpch_explain_${use_innodb_engine}.log
  if [ $? -eq 0 ]; then
    echo -e "\033[32m[CI_INFO]: TPCH explain test finish success!\033[0m"
  fi
}

function gen_cov_report()
{
  echo -n -e "\033[32m[CI_INFO]: run gen_cov_report, start time: \033[0m" && date "+%Y-%m-%d %H:%M:%S"
  docker exec $root_docker_id bash -c "cd ${mysql_root_path};\
      lcov --rc lcov_branch_coverage=1 -c --directory ${mysql_root_path}/hwsql-builder/build/ --output-file mysql_cov.info --ignore-errors gcov,source,graph --gcov-tool /opt/hw/gcc-10.3/bin/gcov;\
      lcov --rc lcov_branch_coverage=1 -r mysql_cov.info '*/gcc-10.3/*' '*boost/*' '*extra/*' '*storage/ndb/*' '/usr/include/*' '*/unittest/*' '*/build/*' '*/dstore/*' '*/router/src/routing/src/*' '*/testclients/*' -o mysql_cov.info;\
      genhtml mysql_cov.info --rc genhtml_branch_coverage=1 --output-directory mysql_cov_report --ignore-errors source;\
      tar zcf mysql_cov_report.tar.gz mysql_cov_report *.info;\
      lcov --rc lcov_branch_coverage=1 --summary mysql_cov.info | tee ${mysql_root_path}/cov_summary.txt"
  if [ "$build_dstore"x == "1"x ]; then
    docker exec $root_docker_id bash -c "cd ${mysql_root_path};\
        lcov --rc lcov_branch_coverage=1 -c --directory ${mysql_root_path}/hwsql-builder/build/storage/dstore/CMakeFiles/cde.dir/${dstore_root_path}/dstore/ --output-file dstore_cov_mtr.info --exclude '*/distribute/*' --exclude '*/gcc-10.3/*' --ignore-errors gcov,source,graph --gcov-tool /opt/hw/gcc-10.3/bin/gcov;\
        lcov --rc lcov_branch_coverage=1 -c --directory ${dstore_root_path}/dstore/ --output-file dstore_cov_ut.info --exclude '*/distribute/*' --exclude '*/gcc-10.3/*' --ignore-errors gcov,source,graph --gcov-tool ${dstore_root_path}/GaussDBKernel-third_party_binarylibs/euleros2.5_x86_64/buildtools/gcc7.3/gcc/bin/gcov;\
        lcov --rc lcov_branch_coverage=1 -a dstore_cov_mtr.info -a dstore_cov_ut.info -o dstore_cov.info;\
        genhtml dstore_cov.info --rc genhtml_branch_coverage=1 --output-directory dstore_cov_report --ignore-errors source;\
        tar zcf dstore_cov_report.tar.gz dstore_cov_report *.info;\
        lcov --rc lcov_branch_coverage=1 --summary dstore_cov.info | tee -a ${mysql_root_path}/cov_summary.txt;\
        mkdir cov_report; cp -rf dstore_cov_report cov_report/; cp -rf mysql_cov_report cov_report/"
  fi
}

# Start processing
cd $mysql_root_path/local_ci/script

# Parse options
echo  -e "\033[32m[CI_INFO]: Start ci with options: $options \033[0m"
if [ -n $options ];then
  if [ -n "`echo $options | grep -w "no_build"`" ];then
    make_flag="false"
  fi

  if [ -n "`echo $options | grep -w "no_format"`" ];then
    format_flag="false"
  fi
  
  if [ -n "`echo $options | grep -w "clean"`" ];then
    clean_flag="true"
  fi
  
  if [ -n "`echo $options | grep -w "run_mtr"`" ];then
    mtr_flag="true"
    mtr_type="mtr_at"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_pkg"`" ];then
    mtr_flag="true"
    mtr_type="mtr_pkg"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_gate"`" ];then
    mtr_flag="true"
    mtr_type="mtr_gate"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_innodb_gate1"`" ];then
    mtr_flag="true"
    mtr_type="mtr_innodb_gate1"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_innodb_gate2"`" ];then
    mtr_flag="true"
    mtr_type="mtr_innodb_gate2"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_innodb_gate3"`" ];then
    mtr_flag="true"
    mtr_type="mtr_innodb_gate3"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_innodb_gate4"`" ];then
    mtr_flag="true"
    mtr_type="mtr_innodb_gate4"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_AT_innodb"`" ];then
    mtr_flag="true"
    mtr_type="mtr_AT_innodb"
  fi

  if [ -n "`echo $options | grep -w "run_mtr_AT_innodb_pq"`" ];then
    mtr_flag="true"
    mtr_type="mtr_AT_innodb_pq"
  fi

  if [ -n "`echo $options | grep -w "run_cov"`" ];then
    mtr_flag="true"
    mtr_type="mtr_at"
    ut_flag="true"
    with_cov=1
  fi
    
  if [ -n "`echo $options | grep -w "run_ut"`" ];then
    ut_flag="true"
  fi

  if [ -n "`echo $options | grep -w "run_tpcc"`" ];then
    tpcc_flag="true"
  fi

  if [ -n "`echo $options | grep -w "run_pt_osc"`" ];then
    pt_osc_flag="true"
  fi

  if [ -n "`echo $options | grep -w "run_ghost"`" ];then
    ghost_flag="true"
  fi

  if [ -n "`echo $options | grep -w "run_sysbench"`" ];then
    sysbench_flag="true"
  fi
fi

if [ "$build_type"x == "asan"x ]; then
  # todo use release
  build_type=debug
  asan_option=asan
fi

if [ "$build_type"x == "tsan"x ]; then
  # todo use release
  build_type=release
  asan_option=tsan
fi

if [ "$build_type"x != "release"x ] && [ "$build_type"x != "debug"x ] && [ "$build_type"x != "relwithdebinfo"x ]; then
  echo "\033[32m[CI_INFO]: Unknown compilation type!\033[0m"
  exit 1
fi

if [ "$root_docker_id"x == x ]; then
  start_docker 0
  root_docker_id=$new_docker_id
fi

# Check format
if [ "$format_flag"x == "true"x ]; then
   docker exec $root_docker_id bash -c "cd ${mysql_root_path}/src/sql; make check_format" 2>&1 > ${log_dir}/format.log
   error_occur=`grep "before formatting" ${log_dir}/format.log| wc -l`
   if [ $error_occur != 0 ]; then
     echo -e "\033[31m[CI_ERROR]: check format failed, see ${log_dir}/format.log for more info.\033[0m"
     exit 1
   fi
   rm -f ${log_dir}/format.log
fi

if [ "$make_flag"x == "true"x ]; then
  # simulator no need to complie release version
  complie_build $build_type $clean_flag
fi

if [ "$build_dstore"x == "1"x ]; then
  # simulator no need to complie release version
  complie_build_dstore $build_type $clean_flag
fi

if [ "$mtr_flag"x == "true"x ]; then
  # simulator no need to complie release version
  parallel_run_test $build_type
fi

if [ "$ut_flag"x == "true"x ]; then
  # simulator no need to complie release version
  run_ut $build_type $build_dstore
fi

if [ "$tpcc_flag"x == "true"x ]; then
  # simulator no need to complie release version
  run_tpcc $build_type
fi

if [ "$tpch_flag"x == "true"x ]; then
  run_tpch $tpch_scale
fi

if [ "$tpch_explain_flag"x == "true"x ]; then
  run_tpch_explain $use_innodb_engine
fi

if [ "$with_cov"x == "1"x ]; then
  gen_cov_report
fi

if [ "${pt_osc_flag}"x == "true"x ]; then
  # simulator no need to complie release version
  run_pt_osc $build_type
fi

if [ "${ghost_flag}"x == "true"x ]; then
  run_gh_ost $build_type
fi

if [ "${sysbench_flag}"x == "true"x ]; then
  # simulator no need to complie release version
  run_sysbench $build_type
fi
