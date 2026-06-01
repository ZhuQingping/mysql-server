# Some setup which is common to the variants of pq_divide_derived*.test

create database pq_test;
use pq_test;

# Materialize derived table, and leave filtering condition on it (to test ref access).
SET @@optimizer_switch="derived_merge=off,derived_condition_pushdown=off";

create table t1(
        id int primary key auto_increment,
        col1 varchar(100),
        col2 int
 )engine= innodb;

--disable_query_log
# With the table definition, our derived table has 2730 rows per page. We need
# more rows, to have at least two data pages, so that at least two
# workers can share the job. Here we have ~6 pages.
 let $loop=100;
 while($loop)
 {
  eval insert into t1(col1, col2) values(repeat('test', 20), $loop);
  dec $loop;
 }
--enable_query_log

show variables like 'force_parallel_execute';
flush status;

select @@internal_tmp_mem_storage_engine;

let $query_full = select sum(prod) from (select t1.id*t2.id as prod from t1, t1 as t2) as derived;
let $query_ref = select sum(prod) from (select t1.id*t2.id as prod from t1, t1 as t2) as derived where prod=33;

# Same with more rows, to test when memory limit is exceeded in Temptable engine
let $query_full_big = select sum(prod) from (select t1.id*t2.id as prod from t1, t1 as t2, t1 as t3) as derived;
let $query_ref_big = select sum(prod) from (select t1.id*t2.id as prod from t1, t1 as t2, t1 as t3) as derived where prod=33;

# Returns 1 if Temptable engine wrote to mmap-ed file
let $check_disk_alloc=
SELECT count_alloc > 0
FROM performance_schema.memory_summary_global_by_event_name
WHERE event_name = 'memory/temptable/physical_disk';
