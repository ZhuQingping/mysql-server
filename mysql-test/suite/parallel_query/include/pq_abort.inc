set session debug="";
set session debug="+d, dup_thd_abort";

explain select * from t1 limit 1;

select * from t1 limit 1;
show status like "PQ_memory_used";

set session debug="";
set session debug="+d, dup_join_abort";

explain select * from t1 limit 1;

select * from t1 limit 1;
show status like "PQ_memory_used";

set session debug="";
set session debug="+d, dup_select_abort1";

explain select * from t1 limit 1;

select * from t1 limit 1;
show status like "PQ_memory_used";

set session debug="";
set session debug="+d, dup_select_abort2";
explain select * from t1 limit 1;

select * from t1 limit 1;
show status like "PQ_memory_used";

set session debug="";
set session debug="+d, pq_leader_abort1";
explain select * from t1 limit 1;

select * from t1 limit 1;
show status like "PQ_memory_used";

set session debug="";
set session debug="+d, pq_leader_abort2";

explain select * from t1 limit 1;

select * from t1 limit 1;

show status like "PQ_memory_used";

set session debug="";
set session debug="+d, pq_leader_abort3";

explain select * from t1 limit 1;

select * from t1 limit 1;
