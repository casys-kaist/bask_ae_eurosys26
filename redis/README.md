# Redis Interference Experiment - Application Interference

## Overview

### Purpose
This experiment evaluates whether BASK introduces any interference with other applications, specifically focusing on tail latency impacts in latency-sensitive cloud services. The experiment measures P99.9 tail latency across different YCSB workload patterns while KSM daemons co-run with Redis servers.

### Document Structure
This README is organized into three main sections:
- **Experiment Setup**: Configuration of VMs, workloads, and baseline KSM methods
- **Run**: Step-by-step execution instructions for running the interference measurement
- **Artifact Generation**: Steps to generate figures and tables from the paper

---

## Experiment Setup

### VM Configuration
- **Setup**: Eight VM sets, each with 3 YCSB client VMs + 1 Redis server VM
- **Resources**: Single vCPU and 8 GiB memory per VM
- **Core Pinning**: Each VM pinned to dedicated physical core for consistent performance
- **Interference Model**: KSM daemon co-runs with Redis servers to measure interference

### YCSB Workload Types
Four workload patterns representing common cloud access patterns:

1. **Update-heavy**: 50% read, 50% update operations
2. **Read-heavy**: 95% read, 5% update operations  
3. **Read-only**: 100% read operations
4. **Read-latest**: 95% read, 5% insert operations

### Baselines Evaluated
- **no_ksm**: No deduplication service (interference baseline)
- **4k-20**: Standard KSM with 4000 pages per scan, 20ms sleep
- **1.25k-20**: Standard KSM with 1250 pages per scan, 20ms sleep  
- **dataplane**: SNIC-based offloading with RDMA communication
- **BASK**: Smart NIC-based approach with control plane offloading
- **BASK_OPT**: Optimized and bug-fixed version of BASK

### Service Configuration
Register the automated benchmark service:

```bash
sudo vim /etc/systemd/system/bask_redis_bench.service
```

```ini
[Unit]
Description=Auto Redis VM Benchmark Service for Bask
After=network.target

[Service]
Type=simple
User=<user_name>
ExecStart=<BASK_AE_DIR>/redis/scripts/auto_run.sh
Restart=no
WorkingDirectory=<BASK_AE_RID>/redis/scripts
Environment=HOME=/home/<user_name>
Environment=SHELL=/bin/bash

[Install]
WantedBy=multi-user.target
```

---

## Run

### Phase 1: Standard Baselines Including BASK Variants (18 hours)

1. **Enable and start the benchmark service:**
   ```bash
   sudo systemctl enable bask_redis_bench.service
   sudo reboot now
   ```

2. **Monitor progress:**
   - The benchmark will automatically run for: no_ksm, 4k-20, 1.25k-20, bask, bask_opt
   - DataPlane baseline requires separate configuration (Phase 2)
   - Check completion by looking for `redis_benchmark_done` file in \$HOME directory
   - Total runtime: approximately 18 hours

### Phase 2: DataPlane Baseline (4 hours)

3. **Configure GRUB for DataPlane:**
   ```bash
   sudo vim /etc/default/grub
   # Comment out: GRUB_CMDLINE_LINUX_DEFAULT=""
   # Comment in: GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=off"
   sudo update-grub
   ```

4. **Update benchmark script:**
   ```bash
   vim <BASK_AE_DIR>/redis/scripts/auto_run.sh
   # Comment out: CONFIG_NAMES=("no_ksm" "4k-20" "1.25k-20" "bask" "bask_opt")
   # Comment in: CONFIG_NAMES=("dataplane")
   ```

5. **Run DataPlane benchmark:**
   ```bash
   rm ~/redis_benchmark_done  # Remove completion flag
   sudo reboot now            # Restart to run DataPlane
   ```
   - Wait for `redis_benchmark_done` file to appear again (~4 hours)

### Phase 3: Cleanup for Other Experiments

6. **Disable service and restore GRUB configuration:**
   ```bash
   sudo systemctl disable bask_redis_bench.service
   
   sudo vim /etc/default/grub
   # Comment out: GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=off"
   # Comment in: GRUB_CMDLINE_LINUX_DEFAULT=""
   sudo update-grub
   ```
   - This prepares the system for running other experiments

## **!!! TROUBLESHOOTING !!!**
- **Benchmark hanging**: Check kernel logs with `sudo dmesg -w` for kernel panics
- **External reboot needed**: For AE reviewers, contact me if system becomes unresponsive
- **Progress monitoring**: Check for new files in `<BASK_AE_DIR>/redis/scripts/redis-benchmark-results/`
---

## Artifact Generation

### Aggregate Latency Distributions

```bash
cd <BASK_AEDIR>/redis/scripts # Contains redis-benchmark-results/
```

```bash
./union_all_ops.sh no_ksm_a no_ksm_b no_ksm_c no_ksm_d
./union_all_ops.sh 1.25k-20_a 1.25k-20_b 1.25k-20_c 1.25k-20_d
./union_all_ops.sh 4k-20_a 4k-20_b 4k-20_c 4k-20_d
./union_all_ops.sh dataplane_a dataplane_b dataplane_c dataplane_d
./union_all_ops.sh bask_a bask_b bask_c bask_d
./union_all_ops.sh bask_opt_a bask_opt_b bask_opt_c bask_opt_d
```

### Generate Summary Statistics

