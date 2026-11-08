import struct
import subprocess

# 1. Generate fast_payload.bin (taskId="t1", taskCommand=":")
with open("fast_payload.bin", "wb") as f:
    f.write(struct.pack("<II", 2, 1) + b"t1:")

# 2. Run autocannon via subprocess.run
cmd = [
    "npx", "autocannon",
    "-4",
    "-c", "50",
    "-d", "10",
    "-m", "POST",
    "-H", "Content-Type=application/octet-stream",
    "--renderStatusCodes",
    "-i", "fast_payload.bin",
    "http://localhost:8080/task",
]

subprocess.run(cmd, check=True)
