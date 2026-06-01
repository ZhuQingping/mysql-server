eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);

insert into t1(part_no, amount) values(1,1),(1,2),(1,3),(1,4),(1,5),(1,6);
insert into t1(part_no, amount) values(3,1),(3,2),(3,3),(3,4),(3,5),(3,6);
insert into t1(part_no, amount) values(5,1),(5,2),(5,3),(5,4),(5,5),(5,6);

select * from t1 order by amount, part_no;
--replace_column 10 # 11 #
explain select * from t1 where part_no = 1;
--replace_column 10 # 11 #
explain select * from t1 where part_no = 1 and amount = 4;
select * from t1 where amount = 4 order by amount, part_no;
--replace_column 10 # 11 #
explain select * from t1 where amount = 4;

--error ER_NO_PARTITION_FOR_GIVEN_VALUE
insert into t1(part_no, amount) values(1,8);

--error ER_NO_PARTITION_FOR_GIVEN_VALUE
insert into t1(part_no, amount) values(8,1);

select part_no, amount from t1 partition(partno7) order by amount, part_no;
update t1 partition(partno7) set part_no = part_no + 1;
select part_no, amount from t1 partition(partno7) order by amount, part_no;
delete from t1 partition(partno7);
select part_no, amount from t1 partition(partno7) order by amount, part_no;

drop table t1;

# Test : subpartitions

--error ER_PARTITIONS_MUST_BE_DEFINED_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(amount)
subpartition by list $subpart_opt_columns(part_no) subpartitions 3
(
  partition p0 values in (1, 2),
  partition p1 values in (3, 4),
  partition p2 values in (5, 6)
);

--error ER_PARSE_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(amount)
subpartition by list $subpart_opt_columns(part_no) subpartitions 2
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);

--error ER_PARSE_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount) subpartitions 2
subpartition by list $subpart_opt_columns (part_no) subpartitions 2
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no) subpartitions 3
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);
show create table t1;
drop table t1;

# Test : partition name must be unique within the table.
--error ER_SAME_NAME_PARTITION
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno0 values in (3, 4)
  )
);

--error ER_SAME_NAME_PARTITION
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4)
  ),
  partition p1 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno2 values in (3, 4)
  )
);

--error ER_SAME_NAME_PARTITION
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4)
  ),
  partition p1 values in (1, 2)(
    subpartition p0 values in (1, 2),
    subpartition partno2 values in (3, 4)
  )
);

# Test : number of subpartitions must be same for every part_defination

--error ER_PARSE_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4)
  ),
  partition p1 values in (1, 2)(
    subpartition partno2 values in (1, 2)
  )
);

--error ER_PARSE_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2)
  ),
  partition p1 values in (1, 2)(
    subpartition partno2 values in (1, 2),
    subpartition partno1 values in (3, 4)
  )
);

# Test : must be defining subpartition defination for list type
--error ER_PARTITIONS_MUST_BE_DEFINED_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)
);

--error ER_PARSE_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2) (
    subpartition p0sp0 values in (1)
  ),
  partition p1 values in (3, 4)
);

--error ER_PARTITIONS_MUST_BE_DEFINED_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2),
  partition p1 values in (3, 4)(
    subpartition p0sp0 values in (1)
  )
);

# Test: list type's value defination is values in 

--error ER_PARTITION_WRONG_VALUES_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2) (
    subpartition p0sp0 values less than(3)
  )
);

--echo #
--echo # DDL
--echo #

# Test: add partition
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns (amount)
subpartition by list $subpart_opt_columns (part_no)
(
  partition p0 values in (1, 2)(
    subpartition p0sp0 values in (1, 2),
    subpartition p0sp1 values in (3, 4)
  ),
  partition p1 values in (3, 4)(
    subpartition p1sp0 values in (1, 2),
    subpartition p1sp1 values in (3, 4)
  )
);

