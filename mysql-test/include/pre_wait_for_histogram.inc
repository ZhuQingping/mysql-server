# ==== Usage ====
#
# [--let $table_name = "TABLE_NAME"]
# [--let $columns = "COL1" [, "COLn"]]
# [--let $schema_name = "SCHEMA_NAME"]
# --source include/pre_wait_for_histogram.inc
# ... INSERT OR MODIFY DATA ...
# --source include/post_wait_for_histogram.inc
#

if (!$schema_name) {
    --let $schema_name = `SELECT CONCAT("""", database(), """")`
}

# Find the histogram that was last updated and store that timestamp. If there are no histogram for
# any of the given columns, we store "0".
# The query does a LEFT JOIN on INFORMATION_SCHEMA.COLUMNS to include columns that do not have any
# histogram created for it yet.
--let $max_histogram_date = `SELECT MAX(UNIX_TIMESTAMP(IFNULL(HISTOGRAM->>'$."last-updated"', 0))) FROM INFORMATION_SCHEMA.COLUMNS c LEFT JOIN information_schema.column_statistics cs ON cs.SCHEMA_NAME = c.TABLE_SCHEMA AND cs.TABLE_NAME = c.TABLE_NAME AND cs.COLUMN_NAME = c.COLUMN_NAME WHERE c.TABLE_SCHEMA = $schema_name and c.TABLE_NAME = $table_name and c.COLUMN_NAME in ( $columns );`
