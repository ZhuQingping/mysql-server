cd ~

local_lcov=".lcovrc"

if [ -f "/etc/lcovrc" ]; then
  \cp /etc/lcovrc ${local_lcov}
elif [ -f "/usr/local/etc/lcovrc" ]; then
  \cp /usr/local/etc/lcovrc ${local_lcov}
else
  touch ${local_lcov}
fi
 
function replace_lcovrc()
{
  key="$1"
  value="$2"
  line_count=`grep "^${key}" ${local_lcov}`
  if [ "$line_count" = "" ]; then
    echo "$key = $value" >> ${local_lcov}
  else
    sed -i "s/^$key.*$/$key = $value/g" ${local_lcov}
  fi
}
 
#replace_lcovrc lcov_excl_line 'SAL_*|TAURUS_INFO*|TAURUS_WARN*|TAURUS_ERROR*'
replace_lcovrc geninfo_no_exception_branch 1
replace_lcovrc genhtml_branch_hi_limit 60
replace_lcovrc genhtml_branch_med_limit 30