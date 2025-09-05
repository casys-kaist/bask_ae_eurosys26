# Cloud Workloads Experiment - KSM Benefit Over Time

## Overview

### Purpose
This experiment evaluates whether BASK achieves high scan throughput and reduces scan time to enable fast memory deduplication, supporting faster launch of new VMs or services. It measures the effective throughput and memory savings of different KSM configurations over time.

### Document Structure
This README is organized into three main sections:
- **Experiment Setup**: Configuration of VMs, workloads, and baseline KSM methods
- **Run**: Step-by-step execution instructions for running the KSM benefit evaluation
- **Artifact Generation**: Steps to generate figures and tables from the paper

---

## Experiment Setup

### Cloud Workload Configuration

We launch realistic cloud workloads with mixed applications including GAP Benchmark Suite (GAPBS), DLRM, Redis, Memcached, Faster KV with YCSB, and Spark Terasort. Two VM sets are instantiated containing:

- **4-vCPU GAPBS bfs-web**: Graph analytics workload
- **2-vCPU redis-ycsb bench**: In-memory database with YCSB benchmark
- **1-vCPU faster-YCSB bench**: Fast key-value store with YCSB
- **1-vCPU dlrm workload**: Deep learning recommendation model
- **2-vCPU Memcached bench**: Distributed memory caching system
- **2-vCPU spark-terasort**: Big data processing workload

### Baselines Evaluated
- **no_ksm**: No active deduplication service (baseline)
- **4k-20**: Standard KSM with 4000 pages per scan, 20ms sleep
- **1.25k-20**: Standard KSM with 1250 pages per scan, 20ms sleep
- **dataplane**: SNIC-based offloading with RDMA communication
- **bask**: Smart NIC-based approach with batch processing
- **bask_opt**: Optimized and bug-fixed version of BASK


### VM Configuration

**Note for AE Reviewers**: VM images are pre-configured and ready to use. You do not need to setup VMs manually. 

- **!!! Do not configure VMs on the provided server. !!!**

**For Manual VM Setup** (if needed):

#### VM Image Generation Steps

1. **Generate SSH key for VM access:**
   ```bash
   sudo ssh-keygen -t ed25519 -f /root/.ssh/id_test123 -N ""
   ```

2. **Create base VM image:**
   ```bash
   cd <BASK_AE_DIR>/cloud_workloads/vms
   ../scripts/conf_scripts/create.sh
   # This command also starts the VM
   ```

3. **Install workloads in VM:**
   ```bash
   # In host, check VM IP address:
   ip neigh show dev virbr0
   
   # Inside VM, install workloads:
    cd <BASK_AE_DIR>/cloud_workloads/conf_scripts
   ./install_workloads.sh
   
   # Shutdown VM when setup is complete
   ```
   **Note**: Installing workloads may take up to 1 day due to downloading large workload files.

4. **Clone VM images with Copy-on-Write:**
   ```bash
   cd <BASK_AE_DIR>/cloud_workloads/vms
   sudo cp /var/lib/libvirt/images/vm1/vm1.qcow2 ./base.qcow2
   sudo chown <user_name>:<user_name> ./base.qcow2
   ../scripts/conf_scripts/make_clone.sh
   ```

**Important**: With manual VM setup, some scripts may need modification for SSH keys, directories, and workload_script paths to match your configuration.

---

## Run

### Reboot Before Each Run
```bash
sudo reboot now
```
**Important**: Reboot the system before running each baseline for consistent results.

**Note**: Each run takes approximately 1 hour.

### 1.25k-20 Baseline
```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
./run_cloud_workload_1.25k-20.sh 1.25k-20
```

### 4k-20 Baseline
```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
./run_cloud_workload_4k-20.sh 4k-20
```

### BASK Baseline
```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
./run_cloud_workload_bask.sh bask
```

### BASK_OPT Baseline
```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
./run_cloud_workload_bask_opt.sh opt
```

### Fault Tolerance Test
```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
./run_cloud_workload_bask_fault.sh fault
```

### DataPlane Baseline

1. **Configure GRUB for DataPlane:**
   ```bash
   sudo vim /etc/default/grub
   # Comment out: GRUB_CMDLINE_LINUX_DEFAULT=""
   # Comment in: GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=off"
   sudo update-grub
   sudo reboot now
   ```

2. **Run DataPlane workload:**
   ```bash
   cd <BASK_AE_DIR>/cloud_workloads/scripts
   ./run_cloud_workload_dataplane.sh dataplane
   ```

3. **Restore GRUB configuration:**
   ```bash
   sudo vim /etc/default/grub
   # Comment out: GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=off"
   # Comment in: GRUB_CMDLINE_LINUX_DEFAULT=""
   sudo update-grub
   sudo reboot now
   ```
---

## Artifact Generation

### Figure 5 - Memory Sharing Over Time

```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
python plot_mem_ksm_sharing.py \
  raw_linux_1.25k-20_1 \
  raw_linux_4k-20_1 \
  raw_dataplane_dataplane_1 \
  raw_bask_bask_1/ \
  raw_bask_opt_opt_1/ \
  --limit 2500 \
  --mark 32 \
  --labels 1250-20ms 4000-20ms DataPlane BASK BASK-OPT
```
**Output**: `summary.pdf`
### Table 3 - KSM Throughput Statistics

```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
python calc_ksm_thr.py \
  raw_linux_1.25k-20_1 \
  raw_linux_4k-20_1 \
  raw_dataplane_dataplane_1 \
  raw_bask_bask_1 \
  raw_bask_opt_opt_1 \
  --limit 2500
```

### CPU Performance Comparison

```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
python plot_ksm_cpu_perf.py \
  --limit 2500 \
  raw_linux_1.25k-20_1 \
  raw_linux_4k-20_1 \
  raw_dataplane_dataplane_1 \
  raw_bask_bask_1 \
  raw_bask_opt_opt_1
```

### Figure 7 - Fault Tolerance

```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
python plot_failure_one.py \
  raw_linux_4k-20_1/ \
  raw_bask_fault_1/ \
  --limit 2500 \
  --labels 4000-20ms bask_fault \
  --mark-at bask_fault 360 \
  --mark 32
```
**Output**: `fault.pdf`

### Table 5 - Transaction Fial Rate

```bash
cd <BASK_AE_DIR>/cloud_workloads/scripts
python parse_failure_statistics.py \
  logs/kernel_log_opt
```

