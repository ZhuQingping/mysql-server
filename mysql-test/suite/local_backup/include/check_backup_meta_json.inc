# ==== Purpose ====
#
# Check backup_meta.json file content.
#
# ==== Usage ====
#
#   [--let $check_json_crc = 1]
#   [--let $check_status = Completed]
#   [--let $check_status = Aborted]
#   [--let $check_status = In_progress]
#   --source suite/local_backup/include/check_backup_meta_json.inc
#
# Parameters:
#
#   $check_status
#     By default, Completed is configured. You can configure Aborted or In_progress
#     by setting this variable.


# Set default value of $check_status Completed.
if (!$check_status)
{
  --let $check_status = Completed
}

# Wait for the backup_meta.json file to be created.
perl;
use Time::HiRes qw(sleep);
my $vardir = $ENV{'MYSQLTEST_VARDIR'};
my $backup_meta_json_path;
my $wait_counter = 300;
my $found = 0;
while ($wait_counter)
{
  $backup_meta_json_path = `ls $vardir/full_local_backup/* -trd | tail -n 1`;
  chomp($backup_meta_json_path);
  if (-e "$backup_meta_json_path/backup_meta.json") {
    $found = 1;
    last;
  }
  sleep(0.1);
  $wait_counter--;
}
if (!$found) {
  die "ERROR: File $backup_meta_json_path/backup_meta.json not generated after 30 seconds!";
}
EOF

--sleep 1

# Check if backup_meta.json file crc is matched.
if ($check_json_crc)
{
  --source suite/local_backup/include/check_backup_meta_json_crc.inc
}

--exec ls $MYSQLTEST_VARDIR/full_local_backup/* -trd | tail -n 1 > $MYSQLTEST_VARDIR/bakup_dir.txt
--exec cp `cat $MYSQLTEST_VARDIR/bakup_dir.txt`/backup_meta.json $MYSQLTEST_VARDIR/
--let SEARCH_FILE = $MYSQLTEST_VARDIR/backup_meta.json

if ($check_status == 'Completed')
{
  --let SEARCH_PATTERN = "nonDstoreRunningStatus": "Completed"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "tablespaceRunningStatus": "Completed"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "walRunningStatus": "Completed"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "controlFileRunningStatus": "Completed"
  --source include/search_pattern.inc
}
if ($check_status == 'Aborted')
{
  --let SEARCH_PATTERN = "timePoint": -1
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "lsnPoint": -1
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "startRecoveryLsn": -1
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = RunningStatus": "Aborted"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = ErrorMessage": "
  --source include/search_pattern.inc
}
if ($check_status == 'In_progress')
{
  --let SEARCH_PATTERN = "timePoint": 0
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "lsnPoint": 0
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "startRecoveryLsn": 0
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "nonDstoreRunningStatus": "In progress"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "tablespaceRunningStatus": "In progress"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "walRunningStatus": "In progress"
  --source include/search_pattern.inc
  --let SEARCH_PATTERN = "controlFileRunningStatus": "In progress"
  --source include/search_pattern.inc
}

# Clean up.
--exec rm -rf $MYSQLTEST_VARDIR/bakup_dir.txt $MYSQLTEST_VARDIR/backup_meta.json