```bash
python summarize_union.py
```
**Output**: `all_union_summaries.csv`
### Figure 6 - P99.9 Tail Latency

```bash
python plot_redis_ycsb_two_border.py \
  --csv all_union_summaries.csv \
  --metric p99.9 \
  --output_pdf redis_p99.9.pdf \
  --fontsize_title 17 \
  --fontsize_ylabel 17 \
  --fontsize_xticklabel 15 \
  --fontsize_workloadlabel 16 \
  --fontsize_legend 15 \
  --fontsize_yticklabel 17
```
**Output**: `redis_p99.9.pdf`
### Baseline Comparison

```bash
python compare_redis_tail.py no_ksm 1.25k-20
python compare_redis_tail.py no_ksm 4k-20
python compare_redis_tail.py no_ksm dataplane
python compare_redis_tail.py no_ksm bask
python compare_redis_tail.py no_ksm bask_opt
```

## VM configuration

**Note for AE Reviewers**: VM images are pre-configured and ready to use. You do not need to setup VMs manually. 

- **!!! Do not configure VMs on the provided server. !!!**

### Conventions

**For Manual VM Setup** (if needed):

Replace placeholders like <BASK_AE_DIR> and <REDIS_HOST_IP> with your actual paths/addresses.

Unless noted as “inside the VM,” run commands on the host.

### Redis VM
   1. Build `redis-cli` on the host
      ```
      git clone https://github.com/redis/redis.git
      cd redis
      make -j "$(nproc)" all
      cp src/redis-cli <BASK_AE_EIR>/redis/scripts
      ```
   2. Download Ubuntu 22.04 cloud image and rename
      ```
      cd <BASK_AE_DIR>/redis/vms/redis_images
      # Download Ubuntu 22.04 cloud image (e.g., jammy-server-cloudimg-amd64.img)
      mv <downloaded-file> redis-test-0.img
      ```
   3. Create seed.img
      ```
      cd <BASK_AE_DIR>/redis/vms/redis_images/cloud-init-data
      cloud-localds seed.img user-data meta-data
      mv seed.img ..
      ```
   4. Test-run `redis-vm`
      - Launch
         ```
         cd <BASK_AE_DIR>/redis/vms/redis_images
         sudo qemu-system-x86_64 \
            -m 8192 \
            -smp 1 \
            -enable-kvm \
            -cpu host \
            -drive file=redis-test-0.img,format=qcow2,if=virtio \
            -cdrom seed.img \
            -netdev user,id=net0,hostfwd=tcp::16379-:6379 \
            -device virtio-net-pci,netdev=net0 \
            -display none \
            -daemonize
         ```
      - Test
         ```
         cd <BASK_AE_DIR>/redis/scripts
         ./redis-cli -h 127.0.0.1 -p 16379 ping
         # Expected: PONG
         ```
   5. Create additional Redis VM images
      ```
      for i in {1..7}; do cp redis-test-0.img redis-test-$i.img; done
      ```
### YCSB VM
   1. Prepare VM cloud image with size 6 GiB named `ycsb-0.img`
         ```
         cd <BASK_AE_DIR>/redis/vms/ycsb_images
         # Download Ubuntu 22.04 cloud image (e.g., jammy-server-cloudimg-amd64.img)
         mv <downloaded-file> ycsb-0.img

         cd cloud-init-data
         cloud-localds seed.img user-data meta-data
         mv seed.img ..
         ```
      - resize
         ```
         cd <BASK_AE_DIR>/redis/vms/ycsb_images
         qemu-img resize ycsb-0.img +4G
         ```
   2. Launch YCSB VM and enable passwordless SSH
      ```
      cd <BASK_AE_DIR>/redis/vms/ycsb_images
      sudo qemu-system-x86_64 \
         -m 8192 \
         -smp 1 \
         -enable-kvm \
         -cpu host \
         -drive file=ycsb-0.img,format=qcow2,if=virtio \
         -cdrom seed.img \
         -netdev user,id=net1,hostfwd=tcp::32222-:22 \
         -device virtio-net-pci,netdev=net1 \
         -display none \
         -daemonize
      ssh-copy-id -p 32222 ubuntu@127.0.0.1
      ssh -p 32222 ubuntu@127.0.0.1
      ```
   3. Install and Build YCSB inside the VM
      ```bash
      sudo apt-get update
      sudo apt-get install git java maven python python-is-python3
      git clone https://github.com/brianfrankcooper/YCSB.git
      cd YCSB
      mvn -pl site.ycsb:redis-binding -am clean package
      # Required to avoid rebuilding YCSB for every benchmark
      tar -xvzf ~/YCSB/redis/target/ycsb-redis-binding-0.18.0-SNAPSHOT.tar.gz
      mv ~/ycsb-redis-binding-0.18.0-SNAPSHOT ~/ycsb_redis
      ```
   4. Test YCSB inside the VM
      - Ensure a Redis VM is running (see Redis section above).
      - Test
         ```
         cd ~/ycsb_redis
         ./bin/ycsb load redis -s -P workloads/workloada \
            -p redis.host=<REDIS_HOST_IP> \
            -p redis.port=16379 \
            -p recordcount=10000
         # Tip: use the host's reachable IP for <REDIS_HOST_IP>. Localhost will not work.
         ```
   5. Create additional YCSB VM images
      ```
      for i in {1..23}; do cp ycsb-0.img ycsb-$i.img; done
      ```