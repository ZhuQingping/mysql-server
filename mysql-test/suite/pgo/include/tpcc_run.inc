##############################################################################
#
# SUMMARY
#
#   Run TPC-C test on table(s) created by tpcc_prepare.inc
#
# USAGE
#
#   let loop=10
#   --source suite/pgo/include/tpcc_run.inc
#
##############################################################################

USE tpcc10;

if (!$loop)
{
  let $loop = 50;
}

--disable_query_log
--disable_result_log

--echo loop : $loop
while ($loop)
{
    let $warehouse_id = `SELECT FLOOR(1 + RAND() * 10)`;
    let $district_id = `SELECT FLOOR(1 + RAND() * 10)`;
    let $customer_id = `SELECT FLOOR(1 + RAND() * 3000)`;
    let $item_id = `SELECT FLOOR(1 + RAND() * 100000)`;
    let $order_id = `SELECT FLOOR(3000 + RAND() * 10000)`;

    --echo # do new order
    BEGIN;
    --eval SELECT d_next_o_id, d_tax FROM district WHERE d_id = $district_id AND d_w_id = $warehouse_id FOR UPDATE;
    --eval UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = $district_id AND d_w_id = $warehouse_id
    --eval INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local) VALUES ($order_id, $district_id, $warehouse_id, $customer_id, NOW(), NULL, 5, 1);
    --eval INSERT INTO new_orders (no_o_id, no_d_id, no_w_id) VALUES ($order_id, $district_id, $warehouse_id);
    --eval SELECT i_price, i_name, i_data FROM item WHERE i_id = $item_id;
    --eval SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 FROM stock WHERE s_i_id = $item_id AND s_w_id = $warehouse_id FOR UPDATE;
    # insert order_line
    let $i = 1;
    while ($i <= 5) {
        --eval INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES ($order_id, $district_id, $warehouse_id, $i, $item_id, $warehouse_id, 5, 10.50, 'dist_info')
        inc $i;
        inc $item_id;
    }
    # update stock
    --eval UPDATE stock SET s_quantity = s_quantity - 5 WHERE s_i_id = $item_id AND s_w_id = $warehouse_id;
    COMMIT;

    --echo # do payment
    BEGIN;
    --eval UPDATE warehouse SET w_ytd = w_ytd + 100 WHERE w_id = $warehouse_id;
    --eval UPDATE district SET d_ytd = d_ytd + 100 WHERE d_id = $district_id AND d_w_id = $warehouse_id;
    --eval SELECT count(c_id) FROM customer WHERE c_w_id = $warehouse_id AND c_d_id = $district_id;
    --eval SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $warehouse_id AND c_d_id = $district_id AND c_id = $customer_id FOR UPDATE;
    --eval UPDATE customer SET c_balance = c_balance - 100, c_ytd_payment = c_ytd_payment + 100 WHERE c_id = $customer_id AND c_d_id = $district_id AND c_w_id = $warehouse_id;
    --eval INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES ($customer_id, $district_id, $warehouse_id, $district_id, $warehouse_id, NOW(), 100, 'Payment');
    COMMIT;

    --echo # do order stat
    BEGIN;
    --eval SELECT count(c_id) FROM customer WHERE c_w_id = $warehouse_id AND c_d_id = $district_id AND c_last = "ABLEABLEANTI";
    --eval SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_w_id = $warehouse_id AND c_d_id = $district_id AND c_last = "ABLEABLEANTI" ORDER BY c_first;
    --eval SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_w_id = $warehouse_id AND o_d_id = $district_id AND o_c_id = $customer_id ORDER BY o_id DESC LIMIT 1;
    --eval SELECT o_id, o_entry_d, COALESCE(o_carrier_id,0) FROM orders WHERE o_w_id = $warehouse_id AND o_d_id = $district_id AND o_c_id = $customer_id AND o_id = (SELECT MAX(o_id) FROM orders WHERE o_w_id = $warehouse_id AND o_d_id = $district_id AND o_c_id = $customer_id);
    --eval SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_w_id = $warehouse_id AND ol_d_id = $district_id AND ol_o_id = $order_id;
    COMMIT;

    --echo # do delivery
    BEGIN;
    --eval SELECT COALESCE(MIN(no_o_id),0) FROM new_orders WHERE no_d_id = $district_id AND no_w_id = $warehouse_id;
    --eval SELECT no_o_id INTO @order_id FROM new_orders WHERE no_w_id = $warehouse_id AND no_d_id = $district_id ORDER BY no_o_id ASC LIMIT 1;
    --eval UPDATE orders SET o_carrier_id = 1 WHERE o_id = $order_id AND o_d_id = $district_id AND o_w_id = $warehouse_id;
    --eval UPDATE order_line SET ol_delivery_d = NOW() WHERE ol_o_id = $order_id AND ol_d_id = $district_id AND ol_w_id = $warehouse_id;
    --eval DELETE FROM new_orders WHERE no_o_id = $order_id AND no_d_id = $district_id AND no_w_id = $warehouse_id;
    --eval SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = $order_id AND ol_d_id = $district_id AND ol_w_id = $warehouse_id;
    --eval UPDATE customer SET c_balance = c_balance + 100 , c_delivery_cnt = c_delivery_cnt + 1 WHERE c_id = $customer_id AND c_d_id = $district_id AND c_w_id = $warehouse_id;
    COMMIT;

    --echo # do stock level
    BEGIN;
    --eval SELECT d_next_o_id FROM district WHERE d_id = $district_id AND d_w_id = $warehouse_id;
    --eval SELECT DISTINCT ol_i_id FROM order_line WHERE ol_w_id = $warehouse_id AND ol_d_id = $district_id AND ol_o_id < $order_id AND ol_o_id >= 1500;
    --eval SELECT COUNT(*) FROM stock WHERE s_w_id = $warehouse_id AND s_i_id = $item_id AND s_quantity < 10;
    COMMIT;

    dec $loop;
}

--enable_result_log
--enable_query_log

--echo # check data
--eval SELECT COUNT(*) AS orphaned_new_orders FROM new_orders no LEFT JOIN orders o ON no.no_o_id = o.o_id AND no.no_d_id = o.o_d_id AND no.no_w_id = o.o_w_id WHERE o.o_id IS NULL;
--eval SELECT COUNT(*) AS inconsistent_stock FROM stock WHERE s_quantity < 0;
