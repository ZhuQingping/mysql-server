--disable_warnings
let $threadpool_enabled= `SELECT count(*) FROM performance_schema.global_variables WHERE
      VARIABLE_NAME = 'THREAD_HANDLING' AND
      VARIABLE_VALUE = 'loaded-dynamically'`;
if (!$threadpool_enabled)
{
  --skip Test requires threadpool
}
--enable_warnings
