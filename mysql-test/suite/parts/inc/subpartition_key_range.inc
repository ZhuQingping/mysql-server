SET @saved_rds_extended_partitions_enabled=@@rds_extended_partitions_enabled;
SET global rds_extended_partitions_enabled = true;
--echo #
--echo # Verify hash-range
--echo #

eval CREATE TABLE t3 (
    id INT NOT NULL,
    store_id INT NOT NULL
)
PARTITION BY $hash_type KEY (store_id) 
SUBPARTITION BY RANGE(id) (
    PARTITION p0 (
		SUBPARTITION q1 VALUES LESS THAN (4),
		SUBPARTITION q2 VALUES LESS THAN (10)
	),
    PARTITION p1 (
		SUBPARTITION q3 VALUES LESS THAN (15),
		SUBPARTITION q4 VALUES LESS THAN (20)
	)
);
INSERT INTO t3 VALUES(1,3);
--error ER_NO_PARTITION_FOR_GIVEN_VALUE
INSERT INTO t3 VALUES(14, 9);
INSERT INTO t3 VALUES(9, 8);
ALTER TABLE t3 ADD PARTITION (PARTITION p3 (SUBPARTITION q5 VALUES LESS THAN(15), SUBPARTITION q6 VALUES LESS THAN(19)));
SHOW CREATE TABLE t3;
--error ER_ONLY_ON_RANGE_LIST_PARTITION
ALTER TABLE t3 DROP PARTITION p0;
SELECT * FROM t3;
DROP TABLE t3;

eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date, amount)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  ),
  PARTITION p_2022 (
    SUBPARTITION q1_2022 VALUES LESS THAN(4),
    SUBPARTITION q2_2022 VALUES LESS THAN(7),
    SUBPARTITION q3_2022 VALUES LESS THAN(10),
    SUBPARTITION q4_2022 VALUES LESS THAN(13)
  )
);
ALTER TABLE sales_range_range ADD PARTITION (
    PARTITION p_2025  (
    SUBPARTITION q1_2015 VALUES LESS THAN(4),
    SUBPARTITION q2_2015 VALUES LESS THAN(7),
    SUBPARTITION q3_2015 VALUES LESS THAN(10),
    SUBPARTITION q4_2015 VALUES LESS THAN(13)
  )
);
SHOW CREATE TABLE sales_range_range; 
--error ER_ONLY_ON_RANGE_LIST_PARTITION
ALTER TABLE sales_range_range DROP PARTITION p_2025;
SHOW CREATE TABLE sales_range_range; 
--error ER_REORG_HASH_ONLY_ON_SAME_NO
ALTER TABLE sales_range_range REORGANIZE PARTITION p_2021 INTO (
    PARTITION p_2021_1 (
    SUBPARTITION q1_2021_1 VALUES LESS THAN(4),
    SUBPARTITION q2_2021_1 VALUES LESS THAN(7),
    SUBPARTITION q3_2021_1 VALUES LESS THAN(10),
    SUBPARTITION q4_2021_1 VALUES LESS THAN(13)
  ),
    PARTITION p_2021_2 (
    SUBPARTITION q1_2021_2 VALUES LESS THAN(4),
    SUBPARTITION q2_2021_2 VALUES LESS THAN(7),
    SUBPARTITION q3_2021_2 VALUES LESS THAN(10),
    SUBPARTITION q4_2021_2 VALUES LESS THAN(13)
  )
);
ALTER TABLE sales_range_range REORGANIZE PARTITION p_2021 INTO (
    PARTITION p_2021_1 (
    SUBPARTITION q1_2021_1 VALUES LESS THAN(4),
    SUBPARTITION q2_2021_1 VALUES LESS THAN(7),
    SUBPARTITION q3_2021_1 VALUES LESS THAN(10),
    SUBPARTITION q4_2021_1 VALUES LESS THAN(13)
  )
);
SHOW CREATE TABLE sales_range_range; 
INSERT INTO sales_range_range(date) VALUES('2020-04-05'),('2020-08-06'),('2021-05-06');
SELECT * FROM sales_range_range;
# ALTER TABLE sales_range_range DISCARD PARTITION q1_2022 TABLESPACE;
# ALTER TABLE sales_range_range IMPORT PARTITION q1_2022 TABLESPACE;
# SELECT * FROM sales_range_range;
ALTER TABLE sales_range_range ANALYZE PARTITION q2_2021_1, q1_2022;
ALTER TABLE sales_range_range CHECK PARTITION q2_2021_1, q1_2022;
ALTER TABLE sales_range_range OPTIMIZE PARTITION q2_2021_1, q1_2022;
--echo "REBUILD only adapts to partition but not SUBPARTITION"
ALTER TABLE sales_range_range REBUILD PARTITION p_2021_1;
ALTER TABLE sales_range_range REPAIR PARTITION q2_2021_1, q1_2022;
ALTER TABLE sales_range_range TRUNCATE PARTITION q2_2021_1;
SELECT * FROM sales_range_range;
ALTER TABLE sales_range_range TRUNCATE PARTITION p_2021_1;
SELECT * FROM sales_range_range;
ALTER TABLE sales_range_range REMOVE PARTITIONING;
SHOW CREATE TABLE sales_range_range; 
SELECT * FROM sales_range_range;
DROP TABLE sales_range_range; 

