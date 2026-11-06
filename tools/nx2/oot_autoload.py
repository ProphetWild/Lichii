#!/usr/bin/env python3
"""Drive Ocarina of Time from boot to gameplay through the debug server (EDEN_NX2_DEBUG_SERVER).

Waits for the server and the first presents, then presses A at the title screen until the game
reports the in-game frame rate pattern (or the press budget runs out), and saves a screenshot.

  python -I tools/nx2/oot_autoload.py [--server 127.0.0.1:6545] [--out shot.png] [--presses 8]
"""

import argparse
import json
import socket
import sys
import time


def request(server, method, params=None, timeout=60.0):
    host, port = server.rsplit(":", 1)
    with socket.create_connection((host, int(port)), timeout=timeout) as sock:
        sock.sendall((json.dumps({"method": method, "params": params or {}}) + "\n").encode())
        buffer = b""
        while not buffer.endswith(b"\n"):
            chunk = sock.recv(1 << 20)
            if not chunk:
                break
            buffer += chunk
    return json.loads(buffer.decode())


def wait_for_server(server, deadline):
    while time.time() < deadline:
        try:
            status = request(server, "status", timeout=5.0)
            result = status.get("result", status)
            if result.get("presents", 0) > 0:
                return result
        except OSError:
            pass
        time.sleep(1.0)
    raise SystemExit("debug server did not answer with presents before the deadline")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="127.0.0.1:6545")
    parser.add_argument("--out", default=None)
    parser.add_argument("--presses", type=int, default=8)
    parser.add_argument("--title-wait", type=float, default=20.0,
                        help="seconds after the first present before pressing A")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--bench", type=float, default=0, help="seconds of fps samples")
    parser.add_argument("--settle", type=float, default=20.0)
    args = parser.parse_args()

    start = time.time()
    wait_for_server(args.server, start + args.timeout)
    print(f"presenting after {time.time() - start:.0f} s; waiting {args.title_wait:.0f} s for the title")
    time.sleep(args.title_wait)
    for press in range(args.presses):
        request(args.server, "input_sequence",
                {"steps": [{"buttons": ["A"], "frames": 6}, {"frames": 150}]}, timeout=120.0)
        status = request(args.server, "status")
        result = status.get("result", status)
        print(f"A #{press + 1}: fps {result.get('fps', 0):.1f}, presents {result.get('presents')}")
    if args.bench > 0:
        # Let shaders and streaming settle, then sample the emulator's fps once a second.
        time.sleep(args.settle)
        samples = []
        for _ in range(int(args.bench)):
            time.sleep(1.0)
            status = request(args.server, "status")
            samples.append(status.get("result", status).get("fps", 0.0))
        samples.sort()
        median = samples[len(samples) // 2]
        print(f"BENCH median {median:.1f} fps, min {samples[0]:.1f}, max {samples[-1]:.1f} "
              f"over {len(samples)} s")
    if args.out:
        shot = request(args.server, "screenshot", timeout=30.0)
        print(json.dumps(shot.get("result", shot))[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main())