--error ER_ADD_PARTITION_SUBPART_ERROR
alter table t1 add partition (
  partition p2 values in (5, 6) (
    subpartition p2sp0 values in (5, 6)
  )
);

--error ER_PARTITION_WRONG_VALUES_ERROR
alter table t1 add partition (
  partition p2 values in (5, 6) (
    subpartition p2sp0 values less than (5),
    subpartition p2sp0 values in (6)
  )
);

alter table t1 add partition (
  partition p2 values in (5, 6) (
    subpartition p2sp0 values in (1, 2),
    subpartition p2sp1 values in (3, 4)
  )
);

show create table t1;
insert into t1(part_no, amount) values (3, 5), (4, 6);
select * from t1 partition(p2sp1) order by amount, part_no;
drop table t1;

# Test: converting a Non-partitioned Table to a Partitioned Table

create table t1 (
`a` int ,
`b` int ,
primary Key(a, b));

insert into t1 values(1,1),(2,1),(3,1),(4,1),(111,111),(3333,333);

eval alter table t1 partition by list $part_opt_columns (a) subpartition by list $subpart_opt_columns (b) (
  partition p0 values in (1, 2, 3, 4) (
    subpartition p0sp0 values in (1),
    subpartition p0sp1 values in (111),
    subpartition p0sp2 values in (333)
  ),
  partition p1 values in (111, 3333) (
    subpartition p1sp0 values in (1),
    subpartition p1sp1 values in (111),
    subpartition p1sp2 values in (333)
  )
);
show create table t1;

select * from t1 partition(p0sp0) order by a,b;
select * from t1 partition(p0) order by a,b;
select * from t1 partition(p1) order by a,b;

--error ER_NO_PARTITION_FOR_GIVEN_VALUE
eval alter table t1 partition by list $part_opt_columns(a) subpartition by list $subpart_opt_columns(b) (
  partition p0 values in (1, 2, 3, 4) (
    subpartition p0sp0 values in (1),
    subpartition p0sp1 values in (111),
    subpartition p0sp2 values in (333)
  ),
  partition p1 values in (111) (
    subpartition p1sp0 values in (1),
    subpartition p1sp1 values in (111),
    subpartition p1sp2 values in (3)
  )
);

# Test: drop partition

--error ER_DROP_PARTITION_NON_EXISTENT
alter table t1 drop partition p0sp0;
# Currently, we only support copy algorithm for partitioned tables in DStore.
# The default for MySQL for DROP PARTITION is the inplace algorithm.
# The behavior for inplace and copy algorithm is different for drop partition.
#alter table t1 drop partition p1;
show create table t1;
select * from t1 order by a,b;

--echo # Test for truncate partition

alter table t1 truncate partition p0sp0;
show create table t1;
select * from t1 partition(p0sp0) order by a,b;

insert into t1 values(1, 111),(2, 333);
select * from t1 partition(p0) order by a,b;
alter table t1 truncate partition p0;
select * from t1 partition(p0) order by a,b;
drop table t1;

# Test: for reorganize partition

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(amount)
subpartition by list $subpart_opt_columns(part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);
insert into t1(part_no, amount) values(1,1),(1,2),(1,3),(1,4),(1,5),(1,6);
insert into t1(part_no, amount) values(3,1),(3,2),(3,3),(3,4),(3,5),(3,6);
insert into t1(part_no, amount) values(5,1),(5,2),(5,3),(5,4),(5,5),(5,6);

select * from t1 partition(partno3) order by amount, part_no;
select * from t1 partition(partno4) order by amount, part_no;
select * from t1 partition(partno5) order by amount, part_no;

alter table t1 reorganize partition p1 into (
  partition p1 values in (3, 4) (
    subpartition partno3 values in (1),
    subpartition partno4 values in (2),
    subpartition partno5 values in (3, 4, 5, 6)
  )
);
show create table t1;
select * from t1 partition(partno3) order by amount, part_no;
select * from t1 partition(partno4) order by amount, part_no;
select * from t1 partition(partno5) order by amount, part_no;

