drop trigger if exists mysql.block_user_insert;
drop trigger if exists mysql.block_user_update;
drop trigger if exists mysql.block_user_delete;
drop trigger if exists mysql.block_sqlfilter_insert;
drop trigger if exists mysql.block_sqlfilter_update;
drop trigger if exists mysql.block_sqlfilter_delete;
drop trigger if exists mysql.block_outline_insert;
drop trigger if exists mysql.block_outline_update;
drop trigger if exists mysql.block_outline_delete;

SET @old_rds_permission_control = @@global.rds_permission_control,@@global.rds_permission_control=OFF;
INSERT IGNORE INTO mysql.user VALUES ('localhost','rdsAdmin','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','','','','',0,0,0,0,'caching_sha2_password','','N',now(),NULL,'N','Y','Y',NULL,NULL,NULL,NULL);
FLUSH PRIVILEGES;
GRANT ALL ON *.* TO 'rdsAdmin'@'localhost' WITH GRANT OPTION;
SET @@global.rds_permission_control = @old_rds_permission_control;
SET NAMES utf8;

DELIMITER $$
CREATE DEFINER='rdsAdmin'@'localhost' TRIGGER mysql.block_user_insert
BEFORE INSERT ON mysql.user
FOR EACH ROW
BEGIN
    DECLARE message VARCHAR(255);
    DECLARE is_reserved_user INT;
    DECLARE permission_control VARCHAR(255);
    DECLARE allow_empty_users varchar(3);

    SET message = CONCAT('CANNOT CREATE ', new.User, ' USER');

    SELECT COUNT(*) INTO is_reserved_user
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_RESERVED_USERS'
      AND CONCAT(',', VARIABLE_VALUE, ',') LIKE CONCAT('%,', new.User, ',%')
      AND CONCAT('%,', new.User, ',%') != '%,,%'
      AND LOCATE(',', new.User) = 0;

    SELECT VARIABLE_VALUE INTO permission_control
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_PERMISSION_CONTROL';

    IF permission_control = 'ON' THEN
        IF is_reserved_user != 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message;
        ELSEIF new.super_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'SUPER PRIVILEGE CANNOT BE GRANTED OR MAINTAINED';
        ELSEIF new.shutdown_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'SHUTDOWN PRIVILEGE CANNOT BE GRANTED OR MAINTAINED';
        ELSEIF new.file_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'FILE PRIVILEGE CANNOT BE GRANTED OR MAINTAINED';
        ELSEIF new.create_tablespace_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'CREATE TABLESPACE PRIVILEGE CANNOT BE GRANTED OR MAINTAINED';
        ELSEIF new.authentication_string = '' OR new.authentication_string IS NULL THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'CANNOT CREATE USER WITH NULL PASSWORD';
        END IF;
    END IF;

    SELECT VARIABLE_VALUE INTO allow_empty_users
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_ALLOW_EMPTY_USERS';
    IF allow_empty_users = 'OFF' AND new.User = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'CANNOT CREATE AN EMPTY USER.';
    END IF;
END$$

CREATE DEFINER='rdsAdmin'@'localhost' TRIGGER mysql.block_user_update
BEFORE UPDATE ON mysql.user
FOR EACH ROW
BEGIN
    DECLARE message VARCHAR(255);
    DECLARE is_reserved_user INT;
    DECLARE permission_control VARCHAR(255);
    DECLARE allow_empty_users varchar(3);

    SET message = CONCAT('CANNOT UPDATE ', old.User, ' USER');

    SELECT COUNT(*) INTO is_reserved_user
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_RESERVED_USERS'
      AND CONCAT(',', VARIABLE_VALUE, ',') LIKE CONCAT('%,', old.User, ',%')
      AND CONCAT('%,', old.User, ',%') != '%,,%'
      AND LOCATE(',', old.User) = 0;

    SELECT VARIABLE_VALUE INTO permission_control
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_PERMISSION_CONTROL';

    IF permission_control = 'ON' THEN
        IF is_reserved_user != 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message;
        END IF;

        IF old.super_priv = 'N' AND new.super_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'SUPER PRIVILEGE CANNOT BE GRANTED';
        ELSEIF old.shutdown_priv = 'N' AND new.shutdown_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'SHUTDOWN PRIVILEGE CANNOT BE GRANTED';
        ELSEIF old.file_priv = 'N' AND new.file_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'FILE PRIVILEGE CANNOT BE GRANTED';
        ELSEIF old.create_tablespace_priv = 'N' AND new.create_tablespace_priv = 'Y' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'CREATE TABLESPACE PRIVILEGE CANNOT BE GRANTED';
        END IF;
    END IF;

    SET message = CONCAT('CANNOT UPDATE ', old.User, ' USER TO RESERVED USER');

    SELECT COUNT(*) INTO is_reserved_user
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_RESERVED_USERS'
      AND CONCAT(',', VARIABLE_VALUE, ',') LIKE CONCAT('%,', new.User, ',%')
      AND CONCAT('%,', new.User, ',%') != '%,,%'
      AND LOCATE(',', new.User) = 0;

    IF permission_control = 'ON' THEN
        IF is_reserved_user != 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message;
        ELSEIF new.authentication_string = "" OR new.authentication_string IS NULL THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'CANNOT UPDATE USER WITH NULL PASSWORD';
        END IF;
    END IF;

    SELECT VARIABLE_VALUE INTO allow_empty_users
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_ALLOW_EMPTY_USERS';
    IF allow_empty_users = 'OFF' AND new.User = '' THEN
        SET message = concat( 'CANNOT UPDATE ', old.User, ' USER TO AN EMPTY USER.' );
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message;
    END IF;
