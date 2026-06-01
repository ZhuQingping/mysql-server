--source include/have_debug.inc
--source include/have_dstore.inc

# Tests of session-level plan cache which require a debug binary

# We will be displaying global status variables which FLUSH STATUS does not reset (like Cached_plan_invalidations); if mysqld has already executed some other mtr tests and they used plan cache, this status will contain effects from those other tests, so restart:
--source include/restart_mysqld.inc

# PLAN_CACHE_PORT: here some sections moved from session_plan_cache.test

set rds_plan_cache=ON;

--echo "test optimizer trace for plan cache"
flush status;
show session status like "cached_plan%";
create table t1(a int, b int);
insert into t1 values(1,2),(3,4);
prepare stmt from 'select * from t1 where a > ?';
set @a = 1;
set optimizer_trace='enabled=on';
execute stmt using @a;
show session status like "cached_plan%";
execute stmt using @a;
show session status like "cached_plan%";

set debug='+d,apply_cached_plan_fail';
execute stmt using @a;
--replace_regex /  \"cost\".*// /  \"cost_for_plan\".*//
select * from information_schema.optimizer_trace;
show session status like "cached_plan%";
set optimizer_trace='enabled=off';
drop table t1;
set debug='-d,apply_cached_plan_fail';

--echo # BUG2025031715722 : mysqld crash, keyword:plan_cache::exec_cached_plan.

CREATE TABLE `sbtest` (
	`id` int(10) UNSIGNED NOT NULL AUTO_INCREMENT,
	`k` int(10) UNSIGNED NOT NULL DEFAULT '0',
	`c` char(120) NOT NULL DEFAULT '',
	`pad` char(60) NOT NULL DEFAULT '',
	PRIMARY KEY (`id`),
	KEY `k` (`k`)
) engine=innodb;

# PLAN_CACHE_PORT we use innodb above, not dstore; because we want to
# inject an error at the point ("pq_dup_tabs_for_plan_cache_error0") where the
# condition attached to the table is cloned; in dstore there is no
# such condition (it was moved to ICP).

DELIMITER |;
CREATE PROCEDURE InsertRandomData()
BEGIN
    DECLARE i INT DEFAULT 1;
    DECLARE random_value INT;
    DECLARE repeated_values_count INT DEFAULT 200;

    SET @@session.rand_seed1 = 12345;
    SET @@session.rand_seed2 = 67890;

    WHILE i <= 1000 DO
        IF i <= repeated_values_count THEN
            SET random_value = FLOOR(1 + (RAND() * 100));
        ELSE
            SET random_value = FLOOR(101 + (RAND() * 900));
        END IF;

        INSERT INTO sbtest (`k`, `c`, `pad`)
        VALUES (i, CONCAT('value_', random_value), CONCAT('pad_', i));

        SET i = i + 1;
    END WHILE;
END|
DELIMITER ;|

CALL InsertRandomData();

PREPARE stmt FROM 'SELECT DISTINCT c FROM sbtest WHERE id BETWEEN ? AND ? ORDER BY c';

SET @n = 10;
SET @m = 15;
EXECUTE stmt USING @n, @m;
EXECUTE stmt USING @n, @m;

SET debug= "+d,plan_cache_debug_test_quick_select_fail";
EXECUTE stmt USING @n, @m;
SET debug="-d,plan_cache_debug_test_quick_select_fail";

DROP PROCEDURE InsertRandomData;
DEALLOCATE PREPARE stmt;

# PLAN_CACHE_PORT New sections here.

PREPARE stmt FROM 'SELECT DISTINCT c FROM sbtest WHERE id BETWEEN ? AND ? ORDER BY c';
# Simulate OOM failure which happens when caching the plan
SET session debug="+d,pq_dup_tabs_for_plan_cache_error0";
flush status;
EXECUTE stmt USING @n, @m;
EXECUTE stmt USING @n, @m;
EXECUTE stmt USING @n, @m;
# Nothing should be cached
show session status like "cached_plan%";
SET session debug="-d,pq_dup_tabs_for_plan_cache_error0";
PREPARE stmt FROM 'SELECT DISTINCT c FROM sbtest WHERE id BETWEEN ? AND ? ORDER BY c';
EXECUTE stmt USING @n, @m;
EXECUTE stmt USING @n, @m;
EXECUTE stmt USING @n, @m;
# Now caching should work
show session status like "cached_plan%";
DEALLOCATE PREPARE stmt;

if ($engine_is_dstore)
{
--echo #
--echo # BUG2025070919201 after enable rds_plan_cache, using the sysbench range query model(select_random_ranges.lua), the performance fluctuates sharply and the performance decreases by 5% to 25%
--echo #

# Even though EXPLAIN reports 'Using index', the actual reading of the constant table read the full row. Make sure this does not happen anymore:
alter table sbtest engine=dstore;
set debug='+d,dstore_must_read_only_index';
explain SELECT id FROM sbtest WHERE id=100;
prepare s from 'SELECT id FROM sbtest WHERE id=?';
set @a=100;
execute s using @a;
execute s using @a;
execute s using @a;
show session status like "cached_plan%";
set debug='-d,dstore_must_read_only_index';
}

DROP TABLE sbtest;
