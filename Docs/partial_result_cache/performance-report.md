# Partial Result Cache Performance Report

## Scope

This report records local engineering performance validation for Partial
Result Cache (PRC/PTRC). It is not an official TPC-H benchmark result.

The performance gate is intentionally narrow: TPC-H Q17 with
`partial_result_cache` turned off and on. SF 1 is kept as the baseline
reproducible run, and SF 10 is added as extended validation on a larger data
set. No other `optimizer_switch` flags are changed, no PTRC system variables
are changed, and no PTRC hint is added to the query.

The goal is reproducibility and a clear attribution boundary. The measured
difference comes from the optimizer choosing PTRC after the
`partial_result_cache` switch is enabled.

## Environment

Repository:

```text
/Users/zhuqingping/Work/Database/MySQL/mysql-server-ptrc
```

Build:

```text
CMAKE_BUILD_TYPE=release
WITH_DEBUG=OFF
WITH_ASAN=0
WITH_UBSAN=OFF
WITH_LSAN=OFF
```

Server binary:

```text
build-ninja/runtime_output_directory/mysqld
8.0.46 for macos26.4 on arm64
```

Standalone server:

| Setting | Value |
|---|---|
| Datadir | `/tmp/mysql-ptrc-tpch-standard-sf1-20260623-114307/mysql-data` |
| Socket | `/tmp/mysql-ptrc-tpch-standard-sf1-20260623-114307/mysql.sock` |
| Port | `33316` |
| Buffer pool | `768M` |
| `local_infile` | `1` |
| `secure_file_priv` | empty string |

Artifacts:

```text
/tmp/mysql-ptrc-tpch-standard-sf1-20260623-114307
/tmp/mysql-ptrc-tpch-standard-sf1-latest
```

## Data Generation

TPC-H SF 1 data was generated with:

```bash
cd /tmp/mysql-ptrc-tpch-standard-sf1-20260623-114307/datafiles
ln -sf /Users/zhuqingping/Work/Database/TPC-H/2.18.0_rc2/dbgen/dists.dss .
/Users/zhuqingping/Work/Database/TPC-H/2.18.0_rc2/dbgen/dbgen -f -s 1
```

Generated row counts:

| Table | Rows |
|---|---:|
| `REGION` | `5` |
| `NATION` | `25` |
| `PART` | `200000` |
| `SUPPLIER` | `10000` |
| `PARTSUPP` | `800000` |
| `CUSTOMER` | `150000` |
| `ORDERS` | `1500000` |
| `LINEITEM` | `6001215` |

Load command:

```sql
LOAD DATA LOCAL INFILE '<table>.tbl'
INTO TABLE <table>
FIELDS TERMINATED BY '|'
LINES TERMINATED BY '|\n';
```

Data load time was `47.19s`.

## Schema

The test used the standard TPC-H logical table relationships with primary keys
and foreign keys. No extra covering indexes were added for Q17.

Relevant Q17 tables and relationships:

```sql
CREATE TABLE PART (
  P_PARTKEY INTEGER NOT NULL,
  P_NAME VARCHAR(55) NOT NULL,
  P_MFGR CHAR(25) NOT NULL,
  P_BRAND CHAR(10) NOT NULL,
  P_TYPE VARCHAR(25) NOT NULL,
  P_SIZE INTEGER NOT NULL,
  P_CONTAINER CHAR(10) NOT NULL,
  P_RETAILPRICE DECIMAL(15,2) NOT NULL,
  P_COMMENT VARCHAR(23) NOT NULL,
  PRIMARY KEY (P_PARTKEY)
) ENGINE=InnoDB;

CREATE TABLE PARTSUPP (
  PS_PARTKEY INTEGER NOT NULL,
  PS_SUPPKEY INTEGER NOT NULL,
  PS_AVAILQTY INTEGER NOT NULL,
  PS_SUPPLYCOST DECIMAL(15,2) NOT NULL,
  PS_COMMENT VARCHAR(199) NOT NULL,
  PRIMARY KEY (PS_PARTKEY, PS_SUPPKEY),
  FOREIGN KEY (PS_PARTKEY) REFERENCES PART(P_PARTKEY),
  FOREIGN KEY (PS_SUPPKEY) REFERENCES SUPPLIER(S_SUPPKEY)
) ENGINE=InnoDB;

CREATE TABLE ORDERS (
  O_ORDERKEY INTEGER NOT NULL,
  O_CUSTKEY INTEGER NOT NULL,
  O_ORDERSTATUS CHAR(1) NOT NULL,
  O_TOTALPRICE DECIMAL(15,2) NOT NULL,
  O_ORDERDATE DATE NOT NULL,
  O_ORDERPRIORITY CHAR(15) NOT NULL,
  O_CLERK CHAR(15) NOT NULL,
  O_SHIPPRIORITY INTEGER NOT NULL,
  O_COMMENT VARCHAR(79) NOT NULL,
  PRIMARY KEY (O_ORDERKEY),
  FOREIGN KEY (O_CUSTKEY) REFERENCES CUSTOMER(C_CUSTKEY)
) ENGINE=InnoDB;

CREATE TABLE LINEITEM (
  L_ORDERKEY INTEGER NOT NULL,
  L_PARTKEY INTEGER NOT NULL,
  L_SUPPKEY INTEGER NOT NULL,
  L_LINENUMBER INTEGER NOT NULL,
  L_QUANTITY DECIMAL(15,2) NOT NULL,
  L_EXTENDEDPRICE DECIMAL(15,2) NOT NULL,
  L_DISCOUNT DECIMAL(15,2) NOT NULL,
  L_TAX DECIMAL(15,2) NOT NULL,
  L_RETURNFLAG CHAR(1) NOT NULL,
  L_LINESTATUS CHAR(1) NOT NULL,
  L_SHIPDATE DATE NOT NULL,
  L_COMMITDATE DATE NOT NULL,
  L_RECEIPTDATE DATE NOT NULL,
  L_SHIPINSTRUCT CHAR(25) NOT NULL,
  L_SHIPMODE CHAR(10) NOT NULL,
  L_COMMENT VARCHAR(44) NOT NULL,
  PRIMARY KEY (L_ORDERKEY, L_LINENUMBER),
  FOREIGN KEY (L_ORDERKEY) REFERENCES ORDERS(O_ORDERKEY),
  FOREIGN KEY (L_PARTKEY, L_SUPPKEY)
      REFERENCES PARTSUPP(PS_PARTKEY, PS_SUPPKEY)
) ENGINE=InnoDB;
```

MySQL created the supporting foreign-key indexes used by the plan. Q17 did not
use any manually added covering index.

## Query

The same SQL text was used for both measurements. Only
`optimizer_switch=partial_result_cache` changed.

```sql
SELECT SUM(L_EXTENDEDPRICE) / 7.0 AS avg_yearly
FROM LINEITEM, PART
WHERE P_PARTKEY = L_PARTKEY
  AND P_BRAND = 'Brand#15'
  AND P_CONTAINER = 'WRAP PACK'
  AND L_QUANTITY < (
    SELECT 0.2 * AVG(L_QUANTITY)
    FROM LINEITEM
    WHERE L_PARTKEY = P_PARTKEY
  );
```

Feature-off setup:

```sql
SET optimizer_switch='partial_result_cache=off';
```

Feature-on setup:

```sql
SET optimizer_switch='partial_result_cache=on';
```

## Method

- TPC-H Q17 on SF 1 data.
- Same SQL executed 200 times in one client connection.
- One warmup run and five measured runs for each mode.
- Timing results report the wall-clock elapsed time for each 200-execution
  batch. `Per-query mean` is `Batch mean / 200`.
- No other optimizer switch was changed.
- No PTRC system variable was changed.
- No PTRC hint was used.
- Off/on result files were compared before using timings.
- Plans were captured with `EXPLAIN FORMAT=TREE`.
- Runtime counters were captured with `EXPLAIN ANALYZE FORMAT=TREE` for a
  single execution. These counters are diagnostic evidence and are not the
  source of the 200-execution timing table.

## Plan Difference

With `partial_result_cache=off`, `EXPLAIN FORMAT=TREE` contained no Result
cache node:

