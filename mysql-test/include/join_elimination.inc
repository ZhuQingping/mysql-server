--echo # This tests the feature: "LEFT JOIN elimination" implemented by Huawei
--echo # in code file sql/sql_join_elimination.cc .

--echo # Results of SELECTs have all been compared to results with the
--echo # feature off, and are identical.


CREATE TABLE t1(a INT PRIMARY KEY, b INT, c VARCHAR(10), dt DATETIME);
CREATE TABLE t2(a INT PRIMARY KEY, b INT, c VARCHAR(10), dt DATETIME);
SHOW CREATE TABLE t2;
INSERT INTO t1 VALUES(1,1,"4.0","2000-01-01 12:34:56"),
  (2,3,"abc","2002-01-01"),(7,null,"wxc","2004-02-02");
INSERT INTO t2 VALUES(2,2,"4.0","2000-01-01 12:34:56"),
  (4,5,"def","2002-01-01"),(5,4,"abc","2004-02-02");

--echo # Should be ON by default
SELECT @@optimizer_switch LIKE "%left_join_elimination=off%",
  @@optimizer_switch LIKE "%left_join_elimination=on%";

--echo # No elimination, as no GROUP BY or DISTINCT, and no equality
--echo # on unique key
  
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;

--echo # (I) Elimination due to LEFT JOIN with GROUP BY or DISTINCT

let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;

--echo # If switch is off, feature is disabled:
SET optimizer_switch="left_join_elimination=off";
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;
SET optimizer_switch="left_join_elimination=on";

--echo # No elimination in inner join
let $query=
SELECT DISTINCT t1.a FROM t1 JOIN t2 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1, t2;
eval EXPLAIN $query;
eval $query;

--echo # Elimination in right join
let $query=
SELECT DISTINCT t1.a FROM t2 RIGHT JOIN t1 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;

--echo # No elimination as t2.a is selected
let $query=
SELECT t1.a, t2.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a, t2.a;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a, t2.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;

--echo # In next queries, t2.a is referenced here and there, to check
--echo # that it prevents elimination.
  

--echo # t2.a in GROUP BY
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a, t2.a;
eval EXPLAIN $query;
eval $query;

--echo # t2.a in WHERE
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t2.a>3 GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;
--echo # vs this ok case
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t1.a>3 GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;

--echo # t2.a in subquery in WHERE
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE (SELECT t2.a FROM t1 AS t3 ORDER BY 1 LIMIT 1)>4;
eval EXPLAIN $query;
eval $query;

--echo # t2.a in ON condition of another join.
let $query=
SELECT DISTINCT t1.a, t3.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b LEFT JOIN t1 AS t3 ON t2.a>t3.b;
eval EXPLAIN $query;
eval $query;
--echo # vs
let $query=
SELECT DISTINCT t1.a, t3.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b LEFT JOIN t1 AS t3 ON t1.a>t3.b;
eval EXPLAIN $query;
eval $query;

--echo # t2.a in lateral table's body
let $query=
SELECT DISTINCT t1.a, derived.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b JOIN LATERAL (SELECT t3.a, t3.b FROM t1 AS t3 WHERE t2.a>t3.b) AS derived ON TRUE;
eval EXPLAIN $query;
eval $query;
--echo # vs
let $query=
SELECT DISTINCT t1.a, derived.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b JOIN LATERAL (SELECT t3.a, t3.b FROM t1 AS t3 WHERE t1.a>t3.b) AS derived ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # the same, with materialization
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b JOIN LATERAL (SELECT t3.a, t3.b FROM t1 AS t3 WHERE t2.a>t3.b LIMIT 2) AS derived ON TRUE;
eval EXPLAIN $query;
eval $query;
--echo # vs
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b JOIN LATERAL (SELECT t3.a, t3.b FROM t1 AS t3 WHERE t1.a>t3.b LIMIT 2) AS derived ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # the same, with a JSON_TABLE which is similar to a lateral
--echo # table but is not one in MySQL's implementation, so needs
--echo # special handling
  
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b JOIN JSON_TABLE(
    COALESCE('[{"a":"3"},{"a":2},{"b":1},{"a":0}]', CONCAT(t2.a)),
    '$[*]' COLUMNS (id FOR ORDINALITY,
             jpath VARCHAR(100) PATH '$.a',
             jexst INT EXISTS PATH '$.b')
  ) AS derived ON TRUE;
eval EXPLAIN $query;
eval $query;
--echo # vs
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b JOIN JSON_TABLE(
    COALESCE('[{"a":"3"},{"a":2},{"b":1},{"a":0}]', CONCAT(t1.a)),
    '$[*]' COLUMNS (id FOR ORDINALITY,
             jpath VARCHAR(100) PATH '$.a',
             jexst INT EXISTS PATH '$.b')
  ) AS derived ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # t2.a in subquery in ON of t2's join: eliminated
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON (SELECT t2.a FROM t1 AS t3 ORDER BY 1 LIMIT 1)>t1.b;
eval EXPLAIN $query;
eval $query;
--echo # vs
let $query=
SELECT DISTINCT t1.a, t3.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b LEFT JOIN t1 AS t3 ON (SELECT t2.a FROM t1 AS t4 ORDER BY 1 LIMIT 1)>t3.b;
eval EXPLAIN $query;
eval $query;

--echo # Two consecutive eliminations: elimination of t3, which then
--echo # allows elimination of t2 (do_join_elimination() processes join nests
--echo # from right to left)
  
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b LEFT JOIN t1 AS t3 ON t2.a>t3.b;
eval EXPLAIN $query;
eval $query;

