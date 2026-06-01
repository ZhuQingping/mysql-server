#
# check nested loop join + distinct optimization worked or not
#

--echo # with optimization, handler_read status
SET optimizer_switch="nlj_distinct_optimize=ON";
--disable_warnings
--replace_regex /\(cost=\d+(\.\d+)?(\.\.\d+(\.\d+)?)?\s+rows=\d+(\.\d+)?\)/(cost=X rows=X)/
eval EXPLAIN FORMAT=TREE $query;
--enable_warnings
FLUSH STATUS;
--disable_warnings
eval $query;
--enable_warnings
SELECT SUM(VARIABLE_VALUE) INTO @with_optimize_read FROM performance_schema.session_status WHERE VARIABLE_NAME LIKE 'Handler_read%';

--echo # without optimization, handler_read status
SET optimizer_switch="nlj_distinct_optimize=OFF";
--disable_warnings
--replace_regex /\(cost=\d+(\.\d+)?(\.\.\d+(\.\d+)?)?\s+rows=\d+(\.\d+)?\)/(cost=X rows=X)/
eval EXPLAIN FORMAT=TREE $query;
--enable_warnings
FLUSH STATUS;
--disable_warnings
eval $query;
--enable_warnings
SELECT SUM(VARIABLE_VALUE) INTO @without_optimize_read FROM performance_schema.session_status WHERE VARIABLE_NAME LIKE 'Handler_read%';

--echo # following check should always true
SELECT IF(@with_optimize_read < @without_optimize_read, 'TRUE', 'FALSE') AS 'check';


