####################################################
# Explain in all format and execute the query      #
# ==== Usage ====                                  #
# --let $query= query                              #
# --let $explain= traditional,json,tree,opt_trace  #
####################################################

--echo # Explain and execute query: $query
--eval set @explain = $explain;
--let $traditional=`select find_in_set("traditional", @explain)`
--let $json=`select find_in_set("json", @explain)`
--let $tree=`select find_in_set("tree", @explain)`
--let $opt_trace=`select find_in_set("opt_trace", @explain)`

if($traditional)
{
  --replace_column 10 # 11 #
  --eval EXPLAIN $query
}

if($tree)
{
  --replace_regex /cost=\d+.*?\s/cost=*** /  /rows=\d+.*?\)/rows=***)/
  --eval EXPLAIN FORMAT=tree $query
}

if($json)
{
  --replace_regex /"\d+.\d+"/***/ /"\d+"/***/
  --eval EXPLAIN FORMAT=json $query
}

if($opt_trace)
{
  SET optimizer_trace="enabled=on";
}

--eval $query

if($opt_trace)
{
  --replace_regex /"cost": \d+.*\d+/"cost": ***/ /"cost_for_plan": \d+.*\d+/"cost_for_plan": ***/
  SELECT * FROM information_schema.OPTIMIZER_TRACE;
  SET optimizer_trace="enabled=off";
}
