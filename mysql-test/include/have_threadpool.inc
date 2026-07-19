# Skip tests that require the optional threadpool plugin when it was not built.

if (`SELECT COUNT(*) = 0
      FROM performance_schema.global_variables
      WHERE VARIABLE_NAME = 'THREADPOOL_ENABLED'`)
{
  --skip Test requires threadpool support
}
