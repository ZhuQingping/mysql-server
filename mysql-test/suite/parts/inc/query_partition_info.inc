set global debug='+d,skip_dd_table_access_check';
select partition_type,partition_expression,partition_expression_utf8,default_partitioning,
  subpartition_type,subpartition_expression,subpartition_expression_utf8,default_subpartitioning
  from mysql.tables where name='t1';
select number,name,description_utf8,engine,comment,options from mysql.table_partitions where table_id in (select id from mysql.tables where name='t1');
set global debug='-d,skip_dd_table_access_check';

