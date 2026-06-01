#
# Testing for pt-osc tools concurrency behaivor.
#

# Save the initial number of concurrent sessions

--source include/have_only_dstore.inc
--source include/count_sessions.inc

--disable_warnings
drop table if exists t;
--enable_warnings

--echo # Establish connection thr1 (user=root)
connect (thr1,localhost,root,,);
--echo # Establish connection thr2 (user=root)
connect (thr2,localhost,root,,);

--echo # Switching to connection 'default'.
connection default;

create table t (
    id int not null primary key, a int
) ENGINE=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;

--echo # Switching to connection 'thr1'.
connection thr1;
set autocommit=0;
begin;
insert into t values(1,1);

--echo # Switching to connection 'thr2'.
connection thr2;
set autocommit=0;
begin;
--echo # thr2 hang here
--send
replace into t values(1,2);
--sleep 1


--echo # Switching to connection 'thr1'.
connection thr1;
select * from t;
commit;

--echo # Switching to connection 'thr2'.
connection thr2;
reap;
# see updated tuple
select * from t;
# see updated tuple
select * from t for update;
commit;

--echo # clean
connection default;
disconnect thr1;
disconnect thr2;
drop table t;

--echo # Establish connection thr3 (user=root)
connect (thr3,localhost,root,,);
connection default;

--echo 
--echo #case1: insert select first, then delete
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo #View data
select * from old_table order by id;
select * from new_table order by id;

--echo #Create delete trigger
--disable_warnings
drop trigger if exists pt_osc_delete;
--enable_warnings
delimiter $$;
create trigger pt_osc_delete
after delete on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    delete ignore from new_table
    where new_table.id <=> old.id;
end $$
delimiter ;$$

--echo #Execute concurrent DML operations
set debug_sync='before_lock_tuple wait_for proceed';
--send delete from old_table where id = 1;
--sleep 1

--echo #Simulate execution tool operation
connection thr3;
insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
set debug_sync='now signal proceed';

connection default;
reap;
--echo #Verify data consistency
select * from old_table order by id;
select * from new_table order by id;

--echo # clean
drop table old_table;
drop table new_table;

--echo 
--echo # case2: delete first, then insert select 
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo #View data
select * from old_table order by id;
select * from new_table order by id;

--echo #Create delete trigger
--disable_warnings
drop trigger if exists pt_osc_delete;
--enable_warnings
delimiter $$;
create trigger pt_osc_delete
after delete on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    delete ignore from new_table
    where new_table.id <=> old.id;
end $$
delimiter ;$$

--echo #Execute concurrent DML operations
set debug_sync='before_lock_tuple wait_for proceed';
--send insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
--sleep 1

--echo #Simulate execution tool operation
connection thr3;
delete from old_table where id = 1;
set debug_sync='now signal proceed';

connection default;
reap;
--echo #Verify data consistency
select * from old_table order by id;
select * from new_table order by id;

--echo # clean
drop table old_table;
drop table new_table;

--echo 
--echo # case3: insert select first, then update primary key or unique key
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo #View data
select * from old_table order by id;
select * from new_table order by id;

--echo #Create update trigger
--disable_warnings
drop trigger if exists pt_osc_update;
--enable_warnings
delimiter $$;
create trigger pt_osc_update
after update on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    delete ignore from new_table
    where !(old.id <=> new.id) and new_table.id <=> old.id;
    replace into new_table (id, a) values (new.id, new.a);
end $$
delimiter ;$$

--echo #Execute concurrent DML operations
set debug_sync='before_lock_tuple wait_for proceed';
--send update old_table set id=2 where id=1;
--sleep 1

--echo #Simulate execution tool operation
connection thr3;
insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
set debug_sync='now signal proceed';

connection default;
reap;
--echo #Verify data consistency
select * from old_table order by id;
select * from new_table order by id;

--echo # clean
drop table old_table;
drop table new_table;

--echo 
--echo # case4: update primary key or unique key first, then  insert select
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo 
--echo #View data
--echo 
select * from old_table order by id;
select * from new_table order by id;

--echo 
--echo #Create update trigger
--echo 
--disable_warnings
drop trigger if exists pt_osc_update;
--enable_warnings
delimiter $$;
create trigger pt_osc_update
after update on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    delete ignore from new_table
    where !(old.id <=> new.id) and new_table.id <=> old.id;
    replace into new_table (id, a) values (new.id, new.a);
end $$
delimiter ;$$

--echo 
--echo #Simulate execution tool operation
--echo 
set debug_sync='before_lock_tuple wait_for proceed';
--send insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
--sleep 1

--echo 
--echo #Execute concurrent DML operations
--echo 
connection thr3;
update old_table set id=2 where id=1;
set debug_sync='now signal proceed';

