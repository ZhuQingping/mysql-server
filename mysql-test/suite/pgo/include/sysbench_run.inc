
##############################################################################
#
# SUMMARY
#
#   Run sysbench OLTP RW benchmark on table(s) created by sysbench_prepare.inc
#
# USAGE
#
#   let $tables=10;
#   let transactions=10000
#   --source suite/pgo/include/sysbench_run.inc
#
#   $tables defaults to 1.
#
#   $transactions defaults to 500 (should take about 30 seconds on a
#   PGO-enabled build).
#
##############################################################################

if (!$tables)
{
  let $tables = 1;
}

if (!$transactions)
{
  let $transactions = 500;
}

--disable_query_log
--disable_result_log

while ($transactions)
{
  START TRANSACTION;

  # Point SELECTs
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $pscount=10;
  while ($pscount)
  {
    let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
    eval SELECT * FROM sbtest$table WHERE id=$id;
    dec $pscount;
  }

  # Simple range
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval SELECT c FROM sbtest$table WHERE id BETWEEN $id and $id+100;

  # Sum range
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval SELECT SUM(k) FROM sbtest$table WHERE id BETWEEN $id and $id+100;

  # Order range
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval SELECT c FROM sbtest$table WHERE id BETWEEN $id and $id+100 ORDER BY c;

  # Distinct range
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval SELECT DISTINCT c FROM sbtest$table WHERE id BETWEEN $id and $id+100 ORDER BY c;

  # Index update
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval UPDATE sbtest$table SET k=k+1 WHERE id=$id;

  # Non-index update
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval UPDATE sbtest$table
    SET c=CONCAT(REPEAT(CONCAT(FLOOR(RAND()*100000000000), '-'), 9), FLOOR(RAND()*100000000000))
    WHERE id=$id;

  # DELETE
  let $table=`SELECT FLOOR(1 + (RAND() * $tables))`;
  let $id=`SELECT FLOOR(1 + RAND() * 1048576)`;
  eval DELETE FROM sbtest$table WHERE id=$id;

  # INSERT
  eval INSERT INTO sbtest$table VALUES
    ($id, FLOOR(1 + RAND() * 1048576),
    CONCAT(REPEAT(CONCAT(FLOOR(RAND()*100000000000), '-'), 9), FLOOR(RAND()*100000000000)),
    CONCAT(REPEAT(CONCAT(FLOOR(RAND()*100000000000), '-'), 4), FLOOR(RAND()*100000000000)));

  COMMIT;

  dec $transactions;
}

# Reset transactions, so we don't use its current value (0) on repeated
# inclusions
let $transactions=;

--enable_result_log
--enable_query_log
