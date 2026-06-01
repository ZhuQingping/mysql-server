SET @saved_rds_extended_partitions_enabled=@@rds_extended_partitions_enabled;
SET global rds_extended_partitions_enabled = true;

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(amount)
subpartition by $opt_subpart_linear key(date)
(
  partition dp0 (
    subpartition p0,
    subpartition p1,
    subpartition p2
  ),
  partition dp1
  (
    subpartition p3,
    subpartition p4,
    subpartition p5
  )
);
show create table t1;
insert into t1(date, amount) values('2021-01-01',1),
  ('2021-01-02',2),('2021-01-03',3),('2021-01-04',4),
  ('2021-01-05',5),('2021-01-06',6),('2021-09-01',1),
  ('2021-09-03',2),('2021-09-05',3),('2021-09-07',4),
  ('2021-09-21',5),('2021-09-23',6);

select * from t1;
select * from t1 where date in ('2021-01-02', '2021-01-04', '2021-09-23');
explain select * from t1 where date in ('2021-01-02', '2021-01-04', '2021-09-23');
select * from t1 where date in ('2021-01-02', '2021-01-04', '2021-09-23') and amount = 4;
explain select * from t1 where date in ('2021-01-02', '2021-01-04', '2021-09-23') and amount = 4;
drop table t1;

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(dept_no) partitions 2
subpartition by $opt_subpart_linear key(date)
(
  partition dp0,
  partition dp1
);
show create table t1;
--source suite/parts/inc/query_partition_info.inc
drop table t1;

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(amount) partitions 2
subpartition by $opt_subpart_linear key(date) subpartitions 3
(
  partition dp0,
  partition dp1
);
show create table t1;
--source suite/parts/inc/query_partition_info.inc
drop table t1;

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(amount) partitions 2
subpartition by $opt_subpart_linear key(date) subpartitions 3;
show create table t1;
--source suite/parts/inc/query_partition_info.inc
drop table t1;

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(amount)
subpartition by $opt_subpart_linear key(date) subpartitions 3
(
  partition dp0,
  partition dp1
);
show create table t1;
--source suite/parts/inc/query_partition_info.inc
drop table t1;

eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(amount)
subpartition by $opt_subpart_linear key(date) subpartitions 3
(
  partition dp0 (
    subpartition p0,
    subpartition p1,
    subpartition p2
  ),
  partition dp1
  (
    subpartition p3,
    subpartition p4,
    subpartition p5
  )
);
show create table t1;
--source suite/parts/inc/query_partition_info.inc
drop table t1;

--error ER_PARSE_ERROR
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash(amount)
subpartition by $opt_subpart_linear key(date) subpartitions 3
(
  partition dp0 (
    subpartition p0,
    subpartition p1
  )
);

# Test : partition name must be unique within the table.
--error ER_PARSE_ERROR
eval create table t1 (a int, b int)
partition by $opt_part_linear hash (a) partitions 2
subpartition by $opt_subpart_linear key (b)
(
  partition p0 (
    subpartition partno0,
    subpartition partno0
  ),
  partition p1 (
    subpartition partno0,
    subpartition partno0
  ),
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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0,
    subpartition partno1
  ),
  partition p1 (
    subpartition partno0,
    subpartition partno2
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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0,
    subpartition partno1
  ),
  partition p1 (
    subpartition p0,
    subpartition partno2
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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0,
    subpartition partno1
  ),
  partition p1 (
    subpartition partno2
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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0
  ),
  partition p1 (
    subpartition partno2,
    subpartition partno1
  )
);

--echo #
--echo # DDL for key-key
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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition p0sp0,
    subpartition p0sp1
  ),
  partition p1 (
    subpartition p1sp0,
    subpartition p1sp1
  )
);

--error ER_ADD_PARTITION_SUBPART_ERROR
alter table t1 add partition (
  partition p2 (
    subpartition p2sp0
  )
);

--error ER_PARTITION_WRONG_VALUES_ERROR
alter table t1 add partition (
  partition p2 (
    subpartition p2sp0,
    subpartition p2sp1 values in (6)
  )
);

alter table t1 add partition (
  partition p2 (
    subpartition p2sp0,
    subpartition p2sp1
  )
);
show create table t1;
--source suite/parts/inc/query_partition_info.inc