alter table t1 reorganize partition p1,p2 into (
  partition p1 values in (3, 4, 5, 6) (
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  )
);
show create table t1;
select * from t1 partition(p1) order by amount, part_no;
drop table t1;

# Error scenario
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(part_no)
subpartition by list $subpart_opt_columns(amount)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);
insert into t1(part_no, amount) values(5, 1),(5,3),(5,6);

# Fails due to dstore using copy algorithm,
# while INNODB instant. Should pass after we
# implement instant algorithm for dstore.
#alter table t1 reorganize partition p2 into (
#  partition p2 values in (5, 6) (
#    subpartition partno6 values in (1, 2),
#    subpartition partno7 values in (3, 4),
#    subpartition partno8 values in (7, 8)
#  )
#);
# leading to data loss
select * from t1 order by amount, part_no;
drop table t1;

# Test: reorganize partition

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(amount)
subpartition by list $subpart_opt_columns(part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);
insert into t1(part_no, amount) values(1,1),(1,2),(1,3),(1,4),(1,5),(1,6);
insert into t1(part_no, amount) values(3,1),(3,2),(3,3),(3,4),(3,5),(3,6);
insert into t1(part_no, amount) values(5,1),(5,2),(5,3),(5,4),(5,5),(5,6);

create table t2 like t1;
alter table t2 remove partitioning;

alter table t1 exchange partition partno7 with table t2;
select * from t1 partition(partno7) order by amount, part_no;
select * from t2 order by amount, part_no;

delete from t2;
insert into t2(part_no, amount) values(10, 5), (4, 6);
insert into t1(part_no, amount) values(3, 5);
--error ER_NO_PARTITION_FOR_GIVEN_VALUE
alter table t1 exchange partition partno7 with table t2;

alter table t1 exchange partition partno7 with table t2 without validation;
select * from t1 partition(partno7) order by amount, part_no;
select * from t2 order by amount, part_no;

drop table t1, t2;

# Test: analyze partition
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(amount)
subpartition by list $subpart_opt_columns(part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  ),
  partition p2 values in (5, 6)(
    subpartition partno6 values in (1, 2),
    subpartition partno7 values in (3, 4),
    subpartition partno8 values in (5, 6)
  )
);
insert into t1(part_no, amount) values(1,1),(1,2),(1,3),(1,4),(1,5),(1,6);
insert into t1(part_no, amount) values(3,1),(3,2),(3,3),(3,4),(3,5),(3,6);
insert into t1(part_no, amount) values(5,1),(5,2),(5,3),(5,4),(5,5),(5,6);

alter table t1 analyze partition partno3, partno8;
select * from t1 partition(partno3) order by amount, part_no;

# Test: check partition
select * from t1 partition(partno1, partno4, partno5);
alter table t1 check partition partno1, partno4, partno5;
select * from t1 partition(partno1, partno4, partno5);

# Test: optimize partition
select * from t1 partition(partno3, partno6, partno8);
alter table t1 optimize partition partno3, partno6, partno8;
select * from t1 partition(partno3, partno6, partno8);

# Test: rebuild partition, only for partiton_name, not for subpartition_name
select * from t1 partition(p0);
alter table t1 rebuild partition p0;
select * from t1 partition(p0);
--error ER_DROP_PARTITION_NON_EXISTENT
alter table t1 rebuild partition partno2;

# Test: repair partition
select * from t1 partition(partno3);
alter table t1 repair partition partno3;
select * from t1 partition(partno3);

# Test: remove partitioning
alter table t1 remove partitioning;
show create table t1;

drop table t1;

