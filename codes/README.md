# BASK Code

## Linux mm
### Compilation
See [Getting Started](../README.md#getting-started).

### File Overview
#### Modified from the Linux source
- `Makefile`
- `ksm.c`
  - Modified to support BASK and the DataPlane offloading model.
  - See `ksm_scan_thread` and `ksm_do_scan` for the BASK-Host implementation.

#### New files
- `ksm.h`
  - Defines types used by KSM and BASK-Host.
- `ksm_rdma.c`, `ksm_rdma.h`
  - Implements initialization and communication between BASK-Host and BASK-SNIC.
- `ksm_shadow.c`
  - Implements `shadow_mm` creation and cleanup.
- `time_utils.h`
  - Timing helpers.

## BASK
### Compilation
See [Getting Started](../README.md#getting-started).  
Also compile on the BlueField DPU.

### File Overview
#### Common (Host + SNIC)
- `rdma_common.h`
  - Defines types for communication between BASK-Host and BASK-SNIC (mirrors `ksm_rdma.h`).

#### BASK-Host
- `client_stub.c`, `client_stub.h`
  - Implements RDMA communication for BASK-Host.
- `rdma_stub.c`
  - Provides stub wrappers for kernel-space RDMA APIs.

#### BASK-SNIC
- `server.h`, `server.c`
  - Implements BASK-SNIC and the DataPlane offloading model.
  - See `on_established` for the BASK-SNIC implementation.
  - See `on_established_ops_offload_mode` for the DataPlane offloading implementation.