insert into t1(dept_no, part_no) values (23, 4), (24, 1);
select * from t1;
alter table t1 add partition (
  partition p3
);
show create table t1;
--source suite/parts/inc/query_partition_info.inc
select * from t1;
drop table t1;

# Test: alter table , converting a Non-partitioned Table to a Partitioned Table

create table t1 (
`a` int ,
`b` int ,
primary Key(a, b));

insert into t1 values(1,1),(2,1),(3,1),(4,1),(111,111),(3333,333);

eval alter table t1 partition by $opt_part_linear hash(a) subpartition by $opt_subpart_linear key(b) (
  partition p0 (
    subpartition p0sp0,
    subpartition p0sp1,
    subpartition p0sp2
  ),
  partition p1 (
    subpartition p1sp0,
    subpartition p1sp1,
    subpartition p1sp2
  )
);
show create table t1;
select * from t1;
select * from t1 partition(p0);
select * from t1 partition(p1);
select * from t1 where a = 111 and b = 111;
explain select * from t1 where a = 111 and b = 111;
select * from t1 partition(p0sp0);

# change part number
eval alter table t1 partition by $opt_part_linear hash(a) partitions 2 subpartition by $opt_subpart_linear key(b) subpartitions 2;
select * from t1;
select * from t1 partition(p0);
select * from t1 partition(p1);
select * from t1 where a = 111 and b = 111;
explain select * from t1 where a = 111 and b = 111;
select * from t1 partition(p0sp0);

# Test: drop partition
# only for list/range partition table
--error ER_ONLY_ON_RANGE_LIST_PARTITION
alter table t1 drop partition p1;
--error ER_ONLY_ON_RANGE_LIST_PARTITION
alter table t1 drop partition p0sp0;

# Test: truncate partition
select * from t1 partition(p0sp0);
alter table t1 truncate partition p0sp0;
show create table t1;
select * from t1 partition(p0sp0);

insert into t1 values(1, 111),(2, 333);
select * from t1 partition(p0);
alter table t1 truncate partition p0;
select * from t1 partition(p0);
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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0,
    subpartition partno1,
    subpartition partno2
  ),
  partition p1 (
    subpartition partno3,
    subpartition partno4,
    subpartition partno5
  ),
  partition p2 (
    subpartition partno6,
    subpartition partno7,
    subpartition partno8
  )
);
insert into t1(dept_no, part_no) values(1,1),(5,2),(6,3),(6,4),(9,5),(9,6);
insert into t1(dept_no, part_no) values(13,1),(13,2),(13,3),(13,4),(13,5),(13,6);
insert into t1(dept_no, part_no) values(20,1),(52,2),(52,3),(52,4),(52,5),(52,6);
select * from t1 partition(p1);
select * from t1 partition(partno3);
select * from t1 partition(partno4);
select * from t1 partition(partno5);

alter table t1 reorganize partition p1 into (
  partition p1_1 (
    subpartition partno3_1,
    subpartition partno4_1,
    subpartition partno5_1
  )
);
select * from t1 partition(p1_1);
select * from t1 partition(partno3_1);
select * from t1 partition(partno4_1);
select * from t1 partition(partno5_1);

--error ER_REORG_HASH_ONLY_ON_SAME_NO
alter table t1 reorganize partition p1,p2 into (
  partition p1 (
    subpartition partno3,
    subpartition partno4,
    subpartition partno5
  )
);
drop table t1;

# Test: exchange partition
eval create table t1
(
  dept_no     int,
  part_no     int,
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0,
    subpartition partno1,
    subpartition partno2
  ),
  partition p1 (
    subpartition partno3,
    subpartition partno4,
    subpartition partno5
  ),
  partition p2 (
    subpartition partno6,
    subpartition partno7,
    subpartition partno8
  )
);
insert into t1(dept_no, part_no) values(1,1),(5,2),(6,3),(6,4),(9,5),(9,6);
insert into t1(dept_no, part_no) values(13,1),(13,2),(13,3),(13,4),(13,5),(13,6);
insert into t1(dept_no, part_no) values(20,1),(52,2),(52,3),(52,4),(52,5),(52,6);

create table t2 like t1;
alter table t2 remove partitioning;

