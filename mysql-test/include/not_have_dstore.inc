# include/not_have_dstore.inc
#
# Engine support have 4 states, which are 'YES', 'NO', 'DISABLED', 'DEFAULT'.
# In this case DStore engine should be 'NO' or 'DISABLED'.
# Skip the test if DStore is supported

if (`SELECT COUNT(*) = 0 FROM information_schema.engines WHERE ENGINE = "Dstore" AND SUPPORT IN ('NO', 'DISABLED')`)
{
    --skip Test requires DStore is not supported
}
