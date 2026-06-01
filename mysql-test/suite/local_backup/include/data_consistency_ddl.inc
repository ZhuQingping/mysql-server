#
# Execute DDL
#

DELIMITER //;

CREATE PROCEDURE IF NOT EXISTS ExecuteAllDDL()
BEGIN
    CREATE VIEW customer_orders_summary AS
    SELECT 
        c.c_w_id, c.c_d_id, c.c_id, c.c_first, c.c_last,
        COUNT(o.o_id) as order_count,
        COALESCE(SUM(o.o_amount), 0) as total_order_amount,
        c.c_balance
    FROM customer c
    LEFT JOIN orders o ON c.c_w_id = o.o_w_id AND c.c_d_id = o.o_d_id AND c.c_id = o.o_c_id
    GROUP BY c.c_w_id, c.c_d_id, c.c_id, c.c_first, c.c_last, c.c_balance;

    CREATE TABLE orders_history (
        o_id INT,
        o_d_id INT,
        o_w_id INT,
        o_c_id INT,
        o_entry_d TIMESTAMP,
        o_amount DECIMAL(10,2),
        o_status VARCHAR(20),
        archived_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        INDEX idx_oh_archived (archived_date)
    );

    CREATE TABLE audit_log (
        audit_id INT AUTO_INCREMENT PRIMARY KEY,
        table_name VARCHAR(50),
        operation VARCHAR(10),
        record_id VARCHAR(100),
        old_values JSON,
        new_values JSON,
        changed_by VARCHAR(50),
        changed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    );

    ALTER TABLE orders ADD COLUMN o_tax_amount DECIMAL(10,2) AS (o_amount * 0.1);
    ALTER TABLE order_line ADD COLUMN ol_total DECIMAL(8,2) AS (ol_quantity * ol_amount);

    CREATE INDEX idx_order_line_delivery ON order_line(ol_delivery_d);
    CREATE INDEX idx_item_price ON item(i_price);
    CREATE INDEX idx_history_data ON history(h_data);

    COMMIT;
END//

DELIMITER ;//