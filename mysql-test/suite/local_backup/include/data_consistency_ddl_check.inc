#
# Execute sql to check data consistency with zero output
#

SELECT COUNT(*)
FROM information_schema.STATISTICS 
WHERE TABLE_SCHEMA = DATABASE() 
AND INDEX_NAME IN (
    'idx_order_line_delivery', 'idx_item_price', 'idx_history_data'
);

SELECT COUNT(*) 
FROM information_schema.VIEWS 
WHERE TABLE_SCHEMA = DATABASE();

SELECT COUNT(*) 
FROM information_schema.TABLES 
WHERE TABLE_SCHEMA = DATABASE() 
AND TABLE_NAME IN ('orders_history', 'audit_log') 
AND TABLE_TYPE = 'BASE TABLE';

SELECT COUNT(*) 
FROM information_schema.COLUMNS 
WHERE TABLE_SCHEMA = DATABASE() 
AND COLUMN_NAME IN ('o_tax_amount', 'ol_total') 
AND EXTRA LIKE '%GENERATED%';