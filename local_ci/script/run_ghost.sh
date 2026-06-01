#!/bin/bash
source ./common_conf.sh
source ./common_functions.sh

readonly DATABASE="ghostdb"
readonly TABLE="test_table"
readonly MAX_COUNT=1000
readonly ENGINE="Dstore"
readonly COLUMN_COUNT=6
readonly MYSQL_CMD="${install_path}/mysql/bin/mysql -h$MYSQL_HOST -P$MYSQL_PORT -u$MYSQL_USER -p$MYSQL_PASSWD"
readonly CREATE_CONN="$MYSQL_CMD -e"
readonly QUERY_CONN="$MYSQL_CMD -D$DATABASE -Nse"

init_database() {
    echo "\033[32m[CI_INFO]: Creating a Test Environment.\033[0m"
    $CREATE_CONN "DROP DATABASE IF EXISTS $DATABASE; \
        CREATE DATABASE $DATABASE; \
        CREATE TABLE $DATABASE.$TABLE ( \
            id INT PRIMARY KEY, \
            a INT \
        ) ENGINE=$ENGINE;"

    $CREATE_CONN "USE $DATABASE;
        DELIMITER //
        CREATE PROCEDURE insert_rows()
        BEGIN
            DECLARE i INT DEFAULT 0;
            WHILE i < $MAX_COUNT DO
                INSERT INTO $TABLE (id, a) VALUES (i, i);
                SET i = i + 1;
            END WHILE;
        END //
        DELIMITER ;
        CALL insert_rows();
        DROP PROCEDURE insert_rows;"
}

# Table structure validation function
verify_column_count() {
    local actual_count=$($QUERY_CONN "SELECT COUNT(*)  FROM INFORMATION_SCHEMA.COLUMNS  WHERE TABLE_SCHEMA = '$DATABASE'  AND TABLE_NAME = '$TABLE';")
    
    if [ "$actual_count" -eq "$COLUMN_COUNT" ]; then
       echo -e "\033[32m[CI_INFO]: The number of fields is correct. \033[0m"
    else
        echo -e "\033[32m[CI_INFO]: ERROR: The number of fields is incorrect. The expected value: $COLUMN_COUNT, but the actual value: $actual_count. \033[0m"
        exit 1
    fi
}

verify_data() {
    local operation_type=$1
    local query=$2
    local expected_count=$3
    
    local actual_count=$($QUERY_CONN "$query")
    
    if [ "$actual_count" -eq "$expected_count" ]; then
        echo -e "\033[32m[CI_INFO]: ${operation_type},Data validation succeeded.\033[0m"
    else
        echo -e "\033[32m[CI_INFO]: ERROR: ${operation_type},Data validation failed.\033[0m"
        exit 1
    fi
}

verify_data_update_non_primary_key() {
    verify_data "update Non-primary key" "SELECT COUNT(*) FROM $DATABASE.$TABLE WHERE a=$MAX_COUNT;" "$MAX_COUNT"
}

verify_data_update_primary_key() {
    verify_data "update Primary key" "SELECT COUNT(*) FROM $DATABASE.$TABLE WHERE id>=$MAX_COUNT AND id<$MAX_COUNT+$MAX_COUNT;" "$MAX_COUNT"
}

verify_data_delete() {
    verify_data "delete" "SELECT COUNT(*) FROM $DATABASE.$TABLE;" 0
}

verify_data_insert() {
    verify_data "insert" "SELECT COUNT(*) FROM $DATABASE.$TABLE;" "$MAX_COUNT"
}

start_ptosc() {
    local column_name=$1
    echo -e "\033[32m[CI_INFO]: START pt-osc\033[0m"
    gh-ost \
        --alter="ADD COLUMN $column_name INT DEFAULT NULL" \
        --chunk-size=50 \
        --execute \
        --user="$MYSQL_USER" --password="$MYSQL_PASSWD" --host="$MYSQL_HOST" --port="$MYSQL_PORT" --database="$DATABASE" --table="$TABLE" --initially-drop-ghost-table -initially-drop-socket-file --initially-drop-old-table --chunk-size=3000 --debug --allow-on-master
}

update_non_primary_key() {
    init_database
    echo -e "\033[32m[CI_INFO]: Start the concurrent update Non-primary key\033[0m"
    $QUERY_CONN "UPDATE $DATABASE.$TABLE SET a=$MAX_COUNT;" &
    
    start_ptosc "a1"
    verify_data_update_non_primary_key
}

update_primary_key() {
    echo -e "\033[32m[CI_INFO]: Start the concurrent update Primary key\033[0m"
    $QUERY_CONN "UPDATE $DATABASE.$TABLE SET id=id+$MAX_COUNT;" &
    
    start_ptosc "a2"
    verify_data_update_primary_key
}

delete_data() {
    echo -e "\033[32m[CI_INFO]: Start the concurrent Delete\033[0m"
    $QUERY_CONN "DELETE FROM $DATABASE.$TABLE;" &
    
    start_ptosc "a3"
    verify_data_delete
}

create_insert_procedure() {
    $CREATE_CONN "USE $DATABASE;
        DELIMITER //
        CREATE PROCEDURE insert_rows_proc()
        BEGIN
            DECLARE i INT DEFAULT 0;
            WHILE i < $MAX_COUNT DO
                INSERT INTO $TABLE (id, a) VALUES (i, i);
                SET i = i + 1;
            END WHILE;
        END //
        DELIMITER ;"
}

insert_data() {
    create_insert_procedure
    echo -e "\033[32m[CI_INFO]: Start the concurrent Insert\033[0m"
    $QUERY_CONN "CALL insert_rows_proc();" &
    
    start_ptosc "a4"
    sleep 20
    
    verify_data_insert
}

# main
main() {
    #trap '$CREATE_CONN "DROP DATABASE IF EXISTS $DATABASE"' EXIT
    
    # case1 update Non-primary key
    update_non_primary_key
    
    # case2 update Primary key
    update_primary_key
    
    # case3 delete
    delete_data
    
    # case4 insert
    insert_data

    # Verify the number of fields in a table.
    verify_column_count
}

main