--echo # Elimination of a two-table nest t2-t3. COALESCE is here to
--echo # prevent t2-t3 from being changed to an inner join
  
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2 LEFT JOIN t1 AS t3 ON t3.a=t2.b) ON COALESCE(t3.a,1)=1;
eval EXPLAIN $query;
eval $query;
--echo # Without COALESCE, t2-t3 is changed to an inner join, then is
--echo # eliminated.
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2 LEFT JOIN t1 AS t3 ON t3.a=t2.b) ON t3.a=1;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2,t1 AS t3) ON t3.a=1;
eval EXPLAIN $query;
eval $query;

--echo # The same, with a subquery in ON inside the removed nest. This
--echo # is to demonstrate that, when removing the nest t2-t3, we must also
--echo # clean up any subquery inside it, not just the ON clause of t1 LEFT
--echo # JOIN (t2-t3). Otherwise, it remains "dangling" and causes
--echo # subquery-partial-result-cache to crash.
  
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2 LEFT JOIN t1 AS t3 ON t3.a=(SELECT t4.a FROM t1 AS t4 WHERE t2.b<4 ORDER BY 1 LIMIT 1)) ON COALESCE(t3.a,1)=1;
eval EXPLAIN $query;
eval $query;

--echo # The same, with a subquery into a derived table inside the
--echo # removed nest.
  
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2 LEFT JOIN (SELECT t4.a FROM t1 AS t4 WHERE t4.b<4 LIMIT 2) AS t3 ON TRUE) ON COALESCE(t3.a,1)=1;
eval EXPLAIN $query;
eval $query;

--echo # The same, with less nesting

let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN (SELECT t4.a FROM t1 AS t4 WHERE t4.b<4 LIMIT 2) AS t3 ON COALESCE(t3.a,1)=1;
eval EXPLAIN $query;
eval $query;

--echo # The same, but this time the derived table is a common table
--echo # expression referenced once. The CTE is removed and not optimized.
--echo # Because there was only one reference to the CTE, the WITH clause is
--echo # not printed in the 'Note'.

let $query=
WITH cte AS (SELECT t4.a FROM t1 AS t4 WHERE t4.b<4 LIMIT 2) SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2 LEFT JOIN cte AS t3 ON TRUE) ON COALESCE(t3.a,1)=1;
eval EXPLAIN $query;
eval $query;
--echo # The same, but this time the common table expression is
--echo # referenced twice. One CTE reference (t3) is removed. The other
--echo # reference (t0) stays, and thus WITH is printed.
let $query=
WITH cte AS (SELECT t4.a FROM t1 AS t4 WHERE t4.b<4 LIMIT 2) SELECT DISTINCT t1.a FROM cte AS t0, t1 LEFT JOIN (t2 LEFT JOIN cte AS t3 ON TRUE) ON COALESCE(t3.a,1)=1;
eval EXPLAIN $query;
eval $query;
--echo # The same, but this time both references are eliminated.
--echo # FORMAT=TREE shows iterators, and here we only see a table scan on t1,
--echo # confirming the CTE is gone (never materialized).
let $query=
WITH cte AS (SELECT t4.a FROM t1 AS t4 WHERE t4.b<4 LIMIT 2) SELECT DISTINCT t1.a FROM t1 LEFT JOIN (t2 LEFT JOIN cte AS t3 ON TRUE) ON COALESCE(t3.a,1)=1 LEFT JOIN cte ON TRUE;
eval EXPLAIN $query;
eval EXPLAIN format=tree $query;
eval $query;

--echo # Make sure elimination does not apply to an anti-join, which
--echo # in MySQL's implementation is very similar to a LEFT JOIN and should
--echo # not be confused with it. Elimination shouldn't apply to the
--echo # semi-join either. For the anti-join transformation of NOT IN, the
--echo # left and right arguments must not be nullable so we use COALESCE.
--echo # For reference, we start with a LEFT JOIN which does elimination.
  
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON COALESCE(t1.a,0)=COALESCE(t2.b,0);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 WHERE COALESCE(t1.a,0) NOT IN (SELECT COALESCE(t2.b,0) FROM t2);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 WHERE NOT EXISTS (SELECT * FROM t2 WHERE t2.b=t1.a);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 WHERE t1.a IN (SELECT t2.b FROM t2);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 WHERE EXISTS (SELECT * FROM t2 WHERE t2.b=t1.a);
eval EXPLAIN $query;
eval $query;

--echo # Aggregates and window functions prevent elimination except in
--echo # the second of these four queries (in that query, duplicate
--echo # elimination by GROUP BY happens before the window function is
--echo # calculated, so the increase in number of rows due to the LEFT JOIN
--echo # is without influence on the window function's value)
  
let $query=
SELECT t1.a,COUNT(*) FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t1.a,COUNT(*) OVER() FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT COUNT(*) FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT COUNT(*) OVER () FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
eval EXPLAIN $query;
eval $query;
--echo # No elimination, like the first query except that the
--echo # incompatible aggregate is hidden in a compatible window function
let $query=
SELECT t1.a,COUNT(SUM(1)) OVER() FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;
--echo # Window function and DISTINCT together prevent elimination,
--echo # but not if GROUP BY is used too:
let $query=
SELECT DISTINCT COUNT(*) OVER () FROM t1 LEFT JOIN t2 ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;

--echo # Elimination can make semi-joins disappear (+0 is to make sure
--echo # that semi-join pullout does not happen - t4.a is a primary key):
let $query=
SELECT t1.a FROM t1 LEFT JOIN (t1 AS t3 JOIN t2 ON t2.a+t3.a IN (SELECT t4.a+0 FROM t2 AS t4)) ON t2.a>t1.b GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;

