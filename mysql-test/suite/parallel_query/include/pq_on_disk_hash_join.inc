# A set of queries that is reused multiple times in pq_hash_join.test

# Do a reconnect so that any changes to global variables
# (like vfd_max_open_files) becomes visible.
--disconnect default
--connect (default,localhost,root,,test)
ANALYZE TABLE t1;

### Test inner join

# Make sure that the IO statistics are empty.
TRUNCATE performance_schema.file_summary_by_event_name;

SELECT COUNT_STAR = 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

--replace_regex /cost=[0-9e\.\+]+/cost=***/  /rows=[0-9e\.\+]+/rows=***/
EXPLAIN FORMAT=tree SELECT /*+ PQ(t2) */ COUNT(*) FROM t1 JOIN t1 t2 ON t1.s = t2.s;

# Before sending EOF after executing the hash join, open a new connection and
# look at the performance schema to see that the number of open files is equal
# to or below vfd_max_open_files.
SET debug_sync = "before_send_eof SIGNAL stats_ready
                  WAIT_FOR main_thread_continue";
--send SELECT /*+ PQ(t2) */ COUNT(*) FROM t1 JOIN t1 t2 ON t1.s = t2.s;

--connect (stats_connection, localhost, root,,test)
SET debug_sync = "now WAIT_FOR stats_ready";
SELECT
  COUNT(*) BETWEEN FLOOR(@@vfd_max_open_files * 0.9) AND @@vfd_max_open_files,
  @@vfd_max_open_files
FROM performance_schema.file_instances
WHERE EVENT_NAME LIKE "%hash_join%" AND OPEN_COUNT > 0;
SET debug_sync = "now SIGNAL main_thread_continue";

--connection default
--reap


SELECT COUNT_STAR > 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";


### Test outer join

TRUNCATE performance_schema.file_summary_by_event_name;

SELECT COUNT_STAR = 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

--replace_regex /cost=[0-9e\.\+]+/cost=***/  /rows=[0-9e\.\+]+/rows=***/
EXPLAIN FORMAT=tree SELECT COUNT(*) FROM t1 LEFT JOIN t1 t2 ON t1.s = t2.s;
SET debug_sync = "before_send_eof SIGNAL stats_ready
                  WAIT_FOR main_thread_continue";
--send SELECT COUNT(*) FROM t1 LEFT JOIN t1 t2 ON t1.s = t2.s;

--connection stats_connection
SET debug_sync = "now WAIT_FOR stats_ready";
SELECT
  COUNT(*) BETWEEN FLOOR(@@vfd_max_open_files * 0.9) AND @@vfd_max_open_files,
  @@vfd_max_open_files
FROM performance_schema.file_instances
WHERE EVENT_NAME LIKE "%hash_join%" AND OPEN_COUNT > 0;
SET debug_sync = "now SIGNAL main_thread_continue";

--connection default
--reap

SELECT COUNT_STAR > 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

### Test semijoin

TRUNCATE performance_schema.file_summary_by_event_name;

SELECT COUNT_STAR = 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

SET optimizer_switch = "materialization=off";
--replace_regex /cost=[0-9e\.\+]+/cost=***/  /rows=[0-9e\.\+]+/rows=***/
EXPLAIN FORMAT=tree SELECT COUNT(*) FROM t1 WHERE t1.s IN (SELECT s FROM t1 t2);
SET debug_sync = "before_send_eof SIGNAL stats_ready
                  WAIT_FOR main_thread_continue";
--send SELECT COUNT(*) FROM t1 WHERE t1.s IN (SELECT s FROM t1 t2);

--connection stats_connection
SET debug_sync = "now WAIT_FOR stats_ready";
SELECT
  COUNT(*) BETWEEN FLOOR(@@vfd_max_open_files * 0.9) AND @@vfd_max_open_files,
  @@vfd_max_open_files
FROM performance_schema.file_instances
WHERE EVENT_NAME LIKE "%hash_join%" AND OPEN_COUNT > 0;
SET debug_sync = "now SIGNAL main_thread_continue";

--connection default
--reap

SELECT COUNT_STAR > 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

### Test antijoin

TRUNCATE performance_schema.file_summary_by_event_name;

SELECT COUNT_STAR = 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

--replace_regex /cost=[0-9e\.\+]+/cost=***/  /rows=[0-9e\.\+]+/rows=***/
EXPLAIN FORMAT=tree SELECT COUNT(*) FROM t1 WHERE t1.s NOT IN (SELECT s FROM t1 t2);
SET debug_sync = "before_send_eof SIGNAL stats_ready
                  WAIT_FOR main_thread_continue";
--send SELECT COUNT(*) FROM t1 WHERE t1.s NOT IN (SELECT s FROM t1 t2);

--connection stats_connection
SET debug_sync = "now WAIT_FOR stats_ready";
SELECT
  COUNT(*) BETWEEN FLOOR(@@vfd_max_open_files * 0.9) AND @@vfd_max_open_files,
  @@vfd_max_open_files
FROM performance_schema.file_instances
WHERE EVENT_NAME LIKE "%hash_join%" AND OPEN_COUNT > 0;
SET debug_sync = "now SIGNAL main_thread_continue";

--connection default
--reap

SELECT COUNT_STAR > 0 FROM performance_schema.file_summary_by_event_name
WHERE EVENT_NAME LIKE "%hash_join%";

SET optimizer_switch = DEFAULT;
--disconnect stats_connection
