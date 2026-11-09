# July: High-Performance Distributed Remote Compute Engine

**July** is an ultra-low-latency, zero-dependency distributed task broker and containerless remote compute engine written in raw C. It turns any spare machine on your local network (LAN) into an ambient compute node—allowing you to offload builds, tests, and heavy multithreaded workloads from your workstation in milliseconds.

No Docker. No SSH key handshakes. No Kubernetes. No `tar` or `gzip` overhead. Just raw POSIX sockets, non-blocking I/O multiplexing (`epoll` on Linux, `kqueue` on macOS), thread pools, and direct kernel DMA via `sendfile()`.

---

## Key Highlights

- **Sub-Second Round Trips**: A complete end-to-end workflow (pack source $\to$ transmit over Wi-Fi $\to$ unpack into RAM $\to$ compile with `gcc -O3` $\to$ execute parallel benchmarks $\to$ stream logs live $\to$ download compiled ELF binaries) finishes in **0.580 seconds**. Over 80% of that time is pure compiler and CPU work; July framework overhead is **< 80 ms**.
- **Extreme Event-Loop Throughput**: Handles **~118k to 151k requests/sec** with zero dropped packets and $P_{99} < 1\text{ ms}$ on the HTTP task ingestion path.
- **Custom `JULY` Binary Envelope**: Packs multi-file source directories into memory-aligned buffers (`0x4A554C59`). Unpacks straight into RAM disks (`tmpfs`) without decompression delays.
- **Zero-Copy Artifact Delivery via `sendfile()`**: Compiled binaries, logs, and analytical datasets stream back to the client directly from the Linux **OS Page Cache** using kernel DMA—bypassing user-space memory copies entirely.
- **Worker Sandboxing & Process Jailing**: Enforces kernel memory limits (`RLIMIT_DATA`), process caps (`RLIMIT_NPROC`), Linux namespaces (`CLONE_NEWUSER`, `CLONE_NEWNS`, `CLONE_NEWPID`), and offline loopback network isolation (`CLONE_NEWNET`).
- **Token Authentication**: Zero-RTT header verification (`X-July-Token` / `Authorization: Bearer`). Protected nodes reject unauthorized LAN requests with `401 Unauthorized`.
- **Interactive Ambient Developer Console**: A persistent terminal session (`july>`) that holds a warm TCP pipe open while you hack, dispatches builds with zero handshake delay, and cleanly disconnects when you close your laptop lid.
- **Multi-Tenant Concurrency**: Fully isolated sandboxes (`/tmp/july_runs/<taskId>/`) and worker thread pool scheduling (`NUM_WORKERS 4`). Saturated 5-way parallel builds (`make -j5 test-all`) compile, run, and transfer **~8.5 MB** of artifacts across 5 parallel TCP sockets in **~3.2 seconds**.
- **Cross-Platform**: Dual event loops—Linux `epoll` and macOS `kqueue`. Self-discovering network interfaces with automatic LAN IP reporting.

---

## Architecture

```
Mac Client (Developer Workstation)             Linux Remote Compute Host (Fedora x86_64)
┌─────────────────────────────────────────┐    ┌──────────────────────────────────────────────┐
│  Interactive Developer Console / CLI    │    │  epoll Event Loop (Port 8080)                │
│  Memory-Aligned Container Packer        │───►│  Worker Thread Pool (NUM_WORKERS 4)          │
│  (Magic Header: 0x4A554C59 'JULY')      │    │  RAM Sandboxes (/tmp/july_runs/<taskId>/)    │
│  Warm TCP Connection Pool               │    │  Linux Kernel Namespaces + setrlimit Jails   │
│  Live Log & sendfile Artifact Receiver  │◄───│  Linux Kernel OS Page Cache + sendfile() DMA │
└─────────────────────────────────────────┘    └──────────────────────────────────────────────┘
```

---

## Core Functionalities

