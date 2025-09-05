#!/bin/bash

set -euo pipefail

# === Config ===
SEED_IMG="../vms/redis_images/seed.img"
BASE_PORT=16379
REDIS_CLI_TIMEOUT=30  # seconds
IMG_DIR="../vms/redis_images"
PID_DIR="redis_vm_pids"

NUM_VMS="${1:-2}"  # default to 2 instances if not specified

# === Function to launch a VM ===
launch_vm() {
  local idx=$1
  local port=$2
  local pidfile="${PID_DIR}/redis-vm${idx}.pid"
  local netid="net${idx}"
  local img="${IMG_DIR}/redis-test-${idx}.img"

  echo "Launching redis-vm${idx} (image: $img, port: $port)..."

  taskset -c $((idx - 1)) sudo qemu-system-x86_64 \
    -m 8192 \
    -smp 1 \
    -enable-kvm \
    -cpu host \
    -drive file="$img",format=qcow2,if=virtio \
    -cdrom "$SEED_IMG" \
    -netdev user,id="$netid",hostfwd=tcp::"$port"-:6379 \
    -device virtio-net-pci,netdev="$netid" \
    -display none \
    -daemonize \
    -pidfile "$pidfile"
}

# === Function to wait for Redis to be ready ===
wait_for_redis() {
  local port=$1
  local label=$2
  echo -n "Waiting for $label on port $port "

  for i in $(seq 1 "$REDIS_CLI_TIMEOUT"); do
    if ./redis-cli -h 127.0.0.1 -p "$port" ping &>/dev/null; then
      echo -e "\n✅ $label is ready!"
      return 0
    fi
    echo -n "."
    sleep 1
  done

  echo -e "\n❌ Timeout: $label did not respond on port $port."
  return 1
}

# === Launch All VMs ===
for i in $(seq 1 "$NUM_VMS"); do
  port=$((BASE_PORT + i - 1))
  launch_vm "$i" "$port"
done

# === Wait for All VMs to Be Ready ===
for i in $(seq 1 "$NUM_VMS"); do
  port=$((BASE_PORT + i - 1))
  wait_for_redis "$port" "redis-vm${i}"
done

echo "🚀 All $NUM_VMS Redis VM instances are up and running!"
