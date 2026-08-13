#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Exercise the web UI: the read endpoints, and every way a settings POST
should be refused.

    python web_probe.py 192.168.11.2

Needs nothing but a Python 3 install. Runs read-only by default -- the address
and port are left exactly as they were found, because a probe that moves the
device it is testing is a probe you can only run once.

Pass --apply to include the round trip that actually changes the Modbus port and
puts it back. That one takes about ten seconds and briefly interrupts any master
that is polling.
"""
import json
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.11.2"
do_apply = "--apply" in sys.argv
failures = 0


def request(method, path, body=None, timeout=10):
    s = socket.create_connection((host, 80), timeout=timeout)
    payload = (body or "").encode()
    head = "%s %s HTTP/1.1\r\nHost: %s\r\n" % (method, path, host)
    if body is not None:
        head += "Content-Type: application/json\r\nContent-Length: %d\r\n" % len(payload)
    s.sendall((head + "\r\n").encode() + payload)

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("closed with no response")
        buf += chunk
    head_bytes, _, rest = buf.partition(b"\r\n\r\n")
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        rest += chunk
    s.close()
    status = head_bytes.split(b"\r\n", 1)[0].decode()
    return status, rest


def check(label, ok, detail=""):
    global failures
    if not ok:
        failures += 1
    print("  %-46s %s%s" % (label, "OK" if ok else "FAIL", detail))


print("--- read endpoints")
status, body = request("GET", "/")
check("GET /", "200" in status and b"<html" in body.lower(),
      "  (%d bytes)" % len(body))

status, body = request("GET", "/api/status")
st = json.loads(body)
check("GET /api/status", "200" in status)
print("      %s:%s  running=%s requests=%s exceptions=%s"
      % (st["ip"], st["port"], st["running"], st["requests"], st["exceptions"]))

status, body = request("GET", "/api/registers")
regs = json.loads(body)
check("GET /api/registers",
      len(regs["holding"]) == st["regs"] and len(regs["coil"]) == st["coils"],
      "  (holding[0]=%s)" % regs["holding"][0])

status, body = request("GET", "/api/config")
cfg = json.loads(body)
check("GET /api/config", cfg["ip"] == st["ip"] and cfg["port"] == st["port"])

status, _ = request("GET", "/api/nope")
check("GET /api/nope -> 404", "404" in status)

print("\n--- the register view follows the Modbus side")
# Write through Modbus, then read it back through HTTP: this is the whole point
# of the shared data model, and the only test that can tell it apart from two
# separate copies that happen to start identical.
import struct
m = socket.create_connection((host, st["port"]), timeout=10)
m.sendall(struct.pack("!HHHB", 1, 0, 6, 1) + struct.pack("!BHH", 0x06, 3, 0xCAFE))
m.recv(256)
m.close()
time.sleep(0.3)
_, body = request("GET", "/api/registers")
check("holding[3] written over Modbus reads 0xCAFE via HTTP",
      json.loads(body)["holding"][3] == 0xCAFE)

print("\n--- settings must be refused, and the device must not move")
before = cfg.copy()
for label, payload in [
    ("IP is not an address",        '{"ip":"192.168.11"}'),
    ("IP octet over 255",           '{"ip":"192.168.11.999"}'),
    ("IP has trailing junk",        '{"ip":"192.168.11.2x"}'),
    ("loopback address",            '{"ip":"127.0.0.1"}'),
    ("multicast address",           '{"ip":"224.0.0.5"}'),
    ("non-contiguous mask",         '{"mask":"255.255.0.255"}'),
    ("gateway outside the subnet",  '{"gateway":"10.0.0.1"}'),
    ("port 0",                      '{"port":0}'),
    ("port over 65535",             '{"port":70000}'),
    ("port collides with the web UI", '{"port":80}'),
]:
    status, body = request("POST", "/api/config", payload)
    refused = "400" in status
    reason = ""
    try:
        reason = "  (%s)" % json.loads(body)["error"]
    except Exception:                          # noqa: BLE001 - cosmetic only
        pass
    check(label, refused, reason)

_, body = request("GET", "/api/config")
check("configuration unchanged after all refusals",
      json.loads(body) == before)

if do_apply:
    print("\n--- apply a port change, then put it back")
    new_port = 1502 if before["port"] != 1502 else 1503
    status, body = request("POST", "/api/config", '{"port":%d}' % new_port)
    check("POST accepted", "200" in status and json.loads(body)["ok"])

    ok = False
    for _ in range(30):
        time.sleep(0.5)
        try:
            m = socket.create_connection((host, new_port), timeout=2)
            m.close()
            ok = True
            break
        except OSError:
            pass
    check("Modbus answers on the new port %d" % new_port, ok)

    _, body = request("GET", "/api/status")
    check("status reports the new port", json.loads(body)["port"] == new_port)

    request("POST", "/api/config", '{"port":%d}' % before["port"])
    ok = False
    for _ in range(30):
        time.sleep(0.5)
        try:
            m = socket.create_connection((host, before["port"]), timeout=2)
            m.close()
            ok = True
            break
        except OSError:
            pass
    check("restored to port %d" % before["port"], ok)
else:
    print("\n(skipping the apply round trip; pass --apply to include it)")

print("\n%d failures" % failures)
sys.exit(1 if failures else 0)
