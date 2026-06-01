# Get the lines number of the given file. The file path
# is given in the environment variable SEARCH_FILE. 
#
# Usage:
#
# let SEARCH_FILE=$MYSQLTEST_VARDIR/log/mysqld.1.err;
# --source include/get_file_line_number.inc

--perl
  my $file_path = $ENV{SEARCH_FILE} or die "Input file path is not set";
  my $line_number = `wc -l < $file_path`;
  open(FILE, ">", "$ENV{MYSQL_TMP_DIR}/line_number.inc") or die;
  print FILE "let \$LINE_NUMBER= $line_number;\n";
EOF
--source $MYSQL_TMP_DIR/line_number.inc
--remove_file $MYSQL_TMP_DIR/line_number.inc
