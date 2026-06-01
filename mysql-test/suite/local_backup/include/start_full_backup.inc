#
# call rds_backup.start_full_local_backup(full_backup_flag);
#   full_backup_flag : MYSQL_TYPE_LONGLONG
#     0: Set none
#     1: Set LSN start point for incremental backup(wal archive)
#     2: Set LSN start point for replica
#     3: Set both 1 and 2
#
# Usage:
#   --let $full_backup_flag = 0 (see above)
#   --source suite/local_backup/include/start_full_backup.inc
#

eval call rds_backup.start_full_local_backup($full_backup_flag);

--let $status_timeout= 30
--let $status_var= Rds_local_backup_full_backup_is_running
--let $status_var_value= ON
--source include/wait_for_status_var.inc

show status like 'Rds_local_backup_full_backup_is_running';