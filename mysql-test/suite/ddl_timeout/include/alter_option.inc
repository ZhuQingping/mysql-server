--let $ALTER_TYPE = ADD_COLUMN_AFTER
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_VIRTUAL_COLUMN
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_COLUMN
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = CREATE_INDEX
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_UNIQUE
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_FULLTEXT
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = SET_DEFAULT
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_DEFAULT
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ALTER_COLUMN
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = CHANGE_COLUMN
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = MODIFY_COLUMN
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = SET_NULL
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = CHARSET
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = CONVERT_TO
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_VIRTUAL_COLUMN
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_COLUMN
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_INDEX_ALTER
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_INDEX
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = FORCE
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = RENAME_COLUMN
--let $ALGO=INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = RENAME_INDEX
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = RENAME_TABLE
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ALTER_INDEX
--let $ALGO=INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = VIRTUAL_INDEX
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_SPATIAL
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_PRIMARY
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = ADD_VIRTUAL_COLUMN
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_VIRTUAL_COLUMN
--let $ALGO= INSTANT
--source suite/ddl_timeout/include/alter_option_low.inc
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = DROP_STORED_COLUMN
--let $ALGO= INPLACE
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = VALIDATION
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc


--let $ALTER_TYPE = WITHOUT_VALIDATION
--let $ALGO= COPY
--source suite/ddl_timeout/include/alter_option_low.inc
