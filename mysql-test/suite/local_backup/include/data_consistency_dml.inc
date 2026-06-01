#
# Execute DML
#

DELIMITER //;

CREATE PROCEDURE IF NOT EXISTS ExecuteTPCCTransaction(
    IN p_transaction_type VARCHAR(20),
    IN p_w_id INT,
    IN p_d_id INT,
    IN p_c_id INT,
    IN p_o_ol_cnt INT,
    IN p_o_all_local INT,
    IN p_amount DECIMAL(6,2),
    IN p_o_carrier_id INT,
    IN p_threshold INT
)
BEGIN
    DECLARE base_date TIMESTAMP DEFAULT '2020-01-01 00:00:00';
    DECLARE v_error_msg VARCHAR(500);
    
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
        SELECT CONCAT('事务执行失败: ', v_error_msg) AS error_message;
        ROLLBACK;
    END;
    
    START TRANSACTION;
    
    IF p_transaction_type = 'NEW_ORDER' THEN
        BEGIN
            DECLARE v_o_id INT;
            DECLARE v_o_entry_d TIMESTAMP;
            DECLARE v_total_amount DECIMAL(8,2) DEFAULT 0;
            DECLARE i INT DEFAULT 1;
            DECLARE v_ol_supply_w_id INT;
            DECLARE v_ol_i_id INT;
            DECLARE v_ol_quantity INT;
            DECLARE v_i_price DECIMAL(5,2);
            DECLARE v_s_quantity INT;
            DECLARE v_ol_amount DECIMAL(6,2);
            DECLARE v_remote_cnt INT DEFAULT 0;
            DECLARE v_c_discount DECIMAL(4,4);
            DECLARE v_c_last VARCHAR(16);
            DECLARE v_c_credit CHAR(2);
            DECLARE v_w_tax DECIMAL(4,4);
            DECLARE v_d_tax DECIMAL(4,4);
            DECLARE v_dist_info CHAR(24);
            
            IF p_w_id IS NULL OR p_d_id IS NULL OR p_c_id IS NULL OR p_o_ol_cnt IS NULL OR p_o_all_local IS NULL THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '新订单事务缺少必要参数';
            END IF;
            
            IF p_o_ol_cnt < 5 OR p_o_ol_cnt > 15 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '订单行数必须在5-15之间';
            END IF;
            
            SELECT d_next_o_id, d_tax INTO v_o_id, v_d_tax
            FROM district 
            WHERE d_w_id = p_w_id AND d_id = p_d_id
            FOR UPDATE;
            
            SELECT c_discount, c_last, c_credit INTO v_c_discount, v_c_last, v_c_credit
            FROM customer
            WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;
            
            SELECT w_tax INTO v_w_tax FROM warehouse WHERE w_id = p_w_id;
            
            SET v_o_entry_d = DATE_ADD(base_date, INTERVAL ((p_w_id * 1000 + p_d_id * 100 + v_o_id) % 10000) HOUR);
            
            INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local, o_amount)
            VALUES (v_o_id, p_d_id, p_w_id, p_c_id, v_o_entry_d, NULL, p_o_ol_cnt, p_o_all_local, 0);
            
            INSERT INTO new_orders (no_o_id, no_d_id, no_w_id)
            VALUES (v_o_id, p_d_id, p_w_id);
            
            WHILE i <= p_o_ol_cnt DO
                SET v_ol_supply_w_id = CASE 
                    WHEN i = p_o_ol_cnt AND p_o_all_local = 0 THEN 
                        CASE WHEN p_w_id = 1 THEN 2 ELSE 1 END
                    ELSE p_w_id 
                END;
                
                SET v_ol_i_id = 1 + (((p_w_id + p_d_id + p_c_id + i) * 7) % (SELECT COUNT(*) FROM item));
                SET v_ol_quantity = 1 + ((p_w_id + p_d_id + p_c_id + i) % 9);
                
                SELECT i_price INTO v_i_price FROM item WHERE i_id = v_ol_i_id;
                
                SELECT s_quantity INTO v_s_quantity 
                FROM stock 
                WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id
                FOR UPDATE;
                
                IF v_s_quantity >= v_ol_quantity + 10 THEN
                    UPDATE stock 
                    SET s_quantity = s_quantity - v_ol_quantity,
                        s_ytd = s_ytd + v_ol_quantity,
                        s_order_cnt = s_order_cnt + 1
                    WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                ELSE
                    UPDATE stock 
                    SET s_quantity = s_quantity - v_ol_quantity + 91,
                        s_ytd = s_ytd + v_ol_quantity,
                        s_order_cnt = s_order_cnt + 1
                    WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                END IF;
                
                IF v_ol_supply_w_id != p_w_id THEN
                    UPDATE stock 
                    SET s_remote_cnt = s_remote_cnt + 1
                    WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    SET v_remote_cnt = v_remote_cnt + 1;
                END IF;
                
                SET v_ol_amount = v_ol_quantity * v_i_price;
                SET v_total_amount = v_total_amount + v_ol_amount;
                
                CASE p_d_id
                    WHEN 1 THEN SELECT s_dist_01 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 2 THEN SELECT s_dist_02 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 3 THEN SELECT s_dist_03 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 4 THEN SELECT s_dist_04 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 5 THEN SELECT s_dist_05 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 6 THEN SELECT s_dist_06 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 7 THEN SELECT s_dist_07 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 8 THEN SELECT s_dist_08 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 9 THEN SELECT s_dist_09 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                    WHEN 10 THEN SELECT s_dist_10 INTO v_dist_info FROM stock WHERE s_w_id = v_ol_supply_w_id AND s_i_id = v_ol_i_id;
                END CASE;
                
                INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, 
                                       ol_delivery_d, ol_quantity, ol_amount, ol_dist_info)
                VALUES (v_o_id, p_d_id, p_w_id, i, v_ol_i_id, v_ol_supply_w_id, 
                        NULL, v_ol_quantity, v_ol_amount, v_dist_info);
                
                SET i = i + 1;
            END WHILE;
            
            SET v_total_amount = v_total_amount * (1 - v_c_discount) * (1 + v_w_tax + v_d_tax);
            
            UPDATE orders SET o_amount = v_total_amount 
            WHERE o_id = v_o_id AND o_d_id = p_d_id AND o_w_id = p_w_id;
            
            UPDATE district SET d_next_o_id = d_next_o_id + 1 
            WHERE d_w_id = p_w_id AND d_id = p_d_id;
            
            SELECT 
                'NEW_ORDER' AS transaction_type,
                v_o_id AS order_id,
                v_total_amount AS total_amount,
                v_o_entry_d AS entry_date,
                v_c_last AS customer_last_name,
                v_c_credit AS customer_credit,
                v_c_discount AS customer_discount,
                v_remote_cnt AS remote_count;
        END;

    ELSEIF p_transaction_type = 'PAYMENT' THEN
        BEGIN
            DECLARE v_w_name VARCHAR(10);
            DECLARE v_d_name VARCHAR(10);
            DECLARE v_c_first VARCHAR(16);
            DECLARE v_c_middle CHAR(2);
            DECLARE v_c_last VARCHAR(16);
            DECLARE v_c_credit CHAR(2);
            DECLARE v_h_data VARCHAR(24);
            DECLARE v_c_data_updated VARCHAR(500);
            
            IF p_w_id IS NULL OR p_d_id IS NULL OR p_c_id IS NULL OR p_amount IS NULL THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '支付事务缺少必要参数';
            END IF;
            
            IF p_amount <= 0 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '支付金额必须大于0';
            END IF;
            
            SELECT w_name, w_ytd INTO v_w_name, @w_ytd FROM warehouse WHERE w_id = p_w_id FOR UPDATE;

            SELECT d_name, d_ytd INTO v_d_name, @d_ytd FROM district WHERE d_w_id = p_w_id AND d_id = p_d_id FOR UPDATE;

            SELECT c_first, c_middle, c_last, c_credit, c_data, c_balance, c_ytd_payment, c_payment_cnt
            INTO v_c_first, v_c_middle, v_c_last, v_c_credit, v_c_data_updated, @c_balance, @c_ytd_payment, @c_payment_cnt
            FROM customer 
            WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id
            FOR UPDATE;
            
            SET v_h_data = CONCAT(LEFT(v_w_name, 10), ' ', LEFT(v_d_name, 10));
            
            UPDATE warehouse 
            SET w_ytd = w_ytd + p_amount 
            WHERE w_id = p_w_id;
            
            UPDATE district 
            SET d_ytd = d_ytd + p_amount 
            WHERE d_w_id = p_w_id AND d_id = p_d_id;
            
            UPDATE customer 
            SET c_balance = c_balance - p_amount,
                c_ytd_payment = c_ytd_payment + p_amount,
                c_payment_cnt = c_payment_cnt + 1
            WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;
            
            IF v_c_credit = 'BC' THEN
                SET v_c_data_updated = CONCAT(
                    p_c_id, ' ', p_d_id, ' ', p_w_id, ' ', p_d_id, ' ', p_w_id, ' ', 
                    p_amount, ' | ', 
                    SUBSTRING(v_c_data_updated, 1, 500 - LENGTH(CONCAT(
                        p_c_id, ' ', p_d_id, ' ', p_w_id, ' ', p_d_id, ' ', p_w_id, ' ', 
                        p_amount, ' | '
                    )))
                );
                
                UPDATE customer 
                SET c_data = v_c_data_updated
                WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;
            END IF;
            
            INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
            VALUES (
                p_c_id, p_d_id, p_w_id, p_d_id, p_w_id,
                DATE_ADD(base_date, INTERVAL ((p_w_id * 1000 + p_d_id * 100 + p_c_id) % 10000) HOUR),
                p_amount, 
                v_h_data
            );
            
            SELECT 
                'PAYMENT' AS transaction_type,
                v_c_first AS customer_first_name,
                v_c_middle AS customer_middle,
                v_c_last AS customer_last_name,
                v_c_credit AS customer_credit,
                (SELECT c_balance FROM customer WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id) AS new_balance,
                v_w_name AS warehouse_name,
                v_d_name AS district_name,
                p_amount AS payment_amount;
        END;

    ELSEIF p_transaction_type = 'DELIVERY' THEN
        BEGIN
            DECLARE v_d_id INT DEFAULT 1;
            DECLARE v_no_o_id INT;
            DECLARE v_o_c_id INT;
            DECLARE v_ol_total DECIMAL(8,2);
            DECLARE v_delivery_d TIMESTAMP;
            DECLARE delivered_count INT DEFAULT 0;
            
            IF p_w_id IS NULL OR p_o_carrier_id IS NULL THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '交付事务缺少必要参数';
            END IF;
            
            IF p_o_carrier_id < 1 OR p_o_carrier_id > 10 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '承运商ID必须在1-10之间';
            END IF;
            
            CREATE TEMPORARY TABLE IF NOT EXISTS delivery_results (
                district_id INT,
                order_id INT,
                customer_id INT,
                amount DECIMAL(8,2)
            );
            
            WHILE v_d_id <= 10 DO
                SELECT no_o_id INTO v_no_o_id 
                FROM new_orders 
                WHERE no_w_id = p_w_id AND no_d_id = v_d_id 
                ORDER BY no_o_id ASC 
                LIMIT 1
                FOR UPDATE;
                
                IF v_no_o_id IS NOT NULL THEN
                    SET v_delivery_d = DATE_ADD(base_date, INTERVAL ((p_w_id * 100 + v_d_id * 10 + v_no_o_id) % 1000) HOUR);

                    DELETE FROM new_orders 
                    WHERE no_w_id = p_w_id AND no_d_id = v_d_id AND no_o_id = v_no_o_id;
                    
                    SELECT o_c_id INTO v_o_c_id 
                    FROM orders 
                    WHERE o_w_id = p_w_id AND o_d_id = v_d_id AND o_id = v_no_o_id;
                    
                    SELECT SUM(ol_amount) INTO v_ol_total 
                    FROM order_line 
                    WHERE ol_w_id = p_w_id AND ol_d_id = v_d_id AND ol_o_id = v_no_o_id;
                    
                    UPDATE orders 
                    SET o_carrier_id = p_o_carrier_id 
                    WHERE o_w_id = p_w_id AND o_d_id = v_d_id AND o_id = v_no_o_id;
                    
                    UPDATE order_line 
                    SET ol_delivery_d = v_delivery_d 
                    WHERE ol_w_id = p_w_id AND ol_d_id = v_d_id AND ol_o_id = v_no_o_id;
                    
                    SELECT c_balance, c_delivery_cnt 
                    INTO @c_balance, @c_delivery_cnt
                    FROM customer 
                    WHERE c_w_id = p_w_id AND c_d_id = v_d_id AND c_id = v_o_c_id
                    FOR UPDATE;

                    UPDATE customer 
                    SET c_balance = c_balance + v_ol_total,
                        c_delivery_cnt = c_delivery_cnt + 1
                    WHERE c_w_id = p_w_id AND c_d_id = v_d_id AND c_id = v_o_c_id;
                    
                    INSERT INTO delivery_results (district_id, order_id, customer_id, amount)
                    VALUES (v_d_id, v_no_o_id, v_o_c_id, v_ol_total);
                    
                    SET delivered_count = delivered_count + 1;
                END IF;
                
                SET v_d_id = v_d_id + 1;
            END WHILE;
            
            SELECT 
                'DELIVERY' AS transaction_type,
                p_w_id AS warehouse_id,
                p_o_carrier_id AS carrier_id,
                delivered_count AS orders_delivered;
            
            IF delivered_count > 0 THEN
                SELECT 
                    district_id,
                    order_id,
                    customer_id,
                    amount
                FROM delivery_results
                ORDER BY district_id;
            END IF;
            
            DROP TEMPORARY TABLE IF EXISTS delivery_results;
        END;

    ELSEIF p_transaction_type = 'ORDER_STATUS' THEN
        BEGIN
            DECLARE v_c_first VARCHAR(16);
            DECLARE v_c_middle CHAR(2);
            DECLARE v_c_last VARCHAR(16);
            DECLARE v_c_balance DECIMAL(12,2);
            DECLARE v_o_id INT;
            DECLARE v_o_entry_d TIMESTAMP;
            DECLARE v_o_carrier_id INT;
            DECLARE v_o_ol_cnt INT;
            DECLARE v_o_amount DECIMAL(6,2);
            
            IF p_w_id IS NULL OR p_d_id IS NULL OR p_c_id IS NULL THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '订单状态查询缺少必要参数';
            END IF;
            
            SELECT c_first, c_middle, c_last, c_balance 
            INTO v_c_first, v_c_middle, v_c_last, v_c_balance
            FROM customer 
            WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;
            
            SELECT o_id, o_entry_d, o_carrier_id, o_ol_cnt, o_amount
            INTO v_o_id, v_o_entry_d, v_o_carrier_id, v_o_ol_cnt, v_o_amount
            FROM orders 
            WHERE o_w_id = p_w_id AND o_d_id = p_d_id AND o_c_id = p_c_id 
            ORDER BY o_id DESC 
            LIMIT 1;
            
            SELECT 
                'ORDER_STATUS' AS transaction_type,
                v_c_first AS customer_first_name,
                v_c_middle AS customer_middle,
                v_c_last AS customer_last_name,
                v_c_balance AS customer_balance,
                v_o_id AS last_order_id,
                v_o_entry_d AS order_entry_date,
                v_o_carrier_id AS carrier_id,
                v_o_ol_cnt AS order_line_count,
                v_o_amount AS order_amount;
                
            IF v_o_id IS NOT NULL THEN
                SELECT 
                    ol_number AS line_number,
                    ol_i_id AS item_id,
                    ol_supply_w_id AS supply_warehouse,
                    ol_quantity AS quantity,
                    ol_amount AS amount,
                    ol_delivery_d AS delivery_date
                FROM order_line 
                WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id = v_o_id
                ORDER BY ol_number;
            END IF;
        END;

    ELSEIF p_transaction_type = 'STOCK_LEVEL' THEN
        BEGIN
            DECLARE v_d_next_o_id INT;
            DECLARE v_low_stock_count INT;
            
            IF p_w_id IS NULL OR p_d_id IS NULL OR p_threshold IS NULL THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '库存水平查询缺少必要参数';
            END IF;
            
            IF p_threshold < 10 OR p_threshold > 20 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '库存阈值必须在10-20之间';
            END IF;
            
            SELECT d_next_o_id INTO v_d_next_o_id
            FROM district 
            WHERE d_w_id = p_w_id AND d_id = p_d_id;
            
            SELECT COUNT(DISTINCT s.s_i_id) INTO v_low_stock_count
            FROM order_line ol
            JOIN stock s ON ol.ol_i_id = s.s_i_id AND ol.ol_w_id = s.s_w_id
            WHERE ol.ol_w_id = p_w_id 
              AND ol.ol_d_id = p_d_id
              AND ol.ol_o_id BETWEEN (v_d_next_o_id - 20) AND (v_d_next_o_id - 1)
              AND s.s_quantity < p_threshold;
            
            SELECT 
                'STOCK_LEVEL' AS transaction_type,
                p_w_id AS warehouse_id,
                p_d_id AS district_id,
                v_d_next_o_id AS next_order_id,
                v_low_stock_count AS low_stock_count,
                p_threshold AS threshold;
        END;

    ELSE
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = '无效的事务类型，支持: NEW_ORDER, PAYMENT, DELIVERY, ORDER_STATUS, STOCK_LEVEL';
    END IF;
    
    COMMIT;