### 1. Token-Based Authentication (`--token` / `JULY_TOKEN`)
July protects worker nodes against unauthorized LAN callers without the multi-RTT latency of TLS or SSH handshakes:
* **Enabling Auth on Worker**:
  ```bash
  ./server 8080 --token secret123
  # Or via environment variable:
  export JULY_TOKEN="secret123" && ./server 8080
  # Or via persistent file:
  echo "secret123" > ~/.july/token && ./server 8080
  ```
* **Client Authentication**:
  `dispatcher` automatically injects `X-July-Token: secret123` on all requests. If the token is missing or mismatched, the worker rejects the request with `401 Unauthorized`.
* **Open LAN Mode**: If no token is configured, the server defaults to open mode with an explicit startup notice.

### 2. Linux Kernel Sandboxing & Jailing
Tasks run in dedicated, RAM-backed directories (`/tmp/july_runs/<taskId>/`) with multi-layer kernel isolation:
* **Memory Ceiling (`RLIMIT_DATA`)**: Caps data segment memory usage to prevent rogue allocations from starving host RAM.
* **Fork-Bomb Defense (`RLIMIT_NPROC`)**: Restricts maximum child processes per task.
* **Network Isolation (`CLONE_NEWNET`)**: Unshares the network namespace, giving the build an isolated loopback interface. Outbound WAN connections are blocked by the kernel (`Network is unreachable`), ensuring untrusted code cannot exfiltrate data or scan your LAN.
* **Namespaces (`CLONE_NEWUSER`, `CLONE_NEWNS`, `CLONE_NEWPID`)**: Isolated mount tables, PID tables, and unprivileged user mappings.

### 3. Rich JSON Manifest Specification (`workflow.json`)
Workflows support custom environment variables, sandbox controls, and failure artifact harvesting:

```json
{
  "name": "isolated-production-pipeline",
  "options": {
    "timeout_sec": 30,
    "kill_if_timeout": true
  },
  "env": {
    "CC": "gcc",
    "CFLAGS": "-O3 -march=native",
    "CUSTOM_TAG": "v2.0"
  },
  "sandbox": {
    "network": false,
    "memory_mb": 512,
    "max_pids": 64,
    "namespace": true
  },
  "pre_flight": {
    "check_tools": ["gcc", "make"],
    "deps": [
      "echo 'Verifying isolated environment'"
    ]
  },
  "build": [
    "make -f Makefile clean",
    "make -f Makefile",
    "./app"
  ],
  "artifacts": {
    "required": [
      "app",
      "report.json"
    ],
    "on_failure": [
      "crash.log"
    ]
  }
}
```

### 4. Interactive Ambient Developer Console
Rather than running an invisible, brittle background daemon that freezes when your laptop sleeps, `./dispatcher` provides an interactive shell that holds a warm TCP pipe open while you code:

```bash
./dispatcher 192.168.1.41:8080
```

Inside the console:
```
┌─────────────────────────────────────────────────────────────┐
│             July Interactive Developer Console              │
│  Target: 192.168.1.41:8080                                  │
│  Type 'help' for commands, 'exit' or Ctrl+D to quit         │
└─────────────────────────────────────────────────────────────┘

july(192.168.1.41:8080)> status
[✓] Remote node 192.168.1.41:8080 is ONLINE (TCP RTT: 1.84 ms)

july(192.168.1.41:8080)> run workflow.json Makefile src/main.c
[+] Packing workflow container...
[✓] Workflow Completed Successfully (Exit Code: 0)!
[*] Downloading 2 required artifact(s) via zero-copy sendfile()...

july(192.168.1.41:8080)> uname -a
[✓] (exit=0): Linux fedora 6.19.10-300.fc44.x86_64 ...

july(192.168.1.41:8080)> exit
```
* **Zero Lifecycle Baggage**: Closing the terminal or closing your laptop lid cleanly terminates the connection. Zero zombie daemons or stale socket locks.

---

## Getting Started

### 1. Build the Binaries

