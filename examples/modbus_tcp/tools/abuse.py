#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Treat the device badly, then check it still works.

    python abuse.py 192.168.11.2

Every case here is something a real client does by accident -- a browser
navigating away, a proxy that truncates, a master that is reset mid-poll -- and
each one is followed by a check that the server still answers correctly. That
second half is the point: a device that survives an abusive request but stops
serving the next good one has failed, and a test that only checks for a crash
would call it a pass.

Exits non-zero if any recovery check fails.
"""
import socket
import struct
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.11.2"
failures = 0


def check(label, ok, detail=""):
    global failures
    if not ok:
        failures += 1
    print("  %-44s %s%s" % (label, "OK" if ok else "FAIL", detail))


# ---- recovery checks -------------------------------------------------------

def web_alive(tries=25):
    """The web UI answers a normal request. Retried, because the listener
    recycle takes a couple of seconds and that is a recovery, not a failure."""
    for _ in range(tries):
        try:
            s = socket.create_connection((host, 80), timeout=3)
            s.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n")
            buf = b""
            while b"\r\n\r\n" not in buf:
                c = s.recv(4096)
                if not c:
                    break
                buf += c
            s.close()
            if b"200 OK" in buf:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def modbus_alive(port=502, tries=15):
    for _ in range(tries):
        try:
            s = socket.create_connection((host, port), timeout=3)
            s.sendall(struct.pack("!HHHB", 1, 0, 6, 1) +
                      struct.pack("!BHH", 0x03, 0, 1))
            head = s.recv(9)
            s.close()
            if len(head) >= 9 and head[7] == 0x03:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def modbus_port():
    s = socket.create_connection((host, 80), timeout=5)
    s.sendall(b"GET /api/config HTTP/1.1\r\nHost: x\r\n\r\n")
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    body = buf.split(b"\r\n\r\n", 1)[1]
    while True:
        c = s.recv(4096)
        if not c:
            break
        body += c
    s.close()
    import json
    return json.loads(body)["port"]


PORT = modbus_port()
print("device on %s, Modbus port %d\n" % (host, PORT))

# ---- HTTP abuse ------------------------------------------------------------

print("--- HTTP")

# Connect and vanish. This is the one that used to kill a listener outright:
# the socket ends up in CLOSE_WAIT and accept() never advances past it.
for _ in range(6):
    try:
        s = socket.create_connection((host, 80), timeout=3)
        s.close()
    except OSError:
        pass
check("connect and close, six times", web_alive())

# Half a request, then gone.
for _ in range(6):
    try:
        s = socket.create_connection((host, 80), timeout=3)
        s.sendall(b"GET /api/sta")
        s.close()
    except OSError:
        pass
check("truncated request line, six times", web_alive())

# Abort with RST rather than FIN: the peer disappears without a handshake.
for _ in range(4):
    try:
        s = socket.create_connection((host, 80), timeout=3)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        s.close()                       # SO_LINGER 0 -> RST
    except OSError:
        pass
check("RST mid-response, four times", web_alive())

# A slow client that never finishes: one byte every 300 ms, which resets a
# per-read timeout forever. REQUEST_DEADLINE_MS is what has to stop this.
t0 = time.time()
try:
    s = socket.create_connection((host, 80), timeout=3)
    for ch in b"GET / HTTP/1.1\r\nHost: x\r\n":
        s.sendall(bytes([ch]))
        time.sleep(0.3)
        if time.time() - t0 > 12:
            break
    s.close()
except OSError:
    pass
check("slow client held off for %.0f s" % (time.time() - t0), web_alive())

# Bigger than the request buffer.
try:
    s = socket.create_connection((host, 80), timeout=5)
    s.sendall(b"GET /" + b"a" * 4000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
    s.recv(256)
    s.close()
except OSError:
    pass
check("oversized request line", web_alive())

# Not HTTP at all.
for junk in (b"\x00\x01\x02\x03\r\n\r\n", b"HELLO\r\n\r\n",
             b"POST /api/config HTTP/1.1\r\nContent-Length: 99999\r\n\r\n{}"):
    try:
        s = socket.create_connection((host, 80), timeout=3)
        s.sendall(junk)
        s.settimeout(3)
        try:
            s.recv(256)
        except OSError:
            pass
        s.close()
    except OSError:
        pass
check("junk and a lying Content-Length", web_alive())

# Malformed JSON on the settings endpoint: none of it may be applied.
before = modbus_port()
for body in (b"{", b"{}", b'{"ip":}', b'{"ip":"' + b"9" * 200 + b'"}',
             b'{"port":"not a number"}', b'{"ip":null}'):
    try:
        s = socket.create_connection((host, 80), timeout=5)
        s.sendall(b"POST /api/config HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n"
                  % len(body) + body)
        s.recv(512)
        s.close()
    except OSError:
        pass
    time.sleep(0.2)
check("malformed JSON left the port alone", modbus_port() == before)

# ---- Modbus abuse ----------------------------------------------------------

print("\n--- Modbus")


def raw(payload, read=True):
    try:
        s = socket.create_connection((host, PORT), timeout=3)
        s.sendall(payload)
        if read:
            s.settimeout(2)
            try:
                s.recv(256)
            except OSError:
                pass
        s.close()
    except OSError:
        pass


for _ in range(6):
    raw(b"", read=False)
check("connect and close, six times", modbus_alive(PORT))

# A header that promises more than it sends.
raw(struct.pack("!HHHB", 1, 0, 250, 1) + b"\x03\x00")
check("MBAP length larger than the body", modbus_alive(PORT))

# Half an MBAP header, then gone.
raw(b"\x00\x01\x00")
check("truncated MBAP header", modbus_alive(PORT))

# Protocol id that is not Modbus.
raw(struct.pack("!HHHB", 1, 0x1234, 6, 1) + struct.pack("!BHH", 0x03, 0, 1))
check("non-zero protocol id", modbus_alive(PORT))

# Lengths the spec forbids.
for length in (0, 1, 0xFFFF):
    raw(struct.pack("!HHHB", 1, 0, length, 1) + b"\x03\x00\x00\x00\x01")
check("MBAP lengths 0, 1 and 65535", modbus_alive(PORT))

# Every function code, defined or not. Each must be answered, not dropped.
answered = 0
try:
    s = socket.create_connection((host, PORT), timeout=5)
    for code in range(1, 128):
        s.sendall(struct.pack("!HHHB", code, 0, 6, 1) +
                  struct.pack("!BHH", code, 0, 1))
        head = s.recv(9)
        if len(head) < 9:
            break
        body_len = struct.unpack("!H", head[4:6])[0] - 1
        remaining = body_len - 2
        while remaining > 0:
            chunk = s.recv(remaining)
            if not chunk:
                break
            remaining -= len(chunk)
        answered += 1
    s.close()
except OSError:
    pass
check("all 127 function codes answered on one socket", answered == 127,
      "  (%d answered)" % answered)

# Rapid connect/disconnect, the way a master with a short retry timer behaves.
for _ in range(30):
    try:
        s = socket.create_connection((host, PORT), timeout=2)
        s.sendall(struct.pack("!HHHB", 1, 0, 6, 1) +
                  struct.pack("!BHH", 0x03, 0, 1))
        s.close()
    except OSError:
        pass
check("thirty rapid sessions", modbus_alive(PORT))

print("\n--- both still healthy together")
check("web", web_alive())
check("Modbus", modbus_alive(PORT))

print("\n%d failures" % failures)
sys.exit(1 if failures else 0)
