########################################################################
#
# SUMMARY
#
#   Drop tables created by sysbench_prepare.inc
#
# USAGE
#
#   let $tables=10;
#   --source suite/pgo/include/sysbench_cleanup.inc
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
  eval DROP TABLE sbtest$tables;
  dec $tables;
}
--enable_query_log
