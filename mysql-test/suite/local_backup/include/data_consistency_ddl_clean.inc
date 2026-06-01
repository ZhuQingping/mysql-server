#
# Clean DDL operation
#

DELIMITER //;

CREATE PROCEDURE IF NOT EXISTS CleanAllDDL()
BEGIN
    drop view customer_orders_summary;
    drop table orders_history;
    drop table audit_log;
    drop INDEX idx_order_line_delivery ON order_line;
    drop INDEX idx_item_price ON item;
    drop INDEX idx_history_data ON history;
    ALTER TABLE orders DROP COLUMN o_tax_amount;
    ALTER TABLE order_line DROP COLUMN ol_total;
    COMMIT;
END//

DELIMITER ;//