# partition name must be unique within the table.
--error ER_SAME_NAME_PARTITION
eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7)
  )
);

# number of partitions must be same.
--error ER_PARSE_ERROR 
eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4)
  )
);

--error ER_PARTITIONS_MUST_BE_DEFINED_ERROR
eval CREATE TABLE sales_range_range3
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 
);
eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7)
  )
);
INSERT INTO sales_range_range(date) VALUES('2021-5-23'),('2010-2-23');
ALTER TABLE sales_range_range ANALYZE PARTITION p_2020, p_2021;
SELECT partition_name, SUBPARTITION_name, table_rows from information_schema.partitions where table_name = 'sales_range_range';
DROP TABLE sales_range_range;

# partition_definition can be different
eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020(
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(8),
    SUBPARTITION q2_2021 VALUES LESS THAN(13)
  )
);
DROP TABLE sales_range_range;


--error ER_FIELD_NOT_FOUND_PART_ERROR
eval CREATE TABLE sales_range_columns_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY (dept_no, part_no)
SUBPARTITION BY RANGE COLUMNS(am)
(
  PARTITION p1 (
    SUBPARTITION q1_2019 VALUES LESS THAN(4),
    SUBPARTITION q2_2019 VALUES LESS THAN(7)
  ),
  PARTITION p2 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  )
);

# DML
eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(8),
    SUBPARTITION q2_2021 VALUES LESS THAN(13)
  )
);

# insert 
INSERT INTO sales_range_range PARTITION (p_2020) VALUES (1, 1, 'china', '2012-01-12', 1);
--error ER_NO_PARTITION_FOR_GIVEN_VALUE 
INSERT INTO sales_range_range PARTITION (q2_2020) VALUES (1, 1, 'china', '2012-08-12', 1);
--error ER_NO_PARTITION_FOR_GIVEN_VALUE 
INSERT INTO sales_range_range PARTITION (p_2020,q2_2020) VALUES (1, 1, 'china', '2012-08-12', 1);

# update
SELECT * from sales_range_range;
UPDATE sales_range_range partition(q1_2020) SET dept_no = dept_no + 1;
SELECT * from sales_range_range;

# delete
DELETE FROM sales_range_range partition(q1_2020);
SELECT * from sales_range_range;

DROP TABLE sales_range_range;

# DDL
# add partition
eval CREATE TABLE sales_range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020(
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(8),
    SUBPARTITION q2_2021 VALUES LESS THAN(13)
  )
);

--error ER_ADD_PARTITION_SUBPART_ERROR
ALTER TABLE sales_range_range ADD PARTITION (
    PARTITION p_2022 (
        SUBPARTITION q1_2022 VALUES LESS THAN(4),
        SUBPARTITION q2_2022 VALUES LESS THAN(7),
        SUBPARTITION q2_2022 VALUES LESS THAN(10),
        SUBPARTITION q2_2022 VALUES LESS THAN(13)
    )
);

