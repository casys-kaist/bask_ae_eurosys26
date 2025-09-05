#!/bin/bash

USERNAME="ubuntu"
SCRIPT_NAME="turn_off_daemons.sh"
BASE_PORT=23456
#core_index=1  # Core 0 is reserved
core_index=8

declare -a ports
declare -a workloads
declare -a workload_ids
declare -a core_lists
declare -a vm_images
declare -A workload_counts

# === Phase 1: Ordered preparation ===
i=0
for workload in "$@"; do
  config_file="workload_scripts/${workload}/exp_config.sh"
  if [[ ! -f $config_file ]]; then
    echo "[!] Config for workload '$workload' not found."
    exit 1
  fi

  source "$config_file"

  if [[ -z $num_cores || -z $vm_size_MB ]]; then
    echo "[!] Invalid config in $config_file (missing num_cores or vm_size_MB)"
    exit 1
  fi

  # Track duplicate workload instances
  workload_counts["$workload"]=$((workload_counts["$workload"] + 1))
  workload_id="${workload}_${workload_counts["$workload"]}"

  port=$((BASE_PORT + i))
  ports[i]=$port
  workloads[i]=$workload
  workload_ids[i]=$workload_id
  vm_images[i]="../vms/vm$((i+1)).qcow2"

  start_core=$core_index
  end_core=$((core_index + num_cores - 1))
  core_lists[i]=$(seq -s, $start_core $end_core)
  core_index=$((core_index + num_cores))

  ((i++))
done

# === Phase 2: Parallel launching ===
declare -a pids

for j in "${!workload_ids[@]}"; do
  (
    workload="${workloads[j]}"
    workload_id="${workload_ids[j]}"
    port="${ports[j]}"
    img_file="${vm_images[j]}"
    core_list="${core_lists[j]}"

    source "workload_scripts/${workload}/exp_config.sh"

    echo "[*] Launching VM $j ($workload_id) on port $port with cores $core_list, memory ${vm_size_MB}MB"

    sudo taskset -c $core_list qemu-system-x86_64 \
      -m "${vm_size_MB}M" \
      -smp "$num_cores" \
      -enable-kvm \
      -cpu host \
      -drive file="${img_file}",format=qcow2,if=virtio \
      -netdev user,id=net${j},hostfwd=tcp::${port}-:22 \
      -device virtio-net-pci,netdev=net${j} \
      -display none \
      -daemonize

    # Wait for SSH to become fully ready
    echo "  ↳ Waiting for SSH on port $port..."
    timeout 120 bash -c "
      until sshpass ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $port ${USERNAME}@localhost 'exit' 2>/dev/null; do
        sleep 1
      done
    " || {
      echo "  ❌ Timeout waiting for SSH on port $port"
      exit 1
    }

    # Clean up known_hosts entry before upload
    ssh-keygen -R "[localhost]:$port" >/dev/null 2>&1

    # Retry script upload up to 5 times
    upload_success=0
    for attempt in {1..5}; do
      echo "  ↳ Upload attempt $attempt for '$workload_id'..."

      sshpass ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $port ${USERNAME}@localhost "
        cp ./memstrata/workload_scripts/${workload}/* ./
      " && {
        echo "  ✅ Upload succeeded for $workload_id"
        upload_success=1
        break
      }

      sleep 2
    done

    if [[ $upload_success -ne 1 ]]; then
      echo "  ❌ Upload failed for '$workload_id' after 5 attempts"
      exit 1
    fi

    echo "[✓] VM $j ($workload_id) launched and ready."

  ) &
  pids+=($!)
done

# Wait for all launches to finish
for pid in "${pids[@]}"; do
  wait $pid
done

echo "[✔] All VMs launched and initialized successfully."
