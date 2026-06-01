# Reset test table for dstore online create index test
--disable_query_log
--disable_warnings

DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS countrys;
DROP TABLE IF EXISTS departments;

CREATE TABLE departments (
    department_id VARCHAR(32) PRIMARY KEY,
    department_name VARCHAR(100) NOT NULL
) ENGINE=DSTORE CHARSET=utf8mb4;

INSERT INTO departments VALUES ('D001','ICT'), ('D002','CBU');

CREATE TABLE countrys (
    country_id VARCHAR(32) PRIMARY KEY,
    country_name VARCHAR(100) NOT NULL
) ENGINE=DSTORE CHARSET=utf8mb4;

INSERT INTO countrys VALUES ('C001','China'), ('C002','America');

CREATE TABLE employees (
    employee_id VARCHAR(32) PRIMARY KEY NOT NULL,
	department_id VARCHAR(32) NOT NULL,
	CONSTRAINT fk_group
    FOREIGN KEY (department_id) REFERENCES `departments`(department_id)
    ON DELETE CASCADE
    ON UPDATE CASCADE,
	country_id VARCHAR(32),
	CONSTRAINT fk_country
    FOREIGN KEY (country_id) REFERENCES `countrys`(country_id)
    ON DELETE SET NULL
    ON UPDATE CASCADE,
	boss_id VARCHAR(32),
    FOREIGN KEY (boss_id) 
    REFERENCES employees(employee_id)
    ON DELETE CASCADE
    ON UPDATE CASCADE,
    name VARCHAR(50) NOT NULL,
    email VARCHAR(150),
    age TINYINT UNSIGNED,
    gender ENUM('M', 'F', 'O') DEFAULT 'O',
    status TINYINT DEFAULT 1 COMMENT '0:禁用,1:正常,2:冻结',
    credit_score INT DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    age_group VARCHAR(20) GENERATED ALWAYS AS (
        CASE 
            WHEN age < 18 THEN 'minor'
            WHEN age BETWEEN 18 AND 35 THEN 'youth'
            WHEN age BETWEEN 36 AND 55 THEN 'midlife'
            ELSE 'elderly'
        END
    ) VIRTUAL,
	brief_description VARCHAR(60000) CHARSET latin1,
	j JSON DEFAULT NULL,
    INDEX idx_name (name),
    INDEX idx_status_created (status, created_at)
) ENGINE=DSTORE CHARSET=utf8mb4 TABLESPACE=tb1;

INSERT INTO employees (employee_id, department_id, country_id, boss_id, name, email, age, gender, status, credit_score, created_at, brief_description, j) VALUES
('U001', 'D001', 'C001', NULL, 'zhangsan', 'zhangsan@example.com', 25, 'M', 1, 750, '2023-01-15 13:30:00', repeat('I am zhangsan', 30), '"2019-1-1"'),
('U002', 'D001', 'C001', 'U001', 'lisi', 'lisi@example.com', 30, 'M', 1, 800, '2023-06-15 10:30:00', repeat('A', 60000), '"2019-01-02"'),
('U003', 'D001', 'C002', 'U001', 'wangwu', 'wangwu@example.com', 17, 'F', 1, 650, '2023-07-20 14:45:00', repeat('I am wangwu', 60), CAST(DATE'2019-1-3' AS JSON)),
('U004', 'D002', 'C001', 'U002', 'zhaoliu', 'zhaoliu@example.com', 45, 'M', 0, 550, '2023-08-10 09:15:00', repeat('Make zhaoliu great again!', 60), CAST(DATE'2020-1-5' AS JSON)),
('U005', 'D002', 'C002', 'U002', 'qianqi', 'qianqi@example.com', 60, 'F', 2, 900, '2023-09-05 16:20:00', repeat('waaaaagh!', 5000), CAST(DATE'2020-1-30' AS JSON));

--enable_warnings
--enable_query_log
