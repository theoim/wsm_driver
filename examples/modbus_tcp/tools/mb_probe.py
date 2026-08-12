#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Exercise every function code the Modbus TCP example implements, and report.

Usage:
    python mb_probe.py 192.168.11.2          # Ethernet, port 502
    python mb_probe.py 192.168.11.8 5020     # Wi-Fi

Needs nothing but a Python 3 install -- MBAP is 7 bytes and struct can build it,
so there is no library to fetch onto a machine that may not have one. Point it
at the device and it prints a pass/fail line per operation; a Modbus GUI shows
you register values, this shows you whether the server is correct.

What it checks beyond the values themselves:

  - Every reply's transaction id, protocol id and unit id are matched against
    the request. A server that hardcodes any of them looks fine to a polling
    tool, which usually sends one request at a time and never notices.
  - Writes are read back rather than trusted.
  - The 126-and-up paths: a 10-register read is 22 bytes of response, past the
    point where a length field has to be right.
  - Refusals. An out-of-range address, a zero count and an undefined function
    code must each come back as a proper exception WITH THE CONNECTION INTACT.
    Dropping the socket instead is the common way to get this wrong, and it is
    invisible until a master retries.

Everything runs down one socket, which is what a real master does: Modbus TCP
keeps the connection open across polls.
"""
import socket
import struct
import sys

if len(sys.argv) < 2:
    sys.exit(__doc__)

host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 502

sock = socket.create_connection((host, port), timeout=10)
tid = 0
failures = 0


def request(pdu, unit=1):
    """Send one PDU, return the response PDU. Raises if the MBAP does not match."""
    global tid
    tid = (tid + 1) & 0xFFFF
    sock.sendall(struct.pack("!HHHB", tid, 0, len(pdu) + 1, unit) + pdu)

    head = b""
    while len(head) < 7:
        chunk = sock.recv(7 - len(head))
        if not chunk:
            raise RuntimeError("closed while reading the MBAP header")
        head += chunk

    rtid, rpid, rlen, runit = struct.unpack("!HHHB", head)
    if rtid != tid:
        raise RuntimeError("transaction id %d echoed as %d" % (tid, rtid))
    if rpid != 0:
        raise RuntimeError("protocol id %d, expected 0" % rpid)
    if runit != unit:
        raise RuntimeError("unit id %d echoed as %d" % (unit, runit))

    body = b""
    while len(body) < rlen - 1:
        chunk = sock.recv(rlen - 1 - len(body))
        if not chunk:
            raise RuntimeError("closed mid-PDU")
        body += chunk
    return body


def check(label, got, want):
    global failures
    ok = got == want
    if not ok:
        failures += 1
    print("%-22s -> %s  %s" % (label, got, "OK" if ok else "MISMATCH, want %s" % (want,)))


def regs(pdu):
    """Decode a read-registers response body into a list of ints."""
    return list(struct.unpack("!%dH" % (pdu[1] // 2), pdu[2:2 + pdu[1]]))


def bits(pdu, count):
    packed = pdu[2:2 + pdu[1]]
    return [(packed[i // 8] >> (i % 8)) & 1 for i in range(count)]


print("--- reads (the startup pattern from mb_datastore_init)")
check("0x03 holding[0:10]", regs(request(struct.pack("!BHH", 0x03, 0, 10))),
      list(range(1000, 1010)))
check("0x04 input[0:8]", regs(request(struct.pack("!BHH", 0x04, 0, 8))),
      [i * i for i in range(8)])
check("0x01 coils[0:16]", bits(request(struct.pack("!BHH", 0x01, 0, 16)), 16),
      [1 if i % 2 == 0 else 0 for i in range(16)])
check("0x02 discrete[0:16]", bits(request(struct.pack("!BHH", 0x02, 0, 16)), 16),
      [1 if i % 4 == 0 else 0 for i in range(16)])

print("\n--- writes, each read back")
request(struct.pack("!BHH", 0x06, 5, 0xBEEF))
check("0x06 holding[5]", regs(request(struct.pack("!BHH", 0x03, 5, 1))), [0xBEEF])

request(struct.pack("!BHHBHH", 0x10, 20, 2, 4, 0x1111, 0x2222))
check("0x10 holding[20:22]", regs(request(struct.pack("!BHH", 0x03, 20, 2))),
      [0x1111, 0x2222])

request(struct.pack("!BHH", 0x05, 1, 0xFF00))
check("0x05 coil[1]", bits(request(struct.pack("!BHH", 0x01, 1, 1)), 1), [1])

request(struct.pack("!BHHBB", 0x0F, 8, 4, 1, 0b1010))
check("0x0F coils[8:12]", bits(request(struct.pack("!BHH", 0x01, 8, 4)), 4),
      [0, 1, 0, 1])

print("\n--- refusals (the server must answer, not drop the connection)")
check("0x03 addr 9000", request(struct.pack("!BHH", 0x03, 9000, 1)),
      b"\x83\x02")                      # illegal data address
check("0x03 count 0", request(struct.pack("!BHH", 0x03, 0, 0)),
      b"\x83\x03")                      # illegal data value
check("0x42 undefined", request(struct.pack("!BHH", 0x42, 0, 1)),
      b"\xc2\x01")                      # illegal function

print("\n%d requests on one connection, %d failures" % (tid, failures))
sock.close()
sys.exit(1 if failures else 0)