--error ER_SAME_NAME_PARTITION 
ALTER TABLE sales_range_range ADD PARTITION (
    partition p_2022 (
        SUBPARTITION q2_2022 VALUES LESS THAN(10),
        SUBPARTITION q2_2022 VALUES LESS THAN(13)
    )
);

--error ER_RANGE_NOT_INCREASING_ERROR
ALTER TABLE sales_range_range add partition (
    partition p_2022 (
        SUBPARTITION q1_2022 VALUES LESS THAN(15),
        SUBPARTITION q2_2022 VALUES LESS THAN(13)
    )
);

ALTER TABLE sales_range_range add partition (
    partition p_max (
        SUBPARTITION q1_max VALUES LESS THAN(4),
        SUBPARTITION q3_max VALUES LESS THAN(MAXVALUE) 
    )
);

ALTER TABLE sales_range_range add partition (
    partition p_2024 (
        SUBPARTITION q3_2025 VALUES LESS THAN(10),
        SUBPARTITION q4_2025 VALUES LESS THAN(13)
    )
);
DROP table sales_range_range;

# add partition .. without validation
CREATE TABLE t1 (
`a` int ,
`b` int ,
Primary Key(a, b));

INSERT INTO t1 values(1,1),(2,1),(3,1),(4,1),(111,111),(3333,333);

eval ALTER TABLE t1 partition by $hash_type KEY(a) (
partition p0,
partition p1,
partition p2
);
ALTER TABLE t1 ANALYZE PARTITION p0, p1, p2;
select partition_name, SUBPARTITION_name, TABLE_rows from information_schema.partitions where table_name = 't1';
SELECT * FROM t1 partition(p0);

eval ALTER TABLE t1 partition by $hash_type KEY(a) SUBPARTITION by range(b) (
    partition p0(
        SUBPARTITION p0sp0 VALUES LESS THAN(6),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    ),
    partition p1(
        SUBPARTITION p1sp0 VALUES LESS THAN(6),
        SUBPARTITION p1sp1 VALUES LESS THAN(1000)
    ),
    partition p2(
        SUBPARTITION p2sp0 VALUES LESS THAN(6),
        SUBPARTITION p2sp1 VALUES LESS THAN(1000)
    )
);
DROP TABLE t1;
# partition by

CREATE TABLE t1 (
`a` int ,
`b` int ,
Primary Key(a, b));
INSERT INTO t1 values(1,1),(2,1),(3,1),(4,1),(111,111),(3333,333);

eval ALTER TABLE t1 partition by $hash_type KEY(a) SUBPARTITION by range(b) (
    partition p0(
        SUBPARTITION p0sp0 VALUES LESS THAN(6),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    ),
    partition p1(
        SUBPARTITION p1sp0 VALUES LESS THAN(6),
        SUBPARTITION p1sp1 VALUES LESS THAN(1000)
    ),
    partition p2(
        SUBPARTITION p2sp0 VALUES LESS THAN(6),
        SUBPARTITION p2sp1 VALUES LESS THAN(1000)
    )
);
ALTER TABLE t1 ANALYZE PARTITION p0, p1, p2;
select partition_name, SUBPARTITION_name, TABLE_rows from information_schema.partitions where table_name = 't1';

eval ALTER TABLE t1 partition by $hash_type KEY(a) SUBPARTITION by range(b) (
    partition p0(
        SUBPARTITION p0sp0 VALUES LESS THAN(6),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    ),
    partition p1(
        SUBPARTITION p1sp0 VALUES LESS THAN(6),
        SUBPARTITION p1sp1 VALUES LESS THAN(1000)
    )
);
ALTER TABLE t1 ANALYZE PARTITION p0, p1;
select partition_name, SUBPARTITION_name, TABLE_rows from information_schema.partitions where table_name = 't1';

# DROP partition

--error ER_ONLY_ON_RANGE_LIST_PARTITION
ALTER TABLE t1 DROP partition p0sp0;

