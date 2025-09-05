#!/bin/bash

set -euo pipefail

NUM_INSTANCES="${1:-2}"
ACTION="${2:-all}"

BASE_PORT=16379
BASE_SSH_PORT=32222

TOTAL_REQUESTS=4500000
DATA_SIZE=1024
#RECORD_COUNT=100000
RECORD_COUNT=50000

OUT_DIR="${OUT_DIR:-/home/bask_eurosys26/bask_ae_eurosys26/redis/scripts/redis-benchmark-logs}"
mkdir -p "$OUT_DIR"

CLIENT_IMG_BASE="../vms/ycsb_images/ycsb"
SEED_IMG="../vms/ycsb_images/seed.img"

echo "~_~Z~@ Running '$ACTION' for $NUM_INSTANCES instance(s)..."

# Launch YCSB client VMs pinned to separate cores
launch_client_vms() {
  local total_clients=$((NUM_INSTANCES * 3))

  for client_idx in $(seq 0 $((total_clients - 1))); do
    local core=$((NUM_INSTANCES + client_idx))
    local ssh_port=$((BASE_SSH_PORT + client_idx))
    local img="${CLIENT_IMG_BASE}-${client_idx}.img"
    local netid="net${client_idx}"

    echo "🚀 Launching YCSB client VM $client_idx (core $core, ssh port $ssh_port)..."

    taskset -c "$core" sudo qemu-system-x86_64 \
      -m 8192 \
      -smp 1 \
      -enable-kvm \
      -cpu host \
      -drive file="$img",format=qcow2,if=virtio \
      -cdrom "$SEED_IMG" \
      -netdev user,id="$netid",hostfwd=tcp::"$ssh_port"-:22 \
      -device virtio-net-pci,netdev="$netid" \
      -display none \
      -daemonize
  done

  echo "~_~Z~@ Wait VM launch..."
  sleep 15
}


# Prepare Redis with YCSB load
run_prepare_vms() {
  for idx in $(seq 1 "$NUM_INSTANCES"); do
    local ssh_port=$((BASE_SSH_PORT + idx - 1))
    local redis_port=$((BASE_PORT + idx - 1))
    local prefix="${OUT_DIR}/vm${idx}"

    echo "📡 SSH to VM $idx for loading..."

    ssh -o StrictHostKeyChecking=no -p "$ssh_port" ubuntu@127.0.0.1 \
      "cd ycsb_redis && ./bin/ycsb load redis -s -P workloads/workload${YCSB_WORKLOAD:-a} \
       -p redis.host=143.248.136.172 \
       -p redis.port=$redis_port \
       -p recordcount=$RECORD_COUNT" \
       &> "${prefix}-load.log" &
  done
  wait
}

run_ycsb_threads() {
  local total_clients=$((NUM_INSTANCES * 3))

  for client_idx in $(seq 0 $((total_clients - 1))); do
    local ssh_port=$((BASE_SSH_PORT + client_idx))
    local prefix="${OUT_DIR}/client${client_idx}"
    
    # Map each set of 3 clients to one Redis instance
    local redis_idx=$((client_idx / 3))
    local redis_port=$((BASE_PORT + redis_idx))

    echo "📡 SSH into client VM $client_idx (targets Redis port $redis_port)..."

    ssh -o StrictHostKeyChecking=no -p "$ssh_port" ubuntu@127.0.0.1 \
      "cd ycsb_redis && ./bin/ycsb run redis -s -P workloads/workload${YCSB_WORKLOAD:-a} \
       -p redis.host=143.248.136.172 \
       -p redis.port=$redis_port \
       -p recordcount=$RECORD_COUNT \
       -p operationcount=$((TOTAL_REQUESTS / 3))" \
       -p threadcount=1 \
       -p requestdistribution=zipfian \
       -p hdrhistogram.fileoutput=true \
       -p hdrhistogram.output.path=/home/ubuntu/client${client_idx}- \
       &> "${prefix}-bench.log" &
  done

  wait

  echo "~_~S Getting histogram result from clients..."

  for client_idx in $(seq 0 $((total_clients - 1))); do
    local ssh_port=$((BASE_SSH_PORT + client_idx))
    local prefix="${OUT_DIR}/client${client_idx}"

    ssh -p "$ssh_port" ubuntu@127.0.0.1 "tar -cf - client*.hdr" | tar -xf - -C ${OUT_DIR}/
  done 
}


# Run Redis native benchmark (localhost only)
benchmark_instance() {
  local idx=$1
  local port=$((BASE_PORT + idx - 1))
  local prefix="${OUT_DIR}/vm${idx}"

  echo "⚙️ redis-vm${idx}: SET benchmark starting..."
  ./redis-benchmark -n "$TOTAL_REQUESTS" -h 127.0.0.1 -p "$port" -t Set -d "$DATA_SIZE" > "${prefix}-set.log"

  echo "⚙️ redis-vm${idx}: GET benchmark starting..."
  ./redis-benchmark -n "$TOTAL_REQUESTS" -h 127.0.0.1 -p "$port" -t Get -d "$DATA_SIZE" > "${prefix}-get.log"
}

run_native_benchmark() {
  for i in $(seq 1 "$NUM_INSTANCES"); do
    benchmark_instance "$i" &
  done
  wait
}

# Redis FLUSHALL per instance
drop_ycsb() {
  local idx=$1
  local port=$((BASE_PORT + idx - 1))
  ./redis-cli -h 127.0.0.1 -p "$port" FLUSHALL
  echo "🧹 redis-vm${idx}: All keys dropped."
}

run_flush() {
  for i in $(seq 1 "$NUM_INSTANCES"); do
    drop_ycsb "$i" &
  done
  wait
}

# Dispatch actions
case "$ACTION" in
  prepare)
    launch_client_vms
    # sudo bash -c "echo 1 > /sys/kernel/mm/ksm/run"
    run_prepare_vms
    ;;
  ycsb)
    run_ycsb_threads
    ;;
  flush)
    run_flush
    ;;
  native)
    run_native_benchmark
    ;;
  all)
    launch_client_vms
    run_prepare_vms
    run_ycsb_threads
    run_flush
    run_native_benchmark
    ;;
  *)
    echo "❌ Unknown action: $ACTION"
    echo "Usage: $0 <num_instances> [prepare|ycsb|flush|native|all]"
    exit 1
    ;;
esac

echo "✅ All tasks completed. Logs saved in: $OUT_DIR"

