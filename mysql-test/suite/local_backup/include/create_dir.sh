#! /bin/bash

# Create dirs for local backup

d="$MYSQLTEST_VARDIR/backup_output"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*

d="$MYSQLTEST_VARDIR/full_local_backup"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*
 
d="$MYSQLTEST_VARDIR/local_backup_meta"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*
 
d="$MYSQLTEST_VARDIR/local_backup_restore_meta"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*
 
d="$MYSQLTEST_VARDIR/wal_archive"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*

d="$MYSQLTEST_VARDIR/obs"
test -d "$d" || mkdir "$d"
rm -rf "$d"/*

d="$MYSQL_BASEDIR/scripts/local_backup"
if test -d "$d"; then
  cp $MYSQL_BASEDIR/scripts/local_backup/backup_config.py $MYSQLTEST_VARDIR/
  cp $MYSQL_BASEDIR/scripts/local_backup/backup_ctrl.py $MYSQLTEST_VARDIR/
  cp $MYSQL_BASEDIR/scripts/local_backup/backup_download.config $MYSQLTEST_VARDIR/
else
  cp $MYSQL_TEST_DIR/suite/local_backup/include/backup_config.py $MYSQLTEST_VARDIR/
  cp $MYSQL_TEST_DIR/suite/local_backup/include/backup_ctrl.py $MYSQLTEST_VARDIR/
  cp $MYSQL_TEST_DIR/suite/local_backup/include/backup_download.config $MYSQLTEST_VARDIR/
fi
