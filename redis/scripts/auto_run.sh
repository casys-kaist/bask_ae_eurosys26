#!/bin/bash

# Auto Redis VM Benchmark Runner with Multiple Configurations
# Must run as root or with sudo privileges due to reboot commands

WORKLOADS=("a" "b" "c" "d")
#WORKLOADS=("a")
ITERATIONS=5

CONFIG_NAMES=("no_ksm" "4k-20" "1.25k-20" "bask" "bask_old")
#CONFIG_NAMES=("dataplane")
TOTAL_CONFIGS=${#CONFIG_NAMES[@]}

REDIS_VM_DIR="$HOME/bask_ae_eurosys26/redis/scripts/"
PROGRESS_FILE="$HOME/.benchmark_progress"

initialize_environment() {
  sudo /home/bask_eurosys26/bask_ae_eurosys26/redis/scripts/turn_off_daemons.sh

  # Pin ksmd thread to cores 0-7
  KSM_PID=$(pgrep ksmd)
  if [ -n "$KSM_PID" ]; then
    sudo taskset -cp 0-7 "$KSM_PID"
    echo "Pinned ksmd (PID $KSM_PID) to cores 0-7"
  else
    echo "ksmd process not found!"
  fi
}

configure_ksm() {
  local config_name=$1

  case $config_name in
    no_ksm)
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
      ;;
    4k-20)
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
      sudo bash -c "echo 20 > /sys/kernel/mm/ksm/sleep_millisecs"
      sudo bash -c "echo 4000 > /sys/kernel/mm/ksm/pages_to_scan"
      sudo insmod /home/bask_eurosys26/bask_ae_eurosys26/codes/bask/no_client_bridge.ko debug=0
      ;;
    1.25k-20)
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
      sudo bash -c "echo 20 > /sys/kernel/mm/ksm/sleep_millisecs"
      sudo bash -c "echo 1250 > /sys/kernel/mm/ksm/pages_to_scan"
      sudo insmod /home/bask_eurosys26/bask_ae_eurosys26/codes/bask/no_client_bridge.ko debug=0
      ;;
    dataplane)
      sudo systemctl restart rshim
      sudo ifconfig ens85f0np0 10.0.25.10/24 up      
      sleep 30
      sshpass ssh bf2 "pkill _server"
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/sleep_millisecs"
      sudo bash -c "echo 4000 > /sys/kernel/mm/ksm/pages_to_scan"
      sudo insmod ~/bask_ae_eurosys26/codes/bask/dataplane_client_bridge.ko debug=0
      sshpass ssh bf2 "nohup ./bask_snic/bask_server debug=0 dataplane > /dev/null 2>&1 &"
	
      if nc -zvw 5 192.168.100.2 22 2>/dev/null; then
          echo "BF2 working well"
      else
          /home/bask_eurosys26/notify_my_slack.sh "BF2 not responding"
      fi

      sleep 30

      sudo systemctl stop rshim.service

      ;;
    bask_opt)
      sudo systemctl restart rshim
      sudo ifconfig ens85f0np0 10.0.25.10/24 up
      sleep 30
      sshpass ssh bf2 "pkill _server"
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/sleep_millisecs"
      sudo bash -c "echo 4000 > /sys/kernel/mm/ksm/pages_to_scan"
      sudo insmod ~/bask_ae_eurosys26/codes/bask/bask_client_bridge.ko debug=0
      sshpass ssh bf2 "nohup ./bask_snic/bask_server debug=0 > /dev/null 2>&1 &"      

      if nc -zvw 5 192.168.100.2 22 2>/dev/null; then
          echo "BF2 working well"
      else
          /home/bask_eurosys26/notify_my_slack.sh "BF2 not responding"
      fi

      sleep 30

      sudo systemctl stop rshim.service

      ;;
    bask)
      sudo systemctl restart rshim
      sudo ifconfig ens85f0np0 10.0.25.10/24 up
      sleep 30
      sshpass ssh bf2 "pkill _server"
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/run"
      sudo bash -c "echo 0 > /sys/kernel/mm/ksm/sleep_millisecs"
      sudo bash -c "echo 4000 > /sys/kernel/mm/ksm/pages_to_scan"
      sudo insmod ~/bask_ae_eurosys26/codes/bask/bask_client_bridge.ko debug=0
      sshpass ssh bf2 "nohup ./bask_snic/bask_server old > /dev/null 2>&1 &"

      if nc -zvw 5 192.168.100.2 22 2>/dev/null; then
          echo "BF2 working well"
      else
          /home/bask_eurosys26/notify_my_slack.sh "BF2 not responding"
      fi

      sleep 30

      sudo systemctl stop rshim.service

      ;;

  esac

  sleep 60 # Required to wait Server connection and status set for all case

  OFFLOAD_MODE=$(sudo dmesg | grep "mode is decided")
  /home/bask_eurosys26/notify_my_slack.sh "$OFFLOAD_MODE"
}

