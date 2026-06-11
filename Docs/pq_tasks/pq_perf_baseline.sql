DROP PROCEDURE IF EXISTS pq_perf_fill;
DROP PROCEDURE IF EXISTS pq_perf_run_once;
DROP PROCEDURE IF EXISTS pq_perf_run_all;
DROP TABLE IF EXISTS pq_perf_runs;
DROP TABLE IF EXISTS pq_perf_t;

SET @pq_perf_rows := COALESCE(@pq_perf_rows, 1048576);
SET @pq_perf_repeat := COALESCE(@pq_perf_repeat, 3);

CREATE TABLE pq_perf_t (
  id INT NOT NULL PRIMARY KEY,
  pad CHAR(32) NOT NULL
) ENGINE=InnoDB;

DELIMITER //

CREATE PROCEDURE pq_perf_fill(IN target_rows INT)
BEGIN
  DECLARE current_rows INT DEFAULT 1;

  INSERT INTO pq_perf_t VALUES (1, 'a');
  WHILE current_rows < target_rows DO
    INSERT INTO pq_perf_t
      SELECT id + current_rows, pad
        FROM pq_perf_t
       WHERE id + current_rows <= target_rows;
    SELECT COUNT(*) INTO current_rows FROM pq_perf_t;
  END WHILE;
END//

CREATE PROCEDURE pq_perf_run_once(IN mode_name VARCHAR(16), IN dop INT)
BEGIN
  DECLARE cnt BIGINT DEFAULT 0;
  DECLARE sum_id DECIMAL(30,0) DEFAULT 0;
  DECLARE start_ts DATETIME(6);
  DECLARE elapsed_us BIGINT DEFAULT 0;
  DECLARE executed_before BIGINT DEFAULT 0;
  DECLARE fallback_before BIGINT DEFAULT 0;
  DECLARE rows_before BIGINT DEFAULT 0;
  DECLARE workers_before BIGINT DEFAULT 0;

  SET SESSION parallel_query=IF(mode_name = 'serial', 0, 1);
  SET SESSION parallel_cost_threshold=0;
  SET SESSION parallel_default_dop=dop;
  SET SESSION parallel_query_experimental_threaded_dop1=(mode_name = 'dop1');
  SET SESSION parallel_query_experimental_threaded_dop=(mode_name = 'dop2');
  SET SESSION parallel_query_experimental_threaded_dop4=(mode_name = 'dop4');

  SELECT CAST(variable_value AS UNSIGNED) INTO executed_before
    FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_queries_executed';
  SELECT CAST(variable_value AS UNSIGNED) INTO fallback_before
    FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_queries_fallback';
  SELECT CAST(variable_value AS UNSIGNED) INTO rows_before
    FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_rows_scanned';
  SELECT CAST(variable_value AS UNSIGNED) INTO workers_before
    FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_workers_launched';

  SET start_ts=NOW(6);
  SELECT COUNT(*), SUM(id) INTO cnt, sum_id FROM pq_perf_t WHERE pad='a';
  SET elapsed_us=TIMESTAMPDIFF(MICROSECOND, start_ts, NOW(6));

  INSERT INTO pq_perf_runs
  SELECT mode_name, dop, elapsed_us, cnt, sum_id,
         CAST(variable_value AS UNSIGNED) - executed_before,
         0, 0, 0
    FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_queries_executed';

  UPDATE pq_perf_runs
     SET fallback_delta = (
           SELECT CAST(variable_value AS UNSIGNED) - fallback_before
             FROM performance_schema.global_status
            WHERE variable_name = 'Parallel_queries_fallback'),
         rows_delta = (
           SELECT CAST(variable_value AS UNSIGNED) - rows_before
             FROM performance_schema.global_status
            WHERE variable_name = 'Parallel_rows_scanned'),
         workers_delta = (
           SELECT CAST(variable_value AS UNSIGNED) - workers_before
             FROM performance_schema.global_status
            WHERE variable_name = 'Parallel_workers_launched')
   WHERE run_id = LAST_INSERT_ID();

  SET SESSION parallel_query_experimental_threaded_dop1=OFF;
  SET SESSION parallel_query_experimental_threaded_dop=OFF;
  SET SESSION parallel_query_experimental_threaded_dop4=OFF;
  SET SESSION parallel_query=OFF;
  SET SESSION parallel_default_dop=4;
  SET SESSION parallel_cost_threshold=10000;
END//

CREATE PROCEDURE pq_perf_run_all(IN repeat_count INT)
BEGIN
  DECLARE i INT DEFAULT 0;

  CALL pq_perf_run_once('serial', 4);
  WHILE i < repeat_count DO
    CALL pq_perf_run_once('dop1', 1);
    CALL pq_perf_run_once('dop2', 2);
    CALL pq_perf_run_once('dop4', 4);
    SET i := i + 1;
  END WHILE;
END//

DELIMITER ;

CALL pq_perf_fill(@pq_perf_rows);

CREATE TABLE pq_perf_runs (
  run_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  mode_name VARCHAR(16) NOT NULL,
  dop INT NOT NULL,
  elapsed_us BIGINT NOT NULL,
  cnt BIGINT NOT NULL,
  sum_id DECIMAL(30,0) NOT NULL,
  executed_delta BIGINT NOT NULL,
  fallback_delta BIGINT NOT NULL,
  rows_delta BIGINT NOT NULL,
  workers_delta BIGINT NOT NULL
) ENGINE=InnoDB;

CALL pq_perf_run_all(@pq_perf_repeat);

SELECT mode_name, dop, elapsed_us, cnt, sum_id,
       executed_delta, fallback_delta, rows_delta, workers_delta
  FROM pq_perf_runs
 ORDER BY run_id;

SELECT mode_name, dop,
       COUNT(*) AS samples,
       ROUND(AVG(elapsed_us)) AS avg_elapsed_us,
       MIN(elapsed_us) AS min_elapsed_us,
       MAX(elapsed_us) AS max_elapsed_us,
       SUM(executed_delta) AS executed_delta_sum,
       SUM(fallback_delta) AS fallback_delta_sum,
       SUM(rows_delta) AS rows_delta_sum,
       SUM(workers_delta) AS workers_delta_sum
  FROM pq_perf_runs
 GROUP BY mode_name, dop
 ORDER BY FIELD(mode_name, 'serial', 'dop1', 'dop2', 'dop4');

DROP PROCEDURE pq_perf_run_once;
DROP PROCEDURE pq_perf_run_all;
DROP PROCEDURE pq_perf_fill;