--echo # Here one semi-join will remain, out of two:
let $query=
SELECT t1.a FROM t1 LEFT JOIN (t1 AS t3 JOIN t2 ON t2.a+t3.a IN (SELECT t4.a+0 FROM t2 AS t4)) ON t2.a>t1.b WHERE t1.a IN (SELECT t5.a+0 FROM t2 AS t5) GROUP BY t1.a;
eval EXPLAIN $query;
eval $query;

--echo # Elimination when the right side is a lateral derived table or JSON_TABLE

let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN LATERAL (SELECT t3.a, t3.b FROM t1 AS t3 WHERE t1.a>t3.b LIMIT 2) AS derived ON derived.b+t1.a;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a FROM t1 LEFT JOIN JSON_TABLE(
    COALESCE('[{"a":"3"},{"a":2},{"b":1},{"a":0}]', CONCAT(t1.a)),
    '$[*]' COLUMNS (id FOR ORDINALITY,
             jpath VARCHAR(100) PATH '$.a',
             jexst INT EXISTS PATH '$.b')
  ) AS derived ON derived.jexst+t1.a;
eval EXPLAIN $query;
eval $query;

--echo # (II) Elimination due to LEFT JOIN contained in semi-join or anti-join

--echo # EXISTS. Note that 'JOIN t2.b' does not prevent elimination of
--echo # t2, as this 't2.b' is disposed off (during semi-join transformation)
--echo # before we try elimination.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE EXISTS (SELECT t2.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=t1.b);
eval EXPLAIN $query;
eval $query;
--echo # Verify that the choice of semi-join strategy is like if t2
--echo # had never been there
let $query=
SELECT t0.a FROM t1 AS t0 WHERE EXISTS (SELECT 1 FROM t1 WHERE t0.a=t1.b);
eval EXPLAIN $query;
eval $query;

--echo # t2 is referenced in WHERE outside of the left join (i.e. in
--echo # the semi-join condition) => no elimination.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE EXISTS (SELECT t2.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=COALESCE(t2.b,0));
eval EXPLAIN $query;
eval $query;

--echo # IN. We have elimination, then no elimination, as above.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a IN (SELECT t1.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;
--echo # COALESCE is used otherwise the left join becomes inner join,
--echo # which we do not want.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a IN (SELECT COALESCE(t2.b) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;

--echo # NOT EXISTS. We have elimination, then no elimination, as above.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE NOT EXISTS (SELECT t2.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=t1.b);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t0.a FROM t1 AS t0 WHERE NOT EXISTS (SELECT t2.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=COALESCE(t2.b,0));
eval EXPLAIN $query;
eval $query;

--echo # NOT IN. We have elimination, then no elimination, as above.
--echo # COALESCE is used on t1.b, to make that non-nullable, as is necessary
--echo # for NOT IN to be translatable into anti-join. t0.a is already
--echo # non-nullable.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a NOT IN (SELECT COALESCE(t1.b,0) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a NOT IN (SELECT COALESCE(t2.b,0) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;
--echo # One case where the LEFT JOIN is deeply nested in the
--echo # semi-join, in other words there are intermediate joins between the two:
--echo # t0 SEMI JOIN (t1 LEFT JOIN (t3 LEFT JOIN t2)), and t2 is eliminated.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a IN (SELECT COALESCE(t1.b+t3.b) FROM t1 LEFT JOIN (t1 AS t3 LEFT JOIN t2 ON t2.a<t3.b) ON COALESCE(t3.a)>t1.b);
eval EXPLAIN $query;
eval $query;

--echo # Now the same subqueries, but without transformation to
--echo # semi-join or anti-join: because they are EXISTS or IN, elimination
--echo # is sometimes possible too.
  
SET optimizer_switch="semijoin=off";

--echo # EXISTS. Note that 'SELECT t2.b' this time does prevent elimination of t2
let $query=
SELECT t0.a FROM t1 AS t0 WHERE EXISTS (SELECT t2.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=t1.b);
eval EXPLAIN $query;
eval $query;
--echo # But, surprisingly, if it's 'SELECT *' instead of 'SELECT
--echo # t2.b' then it is replaced with 'SELECT 1' in setup_wild() before we
--echo # try elimination, and so we can eliminate:
let $query=
SELECT t0.a FROM t1 AS t0 WHERE EXISTS (SELECT t2.* FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=t1.b);
eval EXPLAIN $query;
eval $query;

--echo # t2 is referenced in WHERE outside of the left join => no elimination.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE EXISTS (SELECT t2.* FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=COALESCE(t2.b,0));
eval EXPLAIN $query;
eval $query;

--echo # IN. We have elimination, then no elimination, as above.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a IN (SELECT t1.b FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a IN (SELECT COALESCE(t2.b) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;

--echo # NOT EXISTS. We have elimination, then no elimination, as above.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE NOT EXISTS (SELECT t2.* FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=t1.b);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t0.a FROM t1 AS t0 WHERE NOT EXISTS (SELECT t2.* FROM t1 LEFT JOIN t2 ON t2.a>t1.b WHERE t0.a=COALESCE(t2.b,0));
eval EXPLAIN $query;
eval $query;

--echo # NOT IN. We have elimination, then no elimination, as above.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a NOT IN (SELECT COALESCE(t1.b,0) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a NOT IN (SELECT COALESCE(t2.b,0) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;

--echo # One case where the LEFT JOIN is deeply nested in the subquery.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a IN (SELECT COALESCE(t1.b+t3.b) FROM t1 LEFT JOIN (t1 AS t3 LEFT JOIN t2 ON t2.a<t3.b) ON COALESCE(t3.a)>t1.b);
eval EXPLAIN $query;
eval $query;

