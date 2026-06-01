DELIMITER //;

CREATE PROCEDURE IF NOT EXISTS ClearAllData()
BEGIN
    DELETE FROM history;
    DELETE FROM order_line;
    DELETE FROM new_orders;
    DELETE FROM orders;
    DELETE FROM customer;
    DELETE FROM stock;
    DELETE FROM item;
    DELETE FROM district;
    DELETE FROM warehouse;
END //

CREATE PROCEDURE IF NOT EXISTS InsertWarehouseData(IN p_warehouse_count INT)
BEGIN
    DECLARE i INT DEFAULT 1;
    DECLARE base_ytd DECIMAL(12,2) DEFAULT 1000000.00;
    DECLARE v_max_existing_id INT DEFAULT 0;
    
    SELECT COALESCE(MAX(w_id), 0) INTO v_max_existing_id FROM warehouse;
    
    SET i = v_max_existing_id + 1;
    
    WHILE i <= (v_max_existing_id + p_warehouse_count) DO
        INSERT INTO warehouse (w_id, w_name, w_street_1, w_street_2, w_city, w_state, w_zip, w_tax, w_ytd)
        VALUES (
            i,
            CONCAT('WH', i),
            CONCAT(i, ' Main St'),
            'Ste 100',
            CASE 
                WHEN i % 3 = 1 THEN 'New York' 
                WHEN i % 3 = 2 THEN 'Chicago' 
                ELSE 'Los Angeles' 
            END,
            CASE 
                WHEN i % 3 = 1 THEN 'NY' 
                WHEN i % 3 = 2 THEN 'IL' 
                ELSE 'CA' 
            END,
            CONCAT('100', LPAD(i, 6, '0')),
            0.1000 + ((i - 1) * 0.001),
            base_ytd + ((i - 1) * 50000)
        )
        ON DUPLICATE KEY UPDATE 
            w_name = VALUES(w_name),
            w_street_1 = VALUES(w_street_1),
            w_zip = VALUES(w_zip);
        
        SET i = i + 1;
    END WHILE;
END //

CREATE PROCEDURE IF NOT EXISTS InsertDistrictData(IN p_districts_per_warehouse INT)
BEGIN
    DECLARE v_w_id INT DEFAULT 1;
    DECLARE v_d_id INT DEFAULT 1;
    DECLARE v_max_w_id INT;
    DECLARE v_w_city VARCHAR(20);
    DECLARE v_w_state CHAR(2);
    DECLARE v_w_ytd DECIMAL(12,2);
    DECLARE v_district_ytd DECIMAL(12,2);
    DECLARE v_total_district_ytd DECIMAL(12,2) DEFAULT 0;
    DECLARE v_w_count INT;
    
    SELECT COUNT(*) INTO v_w_count FROM warehouse;
    IF v_w_count = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '请先插入仓库数据';
    END IF;
    
    SELECT MAX(w_id) INTO v_max_w_id FROM warehouse;
    
    WHILE v_w_id <= v_max_w_id DO
        SELECT COUNT(*) INTO @warehouse_exists FROM warehouse WHERE w_id = v_w_id;
        IF @warehouse_exists > 0 THEN
            SELECT w_city, w_state, w_ytd INTO v_w_city, v_w_state, v_w_ytd 
            FROM warehouse 
            WHERE w_id = v_w_id;
            
            IF v_w_city IS NOT NULL THEN
                SET v_district_ytd = v_w_ytd / p_districts_per_warehouse;
                SET v_total_district_ytd = 0;
                
                SET v_d_id = 1;
                WHILE v_d_id <= p_districts_per_warehouse DO
                    IF v_d_id = p_districts_per_warehouse THEN
                        SET v_district_ytd = v_w_ytd - v_total_district_ytd;
                    END IF;
                    
                    INSERT INTO district (d_id, d_w_id, d_name, d_street_1, d_street_2, d_city, d_state, d_zip, d_tax, d_ytd, d_next_o_id)
                    VALUES (
                        v_d_id,
                        v_w_id,
                        CONCAT('D', v_w_id, '-', v_d_id),
                        CONCAT(v_w_id, '-', v_d_id, ' Dist St'),
                        'Bldg A',
                        v_w_city,
                        v_w_state,
                        CONCAT('200', LPAD(v_w_id, 4, '0'), LPAD(v_d_id, 2, '0')),
                        0.1200 + ((v_d_id - 1) * 0.001),
                        v_district_ytd,
                        3001
                    )
                    ON DUPLICATE KEY UPDATE 
                        d_name = VALUES(d_name),
                        d_street_1 = VALUES(d_street_1),
                        d_zip = VALUES(d_zip),
                        d_ytd = VALUES(d_ytd);
                    
                    SET v_total_district_ytd = v_total_district_ytd + v_district_ytd;
                    SET v_d_id = v_d_id + 1;
                END WHILE;
            END IF;
        END IF;
        SET v_w_id = v_w_id + 1;
    END WHILE;
