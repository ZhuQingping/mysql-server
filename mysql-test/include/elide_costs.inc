# Usage: --replace_regex $elide_costs

#  (cost=1.26..2.52 rows=2)  -> (<cost elided> rows=2)
#  (cost=0.35 rows=1)        -> (... rows=1)
let $elide_costs = /[(]cost=[0-9.e+-]+ rows/(rows/;

# Usage: --replace_regex $elide_costs_and_time
# This converts:
# (cost=1.26..2.52 rows=2) (actual time=63.071..64.263 rows=5 loops=2)
# into:
# -> (rows=2) (actual rows=5 loops=2)
#  (cost=0.35 rows=1)        -> (... rows=1)
let $elide_costs_and_time = /[(]cost=[0-9.e+-]+ rows/(rows/ /[(]actual time=[0-9.e+-]+ rows/(actual rows/;

# Usage: --replace_regex $elide_costs_and_time_and_row_estimate
# This converts:
# (cost=1.26..2.52 rows=2) (actual time=63.071..64.263 rows=5 loops=2)
# into:
# -> (...) (actual rows=5 loops=2)
#  (cost=0.35 rows=1)        -> (... rows=1)
let $elide_costs_and_time_and_row_estimate = /[(]cost=[0-9.e+-]+ rows=[0-9.e+-]+[)]/(...)/ /[(]actual time=[0-9.e+-]+ rows/(actual rows/;

# Usage: --replace_regex $elide_time
# This converts:
# (cost=1.26..2.52 rows=2) (actual time=63.071..64.263 rows=5 loops=2)
# into:
# -> (cost=1.26..2.52 rows=2) (rows=5 loops=2)
#  (cost=0.35 rows=1)        -> (... rows=1)
let $elide_time = /[(]actual time=[0-9.e+-]+ rows/(actual rows/;

# Usage: --replace_regex $elide_costs_and_rows
#  (cost=1.26..2.52 rows=2)  -> (...)
#  (cost=0.35 rows=1)        -> (...)
let $elide_costs_and_rows = /[(]cost=[0-9.e+-]+ rows=[0-9.e+-]+[)]/(...)/;

# Usage: --replace_regex $elide_metrics
# This removes all metrics from 'EXPLAIN ANALYZE' or 'EXPLAIN FORMAT=TREE', meaning
# that it converts:
# (cost=1.26..2.52 rows=2) (actual time=63.071..64.263 rows=5 loops=2)
# into an empty string.
#  (cost=1.26..2.52 rows=2)  -> empty string
#  (cost=0.35 rows=1)        -> empty string
# Contrary to the other patterns above, it does not even leave an
# ellipsis for the elided metrics. This is to make the output the same
# regardless of whether there actually is some metric to elide.
let $elide_metrics = / *[(]cost=.*//;

# Usage: --replace_regex $elide_actual_time_rows_loops
# This converts:
# (cost=101 rows=1000) (actual time=1.25..3.71 rows=1000 loops=1)
# into:
# -> (cost=101 rows=1000) (actual rows loops)
let $elide_actual_time_rows_loops = /actual time=[0-9,.e+-]+ rows=[0-9,.e+-]+ loops=[0-9,.e+-]+/actual rows loops/;

# Usage: --replace_regex $elide_pq_costs_and_time_and_row_estimate
# This converts:
# (cost=1.26..2.52 rows=2) (actual time=0.028,0.072,0.013..0.036,0.102,0.013 rows=0,2,0 loops=1,1,1)
# into:
# -> (...) (actual rows loops)
let $elide_pq_costs_and_time_and_row_estimate = /[(]cost=[0-9.e+-]+ rows=[0-9.e+-]+[)]/(...)/ /[(]actual time=[0-9,.e+-]+ rows/(actual rows/ /rows=[0-9,.e+-]+/rows/ /loops=[0-9,.e+-]+/loops/;
