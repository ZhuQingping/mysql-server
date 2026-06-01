scc_conf_dir=$1
scc_conf=${scc_conf_dir}/scc.conf
scc_log_conf=${scc_conf_dir}/scc_logger.conf

echo "[CRYPTO]" > ${scc_conf}
echo "primaryKeyStoreFile=${scc_conf_dir}/primary.ks" >> ${scc_conf}
echo "standbyKeyStoreFile=${scc_conf_dir}/standby.ks" >> ${scc_conf}
echo "backupFolderName=${scc_conf_dir}" >> ${scc_conf}

rpm -qa |grep seccomponent-1.0 > ${scc_conf_dir}/tmp.txt
if [ $? -eq 0 ]; then
  echo "logCfgFile=${scc_log_conf}" >> ${scc_conf}
  echo "logCategory=SCC" >> ${scc_conf}
fi

echo "domainCount=8" >> ${scc_conf}
echo "logFilePath=${scc_conf_dir}/" >> ${scc_conf}
echo "logFileName=scc" >> ${scc_conf}

echo "[global]" > ${scc_log_conf}
echo "file perms=600" >> ${scc_log_conf}
echo "[formats]" >> ${scc_log_conf}
echo "default=\"%%d[%%-5V][%%p:%%t][%%f:%%U:%%L]%%m%%n\"\n" >> ${scc_log_conf}
echo "[rules]" >> ${scc_log_conf}
echo "SCC.INFO    \"${scc_conf_dir}/scc.log\",1MB * 5 ~ \"${scc_conf_dir}/scc.log-%d(%Y%m%d).#2s.log\"; default" >> ${scc_log_conf}