END //

CREATE PROCEDURE IF NOT EXISTS InsertItemData(IN p_item_count INT)
BEGIN
    DECLARE i INT DEFAULT 1;
    
    IF p_item_count < 100 OR p_item_count > 100000 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '商品数量必须在100-100000之间';
    END IF;
    
    WHILE i <= p_item_count DO
        INSERT INTO item (i_id, i_name, i_price, i_data)
        VALUES (
            i,
            CONCAT('Item', LPAD(i, 6, '0')),
            10.00 + ((i % 50) * 0.5),
            CASE 
                WHEN i % 10 = 0 THEN CONCAT('Premium Item ', i, ' Data')
                ELSE CONCAT('Standard Item ', i, ' Data')
            END
        );
        SET i = i + 1;
    END WHILE;
END //

CREATE PROCEDURE IF NOT EXISTS InsertStockData()
BEGIN
    DECLARE w_id INT DEFAULT 1;
    DECLARE i_id INT DEFAULT 1;
    DECLARE max_w_id INT;
    DECLARE max_i_id INT;
    DECLARE w_count INT;
    DECLARE i_count INT;
    
    SELECT COUNT(*) INTO w_count FROM warehouse;
    SELECT COUNT(*) INTO i_count FROM item;
    
    IF w_count = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '请先插入仓库数据';
    END IF;
    
    IF i_count = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '请先插入商品数据';
    END IF;
    
    SELECT MAX(w_id) INTO max_w_id FROM warehouse;
    SELECT MAX(i_id) INTO max_i_id FROM item;
    
    WHILE w_id <= max_w_id DO
        SET i_id = 1;
        WHILE i_id <= max_i_id DO
            INSERT INTO stock (s_i_id, s_w_id, s_quantity, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, 
                              s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10, s_ytd, s_order_cnt, s_remote_cnt, s_data)
            VALUES (
                i_id,
                w_id,
                50 + ((i_id + w_id) % 50),
                CONCAT('DIST01_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST02_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST03_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST04_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST05_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST06_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST07_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST08_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST09_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                CONCAT('DIST10_', LPAD(w_id, 2, '0'), LPAD(i_id, 6, '0')),
                0,
                0,
                0,
                CASE 
                    WHEN i_id % 5 = 0 THEN CONCAT('ORIGINAL_STOCK_', i_id)
                    ELSE CONCAT('STANDARD_STOCK_', i_id)
                END
            );
            SET i_id = i_id + 1;
        END WHILE;
        SET w_id = w_id + 1;
    END WHILE;
END //

CREATE PROCEDURE IF NOT EXISTS InsertCustomerData(IN p_customers_per_district INT)
BEGIN
    DECLARE v_w_id INT DEFAULT 1;
    DECLARE v_d_id INT DEFAULT 1;
    DECLARE v_c_id INT DEFAULT 1;
    DECLARE v_max_w_id INT;
    DECLARE v_max_d_id INT;
    DECLARE v_base_date DATE DEFAULT '2020-01-01';
    DECLARE v_w_name VARCHAR(10);
    DECLARE v_d_name VARCHAR(10);
    DECLARE v_w_city VARCHAR(20);
    DECLARE v_w_state CHAR(2);
    DECLARE v_h_data VARCHAR(24);
    DECLARE v_d_ytd DECIMAL(12,2);
    DECLARE v_amount_per_customer DECIMAL(12,2);
    DECLARE v_total_h_amount DECIMAL(12,2);
    DECLARE v_w_count INT;
    DECLARE v_d_count INT;
    
    SELECT COUNT(*) INTO v_w_count FROM warehouse;
    SELECT COUNT(*) INTO v_d_count FROM district;
    
    IF v_w_count = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '请先插入仓库数据';
    END IF;
    
    IF v_d_count = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '请先插入地区数据';
    END IF;
    
    SELECT MAX(w_id) INTO v_max_w_id FROM warehouse;
    SELECT MAX(d_id) INTO v_max_d_id FROM district WHERE d_w_id = 1;
    
    IF p_customers_per_district < 1 OR p_customers_per_district > 3000 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '每个地区的客户数量必须在1-3000之间';
    END IF;
    
    WHILE v_w_id <= v_max_w_id DO
        SELECT w_name, w_city, w_state INTO v_w_name, v_w_city, v_w_state 
        FROM warehouse WHERE w_id = v_w_id;
        
        IF v_w_name IS NOT NULL THEN
            SET v_d_id = 1;
            WHILE v_d_id <= v_max_d_id DO
                SELECT d_name, d_ytd INTO v_d_name, v_d_ytd
                FROM district WHERE d_w_id = v_w_id AND d_id = v_d_id;
                
                IF v_d_name IS NOT NULL THEN
                    SET v_h_data = CONCAT(LEFT(v_w_name, 10), ' ', LEFT(v_d_name, 10));
                    SET v_amount_per_customer = v_d_ytd / p_customers_per_district;
                    SET v_total_h_amount = 0;
                    
                    SET v_c_id = 1;
                    WHILE v_c_id <= p_customers_per_district DO
                        IF v_c_id = p_customers_per_district THEN
                            SET v_amount_per_customer = v_d_ytd - v_total_h_amount;
                        END IF;
                        
                        INSERT INTO customer (c_id, c_d_id, c_w_id, c_first, c_middle, c_last, c_street_1, c_street_2, 
                                            c_city, c_state, c_zip, c_phone, c_since, c_credit, c_credit_lim, 
                                            c_discount, c_balance, c_ytd_payment, c_payment_cnt, c_delivery_cnt, c_data)
                        VALUES (
                            v_c_id, v_d_id, v_w_id,
                            CASE 
                                WHEN p_customers_per_district <= 999 THEN CONCAT('FN', LPAD(v_c_id, 3, '0'))
                                WHEN p_customers_per_district <= 9999 THEN CONCAT('F', LPAD(v_c_id, 4, '0'))
                                ELSE CONCAT('F', v_c_id)
                            END,
                            'OE',
                            CASE 
                                WHEN p_customers_per_district <= 999 THEN CONCAT('LN', LPAD(v_c_id, 3, '0'))
                                WHEN p_customers_per_district <= 9999 THEN CONCAT('L', LPAD(v_c_id, 4, '0'))
                                ELSE CONCAT('L', v_c_id)
                            END,
                            CONCAT(v_c_id, ' St'),
                            CONCAT('Apt ', (v_c_id % 100)),
                            v_w_city,
                            v_w_state,
                            CASE 
                                WHEN v_w_id <= 9 AND v_d_id <= 9 AND v_c_id <= 999 THEN 
                                    CONCAT('30', v_w_id, v_d_id, LPAD(v_c_id, 3, '0'))
                                WHEN v_w_id <= 99 AND v_d_id <= 9 AND v_c_id <= 99 THEN 
                                    CONCAT('3', LPAD(v_w_id, 2, '0'), v_d_id, LPAD(v_c_id, 2, '0'))
                                ELSE 
                                    CONCAT('3', LPAD(v_w_id, 2, '0'), LPAD(v_d_id, 2, '0'), LPAD(v_c_id % 100, 2, '0'))
                            END,
                            CASE 
                                WHEN v_w_id <= 9 AND v_d_id <= 9 AND v_c_id <= 9999 THEN 
                                    CONCAT('555-', v_w_id, v_d_id, LPAD(v_c_id, 4, '0'))
                                ELSE 
                                    CONCAT('555-', LPAD(v_w_id, 2, '0'), LPAD(v_d_id, 2, '0'), LPAD(v_c_id % 1000, 3, '0'))
                            END,
                            DATE_ADD(v_base_date, INTERVAL ((v_w_id * 100 + v_d_id * 10 + v_c_id) % 1000) DAY),
                            CASE 
                                WHEN (v_c_id % 10) = 0 THEN 'BC'
                                ELSE 'GC'
                            END,
                            50000.00,
                            CASE 
                                WHEN (v_c_id % 2) = 0 THEN 0.10
                                WHEN (v_c_id % 5) = 0 THEN 0.15
                                ELSE 0.05
                            END,
                            100.00 + ((v_c_id % 20) * 50),
                            500.00 + ((v_c_id % 10) * 100),
                            5 + (v_c_id % 10),
                            3 + (v_c_id % 8),
                            CONCAT('Cust', v_c_id, ' D', v_d_id, ' W', v_w_id)
                        );
                        
                        INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
                        VALUES (
                            v_c_id, v_d_id, v_w_id, v_d_id, v_w_id,
                            DATE_ADD(v_base_date, INTERVAL ((v_w_id * 100 + v_d_id * 10 + v_c_id) % 1000) DAY),
                            v_amount_per_customer,
                            v_h_data
                        );
                        
                        SET v_total_h_amount = v_total_h_amount + v_amount_per_customer;
                        SET v_c_id = v_c_id + 1;
                    END WHILE;
                END IF;
                SET v_d_id = v_d_id + 1;
            END WHILE;
        END IF;
        SET v_w_id = v_w_id + 1;
    END WHILE;
END //

CREATE PROCEDURE IF NOT EXISTS LoadAllTPCCData(
    IN p_warehouse_count INT,
    IN p_districts_per_warehouse INT,
    IN p_customers_per_district INT,
    IN p_item_count INT
)
BEGIN
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        ROLLBACK;
        RESIGNAL;
    END;
    
    START TRANSACTION;
    
    CALL ClearAllData();
    CALL InsertWarehouseData(p_warehouse_count);
    CALL InsertDistrictData(p_districts_per_warehouse);
    CALL InsertItemData(p_item_count);
    CALL InsertStockData();
    CALL InsertCustomerData(p_customers_per_district);
    
    COMMIT;
END //

DELIMITER ;//