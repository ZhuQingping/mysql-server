# Allow anonymous users to connect 
disable_warnings;
disable_query_log;
set @save_rds_allow_empty_users = @@global.rds_allow_empty_users;
set @@global.rds_allow_empty_users = ON;
INSERT IGNORE INTO mysql.user (host, user) VALUES ('localhost','');
FLUSH PRIVILEGES;
set @@global.rds_allow_empty_users = @save_rds_allow_empty_users;
enable_query_log;
enable_warnings;