--echo # IN, with an aggregate, should not have elimination.
let $query=
SELECT t0.a FROM t1 AS t0 WHERE t0.a in (SELECT COUNT(*) FROM t1 LEFT JOIN t2 ON t2.a>t1.b);
eval EXPLAIN $query;
eval $query;

SET optimizer_switch="semijoin=on";

--echo # (III.1) Elimination due to LEFT JOIN ON <condition which binds
--echo # all columns of a unique key of the right table>

let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a=t1.b;
eval EXPLAIN $query;
eval $query;

--echo # No elimination as equality is not deterministic
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a=RAND();
eval EXPLAIN $query;
eval $query;
--echo # This one is deterministic
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a=3;
eval EXPLAIN $query;
eval $query;

--echo # The order of arguments in the important equality, and the
--echo # presence of extra uninteresting AND-ed conditions, don't matter:
--echo # elimination is done
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t1.b=t2.a AND RAND()/10>t2.b;
eval EXPLAIN $query;
eval $query;

--echo # No elimination as t2.a is selected
let $query=
SELECT t1.a, t2.a FROM t1 LEFT JOIN t2 ON t2.a=t1.b;
eval EXPLAIN $query;
eval $query;

--echo # No elimination as using <=> (in case t2.a is nullable)
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a<=>t1.b;
eval EXPLAIN $query;
eval $query;

--echo # Elimination of a two-table nest on right side, as both have a
--echo # bound unique key. Also testing optimizer tracing of the feature.
SET optimizer_trace="enabled=on";
let $query=
SELECT t1.a FROM t1 LEFT JOIN (t2, t2 AS t3) ON t2.a=t1.b AND t3.a=t1.b+1;
eval EXPLAIN $query;
eval $query;
--echo # we see a block in the trace, saying 2 tables have been eliminated
SELECT TRACE FROM information_schema.optimizer_trace;
--echo # OR-ed conditions, no elimination
let $query=
SELECT t1.a FROM t1 LEFT JOIN (t2, t2 AS t3) ON t2.a=t1.b OR t3.a=t1.b+1;
eval EXPLAIN $query;
eval $query;
--echo # and we see nothing in the trace
SELECT TRACE FROM information_schema.optimizer_trace;
SET optimizer_trace=default;
--echo # One single bound unique key, no elimination
let $query=
SELECT t1.a FROM t1 LEFT JOIN (t2, t2 AS t3) ON t2.a=t1.b AND t3.a>t1.b+1;
eval explain $query;
eval $query;

--echo # Equality on string column is tricky, we test it
ALTER TABLE t2 ADD UNIQUE INDEX (c);

--echo # No elimination, comparing number on left to string on right.
--echo # In detail: imagine te left table has one row with integer
--echo # value 1, and the right table has two rows with string values
--echo # '1' and '1.0' (which are different strings, so UNIQUE is
--echo # respected) : with the original query there will be _two_ matches in
--echo # the join (comparison is done as numbers), and two rows in the result;
--echo # with elimination, only one row).
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.c=t1.a;
eval EXPLAIN $query;
eval $query;
--echo # In the reverse case, there is elimination
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a=t1.c;
eval EXPLAIN $query;
eval $query;
--echo # Also with string=string, but only if collations match
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.c=t1.c;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.c=t1.c COLLATE utf8mb4_lv_0900_ai_ci;
eval EXPLAIN $query;
eval $query;
--echo # Here the collation is explicit but it's the same as the columns'.
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.c=t1.c COLLATE utf8mb4_0900_ai_ci;
eval EXPLAIN $query;
eval $query;

FLUSH STATUS;
--echo # Here t1.c is replaced with a '?'. Even though the comments
--echo # below are a bit complicated as they try to explain what is going
--echo # internally, the visible result of queries is that elimination is done
--echo # and not done, properly.
--echo # 'c' is string so '?' is also given an inferred string type by
--echo # param_type_uses_non_param(). string_col=string is safe for
--echo # elimination, so elimination is done.
PREPARE s FROM 'EXPLAIN SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.c=?';
SET @var='abc';
EXECUTE s USING @var;
SET @var=1;
--echo # As is usual when a PS parameter changes from string type to
--echo # integer type (checked in check_parameter_types()), there is
--echo # re-preparation. In this new preparation we have t2.c=integer, so
--echo # elimination cannot be done (string_col=int is unsafe, look for
--echo # is_string_type() in sql_join_elimination.cc).
EXECUTE s USING @var;
SHOW STATUS LIKE "Com_stmt_reprepare";
--echo # Here the type of '?' is integer (inferred from 'a').
PREPARE s FROM 'EXPLAIN SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.a=?';
SET @var=1;
--echo # Elimination can be done ('a' is an integer, int_col=anything
--echo # is safe).
EXECUTE s USING @var;
SET @var='abc';
--echo # No re-preparation (parameter type changes int->string does
--echo # not cause a re-preparation). So elimination remains in force (and
--echo # fortunately, int_col=string is safe).
EXECUTE s USING @var;
DROP PREPARE s;
SHOW STATUS LIKE "Com_stmt_reprepare";

--echo # Elimination works with equality on ROW type too
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON (t2.c,t2.b+1)=(t1.c,t2.a+1);
eval EXPLAIN $query;
eval $query;

--echo # With temporal type
ALTER TABLE t2 add unique(dt);
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.dt=t1.dt;
eval EXPLAIN $query;
eval $query;

--echo # As in a previous test, this can do elimination as a string in
--echo # t1.c can only map to a single DATETIME in t2.dt
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.dt=t1.c;
eval EXPLAIN $query;
eval $query;
--echo # but this cannot do join elimination, as multiple strings in
--echo # t2.c can map to the same DATETIME in t1.dt
let $query=
SELECT t1.a FROM t1 LEFT JOIN t2 ON t2.c=t1.dt;
eval EXPLAIN $query;
eval $query;