--error ER_ONLY_ON_RANGE_LIST_PARTITION
ALTER TABLE t1 DROP partition p2;
SHOW CREATE TABLE t1;

# truncate partition
SELECT * FROM t1 partition(p1sp1);
ALTER TABLE t1 truncate partition p1sp1;
SHOW CREATE TABLE t1;
SELECT * FROM t1 partition(p1sp1);

SELECT * FROM t1 partition(p1);
ALTER TABLE t1 truncate partition p1;
SHOW CREATE TABLE t1;
SELECT * FROM t1 partition(p1);

DROP TABLE t1;

# reorganize partition
CREATE TABLE t1 (
`a` int ,
`b` int ,
Primary Key(a, b));
INSERT INTO t1 values(1,1),(2,1),(3,1),(4,1),(111,111),(3333,333);

eval ALTER TABLE t1 partition by $hash_type KEY(a) SUBPARTITION by range(b) (
    partition p0(
        SUBPARTITION p0sp0 VALUES LESS THAN(6),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    ),
    partition p1(
        SUBPARTITION p1sp0 VALUES LESS THAN(6),
        SUBPARTITION p1sp1 VALUES LESS THAN(1000)
    ),
    partition p2(
        SUBPARTITION p2sp0 VALUES LESS THAN(6),
        SUBPARTITION p2sp1 VALUES LESS THAN(1000)
    )
);

# reorganize SUBPARTITION
ALTER TABLE t1 reorganize partition p2 INTO
(
    partition p2(
        SUBPARTITION p2sp0 VALUES LESS THAN(100),
        SUBPARTITION p2sp1 VALUES LESS THAN(1000)
    )
);

SHOW CREATE TABLE t1;
# merge
--error ER_REORG_HASH_ONLY_ON_SAME_NO 
ALTER TABLE t1 reorganize partition p0,p1 into
(
    partition p0(
        SUBPARTITION p0sp0 VALUES LESS THAN(100),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    )
);

# splite
--error ER_REORG_HASH_ONLY_ON_SAME_NO 
ALTER TABLE t1 reorganize partition p0 into
(
    partition p0(
        SUBPARTITION p0sp0 VALUES LESS THAN(100),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    ),
    partition p1(
        SUBPARTITION p1sp0 VALUES LESS THAN(100),
        SUBPARTITION p1sp1 VALUES LESS THAN(1000)
    )
);

SHOW CREATE TABLE t1;
DROP TABLE t1;

# exchange partition 

CREATE TABLE tsp (
`a` int ,
`b` int ,
Primary Key(a, b));
INSERT INTO tsp values(1,1),(2,1),(3,1),(4,1),(111,111),(3333,333);
eval ALTER TABLE tsp partition by $hash_type KEY(a) SUBPARTITION by range(b) (
    partition p0 (
        SUBPARTITION p0sp0 VALUES LESS THAN(6),
        SUBPARTITION p0sp1 VALUES LESS THAN(1000)
    ),
    partition p1 (
        SUBPARTITION p1sp0 VALUES LESS THAN(6),
        SUBPARTITION p1sp1 VALUES LESS THAN(1000)
    ),
    partition p2 (
        SUBPARTITION p2sp0 VALUES LESS THAN(6),
        SUBPARTITION p2sp1 VALUES LESS THAN(1000)
    )
);

create TABLE t like tsp;
ALTER TABLE t remove partitioning;

SELECT * FROM tsp partition(p2sp1);
ALTER TABLE tsp EXCHANGE PARTITION p2sp1 WITH TABLE t;
SHOW CREATE TABLE t;
SHOW CREATE TABLE tsp;

SELECT * FROM tsp partition(p2sp1);
SELECT * FROM t;

DELETE FROM t;
INSERT INTO t values(1023,2000);
--error ER_NO_PARTITION_FOR_GIVEN_VALUE 
ALTER TABLE tsp EXCHANGE PARTITION p2sp1 WITH TABLE t;

ALTER TABLE tsp EXCHANGE PARTITION p2sp1 WITH TABLE t without VALIDATION;
SELECT * FROM tsp PARTITION(p2sp1);
SELECT * FROM t;