# Test: DDL for partition table
eval create table t_list_list
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by list $part_opt_columns(amount)
subpartition by list $subpart_opt_columns(part_no)
(
  partition p0 values in (1, 2)(
    subpartition partno0 values in (1, 2),
    subpartition partno1 values in (3, 4),
    subpartition partno2 values in (5, 6)
  ),
  partition p1 values in (3, 4)(
    subpartition partno3 values in (1, 2),
    subpartition partno4 values in (3, 4),
    subpartition partno5 values in (5, 6)
  )
);
alter table t_list_list add column col1 date;
--error ER_NO_PARTITION_FOR_GIVEN_VALUE
insert into t_list_list(col1) values('2031-02-01');
insert into t_list_list(part_no, amount, col1) values(1,1,'2023-02-01');
show create table t_list_list;
create index idx1 on t_list_list(dept_no);
show indexes from t_list_list;
drop table t_list_list;

# Verify index
eval create table t1(a int, b int) partition by list $part_opt_columns(a) subpartition by list $subpart_opt_columns(b) (
partition p1 values in (3,4,5) (
  subpartition p1sp0 values in (1,2,3),
  subpartition p1sp1 values in (4,5,6)),
partition p2 values in (6,7,8) (
  subpartition p2sp0 values in (7,8,9),
  subpartition p2sp1 values in (20,21,22))
);
insert into t1 values(3,2),(4,5),(5,3),(6,7),(7,20),(8,21),(6,9);
--replace_column 10 # 11 #
explain select * from t1 where a < 5;
alter table t1 add index idx(a);
select * from t1 where a < 5 order by a,b;
--replace_column 10 # 11 #
explain select * from t1 where a < 5;
select * from t1 where a = 8 order by a,b;
alter table t1 add index idx1(a, b);
select * from t1 where a = 5 and b = 3 order by a,b;
--replace_column 10 # 11 #
explain select * from t1 where a = 5 and b = 3;
--replace_column 10 # 11 #
explain select * from t1 where b = 20;
select * from t1 where b = 20 order by a,b;
alter table t1 drop index idx;
alter table t1 drop index idx1;
alter table t1 add index idx2(b);
select * from t1 where a = 7 order by a,b;
select * from t1 where b = 3 order by a,b;
drop table t1;

# Test partitioning interval analysis
eval create table t1(a int, b int) partition by list $part_opt_columns(a) subpartition by list $subpart_opt_columns(b) (
partition p1 values in (5,3,4) (
  subpartition p1sp0 values in (1,2,3),
  subpartition p1sp1 values in (4,5,6)),
partition p2 values in (7,8,6) (
  subpartition p2sp0 values in (7,8,9),
  subpartition p2sp1 values in (20,21,22))
);
insert into t1 values(3,2),(4,5),(5,3),(6,7),(7,20),(8,21),(6,9);
select b from t1 order by a,b;
select b from t1 where b > 3 and b < 9 order by a,b;
--replace_column 10 # 11 #
explain select b from t1 where b > 3 and b < 9;
select b from t1 where b > 4 order by a,b;
--replace_column 10 # 11 #
explain select b from t1 where b > 4;
select b from t1 where b is null order by a,b;
--replace_column 10 # 11 #
explain select b from t1 where b is null;
select b from t1 where b > 19 and b < 22 order by a,b;
drop table t1;

# Out of order
eval create table t1(a int, b int) partition by list $part_opt_columns(a) subpartition by list $subpart_opt_columns(b) (
partition p1 values in (3,7,8,33) (
  subpartition p1sp0 values in (1,10,6,21),
  subpartition p1sp1 values in (3,4,11,23)),
partition p2 values in (4,9,21,6,45) (
  subpartition p2sp0 values in (2,9,12),
  subpartition p2sp1 values in (1,3,10,20,8))
);
insert into t1 values(3,1),(7,10),(8,6),(33,3),(8,11),(8,4),(33,23);
insert into t1 values(4,1),(21,10),(9,2),(9,9),(6,20),(45,20),(21,12);

select * from t1 order by a,b;
select * from t1 where b > 11  and b < 21 order by a,b;
--replace_column 10 # 11 #
explain select * from t1 where b > 11  and b < 21 order by a,b;
drop table t1;
