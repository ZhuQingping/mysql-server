
--source include/have_log_bin.inc
DELIMITER |;
CREATE PROCEDURE delete_one_sql_filter_rule(IN offset_value INT)
begin
declare my_var BIGINT default 1;
select item_id into my_var from mysql.rds_sql_filter_rules limit offset_value,1;
call dbms_sqlfilter.delete_sql_filter(my_var);
end |
DELIMITER ;|
