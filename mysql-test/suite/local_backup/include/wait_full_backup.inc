#
# Wait full backup finish
#
# Usage:
#   [--let $skip_check_meta_json = 1]
#   --source suite/local_backup/include/wait_full_backup.inc
#
# Parameters:
#
#   $skip_check_meta_json
#     By default, 0 is configured. You can configure 1 by setting this variable.


# 15 min
--let $status_timeout= 9000
--let $status_var= Rds_local_backup_full_backup_is_running
--let $status_var_value= OFF
--source include/wait_for_status_var.inc

show status like '%local_backup_full_backup_is_running%';

# Download from obs if use obs.
--let $rds_local_backup_use_obs = `SELECT @@rds_local_backup_use_obs`
if ($rds_local_backup_use_obs == '1') {
  --source suite/local_backup/include/download_from_obs.inc
}

# Check backup_meta.json file content.
if (!$skip_check_meta_json)
{
  --let $check_status = Completed
  --source suite/local_backup/include/check_backup_meta_json.inc
}
