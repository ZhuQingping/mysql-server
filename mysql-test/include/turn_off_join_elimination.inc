--disable_query_log
# Cryptic variable name to avoid name conflicts
set @previous_opt_switch_htnt542nh=@@optimizer_switch;
set optimizer_switch="left_join_elimination=off";
--enable_query_log

