#
# Stop log archive and wait until not running
#
# Usage:
#   --source suite/local_backup/include/stop_log_archive.inc
#

call rds_backup.stop_log_archive();
