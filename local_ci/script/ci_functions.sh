#!/bin/bash
export CDE_MYSQL_DIR=/home/ci/mysql-server
export SCRIPT_DIR=$CDE_MYSQL_DIR/ci

# Source common functions
source "${SCRIPT_DIR}"/common_functions.sh

function create_tpch_database_cp() {
    DB=hermes_tpch
    if [ "$GENERATE_DATA" == "true" ]; then
        go-tpc tpch prepare --dropdata --sf="$SCALE_FACTOR" -H 127.0.0.1 -P "$MYSQL_PORT" --db "$DB" --threads 16

        $MYSQL_BINARY "$DB" $CLIENT_ROOT_OPTIONS -e "select count(*) from lineitem;"

        # Create one index to avoid some of the queries to hang.
        $MYSQL_BINARY "$DB" $CLIENT_ROOT_OPTIONS -e "create index lineitem_ck3 on lineitem (l_partkey, l_suppkey);"
        $MYSQL_BINARY "$DB" $CLIENT_ROOT_OPTIONS -e "select count(*) from lineitem;"

        # To remove after we accept PK composed by more than one column
        exchange_compound_pk_by_single_pk hermes_tpch lineitem "L_ORDERKEY, L_LINENUMBER" "L"
        exchange_compound_pk_by_single_pk hermes_tpch partsupp "PS_PARTKEY, PS_SUPPKEY" "PS"
        $MYSQL_BINARY "$DB" $CLIENT_ROOT_OPTIONS -e "select count(*) from lineitem;"
    fi

    save_generated_mysql_datadir

    # Change propagation setting should be configured after table population, but before setting secondary_engine=hermes
    set_rds_hermes_variables
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "SELECT * FROM performance_schema.hermes_table_sync_status;"
    while [ $? -ne 0 ]; do !!; sleep 5; done
    if [ "$OBS_RESTORE" == "true" ]; then
        echo "no need to ingestion. data recovered from OBS."
        $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "SELECT count(*) FROM performance_schema.hermes_table_sync_status;" | grep "8"
        if [ "$?" == "0" ]; then
            return
        fi
    fi
    echo "wrong branch..."
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "SELECT count(*) FROM performance_schema.hermes_table_sync_status;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "SELECT name, @@global.gtid_executed, sync_gtid_executed, status, data_source_uri, sync_state_prefix FROM performance_schema.hermes_stream_sync_status;"
    $MYSQL_BINARY $CLIENT_ROOT_OPTIONS -e "SELECT @@global.gtid_executed;"

    for TABLE in customer lineitem nation orders part partsupp region supplier; do
        if [ "$GENERATE_DATA" == "false" ]; then
            $MYSQL_BINARY hermes_tpch $CLIENT_ROOT_OPTIONS -e "alter table $TABLE secondary_engine=NULL;"
        fi
        $MYSQL_BINARY hermes_tpch $CLIENT_ROOT_OPTIONS -e "alter table $TABLE secondary_engine=hermes;"
    done
}

function save_results_file() {
    local save_results_dir=$1

    echo "-------------$RESULT_MYSQL_CSV_FILE------------"
    cp $RESULT_MYSQL_CSV_FILE $save_results_dir/
    cat "$RESULT_MYSQL_CSV_FILE"
    echo ""
    echo "-------------$RESULT_HERMES_CSV_FILE------------"
    cp $RESULT_HERMES_CSV_FILE $save_results_dir/
    cat "$RESULT_HERMES_CSV_FILE"
    echo ""
}
