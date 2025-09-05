# EuroSys'26 BASK - Artifact Evaluation

## Abstract

This repository contains the artifact evaluation materials for **BASK: Batch And SmartNIC-offloaded KSM**. We evaluate our claim that BASK successfully preserves high deduplication throughput while reducing application interference caused by KSM. The experiments demonstrate BASK's effectiveness in achieving high memory deduplication rates while maintaining application performance through Smart NIC offloading.

## General Notes

Please note that scripts contain hardcoded absolute paths (`/home/bask_eurosys26/bask_ae_eurosys26`) which need to be updated for your environment before execution.

The DataPlane offloading setup requires GRUB configuration changes and a system reboot. Therefore, we recommend running all other experiments first, then modifying the GRUB configuration to run the DataPlane offloading targets.

## Notes for AE Reviewers

Since running BASK requires a BlueField-2 SmartNIC, we have prepared a pre-configured server for AE. Please contact us if you would like to use it.

All necessary configurations have been completed on the provided server. You can proceed directly to running the experiments without additional setup.

Experiments 1 and 2 include evaluation of 'bask_opt', an optimized and bug-fixed version of BASK that will be incorporated in our paper revisions.

## Testbed Configuration

#### Hardware Configuration

We tested BASK with dual-socket 16-cores Intel Xeon Gold 6326 2.9GHz CPUs with 256GB RAM, NVIDIA BlueField-2 SmartNIC with one 8-cores ARM A72 2.5GHz CPU with 16GB RAM.

**Note**: The full workload setup requires at least 500 GiB of available disk space.

#### Software Configurations

The host server runs Ubuntu 24.04 with a custom kernel based on Linux 6.8.0, and the SmartNIC runs Linux 5.15.0-bluefield. For experiment consistency, we disabled hyper-threading, transparent huge page management, dynamic scaling of CPU frequencies, and all VMs are pinned to specific cores, minimizing variations. Each VM runs Linux 5.4.0.

The testbed supports various KSM configurations including traditional host-based KSM, dataplane optimizations, and our BASK approach with Smart NIC offloading.

## Getting Started

### Prerequisites

#### System Requirements
- Linux environment with KSM support
- Root privileges for KSM configuration and script execution

#### Smart NIC Setup (NVIDIA BlueField-2)
- In-box RDMA driver (not NVIDIA OFED)
- rshim utility installed

**SSH Configuration:**
```bash
# Add to ~/.ssh/config
Host bf2
    HostName 192.168.100.2
    User ubuntu
    Port 22
```

**Enable passwordless SSH:**
```bash
ssh-keygen
ssh-copy-id ubuntu@192.168.100.2
```

#### Root Access Configuration
```bash
sudo visudo
# Add the following line (replace 'your_name' with your username):
your_name ALL=(ALL) NOPASSWD: ALL
```

#### Additional Dependencies
- See individual experiment READMEs for specific requirements

### Build the BASK

1. **Prepare Smart NIC environment:**
   ```bash
   ssh bf2
   mkdir ~/bask_snic
   exit
   ```

2. **Build and deploy BASK code:**
   ```bash
   cd <BASK_AE_DIR>/codes/bask
   make  # This automatically copies contents to bf2
   ```

3. **Complete build on Smart NIC:**
   ```bash
   ssh bf2
   cd ~/bask_snic
   make fast
   ```

### Build the custom kernel

1. **Clone Linux kernel source:**
   ```bash
   cd <BASK_AE_DIR>/codes/
   git clone https://github.com/torvalds/linux.git
   cd linux
   ```

2. **Checkout to Linux v6.8:**
   ```bash
   git checkout v6.8
   ```

3. **Replace mm directory with BASK implementation:**
   ```bash
   rm -rf mm
   ln -s <BASK_AE_DIR>/codes/mm linux/mm
   ```

4. **Build and install custom kernel:**
   ```bash
   make -j32
   sudo make modules_install
   sudo make install
   ```

5. **Reboot and select new kernel:**
   ```bash
   sudo reboot now
   # After reboot, select the new kernel from GRUB menu
   ```

### Build utilities

#### Perf
**Build performance monitoring tool:**
```bash
cd <BASK_AE_DIR>/codes/linux/tools/perf
make
```

#### Hodor
*Note: Hodor integration is planned for future releases.*

### Quick Start

**For AE Reviewers (using provided server):**
1. All configurations are already complete - proceed directly to running experiments
2. Navigate to experiment directories and follow the execution instructions

**For Independent Setup:**
1. **Clone and setup:**
   ```bash
   git clone <BASK_AE_REPO-url>
   cd <BASK_AE_DIR>
   # Update hardcoded paths in scripts to match your environment
   ```

2. **Complete build process:**
   - Follow prerequisites setup above
   - Build BASK, custom kernel, and utilities as detailed in previous sections

3. **Run experiments:**
   ```bash
   # Experiment 1: Cloud workload evaluation
   cd cloud_workloads
   # Follow instructions in cloud_workloads/README.md
   
   # Experiment 2: Redis interference testing
   cd redis
   # Follow instructions in redis/README.md
   
   # Experiment 3: Fault tolerance evaluation
   cd cloud_workloads
   # Follow fault tolerance instructions in cloud_workloads/README.md
   ```

4. **Analyze results:**
   - Check experiment directories for generated logs and performance data
   - Use provided analysis scripts to process results

## Experiments Overview

This artifact evaluation includes three main experiments that comprehensively evaluate BASK's performance, interference characteristics, and fault tolerance capabilities.

### Experiment 1: KSM Benefit Over Time (Figure 5)

**Purpose**: Evaluate whether BASK achieves high scan throughput and reduces scan time to enable fast memory deduplication, supporting faster launch of new VMs or services.

**Setup**: Realistic cloud workloads with mixed applications (GAPBS, DLRM, Redis, Memcached, Faster KV, Spark) running on multiple VMs.

**Key Focus**: Early memory deduplication time - the time required for KSM to deduplicate a certain amount of memory, enabling operators to achieve higher server density.

**Details**: See [Cloud Workloads Experiment](cloud_workloads/README.md)

### Experiment 2: Application Interference (Figure 6)

**Purpose**: Evaluate whether BASK introduces any interference with other applications, focusing on tail latency in latency-sensitive cloud services.

**Setup**: Redis-YCSB workloads with four access patterns (update-heavy, read-heavy, read-only, read-latest). KSM daemon co-runs with Redis servers to measure interference impact.

**Key Focus**: P99.9 tail latency measurement across workloads, representing Service Level Objectives (SLO) for latency-sensitive cloud services.

**Details**: See [Redis Interference Experiment](redis/README.md)

### Experiment 3: Fault Tolerance Evaluation (Figure 7)

**Purpose**: Demonstrate BASK's fault tolerance capabilities when Smart NIC offloading becomes unavailable.

**Setup**: Uses the same cloud workload setup as Experiment 1. KSM server is terminated during execution to emulate Smart NIC failure, testing graceful fallback to host-based deduplication (4000-20ms configuration).

**Key Validation**: BASK retains KSM state locally and continues memory deduplication progress despite Smart NIC interruption, demonstrating fault tolerance without compromising system stability.

**Details**: See [Cloud Workloads Experiment - Fault Tolerance Test](cloud_workloads/README.md#fault-tolerance-test)

## Repository Structure

```
BASK_AE/
├── cloud_workloads/     # Experiment 1: Cloud workload evaluation
├── redis/              # Experiment 2: Redis interference testing  
├── codes/              # BASK implementation and utilities
└── README.md           # This file
```
