# Echo the contents of the audit log (in CSV2 format)
# log_file is the name of the log file

--move_file $log_file $log_file.copy
set global audit_log_flush= ON;
let log_file=$log_file;
perl;
  print "===================================================================\n";
  open my $file, $ENV{'log_file'} . '.copy' or die "Can not open log: $!";
  while ($line = <$file>) {
    if ($line =~ /SET NAMES/) {
      # change_user does automatic reconnect and messing up 'SET NAMES' around
      next;
    }
    if ($line =~ /Threads_connected/ || $line =~ /SELECT \d <= \d/
       || /SELECT.*FROM.*INFORMATION_SCHEMA.PLUGINS/) {
      # part of wait_until_count_sessions.inc and wait_condition.inc scripts
      next;
    }
    if ($line =~ /^"Audit"/) {
      # skip opening log record and disconnect record
      next;
    }
    if ($line =~ /SELECT count\(\*\)=1 FROM t WHERE a=7/) {
      # skip checking for event shot
      next;
    }
    if ($line =~ /INSERT INTO t VALUES \(7\)/) {
      # don't show the last statement of the event
      next;
    }
    if ($line =~ /SELECT 600 \* 10/) {
      # skip checking for event shot
      next;
    }
    $line =~ s/"[0-9]+","[0-9]+","[0-9]+","([a-zA-Z_ ]*)","[0-9_ :A-Z-]*"/"<ID>","$1","<DATETIME>"/;
    $line =~ s/"[0-9]+", "[0-9]+", "[0-9]+", "([a-zA-Z_ ]*)", "[0-9_ :A-Z-]*"/"<ID>","$1","<DATETIME>"/;
    if ($line =~ /SET GLOBAL .*_accounts/ || $line =~ /SET GLOBAL audit_log_anonymized_ip/) {
      print "*************************************************************\n";
    }
    print "$line";
  };
  close $file;
  print "===================================================================\n";
EOF
--remove_file $log_file.copy
