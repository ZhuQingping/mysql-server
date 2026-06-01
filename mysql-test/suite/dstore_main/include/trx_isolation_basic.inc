--echo # this mtr includes test cases:
--echo # test-replace into, session1 commit.
--echo # test-replace into, session1 rollback.
--echo # test-insert/update conflict.
--echo # test-update/update conflict.
--echo # test-insert select.

--disable_warnings
drop table if exists test;
--enable_warnings

--echo # Establish connection thr1 (user=root)
connect (thr1,localhost,root,,);
--echo # Establish connection thr2 (user=root)
connect (thr2,localhost,root,,);

--echo # test-replace into, session1 commit. rr and rc are same.
--echo # +----------------------------+--------------------------------+
--echo # | session1                   | session2                       |
--echo # +----------------------------+--------------------------------+
--echo # | begin;                     | begin;                         |
--echo # | insert into t value(1,10); |                                |
--echo # |                            | replace into t values(1,20);   |
--echo # |                            | #wait                          |
--echo # | commit;                    |                                |
--echo # |                            | #continue                      |
--echo # |                            | select * from  t;              |
--echo # |                            | #1,20                          |
--echo # |                            | commit;                        |
--echo # |                            | insert into t values(1,50);    |
--echo # |                            | #error, duplicate              |
--echo # |                            | insert into t values(2,10);    |
--echo # |                            | update t set b=30 where a = 1; |
--echo # |                            | select * from t;               |
--echo # |                            | #1,30;2,10                     |
--echo # |                            | update t set b = 50;           |
--echo # |                            | delete from t;                 |
--echo # |                            | select * from t;               |
--echo # |                            | #1,50;2,50                     |
--echo # +----------------------------+--------------------------------+
--echo # get isolation for thr1.
connection thr1;
select @@transaction_isolation;
--echo # get isolation for thr2.
connection thr2;
select @@transaction_isolation;

--echo # Switching to connection 'default'.
connection default;
create table t(a int primary key, b int);

--echo # Switching to connection 'thr1'.
connection thr1;

begin;
insert into t value(1,10);

--echo # Switching to connection 'thr2'.
connection thr2;

begin;
--send
replace into t values(1,20);

--echo # Switching to connection 'thr1'.
connection thr1;

commit;

--echo # Switching to connection 'thr2'.
connection thr2;
reap;
commit;

select * from  t;

--echo # Switching to connection 'thr1'.
connection thr1;
--error ER_DUP_ENTRY
insert into t values(1,50);
insert into t values(2,10);
update t set b = 30 where a = 1;
select * from t;
update t set b = 50;
select * from t;
delete from t;
select * from t;
connection default;
drop table t;

--echo # test-replace into, session1 rollback. rr and rc are same.
--echo # +----------------------------+--------------------------------+
--echo # | session1                   | session2                       |
--echo # +----------------------------+--------------------------------+
--echo # | begin;                     | begin;                         |
--echo # | insert into t value(1,10); |                                |
--echo # |                            | replace into t values(1,20);   |
--echo # |                            | #wait                          |
--echo # | rollback;                  |                                |
--echo # |                            | #continue                      |
--echo # |                            | select * from  t;              |
--echo # |                            | #1,20                          |
--echo # |                            | commit;                        |
--echo # |                            | insert into t values(1,50);    |
--echo # |                            | #error, duplicate              |
--echo # |                            | insert into t values(2,10);    |
--echo # |                            | update t set b=30 where a = 1; |
--echo # |                            | select * from t;               |
--echo # |                            | #1,30;2,10                     |
--echo # |                            | update t set b = 50;           |
--echo # |                            | delete from t;                 |
--echo # |                            | select * from t;               |
--echo # |                            | #1,50;2,50                     |
--echo # +----------------------------+--------------------------------+
--echo # get isolation for thr1.
connection thr1;
select @@transaction_isolation;
--echo # get isolation for thr2.
connection thr2;
select @@transaction_isolation;

--echo # Switching to connection 'default'.
connection default;
create table t(a int primary key, b int);

--echo # Switching to connection 'thr1'.
connection thr1;

begin;
insert into t value(1,10);

--echo # Switching to connection 'thr2'.
connection thr2;

begin;
--send
replace into t values(1,20);

--echo # Switching to connection 'thr1'.
connection thr1;

rollback;

--echo # Switching to connection 'thr2'.
connection thr2;
reap;
commit;

select * from  t;

--echo # Switching to connection 'thr1'.
connection thr1;
--error ER_DUP_ENTRY
insert into t values(1,50);
insert into t values(2,10);
update t set b = 30 where a = 1;
select * from t;
update t set b = 50;
select * from t;
delete from t;
select * from t;
connection default;
drop table t;

--echo # test-insert/update conflict
--echo # +------------------------------+----------------------------------------------------+
--echo # | session1                     | session2                                           |
--echo # +------------------------------+----------------------------------------------------+
--echo # | begin;                       | begin;                                             |
--echo # |                              | select * from t where a = 1;                       |
--echo # | insert into t values(1,'m'); |                                                    |
--echo # |                              | select * from t where a = 1;                       |
--echo # |                              | # empty                                            |
--echo # |                              | select * from t for update where a = 1;            |
--echo # |                              | # empty                                            |
--echo # | commit;                      |                                                    |
--echo # |                              | select * from t where a = 1;                       |
--echo # |                              | # empty for rr and 1,m for rc                      |
--echo # |                              | select * from t for update where a = 1;            |
--echo # |                              | # 1,m                                              |
--echo # +------------------------------+----------------------------------------------------+
--echo # get isolation for thr1.
connection thr1;
select @@transaction_isolation;
--echo # get isolation for thr2.
connection thr2;
select @@transaction_isolation;

