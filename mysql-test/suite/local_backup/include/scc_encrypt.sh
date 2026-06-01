SCC_CONF=$1
SCC_PLAIN_TEXT=$2
SCC_ENCRYPT_RES_FILE=$3

rpm -qa |grep seccomponent-1.0 > $SCC_ENCRYPT_RES_FILE
if [ $? -eq 0 ]; then
  /usr/local/seccomponent/bin/CryptoAPI -f $SCC_CONF -e $SCC_PLAIN_TEXT > $SCC_ENCRYPT_RES_FILE
else
  echo $SCC_PLAIN_TEXT | /usr/local/seccomponent/bin/CryptoAPI -f $SCC_CONF -e | awk -F':' '{print $2}' > $SCC_ENCRYPT_RES_FILE
fi
