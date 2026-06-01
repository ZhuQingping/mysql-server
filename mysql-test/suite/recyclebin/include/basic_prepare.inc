# prepare some user and db\tables with various ownership

set @saved_rds_recycle_bin_mode = @@GLOBAL.rds_recycle_bin_mode;
set global rds_recycle_bin_mode = ON;

connection default;
create database sbtest1;
create database sbtest2;
create table sbtest1.t1(id int);
create table sbtest2.t2(id int);
create table sbtest1.another_t1(uid int, name varchar(255));
create user 'test1'@'localhost' identified by '123456';
create user 'test2'@'localhost' identified by '123456';
create user 'my_root'@'localhost' identified by '123456';

GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, RELOAD, SHUTDOWN, PROCESS, FILE, REFERENCES, INDEX, ALTER, SHOW DATABASES,
CREATE TEMPORARY TABLES, LOCK TABLES, EXECUTE, CREATE VIEW, SHOW VIEW, CREATE ROUTINE, ALTER ROUTINE, CREATE USER, EVENT, TRIGGER,
CREATE TABLESPACE, CREATE ROLE, DROP ROLE ON *.* TO 'my_root'@'localhost';

GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP ON sbtest1.* TO 'test1'@'localhost';
GRANT SHOW DATABASES ON *.* TO 'test1'@'localhost';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP ON __recyclebin__.* TO 'test1'@'localhost';

GRANT SHOW DATABASES ON *.* TO 'test2'@'localhost';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP ON __recyclebin__.* TO 'test2'@'localhost';