enable_ksm_if_needed() {
  local config_name=$1


  if [ "$config_name" != "no_ksm" ]; then
    sudo bash -c "echo 1 > /sys/kernel/mm/ksm/run"
  fi
}

run_benchmark() {
  local config=$1
  local workload=$2
  local iteration=$3

  export YCSB_WORKLOAD="$workload"
  
  cd "$REDIS_VM_DIR"
  ./run.sh "${config}_${workload}_${iteration}"
}

save_progress() {
  echo "$CURRENT_CONFIG $CURRENT_WORKLOAD $CURRENT_ITERATION" > "$PROGRESS_FILE"
}

load_progress() {
  if [ -f "$PROGRESS_FILE" ]; then
    read -r CURRENT_CONFIG CURRENT_WORKLOAD CURRENT_ITERATION < "$PROGRESS_FILE"
  else
    CURRENT_CONFIG=0
    CURRENT_WORKLOAD=0
    CURRENT_ITERATION=0
  fi
}

next_run() {
  if [ "$CURRENT_ITERATION" -lt $((ITERATIONS - 1)) ]; then
    CURRENT_ITERATION=$((CURRENT_ITERATION + 1))
  else
    CURRENT_ITERATION=0
    if [ "$CURRENT_WORKLOAD" -lt $((${#WORKLOADS[@]} - 1)) ]; then
      CURRENT_WORKLOAD=$((CURRENT_WORKLOAD + 1))
    else
      CURRENT_WORKLOAD=0
      CURRENT_CONFIG=$((CURRENT_CONFIG + 1))
    fi
  fi
}

main() {
  load_progress
 
  /home/bask_eurosys26/notify_my_slack.sh "Boot"

  if [ "$CURRENT_CONFIG" -ge "$TOTAL_CONFIGS" ]; then
    echo "✅ All benchmarks completed."
    rm -f "$PROGRESS_FILE"
    touch /home/bask_eurosys26/redis_bench_all_done
    
    /home/bask_eurosys26/notify_my_slack.sh "Redis All done"
    exit 0
  fi

  CURRENT_CONFIG_NAME=${CONFIG_NAMES[$CURRENT_CONFIG]}
  CURRENT_WORKLOAD_NAME=${WORKLOADS[$CURRENT_WORKLOAD]}

  export YCSB_WORKLOAD=$CURRENT_WORKLOAD_NAME

  echo "🚩 Current run: [$CURRENT_CONFIG_NAME] workload [$CURRENT_WORKLOAD_NAME] iteration [$((CURRENT_ITERATION + 1))/$ITERATIONS]"

  initialize_environment
  configure_ksm "$CURRENT_CONFIG_NAME"
#  enable_ksm_if_needed "$CURRENT_CONFIG_NAME"

  sudo dmesg -w > kernel_logs/ksm_log_${CURRENT_CONFIG_NAME}_${CURRENT_WORKLOAD_NAME}_${CURRENT_ITERATION} &

  run_benchmark "$CURRENT_CONFIG_NAME" "$CURRENT_WORKLOAD_NAME" "$CURRENT_ITERATION"

  next_run
  save_progress

  sudo systemctl start rshim.service
  sshpass ssh bf2 "pkill _server"

  sleep 5

  echo "🔄 Rebooting for next benchmark run..."
  /home/bask_eurosys26/notify_my_slack.sh "[$CURRENT_CONFIG_NAME] [$CURRENT_WORKLOAD_NAME] [$CURRENT_ITERATION] Done"
  sudo reboot
}

main
