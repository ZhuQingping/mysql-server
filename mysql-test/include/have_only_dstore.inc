# include/have_only_dstore.inc
#
# Engine support have 4 states, which are 'YES', 'NO', 'DISABLED', 'DEFAULT'.
# In this case DStore engine should be 'DEFAULT'.
# Skip the test if DStore is not default engine

if (`SELECT COUNT(*) = 0 FROM information_schema.engines WHERE ENGINE = "Dstore" AND SUPPORT = "DEFAULT"`)
{
    --skip Test requires Dstore is default engine
}
