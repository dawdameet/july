#!/usr/bin/env python3
import os
import sys
import time
import hashlib
import subprocess
import socket
import json

TARGET = os.environ.get("TARGET", "192.168.1.41:8080")
HOST, PORT = TARGET.split(":")
PORT = int(PORT)

PROJECT_DIR = os.path.abspath("tests/06_codebase_sync")
DISPATCHER_BIN = os.path.abspath("./dispatcher")
SOURCE_FILES = [
    "workflow.json", "Makefile", "main.c",
    "math_engine.c", "math_engine.h",
    "string_parser.c", "string_parser.h",
    "hash_table.c", "hash_table.h",
    "telemetry.c", "telemetry.h"
]

def run_current_approach():
    """Runs current July dispatcher workflow: full pack -> send -> clean make -> download"""
    t0 = time.perf_counter()
    cmd = [DISPATCHER_BIN, TARGET, "workflow"] + SOURCE_FILES
    p = subprocess.run(cmd, cwd=PROJECT_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    t1 = time.perf_counter()
    success = (p.returncode == 0) and os.path.exists(os.path.join(PROJECT_DIR, "sync_app"))
    return (t1 - t0) * 1000.0, success, p.stdout

def compute_hashes():
    """Computes file hashes (CAS manifest)"""
    t0 = time.perf_counter()
    manifest = {}
    for f in SOURCE_FILES:
        path = os.path.join(PROJECT_DIR, f)
        with open(path, "rb") as fp:
            manifest[f] = hashlib.sha256(fp.read()).hexdigest()
    t1 = time.perf_counter()
    return manifest, (t1 - t0) * 1000.0

def run_cas_approach(cached_manifest=None):
    """
    Simulates CAS / Persistent Workspace pipeline over the network:
    1. Local hash scan
    2. Delta negotiation: check which files changed
    3. Transfer ONLY changed files to worker's persistent workspace (/tmp/july_workspace_sync)
    4. Worker runs incremental make (preserves .o files)
    5. Download binary
    """
    # Phase 1: Local Hash Scan
    current_manifest, hash_time_ms = compute_hashes()

    # Phase 2: Delta determination
    t_delta0 = time.perf_counter()
    changed_files = []
    if cached_manifest is None:
        changed_files = list(SOURCE_FILES)
    else:
        for f in SOURCE_FILES:
            if cached_manifest.get(f) != current_manifest.get(f):
                changed_files.append(f)
    t_delta1 = time.perf_counter()
    delta_time_ms = (t_delta1 - t_delta0) * 1000.0

    # Phase 3 & 4: Execute on worker via persistent workspace
    # We construct a remote script that syncs only changed_files, runs incremental make, and emits binary
    t_exec0 = time.perf_counter()
    
    # Send files via base64 or tar stream over July's raw execution socket
    workspace_dir = "/tmp/july_workspace_sync"
    
    # Prepare update commands
    cmds = [f"mkdir -p {workspace_dir}"]
    for f in changed_files:
        path = os.path.join(PROJECT_DIR, f)
        with open(path, "r") as fp:
            content = fp.read()
        # Escape for echo/cat
        cmds.append(f"cat << 'EOFJULY' > {workspace_dir}/{f}\n{content}\nEOFJULY")
    
    # Run incremental make and output binary size
    cmds.append(f"cd {workspace_dir} && make -j4 && ./sync_app && ls -l sync_app")
    full_cmd = " && ".join(cmds)
    
    # Dispatch through July's ultra-fast one-shot execution socket
    cmd_run = [DISPATCHER_BIN, TARGET, full_cmd]
    p = subprocess.run(cmd_run, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    
    # Download binary back to client
    dl_cmd = [DISPATCHER_BIN, TARGET, f"cat {workspace_dir}/sync_app"]
    with open(os.path.join(PROJECT_DIR, "sync_app_cas"), "wb") as out_fp:
        subprocess.run(dl_cmd, stdout=out_fp, stderr=subprocess.DEVNULL)
    
    t_exec1 = time.perf_counter()
    remote_time_ms = (t_exec1 - t_exec0) * 1000.0
    total_time_ms = hash_time_ms + delta_time_ms + remote_time_ms

    success = (p.returncode == 0) and os.path.exists(os.path.join(PROJECT_DIR, "sync_app_cas"))
    return total_time_ms, hash_time_ms, delta_time_ms, remote_time_ms, len(changed_files), success, current_manifest

def touch_file(comment):
    """Simulates developer editing a single source file"""
    path = os.path.join(PROJECT_DIR, "math_engine.c")
    with open(path, "a") as f:
        f.write(f"\n// {comment}\n")

def reset_file():
    """Resets math_engine.c to clean state"""
    path = os.path.join(PROJECT_DIR, "math_engine.c")
    with open(path, "w") as f:
        f.write('''#include "math_engine.h"

double vector_dot(const double *a, const double *b, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

void vector_add_scalar(double *a, double scalar, size_t n) {
    for (size_t i = 0; i < n; i++) {
        a[i] += scalar;
    }
}
''')

def main():
    print("=" * 72)
    print("       CODEBASE SHARING & COMPILATION SHOOTOUT: CURRENT VS CAS/DELTA")
    print(f"Target Worker: {TARGET} | Project: 11 files (5 C modules + headers + Makefile)")
    print("=" * 72)

    reset_file()
    # Clean remote workspace
    subprocess.run([DISPATCHER_BIN, TARGET, "rm -rf /tmp/july_workspace_sync /tmp/july_runs"], 
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print("\n--- TEST 1: COLD RUN (First build from scratch) ---")
    
    # 1. Current Approach (Cold)
    t_curr_cold, ok_curr, out_curr = run_current_approach()
    print(f"[Approach A: Current Full-Pack] Time: {t_curr_cold:.2f} ms | Status: {'OK' if ok_curr else 'FAIL'}")

    # 2. CAS Approach (Cold)
    t_cas_cold, h_ms, d_ms, r_ms, n_files, ok_cas, manifest = run_cas_approach(None)
    print(f"[Approach B: CAS / Workspace]   Time: {t_cas_cold:.2f} ms (Hash: {h_ms:.3f}ms, Delta: {d_ms:.3f}ms, Net+Build: {r_ms:.2f}ms) | Files: {n_files} | Status: {'OK' if ok_cas else 'FAIL'}")

    print("\n--- TEST 2: WARM EDIT (Edit 1 line in math_engine.c and re-run) ---")
    touch_file("edit: added SIMD hint comment")

    # 1. Current Approach (Warm)
    t_curr_warm, ok_curr_w, out_curr_w = run_current_approach()
    print(f"[Approach A: Current Full-Pack] Time: {t_curr_warm:.2f} ms | Status: {'OK' if ok_curr_w else 'FAIL'}")

    # 2. CAS Approach (Warm)
    t_cas_warm, h_ms_w, d_ms_w, r_ms_w, n_files_w, ok_cas_w, manifest_w = run_cas_approach(manifest)
    print(f"[Approach B: CAS / Workspace]   Time: {t_cas_warm:.2f} ms (Hash: {h_ms_w:.3f}ms, Delta: {d_ms_w:.3f}ms, Net+Build: {r_ms_w:.2f}ms) | Files: {n_files_w} | Status: {'OK' if ok_cas_w else 'FAIL'}")

    print("\n" + "=" * 72)
    print("                              RESULTS SUMMARY")
    print("=" * 72)
    print(f"{'Metric':<30} | {'Current (Full Snapshot)':<20} | {'CAS / Delta Workspace':<20}")
    print("-" * 76)
    print(f"{'Cold Run (Total)':<30} | {t_curr_cold:>14.2f} ms     | {t_cas_cold:>14.2f} ms")
    print(f"{'Warm Run (1-line edit)':<30} | {t_curr_warm:>14.2f} ms     | {t_cas_warm:>14.2f} ms")
    speedup = t_curr_warm / t_cas_warm if t_cas_warm > 0 else 0.0
    print(f"{'Incremental Speedup':<30} | {'Baseline':>14}        | {speedup:>13.2f}x FASTER")
    print("=" * 72)

if __name__ == "__main__":
    main()
