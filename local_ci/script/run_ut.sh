#!\bin\bash
build_type=$1
build_dstore=${2:-0}

source ./common_conf.sh

echo -n -e "\033[32m[CI_INFO]: run mysql ut, build_type:$build_type, start time: \033[0m " && date "+%Y-%m-%d %H:%M:%S"
cd $build_path/unittest
make test-unit
mysql_ret=$?

TESTS=(
    "make run_dstore_local_backup_unittest"
    "make run_dstore_undo_unittest"
    "make run_dstore_buffer_unittest"
    "make run_dstore_ha_unittest"
    "make run_dstore_datamanager_unittest"
    "make run_dstore_index_unittest"
    "make run_dstore_xact_unittest"
    "make run_dstore_lock_unittest"
)

if [ "$build_dstore"x == "1"x ]; then
    export THIRD_BIN_PATH=${dstore_root_path}/GaussDBKernel-third_party_binarylibs/$PLATFORM
    export BUILD_TOOLS_PATH=$THIRD_BIN_PATH/buildtools
    export GCC_VERNAME=gcc7.3
    export GCC_VERSION=7.3.0
    export GCCFOLDER=$BUILD_TOOLS_PATH/$GCC_VERNAME
    export CC=$GCCFOLDER/gcc/bin/gcc
    export CXX=$GCCFOLDER/gcc/bin/g++
    export LD_LIBRARY_PATH=$GCCFOLDER/gcc/lib64:$GCCFOLDER/isl/lib:$GCCFOLDER/mpc/lib/:$GCCFOLDER/mpfr/lib/:$GCCFOLDER/gmp/lib/:$LD_LIBRARY_PATH
    export PATH=$GCCFOLDER/gcc/bin:$PATH
    echo -n -e "\033[32m[CI_INFO]: run dstore ut, build_type:$build_type, start time: \033[0m " && date "+%Y-%m-%d %H:%M:%S"
    cd ${dstore_root_path}/dstore/tmp_build
    for test in "${TESTS[@]}"; do
        for i in {1..3}; do
            echo "执行测试 ($i/3): $test"
            if $test; then
                echo "UT测试成功"
                break
            elif [ $i -eq 3 ]; then
                echo "UT测试失败，已达最大重试次数"
            else
                echo "测试失败，将重试"
                sleep 5
            fi
        done
    done
fi

if [ $mysql_ret -eq 0 ];then
    echo "run ut succeed."
else
    echo "run ut failed."
    exit 1
fi