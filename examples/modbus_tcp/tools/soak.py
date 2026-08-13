#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Run the device the way a commissioned one runs, for as long as you like.

    python soak.py 192.168.11.2 --minutes 30

A Modbus master polling continuously, a browser polling once a second, and the
occasional abandoned connection -- which is the mix that broke earlier versions
of this firmware, and none of the individual pieces did.

Reports once a minute. What to watch is not the totals but the trend: free heap
and its low-water mark, whether refusals appear, and whether the response time
climbs. A leak shows as a low-water mark that keeps falling; a socket problem
shows as refusals that start partway through and never stop.
"""
import argparse
import json
import socket
import struct
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("host", nargs="?", default="192.168.11.2")
parser.add_argument("--minutes", type=float, default=10)
parser.add_argument("--port", type=int, default=0, help="Modbus port; 0 to ask")
args = parser.parse_args()
host = args.host


def http(path, timeout=5):
    s = socket.create_connection((host, 80), timeout=timeout)
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\n\r\n" % (path, host)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        c = s.recv(4096)
        if not c:
            raise RuntimeError("closed early")
        buf += c
    _, _, body = buf.partition(b"\r\n\r\n")
    while True:
        c = s.recv(65536)
        if not c:
            break
        body += c
    s.close()
    return body


port = args.port or json.loads(http("/api/config"))["port"]
start = json.loads(http("/api/status"))
print("soaking %s for %.0f min -- Modbus %d, heap %d KB, up %d s\n"
      % (host, args.minutes, port, start["heap"] // 1024, start["uptime"]))

deadline = time.time() + args.minutes * 60
next_report = time.time() + 60

web_ok = web_fail = mb_ok = mb_fail = 0
worst_ms = 0.0
heap_first = start["heap"]
heap_min_first = start["heap_min"]
mb_sock = None
tid = 0
round_no = 0

print("%-6s %8s %8s %9s %9s %8s" %
      ("min", "web", "modbus", "heap KB", "min KB", "worst ms"))

while time.time() < deadline:
    round_no += 1

    # The browser's two polls.
    for path in ("/api/status", "/api/registers"):
        t0 = time.time()
        try:
            http(path)
            web_ok += 1
            worst_ms = max(worst_ms, (time.time() - t0) * 1000)
        except Exception:                       # noqa: BLE001 - counted
            web_fail += 1

    # A master holding one connection across many polls, reconnecting when the
    # device drops it -- which is what a real master does, and what makes a
    # leaked session visible.
    try:
        if mb_sock is None:
            mb_sock = socket.create_connection((host, port), timeout=5)
        tid = (tid + 1) & 0xFFFF
        mb_sock.sendall(struct.pack("!HHHB", tid, 0, 6, 1) +
                        struct.pack("!BHH", 0x03, 0, 10))
        head = mb_sock.recv(9)
        if len(head) < 9 or struct.unpack("!H", head[0:2])[0] != tid:
            raise RuntimeError("bad reply")
        rest = struct.unpack("!H", head[4:6])[0] - 3
        while rest > 0:
            chunk = mb_sock.recv(rest)
            if not chunk:
                raise RuntimeError("closed mid-reply")
            rest -= len(chunk)
        mb_ok += 1
    except Exception:                           # noqa: BLE001 - counted
        mb_fail += 1
        try:
            mb_sock.close()
        except Exception:                       # noqa: BLE001
            pass
        mb_sock = None

    # Every 25th round, walk away mid-request. A browser tab closing does this.
    if round_no % 25 == 0:
        for target in (80, port):
            try:
                s = socket.create_connection((host, target), timeout=2)
                s.sendall(b"GET /api/st")
                s.close()
            except OSError:
                pass

    if time.time() >= next_report:
        next_report += 60
        try:
            st = json.loads(http("/api/status"))
            print("%-6.0f %8s %8s %9d %9d %8.0f"
                  % ((time.time() - (deadline - args.minutes * 60)) / 60,
                     "%d/%d" % (web_ok, web_ok + web_fail),
                     "%d/%d" % (mb_ok, mb_ok + mb_fail),
                     st["heap"] // 1024, st["heap_min"] // 1024, worst_ms))
        except Exception as exc:                # noqa: BLE001 - reported
            print("  status unavailable: %s" % type(exc).__name__)
        worst_ms = 0.0

    time.sleep(0.5)

print()
try:
    end = json.loads(http("/api/status"))
    print("web    %d ok, %d failed" % (web_ok, web_fail))
    print("modbus %d ok, %d failed  (device counted %d requests, %d sessions)"
          % (mb_ok, mb_fail, end["requests"], end["sessions"]))
    print("heap   %d -> %d KB, low-water %d -> %d KB"
          % (heap_first // 1024, end["heap"] // 1024,
             heap_min_first // 1024, end["heap_min"] // 1024))
    leaked = heap_min_first - end["heap_min"]
    print("low-water fell by %d bytes over the run%s"
          % (leaked, "" if leaked < 4096 else "  <-- look into this"))

    # What counts as a failed soak.
    #
    # Not "any failed request". A connection abandoned by the test itself can
    # hold a listener for the couple of seconds the recycle takes, so a handful
    # of refusals in several thousand is the workaround working, not a defect --
    # a 30-minute run measured 1 in 5522. Failing on that would train everyone
    # to ignore the exit code.
    #
    # A rate above half a percent is different: that is a server losing
    # connections rather than covering for one. So is any real movement in the
    # heap low-water mark, which only ever falls.
    total = web_ok + web_fail + mb_ok + mb_fail
    rate = (web_fail + mb_fail) / total if total else 0
    print("failure rate %.3f%%" % (rate * 100))
    sys.exit(1 if (rate > 0.005 or leaked >= 4096) else 0)
except Exception as exc:                        # noqa: BLE001 - reported
    print("device did not answer at the end: %s" % type(exc).__name__)
    sys.exit(1)
