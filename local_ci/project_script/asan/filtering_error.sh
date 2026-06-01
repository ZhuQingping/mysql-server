#!/bin/bash

log_file=$1
grep -n '==[0-9]\+==ERROR' ${log_file} | awk -F '[:]' '{print $1}' > line_txt
let i=0
line_array=()
while read line
do
    line_array[$i]=$line
    let i+=1
done < line_txt

function stack_judgment()
{
    log_segment=$1
    if [ -n "`grep '==[0-9]\+==ABORTING' ${log_segment}`" ]; then
        end_line=$(grep -m1 -n '==[0-9]\+==ABORTING' ${log_segment} | awk -F '[:]' '{print $1}')
    elif [ -n "`grep 'SUMMARY: AddressSanitizer' ${log_segment}`" ]; then
        end_line=$(grep -m1 -n 'SUMMARY: AddressSanitizer' ${log_segment} | awk -F '[:]' '{print $1}')
    fi
    sed -n "1,${end_line}p" ${log_segment} >${log_segment}_stack
    while read line
    do
        if [ $(grep -c $line ${log_segment}_stack) -ne 0 ]; then
            return 0
        fi
    done < ASAN_masking_keywords
    mv ${log_segment}_stack ERROR_${log_segment}_stack
    echo "=======================ERROR STACK======================="
    cat ERROR_${log_segment}_stack
    return 1
}


k=`expr ${#line_array[*]} - 1`
exit_flag=0
let l=0
while [ $l -lt ${k} ]
do
    j=`expr ${l} + 1`
    start_line=${line_array[$l]}
    end_line=`expr ${line_array[$j]} - 1`
    sed -n "${start_line},${end_line}p" ${log_file} >log_segment${l}
    stack_judgment log_segment${l}
    let exit_flag+=$?
    echo "Num: $exit_flag"
    let l+=1
done

log_num=$(grep -c "" ${log_file})
sed -n "${line_array[$k]},${log_num}p" ${log_file} >log_segment${l}
stack_judgment log_segment${l}
let exit_flag+=$?
echo "Num: $exit_flag"

if [ $exit_flag -gt 0 ]; then
    exit 1
fi
exit 0