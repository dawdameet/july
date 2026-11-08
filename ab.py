import json, statistics, struct, subprocess, sys

runs = int(sys.argv[1]) if len(sys.argv) > 1 else 3
workers = sys.argv[2] if len(sys.argv) > 2 else "1"
dur = sys.argv[3] if len(sys.argv) > 3 else "10"

with open("fast_payload.bin", "wb") as f:
    f.write(struct.pack("<II", 2, 1) + b"t1:")

rows = []
for i in range(runs):
    out = subprocess.run(
        ["npx", "autocannon", "-c", "50", "-d", dur, "-w", workers, "-m", "POST",
         "-H", "Content-Type=application/octet-stream", "-i", "fast_payload.bin",
         "--json", "http://localhost:8080/task"],
        capture_output=True, text=True, check=True).stdout
    d = json.loads(out)
    row = (d["requests"]["average"], d["latency"]["average"], d["latency"]["p99"],
           d["latency"]["max"], d.get("2xx", 0), d.get("non2xx", 0))
    rows.append(row)
    print(f"run {i+1}: {row[0]:.0f} req/s  avg {row[1]} ms  p99 {row[2]} ms  "
          f"max {row[3]} ms  2xx={row[4]} non2xx={row[5]}")

print(f"MEDIAN: {statistics.median(r[0] for r in rows):.0f} req/s, "
      f"p99 {statistics.median(r[2] for r in rows)} ms  (workers={workers})")
