# How to Test Modbus TCP Example

> **Verified on a W5500.** Run on an XIAO ESP32-S3 + W5500, TOE backend, over Ethernet and over Wi-Fi. Every implemented function code and every exception path was exercised against the running device.

A Modbus TCP server (slave). A master reads and writes the device's registers and coils over port 502, which is what a PLC, a SCADA package, or a `mbpoll` command line expects to find.

## Step 1: Prepare software

A serial terminal and any Modbus master.

- [Tera Term][link-tera_term]
- A Modbus master — [mbpoll][link-mbpoll] (command line, Windows/Linux/macOS), [Modbus Poll][link-modbus_poll] (Windows, GUI), or a few lines of Python

There is nothing to install on the device side and no vendor tool involved: the protocol is the interface.

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-modbus-tcp-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

## Step 3: Setup Modbus TCP Example

### Chip and SPI configuration

Set the target and open menuconfig:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Select **Component config**, then **WIZnet WSM Driver**, then choose the **Board**. The chip and its pins both follow from that, because picking the chip alone cannot express the wiring — the W6300 Dev-kit and the W6300 SoM carry the same chip on different GPIOs. For wiring no listed board covers, choose **Custom**: that is the one setting where the pin fields become editable.

> Do not set `CONFIG_WSM_DRIVER_CHIP_*` by hand in `sdkconfig` or `sdkconfig.defaults`. It contradicts whichever board is selected and leaves the driver talking to the wrong chip on the right pins.

**W5500 wiring (standard SPI)**

| W5500 | ESP32-S3 Pin |
|-------|--------------|
| MISO  | 13 |
| MOSI  | 11 |
| SCLK  | 12 |
| CS    | 10 |
| RESET | 9  |
| INT   | 14 |

**W6300 wiring (QSPI)**

| W6300   | ESP32-S3 Pin |
|---------|--------------|
| D0 (MOSI) | 11 |
| D1 (MISO) | 13 |
| D2 (IO2)  | 14 *(Quad mode only)* |
| D3 (IO3)  | 9  *(Quad mode only)* |
| SCLK    | 12 |
| CS      | 10 |
| RESET   | 21 |
| INT     | 8  |

> If your board puts RSTn or INTn on **GPIO43/44**, those are the ESP32-S3's UART0 console pins. Move the console to `Component config -> ESP System Settings -> Channel for console output -> USB Serial/JTAG Controller`, or the reset line and the console will fight over the same pin.

### Network configuration

Network identity and the ports live in `examples/modbus_tcp/inc/net_config.h`, the same way as in `examples/loopback`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define MB_PORT               502     /* Ethernet */
#define WIFI_MB_PORT          5020    /* Wi-Fi    */
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

Port 502 is the registered Modbus port, so a master reaches the device with no port argument at all. The two interfaces bind different ports because with `SOCKET_WRAP=0` (esp_eth backend) they share one LwIP stack, where the same port would clash on bind.

### Architecture

Same layout as `examples/loopback`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, ports |
| `inc/mb_core.h` · `src/mb_core.c` | the data model and the PDU executor |
| `inc/mb_transport.h` · `src/mb_transport.c` | the network seam |
| `src/mb_server.c` | one session: MBAP framing, execute, reply |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |

`mb_core.c` touches no socket, no timer and no UART. `mb_pdu_execute()` is a pure function over a caller-owned `mb_datastore_t` — hand it a request PDU, get a response PDU back — which is what makes it identical on both network backends and testable on a host. Only `mb_transport.c` includes lwIP, and it calls the component's `net_sock_ops_t` vtable, so the same server runs on the WIZnet hardware sockets or on the software LwIP behind the Wi-Fi netif.

### Why the protocol is written here rather than ported

Modbus TCP is small enough to read in full, and most of the bulk of a general Modbus library is the part TCP does not use: CRC-16, the 3.5-character inter-frame timer, ASCII framing, RTU/ASCII transcoding. Over TCP, framing is the 7-byte MBAP header with an explicit length, and the transport already guarantees ordering and integrity. What is left is `mb_core.c`.

```
    <---------------- Modbus TCP ADU ---------------->
                      <-------- PDU ---------->
    +-----+-----+-----+-----+------+------------------+
    | TID | PID | Len | UID | Func | Data             |
    +-----+-----+-----+-----+------+------------------+
       2     2     2     1      1     0..252 bytes
```

TID is echoed untouched — the master pairs replies with it — PID is 0 for Modbus, and Len counts UID onward, so `Len == PDU length + 1`. `mb_server.c` reads the 7-byte header first and only then the number of bytes it announced, so the server never has to guess how much is in flight or resynchronise a stream that got out of step.

