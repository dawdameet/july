# July Remote Compute Engine: Architecture, Protocols & Multi-Tenant Benchmark Report

This document records the architectural design, wire protocols, storage mechanics, and real-world performance benchmarks of the **July High-Performance Remote Compute Engine** developed and tested across a macOS client (M-series ARM64) and a Fedora Linux compute node (x86_64 Intel Core i3-1005G1 @ 1.20GHz).

---

## 1. High-Level Architecture Overview

July is an ambient compute engine designed to offload arbitrary compilation, compute, and data analysis workloads from a developer workstation (Mac) to a networked secondary machine (Linux) with near-zero latency overhead.

```
Mac Client (Workstation)                        Linux Remote Compute Host (Fedora x86_64)
┌───────────────────────────────┐               ┌────────────────────────────────────────┐
│  dispatcher CLI / Makefile    │               │  epoll Event Loop (Port 8080)          │
│  Binary Container Packer      │──POST /wf────►│  Worker Thread Pool (NUM_WORKERS 4)    │
│  (Header: 0x4A554C59 'JULY')  │               │  RAM Sandboxes (/tmp/july_runs/<id>/)  │
│  Live Log & sendfile Receiver │◄──Logs/Files──│  Kernel OS Page Cache + sendfile() DMA │
└───────────────────────────────┘               └────────────────────────────────────────┘
```

---

## 2. Wire Protocol: `JULY` Binary Container Envelope

Traditional tools introduce massive delays by executing `tar -czf`, sending gzipped streams, and running `tar -xzf` through Docker daemons or SSH layers. July eliminates this with a zero-compression, memory-aligned binary envelope:

### Envelope Binary Layout

| Byte Offset | Field | Type | Description |
| :--- | :--- | :--- | :--- |
| `0x00 - 0x03` | Magic | `char[4]` | Constant `"JULY"` (`0x4A554C59`) |
| `0x04 - 0x05` | Version | `uint16_t` | Protocol Version (`1`) |
| `0x06 - 0x07` | Flags | `uint16_t` | Reserved flags |
| `0x08 - 0x0B` | Manifest Length | `uint32_t` | Byte length ($M$) of JSON manifest |
| `0x0C - (0x0C+M-1)` | Manifest Content | `char[M]` | Raw JSON workflow manifest |
| `0x0C+M - 0x0F+M` | Number of Files | `uint32_t` | Count of packed source files ($N$) |

### Repeated File Entry Header (Repeated $N$ times)

| Field | Type | Description |
| :--- | :--- | :--- |
| `path_len` | `uint16_t` | Length of relative path string ($P$) |
| `rel_path` | `char[P]` | Relative filepath (e.g., `src/main.c`, `Makefile`) |
| `mode` | `uint16_t` | POSIX file permissions (`0755`, `0644`) |
| `file_size` | `uint64_t` | Exact file size in bytes ($S$) |
| `payload` | `uint8_t[S]` | Raw uncompressed file bytes |

---

## 3. Real-Time Streaming & Zero-Copy Artifact Delivery

### Write-Ahead Log (WAL) Streaming
* All `stdout` and `stderr` execution logs are piped to `/tmp/july_storage/<taskId>/output.log`.
* Because Linux caches recently written files in the **OS Page Cache**, `GET /retrieve/<taskId>` serves logs directly from RAM without touching disk.

### Zero-Copy Artifact Transfers via `sendfile()`
When output artifacts are downloaded via `GET /retrieve/<taskId>/artifact/<filename>`:
* The Linux kernel uses Direct Memory Access (DMA) to stream bytes directly from the Page Cache into network socket TX buffers.
* The CPU never copies bytes into user-space application buffers.

---

## 4. The 4 Modular Application Workloads in `tests/`

| Suite | Description | Key Technologies | Output Artifacts | Performance |
| :--- | :--- | :--- | :--- | :--- |
| **`01_system_benchmark`** | Linux telemetry & prime calculation | `/proc` parser, `pthread` | `sys_bench`, `benchmark_report.json` | 166,152 primes in 51.91 ms (~38.5 Mops/s) |
| **`02_matrix_simd`** | Parallel block-tiled matrix multiplication | Cache tiling ($32 \times 32$), Frobenius norm | `matrix_simd`, `matrix_result.bin`, `matrix_report.json` | $512 \times 512$ floats in 35.17 ms (**7.63 GFLOPS**) |
| **`03_kv_storage`** | Concurrent sharded KV store with binary WAL | 16-shard mutexes, binary WAL logging | `kv_engine`, `wal.bin` (1.06 MB), `kv_report.json` | 100,000 ops in 25.62 ms (**3.90 Mops/s**, $P_{99} = 7\,\mu\text{s}$) |
| **`04_data_pipeline`** | Polyglot stream analytics (C + Python 3) | C binary generator, Python `struct` | `ticks.bin` (7.25 MB), `analytics_summary.json` | 250,000 ticks analyzed in 196.81 ms (**1.27M ticks/s**) |

---

## 5. Performance Benchmarks

### Single-Application Latency Budget (580 ms Total)
* **Packaging (Mac)**: ~2 ms
* **LAN Transfer (Wi-Fi)**: ~3 ms
* **Unpack into RAM (Linux)**: ~3 ms
* **GCC Compilation (`-O3`)**: ~420 ms
* **Multithreaded Execution**: ~51 ms
* **Artifact Retrieval (`sendfile`)**: ~10 ms
* **Client Validation (Mac)**: ~90 ms
* **Summary**: Over **81%** of the wall clock time was actual GCC compilation and CPU compute on Linux; framework overhead was only ~80 ms.

### 4-Way Parallel Saturation Stress Test (`make -j4 test-all`)
* **Trigger**: All 4 dispatchers fired at the exact same epoch second (`1794247382`).
* **Concurrency**: 4 independent sandboxes, 4 worker threads running 3 GCC builds and 1 Python analytics script simultaneously.
* **CPU Saturation**: Pegged all 4 cores to 100% capacity (`System Load: 5/579 active threads`).
* **Throughput under Contention**: In-memory KV still achieved **1.37 Mops/s** ($P_{99} = 1\,\mu\text{s}$) while GCC was thrashing the CPU.
* **Network Throughput**: Transferred **~8.5 MB** of binaries and logs across 4 concurrent TCP connections with zero packet loss or corruption.
* **Total Parallel Wall Time**: **3.21 seconds** for all 4 workflows combined.

