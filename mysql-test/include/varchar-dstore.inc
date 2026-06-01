# Initialise
--disable_warnings
drop table if exists t1,t2,t3;
--enable_warnings

disable_query_log;
select "--- Testing varchar ---";
enable_query_log;

#
# Simple basic test that endspace is saved
#

create table t1 (v varchar(10), c char(10), t text);
insert into t1 values('+ ', '+ ', '+ ');
set @a=repeat(' ',20);
insert into t1 values (concat('+',@a),concat('+',@a),concat('+',@a));
select concat('*',v,'*',c,'*',t,'*') from t1;

# Check how columns are copied
show create table t1;
create table t2 like t1;
show create table t2;
create table t3 select * from t1;
show create table t3;
alter table t1 modify c varchar(10);
show create table t1;
alter table t1 modify v char(10);
show create table t1;
alter table t1 modify t varchar(10);
show create table t1;
select concat('*',v,'*',c,'*',t,'*') from t1;
drop table t1,t2,t3;

#
# Test unique keys
#

SET NAMES utf8mb4;  # Make sure á etc. below are interpreted correctly as UTF-8.

create table t1 (a char(10), unique (a));
insert into t1 values ('a   ');
--error ER_DUP_ENTRY
insert into t1 values ('a ');

alter table t1 modify a varchar(10);
--error ER_DUP_ENTRY
insert into t1 values ('a'),('á'),('à'),('å');
--error ER_DUP_ENTRY
insert into t1 values ('á');
--error ER_DUP_ENTRY
insert into t1 values ('A');
--error ER_DUP_ENTRY
insert into t1 values ('å');
update t1 set a='a  ' where a like 'a%';
select concat(a,'.') from t1;
update t1 set a='abc    ' where a like 'a ';
select concat(a,'.') from t1;
update t1 set a='a      ' where a like 'a %';
select concat(a,'.') from t1;
update t1 set a='a  ' where a like 'a      ';
select concat(a,'.') from t1;
drop table t1;

SET NAMES DEFAULT;


create table t1 (v varchar(10), c char(10)) row_format=fixed;
show create table t1;
insert into t1 values('a','a'),('a ','a ');
select concat('*',v,'*',c,'*') from t1;
drop table t1;

#
# Test long varchars
#

create table t1 (v varchar(65530), key(v(10))) charset latin1;
insert into t1 values(repeat('a',65530));
select length(v) from t1 where v=repeat('a',65530);
drop table t1;

#
# Bug #9489: problem with hash indexes
# Bug #10802: Index is not used if table using BDB engine on HP-UX
#

create table t1(a int, b varchar(12), key ba(b, a));
insert into t1 values (1, 'A'), (20, NULL);
explain select * from t1 where a=20 and b is null;
select * from t1 where a=20 and b is null;
drop table t1;
