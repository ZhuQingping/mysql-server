#!\bin\bash
build_type=$1
use_innodb_engine=$2

echo "use_innodb_engine:"$use_innodb_engine

source ./common_conf.sh
export MYSQL_BIN_DIR=$install_path/mysql/bin

source ./common_functions.sh

mysqld_options_loadfile "/home/ci/mysql-server/TPCH_Tool/dbgen"
start_mysqld
ping_mysqld

load_tpch_data $use_innodb_engine
run_tpch_explain_sql $use_innodb_engine
