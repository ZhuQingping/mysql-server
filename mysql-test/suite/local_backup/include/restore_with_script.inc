# ==== Purpose ====
#
# Restore with script.
#
# ==== Usage ====
#
#   [--let $list_type = full_backup]
#   [--let $list_type = wal_archive]
#   --source suite/local_backup/include/restore_with_script.inc
#
# Parameters:
#
#   $list_type
#     By default, full_backup is configured. You can configure wal_archive
#     by setting this variable.


# Set default value of $list_type full_backup.
if (!$list_type)
{
  --let $list_type = full_backup
}

--disable_result_log
--disable_query_log

--let $bc = $MYSQLTEST_VARDIR/backup_ctrl.py
--exec python $bc list_time_point $list_type

--exec python $bc create_data `grep '\"timePoint\"' $list_type\_time_point_list | tail -n 1 | awk '{print \$2}' | sed 's/.\$//'`
--exec python $bc copy_back

--exec rm -rf ./$list_type\_time_point_list

--enable_query_log
--enable_result_log
