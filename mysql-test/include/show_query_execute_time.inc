# ==== Purpose ====
#
# Explain DDL/DCL/DML execution Time 
# 
# ==== Usage ====
#
# let $QUERY_STAT= 

# Parameters:
#
#   $QUERY_STAT
#     Query statement.

--echo # connect con1
connection con1;
--let $start_time= `select unix_timestamp(now())`
set DEBUG_SYNC='after_set_transaction_psi_before_set_transaction_gtid SIGNAL sleep WAIT_FOR commit';
--send_eval $QUERY_STAT

--echo # connect default
connection default;
SET  DEBUG_SYNC= 'now WAIT_FOR sleep';
select sleep(3);
--let $TRX_EXECUTED_TIME= `SELECT TRX_EXECUTED_TIME FROM INFORMATION_SCHEMA.PROCESSLIST ORDER BY TRX_EXECUTED_TIME DESC limit 1`

--disable_result_log
SHOW FULL PROCESSLIST;
--enable_result_log

SET  DEBUG_SYNC= 'now SIGNAL commit';

--echo # connect con1
connection con1;
--reap

--let $TIME_INTERVAL= `(select unix_timestamp(now()) - @start_time)`
--echo # Verify the correctness of the query time
if ($TRX_EXECUTED_TIME >= 3 and $TRX_EXECUTED_TIME <= $TIME_INTERVAL)
{
  echo # "Success";
}

--echo # query end, The TRX_EXECUTED_TIME is reset to 0.
SELECT TRX_EXECUTED_TIME FROM INFORMATION_SCHEMA.PROCESSLIST;
