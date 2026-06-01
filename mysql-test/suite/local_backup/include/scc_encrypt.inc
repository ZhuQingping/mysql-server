# Use scc to encrypt $SCC_PLAIN_TEXT and save in $SCC_ENCRYPT_RES_FILE

--exec sh $MYSQL_TEST_DIR/suite/local_backup/include/scc_encrypt.sh $SCC_CONF $SCC_PLAIN_TEXT $SCC_ENCRYPT_RES_FILE