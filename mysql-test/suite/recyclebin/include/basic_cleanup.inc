
connection default;
set rds_recycle_bin_mode = OFF;
drop database sbtest1;
drop database sbtest2;
drop user test1@localhost;
drop user test2@localhost;
drop user my_root@localhost;

set global rds_recycle_bin_mode = @saved_rds_recycle_bin_mode;