```text
-> Aggregate: sum(lineitem.L_EXTENDEDPRICE)
    -> Nested loop inner join
        -> Filter: ((part.P_CONTAINER = 'WRAP PACK') and
                    (part.P_BRAND = 'Brand#15'))
            -> Table scan on PART
        -> Filter: (lineitem.L_QUANTITY < (select #2))
            -> Index lookup on LINEITEM using L_PARTKEY
               (L_PARTKEY=part.P_PARTKEY)
            -> Select #2 (subquery in condition; dependent)
                -> Aggregate: avg(lineitem.L_QUANTITY)
                    -> Index lookup on LINEITEM using L_PARTKEY
                       (L_PARTKEY=part.P_PARTKEY)
```

With `partial_result_cache=on`, the optimizer selected PTRC for the dependent
Q17 subquery:

```text
-> Aggregate: sum(lineitem.L_EXTENDEDPRICE)
    -> Nested loop inner join
        -> Filter: ((part.P_CONTAINER = 'WRAP PACK') and
                    (part.P_BRAND = 'Brand#15'))
            -> Table scan on PART
        -> Filter: (lineitem.L_QUANTITY < (select #2))
            -> Index lookup on LINEITEM using L_PARTKEY
               (L_PARTKEY=part.P_PARTKEY)
            -> Select #2 (subquery in condition; dependent)
                -> Result cache : cache keys(PART.P_PARTKEY)
                    -> Aggregate: avg(lineitem.L_QUANTITY)
                        -> Index lookup on LINEITEM using L_PARTKEY
                           (L_PARTKEY=part.P_PARTKEY)
```

## EXPLAIN ANALYZE

Full outputs are preserved in the artifact directory:

```text
/tmp/mysql-ptrc-tpch-standard-sf1-20260623-114307/results/q17_off_explain_analyze.txt
/tmp/mysql-ptrc-tpch-standard-sf1-20260623-114307/results/q17_on_explain_analyze.txt
```

With `partial_result_cache=off`:

```text
-> Aggregate: sum(lineitem.L_EXTENDEDPRICE)  (cost=79177 rows=1) (actual time=82..82 rows=1 loops=1)
    -> Nested loop inner join  (cost=73288 rows=58883) (actual time=0.476..81.9 rows=439 loops=1)
        -> Filter: ((part.P_CONTAINER = 'WRAP PACK') and (part.P_BRAND = 'Brand#15'))  (cost=20315 rows=1981) (actual time=0.305..26.4 rows=168 loops=1)
            -> Table scan on PART  (cost=20315 rows=198100) (actual time=0.0504..19.9 rows=200000 loops=1)
        -> Filter: (lineitem.L_QUANTITY < (select #2))  (cost=23.8 rows=29.7) (actual time=0.121..0.33 rows=2.61 loops=168)
            -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=23.8 rows=29.7) (actual time=0.0206..0.0223 rows=30.4 loops=168)
            -> Select #2 (subquery in condition; dependent)
                -> Aggregate: avg(lineitem.L_QUANTITY)  (cost=29.7 rows=1) (actual time=0.00996..0.00998 rows=1 loops=5104)
                    -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=26.7 rows=29.7) (actual time=0.00745..0.00905 rows=31.3 loops=5104)
```

With `partial_result_cache=on`:

```text
-> Aggregate: sum(lineitem.L_EXTENDEDPRICE)  (cost=79177 rows=1) (actual time=31.2..31.2 rows=1 loops=1)
    -> Nested loop inner join  (cost=73288 rows=58883) (actual time=0.222..31.2 rows=439 loops=1)
        -> Filter: ((part.P_CONTAINER = 'WRAP PACK') and (part.P_BRAND = 'Brand#15'))  (cost=20315 rows=1981) (actual time=0.184..24.5 rows=168 loops=1)
            -> Table scan on PART  (cost=20315 rows=198100) (actual time=0.0228..18.4 rows=200000 loops=1)
        -> Filter: (lineitem.L_QUANTITY < (select #2))  (cost=23.8 rows=29.7) (actual time=0.0327..0.0396 rows=2.61 loops=168)
            -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=23.8 rows=29.7) (actual time=0.019..0.0205 rows=30.4 loops=168)
            -> Select #2 (subquery in condition; dependent)
                -> Result cache : cache keys(PART.P_PARTKEY) (Cache Hits: 4936, Cache Misses: 168, Cache Evictions: 0, Cache Overflows: 0, Memory Usage: 40960 )  (actual time=513e-6..528e-6 rows=1 loops=5104)
                    -> Aggregate: avg(lineitem.L_QUANTITY)  (cost=29.7 rows=1) (actual time=447e-6..447e-6 rows=0.0329 loops=5104)
                        -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=26.7 rows=29.7) (actual time=320e-6..390e-6 rows=1 loops=5104)
```

