#!/bin/bash

USERNAME="ubuntu"

# Start results directory from results1
base_dir="results"
suffix=1
RESULTS_DIR="${base_dir}${suffix}"
while [[ -d "$RESULTS_DIR" ]]; do
  ((suffix++))
  RESULTS_DIR="${base_dir}${suffix}"
done
mkdir -p "$RESULTS_DIR"

declare -A workload_counts

i=0
for workload in "$@"; do
  port=$((23456 + i))

  # Track instance count for this workload
  workload_counts["$workload"]=$((workload_counts["$workload"] + 1))
  workload_instance_id="${workload}_${workload_counts["$workload"]}"

  workload_result_dir="${RESULTS_DIR}/${workload_instance_id}"
  mkdir -p "${workload_result_dir}"

  config_file="workload_scripts/${workload}/exp_config.sh"
  if [[ ! -f $config_file ]]; then
    echo "[!] Missing config file: $config_file"
    exit 1
  fi

  source "$config_file"
  iterations=${iterations:-1}

  echo "[*] Benchmarking $workload_instance_id (VM $i, port $port, $iterations iterations)..."
  echo "    ↳ Start time: $(date '+%Y-%m-%d %H:%M:%S')"

  (
    for ((iter=1; iter<=iterations; iter++)); do
      echo "  ↳ Iteration $iter for '$workload_instance_id'..."

      iter_dir="${workload_result_dir}/iter${iter}"
      mkdir -p "$iter_dir"
      touch "${iter_dir}/output"
      touch "${iter_dir}/time"

      start_ts=$(date +%s)

      if [[ $iter -eq 1 ]]; then
        sshpass ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $port ${USERNAME}@localhost "
          ./prepare_exp.sh && ./run_exp.sh
        " > "${iter_dir}/output" 2>&1
      else
        sshpass ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $port ${USERNAME}@localhost "
          ./run_exp.sh
        " > "${iter_dir}/output" 2>&1
      fi

      end_ts=$(date +%s)
      elapsed=$((end_ts - start_ts))
      echo "$elapsed" > "${iter_dir}/time"

      echo "    ↳ Finished iteration $iter at $(date '+%Y-%m-%d %H:%M:%S'), elapsed time: ${elapsed} sec"

      # Efficient single scp call for multiple files
      sshpass scp -P $port -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        ${USERNAME}@localhost:/home/ubuntu/{result_app_perf.txt,result_log.txt,result_err.txt} \
        "${iter_dir}/" 2>/dev/null
    done

    sshpass ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $port ${USERNAME}@localhost "
      rm -f ./prepare_exp.sh ./run_exp.sh
    "

    echo "[✔] Finished benchmarking $workload_instance_id at $(date '+%Y-%m-%d %H:%M:%S')"

  ) &

  pids[$i]=$!
  ((i++))
done

echo "[*] Waiting for all benchmark processes to complete..."
for pid in "${pids[@]}"; do
  wait $pid
done

echo "[✔] All benchmarks finished. Results saved in '$RESULTS_DIR/'"
