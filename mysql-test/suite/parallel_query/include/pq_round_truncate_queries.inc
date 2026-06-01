--echo # --------------------------------
--echo # ROUND 
--echo # --------------------------------

--echo # Same execution plan (FLOOR and ROUND)

EXPLAIN FORMAT=TREE SELECT FLOOR(a) FROM t0; 
SELECT FLOOR(a) FROM t0; 

--echo # Same result (ROUND with and without PQ) 
EXPLAIN FORMAT=TREE SELECT ROUND(a) FROM t0;
SELECT ROUND(a) FROM t0;
EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ ROUND(a) FROM t0;
SELECT /*+ NO_PQ */ ROUND(a) FROM t0;


--echo # With aggregates

--echo # Same execution plan (FLOOR and ROUND)

EXPLAIN FORMAT=TREE SELECT FLOOR(SUM(a)) FROM t0; 
SELECT FLOOR(SUM(a)) FROM t0; 

--echo # Same result (ROUND with and without PQ) 

EXPLAIN FORMAT=TREE SELECT ROUND(SUM(a)) FROM t0;
SELECT ROUND(SUM(a)) FROM t0;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ ROUND(SUM(a)) FROM t0;
SELECT /*+ NO_PQ */ ROUND(SUM(a)) FROM t0;


--echo #3/ With aggregates and GROUP BY 

--echo # Same execution plan (FLOOR and ROUND)

EXPLAIN FORMAT=TREE  SELECT FLOOR(SUM(a)),type  FROM t0 GROUP BY type;
SELECT FLOOR(SUM(a)),type  FROM t0 GROUP BY type; 

EXPLAIN FORMAT=TREE SELECT ROUND(SUM(a)),type  FROM t0 GROUP BY type;
SELECT ROUND(SUM(a)),type  FROM t0 GROUP BY type;

--echo # Same result (ROUND with and without PQ) 

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ ROUND(SUM(a)),type  FROM t0 GROUP BY type;
SELECT /*+ NO_PQ */ ROUND(SUM(a)),type  FROM t0 GROUP BY type;


--echo # 4/ SUM_DISTINCT_FUNC is not supported by PQ (yet)
--echo # Same layout as previous tests - if evt. enabled only the execution plans should change 

EXPLAIN FORMAT=TREE SELECT FLOOR(SUM(DISTINCT a))  FROM t0 GROUP BY b;
SELECT FLOOR(SUM(DISTINCT a))  FROM t0 GROUP BY b;

EXPLAIN FORMAT=TREE  SELECT ROUND(SUM(DISTINCT a))  FROM t0 GROUP BY b;
SELECT ROUND(SUM(DISTINCT a))  FROM t0 GROUP BY b;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ ROUND(SUM(DISTINCT a))  FROM t0 GROUP BY b;
SELECT /*+ NO_PQ */  ROUND(SUM(DISTINCT a))  FROM t0 GROUP BY b;


--echo # 5/  Aggregate + SUM of another column and GROUP BY

EXPLAIN FORMAT=TREE SELECT  FLOOR(a + SUM(b)) FROM t0 GROUP BY a;
SELECT  FLOOR(a + SUM(b)) FROM t0 GROUP BY a;

EXPLAIN FORMAT=TREE SELECT  ROUND(a + SUM(b)) FROM t0 GROUP BY a;
SELECT  ROUND(a + SUM(b)) FROM t0 GROUP BY a;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */  ROUND(a + SUM(b)) FROM t0 GROUP BY a;
SELECT /*+ NO_PQ */ ROUND(a + SUM(b)) FROM t0 GROUP BY a;

--echo # Additional tests
--echo # 1 IF

EXPLAIN FORMAT=TREE SELECT ROUND(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b)) FROM t1;
SELECT ROUND(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b)) FROM t1;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ ROUND(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b)) FROM t1;
SELECT /*+ NO_PQ */ROUND(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b)) FROM t1;

--echo # 2 IFNULL

EXPLAIN FORMAT=TREE SELECT IFNULL(ROUND(SUM(ROUND(t1.a, 2) - ROUND(IFNULL(t1.b, 0), 2)) * 100 / ABS(SUM(ROUND(t1.a, 2))), 2), 0) avgGrossProfitRate FROM t1;
EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ IFNULL(ROUND(SUM(ROUND(t1.a, 2) - ROUND(IFNULL(t1.b, 0), 2)) * 100 / ABS(SUM(ROUND(t1.a, 2))), 2), 0) avgGrossProfitRate FROM t1;


SELECT IFNULL(ROUND(SUM(ROUND(t2.a, 2) - ROUND(IFNULL(t2.b, 0), 2)) * 100 / ABS(SUM(ROUND(t2.a, 2))), 2), 0) avgGrossProfitRate FROM t2;
SELECT /*+ NO_PQ */ IFNULL(ROUND(SUM(ROUND(t2.a, 2) - ROUND(IFNULL(t2.b, 0), 2)) * 100 / ABS(SUM(ROUND(t2.a, 2))), 2), 0) avgGrossProfitRate FROM t2;

--echo # Test switch 

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ ROUND(a) FROM t0; 
EXPLAIN FORMAT=TREE SELECT ROUND(a) FROM t0; 


--echo # ------------------------------
--echo # TRUNCATE
--echo # --------------------------------

--echo # Same execution plan (FLOOR and TRUNCATE)

EXPLAIN FORMAT=TREE SELECT FLOOR(a) FROM t0; 
SELECT FLOOR(a) FROM t0; 

