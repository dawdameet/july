# July Roadmap & TODOs

## 1. Live Remote Execution Streaming (`stdout` / `stderr` in Real Time)
- Add live incremental streaming from Fedora back to Mac while tasks/workflows execute (instead of buffering until completion).
- Options: chunked HTTP/SSE endpoint (`GET /stream/<taskId>`) or incremental `Range` / byte-offset tailing of `/tmp/july_storage/<taskId>/stdout.log` in `server.c` + `dispatcher.c`.
- Goal: See live GCC compiler progress, QEMU boot output, Ollama tok/s progress, and remote harness reasoning in real time as it happens on the worker node.

## 2. True Parallel Transaction & Multi-Agent Execution Pipeline
- Eliminate sequential blocking between Mac work and Fedora remote execution.
- Add non-blocking async dispatch (`--async` returning `taskId` immediately, plus `--wait <taskId>` / multiplexed polling) so the coordinator agent or client can launch remote work, keep building/coding locally in parallel, and join results only when ready.
- Pair with multi-node task routing + lease/merge coordination so Mac + multiple LAN workers execute genuinely concurrently rather than serializing transactions.

