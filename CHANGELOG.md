# Changelog & Architectural Evolution

All notable milestones, git commits, optimization breakthroughs, and architectural specifications for the **July High-Performance Task Engine**.

---

## Git Commit History

### `0c4022c` — `dispatcher on LAN`
- **Date**: Mon Nov 9 20:50:47 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Implemented `dispatcher.c` (760+ lines of pure C):
    - Direct binary ABI serialization (`TaskHeader` + `taskId` + `taskCommand`).
    - Full HTTP client in bare C: `POST /task` submission and `GET /retrieve/<taskId>` result polling.
    - Two operating modes: one-shot CLI execution and interactive REPL shell (`dispatcher> `).
    - Async dispatch (`-a`) and standalone task query (`-g`).
    - Colorized ANSI terminal output and execution latency reporting.
  - Enhanced `server.c`:
    - Auto-detection of all local IPv4 network interfaces via `getifaddrs()` on launch.
    - Printed clear network banner showing active LAN IPs (e.g., `eth0: 192.168.1.x`) and listening port.
    - Configurable port via command line argument (`./server <port>`) and `PORT` environment variable.
    - Full LAN support: binding explicitly to `0.0.0.0` (`INADDR_ANY`).

---

### `e9e2efd` — `epoll`
- **Date**: Mon Nov 9 20:34:48 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Added Linux `epoll` engine alongside macOS `kqueue`:
    - `#if defined(__APPLE__)` for `kqueue` and `#else` for Linux `epoll`.
    - `epoll_create1(EPOLL_CLOEXEC)`, `epoll_ctl`, `epoll_wait`.
    - Registered sockets with `EPOLLIN | EPOLLRDHUP` for immediate client hangup detection.
    - Interest switching between `EPOLLIN` and `EPOLLIN | EPOLLOUT` for non-blocking flush cycles.
  - Signal safety: Added `signal(SIGPIPE, SIG_IGN)` to prevent process crashes on disconnected clients.
  - Added striped concurrency hash table for task results (`ResultStripe g_stripes[64]`).
  - Implemented `GET /retrieve/<taskId>` endpoint returning task status, exit code (`X-Task-Exit-Code`), and execution output.

---

### `f81532e` — `mess`
- **Date**: Mon Nov 9 02:10:38 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Experimentation with task result tracking, slow/fast payload variants, and multi-core tuning.
  - Added `slow_payload.bin` and `ab.py` benchmarks.
  - Began separation of fast no-op paths from full command execution pipes.

---

### `b3776ca` — `test`
- **Date**: Mon Nov 9 01:10:48 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Re-wrote network loop from blocking `accept()`/`read()` to non-blocking `kqueue` event loop.
  - Introduced `Connection` state machine to manage partial reads, writes, and HTTP pipelining.
  - Integrated `autocannon` into `test.py` (50 persistent connections, 10s benchmark).
  - Generated `fast_payload.bin` (11-byte binary task: `taskId="t1"`, `taskCommand=":"`).

---

### `0ac8a81` — `threading 4`
- **Date**: Mon Nov 9 00:59:06 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Introduced bounded circular queue (`TaskQueue`) with `pthread_mutex_t` and `pthread_cond_t`.
  - Added 4 background worker threads.
  - Server returned `202 Accepted` immediately upon queueing rather than blocking the network thread during execution.

---

### `e906426` — `woeker is up ig`
- **Date**: Mon Nov 9 00:55:13 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Added `execute_task()` using `popen()` / `pclose()`.
  - Added `free_task()`.
  - Created initial `test.py` using Python `urllib` to POST binary payloads.

---

### `e80602f` — `fucking parser`
- **Date**: Mon Nov 9 00:36:17 2026 +0530
- **Author**: Meet Dawda
- **Summary**:
  - Initial server repository setup.
  - Defined binary Task ABI in `task.h` (`TaskHeader` with `uint32_t taskIdLen`, `uint32_t taskCommandLen`).
  - Initial HTTP parser: `extract_body()` and `parse_task()`.
  - Basic blocking single-threaded socket server on port 8080.

---

## Performance Optimization Milestones

Through iterative profiling on Apple Silicon (4 Performance + 4 Efficiency cores):

| Milestone | Architecture State | Req/Sec | 202 Success Rate | Avg Latency | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **v1 (Baseline)** | `popen`, stdio `printf`, `malloc` per task, 1K queue | 32,888 | **8,546 (2.6%)** | 0.98 ms | 97.4% failed with 503 Service Unavailable |
| **v2 (Zero-Alloc)** | `posix_spawn`, inline buffers, fast-path, 32K queue | 79,625 | **875,836 (100%)** | 0.06 ms | 100x increase in handled tasks; 0 errors |
| **v3 (Pool + Batch)** | Connection pool + batched `kevent` changelists | 70,278 | **702,752 (100%)** | 0.14 ms | Deferred registrations added 1-loop latency |
| **v4 (SPSC Queues)** | Per-worker lock-free SPSC queues with spin-yield | 74,837 | **748,413 (100%)** | 0.14 ms | Spinning workers saturated E-cores |
| **v5 (Multi-IO)** | 2 I/O threads via `SO_REUSEPORT` | 58,941 | **589,402 (100%)** | 0.27 ms | Core contention with client autocannon on same CPU |
| **v6 (Tuned Prod)** | **1 I/O on P-core + condvar dedup + hot L1 buffer** | **85,670** *(peak 87,423)* | **942,318 (100%)** | **0.05 ms (50 µs)** | **Production reference build** |

---

## Architectural Decision Log: Next Phase (Task Containers & Workflows)

### 1. Workflow Manifest Format: **JSON**
- **Decision**: Use a structured JSON manifest (`workflow.json`).
- **Rationale**: Strict grammar, zero whitespace/indentation ambiguity, fast to parse in C, easily generated by scripts/tools.

### 2. Multi-File Transfer: **Custom Binary Multi-File Envelope**
- **Decision**: Avoid `tar.gz` and uncompressed `tar`. Use a custom binary envelope:
  `[MAGIC 4B][MANIFEST_LEN 4B][MANIFEST_JSON][NUM_FILES 4B][File 1: path_len, path, mode, file_size, raw_bytes]...`
- **Rationale**: Eliminates subprocess spawning (`fork()` + `tar -xf`) and CPU compression cycles. Files are extracted directly with one `open()` + `write()` per file in sub-microsecond time.

### 3. Storage & Retrieval: **WAL + OS Page Cache**
- **Decision**: Workers append task results, logs, and artifacts directly to a structured Write-Ahead Log / task directory on disk (`/var/lib/july_storage/<taskId>/`).
- **Rationale**: Freshly written pages are immediately hot in the **Linux OS Page Cache (RAM)**. When the Mac requests results via `sendfile()`, the kernel streams directly from RAM over DMA to the socket with **zero user-space memory copies**, zero manual eviction threads, and automatic crash durability.

