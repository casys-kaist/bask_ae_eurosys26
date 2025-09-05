#!/bin/bash

# Fancy Redis VM Benchmark Script
# Runs Redis VMs benchmark with and without KSM enabled

# Disable KSM initially
sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
echo "[INFO] KSM disabled"

# Define number of VMs
NUM_VMS=8

# Number of iterations
ITERATIONS=5

reset_state() {
  echo -e "${CYAN} Try reset machine state ${NC}"
  sudo bash -c "echo 3 > /proc/sys/vm/drop_caches"
  sudo bash -c "echo 1 > /proc/sys/vm/compact_memory"
  sleep 5
}

reset_state

#for iter in $(seq 0 $((ITERATIONS - 1))); do
for iter in {0..4}; do
    echo "============================"
    echo "🔁 Iteration $iter (KSM OFF)"
    echo "============================"

#    ./launch_redis_vms.sh $NUM_VMS
#    ./benchmark_redis_vms.sh $NUM_VMS prepare
#    sleep 3
#    ./benchmark_redis_vms.sh $NUM_VMS ycsb

#    ./union_all_ops.sh $iter

    echo "[INFO] Stopping VMs..."
#    sudo pkill qemu

#    sleep 5
    
    reset_state

    echo "============================"
    echo "⚙️ Iteration $iter (KSM ON)"
    echo "============================"

    ./launch_redis_vms.sh $NUM_VMS
    ./benchmark_redis_vms.sh $NUM_VMS prepare
    sleep 3

    echo "[INFO] KSM enabled"
    sudo bash -c "echo 1 > /sys/kernel/mm/ksm/run"

    ./benchmark_redis_vms.sh $NUM_VMS ycsb
    
    echo "[INFO] Dump KSM Status"
    grep . /sys/kernel/mm/ksm/pages_* | tee ksm_status_$iter

   ./union_all_ops.sh ksm $iter

    echo "[INFO] Stopping VMs..."
    sudo pkill qemu
    sleep 5

    # Disable KSM again for next round
    # sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
    echo "[INFO] KSM disabled for next iteration"

    echo "✅ Iteration $iter complete"
    echo ""
    reset_state
done

echo "🎉 All benchmarks completed!"