DROP TABLE t,tsp;

# analyze partition

eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
INSERT INTO range_range(date) VALUES('2020-04-12'),('2019-06-12'),('2019-08-12'),('2019-10-12'),('2021-06-12'),('2021-08-12'),('2021-10-12');
SELECT * FROM range_range;
SHOW CREATE TABLE range_range;
ALTER TABLE range_range ANALYZE PARTITION p_2020,q3_2021;
SELECT * FROM range_range partition(p_2021);
SHOW CREATE TABLE range_range;
DROP TABLE range_range;

# check partition 

eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
INSERT INTO range_range(date) VALUES('2020-04-12'),('2019-06-12'),('2019-08-12'),('2019-10-12'),('2021-06-12'),('2021-08-12'),('2021-10-12');
SELECT * FROM range_range;
SHOW CREATE TABLE range_range;
ALTER TABLE range_range CHECK PARTITION p_2020,q3_2021;
SELECT * FROM range_range partition(p_2021);
SHOW CREATE TABLE range_range;
DROP TABLE range_range;

# optimize partition

eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
INSERT INTO range_range(date) VALUES('2020-04-12'),('2019-06-12'),('2019-08-12'),('2019-10-12'),('2021-06-12'),('2021-08-12'),('2021-10-12');
SELECT * FROM range_range;
SHOW CREATE TABLE range_range;
ALTER TABLE range_range OPTIMIZE PARTITION p_2020,q3_2021;
SELECT * FROM range_range partition(p_2021);
SHOW CREATE TABLE range_range;
DROP TABLE range_range;

# rebuild partition
eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
INSERT INTO range_range(date) VALUES('2020-04-12'),('2019-06-12'),('2019-08-12'),('2019-10-12'),('2021-06-12'),('2021-08-12'),('2021-10-12');
SELECT * FROM range_range;
SHOW CREATE TABLE range_range;
--error ER_DROP_PARTITION_NON_EXISTENT
ALTER TABLE range_range REBUILD PARTITION p_2020,q3_2021;
ALTER TABLE range_range REBUILD PARTITION p_2020;
SELECT * FROM range_range partition(p_2021);
SHOW CREATE TABLE range_range;
DROP TABLE range_range;

# repair partition
eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
INSERT INTO range_range(date) VALUES('2020-04-12'),('2019-06-12'),('2019-08-12'),('2019-10-12'),('2021-06-12'),('2021-08-12'),('2021-10-12');
SELECT * FROM range_range;
SHOW CREATE TABLE range_range;
ALTER TABLE range_range REPAIR PARTITION p_2020,q3_2021;
SELECT * FROM range_range partition(p_2021);
SHOW CREATE TABLE range_range;
DROP TABLE range_range;

# remove partitioning
eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
INSERT INTO range_range(date) VALUES('2020-04-12'),('2019-06-12'),('2019-08-12'),('2019-10-12'),('2021-06-12'),('2021-08-12'),('2021-10-12');
SHOW CREATE TABLE range_range;
ALTER TABLE range_range remove partitioning;
SHOW CREATE TABLE range_range;
SELECT * FROM range_range;
DROP TABLE range_range;

# coalesce partition
eval CREATE TABLE range_range
(
  dept_no     INT,
  part_no     INT,
  country     varchar(20),
  date        DATE,
  amount      INT
)
PARTITION BY $hash_type KEY(date)
SUBPARTITION BY RANGE(month(date))
(
  PARTITION p_2020 (
    SUBPARTITION q1_2020 VALUES LESS THAN(4),
    SUBPARTITION q2_2020 VALUES LESS THAN(7),
    SUBPARTITION q3_2020 VALUES LESS THAN(10),
    SUBPARTITION q4_2020 VALUES LESS THAN(13)
  ),
  PARTITION p_2021 (
    SUBPARTITION q1_2021 VALUES LESS THAN(4),
    SUBPARTITION q2_2021 VALUES LESS THAN(7),
    SUBPARTITION q3_2021 VALUES LESS THAN(10),
    SUBPARTITION q4_2021 VALUES LESS THAN(13)
  )
);
ALTER TABLE range_range COALESCE PARTITION 1;
DROP TABLE range_range;
#Verify alter table error
eval CREATE TABLE t1(a int, b int) PARTITION BY $hash_type KEY(a)(
partition p1,
partition p2
);
--error ER_PARTITION_MGMT_ON_NONPARTITIONED
ALTER TABLE t1 ADD PARTITION (PARTITION p3 (SUBPARTITION p3p0));
--error ER_PARTITION_MGMT_ON_NONPARTITIONED
ALTER TABLE t1 ADD PARTITION (PARTITION p3 (SUBPARTITION p3p0 VALUES LESS THAN(4)));

