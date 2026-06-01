
--let $old_table_id = `SELECT table_id FROM information_schema.innodb_tables WHERE name like '%t1%';`

--let $old_instant_cols = `SELECT instant_cols FROM information_schema.innodb_tables WHERE name like '%t1%';`

--disable_warnings
--eval $instant_add_column
--enable_warnings

--let $table_id = `SELECT table_id FROM information_schema.innodb_tables WHERE name like '%t1%';`

--let $instant_cols = `SELECT instant_cols FROM information_schema.innodb_tables WHERE name like '%t1%';`

if ($old_table_id != $table_id)
{
  --echo Table ID differed
}

if ($old_table_id == $table_id)
{
  --echo Table id did not change
}



CHECK TABLE t1;
