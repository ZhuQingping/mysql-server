########################################################################
#
# SUMMARY
#
#   Create and populate table(s) using the sysbench OLTP schema
#
# USAGE
#
#   let $tables=10;
#   --source suite/pgo/include/sysbench_prepare.inc
#
#   $tables defaults to 1.
#
########################################################################

if (!$tables)
{
  let $tables = 1;
}

--disable_query_log
while ($tables)
{
  eval CREATE TABLE sbtest$tables(
    id INTEGER NOT NULL AUTO_INCREMENT,
    k INTEGER DEFAULT '0' NOT NULL,
    c CHAR(120) DEFAULT '' NOT NULL,
    pad CHAR(60) DEFAULT '' NOT NULL,
    PRIMARY KEY (id));
  eval INSERT INTO sbtest$tables(id, k, c, pad) VALUES
    (1, FLOOR(1 + RAND() * 1048576),
    CONCAT(REPEAT(CONCAT(FLOOR(RAND()*100000000000), '-'), 9), FLOOR(RAND()*100000000000)),
    CONCAT(REPEAT(CONCAT(FLOOR(RAND()*100000000000), '-'), 4), FLOOR(RAND()*100000000000)));
  # Generate 2^20 = 1048576 rows
  let $i = 0;
  let $base = 1;
  while ($i < 20)
  {
    eval INSERT INTO sbtest$tables(id, k, c, pad)
      SELECT $base + id, FLOOR(1 + RAND() * 1048576), c, pad FROM sbtest$tables;
    inc $i;
    let $base = $base * 2;
  }

  eval CREATE INDEX k_$tables ON sbtest$tables(k);
  eval ANALYZE TABLE sbtest$tables;

  dec $tables;
}
--enable_query_log
