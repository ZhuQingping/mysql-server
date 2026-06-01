#!/bin/bash
# Common config, variables and functions shared between multiple scripts

# Create a tmp directory for data if no other is specified
source ./common_conf.sh
WORK_DIR=$run_path
# Defaults
MYSQL_DATADIR="$WORK_DIR/mysql"
MYSQL_BIN_DIR=$install_path/mysql/bin
# Binaries
MYSQLD_BINARY="$MYSQL_BIN_DIR/mysqld"
MYSQL_BINARY="$MYSQL_BIN_DIR/mysql"
MYSQLADMIN="$MYSQL_BIN_DIR/mysqladmin"
# Unix socket files
MYSQL_SOCK="$WORK_DIR/mysql.sock"
MYSQLX_SOCK="$WORK_DIR/mysqlx.sock"

# By default, we generate data before running tpc/sysbench
GENERATE_DATA=true
mkdir -p ${install_path}/dstore/dstore_log
# cleanup function that removes WORK_DIR on EXIT.
function cleanup {
    stop_mysqld
    if kill -0 "$MYSQLD_PID" 2>/dev/null; then
        kill "$MYSQLD_PID"
        if ps -p "$MYSQLD_PID" > /dev/null; then
            wait "$MYSQLD_PID"
        fi
        # Sleep for a short time to allow mysqld to exit cleanly.
        sleep 2
    fi

    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

function check_binaries_exist() {
    if [ ! -f "$MYSQLD_BINARY" ] || [ ! -x "$MYSQLD_BINARY" ]; then
        echo "Did not find MySQL Server executable in '$MYSQLD_BINARY'. Exiting ..."
        exit 1
    fi

    if [ ! -f "$MYSQL_BINARY" ] || [ ! -x "$MYSQL_BINARY" ]; then
        echo "Did not find MySQL Client executable in '$MYSQL_BINARY'. Exiting ..."
        exit 1
    fi
}

function mysqld_options() {
    MYSQLD_OPTIONS="
    --no-defaults \
    --port=$MYSQL_PORT \
    --mysqlx-port=$MYSQLX_PORT \
    --datadir=$MYSQL_DATADIR \
    --dstore_log_path=${install_path}/dstore/dstore_log \
    --dstore_tenant_config=${install_path}/mysql/lib/dstore/tenant_isoland_start.json
    --explicit_defaults_for_timestamp=true  \
    --user=root \
    --socket=$MYSQL_SOCK \
    --mysqlx-socket=$MYSQLX_SOCK \
    --binlog_order_commits=off \
    --skip-ssl \
    --skip-mysqlx \
    --log_error_verbosity=1 \
    --max_connections=2000 \
    --back_log=1000 \
    --join_buffer_size=2M \
    --key_buffer_size=16M \
    --sort_buffer_size=2M \
    --join_buffer_size=2M \
    --read_rnd_buffer_size=1M \
    --table_open_cache_instances=64 \
    --log_slave_updates=ON \
    --max_prepared_stmt_count=1000000 \
    --default_authentication_plugin=mysql_native_password \
    --storage_engine_mode=BOTH_STORAGE_ENGINE
    "
    # MySQL connection options
    CLIENT_ROOT_OPTIONS="-S $WORK_DIR/mysql.sock -u root"
}

function mysqld_options_loadfile() {
    loadfilepath=$1
    MYSQLD_OPTIONS="
    --no-defaults \
    --port=$MYSQL_PORT \
    --mysqlx-port=$MYSQLX_PORT \
    --datadir=$MYSQL_DATADIR \
    --explicit_defaults_for_timestamp=true  \
    --user=root \
    --socket=$MYSQL_SOCK \
    --local_infile=1 \
    --secure-file-priv=$loadfilepath \
    --mysqlx-socket=$MYSQLX_SOCK
    "
    # MySQL connection options
    CLIENT_ROOT_OPTIONS="-S $WORK_DIR/mysql.sock -u root"
}

function start_mysqld() {
    echo "Initializing MySQL datadir in $MYSQL_DATADIR"
    mkdir -p $MYSQL_DATADIR
    "$MYSQLD_BINARY" $MYSQLD_OPTIONS --initialize-insecure

    echo "Starting MySQL Server, and waiting a few seconds for it to become available"
    "$MYSQLD_BINARY" $MYSQLD_OPTIONS &
    MYSQLD_PID=$!
}

function ping_mysqld() {
    local i=0
    set +e
    while [ $i -lt 15 ]; do
        printf "\nWaiting for mysqld to become ready ...\n"
        $MYSQLADMIN ping $CLIENT_ROOT_OPTIONS && break
        sleep 3
        ((i++))
    done
    if [ $i == 15 ]; then
        echo "Giving up. Exiting ..."
        exit 1
    fi
}

function stop_mysqld() {
    echo "------------------------"
    echo "--- Stopping mysqld ---"
    echo "------------------------"
    "$MYSQL_BINARY" $CLIENT_ROOT_OPTIONS -e "shutdown;"
    echo ""
    echo "------------------------"
    echo "--- Stopped ---"
    echo "------------------------"
}

#tpcc database table
function create_tpcc_table() {
    tpch_DB=$1
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "create database ${tpch_DB};"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "set global FOREIGN_KEY_CHECKS=0;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $tpch_DB < $mysql_root_path/tpcc/create_table.sql
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $tpch_DB < $mysql_root_path/tpcc/add_fkey_idx.sql
}

#TPCH_tool deloy in ci machine
function load_tpch_data() {
    echo "load_tpch_data"
    use_innodb_engine=$1
    DB=cde_tpch
    if [ "$use_innodb_engine"x == "1"x ]; then
        echo "load_tpch_data innode"
        $MYSQL_BINARY $CLIENT_ROOT_OPTIONS < $mysql_root_path/TPCH_Tool/dbgen/dss_innodb.ddl
        $MYSQL_BINARY $CLIENT_ROOT_OPTIONS < $mysql_root_path/TPCH_Tool/dbgen/dss_innodb.ri
    else
        $MYSQL_BINARY $CLIENT_ROOT_OPTIONS < $mysql_root_path/TPCH_Tool/dbgen/dss_innodb_cde.ddl
        $MYSQL_BINARY $CLIENT_ROOT_OPTIONS < $mysql_root_path/TPCH_Tool/dbgen/dss_innodb_cde.ri
    fi

    echo "load_tpch_data begin ..."
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS < $mysql_root_path/TPCH_Tool/dbgen/loaddata.sql
    echo "load_tpch_data end ..."

    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE customer;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE lineitem;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE nation;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE orders;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE part;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE partsupp;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE region;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB -e "ANALYZE TABLE supplier;"
}

function run_tpch_explain_sql() {
    use_innodb_engine=$1
    outfile=cde_tpch_explain_result.txt
    if [ "$use_innodb_engine"x == "1"x ]; then
        outfile=innode_tpch_explain_result.txt
    fi
    DB=cde_tpch
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS $DB --default-character-set=utf8mb4 -vvv < $mysql_root_path/TPCH_Tool/dbgen/queries/allquery_explain.sql > $mysql_root_path/TPCH_Tool/dbgen/queries/$outfile
}
