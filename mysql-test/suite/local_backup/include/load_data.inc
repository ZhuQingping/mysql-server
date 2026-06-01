#
# create procedure for loading data
#
--disable_warnings
DROP PROCEDURE IF EXISTS load_data;
--enable_warnings

--disable_query_log
delimiter //;
CREATE PROCEDURE load_data(tb_name varchar(100),col_name varchar(10),id_count INT)
BEGIN
  SET @id = 1;
  SET @lsql = '';
  SET @var_v = REPEAT('a',100);
  WHILE @id <= id_count DO
  SET @lsql = CONCAT('(\'',@var_v,'\'),',@lsql);
  IF @id % 1000 = 0 THEN
  SET @lsql=left(@lsql,CHAR_LENGTH(@lsql)-1);
  SET @lsql = CONCAT('INSERT INTO ',tb_name,'(',col_name,') VALUES',@lsql,';');
  PREPARE st_insert FROM @lsql;
  EXECUTE st_insert ;
  COMMIT;
  SET @lsql = '';
  DEALLOCATE PREPARE st_insert;
  END IF;
  SET @id = @id + 1;
  END WHILE;
  END
//
delimiter ;//
--enable_query_log