--echo # (III.2) Pseudo-unique key of derived table containing GROUP BY

--echo # This can do elimination as all group expressions of the derived
--echo # table have imposed values in the top LEFT JOIN
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY t2.b+1, t2.b*t3.a) AS derived ON derived.x=t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;
--echo # But here one of the two expressions is missing, no elimination
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY t2.b+1, t2.b*t3.a) AS derived ON derived.x>t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;

--echo # The same, with group expressions referencing selected expressions,
--echo # using numbers or alias.
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY 2,1) AS derived ON derived.x=t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY y,x) AS derived ON derived.x=t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;

--echo # The same, with a view instead of a derived table
CREATE VIEW v1 AS
  SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY t2.b+1, t2.b*t3.a;
let $query=
SELECT t1.* FROM t1 LEFT JOIN v1 ON v1.x=t1.a AND v1.y=t1.a+4;
eval EXPLAIN $query;
eval $query;
DROP VIEW v1;
--echo # and as a CTE
let $query=
WITH cte AS (SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY t2.b+1, t2.b*t3.a) SELECT t1.* FROM t1 LEFT JOIN cte ON cte.x=t1.a AND cte.y=t1.a+4;
eval EXPLAIN $query;
eval $query;

--echo # Adding ROLLUP to GROUP BY
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT t2.b+1 AS x, t2.b*t3.a AS y, COUNT(*) AS col_count FROM t2, t2 AS t3 GROUP BY t2.b+1, t2.b*t3.a WITH ROLLUP) AS derived ON derived.x=t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;

--echo (III.3) Pseudo-unique key of derived table containing DISTINCT

--echo # This can do elimination as all selected expressions of the derived
--echo # table have imposed values in the top LEFT JOIN
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT DISTINCT t2.b+1 AS x, t2.b*t3.a AS y FROM t2, t2 AS t3) AS derived ON derived.x=t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;
--echo # But here one of the two expressions is missing, no elimination
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT DISTINCT t2.b+1 AS x, t2.b*t3.a AS y FROM t2, t2 AS t3) AS derived ON derived.x>t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;
--echo # No DISTINCT, no elimination (LIMIT is to keep materialization, avoid merging)
let $query=
SELECT t1.* FROM t1 LEFT JOIN (SELECT t2.b+1 AS x, t2.b*t3.a AS y FROM t2, t2 AS t3 LIMIT 100) AS derived ON derived.x=t1.a AND derived.y=t1.a+4;
eval EXPLAIN $query;
eval $query;

--echo # (IV) Query similar to customer's

let $query=
SELECT COUNT(1) FROM
(SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN t2 AS u ON t.a = u.b+1
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
 GROUP BY t.a, computed_col
) AS derived;
eval EXPLAIN $query;
eval $query;

--echo # Fix for a bug: it's not possible to get reliable used_tables information of an aggregate which is inside a subquery of our query block. We block this situation in code, otherwise we would not notice that t2 is used in count() and eliminate it, which would be wrong.
explain select distinct (select count(t2.a+0) from t1 t0) from t1 left join t1 t2 on t1.a=t2.a;
--echo # It's also not possible to set reliable used_tables information after eliminating; so we cannot eliminate t1 here; count() would still pretend it depends on t2.
explain select distinct (select count(t1.a+0) from t1 t0) from t1 left join t1 t2 on t1.a=t2.a;
--echo # Here the aggregate in the subquery, is not an aggregate from the POV of the query block where the left join is, so there is no problem.
explain select distinct (select count(t0.a+0) from t1 t0) from t1 left join t1 t2 on t1.a=t2.a;

--echo # Without GROUP BY, and with composite unique key:

CREATE TABLE u (a INT, b INT, c INT NOT NULL, UNIQUE (a,b));

--echo # Only one key part is functionally dependent, no elimination:

let $query=
SELECT COUNT(1) FROM
(SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON t.a = u.b
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
) AS derived;
eval EXPLAIN $query;
eval $query;

--echo # Both one key parts are functionally dependent, elimination:

let $query=
SELECT COUNT(1) FROM
(SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON t.a = u.b and t.b = u.a
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
) AS derived;
eval EXPLAIN $query;
eval $query;

--echo # Same, expressed as ROW-equality; we also change the aggregate
--echo # a bit and "nest" it

let $query=
SELECT 1+(2*SUM(derived.a)) FROM
(SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON (t.a, t.b) = (u.b, u.a)
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
) AS derived;
eval EXPLAIN $query;
eval $query;

--echo # Only one key part is functionally dependent, but there is also
--echo # GROUP BY, so elimination

let $query=
SELECT COUNT(1) FROM
(SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON t.a = u.b
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
 GROUP BY t.a
) AS derived;
eval EXPLAIN $query;
eval $query;

--echo # One note: join elimination and COUNT(not-null) optimization have no
--echo # chance to work together: we could hope that the latter would
--echo # eliminate a reference to the right-side table in COUNT, so maybe
--echo # elimination could then happen; but actually no: the right-side table
--echo # is potentially NULL-complemented by definition, so COUNT(not-null)
--echo # cannot apply. Example below, even though in base table 'u' the
--echo # column 'c' is not nullable, we cannot eliminate 'u':

let $query=
SELECT 1+(2*COUNT(derived.uc)) FROM
(SELECT t.a, u.c AS uc,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON (t.a, t.b) = (u.b, u.a)
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
) AS derived;
eval EXPLAIN $query;
eval $query;

--echo # Re-checking three previous cases of elimination, with a view
--echo # instead of a derived table.

CREATE VIEW v1 AS SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON t.a = u.b+1
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
 GROUP BY t.a, computed_col;
 
