# Copyright (c) 2022, Huawei and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

" This Perl script is a tool to mitigate one bad effect of
copy-pasting code: it can be used to ensure that the original code and
the copy do not diverge silently. It is automatically run at
compilation time (see topdir/CmakeLists.txt). It scans C++ files in
the sql/ directory, looking for a certain magic pattern which asks to
do a comparison with a canon (a 'known-valid version'). For example,
in sql/sql_optimizer.cc we have:

bool JOIN::shallow_clone(JOIN *orig) {
  // This function is a very simplified, sort of copy constructor for JOIN,
  // for the use of session-plan-cache.
  // If, in the future, new members are added to class JOIN, they probably
  // must be copied here. So:
  // HUAWEI_ASSERT_MATCH_CANON sql_class_JOIN
  tables_list = select_lex->leaf_tables;
  const_table_map = orig->const_table_map;
  where_cond = orig->where_cond;

The magic line here is:
  HUAWEI_ASSERT_MATCH_CANON sql_class_JOIN

When the script sees it, it opens file 'canons/sql_class_JOIN.txt'
which contains a recording of how class JOIN's declaration was at the
last time when it was known to be consistent with the code in
JOIN::shallow_clone().

A few lines before the recording, this TXT file contains three header lines:
  sql/sql_optimizer.h
  ^class JOIN \{$       (*)
  ^\};$                 (**)

which tell the script how to find the _current_ declaration of class
JOIN: in file sql/sql_optimizer.h, between the first line which
matches the Perl regular expression marked (*) and the next line which
matches the regex marked (**). The canon is all that follows the 3
header lines. The script does a diff between the canon and the current
declaration of JOIN, and reports a problem if they differ. If that
happens, it means that the original code has recently been changed,
and the copy may thus need to go through the same change, or at least
this must be considered; then it is up to the developer to carefully
follow instructions in the report. ";

use warnings;
use strict;

# If Text::Diff::diff is missing, we'll use this home-made comparator instead.
# Returns "" if equal, error message if not.
sub diff
{
    my ($arg1, $arg2) = @_; # inputs are references to arrays
    my $err = "<Diff exists but cannot be shown ; please install Perl's Text::Diff and re-run me, to see it>";
    if (@$arg1 != @$arg2) # not same count of lines
    {
        return $err ;
    }
    my $counter;
    foreach my $l1 (@$arg1) {
        if (not ($l1 eq $arg2->[$counter++]))
        {
            return $err;
        }
    }
    return "";
}

BEGIN { # BEGIN runs at compilation time.
    # If "use" succeeds it hides the "diff" routine above.
    eval "use Text::Diff qw(diff)";
}

sub get_relevant_code_part
{
    my ($re_start, $re_end, @lines) = @_;
    my (@part, $l, $found_start, $in_c_comment);
    $found_start = 0;
    $in_c_comment = 0;
    foreach $l (@lines)
    {
        chomp $l;
        if ($l =~ /$re_start/)
        {
            $found_start = 1;
        }
        if ($found_start)
        {
            if ($l =~ /$re_end/)
            {
                push @part, $l."\n";
                last;
            }

            # Remove some uninteresting diffs. Note that we didn't do
            # it above if the line matches the end-pattern. The reason
            # is that we don't want ' }' to match '^}' - don't want to
            # remove spaces; otherwise we would confuse the end of a
            # block with the end of a function, for example.
            $l =~ s|/\*.*\*/||;  # one-line C comment /* */
            $l =~ s/^\s+//;      # blanks at start
            $l =~ s/\s+$//;      # blanks at end
            $l =~ s|//.*$||;     # C++ comment
            if ($l =~ m|^/\*+$|) # start of long C comment /*
            {
                $in_c_comment = 1;
            }
            if ($in_c_comment)
            {
                if ($l =~ m|^\*/$|) # End of long C comment */
                {
                    $in_c_comment = 0;
                }
                next;
            }
            next if (!$l);
            push @part, $l."\n";
        }
    }
    return @part;
}

my $tag = "HUAWEI_ASSERT_MATCH_CANON";
my $problem = 0;

foreach my $fname (glob ("sql/*.cc sql/*.h"))
{
    open(FH, $fname) or die "PROBLEM: cannot find $fname";
    my @lines = <FH>;
    close(FH);
    my $lineno = 0;
    my $l;
    foreach $l (@lines)
    {
        $lineno = $lineno + 1;
        if ($l =~ /$tag/)
        {
            chomp $l;
            my $instruction_for_errmsg = $l;
            $l =~ s/^.*$tag\s+//;
            my $canon_fname = "./canons/$l.txt";
            open (FH, $canon_fname) or die "PROBLEM: cannot find $canon_fname";
            my @canon_lines = <FH>;
            close(FH);
            my $source_fname = shift @canon_lines;
            chomp $source_fname;
            my $re_start = shift @canon_lines;
            chomp $re_start;
            my $re_end = shift @canon_lines;
            chomp $re_end;
            my @canon_part = get_relevant_code_part($re_start, $re_end, @canon_lines);
            die("PROBLEM: cannot find text in $canon_fname") if (!@canon_part);
            open (FH, $source_fname) or die "PROBLEM: cannot find $source_fname";
            my @source_lines = <FH>;
            close(FH);
            my @new_part = get_relevant_code_part($re_start, $re_end, @source_lines);
            die("PROBLEM: cannot find text in $source_fname") if (!@new_part);
            my $diff = diff \@canon_part, \@new_part;
            if ($diff)
            {
                print $diff;
                print "\nPROBLEM with this check \"$instruction_for_errmsg\". Code has deviated from the canon: above is a diff between $canon_fname and $source_fname. Review this diff, and decide if/how this deviation imposes modifications to the code in $fname near line $lineno. Do any such necessary modifications, and only after doing that, update the canon.\n\n";
                $problem = 1;
            }
        }
    }
}

die if ($problem);