## Timing Results

Raw times are total elapsed seconds for 200 executions of Q17 in one client
connection:

| Run | PTRC off | PTRC on |
|---|---:|---:|
| warmup | `14.27s` | `5.40s` |
| 1 | `14.15s` | `5.29s` |
| 2 | `13.95s` | `5.43s` |
| 3 | `13.95s` | `5.55s` |
| 4 | `14.31s` | `5.47s` |
| 5 | `14.47s` | `5.42s` |

Measured mean, excluding warmup. `Per-query mean` is the average elapsed time
per Q17 execution derived from the 200-execution batch:

| Mode | Batch mean | Per-query mean |
|---|---:|---:|
| PTRC off | `14.166s` | `70.830ms` |
| PTRC on | `5.432s` | `27.160ms` |

Speedup:

```text
14.166 / 5.432 = 2.61x
```

Correctness:

```text
SHA256(Q17 off run 1) = 7b2893a9615bb0821e8e71db6f6200c11a9bba9170f8d57490a3d598b95707f2
SHA256(Q17 on  run 1) = 7b2893a9615bb0821e8e71db6f6200c11a9bba9170f8d57490a3d598b95707f2
```

## SF 10 Extended Validation

TPC-H SF 10 Q17 was run with the same SQL and the same feature boundary:
only `optimizer_switch=partial_result_cache` changed off and on. No PTRC hint,
no PTRC system variable, and no other optimizer switch was changed.

Artifacts:

```text
/tmp/mysql-ptrc-tpch-standard-sf10-20260623-125024
/tmp/mysql-ptrc-tpch-standard-sf10-latest
```

Environment differences from the SF 1 run:

| Setting | Value |
|---|---|
| Datadir | `/tmp/mysql-ptrc-tpch-standard-sf10-latest/mysql-data` |
| Socket | `/tmp/mysql-ptrc-tpch-standard-sf10-latest/mysql.sock` |
| Port | `33317` |
| Buffer pool | `18G` (`19327352832` bytes) |
| Buffer pool instances | `8` |
| Datadir size after validation | `25G` |

The larger buffer pool was used to reduce IO impact. Because the host has
24 GiB physical memory and the SF 10 InnoDB datadir is about 25 GiB, this run
does not claim that the full data set was guaranteed to stay resident in
memory.

SF 10 data was generated with:

```bash
cd /tmp/mysql-ptrc-tpch-standard-sf10-20260623-125024/datafiles
ln -sf /Users/zhuqingping/Work/Database/TPC-H/2.18.0_rc2/dbgen/dists.dss .
/Users/zhuqingping/Work/Database/TPC-H/2.18.0_rc2/dbgen/dbgen -f -s 10
```

Generation time was `114.87s`, data files used `10G`, and InnoDB load time
was `1237.38s`.

Generated row counts:

| Table | Rows |
|---|---:|
| `REGION` | `5` |
| `NATION` | `25` |
| `PART` | `2000000` |
| `SUPPLIER` | `100000` |
| `PARTSUPP` | `8000000` |
| `CUSTOMER` | `1500000` |
| `ORDERS` | `15000000` |
| `LINEITEM` | `59986052` |

Full `EXPLAIN ANALYZE FORMAT=TREE` outputs are preserved in the artifact
directory:

```text
/tmp/mysql-ptrc-tpch-standard-sf10-latest/results/q17_off_explain_analyze.txt
/tmp/mysql-ptrc-tpch-standard-sf10-latest/results/q17_on_explain_analyze.txt
```

With `partial_result_cache=off`:

