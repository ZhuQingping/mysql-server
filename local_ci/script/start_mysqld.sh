#!/bin/bash
source ./common_conf.sh
mysql_install_dir=$install_path/mysql  # notice: if mysql_install_dir not exist, dstore_package can't empty
# mysql config
mysql_data_dir=$install_path/mysql_data
mysql_cnf_file=$install_path/my.cnf
mysql_tmp_dir=$install_path/tmp
mysqld_socket=$install_path/mysql.sock
mysqlx_socket=$install_path/mysqlx.sock
mysql_port=3306
mysqlx_port=58888
mysql_log_error=$install_path/mysql.err
mysql_pid_file=$install_path/mysql.pid
mysql_user=root

# dstore config
dstore_home=$install_path/dstore
dstore_data_dir=$dstore_home/dstore_data
dstore_log_dir=$dstore_home/dstore_log
isoland_start_json=$dstore_home/tenant_isoland_start.json
dstore_guc_json=$dstore_home/guc.json
dstore_libvfslinuxadapter_so=$mysql_install_dir/lib/dstore/libvfslinuxadapter.so

# 配置文件模板
mysql_cnf_template=$(cat <<EOF
[client]
socket=${mysqld_socket}

[mysqld]
default_authentication_plugin=mysql_native_password
bind-address=0.0.0.0
port=${mysql_port}
mysqlx_port=${mysqlx_port}
user=${mysql_user}
basedir=${mysql_install_dir}
datadir=${mysql_data_dir}
tmpdir=${mysql_tmp_dir}
socket=${mysqld_socket}
mysqlx_socket=${mysqlx_socket}
# Disabling symbolic-links is recommended to prevent assorted security risks
character_set_server=utf8mb4
symbolic-links=0
explicit_defaults_for_timestamp=true
# Settings user and group are ignored when systemd is used.
# If you need to run mysqld under a different user or group,
# customize your systemd unit file for mariadb according to the
# instructions in http://fedoraproject.org/wiki/Systemd
dstore_log_path=${dstore_log_dir}
dstore_tenant_config=${isoland_start_json}
core-file
loose_innodb_buffer_pool_in_core_file=1
general_log=ON
general_log_file=$install_path/mysql_general_log.txt
rds_oma_log=ON
rds_oma_log_file=$install_path/mysql_oma_log.txt
plugin-load-add=audit_log.so
default_storage_engine=Dstore
default_tmp_storage_engine=Dstore
disabled_storage_engines=InnoDB,CSV,Memory,MyISAM,MRG_MyISAM
storage_engine_mode=ONLY_DSTORE
# Make audit log debug easier during development.
loose-audit_log_strategy=SYNCHRONOUS
log_error_verbosity=3

# For debug utility.
loose-debug_sync_timeout=3600
loose-gdb

[mysqld_safe]
log-error=${mysql_log_error}
pid-file=${mysql_pid_file}
#
# include all files from the config directory
#

EOF
)

function stop_mysql()
{
  # kill mysqld mysql_safe
  ps -ef | grep -w mysqld | grep -v grep | awk '{print $2}' | xargs kill -9 > /dev/null 2>&1
  ps -ef | grep -w mysqld_safe | grep -v grep | awk '{print $2}' | xargs kill -9 > /dev/null 2>&1
  rm -rf $mysql_data_dir
}

function init_database()
{
  if [ ! -d "$mysql_install_dir" ]; then
      echo "$mysql_install_dir not exist!"
      exit 1
  fi

  stop_mysql
  pushd $install_path >/dev/null
  mkdir -p $mysql_tmp_dir
  mkdir -p $dstore_home
  mkdir -p $dstore_data_dir
  mkdir -p $dstore_log_dir
  
  printf "%s" "$mysql_cnf_template" > $mysql_cnf_file
  cp $mysql_root_path/storage/dstore/config/guc.json $dstore_home/
  cp $mysql_root_path/storage/dstore/config/tenant_isoland_start.json $dstore_home/
  sed -i "s|\"startConfigPath\": \"\"|\"startConfigPath\": \"$isoland_start_json\"|" $dstore_guc_json
  sed -i "s|\"clientLibPath\": \"\"|\"clientLibPath\": \"$dstore_libvfslinuxadapter_so\"|" $isoland_start_json
  chmod 755 $mysql_cnf_file
  chmod 755 $dstore_guc_json
  chmod 755 $isoland_start_json
  
  export LD_LIBRARY_PATH=$GCC/lib64:$LD_LIBRARY_PATH
  export LD_LIBRARY_PATH=$mysql_install_dir/lib/dstore:$LD_LIBRARY_PATH
  
  # mysql init
  if [ ! -d "$log_dir" ]; then
    mkdir -p $log_dir
  fi
  $mysql_install_dir/bin/mysqld --defaults-file=$mysql_cnf_file --initialize --basedir=$mysql_install_dir --datadir=$mysql_data_dir --explicit_defaults_for_timestamp=true 2>&1 | tee $log_dir/mysqld.out
  sleep 5
}

