# include/not_have_only_dstore.inc
#
# Engine support have 4 states, which are 'YES', 'NO', 'DISABLED', 'DEFAULT'.
# In this case DStore engine should be 'YES' or 'NO' or 'DISABLED'.
# Skip the test if Dstore is default engine

if (`SELECT COUNT(*) = 0 FROM information_schema.engines WHERE ENGINE = "Dstore" AND SUPPORT IN ('YES', 'NO', 'DISABLED')`)
{
    --skip Test requires DStore is not default engine
}
