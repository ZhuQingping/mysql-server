#
# create table for data consistency check
#
--disable_warnings
DROP TABLE IF EXISTS order_line;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customer;
DROP TABLE IF EXISTS district;
DROP TABLE IF EXISTS warehouse;
DROP TABLE IF EXISTS item;
DROP TABLE IF EXISTS history;
DROP TABLE IF EXISTS stock;
DROP TABLE IF EXISTS new_orders;
--enable_warnings

CREATE TABLE IF NOT EXISTS warehouse (
    w_id INT PRIMARY KEY,
    w_name VARCHAR(10),
    w_street_1 VARCHAR(20),
    w_street_2 VARCHAR(20),
    w_city VARCHAR(20),
    w_state CHAR(2),
    w_zip CHAR(9),
    w_tax DECIMAL(4,4),
    w_ytd DECIMAL(12,2)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS district (
    d_id INT,
    d_w_id INT,
    d_name VARCHAR(10),
    d_street_1 VARCHAR(20),
    d_street_2 VARCHAR(20),
    d_city VARCHAR(20),
    d_state CHAR(2),
    d_zip CHAR(9),
    d_tax DECIMAL(4,4),
    d_ytd DECIMAL(12,2),
    d_next_o_id INT,
    PRIMARY KEY (d_w_id, d_id)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS customer (
    c_id INT,
    c_d_id INT,
    c_w_id INT,
    c_first VARCHAR(16),
    c_last VARCHAR(16),
    c_middle CHAR(2),
    c_street_1 VARCHAR(20),
    c_street_2 VARCHAR(20),
    c_city VARCHAR(20),
    c_state CHAR(2),
    c_zip CHAR(20),
    c_phone CHAR(16),
    c_since TIMESTAMP,
    c_credit CHAR(2),
    c_credit_lim DECIMAL(12,2),
    c_discount DECIMAL(4,4),
    c_balance DECIMAL(12,2),
    c_ytd_payment DECIMAL(12,2),
    c_payment_cnt INT,
    c_delivery_cnt INT,
    c_data VARCHAR(500),
    PRIMARY KEY (c_w_id, c_d_id, c_id)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS item (
    i_id INT PRIMARY KEY,
    i_name VARCHAR(24),
    i_price DECIMAL(5,2),
    i_data VARCHAR(50)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS stock (
    s_i_id INT,
    s_w_id INT,
    s_quantity INT,
    s_dist_01 CHAR(24),
    s_dist_02 CHAR(24),
    s_dist_03 CHAR(24),
    s_dist_04 CHAR(24),
    s_dist_05 CHAR(24),
    s_dist_06 CHAR(24),
    s_dist_07 CHAR(24),
    s_dist_08 CHAR(24),
    s_dist_09 CHAR(24),
    s_dist_10 CHAR(24),
    s_ytd INT,
    s_order_cnt INT,
    s_remote_cnt INT,
    s_data VARCHAR(50),
    PRIMARY KEY (s_w_id, s_i_id)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS orders (
    o_id INT,
    o_d_id INT,
    o_w_id INT,
    o_c_id INT,
    o_entry_d TIMESTAMP,
    o_carrier_id INT,
    o_ol_cnt INT,
    o_all_local INT,
    o_amount DECIMAL(12,2),
    PRIMARY KEY (o_w_id, o_d_id, o_id)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS new_orders (
    no_o_id INT,
    no_d_id INT,
    no_w_id INT,
    PRIMARY KEY (no_w_id, no_d_id, no_o_id)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS order_line (
    ol_o_id INT,
    ol_d_id INT,
    ol_w_id INT,
    ol_number INT,
    ol_i_id INT,
    ol_supply_w_id INT,
    ol_delivery_d TIMESTAMP,
    ol_quantity INT,
    ol_amount DECIMAL(12,2),
    ol_dist_info CHAR(24),
    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
) engine=Dstore;

CREATE TABLE IF NOT EXISTS history (
    h_c_id INT,
    h_c_d_id INT,
    h_c_w_id INT,
    h_d_id INT,
    h_w_id INT,
    h_date TIMESTAMP,
    h_amount DECIMAL(12,2),
    h_data VARCHAR(24),
    INDEX idx_history_customer (h_c_w_id, h_c_d_id, h_c_id),
    INDEX idx_history_date (h_date)
) engine=Dstore;