function start_mysql()
{
  # mysql start
  nohup $mysql_install_dir/bin/mysqld_safe --defaults-file=$mysql_cnf_file --basedir=/$mysql_install_dir --datadir=$mysql_data_dir &
  
  # wait for start finish
  echo Waiting for mysql start...
  time_used=0
  while [ $time_used -lt 60 ]; do
      (echo > /dev/tcp/127.0.0.1/${mysql_port}) >/dev/null 2>&1
      if [ $? -eq 0 ]; then
          echo "mysqld start success!"
          break
      else
          sleep 1
          time_used=$((time_used+1))
      fi
  done
  
  if ! echo > /dev/tcp/127.0.0.1/${mysql_port} 2>/dev/null; then
      echo "mysqld start fail after ${time_used} seconds."
  fi
  
  db_init_passwd=$(grep 'temporary password' $log_dir/mysqld.out | sed 's/.*root@localhost: //'| sed s/[[:space:]]//g)
  $mysql_install_dir/bin/mysql -h ${MYSQL_HOST} -u ${MYSQL_USER} -P ${MYSQL_PORT} -p"${db_init_passwd}" --connect-expired-password -e "alter user 'root'@'localhost' identified with 'mysql_native_password' by '${MYSQL_PASSWD}';flush privileges;"
  echo "Change mysql root password to ${MYSQL_PASSWD}"
}

function start_mysql_gdb()
{
  export LD_LIBRARY_PATH=$GCC/lib64:$LD_LIBRARY_PATH
  export LD_LIBRARY_PATH=$mysql_install_dir/lib/dstore:$LD_LIBRARY_PATH
  
  gdb_cmd_file=$install_path/gdb_commands.txt
  cat > $gdb_cmd_file <<EOF
set verbose off
set print inferior-events off
set pagination off
set confirm off
handle SIGPIPE nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGALRM nostop noprint pass
set breakpoint pending on
break main
run --defaults-file=$mysql_cnf_file --basedir=$mysql_install_dir --datadir=$mysql_data_dir
EOF
  
  echo "GDB command file created: $gdb_cmd_file"
  echo ""
  echo "Run this command in another terminal:"
  echo ""
  echo "gdb -x $gdb_cmd_file $mysql_install_dir/bin/mysqld"
  echo ""
  echo Waiting for mysql start...
  
  time_used=0
  while [ $time_used -lt 300 ]; do
      (echo > /dev/tcp/127.0.0.1/${mysql_port}) >/dev/null 2>&1
      if [ $? -eq 0 ]; then
          echo "mysqld start success!"
          break
      fi
      sleep 1
      time_used=$((time_used+1))
  done
  
  if ! echo > /dev/tcp/127.0.0.1/${mysql_port} 2>/dev/null; then
      echo "mysqld start fail after ${time_used} seconds."
      return 1
  fi
  
  db_init_passwd=$(grep 'temporary password' $log_dir/mysqld.out | sed 's/.*root@localhost: //'| sed s/[[:space:]]//g)
  $mysql_install_dir/bin/mysql -h ${MYSQL_HOST} -u ${MYSQL_USER} -P ${MYSQL_PORT} -p"${db_init_passwd}" --connect-expired-password -e "alter user 'root'@'localhost' identified with 'mysql_native_password' by '${MYSQL_PASSWD}';flush privileges;" 2>/dev/null
  echo "Change mysql root password to ${MYSQL_PASSWD}"
}

function conn_mysql()
{
  echo "$mysql_install_dir/bin/mysql -h ${MYSQL_HOST} -u ${MYSQL_USER} -P ${MYSQL_PORT} -p${MYSQL_PASSWD}"
  $mysql_install_dir/bin/mysql -h ${MYSQL_HOST} -u ${MYSQL_USER} -P ${MYSQL_PORT} -p${MYSQL_PASSWD}
}

operator=$1
if [ "$operator" = '-startmysql' ];then
    init_database
    start_mysql
elif [ "$operator" = '-startmysql_gdb' ];then
    init_database
    start_mysql_gdb
elif [ "$operator" = '-conn' ];then
    conn_mysql
elif [ "$operator" = '-stopmysql' ];then
    stop_mysql
fi