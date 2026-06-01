
--let $ALGO= INPLACE
--let $ALTER_TYPE = AUTO_INCREMENT
--source suite/ddl_timeout/include/table_option_low.inc


--let $ALGO= INPLACE
--let $ALTER_TYPE = CHARACTER_SET
--source suite/ddl_timeout/include/table_option_low.inc


--let $ALTER_TYPE = CHECKSUM
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALTER_TYPE = COLLATE
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALTER_TYPE = COMMENT
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALTER_TYPE = COMPRESSION
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALTER_TYPE = CONNECTION
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALTER_TYPE = DATA_DIRECTORY
--source suite/ddl_timeout/include/table_option_low.inc

# Currently MySQL does not support using COPY algorithm to change table 
# encryption. We disable test for encrypting a table. Taurus is success
# because it ignores the ENCRYPTION=Y options by default.
#--let $ALTER_TYPE = ENCRYPTION
#--source suite/ddl_timeout/include/table_option_low.inc

--let $ALGO= INPLACE
--let $ALTER_TYPE = ENGINE
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALGO= INPLACE
--let $ALTER_TYPE = ROW_FORMAT
--source suite/ddl_timeout/include/table_option_low.inc


--let $ALTER_TYPE = STATS_AUTO_RECALC
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALGO= INPLACE
--let $ALTER_TYPE = STATS_PERSISTENT
--source suite/ddl_timeout/include/table_option_low.inc


--let $ALTER_TYPE = STATS_SAMPLE_PAGES
--source suite/ddl_timeout/include/table_option_low.inc

--let $ALTER_TYPE = PASSWORD
--source suite/ddl_timeout/include/table_option_low.inc