END//

CREATE PROCEDURE IF NOT EXISTS ExecuteTPCCWorkload(
    IN p_warehouse_count INT,
    IN p_districts_per_warehouse INT,
    IN p_customers_per_district INT,
    IN p_item_count INT,
    IN p_transaction_count INT,
    IN p_new_order_ratio DECIMAL(3,2),
    IN p_payment_ratio DECIMAL(3,2),
    IN p_delivery_ratio DECIMAL(3,2),
    IN p_order_status_ratio DECIMAL(3,2),
    IN p_stock_level_ratio DECIMAL(3,2)
)
BEGIN
    DECLARE i INT DEFAULT 1;
    DECLARE start_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP;
    DECLARE end_time TIMESTAMP;
    DECLARE new_order_count INT;
    DECLARE payment_count INT;
    DECLARE delivery_count INT;
    DECLARE order_status_count INT;
    DECLARE stock_level_count INT;
    DECLARE total_executed INT DEFAULT 0;

    SET p_new_order_ratio = COALESCE(p_new_order_ratio, 0.45);
    SET p_payment_ratio = COALESCE(p_payment_ratio, 0.43);
    SET p_delivery_ratio = COALESCE(p_delivery_ratio, 0.04);
    SET p_order_status_ratio = COALESCE(p_order_status_ratio, 0.04);
    SET p_stock_level_ratio = COALESCE(p_stock_level_ratio, 0.04);

    SET new_order_count = FLOOR(p_transaction_count * p_new_order_ratio);
    SET payment_count = FLOOR(p_transaction_count * p_payment_ratio);
    SET delivery_count = FLOOR(p_transaction_count * p_delivery_ratio);
    SET order_status_count = FLOOR(p_transaction_count * p_order_status_ratio);
    SET stock_level_count = FLOOR(p_transaction_count * p_stock_level_ratio);
    
    IF ABS(p_new_order_ratio + p_payment_ratio + p_delivery_ratio + p_order_status_ratio + p_stock_level_ratio - 1.0) > 0.01 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Transaction ratios must sum to approximately 1.0';
    END IF;
    
    SET start_time = CURRENT_TIMESTAMP;
    
    SET i = 1;
    WHILE i <= new_order_count DO
        CALL ExecuteTPCCTransaction('NEW_ORDER', 
            (i % p_warehouse_count) + 1,
            (i % p_districts_per_warehouse) + 1,
            (i % p_customers_per_district) + 1,
            5 + ((i % 6) + 1),
            CASE WHEN (i % 10) = 0 THEN 0 ELSE 1 END,
            NULL, NULL, NULL);
        SET i = i + 1;
        SET total_executed = total_executed + 1;
    END WHILE;
    
    SET i = 1;
    WHILE i <= payment_count DO
        CALL ExecuteTPCCTransaction('PAYMENT', 
            (i % p_warehouse_count) + 1,
            (i % p_districts_per_warehouse) + 1,
            (i % p_customers_per_district) + 1,
            NULL, NULL, 
            100.00 + ((i % 50) * 5), NULL, NULL);
        SET i = i + 1;
        SET total_executed = total_executed + 1;
    END WHILE;
    
    SET i = 1;
    WHILE i <= delivery_count DO
        CALL ExecuteTPCCTransaction('DELIVERY', 
            (i % p_warehouse_count) + 1,
            NULL, NULL, NULL, NULL, NULL, 
            1 + (i % 10), NULL);
        SET i = i + 1;
        SET total_executed = total_executed + 1;
    END WHILE;
    
    SET i = 1;
    WHILE i <= order_status_count DO
        CALL ExecuteTPCCTransaction('ORDER_STATUS', 
            (i % p_warehouse_count) + 1,
            (i % p_districts_per_warehouse) + 1,
            (i % p_customers_per_district) + 1,
            NULL, NULL, NULL, NULL, NULL);
        SET i = i + 1;
        SET total_executed = total_executed + 1;
    END WHILE;
    
    SET i = 1;
    WHILE i <= stock_level_count DO
        CALL ExecuteTPCCTransaction('STOCK_LEVEL', 
            (i % p_warehouse_count) + 1,
            (i % p_districts_per_warehouse) + 1,
            NULL, NULL, NULL, NULL, NULL, 
            10 + (i % 11));
        SET i = i + 1;
        SET total_executed = total_executed + 1;
    END WHILE;
    
    SET end_time = CURRENT_TIMESTAMP;
END//

DELIMITER ;//