let $query=
SELECT COUNT(1) FROM v1;
eval EXPLAIN $query;
eval $query;

DROP VIEW v1;

CREATE VIEW v1 AS 
SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON (t.a, t.b) = (u.b, u.a)
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a;

let $query=
SELECT 1+(2*SUM(a)) FROM v1;
eval EXPLAIN $query;
eval $query;

DROP VIEW v1;

CREATE VIEW v1 AS 
SELECT t.a,
 CASE WHEN t.b = '1' THEN 'ab' ELSE 'cd' END AS computed_col
 FROM t1 AS t
 LEFT JOIN u ON t.a = u.b
 INNER JOIN t2 AS v ON v.b+2 = t.a
 INNER JOIN t2 AS w ON w.b+3 = t.a
 GROUP BY t.a;

let $query=
SELECT COUNT(1) FROM v1;
eval EXPLAIN $query;
eval $query;

DROP VIEW v1;

DROP TABLE u;

--echo # Now an imaginary example which looks realistic: we have two
--echo # tables with PK-FK relationship, a view which presents information
--echo # gathered from both tables, and a query which happens to select only
--echo # certain columns, making one table actually unnecessary.

CREATE TABLE person(name VARCHAR(10) PRIMARY KEY, zip_code CHAR(6));
CREATE TABLE town(zip_code CHAR(6) PRIMARY KEY, name VARCHAR(10));
INSERT INTO person VALUES('M. A', NULL),('Ms. B', '170150');
INSERT INTO town VALUES('170150', 'Quito'), ('090150', 'Guayaquil');
--echo # A view which presents the person's name and the town's name; using a LEFT JOIN in case the person has a NULL (unknown) zip_code.
CREATE VIEW person_town AS SELECT p.name AS p_name, t.name AS t_name
FROM person p LEFT JOIN town t ON p.zip_code=t.zip_code;

--echo # All information is requested, cannot eliminate 'town'
let $query=
SELECT * FROM person_town;
eval EXPLAIN $query;
eval $query;
--echo # Can eliminate
let $query=
SELECT p_name FROM person_town;
eval EXPLAIN $query;
eval $query;
DROP VIEW person_town;
DROP TABLE person, town;

--echo # (V) Misc

--echo # Show a little difference between the query after elimination of LEFT
--echo # JOIN t3, and the query if it had no LEFT JOIN t3 to begin with: in
--echo # the first case, the nest (t2-t3) becomes (t2), whereas in the second
--echo # case, (t2) becomes just t2 as an effect of simplify_joins().
--echo # simplify_joins() runs before join elimination which is why, in the
--echo # first case, (t2) remains (t2). We could run simplify_joins() a
--echo # second time after join elimination, if something has been
--echo # eliminated, but such function is not quite re-rentrant which leads
--echo # to crashes. And it is not worth it: the Optimizer works just as well
--echo # with (t2) as with just t2, as we see in the identical query plans.

let $query=
SELECT DISTINCT t1.a,t2.a FROM t1 LEFT JOIN (t2 LEFT JOIN t2 AS t3 ON TRUE) ON TRUE;
eval EXPLAIN $query;
eval $query;
let $query=
SELECT DISTINCT t1.a,t2.a FROM t1 LEFT JOIN (t2) ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # In a prepared statement, with plan cache

FLUSH STATUS;
PREPARE s FROM 'EXPLAIN SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.b=t1.a';
EXECUTE s;
EXECUTE s;
EXECUTE s;
SHOW STATUS LIKE "cached_plan_hits";
PREPARE s FROM 'SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.b=t1.a';
EXECUTE s;
EXECUTE s;
EXECUTE s;
SHOW STATUS LIKE "cached_plan_hits";

--echo # and without plan cache (two tables => no plan cache)
PREPARE s FROM 'EXPLAIN SELECT DISTINCT t1.a,t2.a FROM t1 LEFT JOIN (t2 LEFT JOIN t2 AS t3 ON TRUE) ON TRUE';
EXECUTE s;
PREPARE s FROM 'SELECT DISTINCT t1.a,t2.a FROM t1 LEFT JOIN (t2 LEFT JOIN t2 AS t3 ON TRUE) ON TRUE';
EXECUTE s;
DROP PREPARE s;

# Multiple eliminated nests

let $query=
SELECT DISTINCT t1.a,t3.a,t5.a FROM t1 LEFT JOIN t2 ON TRUE LEFT JOIN t1 AS t3 ON TRUE LEFT JOIN (t1 AS t4, t1 AS t6) ON TRUE LEFT JOIN (t1 AS t5, t1 AS t7) ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # Same, with more nesting

let $query=
SELECT DISTINCT t1.a,t3.a,t5.a,t9.a FROM (t1 LEFT JOIN t2 ON TRUE LEFT JOIN t1 AS t3 ON TRUE LEFT JOIN (t1 AS t4, t1 AS t6) ON TRUE LEFT JOIN (t1 AS t5, t1 AS t7) ON TRUE) LEFT JOIN (t1 AS t8 LEFT JOIN (t1 AS t9, t1 AS t10 LEFT JOIN t1 AS t11 ON TRUE) ON TRUE) ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # Elimination in INSERT SELECT ... ON DUPLICATE KEY UPDATE.
--echo # t5 could be eliminated from the SELECT if it were just a
--echo # SELECT, but here should not as it's used in the UPDATE clause. If we
--echo # accidentally eliminate it, and given that we zero its read_set, then
--echo # when reading t5.a to calculate t5.a+0, we fail the assertion
--echo # ASSERT_COLUMN_MARKED_FOR_READ in Field_long::val_int(), as is
--echo # appropriate.