create table t(a int primary key, b char(10));

begin;
select * from t;

--echo # Switching to connection 'thr1'.
connection thr1;
begin;
insert into t values(1,'m');

--echo # Switching to connection 'thr2'.
connection thr2;
select * from t where a = 1;
select * from t where a = 1 for update;

--echo # Switching to connection 'thr1'.
connection thr1;
commit;

--echo # Switching to connection 'thr2'.
connection thr2;
select * from t where a = 1;
select * from t where a = 1 for update;
commit;
select * from t where a = 1;
select * from t where a = 1 for update;

connection default;
drop table t;


--echo # test-update/update conflict
--echo # +----------------------------------+-----------------------------------------+
--echo # | session1                         | session2                                |
--echo # +----------------------------------+-----------------------------------------+
--echo # | begin;                           | begin;                                  |
--echo # |                                  | select * from t where a = 1;            |
--echo # |                                  | #1,m                                    |
--echo # | update t set b ='x' where a = 1; |                                         |
--echo # |                                  | update t set b ='y' where a = 1;        |
--echo # |                                  | #wait                                   |
--echo # | commit;                          |                                         |
--echo # |                                  | select * from t where a = 1;            |
--echo # |                                  | #rr/rc 1, y                             |
--echo # |                                  | select * from t where a = 1 for update; |
--echo # |                                  | #1,y                                    |
--echo # |                                  | commit;                                 |
--echo # +----------------------------------+-----------------------------------------+
--echo # get isolation for thr1.
connection thr1;
select @@transaction_isolation;
--echo # get isolation for thr2.
connection thr2;
select @@transaction_isolation;

create table t(a int primary key, b char(10));
insert into t values(1,'a');

begin;
select * from t where a = 1;

--echo # Switching to connection 'thr1'.
connection thr1;
begin;
update t set b ='x' where a = 1;

--echo # Switching to connection 'thr2'.
connection thr2;
--send 
update t set b ='y' where a = 1;

--echo # Switching to connection 'thr1'.
connection thr1;
commit;

--echo # Switching to connection 'thr2'.
connection thr2;
reap;
select * from t where a = 1;
select * from t where a = 1 for update;
commit;
select * from t where a = 1;
select * from t where a = 1 for update;

--echo # Switching to connection 'default'.
connection default;
drop table t;

--echo # test-insert select
--echo # +----------------------------------+----------------------------------+
--echo # | session1                         | session2                         |
--echo # +----------------------------------+----------------------------------+
--echo # | begin;                           | begin;                           |
--echo # | insert into t1 select * from t1; |                                  |
--echo # | select  count(*) from t1;        |                                  |
--echo # | #4(1)                            |                                  |
--echo # |                                  | select  count(*) from t1;        |
--echo # |                                  | #2(2)                            |
--echo # |                                  | insert into t1 select * from t1; |
--echo # |                                  | select  count(*) from t1;        |
--echo # |                                  | #4(3)                            |
--echo # | select  count(*) from t1;        |                                  |
--echo # | #4(4)                            |                                  |
--echo # | commit;                          |                                  |
--echo # | select  count(*) from t1;        |                                  |
--echo # | #4(5)                            |                                  |
--echo # |                                  | select  count(*) from t1;        |
--echo # |                                  | #rc 6, rr 4(6)                   |
--echo # |                                  | insert into t1 select * from t1; |
--echo # |                                  | select  count(*) from t1;        |
--echo # |                                  | #rc 12, rr 8(7)                  |
--echo # | select  count(*) from t1;        |                                  |
--echo # | #4(8)                            |                                  |
--echo # |                                  | commit;                          |
--echo # |                                  | select  count(*) from t1;        |
--echo # |                                  | #rc 12, rr 10 (9)                |
--echo # | select  count(*) from t1;        |                                  |
--echo # | #rc 12, rr 10 (10)                |                                  |
--echo # +----------------------------------+----------------------------------+
--echo # get isolation for thr1.
connection thr1;
select @@transaction_isolation;
--echo # get isolation for thr2.
connection thr2;
select @@transaction_isolation;

create table t1(a int);
insert into t1 values(1),(2);
begin;

--echo # Switching to connection 'thr1'.
connection thr1;
begin;
insert into t1 select * from t1;
--echo # result is 4(1)
select  count(*) from t1;

--echo # Switching to connection 'thr2'.
connection thr2;
--echo # result is 2(2)
select  count(*) from t1;
insert into t1 select * from t1;
--echo # result is 4(3)
select  count(*) from t1;

--echo # Switching to connection 'thr1'.
connection thr1;
--echo # result is 4(4)
select  count(*) from t1;
commit;
--echo # result is 4(5)
select  count(*) from t1;

--echo # Switching to connection 'thr2'.
connection thr2;
--echo # result is rc 6, rr 4 (6)
select  count(*) from t1;
insert into t1 select * from t1;
--echo # result is rc 12, rr 8(7)
select  count(*) from t1;

--echo # Switching to connection 'thr1'.
--echo # result is 4(8)
connection thr1;
select  count(*) from t1;

--echo # Switching to connection 'thr2'.
connection thr2;
commit;
--echo # result is rc 12, rr 10 (9)
select  count(*) from t1;

--echo # Switching to connection 'thr2'.
connection thr2;
--echo # result is rc 12, rr 10 (10)
select  count(*) from t1;

--echo # Switching to connection 'default'.
connection default;
drop table t1;


# clean up
disconnect thr1;
disconnect thr2;
