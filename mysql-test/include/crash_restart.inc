#
# invoke commit that result in crash and then restart
#
--exec echo "restart " > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--echo "restarting after crash"
--enable_reconnect
--source include/wait_until_connected_again.inc
--disable_reconnect