CREATE TABLE t3 (a INT PRIMARY KEY);
INSERT INTO t3 VALUES (100);
CREATE TABLE t4 (a INT PRIMARY KEY);
INSERT INTO t4 VALUES (100);

--echo # So this cannot eliminate
let $query=
INSERT INTO t3 SELECT DISTINCT t4.a FROM t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a ON DUPLICATE KEY UPDATE t3.a=t5.a+0;
eval EXPLAIN $query;
eval $query;
--echo # vs
let $query=
INSERT INTO t3 SELECT DISTINCT t4.a FROM t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a ON DUPLICATE KEY UPDATE t3.a=t4.a+0;
eval EXPLAIN $query;
eval $query;

--echo # REPLACE SELECT is simpler, having no ON DUPLICATE KEY UPDATE,
--echo # we can eliminate.
let $query=
replace into t3 SELECT DISTINCT t4.a FROM t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a;
eval EXPLAIN $query;
eval $query;

--echo # Multi-table {UPDATE, DELETE} also have a possible problem
--echo # with references, like in the query below. For simplicity, we just
--echo # skip elimination for these two types of statements.

let $query=
UPDATE t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a SET t4.a=t5.a+0;
eval EXPLAIN $query;
eval $query;
let $query=
DELETE t5.* FROM t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a ;
eval EXPLAIN $query;
eval $query;

--echo # Elimination is always possible in any type statement if the
--echo # FROM clause is in a subquery

let $query=
UPDATE t3 SET t3.a = (SELECT t4.a FROM t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a LIMIT 1)+2;
eval EXPLAIN $query;
# subquery returns empty result
--error ER_BAD_NULL_ERROR
eval $query;
let $query=
INSERT INTO t3 VALUES((SELECT t4.a FROM t4 LEFT JOIN t4 AS t5 ON t4.a=t5.a ORDER BY t4.a LIMIT 1)+5);
eval EXPLAIN $query;
--error ER_BAD_NULL_ERROR
eval $query;

DROP TABLE t3,t4;

DROP TABLE t1,t2;

--echo # BUG2025111736066 When 100 SQLsmith services are concurrently
--echo # executed, mysqld crashes：JOIN::attach_join_condition_to_nest

CREATE TABLE t1 (a INT);
CREATE TABLE t2 LIKE t1;
--echo # We need a few rows in t2 for the optimizer to pick a wrong order of tables
--echo # (ref_15-ref_0-ref_5 without the code fix)
INSERT INTO t2 VALUES(1);
INSERT INTO t2 SELECT * FROM t2;
INSERT INTO t2 SELECT * FROM t2;
INSERT INTO t2 SELECT * FROM t2;
INSERT INTO t2 SELECT * FROM t2;
INSERT INTO t2 SELECT * FROM t2;
ANALYZE TABLE t1,t2;

let $query=
SELECT DISTINCT  
ref_0.*
FROM
t1 AS ref_15
LEFT JOIN
(
     (t2 AS ref_5
      LEFT JOIN t1 AS ref_6
      ON TRUE)
   
     LEFT JOIN
   
     t1 AS ref_0
     ON TRUE
) 
ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo #
--echo # BUG2025111835949 Execute query with outer join and semijoin, mysqld crashes:JOIN::make_join_plan
--echo #

let $query=
SELECT t2.a FROM
t1 LEFT JOIN t1 t2
ON
(t2.a) IN
 (SELECT t4.a FROM
  t1 t3 LEFT JOIN t1 t4 ON TRUE
  INNER JOIN t1 t5 ON FALSE)
GROUP BY 1;
eval EXPLAIN $query;
eval $query;

DROP TABLE t1,t2;

--echo #
--echo # BUG2025111917210 The results of the left join query are inconsistent.
--echo #

CREATE TABLE t0 (c1 TINYINT DEFAULT '-45');
INSERT INTO t0 VALUES(0),(1);
CREATE TABLE t98 (c3 DECIMAL(10,0) NOT NULL DEFAULT '1317787993');

--echo # The returned value, NULL, is a bug in MySQL, due to
--echo # NULL-complementing of t98 :
--echo # https://bugs.mysql.com/bug.php?id=119453 , result should be 1317787993.

let $query=
SELECT DEFAULT(t98.c3) FROM t0 LEFT JOIN t98 ON TRUE;
eval EXPLAIN $query;
eval $query;

--echo # Elimination is here disabled because there is DEFAULT(X)
--echo # where X is a column of a table on the right side of a left join.
let $query=
SELECT DEFAULT(t98.c3) FROM t0 LEFT JOIN t98 ON TRUE GROUP BY 1;
eval EXPLAIN $query;
eval $query;

--echo # Elimination is not disabled because there is DEFAULT(X)
--echo # where X is a column of a table _not_ on the right side of a left join.
let $query=
SELECT DEFAULT(t0.c1) FROM t0 LEFT JOIN t98 ON TRUE GROUP BY 1;
eval EXPLAIN $query;
eval $query;

--echo # This query is not respecting GROUP BY's standard rules, but
--echo # has deterministic result.
SET SQL_MODE=0;

--echo # Elimination is disabled. Given that DEFAULT evaluates to NULL (bug
--echo # above), the fourth expression in GROUP BY takes two values: NULL and
--echo # 1. Note that if we did elimination, there would be no
--echo # NULL-complementing, DEFAULT would evaluate to 1317787993, the fourth
--echo # expression in GROUP BY would take only one value: 1.
 
let $query=
SELECT t0.c1 FROM t0 NATURAL LEFT JOIN t98 GROUP BY '[', (((NOT (((-140958305)OR('2LE冑W.'))))) IS NULL), NULL, ((((t0.c1)OR(DEFAULT(t98.c3)))) IS NOT NULL), PI();
eval EXPLAIN $query;
eval $query;