--echo 
--echo ##Verify data consistency
--echo 
connection default;
reap;
select * from old_table order by id;
select * from new_table order by id;

--echo 
--echo # clean
--echo 
drop table old_table;
drop table new_table;

--echo 
--echo # case5: insert select first, then update not primary key or unique key
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo #View data
select * from old_table order by id;
select * from new_table order by id;

--echo #Create update trigger
--disable_warnings
drop trigger if exists pt_osc_update;
--enable_warnings
delimiter $$;
create trigger pt_osc_update
after update on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    delete ignore from new_table
    where !(old.id <=> new.id) and new_table.id <=> old.id;
    replace into new_table (id, a) values (new.id, new.a);
end $$
delimiter ;$$

--echo #Execute concurrent DML operations
set debug_sync='before_lock_tuple wait_for proceed';
--send update old_table set a=2 where id=1;
--sleep 1

--echo #Simulate execution tool operation
connection thr3;
insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
set debug_sync='now signal proceed';

connection default;
reap;
--echo #Verify data consistency
select * from old_table order by id;
select * from new_table order by id;

--echo # clean
drop table old_table;
drop table new_table;

--echo 
--echo # case6: update not primary key and unique key first, then  insert select
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo 
--echo #View data
--echo 
select * from old_table order by id;
select * from new_table order by id;

--echo 
--echo #Create update trigger
--echo 
--disable_warnings
drop trigger if exists pt_osc_update;
--enable_warnings
delimiter $$;
create trigger pt_osc_update
after update on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    delete ignore from new_table
    where !(old.id <=> new.id) and new_table.id <=> old.id;
    replace into new_table (id, a) values (new.id, new.a);
end $$
delimiter ;$$

--echo 
--echo #Simulate execution tool operation
--echo 
set debug_sync='before_lock_tuple wait_for proceed';
--send insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
--sleep 1

--echo 
--echo #Execute concurrent DML operations
--echo 
connection thr3;
update old_table set a=2 where id=1;
set debug_sync='now signal proceed';

--echo 
--echo ##Verify data consistency
--echo 
connection default;
reap;
select * from old_table order by id;
select * from new_table order by id;

--echo 
--echo # clean
--echo 
drop table old_table;
drop table new_table;

--echo 
--echo # case7: insert select first, then insert
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo #View data
select * from old_table order by id;
select * from new_table order by id;

--echo #Create insert trigger
--disable_warnings
drop trigger if exists pt_osc_insert;
--enable_warnings
delimiter $$;
create trigger pt_osc_insert
after insert on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    replace into new_table (id, a) values (new.id, new.a);
end $$
delimiter ;$$

--echo #Execute concurrent DML operations
set debug_sync='before_lock_tuple wait_for proceed';
--send insert old_table values(2,2);
--sleep 1

--echo #Simulate execution tool operation
connection thr3;
insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
set debug_sync='now signal proceed';

connection default;
reap;
--echo #Verify data consistency
select * from old_table order by id;
select * from new_table order by id;

--echo # clean
drop table old_table;
drop table new_table;

--echo 
--echo # case8: insert first, then insert select
--echo 
--disable_warnings
drop table if exists old_table;
--enable_warnings
create table old_table (
  id int primary key,
  a int
) engine=Dstore character set utf8mb4 collate utf8mb4_0900_ai_ci;
insert into old_table values (1,1);
--disable_warnings
drop table if exists new_table;
--enable_warnings
create table new_table like old_table;
alter table new_table add column b int;

--echo 
--echo #View data
--echo 
select * from old_table order by id;
select * from new_table order by id;

--echo 
--echo #Create update trigger
--echo 
--disable_warnings
drop trigger if exists pt_osc_insert;
--enable_warnings
delimiter $$;
create trigger pt_osc_insert
after insert on old_table
for each row
begin
    declare continue handler for 1146 begin end;
    replace into new_table (id, a) values (new.id, new.a);
end $$
delimiter ;$$

--echo 
--echo #Simulate execution tool operation
--echo 
set debug_sync='before_lock_tuple wait_for proceed';
--send insert low_priority ignore into `new_table` (`id`, `a`) select `id`, `a` from `old_table` lock in share mode;
--sleep 1

--echo 
--echo #Execute concurrent DML operations
--echo 
connection thr3;
insert old_table values(2,2);
set debug_sync='now signal proceed';

--echo 
--echo ##Verify data consistency
--echo 
connection default;
reap;
select * from old_table order by id;
select * from new_table order by id;

--echo 
--echo # clean
--echo 
drop table old_table;
drop table new_table;
disconnect thr3;
