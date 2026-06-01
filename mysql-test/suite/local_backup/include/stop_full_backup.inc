#
# Stop full backup
#
# Usage:
#   --source suite/local_backup/include/stop_full_backup.inc
#

call rds_backup.stop_full_local_backup();

show status like '%local_backup_full_backup_is_running%';
