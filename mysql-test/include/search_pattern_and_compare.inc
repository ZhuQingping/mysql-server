# Purpose:
#    Extract numbers following a specified pattern in a file and check if they are strictly increasing.
#
#    The environment variables SEARCH_FILE and SEARCH_PATTERN must be set
#    before sourcing this routine.

perl;
    use strict;
    my $search_file =           $ENV{'SEARCH_FILE'}           or die "SEARCH_FILE not set";
    my $search_pattern =        $ENV{'SEARCH_PATTERN'}        or die "SEARCH_PATTERN not set";
    my $search_echo =           $ENV{'SEARCH_ECHO'}     || "NONE";
    my $ignore_pattern =        $ENV{'IGNORE_PATTERN'}  || "";
    my $search_echo_ctx =       $ENV{'SEARCH_ECHO_CTX'} || 0;
    my $search_start_line =     $ENV{'SEARCH_START_LINE'} || 0;

    open(FILE, "< $search_file") or die("Unable to open '$search_file': $!\n");

    my @extracted_numbers;

    # We include the current line and 'n' previous lines in its context
    # as a FIFO queue i.e., oldest entries in the queue are removed when
    # new lines are read from the file.
    my @context = (undef) x ($search_echo_ctx + 1);

    my $line;
    my $flag = 0;
    my $output = "";
    my $line_no = 0;

    # Read the file line by line
    while ($line = <FILE>)
    {
        my $cur_lineno = $.;
        my $file_pos = tell(FILE);
        $line_no += 1;

        if ($line =~ /^CURRENT_TEST: /)
        {
            $output = "";
            $flag = 0;
            @context = (undef) x ($search_echo_ctx + 1);
        }
		if ($line_no - 1 < $search_start_line)
		{
		    next;
		}

        # Context stored as a sliding window of size 'n' lines
        shift @context;
        push  @context, $line;

        # Skip specific lines
        if ($line =~ /Check dstore datadir no exists, startup fail/ ||
            $line =~ /boot dstore failed, ret=-1/ ||
            $line =~ /Plugin 'Dstore' init function returned error./ ||
            $line =~ /Plugin 'Dstore' registration as a STORAGE ENGINE failed./)
        {
            next;
        }

        if ($line =~ /$search_pattern/)
        {
            if (!(length $ignore_pattern) or !($line =~ /$ignore_pattern/))
            {
                # Line matches the specified pattern
                push @extracted_numbers, $1;
                $flag = 1;
            }
        }
    }

    my $is_increasing = 1;
    
    if ($flag == 1)
    {
        if (@extracted_numbers < 2) {
            $is_increasing = 0;
            print "Not enough numbers to compare (need at least 2, found " . scalar(@extracted_numbers) . ")\n";
        } else {
            for (my $i = 1; $i < @extracted_numbers; $i++) {
                if ($extracted_numbers[$i] <= $extracted_numbers[$i-1]) {
                    $is_increasing = 0;
                    last;
                }
            }
        }
    } else {
        $is_increasing = 0;
        print "No matching lines found\n";
    }

    # Output result
    if ($is_increasing) {
        print "Pattern \"$search_pattern\" value increasing true\n";
    } else {
        print "Pattern \"$search_pattern\" value increasing false\n";
    }

    close(FILE);

EOF

# Setting the env vars to default value
let SEARCH_ECHO=NONE;
let IGNORE_PATTERN=;
let SEARCH_ECHO_CTX=0;