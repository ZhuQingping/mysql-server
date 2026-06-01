--disable_query_log
--disable_warnings

--error 0
SET optimizer_switch = 'aggregate_pushdown=on';
if (`SELECT NOT FIND_IN_SET('aggregate_pushdown=on', @@optimizer_switch)`) {
   --skip Test requires optimizer_switch aggregate_pushdown=on
}
--enable_warnings
--enable_query_log
