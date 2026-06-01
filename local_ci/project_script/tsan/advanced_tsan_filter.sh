#!/bin/bash

# advanced_tsan_filter.sh

analyze_tsan_log() {
    local log_file="$1"
    local output_file="${2:-tsan_report.txt}"
    
    echo "Analyzing TSan log: $log_file" | tee "$output_file"
    echo "Generated at: $(date)" | tee -a "$output_file"
    echo "======================================" | tee -a "$output_file"
    
    # 统计不同类型的错误
    local warnings=$(grep -c "WARNING: ThreadSanitizer" "$log_file")
    local errors=$(grep -c "ERROR: ThreadSanitizer" "$log_file")
    local data_races=$(grep -c -i "data race" "$log_file")
    
    echo "Summary:" | tee -a "$output_file"
    echo "  Warnings: $warnings" | tee -a "$output_file"
    echo "  Errors: $errors" | tee -a "$output_file"
    echo "  Data Races: $data_races" | tee -a "$output_file"
    echo "" | tee -a "$output_file"
    
    # 提取所有TSan错误块
    echo "Detailed Errors:" | tee -a "$output_file"
    awk '
    BEGIN { in_block=0; block_count=0 }
    /^===================/ {
        if (in_block) {
            in_block=0
            print "======================================\n"
        } else {
            in_block=1
            block_count++
            print "Error Block #" block_count ":"
        }
    }
    in_block || /ThreadSanitizer/ {
        print
    }
    ' "$log_file" | tee -a "$output_file"
    
    # 提取涉及的文件和函数
    echo -e "\nAffected Files and Functions:" | tee -a "$output_file"
    grep -oE "#[0-9]+\s+0x[0-9a-f]+\s+in\s+[^ ]+" "$log_file" | sort | uniq -c | sort -nr | tee -a "$output_file"
}

# 使用方法
if [ $# -eq 0 ]; then
    echo "Usage: $0 <logfile> [output_file]"
    echo "Example: $0 myapp.log tsan_analysis.txt"
    exit 1
fi

analyze_tsan_log "$1" "$2"