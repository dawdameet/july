import collections, struct, subprocess, time

cmd = b"sleep 0.2"
with open("slow_payload.bin", "wb") as f:
    f.write(struct.pack("<II", 2, len(cmd)) + b"t1" + cmd)

ac = subprocess.Popen(
    ["npx", "autocannon", "-c", "10", "-d", "8", "-R", "100", "-m", "POST",
     "-H", "Content-Type=application/octet-stream", "-i", "slow_payload.bin",
     "http://localhost:8080/task"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(1.5)  # let the load ramp up
hist = collections.Counter()
while ac.poll() is None:
    n = len(subprocess.run(["pgrep", "-x", "sleep"], capture_output=True, text=True).stdout.split())
    hist[n] += 1
    time.sleep(0.05)

print("concurrent sleep children -> samples:", dict(sorted(hist.items())))
print("MAX concurrent:", max(hist) if hist else 0)
