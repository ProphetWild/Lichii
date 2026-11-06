#!/usr/bin/env python3
"""Watch a running game through the debug server for a broken (flat-colour) 3D scene.

Takes a screenshot every --interval seconds for --duration seconds and reports, for each, the
share of pixels within a small distance of the most common colour in the centre region. A healthy
scene is well below 0.5; the Ocarina cyan failure fills almost all of it.

  python -I tools/nx2/oot_frame_check.py [--server 127.0.0.1:6545] [--duration 90] [--interval 10]
Exits 1 if any screenshot looked broken.
"""

import argparse
import base64
import collections
import io
import json
import socket
import struct
import sys
import time
import zlib


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
    reply = json.loads(buffer.decode())
    return reply.get("result", reply)


def read_png(source):
    """Minimal decoder for 8-bit RGB/RGBA non-interlaced PNGs (what the debug server writes)."""
    data = source if isinstance(source, bytes) else open(source, "rb").read()
    pos, idat, width, height, channels = 8, b"", 0, 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            channels = {2: 3, 6: 4}[colour]
            assert depth == 8
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filt, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            if filt == 1:
                line[x] = (line[x] + a) & 255
            elif filt == 2:
                line[x] = (line[x] + b) & 255
            elif filt == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        previous = line
    return width, height, channels, rows


def flat_share(source):
    width, height, channels, rows = read_png(source)
    pixels = []
    for y in range(height // 4, 3 * height // 4, 4):
        row = rows[y]
        for x in range(width // 4, 3 * width // 4, 4):
            i = x * channels
            pixels.append((row[i] >> 3, row[i + 1] >> 3, row[i + 2] >> 3))
    common, _ = collections.Counter(pixels).most_common(1)[0]
    near = sum(1 for p in pixels if max(abs(p[k] - common[k]) for k in range(3)) <= 1)
    return near / len(pixels), tuple(v << 3 for v in common)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="127.0.0.1:6545")
    parser.add_argument("--duration", type=float, default=90.0)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--threshold", type=float, default=0.6)
    args = parser.parse_args()
    broken = False
    end = time.time() + args.duration
    while time.time() < end:
        shot = request(args.server, "screenshot", timeout=30.0)
        status = request(args.server, "status")
        share, colour = flat_share(base64.b64decode(shot["png_base64"]))
        # A black or near-black flat frame is a load or fade, not the broken scene.
        bad = share >= args.threshold and max(colour) >= 64
        broken |= bad
        print(f"t+{status.get('uptime_s', 0):.0f}s fps {status.get('fps', 0):.1f} flat {share:.2f} "
              f"colour {colour}{'  BROKEN' if bad else ''}", flush=True)
        time.sleep(args.interval)
    print("RESULT", "BROKEN" if broken else "ok")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
