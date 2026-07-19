# include/have_rds_binlog_lock_enable.inc
#
# rds_binlog_lock_enable is an optional TaurusDB system variable.  The backup
# lock tests below exercise its enabled and disabled behavior.

if (`SELECT COUNT(*) = 0 FROM performance_schema.global_variables WHERE variable_name = 'rds_binlog_lock_enable'`)
{
  --skip Test requires rds_binlog_lock_enable
}