END$$

CREATE DEFINER='rdsAdmin'@'localhost' TRIGGER mysql.block_user_delete
BEFORE DELETE ON mysql.user
FOR EACH ROW
BEGIN
    DECLARE message VARCHAR(255);
    DECLARE is_reserved_user INT;
    DECLARE permission_control VARCHAR(255);

    SET message = CONCAT('CANNOT DROP ', old.User, ' USER');

    SELECT COUNT(*) INTO is_reserved_user
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_RESERVED_USERS'
      AND CONCAT(',', VARIABLE_VALUE, ',') LIKE CONCAT('%,', old.User, ',%')
      AND CONCAT('%,', old.User, ',%') != '%,,%'
      AND LOCATE(',', old.User) = 0;

    SELECT VARIABLE_VALUE INTO permission_control
    FROM performance_schema.global_variables
    WHERE VARIABLE_NAME = 'RDS_PERMISSION_CONTROL';

    IF permission_control = 'ON' THEN
        IF is_reserved_user != 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message;
        END IF;
    END IF;
END$$

create definer='rdsAdmin'@'localhost' trigger mysql.block_sqlfilter_insert
	before insert on mysql.rds_sql_filter_rules for each row
BEGIN
	DECLARE
		user_name varchar(255);
	DECLARE
		mecessage varchar(255);
	select
		user() into user_name;
	IF
		user_name != 'rdsAdmin@localhost'
	THEN
		set mecessage = concat('Access denied for user ''',user_name,''' to table ''mysql.rds_sql_filter_rules''.');
		SIGNAL SQLSTATE '45000'
		SET MESSAGE_TEXT = mecessage;
	END IF;
END$$
 
create definer='rdsAdmin'@'localhost' trigger mysql.block_sqlfilter_update
	before update on mysql.rds_sql_filter_rules for each row
BEGIN
	DECLARE
		user_name varchar(255);
	DECLARE
		mecessage varchar(255);
	select user() into user_name;
	IF user_name != 'rdsAdmin@localhost'
	THEN
		set mecessage = concat('Access denied for user ''',user_name,''' to table ''mysql.rds_sql_filter_rules''.');
		SIGNAL SQLSTATE '45000'
		SET MESSAGE_TEXT = mecessage;
	END IF;
END$$
 
create definer='rdsAdmin'@'localhost' trigger mysql.block_sqlfilter_delete
	before delete on mysql.rds_sql_filter_rules for each row
BEGIN
	DECLARE
		user_name varchar(255);
	DECLARE
		mecessage varchar(255);
	select
		user() into user_name;
	IF
		user_name != 'rdsAdmin@localhost'
	THEN
		set mecessage = concat('Access denied for user ''',user_name,''' to table ''mysql.rds_sql_filter_rules''.');
		SIGNAL SQLSTATE '45000'
		SET MESSAGE_TEXT = mecessage;
	END IF;
END$$

CREATE DEFINER='rdsAdmin'@'localhost' TRIGGER mysql.block_outline_insert 
BEFORE INSERT ON mysql.outline FOR EACH ROW 
BEGIN 
    DECLARE user_name varchar(255);
    DECLARE message_text varchar(255);
    SELECT USER() INTO user_name;
    IF user_name != 'rdsAdmin@localhost' THEN 
        SET message_text = CONCAT('Access denied for user ''', user_name, ''' to table ''mysql.outline''.');
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message_text;
    END IF;
END$$

CREATE DEFINER='rdsAdmin'@'localhost' TRIGGER mysql.block_outline_update 
BEFORE UPDATE ON mysql.outline FOR EACH ROW 
BEGIN 
    DECLARE user_name varchar(255);
    DECLARE message_text varchar(255);
    SELECT USER() INTO user_name;
    IF user_name != 'rdsAdmin@localhost' THEN 
        SET message_text = CONCAT('Access denied for user ''', user_name, ''' to table ''mysql.outline''.');
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message_text;
    END IF;
END$$

CREATE DEFINER='rdsAdmin'@'localhost' TRIGGER mysql.block_outline_delete 
BEFORE DELETE ON mysql.outline FOR EACH ROW 
BEGIN 
    DECLARE user_name varchar(255);
    DECLARE message_text varchar(255);
    SELECT USER() INTO user_name;
    IF user_name != 'rdsAdmin@localhost' THEN 
        SET message_text = CONCAT('Access denied for user ''', user_name, ''' to table ''mysql.outline''.');
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = message_text;
    END IF;
END$$

DELIMITER ;
