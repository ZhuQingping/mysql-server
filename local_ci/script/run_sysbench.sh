#!/bin/bash
source ./common_conf.sh
readonly DATABASE="sysbench"
readonly TABLE="test_table"
readonly ENGINE="Dstore"
sysbench_path=/opt/tools/sysbench-1.0.18/src
# MySQL命令
readonly MYSQL_CMD="${install_path}/mysql/bin/mysql -h$MYSQL_HOST -P$MYSQL_PORT -u$MYSQL_USER -p$MYSQL_PASSWD"
readonly CREATE_CONN="$MYSQL_CMD -e"

init_database() {
    echo -e "\033[32m[CI_INFO]: Creating a Test Environment...\033[0m"
    $CREATE_CONN "DROP DATABASE IF EXISTS $DATABASE; \
        CREATE DATABASE $DATABASE; \
        CREATE TABLE $DATABASE.$TABLE ( \
            id INT PRIMARY KEY, \
            a INT \
        ) ENGINE=$ENGINE; \
        SET GLOBAL max_prepared_stmt_count = 1048576; \
        SET GLOBAL max_connections = 2000;"
}

init_database

echo -e "\033[32m[CI_INFO]: Importing Data \033[0m"
echo "$sysbench_path/sysbench --db-driver=mysql --mysql-host=${MYSQL_HOST}  --mysql-port=${MYSQL_PORT} --mysql-user=${MYSQL_USER} --mysql-password=${MYSQL_PASSWD} --mysql-db=sysbench --table_size=10000 --tables=250 --time=300 --threads=64 --report-interval=1 --percentile=95 --events=0 --range_selects=0 --skip-trx=1  $sysbench_path/lua/oltp_read_write.lua prepare"
$sysbench_path/sysbench --db-driver=mysql --mysql-host=${MYSQL_HOST}  --mysql-port=${MYSQL_PORT} --mysql-user=${MYSQL_USER} --mysql-password=${MYSQL_PASSWD} --mysql-db=sysbench --table_size=10000 --tables=250 --time=300 --threads=64 --report-interval=1 --percentile=95 --events=0 --range_selects=0 --skip-trx=1  $sysbench_path/lua/oltp_read_write.lua prepare
echo -e "\033[32m[CI_INFO]: Test the mixed read/write performance \033[0m"
echo "$sysbench_path/sysbench --db-driver=mysql --mysql-host=${MYSQL_HOST}  --mysql-port=${MYSQL_PORT} --mysql-user=${MYSQL_USER} --mysql-password=${MYSQL_PASSWD} --mysql-db=sysbench --table_size=10000 --tables=250 --time=100 --threads=32 --report-interval=1 --percentile=95 --mysql-ignore-errors=1062  $sysbench_path/lua/oltp_read_write.lua run"
$sysbench_path/sysbench --db-driver=mysql --mysql-host=${MYSQL_HOST}  --mysql-port=${MYSQL_PORT} --mysql-user=${MYSQL_USER} --mysql-password=${MYSQL_PASSWD} --mysql-db=sysbench --table_size=10000 --tables=250 --time=100 --threads=32 --report-interval=1 --percentile=95 --mysql-ignore-errors=1062  $sysbench_path/lua/oltp_read_write.lua run