import struct
import time
import urllib.request
import urllib.error
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed

URL = "http://localhost:8080/task"

def send_task(index: int, command: str = "sleep 1") -> tuple[int, str]:
    task_id = f"t{index}".encode("utf-8")
    cmd_bytes = command.encode("utf-8")

    # Pack TaskHeader: taskIdLen and taskCommandLen as little-endian uint32_t
    header = struct.pack("<II", len(task_id), len(cmd_bytes))
    payload = header + task_id + cmd_bytes

    req = urllib.request.Request(
        URL,
        data=payload,
        headers={"Content-Type": "application/octet-stream"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.reason
    except Exception as e:
        return 0, str(e)

if __name__ == "__main__":
    total_requests = 70  # Exceeds MAX_QUEUE (64) to trigger both 202 and 503
    concurrency = 10

    start = time.perf_counter()
    results = []

    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(send_task, i, "sleep 1") for i in range(total_requests)]
        for future in as_completed(futures):
            results.append(future.result()[0])

    elapsed = time.perf_counter() - start
    counts = Counter(results)

    print(f"Sent {total_requests} tasks in {elapsed:.3f}s")
    for status, count in sorted(counts.items()):
        print(f"  HTTP {status}: {count}")