```bash
make
```

Compiles:
- `server`: The non-blocking event engine and execution daemon.
- `dispatcher`: The client CLI, envelope packager, and interactive console.

### 2. Launch the Worker Node

On your remote/secondary Linux machine:

```bash
./server 8080
# Or with authentication enabled:
./server 8080 --token mysecretkey
```

The server automatically scans network interfaces using `getifaddrs()` and displays active LAN IPs:

```
┌─────────────────────────────────────────────────────────────┐
│  July Task & Workflow Engine (epoll  backend)               │
│  Listening on: 0.0.0.0:8080  (All LAN interfaces)          │
│  Worker Pool:  4 threads                                    │
│  Auth Mode:    Protected (Token Enforced)                   │
├─────────────────────────────────────────────────────────────┤
│  Connect with dispatcher from your Mac / LAN:               │
│    • LAN (wlp2s0  ): http://192.168.1.41:8080               │
└─────────────────────────────────────────────────────────────┘
```

---

## Usage

### A. Interactive Developer Console
```bash
./dispatcher 192.168.1.41:8080
```

### B. One-Shot Workflow Container Dispatch
Bundle a manifest and local source files, compile remotely, and pull down artifacts:
```bash
./dispatcher 192.168.1.41:8080 workflow workflow.json Makefile src/main.c
```

### C. Direct Shell Command Execution
```bash
./dispatcher 192.168.1.41:8080 "lscpu | grep 'Model name'; free -h"
```

---

## Built-In Test Suites

The repository includes 5 production-grade applications in `tests/`:

| Target | Workload Description | Technologies Tested | Output Artifacts |
| :--- | :--- | :--- | :--- |
| `make test-sysbench` | Linux hardware telemetry & parallel prime search | `/proc` parsing, `pthread` | `sys_bench` (ELF), `benchmark_report.json` |
| `make test-matrix` | Block-tiled parallel matrix multiplication ($512 \times 512$) | Cache-tiling, Frobenius norm, GFLOPS | `matrix_simd`, `matrix_result.bin`, `matrix_report.json` |
| `make test-kv` | 16-shard in-memory key-value store with binary WAL | Striped mutexes, binary log (1.7M ops/s) | `kv_engine`, `wal.bin` (1.06 MB), `kv_report.json` |
| `make test-pipeline` | Polyglot pipeline: C tick generator + Python 3 stream analytics | C binary stream, Python `struct` (1.27M ticks/s) | `ticks.bin` (7.25 MB), `analytics_summary.json` |
| `make test-sandbox` | Sandbox verification: env injection, memory limit, network isolation | `RLIMIT_DATA` (512 MB), `CLONE_NEWNET` | `sandbox_probe`, `sandbox_report.json` |

### Running Tests

```bash
export TARGET="192.168.1.41:8080"

# Run individual suites
make test-sysbench
make test-matrix
make test-kv
make test-pipeline
make test-sandbox

# Run all 5 suites sequentially
make test-all

# Fire all 5 workflows CONCURRENTLY across LAN
make -j5 test-all
```

---

## Benchmarks & Performance Analysis

### 1. Warm TCP vs Cold TCP Shootout

Measured directly over local Wi-Fi between macOS and the Linux worker (2,000 requests across 20 concurrent threads via `test_comparison.py`):

```
┌──────────────────────────────┬──────────────────┬──────────────────┬──────────────┐
│ Metric                       │ Cold TCP (Fresh) │ Warm TCP (Reused)│ Improvement  │
├──────────────────────────────┼──────────────────┼──────────────────┼──────────────┤
│ Throughput (Req/Sec)         │ 833 req/s        │ 1,929 req/s      │ 2.32x FASTER │
│ Average Latency              │ 23.81 ms         │ 9.54 ms          │ 60% lower    │
│ P50 Latency (Median)         │ 18.90 ms         │ 8.89 ms          │ 53% lower    │
│ P95 Latency                  │ 34.57 ms         │ 14.91 ms         │ 57% lower    │
│ P99 Latency (Tail)           │ 304.71 ms        │ 21.65 ms         │ 14x FASTER   │
│ Total Wall Time (2k reqs)    │ 2.40 seconds     │ 1.04 seconds     │ 2.3x faster  │
└──────────────────────────────┴──────────────────┴──────────────────┴──────────────┘
```

