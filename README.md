# July: High-Performance Distributed Remote Compute Engine

**July** is an ultra-low-latency, zero-dependency distributed task broker and containerless remote compute engine written in raw C. It turns any spare machine on your local network (LAN) into an ambient compute node—allowing you to offload builds, tests, and heavy multithreaded workloads from your workstation in milliseconds.

No Docker. No SSH key handshakes. No Kubernetes. No `tar` or `gzip` overhead. Just raw POSIX sockets, non-blocking I/O multiplexing (`epoll` on Linux, `kqueue` on macOS), thread pools, and direct kernel DMA via `sendfile()`.

---

## Key Highlights

- **Sub-Second Round Trips**: A complete end-to-end workflow (pack source $\to$ transmit over Wi-Fi $\to$ unpack into RAM $\to$ compile with `gcc -O3` $\to$ execute parallel benchmarks $\to$ stream logs live $\to$ download compiled ELF binaries) finishes in **0.580 seconds**. Over 80% of that time is pure compiler and CPU work; July framework overhead is **< 80 ms**.
- **Extreme Event-Loop Throughput**: Handles **~118k to 151k requests/sec** with zero dropped packets and $P_{99} < 1\text{ ms}$ on the HTTP task ingestion path.
- **Custom `JULY` Binary Envelope**: Packs multi-file source directories into memory-aligned buffers (`0x4A554C59`). Unpacks straight into RAM disks (`tmpfs`) without decompression delays.
- **Zero-Copy Artifact Delivery via `sendfile()`**: Compiled binaries, logs, and analytical datasets stream back to the client directly from the Linux **OS Page Cache** using kernel DMA—bypassing user-space memory copies entirely.
- **Multi-Tenant Concurrency**: Fully isolated sandboxes (`/tmp/july_runs/<taskId>/`) and worker thread pool scheduling (`NUM_WORKERS 4`). Saturated 4-way parallel builds (`make -j4 test-all`) compile, run, and transfer **~8.5 MB** of artifacts across 4 parallel TCP sockets in **3.2 seconds**.
- **Cross-Platform**: Dual event loops—Linux `epoll` and macOS `kqueue`. Self-discovering network interfaces with automatic LAN IP reporting.

---

## Architecture

```
Mac Client (Developer Workstation)             Linux Remote Compute Host (e.g. Fedora x86_64)
┌─────────────────────────────────────────┐    ┌──────────────────────────────────────────────┐
│  dispatcher CLI / Interactive REPL      │    │  epoll Event Loop (Port 8080)                │
│  Memory-Aligned Container Packer        │───►│  Worker Thread Pool (NUM_WORKERS 4)          │
│  (Magic Header: 0x4A554C59 'JULY')      │    │  RAM Sandboxes (/tmp/july_runs/<taskId>/)    │
│  Live Log & sendfile Artifact Receiver  │◄───│  Linux Kernel OS Page Cache + sendfile() DMA │
└─────────────────────────────────────────┘    └──────────────────────────────────────────────┘
```

---

## Getting Started

### 1. Build the Binaries

July requires only a standard C compiler (`gcc` or `clang`) with POSIX threads:

```bash
make
```

This compiles:
- `server`: The high-throughput event loop and execution engine.
- `dispatcher`: The client CLI, container packager, and interactive REPL.

### 2. Launch the Worker Node

Run `server` on your secondary/remote machine (e.g. an idle Linux box on your LAN):

```bash
./server 8080
```

The server automatically scans network interfaces using `getifaddrs()` and displays your LAN addresses:

```
┌─────────────────────────────────────────────────────────────┐
│             July Task & Workflow Engine v2.0                │
├─────────────────────────────────────────────────────────────┤
│  Binding:      0.0.0.0:8080 (IPv4 Any)                      │
│  LAN IP:       192.168.1.41                                 │
│  Event Engine: Linux epoll (Edge/Level Triggered)           │
│  Worker Pool:  4 threads                                    │
│  Sandboxing:   RAM-backed tmpfs (/tmp/july_runs/)           │
└─────────────────────────────────────────────────────────────┘
```

---

## Usage

### A. Workflow Container Execution (Recommended)

Bundle a workflow manifest and source files, ship them to the remote machine, compile/run, and download the resulting artifacts:

```bash
./dispatcher 192.168.1.41:8080 workflow workflow.json Makefile src/main.c
```

### B. One-Shot Command Dispatch

Execute arbitrary commands directly on the remote node:

```bash
./dispatcher 192.168.1.41:8080 "uname -a; lscpu | grep 'Model name'"
```

### C. Interactive REPL

Drop into an interactive remote shell targeting the worker:

```bash
./dispatcher 192.168.1.41:8080
dispatcher> lscpu
dispatcher> free -h
dispatcher> uptime
```

---

## Workflow Manifest Format (`workflow.json`)

Workflows are declared via a concise JSON manifest defining options, pre-flight tool probes, dependencies, build/test steps, and required artifacts to retrieve:

