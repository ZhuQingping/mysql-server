
###########################################################################

--echo
--echo TINYINT
--echo

CREATE PROCEDURE p1(OUT v TINYINT)
  SET v = 127;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 127;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo SMALLINT
--echo

CREATE PROCEDURE p1(OUT v SMALLINT)
  SET v = 32767;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 32767;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo MEDIUMINT
--echo

CREATE PROCEDURE p1(OUT v MEDIUMINT)
  SET v = 8388607;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 8388607;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo INT
--echo

CREATE PROCEDURE p1(OUT v INT)
  SET v = 2147483647;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 2147483647;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo BIGINT
--echo

CREATE PROCEDURE p1(OUT v BIGINT)
  SET v = 9223372036854775807;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 9223372036854775807;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo BIT(11)
--echo

CREATE PROCEDURE p1(OUT v BIT(11))
  SET v = b'10100100101';

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = b'10100100101';

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################
--echo
--echo YEAR
--echo

CREATE PROCEDURE p1(OUT v YEAR)
  SET v = 2010;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 2010;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo FLOAT(7, 4)
--echo

CREATE PROCEDURE p1(OUT v FLOAT(7, 4))
  SET v = 123.4567;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a - 123.4567 < 0.00001;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo DOUBLE(8, 5)
--echo

CREATE PROCEDURE p1(OUT v DOUBLE(8, 5))
  SET v = 123.45678;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a - 123.45678 < 0.000001;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################

--echo
--echo DECIMAL(9, 6)
--echo

CREATE PROCEDURE p1(OUT v DECIMAL(9, 6))
  SET v = 123.456789;

PREPARE stmt1 FROM 'CALL p1(?)';
EXECUTE stmt1 USING @a;

CREATE TEMPORARY TABLE tmp1 AS SELECT @a AS c1;

SHOW CREATE TABLE tmp1;

SELECT @a, @a = 123.456789;

DROP TEMPORARY TABLE tmp1;
DROP PROCEDURE p1;

###########################################################################
