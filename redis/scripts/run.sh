#!/bin/bash

# Fancy Redis VM Benchmark Script with Incremental Naming
# Usage: ./run.sh [optional_name]

NUM_VMS=8
ITERATIONS=5

# Base name from argument, or default if not provided
BASE_NAME="${1:-benchmark}"

# Find next available postfix number
RESULTS_DIR="redis-benchmark-results"
mkdir -p "$RESULTS_DIR"

i=0
while [ -d "${RESULTS_DIR}/${BASE_NAME}_${i}" ]; do
  i=$((i+1))
done

RUN_DIR="${RESULTS_DIR}/${BASE_NAME}_${i}"
mkdir -p "$RUN_DIR"

reset_state() {
  echo "🔄 Resetting machine state..."
  sudo bash -c "echo 3 > /proc/sys/vm/drop_caches"
  sudo bash -c "echo 1 > /proc/sys/vm/compact_memory"
  sleep 5
}

reset_state

# Launch Redis VMs
./launch_redis_vms.sh "$NUM_VMS"

# Set output directory for benchmark logs
export OUT_DIR="${RUN_DIR}/logs"
mkdir -p "$OUT_DIR"

# Run preparation and YCSB benchmarks
./benchmark_redis_vms.sh "$NUM_VMS" prepare
sleep 3

START_TIME=$(date)
/home/bask_eurosys26/notify_my_slack.sh "$START_TIME"

sudo bash -c "echo 1 > /sys/kernel/mm/ksm/run"

./benchmark_redis_vms.sh "$NUM_VMS" ycsb

END_TIME=$(date)
/home/bask_eurosys26/notify_my_slack.sh "$END_TIME"

# Save KSM Status uniquely
echo "[INFO] Dumping KSM Status"
grep . /sys/kernel/mm/ksm/pages_* | tee "${RUN_DIR}/ksm_status"

echo "[INFO] Stopping VMs..."
sudo pkill qemu

sleep 5

# sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"

/home/bask_eurosys26/notify_my_slack.sh "Workload Done: ${RUN_DIR}"

sshpass ssh bf2 "pkill _server"

echo "🎉 Benchmarks completed! Results saved in: ${RUN_DIR}"
