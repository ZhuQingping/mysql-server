#!\bin\bash
build_type=$1
warehouses=$(cat tpcc_param | grep "warehouses" | awk -F '[=]' '{print $2}' | sed s/[[:space:]]//g)
running_time=$(cat tpcc_param | grep "running_time" | awk -F '[=]' '{print $2}' | sed s/[[:space:]]//g)
warmup_time=$(cat tpcc_param | grep "warmup_time" | awk -F '[=]' '{print $2}' | sed s/[[:space:]]//g)
connections=$(cat tpcc_param | grep "connections" | awk -F '[=]' '{print $2}' | sed s/[[:space:]]//g)
report_interval=$(cat tpcc_param | grep "report_interval" | awk -F '[=]' '{print $2}' | sed s/[[:space:]]//g)
tpcc_db=tpcc_test

source ./common_conf.sh
sudo chown ci:ci /tmp
source ./common_functions.sh

mysqld_options
start_mysqld
ping_mysqld

create_tpcc_table $tpcc_db
ln -s $run_path/mysql.sock /tmp/mysql.sock
# use run-machine env self tpcc tool
cd $mysql_root_path/tpcc
sh run_tpcc.sh $tpcc_db $warehouses $running_time $warmup_time $connections $report_interval
ret=$?
if [ $ret -ne 0 ];then
    echo "run tpcc fail."
    exit 1
fi
