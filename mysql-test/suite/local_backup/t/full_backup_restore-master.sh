#! /bin/bash

# Create dirs for local backup

d="$MYSQLTEST_VARDIR/full_local_backup"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*

d="$MYSQLTEST_VARDIR/local_backup_meta"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*

d="$MYSQLTEST_VARDIR/local_backup_restore_meta"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*

d="$MYSQLTEST_VARDIR/incre_local_backup"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*