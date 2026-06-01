# ==== Usage ====
#
# [--let $table_name = "TABLE_NAME"]
# [--let $columns = "COL1" [, "COLn"]]
# [--let $schema_name = "SCHEMA_NAME"]
# --source include/pre_wait_for_histogram.inc
# ... INSERT OR MODIFY DATA ...
# --source include/post_wait_for_histogram.inc

if (!$schema_name) {
    --let $schema_name = `SELECT CONCAT("""", database(), """")`
}

# Wait until there are no histograms that are older or equal to $max_histogram_date .
# The query does a LEFT JOIN on INFORMATION_SCHEMA.COLUMNS to include columns that do not have any
# histogram created for it yet.
--let $wait_condition = SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.COLUMNS c LEFT JOIN INFORMATION_SCHEMA.COLUMN_STATISTICS cs ON cs.SCHEMA_NAME = c.TABLE_SCHEMA AND cs.TABLE_NAME = c.TABLE_NAME AND cs.COLUMN_NAME = c.COLUMN_NAME WHERE c.TABLE_SCHEMA = $schema_name and c.TABLE_NAME = $table_name and c.COLUMN_NAME in ( $columns ) AND UNIX_TIMESTAMP(IFNULL(HISTOGRAM->>'$."last-updated"', 0)) <= $max_histogram_date;
--source include/wait_condition.inc
