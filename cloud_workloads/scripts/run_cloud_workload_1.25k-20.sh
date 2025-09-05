#!/usr/bin/env bash

#set -euo pipefail

trial="${1:-}"
if [[ -z "$trial" ]]; then
  echo "Usage: $0 <trial_id>" >&2
  exit 1
fi

bench_target="linux"
bench_option=""

export MY_CLOUD_TRIAL=${bench_target}_${trial}

echo "Trial ${bench_target}_${trial}"

# Make sure sudo is ready to go (will prompt once if needed)
sudo -v

# === Step 1: Init part ===
# Pin ksmd (don’t abort if not found)
if pid=$(pgrep -x ksmd || true); then
  sudo taskset -cp 8-31 "$pid" || true
fi

# Network config (idempotent)
sudo ip addr add 10.0.25.10/24 dev ens85f0np0 2>/dev/null || true
sudo ip link set ens85f0np0 up

# KSM tuning
echo 0    | sudo tee /sys/kernel/mm/ksm/run >/dev/null
echo 1250 | sudo tee /sys/kernel/mm/ksm/pages_to_scan >/dev/null
echo 20   | sudo tee /sys/kernel/mm/ksm/sleep_millisecs >/dev/null

# Remote server (key-based auth)

#ssh bf2 "pkill _server"
#ssh bf2 "pkill -f pcie_bw_mon"
#ssh bf2 "nohup ./bask_snic/bask_server ${bench_option} > bask_snic_log_${trial} 2>&1 &"

# Kernel module (ignore error if already loaded)
sudo insmod /home/bask_eurosys26/bask_ae_eurosys26/codes/bask/no_client_bridge.ko debug=0 || true

sleep 30


# === Step 2: Prepare logging (background) ===
# Run dmesg -w as root, launched by root into background (no TTY issues)
sudo bash -lc 'nohup dmesg -w > "logs/kernel_log_'"$trial"'" 2>&1 &'

# === Step 3: Run workload ===
# Start monitor in background (prints not needed in foreground)
#sudo taskset -c 2-7 bash -lc 'source /home/cskwak99/.venv/bin/activate && python ./monitor_mem_ksm_perf.py' &
sudo env MY_CLOUD_TRIAL="${bench_target}_${trial}" \
  taskset -c 2-7 bash -lc 'python ./monitor_mem_ksm_perf.py' &

# Launch VMs
./launch_vms.sh \
  bfs-web memcached_uniform_ycsb_a faster_ycsb_a dlrm_rm2_1_low redis_ycsb_a spark_terasort \
  bfs-web memcached_uniform_ycsb_a faster_ycsb_a dlrm_rm2_1_low redis_ycsb_a spark_terasort

# Benchmarks (background, like your original)
./bench.sh \
  bfs-web memcached_uniform_ycsb_a faster_ycsb_a dlrm_rm2_1_low redis_ycsb_a spark_terasort \
  bfs-web memcached_uniform_ycsb_a faster_ycsb_a dlrm_rm2_1_low redis_ycsb_a spark_terasort &

sleep 1

# === Step 4: Start benchmark + logging ===
# Schedule cleanup + reboot as a root-owned background job (works without TTY)
#sudo bash -lc 'nohup bash -c "
#  sleep 2600;
#  ./stop_monitor.sh || true;
#  echo stop > /tmp/ram_monitor_pipe || true;
#  pkill qemu || true;
#  pkill dmesg || true;
#  echo "Wait before reboot" || true;
#  sleep 60;
#  reboot now;
#" >/dev/null 2>&1 &'

# Explicitly turn KSM on now
echo 1 | sudo tee /sys/kernel/mm/ksm/run >/dev/null

# For fault test
#nohup bash -c "
#  sleep 800
#  ssh bf2 'pkill _server' || true
#" >/dev/null 2>&1 &


# Start remote PCIe BW monitor
#ssh bf2 "nohup ./bask_snic/pcie_bw_mon.sh > pcie_bw_log_${trial} 2>&1 &"
#ssh bf2 "nohup pidstat -r -u -p \$(pgrep -x bask_server) 1 2600 > bask_snic_pidstat_${trial} 2>&1 &"

# perf counters for 2600s (stderr -> CSV)
sudo /home/bask_eurosys26/bask_ae_eurosys26/codes/linux/tools/perf/perf stat -C 8-31 -I 1000 --no-big-num \
  -e LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses \
  -- sleep 2600 2> "logs/perf_log_ksm_${trial}.csv" &

# After 2600s, stop remote, fetch logs (background)
#nohup bash -c "
# sleep 2610
#  ssh bf2 'pkill _server' || true
#  ssh bf2 'pkill -f pcie_bw_mon' || true
#  scp bf2:/home/ubuntu/pcie_bw_log_${trial} logs
#  scp bf2:/home/ubuntu/bask_snic_log_${trial} logs
#  scp bf2:/home/ubuntu/bask_snic_pidstat_${trial} logs
#" >/dev/null 2>&1 &

echo "Launched all background tasks for trial ${trial}."

sleep 2610;
sudo ./stop_monitor.sh &
sudo bash -c "echo stop > /tmp/ram_monitor_pipe" &
# echo 0 | sudo tee /sys/kernel/mm/ksm/run >/dev/null
sudo pkill qemu &

sleep 10

sudo pkill dmesg &
echo "Wait before reboot"

/home/bask_eurosys26/notify_my_slack.sh "Wait before rboot"

sleep 60;
sudo reboot now;



