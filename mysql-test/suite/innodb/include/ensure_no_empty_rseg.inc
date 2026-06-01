# Make sure all undo rseg is not empty by executing a loop of insert

CREATE TABLE t_make_sure_no_empty_rseg (a INT NOT NULL) ENGINE = InnoDB;

--let autocommit_saved = `SELECT @@AUTOCOMMIT`
SET AUTOCOMMIT = 1;

let $loop_cnt= 256;
while ($loop_cnt)
{
  INSERT INTO t_make_sure_no_empty_rseg (a) VALUES (100);
  dec $loop_cnt;
}

DROP TABLE t_make_sure_no_empty_rseg;
--eval SET AUTOCOMMIT = $autocommit_saved;