```text
-> Aggregate: sum(lineitem.L_EXTENDEDPRICE)  (cost=458852 rows=1) (actual time=952..952 rows=1 loops=1)
    -> Nested loop inner join  (cost=401958 rows=568940) (actual time=0.449..952 rows=5154 loops=1)
        -> Filter: ((part.P_CONTAINER = 'WRAP PACK') and (part.P_BRAND = 'Brand#15'))  (cost=202798 rows=19779) (actual time=0.205..254 rows=1958 loops=1)
            -> Table scan on PART  (cost=202798 rows=1.98e+6) (actual time=0.0281..190 rows=2e+6 loops=1)
        -> Filter: (lineitem.L_QUANTITY < (select #2))  (cost=7.19 rows=28.8) (actual time=0.139..0.357 rows=2.63 loops=1958)
            -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=7.19 rows=28.8) (actual time=0.0316..0.0334 rows=29.9 loops=1958)
            -> Select #2 (subquery in condition; dependent)
                -> Aggregate: avg(lineitem.L_QUANTITY)  (cost=12.9 rows=1) (actual time=0.0106..0.0106 rows=1 loops=58588)
                    -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=10.1 rows=28.8) (actual time=0.008..0.00973 rows=30.9 loops=58588)
```

With `partial_result_cache=on`:

```text
-> Aggregate: sum(lineitem.L_EXTENDEDPRICE)  (cost=458852 rows=1) (actual time=333..333 rows=1 loops=1)
    -> Nested loop inner join  (cost=401958 rows=568940) (actual time=0.232..333 rows=5154 loops=1)
        -> Filter: ((part.P_CONTAINER = 'WRAP PACK') and (part.P_BRAND = 'Brand#15'))  (cost=202798 rows=19779) (actual time=0.186..247 rows=1958 loops=1)
            -> Table scan on PART  (cost=202798 rows=1.98e+6) (actual time=0.022..187 rows=2e+6 loops=1)
        -> Filter: (lineitem.L_QUANTITY < (select #2))  (cost=7.19 rows=28.8) (actual time=0.0366..0.0435 rows=2.63 loops=1958)
            -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=7.19 rows=28.8) (actual time=0.0218..0.0234 rows=29.9 loops=1958)
            -> Select #2 (subquery in condition; dependent)
                -> Result cache : cache keys(PART.P_PARTKEY) (Cache Hits: 56630, Cache Misses: 1958, Cache Evictions: 0, Cache Overflows: 0, Memory Usage: 340480 )  (actual time=557e-6..572e-6 rows=1 loops=58588)
                    -> Aggregate: avg(lineitem.L_QUANTITY)  (cost=12.9 rows=1) (actual time=494e-6..495e-6 rows=0.0334 loops=58588)
                        -> Index lookup on LINEITEM using L_PARTKEY (L_PARTKEY=part.P_PARTKEY)  (cost=10.1 rows=28.8) (actual time=357e-6..438e-6 rows=1 loops=58588)
```

SF 10 timing used 50 executions per batch. Raw times are total elapsed seconds
for each 50-query batch:

| Run | PTRC off | PTRC on |
|---|---:|---:|
| warmup | `42.09s` | `15.11s` |
| 1 | `42.15s` | `15.54s` |
| 2 | `42.55s` | `15.19s` |
| 3 | `42.53s` | `15.51s` |
| 4 | `42.24s` | `15.54s` |
| 5 | `42.65s` | `16.04s` |

Measured mean, excluding warmup:

| Mode | Batch mean | Per-query mean |
|---|---:|---:|
| PTRC off | `42.424s` | `848.480ms` |
| PTRC on | `15.564s` | `311.280ms` |

Speedup:

```text
42.424 / 15.564 = 2.73x
```

Correctness:

```text
SHA256(Q17 off run 1) = 6d109c6e7d9481395ef5273daad5092651a635ab67611d9644558858c6a7531b
SHA256(Q17 on  run 1) = 6d109c6e7d9481395ef5273daad5092651a635ab67611d9644558858c6a7531b
```

## Interpretation

Q17 benefits because the dependent average subquery is keyed by
`PART.P_PARTKEY`, and selected outer rows repeatedly execute the same inner
aggregate work. With PTRC enabled, repeated subquery keys are served from the
statement-local cache.

These results should be cited as local SF 1 and SF 10 Q17 engineering
measurements. They should not be presented as official TPC-H benchmark results
or as a claim about all TPC-H queries.