```json
{
  "name": "matrix-simd-cache-benchmark",
  "options": {
    "timeout_sec": 30,
    "kill_if_timeout": true,
    "from_github": false,
    "github_link": ""
  },
  "pre_flight": {
    "check_tools": ["gcc", "make"],
    "deps": [
      "echo 'Verifying compiler and threading runtime'"
    ]
  },
  "build": [
    "make -f Makefile clean",
    "make -f Makefile",
    "./matrix_simd 512"
  ],
  "required_artifacts": [
    "matrix_simd",
    "matrix_result.bin",
    "matrix_report.json"
  ]
}
```

---

## Built-In Test Suites

The repository contains 4 production-grade applications in `tests/` designed to stress compute, memory, storage, and polyglot runtime capabilities:

| Target | Workload Description | Technologies | Output Artifacts |
| :--- | :--- | :--- | :--- |
| `make test-sysbench` | Linux hardware telemetry & parallel prime search | `/proc` parsing, `pthread` | `sys_bench` (ELF), `benchmark_report.json` |
| `make test-matrix` | Block-tiled parallel matrix multiplication ($512 \times 512$) | Cache-tiling, Frobenius norm | `matrix_simd`, `matrix_result.bin`, `matrix_report.json` |
| `make test-kv` | 16-shard in-memory key-value store with binary WAL | Striped mutexes, binary log | `kv_engine`, `wal.bin` (1.06 MB), `kv_report.json` |
| `make test-pipeline` | Polyglot pipeline: C tick generator + Python 3 stream analytics | C binary stream, Python `struct` | `ticks.bin` (7.25 MB), `analytics_summary.json` |

### Running Tests

```bash
# Set your target remote IP (defaults to 192.168.1.41:8080)
export TARGET="192.168.1.41:8080"

# Run individual tests
make test-sysbench
make test-matrix
make test-kv
make test-pipeline

# Run all 4 tests sequentially
make test-all

# Fire all 4 workflows SIMULTANEOUSLY across LAN
make -j4 test-all
```

---

## Benchmarks & Performance Analysis

### 1. The 580 ms Single-Flight Latency Budget

When dispatching `make test` over Wi-Fi between macOS and a physical Fedora Linux machine:

| Phase | Subsystem | Time |
| :--- | :--- | :--- |
| **Packaging** | Client memory-aligned packing (Mac) | ~2 ms |
| **LAN Transmission** | Raw TCP socket over Wi-Fi | ~3 ms |
| **Unpack to RAM** | `tmpfs` sandbox creation (Linux) | ~3 ms |
| **Compilation** | `gcc -O3 -Wall -Wextra -pthread` | ~420 ms |
| **Execution** | Multithreaded prime benchmark (4 threads) | ~51 ms |
| **Artifact Download** | Direct DMA transfer via kernel `sendfile()` | ~10 ms |
| **Client Formatting** | Format inspection and report printing | ~90 ms |
| **Total Wall Clock** | | **0.580 s** |

*Over **81%** of total elapsed time is spent in GCC and CPU execution. Total framework and network overhead is **~80 ms**.*

### 2. Comparison with Traditional Solutions

| Feature / Step | Docker over SSH | SSH + Tarball | **July Remote Engine** |
| :--- | :--- | :--- | :--- |
| **Connection Handshake** | 300–800 ms (Crypto/TLS) | 300–600 ms | **< 1 ms** (Raw TCP) |
| **Packaging Delay** | 200–500 ms (`tar -czf`) | 200–400 ms (`tar -czf`) | **~2 ms** (`JULY` envelope) |
| **Sandbox Setup** | 1,500–3,000 ms (cgroups, overlayfs) | 50–100 ms | **< 3 ms** (RAM `tmpfs`) |
| **Artifact Delivery** | Multi-step REST / Base64 | `tar -xzf` extraction | **< 10 ms** (Kernel `sendfile()`) |
| **Typical Runtime** | **4 – 10 seconds** | **2.5 – 5 seconds** | **0.58 seconds** |

### 3. Load Testing & Stress Bombarding

July includes a stress testing harness using `autocannon` (`test.py`):

```bash
# Stress test raw HTTP ingestion throughput (default 50 connections, 10s)
python3 test.py 192.168.1.41:8080

# Benchmark real shell command execution
python3 test.py 192.168.1.41:8080 --real-cmd
```

---

## Repository Layout

```
.
├── server.c            # Epoll/kqueue event loop, thread pool, HTTP & WAL server
├── dispatcher.c        # Client CLI, REPL, envelope packager, and artifact retriever
├── workflow.h          # Binary envelope specification & JSON manifest parser
├── task.h              # Binary task ABI & execution data structures
├── Makefile            # Build recipes and automated remote test suites
├── test.py             # Autocannon stress testing and bombarding tool
├── TEST_REPORT.md      # Detailed architectural specification & benchmark logs
├── CHANGELOG.md        # Commit history, design decisions, and performance journey
└── tests/
    ├── 01_system_benchmark/   # CPU info & multithreaded prime search
    ├── 02_matrix_simd/        # Block-tiled parallel matrix multiply (7.6 GFLOPS)
    ├── 03_kv_storage/         # 16-shard in-memory key-value engine with binary WAL
    └── 04_data_pipeline/      # Polyglot pipeline (C tick generator + Python 3 analytics)
```

---

## License

MIT License. Designed and built from scratch with pure POSIX primitives and zero external runtime dependencies.