CREATE TABLE t2(a int, b int);
--error ER_PARTITION_MGMT_ON_NONPARTITIONED
ALTER TABLE t2 ADD PARTITION (PARTITION p0);
DROP TABLE t1, t2;

--error ER_PARTITIONS_MUST_BE_DEFINED_ERROR
eval CREATE TABLE t1(a int, b int) PARTITION BY $hash_type KEY(a) SUBPARTITION BY range(b) (
partition p1,
partition p2
);
eval CREATE TABLE t1(a int, b int) PARTITION BY $hash_type KEY(a)(
partition p1,
partition p2
);
--error ER_PARTITION_MGMT_ON_NONPARTITIONED
ALTER TABLE t1 ADD PARTITION (PARTITION p3 (SUBPARTITION p3p0));
--error ER_PARTITION_WRONG_VALUES_ERROR 
ALTER TABLE t1 ADD PARTITION (PARTITION p3 VALUES LESS THAN (1));
DROP TABLE t1;
eval CREATE TABLE t3 (
    id INT NOT NULL,
    store_id INT NOT NULL
)
PARTITION BY $hash_type KEY (store_id) 
SUBPARTITION BY RANGE(id) (
    PARTITION p0 (
    subpartition q1 values less than (4),
    subpartition q2 values less than (10)
  ),  
    PARTITION p1 (
    subpartition q3 values less than (15),
    subpartition q4 values less than (20)
  )
);

--error ER_RANGE_NOT_INCREASING_ERROR
ALTER TABLE t3 ADD PARTITION (PARTITION p3 (subpartition q5 values less than(19), subpartition q6 values less than(19)));
DROP TABLE t3;

# Verify index
eval CREATE TABLE t1(a int, b int) PARTITION BY $hash_type KEY(a) SUBPARTITION BY range(b) (
partition p1 (subpartition p1sp0 values less than(3),
                                   subpartition p1sp1 values less than (5)),
partition p2 (subpartition p2sp0 values less than(4),
                                    subpartition p2sp1 values less than (9))
);
INSERT INTO t1 VALUES(1,2),(2,4),(5,3),(6,8);
--replace_column 10 # 11 #
EXPLAIN SELECT * FROM t1 WHERE a < 1;
ALTER TABLE t1 ADD index idx(a);
SELECT * FROM t1 WHERE a < 1;
--replace_column 10 # 11 #
EXPLAIN SELECT * FROM t1 WHERE a = 2;
SELECT * FROM t1 WHERE a = 2;
ALTER TABLE t1 ADD index idx1(a, b);
--replace_column 10 # 11 #
EXPLAIN SELECT * FROM t1 WHERE a = 2 and b = 4;
SELECT * FROM t1 WHERE a = 2 and b = 4;
--replace_column 10 # 11 #
EXPLAIN SELECT * FROM t1 WHERE b = 4;
SELECT * FROM t1 WHERE b = 4;
ALTER TABLE t1 DROP INDEX idx;
ALTER TABLE t1 DROP INDEX idx1;
ALTER TABLE t1 ADD index idx2(b);
SELECT * FROM t1 WHERE a = 2;
SELECT * FROM t1 WHERE b = 4;
DROP TABLE t1;
SET global rds_extended_partitions_enabled = @saved_rds_extended_partitions_enabled;
