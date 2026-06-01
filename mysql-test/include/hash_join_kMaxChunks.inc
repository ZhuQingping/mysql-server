--disable_query_log
SELECT VARIABLE_VALUE INTO @start_tmp_files 
FROM performance_schema.global_status 
WHERE VARIABLE_NAME = 'Created_tmp_files';
--enable_query_log

--replace_regex /\(cost=[^)]+\)/(cost=*** rows=***)/ /actual time=[0-9.,]+ rows=[0-9.,]+ loops=[0-9,]+/actual time=*** rows=*** loops=***/
EXPLAIN ANALYZE SELECT t1.cbi_product_code AS productCode, t3.id, t2.product_id, t2.product_name, t2.product_name_cn
	, t2.product_name_en, t3.di_id, t3.di_effective, t3.data_item_id, t3.data_item_name
	, t3.data_item_name_en, t3.data_item_effective, t3.l1_class_id, t3.l1_class_name_cn, t3.l1_class_name_en
	, t3.l1_class_effective, t3.l2_class_id, t3.l2_class_name_cn, t3.l2_class_name_en, t3.l2_class_effective
	, t3.l3_class_id, t3.l3_class_name_cn, t3.l3_class_name_en, t3.l3_class_effective, t3.confidentiality_level_name_cn
	, t3.confidentiality_level_name_en, t3.qlink_name_cn, t3.qlink_name_en, t3.sensitivity_level_name_cn, t3.sensitivity_level_name_en
	, t3.business_process_name, t3.business_process_name_en, t3.business_process_effective, t3.purpose, t3.purpose_cn
	, t3.expiration_time, t3.process_type, t3.process_type_name_cn, t3.process_type_name_en, t3.storage_mode_cn
	, t3.storage_mode_en, t3.pii_type
FROM t_classify_product_info t2
	JOIN t_classify_personal_di t3 ON t2.product_id = t3.product_id
	LEFT JOIN t_classify_dtrust_cbi_rel t1 ON t2.product_id = t1.dtrust_id;

--disable_query_log
SELECT VARIABLE_VALUE INTO @final_tmp_files 
FROM performance_schema.global_status
WHERE VARIABLE_NAME = 'Created_tmp_files';

SET @tmp_files_diff = @final_tmp_files - @start_tmp_files;
--enable_query_log

SELECT @tmp_files_diff AS tmp_files_created_during_query;
