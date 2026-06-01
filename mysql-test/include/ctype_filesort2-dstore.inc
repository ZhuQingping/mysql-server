#
# Testing filesort for full Unicode character sets
# with supplementary characters.
#

--echo #
--echo # BUG#16691598 - ORDER BY LOWER(COLUMN) PRODUCES
--echo #                OUT-OF-ORDER RESULTS
--echo #

--source include/have_only_dstore.inc

CREATE TABLE  t1 SELECT ('a a') as n;
INSERT INTO t1 VALUES('a b');
SELECT * FROM t1 ORDER BY LOWER(n) ASC;
SELECT * FROM t1 ORDER BY LOWER(n) DESC;
DROP TABLE t1;
