#!/bin/bash
# These are configurations in docker containers

# 1. The path
mysql_root_path=$(dirname $(dirname $(dirname `readlink -f "$0"`)))
dstore_root_path=${mysql_root_path}/../dstore
build_path=$mysql_root_path/hwsql-builder/build
install_path=/home/ci/install
run_path=/home/ci/run
datafile_path=/home/ci/datafile
log_dir=$mysql_root_path/local_ci/script/logs

# 2. Judging platform
platform=`arch`
echo "platform=$platform"
if [ "$platform" == "aarch64" ]; then
  dstore_platform=euleros2.9_aarch64
elif [ "$platform" == "x86_64" ]; then
  dstore_platform=euleros2.5_x86_64
fi

# 3. Create path if necessary
if [ ! -d $run_path ];then
  echo "create workspace directory: $run_path"
  mkdir -p $run_path
fi

# 4. Global configuration
MYSQL_PORT=3306
MYSQLX_PORT=59999
MYSQL_PASSWD="123456"
MYSQL_HOST="127.0.0.1"
MYSQL_USER="root"

# 5. Environment variable
if [ -f /etc/os-release ]; then
  . /etc/os-release
  OS_NAME="$NAME"
  OS_VERSION="$VERSION_ID"
  echo "Operating system: $OS_NAME $OS_VERSION"
else
  echo "The operating system cannot be identified."
fi
if [ "$OS_NAME" == "EulerOS" ]; then
  GCC=/opt/hw/gcc-10.3
fi

# No need to specify thirdparty libraries, all dependent libraries can be found by rpath
export LD_LIBRARY_PATH=$GCC/lib:$GCC/lib64:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$build_path/library_output_directory:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$install_path/mysql/lib/dstore:$install_path/mysql/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=${dstore_root_path}/local_libs/openssl/lib:$LD_LIBRARY_PATH
export PATH=$GCC/bin:$PATH
export TMOUT=0

#run
export PERL5LIB=$install_path/mysql/mysql-test/lib:$PERL5LIB

# 6. Chmod every time to avoid someone else change them
sudo chmod 777 /var/log -R
