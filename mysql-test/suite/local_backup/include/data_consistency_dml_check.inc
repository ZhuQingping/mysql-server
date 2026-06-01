#
# Execute sql to check data consistency with zero output
#

SELECT COUNT(*) FROM (SELECT w.w_id, w.w_ytd, d.sum_d_ytd
           FROM warehouse w,
                (SELECT d_w_id, SUM(d_ytd) sum_d_ytd
                 FROM district
                 GROUP BY d_w_id) d
           WHERE w.w_id = d.d_w_id) as x
WHERE w_ytd - sum_d_ytd != 0;

SELECT COUNT(*) FROM (SELECT d.d_w_id, d.d_id, d.d_next_o_id, o.max_o_id, no.max_no_o_id
           FROM district d,
                (SELECT o_w_id, o_d_id, MAX(o_id) max_o_id
                 FROM orders
                 GROUP BY o_w_id, o_d_id) o,
                (SELECT no_w_id, no_d_id, MAX(no_o_id) max_no_o_id
                 FROM new_orders
                 GROUP BY no_w_id, no_d_id) no
           WHERE d.d_w_id = o.o_w_id AND d.d_w_id = no.no_w_id AND
                 d.d_id = o.o_d_id AND d.d_id = no.no_d_id) as x
WHERE d_next_o_id - 1 != max_o_id OR d_next_o_id - 1 != max_no_o_id;

SELECT COUNT(*) FROM (SELECT no_w_id, no_d_id, MAX(no_o_id) max_no_o_id,
                  MIN(no_o_id) min_no_o_id, COUNT(*) count_no
           FROM new_orders
           GROUP BY no_w_id, no_d_Id) as x
WHERE max_no_o_id - min_no_o_id + 1 != count_no;

SELECT COUNT(*) FROM (SELECT o.o_w_id, o.o_d_id, o.sum_o_ol_cnt, ol.count_ol
           FROM (SELECT o_w_id, o_d_id, SUM(o_ol_cnt) sum_o_ol_cnt
                 FROM orders
                 GROUP BY o_w_id, o_d_id) o,
                (SELECT ol_w_id, ol_d_id, COUNT(*) count_ol
                 FROM order_line
                 GROUP BY ol_w_id, ol_d_id) ol
           WHERE o.o_w_id = ol.ol_w_id AND
                 o.o_d_id = ol.ol_d_id) as x
WHERE sum_o_ol_cnt != count_ol;

SELECT COUNT(*) FROM (SELECT o.o_w_id, o.o_d_id, o.o_id, o.o_carrier_id, no.count_no
           FROM orders o,
                (SELECT no_w_id, no_d_id, no_o_id, COUNT(*) count_no
                 FROM new_orders
                 GROUP BY no_w_id, no_d_id, no_o_id) no
           WHERE o.o_w_id = no.no_w_id AND
                 o.o_d_id = no.no_d_id AND
                 o.o_id = no.no_o_id) as x
WHERE (o_carrier_id IS NULL AND count_no = 0) OR
      (o_carrier_id IS NOT NULL AND count_no != 0);

SELECT COUNT(*) FROM (SELECT o.o_w_id, o.o_d_id, o.o_id, o.o_ol_cnt, ol.count_ol
           FROM orders o,
                (SELECT ol_w_id, ol_d_id, ol_o_id, COUNT(*) count_ol
                 FROM order_line
                 GROUP BY ol_w_id, ol_d_id, ol_o_id) ol
           WHERE o.o_w_id = ol.ol_w_id AND
                 o.o_d_id = ol.ol_d_id AND
                 o.o_id = ol.ol_o_id) as x
WHERE o_ol_cnt != count_ol;

SELECT COUNT(*) FROM (SELECT ol.ol_w_id, ol.ol_d_id, ol.ol_o_id, ol.ol_delivery_d,
                  o.o_carrier_id
           FROM order_line ol,
                orders o
           WHERE ol.ol_w_id = o.o_w_id AND
                 ol.ol_d_id = o.o_d_id AND
                 ol.ol_o_id = o.o_id) as x
WHERE (ol_delivery_d IS NULL AND o_carrier_id IS NOT NULL) OR
      (ol_delivery_d IS NOT NULL AND o_carrier_id IS NULL);

SELECT COUNT(*)
FROM (SELECT w.w_id, w.w_ytd, h.sum_h_amount
      FROM warehouse w,
           (SELECT h_w_id, SUM(h_amount) sum_h_amount FROM history GROUP BY h_w_id) h
      WHERE w.w_id = h.h_w_id) as x
WHERE w_ytd - sum_h_amount != 0;

SELECT COUNT(*)
FROM (SELECT d.d_w_id, d.d_id, d.d_ytd, h.sum_h_amount
      FROM district d,
           (SELECT h_w_id, h_d_id, SUM(h_amount) sum_h_amount
            FROM history
            GROUP BY h_w_id, h_d_id) h
      WHERE d.d_w_id = h.h_w_id
        AND d.d_id = h.h_d_id) as x
WHERE d_ytd != sum_h_amount;

