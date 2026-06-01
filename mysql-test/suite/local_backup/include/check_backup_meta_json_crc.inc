# ==== Purpose ====
#
# Check if backup_meta.json file crc is matched.
#
# ==== Usage ====
#
#   --source suite/local_backup/include/check_backup_meta_json_crc.inc


--let $MYSQL_BASEDIR = `SELECT @@basedir`
--exec ls $MYSQLTEST_VARDIR/full_local_backup/* -trd | tail -n 1 > $MYSQLTEST_VARDIR/bakup_dir.txt
--exec $MYSQL_BASEDIR/bin/backup_monitor `cat $MYSQLTEST_VARDIR/bakup_dir.txt` -j -d $MYSQLTEST_VARDIR/crc.txt

--let SEARCH_FILE = $MYSQLTEST_VARDIR/crc.txt
--let SEARCH_PATTERN = "checkResult": "Success"
--source include/search_pattern.inc

--exec rm -rf $MYSQLTEST_VARDIR/bakup_dir.txt $MYSQLTEST_VARDIR/crc.txt
