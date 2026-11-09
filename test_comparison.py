#!/usr/bin/env python3
"""
Side-by-side benchmark: Warm TCP (Keep-Alive) vs Cold TCP (New Connection Per Request)
Targets July Task Engine on 192.168.1.41:8080 over local LAN.
"""

import sys
import time
import socket
import struct
import statistics
from concurrent.futures import ThreadPoolExecutor

TARGET_HOST = "192.168.1.41"
TARGET_PORT = 8080
TOTAL_REQUESTS = 2000
CONCURRENCY = 20

# Payload: taskId="t1", cmd=":"
PAYLOAD = struct.pack("<II", 2, 1) + b"t1:"

def make_request_data(keep_alive=True):
    conn_hdr = "keep-alive" if keep_alive else "close"
    headers = (
        f"POST /task HTTP/1.1\r\n"
        f"Host: {TARGET_HOST}:{TARGET_PORT}\r\n"
        f"Content-Type: application/octet-stream\r\n"
        f"Content-Length: {len(PAYLOAD)}\r\n"
        f"Connection: {conn_hdr}\r\n"
        f"\r\n"
    ).encode("ascii")
    return headers + PAYLOAD

REQUEST_WARM = make_request_data(keep_alive=True)
REQUEST_COLD = make_request_data(keep_alive=False)

def worker_warm_connection(num_requests):
    """Sends multiple requests over a single persistent TCP connection."""
    latencies = []
    try:
        s = socket.create_connection((TARGET_HOST, TARGET_PORT), timeout=5.0)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for _ in range(num_requests):
            t0 = time.perf_counter()
            s.sendall(REQUEST_WARM)
            # Read until \r\n\r\n
            resp = b""
            while b"\r\n\r\n" not in resp:
                chunk = s.recv(512)
                if not chunk:
                    break
                resp += chunk
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            latencies.append(elapsed_ms)
        s.close()
    except Exception as e:
        pass
    return latencies

def worker_cold_connection(num_requests):
    """Opens a brand new TCP socket for EVERY single request."""
    latencies = []
    for _ in range(num_requests):
        t0 = time.perf_counter()
        try:
            s = socket.create_connection((TARGET_HOST, TARGET_PORT), timeout=5.0)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.sendall(REQUEST_COLD)
            resp = b""
            while b"\r\n\r\n" not in resp:
                chunk = s.recv(512)
                if not chunk:
                    break
                resp += chunk
            s.close()
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            latencies.append(elapsed_ms)
        except Exception:
            pass
    return latencies

def run_benchmark(mode_name, worker_func):
    reqs_per_worker = TOTAL_REQUESTS // CONCURRENCY
    print(f"[*] Running {mode_name} Benchmark...")
    print(f"    • Total Requests: {TOTAL_REQUESTS} across {CONCURRENCY} concurrent threads")
    
    t_start = time.perf_counter()
    all_latencies = []

    with ThreadPoolExecutor(max_workers=CONCURRENCY) as executor:
        futures = [executor.submit(worker_func, reqs_per_worker) for _ in range(CONCURRENCY)]
        for f in futures:
            all_latencies.extend(f.result())

    wall_time = time.perf_counter() - t_start
    throughput = len(all_latencies) / wall_time if wall_time > 0 else 0

    if not all_latencies:
        print(f"    [!] No successful requests.")
        return None

    all_latencies.sort()
    p50 = all_latencies[int(len(all_latencies) * 0.50)]
    p95 = all_latencies[int(len(all_latencies) * 0.95)]
    p99 = all_latencies[int(len(all_latencies) * 0.99)]
    avg = statistics.mean(all_latencies)

    print(f"    ➔ Completed in {wall_time:.2f}s | Throughput: {throughput:.0f} req/s")
    print(f"    ➔ Latency: Avg={avg:.2f}ms | P50={p50:.2f}ms | P95={p95:.2f}ms | P99={p99:.2f}ms\n")

    return {
        "mode": mode_name,
        "completed": len(all_latencies),
        "wall_time": wall_time,
        "throughput": throughput,
        "avg": avg,
        "p50": p50,
        "p95": p95,
        "p99": p99
    }

def main():
    target = sys.argv[1] if len(sys.argv) > 1 else f"{TARGET_HOST}:{TARGET_PORT}"
    print("=================================================================")
    print(f"     July Engine: Warm TCP vs Cold TCP Performance Shootout     ")
    print(f"     Target Node: {target} over Local Wi-Fi                      ")
    print("=================================================================\n")

    # 1. Warm TCP (Persistent Socket)
    warm_res = run_benchmark("WARM TCP (Keep-Alive Reused Sockets)", worker_warm_connection)

    # 2. Cold TCP (New 3-Way Handshake Per Request)
    cold_res = run_benchmark("COLD TCP (Fresh Connection Per Request)", worker_cold_connection)

    if warm_res and cold_res:
        speedup = warm_res["throughput"] / cold_res["throughput"] if cold_res["throughput"] > 0 else 0
        lat_reduction = ((cold_res["avg"] - warm_res["avg"]) / cold_res["avg"] * 100.0) if cold_res["avg"] > 0 else 0

        print("=================================================================")
        print("                  FINAL COMPARISON RESULTS                       ")
        print("=================================================================")
        print(f"  • Warm TCP Throughput:  {warm_res['throughput']:>6.0f} req/s  (Avg: {warm_res['avg']:.2f} ms | P50: {warm_res['p50']:.2f} ms | P99: {warm_res['p99']:.2f} ms)")
        print(f"  • Cold TCP Throughput:  {cold_res['throughput']:>6.0f} req/s  (Avg: {cold_res['avg']:.2f} ms | P50: {cold_res['p50']:.2f} ms | P99: {cold_res['p99']:.2f} ms)")
        print("-----------------------------------------------------------------")
        print(f"  ➔ Throughput Speedup:   {speedup:.2f}x FASTER with Warm TCP")
        print(f"  ➔ Latency Reduction:    {lat_reduction:.1f}% lower latency")
        print("=================================================================")

if __name__ == "__main__":
    main()