select * from t1 partition(partno7);
alter table t1 exchange partition partno7 with table t2;
select * from t1 partition(partno7);
select * from t2;

delete from t2;
insert into t2(dept_no, part_no) values(30,7), (30,6), (30,5), (31,4), (31,5), (31,3), (29, 1),(29,2),(29,3);
insert into t1(dept_no, part_no) values(9, 6);
--error ER_ROW_DOES_NOT_MATCH_PARTITION
alter table t1 exchange partition partno7 with table t2;

alter table t1 exchange partition partno7 with table t2 without validation;
select * from t1 partition(partno7);
select * from t2;

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
partition by $opt_part_linear hash (dept_no)
subpartition by $opt_subpart_linear key (part_no)
(
  partition p0 (
    subpartition partno0,
    subpartition partno1,
    subpartition partno2
  ),
  partition p1 (
    subpartition partno3,
    subpartition partno4,
    subpartition partno5
  ),
  partition p2 (
    subpartition partno6,
    subpartition partno7,
    subpartition partno8
  )
);
insert into t1(dept_no, part_no) values(1,1),(5,2),(6,3),(6,4),(9,5),(9,6);
insert into t1(dept_no, part_no) values(13,1),(13,2),(13,3),(13,4),(13,5),(13,6);
insert into t1(dept_no, part_no) values(20,1),(52,2),(52,3),(52,4),(52,5),(52,6);

alter table t1 analyze partition partno3, partno8;
select * from t1 partition(partno3);

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
select * from t1;
drop table t1;

# Verify index
eval create table t1(a int, b int) partition by $opt_part_linear hash(a) subpartition by $opt_subpart_linear key(b) (
partition p1 (
  subpartition p1sp0,
  subpartition p1sp1),
partition p2 (
  subpartition p2sp0,
  subpartition p2sp1)
);
insert into t1 values(3,1);
insert into t1 values(4,7);
insert into t1 values(5,4);
insert into t1 values(6,20);
insert into t1 values(7,3);
insert into t1 values(8,9);
insert into t1 values(6,22);
select * from t1 where a < 5;
explain select * from t1 where a < 5;
alter table t1 add index idx(a);
select * from t1 where a < 5;
explain select * from t1 where a < 5;
select * from t1 where a = 8;
alter table t1 add index idx1(a, b);
select * from t1 where a = 6 and b = 20;
explain select * from t1 where a = 6 and b = 20;
select * from t1 where b = 20;
explain select * from t1 where b = 20;
alter table t1 drop index idx;
alter table t1 drop index idx1;
alter table t1 add index idx2(b);
select * from t1 where a = 7;
select * from t1 where b = 3;
drop table t1;

# Test partitioning interval analysis
eval create table t1(a int, b int) partition by $opt_part_linear hash(a) subpartition by $opt_subpart_linear key(b) (
partition p1 (
  subpartition p1sp0,
  subpartition p1sp1),
partition p2 (
  subpartition p2sp0,
  subpartition p2sp1)
);
insert into t1 values(3,1);
insert into t1 values(4,7);
insert into t1 values(5,4);
insert into t1 values(6,20);
insert into t1 values(7,3);
insert into t1 values(8,9);
insert into t1 values(6,22);
select * from t1 order by b;
select * from t1 where b > 3 and b < 9 order by b;
explain select b from t1 where b > 3 and b < 9 order by b;
select * from t1 where b > 4 order by b;
explain select b from t1 where b > 4 order by b;
select * from t1 where b is null order by b;
explain select b from t1 where b is null order by b;
select * from t1 where b > 19 and b < 22 order by b;

select * from t1 order by a;
select * from t1 where a > 3 and a < 9 order by a;
explain select b from t1 where a > 3 and a < 9 order by a;
select * from t1 where a > 7 order by a;
explain select * from t1 where a > 7 order by a;
select * from t1 where a is null order by a;
explain select b from t1 where a is null order by a;

select * from t1 where a > 4 and (b > 3 and b < 7);
select * from t1 where a > 4 or (b > 3 and b < 7);
explain select * from t1 where a > 4 and (b > 3 and b < 7);
explain select * from t1 where a > 4 or (b > 3 and b < 7);

drop table t1;

