# ==== Purpose ====
#
# Get segment id of table
#
# ==== Usage ====
#   --let $SCHEMA_OF_DSTORE_TABLE = 
#   --let $NAME_OF_DSTORE_TABLE = 
#   --source suite/dstore_swat/include/get_table_segment_id.inc

--disable_query_log
--disable_result_log
set debug='+d,skip_dd_table_access_check';
--enable_result_log
--enable_query_log

--let $TABLE_SEG_FILE_ID=`select SUBSTRING_INDEX(SUBSTRING_INDEX(t.se_private_data, 'relfileid=', -1), ';', 1) as data from mysql.tables t, mysql.schemata s where t.schema_id = s.id and s.name="$SCHEMA_OF_DSTORE_TABLE" and t.name="$NAME_OF_DSTORE_TABLE"`

--let $TABLE_SEG_BLOCK_ID=`select SUBSTRING_INDEX(SUBSTRING_INDEX(t.se_private_data, 'relblknum=', -1), ';', 1) as data from mysql.tables t, mysql.schemata s where t.schema_id = s.id and s.name="$SCHEMA_OF_DSTORE_TABLE" and t.name="$NAME_OF_DSTORE_TABLE"`
