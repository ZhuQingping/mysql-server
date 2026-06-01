# include/have_dstore.inc
#
# Engine support have 4 states, which are 'YES', 'NO', 'DISABLED', 'DEFAULT'.
# In this case DStore engine should be 'YES' or 'DEFAULT'.
# Skip the test if DStore is not supported

if (`SELECT COUNT(*) = 0 FROM information_schema.engines WHERE ENGINE = "Dstore" AND SUPPORT IN ('YES', 'DEFAULT')`)
{
    --skip Test requires DStore is supported
}