### Function codes

| Code | Function | Code | Function |
|---|---|---|---|
| 0x01 | Read Coils | 0x05 | Write Single Coil |
| 0x02 | Read Discrete Inputs | 0x06 | Write Single Register |
| 0x03 | Read Holding Registers | 0x0F | Write Multiple Coils |
| 0x04 | Read Input Registers | 0x10 | Write Multiple Registers |

Anything else is answered with exception 0x01 (illegal function). That is the correct reply rather than a gap — a master is required to handle exceptions, and on Modbus a refusal *is* a response, so the connection stays up.

### The data model

64 registers and 64 bits of each kind, filled at startup with a pattern a master can recognise at a glance, so a wrong address or a byte-order mistake is visible without a debugger:

| Table | Access | Initial contents |
|---|---|---|
| Holding registers | read/write | 1000, 1001, 1002, ... |
| Input registers | read only | 0, 1, 4, 9, 16, ... (i²) |
| Coils | read/write | alternating, even addresses on |
| Discrete inputs | read only | every fourth address on |

Registers are plain `uint16_t` in host order; the wire's big-endian layout is applied at the edge in `mb_pdu_execute`, so application code never has to think about it. Coils are one `bool` per element rather than packed bits: the packing is a wire format, and unpacking it in the data model would push that detail into every caller that wants to read a coil.

### One master at a time

That is a property of the chip, not a shortcut. The TOE has no separate accepted socket: the listening hardware socket *becomes* the connection, so `accept()` returns the same descriptor it was given, where LwIP returns a new one and keeps the listener. The component absorbs the difference on the way out — closing an accepted listener reopens and re-listens it — so the ordinary accept / serve / close loop works unchanged on both backends. Serving several masters from one listening socket does not carry over; on the TOE that needs one hardware socket per client, which is what `examples/tcp_server_multi_socket` demonstrates. One master at a time is the usual Modbus TCP arrangement anyway.

Each interface gets its own `mb_datastore_t`. Sharing one across both would need a lock, and the point of the example is the protocol, not the mutex — writing over Ethernet and reading over Wi-Fi are two independent slaves here.

## Step 4: Build

After completing the setup, build the project.

```bash
idf.py build
```

## Step 5: Upload and Run

On Windows:

```bash
idf.py -p COM6 flash monitor
```

On Linux/macOS:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

If flashing succeeds, the chip is identified and the server comes up.

```
I (360) wsm_driver_spi: W5500 version check OK: 0x04
I (362) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (447) mb_server: [eth] waiting for link...
I (451) mb_server: [eth] Modbus TCP server on port 502
```

Point a master at it. With `mbpoll`, reading ten holding registers from address 1:

```bash
mbpoll -m tcp -a 1 -r 1 -c 10 192.168.11.2
```

```
[1]:    1000
[2]:    1001
[3]:    1002
...
```

Writing a single register and reading it back:

```bash
mbpoll -m tcp -a 1 -r 6 192.168.11.2 48879
mbpoll -m tcp -a 1 -r 6 -c 1 192.168.11.2
```

The serial log names each function code and the size of the reply it produced:

```
I (486591) mb_tx: master connected from 192.168.11.4
I (486591) mb_server: [eth] session open
I (486608) mb_server: [eth] function 0x03 -> 22 bytes
I (486614) mb_server: [eth] function 0x04 -> 18 bytes
I (486621) mb_server: [eth] function 0x01 -> 4 bytes
I (486627) mb_server: [eth] function 0x02 -> 4 bytes
I (486632) mb_server: [eth] function 0x06 -> 5 bytes
I (486643) mb_server: [eth] function 0x10 -> 5 bytes
I (486654) mb_server: [eth] function 0x05 -> 5 bytes
I (486669) mb_server: [eth] function 0x0F -> 5 bytes
W (486682) mb_server: [eth] function 0x03 refused: exception 0x02
W (486688) mb_server: [eth] function 0x03 refused: exception 0x03
W (486693) mb_server: [eth] function 0x42 refused: exception 0x01
I (486696) mb_server: [eth] session closed
```

All of that is one TCP connection — Modbus TCP keeps the socket open across polls, and a master may issue thousands of requests on it. The three refusals at the end are worth provoking on purpose: an out-of-range address, a zero register count, and an undefined function code. The server answers each and the session continues, which is the behaviour a master expects.

### Over Wi-Fi as well

