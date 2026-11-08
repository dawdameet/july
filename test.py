import struct
import urllib.request
import urllib.error


def send_task(task_id: str, command: str, host: str = "127.0.0.1", port: int = 8080):
    id_bytes = task_id.encode("utf-8")
    cmd_bytes = command.encode("utf-8")

    # Pack two 32-bit unsigned integers in little-endian ('<II') to match TaskHeader
    header = struct.pack("<II", len(id_bytes), len(cmd_bytes))
    payload = header + id_bytes + cmd_bytes

    url = f"http://{host}:{port}/task"
    req = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/octet-stream"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req) as resp:
            print(f"Status: {resp.status}")
            print("Output:")
            print(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        print(f"HTTP Error {e.code}: {e.reason}")
    except urllib.error.URLError as e:
        print(f"Connection failed: {e.reason}")


if __name__ == "__main__":
    send_task("t1", "uname -a")
