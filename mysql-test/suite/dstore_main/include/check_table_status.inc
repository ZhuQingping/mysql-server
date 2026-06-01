# ==== Purpose ====
#
# Get table status information.
#
# ==== Usage ====
#
# [--let $table_name = 'xxx']

--source include/have_debug.inc

# Check 'SHOW TABKE STATUS'
--replace_column 9 # 12 #
eval SHOW TABLE STATUS LIKE $table_name;

# Check stats in information_schema.tables
eval SELECT TABLE_NAME,TABLE_TYPE,ENGINE,TABLE_ROWS,AVG_ROW_LENGTH,DATA_LENGTH,MAX_DATA_LENGTH,DATA_FREE FROM information_schema.tables WHERE table_name=$table_name;

# Check stats in mysql.table_stats
--disable_query_log
--disable_result_log
SET debug='+d,skip_dd_table_access_check';
--enable_query_log
--enable_result_log
eval SELECT table_name,table_rows,avg_row_length,data_length,max_data_length,data_free FROM mysql.table_stats WHERE table_name=$table_name;
--disable_query_log
--disable_result_log
SET debug='-d,skip_dd_table_access_check';
--enable_query_log
--enable_result_log

# Check stats in mysql.dstore_table_stats
eval SELECT table_name,n_rows,heap_table_pages,heap_table_free_pages,heap_table_lob_pages FROM mysql.dstore_table_stats WHERE table_name=$table_name;
