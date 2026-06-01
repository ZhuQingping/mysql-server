# include/have_only_innodb_real.inc
#
# Engine support have 4 states, which are 'YES', 'NO', 'DISABLED', 'DEFAULT'.
# In this case Innodb engine should be 'DEFAULT'.
# Skip the test if Innodb is not default engine

if (`SELECT COUNT(*) = 0 FROM information_schema.engines WHERE ENGINE = "Innodb" AND SUPPORT = "DEFAULT"`)
{
    --skip Test requires InnoDB is default engine
}