Under sustained load with `autocannon` (50 connections over 10 seconds), **Warm TCP sustained 2,762 req/s** (peaking at **4,045 req/s**) with **27,618 requests, 100% 202 Accepted, and zero errors**.

### 2. The 580 ms Single-Flight Latency Budget

When dispatching `make test` over Wi-Fi between macOS and the Linux box:

| Phase | Subsystem | Time |
| :--- | :--- | :--- |
| **Packaging** | Client memory-aligned packing (Mac) | ~2 ms |
| **LAN Transmission** | Raw TCP socket over Wi-Fi | ~3 ms |
| **Unpack to RAM** | `tmpfs` sandbox creation (Linux) | ~3 ms |
| **Compilation** | `gcc -O3 -Wall -Wextra -pthread` | ~420 ms |
| **Execution** | Multithreaded prime benchmark (4 threads) | ~51 ms |
| **Artifact Download** | Direct DMA transfer via kernel `sendfile()` | ~10 ms |
| **Client Validation** | Format inspection and report printing | ~90 ms |
| **Total Wall Clock** | | **0.580 s** |

*Over **81%** of elapsed time is spent in GCC and CPU execution. Total framework and network overhead is **~80 ms**.*

### 3. Comparison with Traditional Solutions

| Feature / Step | Docker over SSH | SSH + Tarball | **July Remote Engine** |
| :--- | :--- | :--- | :--- |
| **Connection Handshake** | 300–800 ms (Crypto/TLS) | 300–600 ms | **< 1 ms** (Raw TCP / Warm Pipe) |
| **Packaging Delay** | 200–500 ms (`tar -czf`) | 200–400 ms (`tar -czf`) | **~2 ms** (`JULY` envelope) |
| **Sandbox Setup** | 1,500–3,000 ms (cgroups, overlayfs) | 50–100 ms | **< 3 ms** (RAM `tmpfs` + namespaces) |
| **Artifact Delivery** | Multi-step REST / Base64 | `tar -xzf` extraction | **< 10 ms** (Kernel `sendfile()`) |
| **Typical Runtime** | **4 – 10 seconds** | **2.5 – 5 seconds** | **0.58 seconds** |

---

## Repository Layout

```
.
├── server.c            # Epoll/kqueue event loop, sandboxing, thread pool, HTTP & WAL server
├── dispatcher.c        # Client CLI, interactive console, envelope packager, artifact retriever
├── workflow.h          # Binary envelope specification & JSON manifest parser
├── task.h              # Binary task ABI & execution data structures
├── Makefile            # Build recipes and automated remote test suites
├── test_comparison.py  # Warm TCP vs Cold TCP comparative benchmark shootout
├── test.py             # Autocannon stress testing and bombarding tool
├── linkedinnote.txt    # Project narrative and technical devlog
├── TEST_REPORT.md      # Detailed architectural specification & benchmark logs
├── CHANGELOG.md        # Commit history, design decisions, and performance journey
└── tests/
    ├── 01_system_benchmark/   # CPU info & multithreaded prime search
    ├── 02_matrix_simd/        # Block-tiled parallel matrix multiply (7.6 GFLOPS)
    ├── 03_kv_storage/         # 16-shard in-memory key-value engine with binary WAL
    ├── 04_data_pipeline/      # Polyglot pipeline (C tick generator + Python 3 analytics)
    └── 05_sandbox_rich/       # Kernel sandbox verification (memory caps, network isolation)
```

---

## License

MIT License. Designed and built from scratch with pure POSIX primitives and zero external runtime dependencies.