--echo # Same result (TRUNCATE with and without PQ) 
EXPLAIN FORMAT=TREE SELECT TRUNCATE(a,2) FROM t0;
SELECT TRUNCATE(a,2) FROM t0;
EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ TRUNCATE(a,2) FROM t0;
SELECT /*+ NO_PQ */ TRUNCATE(a,2) FROM t0;

--echo # With aggregates

--echo # Same execution plan (FLOOR and TRUNCATE)

EXPLAIN FORMAT=TREE SELECT FLOOR(SUM(a)) FROM t0; 
SELECT FLOOR(SUM(a)) FROM t0; 

--echo # Same result (TRUNCATE with and without PQ) 

EXPLAIN FORMAT=TREE SELECT TRUNCATE(SUM(a),2) FROM t0;
SELECT TRUNCATE(SUM(a),2) FROM t0;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ TRUNCATE(SUM(a),2) FROM t0;
SELECT /*+ NO_PQ */ TRUNCATE(SUM(a),2) FROM t0;

--echo #3/ With aggregates and GROUP BY 

--echo # Same execution plan (FLOOR and TRUNCATE)

EXPLAIN FORMAT=TREE  SELECT FLOOR(SUM(a)),type  FROM t0 GROUP BY type;
SELECT FLOOR(SUM(a)),type  FROM t0 GROUP BY type; 

EXPLAIN FORMAT=TREE SELECT TRUNCATE(SUM(a),2),type  FROM t0 GROUP BY type;
SELECT TRUNCATE(SUM(a),2),type  FROM t0 GROUP BY type;

--echo # Same result (TRUNCATE with and without PQ) 

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ TRUNCATE(SUM(a),2),type  FROM t0 GROUP BY type;
SELECT /*+ NO_PQ */ TRUNCATE(SUM(a),2),type  FROM t0 GROUP BY type;


--echo # 4/ SUM_DISTINCT_FUNC is not supported by PQ (yet)
--echo # Same layout as previous tests - if evt. enabled only the execution plans should change 

EXPLAIN FORMAT=TREE SELECT FLOOR(SUM(DISTINCT a))  FROM t0 GROUP BY b;
SELECT FLOOR(SUM(DISTINCT a))  FROM t0 GROUP BY b;

EXPLAIN FORMAT=TREE  SELECT TRUNCATE(SUM(DISTINCT a),2)  FROM t0 GROUP BY b;
SELECT TRUNCATE(SUM(DISTINCT a),2)  FROM t0 GROUP BY b;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ TRUNCATE(SUM(DISTINCT a),2)  FROM t0 GROUP BY b;
SELECT /*+ NO_PQ */  TRUNCATE(SUM(DISTINCT a),2)  FROM t0 GROUP BY b;

--echo # 5/  Aggregate + SUM of another column and GROUP BY

EXPLAIN FORMAT=TREE SELECT  FLOOR(a + SUM(b)) FROM t0 GROUP BY a;
SELECT  FLOOR(a + SUM(b)) FROM t0 GROUP BY a;

EXPLAIN FORMAT=TREE SELECT  TRUNCATE(a + SUM(b),2) FROM t0 GROUP BY a;
SELECT  TRUNCATE(a + SUM(b),2) FROM t0 GROUP BY a;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */  TRUNCATE(a + SUM(b),2) FROM t0 GROUP BY a;
SELECT /*+ NO_PQ */ TRUNCATE(a + SUM(b),2) FROM t0 GROUP BY a;

--echo # Additional tests 
--echo # 1 IF

EXPLAIN FORMAT=TREE SELECT TRUNCATE(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b),2) FROM t1;
SELECT TRUNCATE(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b),2) FROM t1;

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ TRUNCATE(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b),2) FROM t1;
SELECT /*+ NO_PQ */ TRUNCATE(IF(t1.a= 1, t1.b - IF(t1.b> 0, t1.c, -t1.c),t1.b),2) FROM t1;

--echo # 2 IFNULL

EXPLAIN FORMAT=TREE SELECT IFNULL(TRUNCATE(SUM(TRUNCATE(t2.a, 2) - TRUNCATE(IFNULL(t2.b, 0), 2)) * 100 / ABS(SUM(TRUNCATE(t2.a, 2))), 2), 0) avgGrossProfitRate FROM t2;
EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ IFNULL(TRUNCATE(SUM(TRUNCATE(t2.a, 2) - TRUNCATE(IFNULL(t2.b, 0), 2)) * 100 / ABS(SUM(TRUNCATE(t2.a, 2))), 2), 0) avgGrossProfitRate FROM t2;

SELECT IFNULL(TRUNCATE(SUM(TRUNCATE(t2.a, 2) - TRUNCATE(IFNULL(t2.b, 0), 2)) * 100 / ABS(SUM(TRUNCATE(t2.a, 2))), 2), 0) avgGrossProfitRate FROM t2;
SELECT /*+ NO_PQ */ IFNULL(TRUNCATE(SUM(TRUNCATE(t2.a, 2) - TRUNCATE(IFNULL(t2.b, 0), 2)) * 100 / ABS(SUM(TRUNCATE(t2.a, 2))), 2), 0) avgGrossProfitRate FROM t2;

--echo # Test switch 

EXPLAIN FORMAT=TREE SELECT /*+ NO_PQ */ TRUNCATE(a,2) FROM t0; 
EXPLAIN FORMAT=TREE SELECT TRUNCATE(a,2) FROM t0; 
