#!/usr/bin/env python3
"""Boot julyos.elf in headless QEMU (-vga std), wait for DASHBOARD_READY, capture PNG screenshot."""

import os
import socket
import subprocess
import sys
import time
from PIL import Image


def main() -> int:
    sock_path = "/tmp/julyos_mon.sock"
    for p in ("serial_boot.log", "julyos_screen.ppm", "julyos_screen.png", sock_path):
        if os.path.exists(p):
            os.unlink(p)

    proc = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-kernel", "julyos.elf",
            "-vga", "std",
            "-m", "128M",
            "-display", "none",
            "-serial", "file:serial_boot.log",
            "-monitor", f"unix:{sock_path},server,nowait",
            "-no-reboot",
        ]
    )

    deadline = time.time() + 6.0
    ready = False
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        if os.path.exists("serial_boot.log"):
            txt = open("serial_boot.log", errors="ignore").read()
            if "DASHBOARD_READY" in txt:
                ready = True
                break
        time.sleep(0.1)

    time.sleep(0.25)
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock_path)
        s.sendall(b"screendump julyos_screen.ppm\n")
        time.sleep(0.2)
        s.sendall(b"quit\n")
        s.close()
    except Exception as exc:
        print(f"[!] Monitor error: {exc}", file=sys.stderr)

    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    if os.path.exists("serial_boot.log"):
        print(open("serial_boot.log", errors="ignore").read())

    if os.path.exists("julyos_screen.ppm"):
        Image.open("julyos_screen.ppm").save("julyos_screen.png")

    if not ready:
        print("[!] Kernel did not emit DASHBOARD_READY!", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