--echo # The same within a merged derived table and a merged semi-join, to
--echo # verify that information about the presence of DEFAULT propagates
--echo # through both. We cannot have GROUP BY in the derived table, or it
--echo # would not be merged. See in EXPLAIN that there is no elimination.

let $query=
SELECT * FROM t0 AS t1 WHERE 1 IN (SELECT * FROM (SELECT t0.c1 FROM t0 NATURAL LEFT JOIN t98 WHERE ((((t0.c1)OR(DEFAULT(t98.c3)))) IS NOT NULL)) AS derived);
eval EXPLAIN $query;
eval $query;

SET SQL_MODE=DEFAULT;

--echo # BUG2025112534813 In the SQL statement for querying a common
--echo # table left join derived table, the query result is incorrect.

let $query=
SELECT subq_1.c1 AS c0 FROM (SELECT subq_0.c1 AS c1 FROM t0 ref_0 LEFT JOIN (SELECT PI() AS c1 FROM t98 ref_1) subq_0 ON FALSE) subq_1 GROUP BY 1;
--echo # After fixing the bug there should be no elimination
eval EXPLAIN $query;
eval $query;

--echo # BUG2025121228767 explain analyze crash when set global
--echo # optimizer_switch='left_join_elimination=on'; in debug version

let $query=
SELECT t0.c1 FROM t0 LEFT JOIN t0 AS t1 ON (t1.c1,t0.c1)=(SELECT 1,2);
eval EXPLAIN $query;
eval $query;

DROP TABLE t0,t98;

--echo # BUG2025112734711 Execute query and enable
--echo # partial_result_cache and left_join_elimination at the same
--echo # time, mysqld crashes：CreateNestedLoopAccessPath

CREATE TABLE t1 (medal_now INT);
CREATE TABLE t2 (id INT);
CREATE TABLE t3 (medal_now INT);

INSERT INTO t1 (medal_now) VALUES (1);
INSERT INTO t2 (id) VALUES (1), (2), (3);
INSERT INTO t3 (medal_now) VALUES (1), (2), (NULL);

--echo # Used to crash. This CASE is always non-NULL, so IS NULL says it is
--echo # constant FALSE and thus contains no reference so ref_49 is eliminated
--echo # from the left join; still the subquery remains in the tree of
--echo # Query_expression-s, gets optimized, Partial Table Result Cache is set
--echo # up for it, sees that it depends on outer value ref49.medal_now, and
--echo # then crashes when trying to access statistics of ref_49 which do not
--echo # exist as ref_49 has not been dealt with by the Optimizer when
--echo # optimizing the outer query.

let $query=
SELECT DISTINCT
(CASE WHEN EXISTS (SELECT ref_49.medal_now FROM t1) THEN 1 ELSE 0 END) IS NULL
FROM t2 LEFT JOIN t3 AS ref_49 ON TRUE;
eval EXPLAIN $query;
eval $query;

DROP TABLE t1,t2,t3;

--echo # BUG2025112734711 Stored procedure + materialization scenario:
--echo # counter Query_block::m_hwm_free_tableno keeps increasing,
--echo # mysqld crashes at set_tableno.

CREATE TABLE tbl_range_table (
  id_col int  AUTO_INCREMENT PRIMARY KEY
);
INSERT INTO tbl_range_table (id_col) VALUES (DEFAULT), (DEFAULT);

DELIMITER $$;
CREATE PROCEDURE t3()
BEGIN
    DECLARE i INT DEFAULT 1;
    
    WHILE i <= 61 DO
        EXPLAIN ANALYZE SELECT * FROM `tbl_range_table` WHERE id_col IN ( SELECT id_col FROM ( SELECT id_col FROM tbl_range_table limit 1) tmp );
        SET i = i + 1;
    END WHILE;
END$$
DELIMITER ;$$
--disable_result_log
call t3();
--enable_result_log
DROP PROCEDURE t3;
DROP TABLE tbl_range_table;

--echo #
--echo # BUG2026030437596 crash in JOIN::finalize_derived_keys
--echo #

--echo # This testcase exercises both if-s added to code. cte1 is eliminated, an index is added for the other reference (cte) due to the equality, and when adding this index we used to access cte1's JOIN_TAB which is nullptr.
CREATE TABLE t(a INT);
let $query=
WITH cte AS (SELECT * FROM t) SELECT /*+ NO_MERGE() */ DISTINCT cte.a FROM cte, t LEFT JOIN cte cte1 ON TRUE WHERE cte.a=t.a;
eval EXPLAIN $query;
eval EXPLAIN FORMAT=TREE $query;
eval $query;

--echo #
--echo # BUG2026030438541 crash in JOIN::compare_costs_of_subquery_strategies
--echo #

--echo # This testcase which exercises all changes in the bugfix. cte1 is eliminated, COUNT()'s used_tables() still referenced it, so when doing cost calculations for semijoin materialization we expected to be able to access cte1 through map2table. After fixing this, we used to hit an assertion failure in Query_expression::optimize() (assert(!is_recursive())).
let $query=
WITH RECURSIVE cte AS (SELECT * FROM t UNION SELECT * FROM cte)
SELECT * FROM t AS y
WHERE y.a IN (SELECT /*+ NO_MERGE() SEMIJOIN(MATERIALIZATION) */ DISTINCT COUNT(cte.a) OVER()
              FROM cte LEFT JOIN cte cte1 ON TRUE, t WHERE cte.a=t.a GROUP BY t.a);
eval EXPLAIN $query;
eval EXPLAIN FORMAT=TREE $query;
eval $query;
DROP TABLE t;
