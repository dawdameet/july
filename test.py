import os
import sys
import struct
import socket
import argparse
import subprocess

DEFAULT_HOST = "192.168.1.41"
DEFAULT_PORT = 8080

def check_server(host, port):
    try:
        s = socket.create_connection((host, port), timeout=2.0)
        s.close()
        return True
    except (socket.timeout, ConnectionRefusedError, OSError):
        return False

def generate_payloads():
    # 1. Fast no-op payload (taskId="t1", taskCommand=":")
    with open("fast_payload.bin", "wb") as f:
        f.write(struct.pack("<II", 2, 1) + b"t1:")

    # 2. Real command payload (echo date)
    cmd = b"date"
    with open("cmd_payload.bin", "wb") as f:
        f.write(struct.pack("<II", 4, len(cmd)) + b"t_dt" + cmd)

def run_stress_test(host, port, connections, duration, pipelining=1, payload="fast_payload.bin"):
    url = f"http://{host}:{port}/task"
    print(f"\n🚀 Launching Stress Test on {url}")
    print(f"   • Concurrent Connections: {connections}")
    print(f"   • Duration:               {duration} seconds")
    print(f"   • Pipelining factor:      {pipelining}")
    print(f"   • Payload:                {payload} ({os.path.getsize(payload)} bytes)")
    print("=" * 65)

    cmd = [
        "npx", "autocannon",
        "-4",
        "-c", str(connections),
        "-d", str(duration),
        "-p", str(pipelining),
        "-m", "POST",
        "-H", "Content-Type=application/octet-stream",
        "--renderStatusCodes",
        "-i", payload,
        url,
    ]

    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        print("[!] Error: 'npx' or 'autocannon' not found. Please install node/npm.")
    except subprocess.CalledProcessError as e:
        print(f"[!] Autocannon exited with error: {e}")

def main():
    parser = argparse.ArgumentParser(description="July Engine Stress Testing & Bombarding Tool")
    parser.add_argument("target", nargs="?", default=None,
                        help="Target server as host or host:port (default: 192.168.1.41:8080 if online, else localhost:8080)")
    parser.add_argument("-c", "--connections", type=int, default=50,
                        help="Number of concurrent HTTP connections (default: 50)")
    parser.add_argument("-d", "--duration", type=int, default=10,
                        help="Test duration in seconds (default: 10)")
    parser.add_argument("-p", "--pipelining", type=int, default=1,
                        help="Number of pipelined requests per connection (default: 1)")
    parser.add_argument("--real-cmd", action="store_true",
                        help="Bombard with real Linux shell commands ('date') instead of no-op (':')")

    args = parser.parse_args()

    # Determine target host and port
    host = DEFAULT_HOST
    port = DEFAULT_PORT

    if args.target:
        if ":" in args.target:
            h, p = args.target.split(":", 1)
            host = h
            port = int(p)
        else:
            host = args.target
    else:
        # Auto-detect if 192.168.1.41 is reachable, otherwise localhost
        if not check_server(DEFAULT_HOST, DEFAULT_PORT):
            if check_server("127.0.0.1", DEFAULT_PORT):
                host = "127.0.0.1"
                print(f"[*] 192.168.1.41 unreachable; falling back to localhost:{port}")

    if not check_server(host, port):
        print(f"[!] Error: Target server {host}:{port} is not reachable!")
        print("    Please verify that ./server is running on that machine.")
        sys.exit(1)

    print(f"[✓] Connected to target {host}:{port}")

    generate_payloads()
    payload = "cmd_payload.bin" if args.real_cmd else "fast_payload.bin"

    run_stress_test(host, port, args.connections, args.duration, args.pipelining, payload)

if __name__ == "__main__":
    main()
