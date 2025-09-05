#!/usr/bin/env bash
# BlueField PCIe bandwidth monitor – works with IN_*_BYTE_CNT names
set -euo pipefail

INTERVAL=${1:-1}          # seconds; default 1 s
DIR="$(grep -l bfperf /sys/class/hwmon/*/name | xargs -n1 dirname)/pcie0"

# List of counter files in the older BSPs
FILES=(IN_P_BYTE_CNT IN_NP_BYTE_CNT IN_C_BYTE_CNT
       OUT_P_BYTE_CNT OUT_NP_BYTE_CNT OUT_C_BYTE_CNT)

read_all() {
    for f in "${FILES[@]}"; do cat "$DIR/$f"; done
}

printf "%-19s | %9s | %9s |  P / NP /  C  (Gb/s)\n" "Timestamp" "In_Total" "Out_Total"
printf -- "-------------------+-----------+-----------+---------------------------\n"

prev=($(read_all)); t0=$(date +%s.%N)
while :; do
    sleep "$INTERVAL"
    now=($(read_all)); t1=$(date +%s.%N)
    dt=$(echo "$t1 - $t0" | bc -l)

    # rate[i] = (now[i]-prev[i])*8 / dt / 1e9  (Gb/s)
    for i in {0..5}; do
        diff=$(( ${now[$i]} - ${prev[$i]} ))
        rate[$i]=$(echo "scale=3; $diff*8/1000000000/$dt" | bc -l)
    done

    in_sum=$(echo "${rate[0]} + ${rate[1]} + ${rate[2]}" | bc)
    out_sum=$(echo "${rate[3]} + ${rate[4]} + ${rate[5]}" | bc)
    ts=$(date +"%Y-%m-%d %H:%M:%S")

    printf "%-19s | %9.3f | %9.3f | %5.3f / %5.3f / %5.3f  | %5.3f / %5.3f / %5.3f\n" \
        "$ts" "$in_sum" "$out_sum" \
        "${rate[0]}" "${rate[1]}" "${rate[2]}" \
        "${rate[3]}" "${rate[4]}" "${rate[5]}"

    prev=("${now[@]}"); t0=$t1
done
