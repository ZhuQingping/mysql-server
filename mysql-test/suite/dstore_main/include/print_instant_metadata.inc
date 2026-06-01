#-------------------------------------------------------
# Prints instant metadata of table and it's columns from
#	- DD Tables
#	- Information_schema tables
# NOTE : $table_name needs to be set.
#      : $is_debug to be set for debug build
#-------------------------------------------------------
--disable_query_log

if ($is_debug) {
  SET SESSION debug= '+d,skip_dd_table_access_check';
  eval SELECT ID INTO @tid FROM mysql.tables WHERE NAME='$table_name';

  --echo # DD Metadata of columns in table
  --replace_regex /table_id=[0-9]+;/table_id=TABLE_ID;/
  SELECT NAME, ORDINAL_POSITION, TYPE, HAS_NO_DEFAULT, HIDDEN, SE_PRIVATE_DATA
     FROM mysql.columns
     WHERE TABLE_ID=@tid
     ORDER BY ORDINAL_POSITION;
}

--enable_query_log
