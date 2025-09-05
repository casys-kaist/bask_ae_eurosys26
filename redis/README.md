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
   vim <BASK_AE_DIR>/redis/scripts/autorun.sh
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