# Test null value
eval create table t1
(
  dept_no     int,
  part_no     int default(0),
  country     varchar(20),
  date        date,
  amount      int
)
partition by $opt_part_linear hash (dept_no) partitions 2
subpartition by $opt_subpart_linear key (part_no) subpartitions 3;
insert into t1(part_no) values(null);
insert into t1(dept_no) values(null);
select * from t1 where dept_no is null;
explain select * from t1 where dept_no is null;
select * from t1 where part_no is null;
explain select * from t1 where part_no is null;
drop table t1;

eval create table t1(a int, b int) partition by $opt_part_linear hash(a) partitions 3 subpartition by $opt_subpart_linear key(b) subpartitions 4;
insert into t1 values(2,7),(5,5),(5,6),(6,7),(7,2),(8,20),(6,7),(3,4),(1,6),(8,21);
select b from t1 where b is null;
explain select b from t1 where b is null;
insert into t1 values(null,4),(12,null);
select b from t1 where b is null;
explain select b from t1 where b is null;
select * from t1 where a is null;
explain select * from t1 where a is null;
drop table t1;

eval create table t1(a int, b int) partition by $opt_part_linear hash(a) partitions 3 subpartition by $opt_subpart_linear key(b) subpartitions 4;
insert into t1 values(3,11),(4,2),(5,1),(6,null);
insert into t1 values(7,7),(8,11),(6,20),(3,null),(1,null),(5,5);
insert into t1 values(null,0);
select * from t1 where b is null;
explain select * from t1 where b is null;
insert into t1 values(17,2),(14,null),(11,6),(20,32),(17,3);
select b from t1 where b is null;
explain select b from t1 where b is null;
select * from t1 where a is null and b is null;
explain select * from t1 where a is null and b is null;
select * from t1 where a < 20 and b is null;
explain select * from t1 where a < 20 and b is null;
select * from t1 where b > 7 and b < 28;
explain select * from t1 where b > 7 and b < 28;
select * from t1 where b > 7 and b < 28 and a is null;
explain select * from t1 where b > 7 and b < 28 and a is null;
select * from t1 where b > 7 and b < 28 and a > 5;
explain select * from t1 where b > 7 and b < 28 and a > 5;
drop table t1;

# DML
eval create table t1(a int, b int) partition by $opt_part_linear hash(a) partitions 3 subpartition by $opt_subpart_linear key(b) subpartitions 2;
insert into t1 values(3,11),(4,2),(5,1),(6,1);
insert into t1 values(7,7),(8,11),(6,20),(3,null),(1,null),(5,5);
insert into t1 values(null,0);

explain select * from t1 where a = 6 and b = 1;
select * from t1 partition(p1sp0);
select * from t1 partition(p1sp1);
update t1 set b = 6 where a = 6 and b = 1;
explain select * from t1 where a = 6 and b = 6;
select * from t1 where a = 6 and b = 6;
select * from t1 partition(p1sp0);
select * from t1 partition(p1sp1);

select * from t1 partition(p1);
select * from t1 partition(p2);
update t1 set a = 8 where a = 6;
select * from t1 partition(p1);
select * from t1 partition(p2);

delete from t1 where a = 8;
select * from t1 partition(p2);
drop table t1;

# column type
eval create table t1(c1 int, c2 int, c3 char(5)) partition by $opt_part_linear hash(c1) partitions 2 subpartition by $opt_subpart_linear key(c2, c3) subpartitions 2;
insert into t1 values(1,1,'abc');
insert into t1 values(2,2,'abcd');
insert into t1 values(3,2,'xian');
insert into t1 values(4,3,'hua');
select * from t1;
select * from t1 partition(p0);
select * from t1 partition(p0sp0);
select * from t1 partition(p0sp1);
select * from t1 partition(p1);
select * from t1 partition(p1sp0);
select * from t1 partition(p1sp1);
select * from t1 where c1 = 1;
explain select * from t1 where c1 = 1;
select * from t1 where c2 = 1;
explain select * from t1 where c2 = 1;
select * from t1 where c2 = 1 and c3 = 'abc';
explain select * from t1 where c2 = 1 and c3 = 'abc';
select * from t1 where c3 > 'h';
select * from t1 where c3 > 'h' and c2 = 3;
drop table t1;

#reset
SET global rds_extended_partitions_enabled = @saved_rds_extended_partitions_enabled;