Filling in `WIFI_SSID` starts a second server on the Wi-Fi interface, and the two run side by side. It comes up on its own schedule, once the station holds a DHCP lease:

```
I (31790) esp_netif_handlers: sta ip: 192.168.11.8, mask: 255.255.255.0, gw: 192.168.11.1
I (31791) wifi: got IP 192.168.11.8
I (31803) mb_server: [wifi] Modbus TCP server on port 5020
```

Address that one **with the port**: `mbpoll -m tcp -p 5020 -a 1 -r 1 -c 10 192.168.11.8`. Port 502 belongs to the Ethernet server, so a master given no port will always land there.

Leave a master polling over Ethernet while you do it. Both stay up, and the log prefix says which one is talking. That is the whole point of the example: identical protocol code driving WIZnet hardware sockets and software LwIP at the same time, differing only in the socket vtable each connection carries.

## Troubleshooting

### `listen(502) failed: errno 95`, right after a version mismatch

Look one line up. If the boot log says

```
E (440) wsm_driver_spi: W5500 version mismatch: 0x00 (expected 0x04)
```

the chip is not answering on SPI at all, and the listen failure is a consequence, not the fault. Wrong board selected, wrong pins, or the RST/INT lines colliding with the console — see the GPIO43/44 note in [Step 3](#step-3-setup-modbus-tcp-example).

### The master reports "illegal data address"

Exception 0x02, and usually an off-by-one rather than a bug. Modbus documentation numbers holding registers from 40001 and coils from 1, while the wire carries a zero-based address; `mbpoll -r 1` sends address 0. This server holds 64 of each, so any address past 63 — or a read whose count runs past 63 — is refused.

### The master reports "illegal data value"

Exception 0x03. A count of zero, a count past the protocol's own ceiling (125 registers or 2000 bits per read), or a write whose byte count does not agree with its element count. The last one is worth knowing about: the byte count in a multiple-write request is redundant with the element count, so a mismatch means the frame contradicts itself and neither number can be trusted.

### Wi-Fi answers a few requests and then stalls

Not a Modbus problem. If replies arrive normally and then latency climbs — measured here at 45 ms, then 1.2 s, then 1.5 s, then nothing — the station is losing frames to power save. The default `WIFI_PS_MIN_MODEM` (the `wifi:pm start, type: 1` line at boot) wakes the radio on a listen interval of 307 ms scaled by the AP's DTIM period, and an AP that buffers poorly drops what does not fit.

On this setup, with `esp_wifi_set_ps(WIFI_PS_NONE)` after `esp_wifi_start()`, the same fifteen-request sequence completed twice with no delay at all; with the default it failed at the second and fifth request on consecutive runs. The DHCP lease shows the same split even earlier in the boot — 31.8 s, 90.6 s and 173.1 s on the default against 2.2 s with power save off, on the same board with the antenna untouched and RSSI between -28 and -29 dBm.

It is left at the default here because it costs idle current and most APs do not show the problem. Request/response protocols expose it more than streaming ones do — a browser reconnects and hides it, a Modbus master times out and reports a failure.

## Appendix

- **Widening the data model:** `MB_REG_COUNT` and `MB_COIL_COUNT` in `inc/mb_core.h`. Nothing else needs to change; the bounds checks and the exception replies follow from them.
- **Wiring it to real I/O:** `mb_datastore_t` is the whole interface between Modbus and the application. Read `ds->coil[n]` where a GPIO should be driven, write `ds->input[n]` where a sensor value should be published, and the protocol side needs no edits.
- **Unit id:** echoed, not checked. On serial Modbus it addresses one slave on a shared bus; over TCP the IP address does that job, and the spec's own guidance is to echo whatever arrived. A gateway to a serial segment would be the case where it starts to matter.
- **The WIZnet S2E Modbus stack:** [W55RP20-S2E][link-s2e] carries a Modbus implementation under `port/app/modbus`, and it solves a different problem — it is a TCP↔RTU/ASCII *gateway* (`mbTCPtoRTU`, `mbRTUtoTCP`), so it forwards frames to a serial slave rather than answering them, and it calls ioLibrary's hardware sockets directly (`getSn_RX_RSR`, `recv(sn, ...)`) rather than BSD ones, which ties it to the TOE backend and rules out the Wi-Fi path. Useful as a reference for the MBAP layout; not a drop-in for a standalone slave.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-mbpoll]: https://github.com/epsilonrt/mbpoll
[link-modbus_poll]: https://www.modbustools.com/modbus_poll.html
[link-s2e]: https://github.com/WIZnet-ioNIC/W55RP20-S2E
