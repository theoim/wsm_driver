# How to Test Modbus TCP Example

> **Verified on hardware, with the I/O binding partly covered.** The server was run on the ESP32-SoM with both the W5500 and the W6300, TOE backend, over Ethernet and over Wi-Fi — including both interfaces serving at the same time — and every implemented function code and every exception path was exercised against the running device. On the XIAO ESP32-S3 board this example's `sdkconfig` targets, the output side was then run for real: the coil clears at boot instead of inheriting the demo pattern, a master's write drives the pin and reads back, and the link survives the toggle. **The input side is still unverified** — no contact has been closed on it, and the debounce has not been measured. No relay board was involved either; the output was checked by what the pin did, not by what a relay did.

A Modbus TCP server (slave). A master reads and writes the device's registers and coils over port 502, which is what a PLC, a SCADA package, or a `mbpoll` command line expects to find.

Two elements of that data model are wired to pins: **coil 0 drives GPIO1**, and **GPIO4 is published as discrete input 0**. A coil written from Modbus Poll closes a relay on the bench, and a contact closed on the bench appears in the master's grid. Everything else in the model stays a pattern in RAM — the binding is two elements on purpose, and it widens by changing an index range.

> **Anything that can reach this device can close that relay.** Modbus TCP on port 502 has no authentication, and neither does the web UI on port 80. With a pin bound, an unauthenticated write is no longer a number changing in RAM — it is a contact closing. Keep this on a bench or an isolated segment, and read [Known limits](#known-limits) before it goes anywhere that matters.

## Step 1: Prepare software

A serial terminal and a Modbus master. There is nothing to install on the device side and no vendor tool involved — the protocol is the interface, so anything that speaks Modbus TCP will do.

- [Tera Term][link-tera_term] for the serial log

### Choosing a master

| Tool | Platform | Cost | Good for |
|---|---|---|---|
| **`tools/mb_probe.py`** (in this example) | any, Python 3 | free | one command, checks correctness rather than showing values |
| [mbpoll][link-mbpoll] | Windows / Linux / macOS | free | quick one-liners, scripting, CI |
| [Modbus Poll][link-modbus_poll] | Windows | trial, then paid | the industry default; live register grid, logging |
| [QModMaster][link-qmodmaster] | Windows / Linux | free | a GUI without a licence, good for a quick look |
| [pymodbus][link-pymodbus] | any, Python 3 | free | writing your own master or automated tests |

**Start with `tools/mb_probe.py`.** It ships with the example, needs no install — MBAP is 7 bytes and `struct` can build it — and checks things a polling tool will not:

- Every reply's transaction id, protocol id and unit id are matched against the request. A server that hardcodes any of them looks correct to a GUI, which sends one request at a time and never notices.
- Writes are read back rather than trusted.
- Refusals are verified to arrive **with the connection intact**. Dropping the socket instead of answering is the common way to get a Modbus server wrong, and it stays invisible until a master retries.

Reach for a GUI when you want to watch values move — `mb_probe.py` answers "is this server correct", not "what is register 7 doing right now".

> **Modbus Poll is not the same program as mbpoll.** The first is a commercial Windows GUI from Modbus Tools; the second is a free command-line tool. Similar names, unrelated projects.

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-modbus-tcp-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.
4. Wire the two bound pins, if you want the I/O half of the example. The server runs without them.

### I/O wiring

| Signal | ESP32-S3 | Connect to | Notes |
|---|---|---|---|
| Output — coil 0 | GPIO1 | relay board `IN` | active-low by default |
| Input — discrete input 0 | GPIO4 | dry contact to GND | internal pull-up; closed reads as 1 |
| Ground | GND | relay board GND, contact return | one common ground, or neither side means anything |

The pins, the active levels and the bound indices are the macros at the top of `inc/io_bind.h`.

**Check the pins against your own wiring, and check `sdkconfig` rather than the Kconfig defaults.** `CONFIG_WSM_DRIVER_BOARD_CUSTOM=y` means the defaults describe nothing that is actually connected — see [the wiring this example ships configured for](#the-wiring-this-example-ships-configured-for). The three pin sets this example has been built with:

| Wiring | WIZnet pins | Leaves free |
|---|---|---|
| W5500 dev-kit | 9-14 | 4, 5 |
| W6300 SoM | 33-42, plus 21 for RST | 4, 5 |
| XIAO ESP32-S3 (this example's `sdkconfig`) | 6, 7, 8, 9, 43, 44 | 1, 2, 3, 4, 5 |

GPIO1 and GPIO4 are clear of all three, and clear of the ESP32-S3's strapping pins (0, 3, 45, 46) and its native USB (19, 20).

> **A pin no table claims can still be tied to something.** The output was on GPIO5 until a bench run showed that driving it low takes the W5500 down — see [the relay pin kills Ethernet](#writing-coil-0-kills-the-ethernet-link). Free in the pin map is a necessary condition, not a sufficient one; the sufficient one is toggling it once with a master watching the link.

> **Binding a pin the Ethernet chip is using does not report an error — it takes the SPI bus down.** The board comes up saying `W5500 version mismatch: 0x00 (expected 0x04)`, which reads as a cable or a solder fault and sends you looking in the wrong place. `io_bind.h` compares both pins against `CONFIG_WSM_DRIVER_PIN_*` and fails the build instead, so change the macros and rebuild rather than reasoning about it.

> **Do not power a relay coil from the board's 3V3.** Use an opto-isolated relay module with its own supply and a common ground. A coil is an inductor across the same rail the Ethernet chip runs on, and the failure that causes does not look like a power problem — it looks like a link that drops whenever the contact does.

**Active level.** Most opto-isolated relay boards energise when `IN` is pulled *low*, and most field contacts close to ground, so both sides default to active-low (`IO_OUT_ACTIVE_LOW` and `IO_IN_ACTIVE_LOW` in `inc/io_bind.h`). Set either to `0` for a board that drives high. Getting this wrong does not fail loudly — the relay simply works backwards, which reads as a protocol bug and is not one.

The internal pull-up is weak, around 45 kOhm. It is enough for a contact on the bench; a run of any length wants an external resistor and some protection in front of the pin.

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

### The wiring this example ships configured for

The two tables above are the reference wiring for the WIZnet dev-kits. **The `sdkconfig` in this directory is not set to either of them.** It is set to a Seeed XIAO ESP32-S3 with a W5500 module wired to the XIAO's own pads:

| W5500 | XIAO pad | GPIO | `sdkconfig` |
|---|---|---|---|
| MOSI | D10 | 9 | `CONFIG_WSM_DRIVER_PIN_MOSI=9` |
| MISO | D5 | 6 | `CONFIG_WSM_DRIVER_PIN_MISO=6` |
| SCLK | D9 | 8 | `CONFIG_WSM_DRIVER_PIN_SCLK=8` |
| SCSn (CS) | D8 | 7 | `CONFIG_WSM_DRIVER_PIN_CS=7` |
| RSTn | D6 | 43 | `CONFIG_WSM_DRIVER_PIN_RST=43` |
| INTn | D7 | 44 | `CONFIG_WSM_DRIVER_PIN_INT=44` |
| 3.3V | 3.3V-OUT | — | — |
| GND | GND | — | — |

That is `CONFIG_WSM_DRIVER_BOARD_CUSTOM=y`, which is why the Kconfig defaults say nothing useful here: **read the pins out of `sdkconfig`, not out of the board list.** RSTn and INTn landing on GPIO43/44 is also why `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` is set — the note above, in the specific.

Wire it from one of the two dev-kit tables instead if that is the hardware you have, and change the six `CONFIG_WSM_DRIVER_PIN_*` values to match. Whichever wiring you use, the pins have to agree with `sdkconfig` or the chip does not answer on SPI at all.

**Do not connect VBUS.** The XIAO's 5V pad is USB bus voltage; the module runs from 3.3V-OUT.

### What is left over, and why the I/O sits where it does

The XIAO brings out eleven signal pads. Six of them are the table above:

```
   left                              right
   D0  / GPIO1   coil 0              VBUS      <- do not connect
   D1  / GPIO2   free                GND
   D2  / GPIO3   free (strapping)    3.3V-OUT
   D3  / GPIO4   discrete input 0    D10 / GPIO9   MOSI
   D4  / GPIO5   free*               D9  / GPIO8   SCLK
   D5  / GPIO6   MISO                D8  / GPIO7   CS
   D6  / GPIO43  RSTn                D7  / GPIO44  INTn
```

Five pads are free, all of them together at the top of the left side, and the two the example binds are the two nearest the module — short wires, and no reshuffling of anything already soldered.

`*` GPIO5 is free in every pin table and still unusable on the board this was run on: driving it low kills the Ethernet link. Verify a pad before binding it to anything.

Three points that decide the choice rather than decorate it:

- **GPIO3 (D2) is a strapping pin.** It selects the JTAG source at reset, so a level held on it while the board comes out of reset is not a level you chose. Free, but last in line: prefer D0 and D1 if the I/O is widened.
- **`/T` on the silkscreen means touch-capable, and can be ignored.** GPIO1-14 are touch channels on the ESP32-S3; unless the touch peripheral is initialised they are ordinary GPIOs. It is why D6 and D7 lack the mark — GPIO43/44 are outside the touch range, not because they are special.
- **There is exactly one GND pad and one 3.3V-OUT pad, and the module has both.** Take the relay board's ground from the protoboard's ground rail rather than stacking a second wire on the pad, and take its supply from its own source. This is the same warning as in [Step 2](#io-wiring), in the specific: on this board there is no second 3.3V pad to be tempted by.

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
| `inc/net_config.h` | factory defaults: network identity, ports |
| `inc/mb_core.h` · `src/mb_core.c` | the data model and the PDU executor |
| `inc/mb_store.h` · `src/mb_store.c` | that model shared with the web UI, under a lock, plus the counters |
| `inc/io_bind.h` · `src/io_bind.c` | coil 0 to an output pin, an input pin to discrete input 0 |
| `inc/mb_transport.h` · `src/mb_transport.c` | the network seam |
| `src/mb_server.c` | one session: MBAP framing, execute, reply |
| `inc/app_config.h` · `src/app_config.c` | runtime settings in NVS, and what makes one valid |
| `inc/app_control.h` · `src/app_control.c` | applying a change without cutting off the reply that announced it |
| `inc/web_server.h` · `src/web_server.c` | the dashboard and the settings API |
| `inc/web_page.h` | the page, as a string literal |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |
| `tools/mb_probe.py` | a dependency-free master that checks every function code |
| `tools/web_probe.py` | the HTTP API, and ten ways a settings POST must be refused |
| `tools/abuse.py` | clients that misbehave, and whether the device still serves afterwards |
| `tools/soak.py` | the whole mix for as long as you like, reporting heap and failures |

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
| Coils | read/write | alternating, even addresses on — except coil 0, see below |
| Discrete inputs | read only | every fourth address on — except discrete input 0 |

Registers are plain `uint16_t` in host order; the wire's big-endian layout is applied at the edge in `mb_pdu_execute`, so application code never has to think about it. Coils are one `bool` per element rather than packed bits: the packing is a wire format, and unpacking it in the data model would push that detail into every caller that wants to read a coil.

**The two bound elements are not part of the pattern.** `coil[0]` is a relay, not a display value, so `io_bind_start()` drives the pin inactive and then clears the coil before any server starts — otherwise the even-addresses-on pattern would close the contact on the way out of reset, before a master had said anything. `discrete[0]` is overwritten every 20 ms by whatever the input pin reads. `mb_core.c` seeds the pattern and knows nothing about either; the override belongs to the layer that did the wiring.

### Binding the model to pins

`mb_datastore_t` is the whole interface between Modbus and an application, and `io_bind.c` is one worked example of using it:

```c
coil[0]     -> GPIO1   /* a master writes it, the pin follows  */
discrete[0] <- GPIO4   /* the pin changes, a master reads it   */
```

A task polls the pair every `IO_POLL_MS` (20 ms) rather than a callback firing from the write path. Two reasons, and the second is the one that decides it:

- The write path runs inside the store lock. Driving a pin from there stretches that lock out to hardware on every coil write, and this example holds the lock over memory and nothing else — a rule that is easy to keep until a binding layer arrives.
- Nothing in the protocol path is triggered by a contact closing, so a write-path callback would leave the input side without a home anyway. One task does both.

20 ms bounds how long a master waits to see its own write on the pin, which is invisible next to the 5-10 ms a relay's own contacts take to travel. The input needs `IO_DEBOUNCE_SAMPLES` consecutive equal samples before it is published, so a closure appears about 60 ms after it happens. Without that, a bouncing contact publishes a burst of transitions and the value visibly flickers in a polling GUI.

The debounce counts agreement rather than time since the last edge. A timer restarted on every edge can be held off indefinitely by a contact that keeps bouncing; a run of equal samples cannot.

**Only the Ethernet data model is bound.** The Wi-Fi server owns a separate `mb_datastore_t`, so writing coil 0 over Wi-Fi moves a number and nothing else. Binding both would make the I/O the point where two interfaces synchronise, which is the design this example declines under [Known limits](#known-limits).

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
I (365) io_bind: coil[0] -> GPIO1, GPIO4 -> discrete[0] (input open)
I (447) mb_server: [eth] waiting for link...
I (451) mb_server: [eth] Modbus TCP server on port 502
```

### Check it with the bundled probe

```bash
python tools/mb_probe.py 192.168.11.2
```

A healthy server prints this, and exits 0:

```
--- reads (the startup pattern from mb_datastore_init)
0x03 holding[30:40]    -> [1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039]  OK
0x04 input[0:8]        -> [0, 1, 4, 9, 16, 25, 36, 49]  OK
0x01 coils[32:48]      -> [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0]  OK
0x02 discrete[16:32]   -> [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]  OK

--- writes, each read back
0x06 holding[5]        -> [48879]  OK
0x10 holding[20:22]    -> [4369, 8738]  OK
0x05 coil[1]           -> [1]  OK
0x0F coils[8:12]       -> [0, 1, 0, 1]  OK

--- refusals (the server must answer, not drop the connection)
0x03 addr 9000         -> b'\x83\x02'  OK
0x03 count 0           -> b'\x83\x03'  OK
0x42 undefined         -> b'\xc2\x01'  OK

15 requests on one connection, 0 failures
```

48879 is 0xBEEF and 4369/8738 are 0x1111/0x2222 — the values the probe wrote, read back. The three `b'\x83...'` lines are exception replies: function code with the high bit set, then the exception code.

The read windows start away from address 0 for two separate reasons, both worth knowing before editing the probe. `holding[30:40]` avoids the addresses the probe writes further down, so a second run without a reboot does not fail on 0xBEEF left over from the first — a red result that says nothing about the server. `coils[32:48]` and `discrete[16:32]` avoid the elements bound to pins, which hold whatever the hardware is doing rather than the startup pattern.

### Or with a general-purpose master

```bash
mbpoll -m tcp -a 1 -r 1 -c 10 192.168.11.2       # read ten holding registers
mbpoll -m tcp -a 1 -r 6 192.168.11.2 48879       # write 0xBEEF to holding[5]
mbpoll -m tcp -a 1 -r 6 -c 1 192.168.11.2        # read it back
```

`mbpoll -r 1` addresses the first register: the tools number from 1, the wire carries a zero-based address, and this server's holding registers start at 1000.

### What the serial log shows

Either way, the device names each function code and the size of the reply it produced:

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

Address that one **with the port** — port 502 belongs to the Ethernet server, so a master given no port will always land there:

```bash
python tools/mb_probe.py 192.168.11.8 5020
mbpoll -m tcp -p 5020 -a 1 -r 1 -c 10 192.168.11.8
```

Leave a master polling over Ethernet while you do it. Both stay up, and the log prefix says which one is talking. That is the whole point of the example: identical protocol code driving WIZnet hardware sockets and software LwIP at the same time, differing only in the socket vtable each connection carries.

### The web UI

Open **http://192.168.11.2** while the Modbus server runs. The page shows what the server is doing, what is in its registers, and lets the network settings be changed without a toolchain.

The register view is the one worth leaving open while a master polls: values that moved since the last second are highlighted, so "is the master writing where I think it is" becomes a question you answer by looking rather than by adding a print.

| Endpoint | Does |
|---|---|
| `GET /` | the page |
| `GET /api/status` | state, counters, PHY link, MAC, free heap, uptime |
| `GET /api/registers` | all four tables |
| `GET /api/config` | the current IP, mask, gateway and Modbus port |
| `POST /api/config` | change any of those; fields left out keep their value |

```bash
python tools/web_probe.py 192.168.11.2            # read endpoints and every refusal
python tools/web_probe.py 192.168.11.2 --apply    # plus a real port change and back
```

**One data model, not two.** The Modbus server and the page read the same registers under one lock, held for the length of a PDU or a snapshot copy and never across a socket call — a master that stops reading mid-response must not be able to stall the page. `web_probe.py` proves the sharing rather than assuming it: it writes a register over Modbus and reads it back over HTTP.

**Settings are saved, answered, then applied — in that order.** Applying a new address tears down the sockets the request arrived on, so doing it inside the handler would kill the connection before the reply left the device and the user would never learn the new address. The handler validates, writes NVS and responds; a separate task does the work a moment later. The reply carries the new address so the page can offer it as a link.

Validation refuses what cannot work and says which field is wrong: an address that is not an address, loopback or multicast, a mask with a hole in it, a gateway outside its own subnet, a port of 0, and port 80 — the web UI's own, which is the way back in when the rest has been set to something unreachable and therefore not settable from the page it serves.

**The Modbus port and the IP survive a reboot.** They live in NVS behind a magic and a version; anything missing, corrupt or from an older layout falls back to `net_config.h` rather than to zeros, because a device that boots on 0.0.0.0 after a bad write is a device that needs a cable.

**Wi-Fi keeps its own data model** and is not configurable from the page. Sharing one store across both interfaces would make it a synchronising point between two stacks, which is a different example; the page reports the Ethernet side, which is the one it can configure.

## Step 6: Drive the I/O from a master

Everything above proves the frames are right. This is the part that shows a coil is a contact.

1. Wire the relay board and the input contact as in [Step 2](#io-wiring), and power the relay board from its own supply.
2. Open **Modbus Poll** and choose **Connection — Connect**: Modbus TCP/IP, IP `192.168.11.2`, port `502`.
3. Choose **Setup — Read/Write Definition**: Function `01 Read Coils`, Address `0`, Quantity `8`, Scan Rate `200 ms`.
4. Double-click the row for address 0 and write `1`.

The relay closes, its LED lights, and the serial log names the transition:

```
I (18412) io_bind: coil[0] ON -> GPIO1 active
```

Write `0` and it opens again. Short the input contact to ground, and discrete input 0 follows in the master's grid about 60 ms later — three 20 ms samples of agreement, which is the debounce:

```
I (24905) io_bind: GPIO4 closed -> discrete[0] 1
```

The same thing from a command line, where `-t 0` is a coil and `-t 1` a discrete input:

```bash
mbpoll -m tcp -a 1 -t 0 -r 1 192.168.11.2 1        # coil 0 on   (-r 1 is address 0)
mbpoll -m tcp -a 1 -t 0 -r 1 192.168.11.2 0        # coil 0 off
mbpoll -m tcp -a 1 -t 1 -r 1 -c 1 192.168.11.2     # read discrete input 0
```

The web UI's register view shows the same two elements, so a browser open on `http://192.168.11.2` is a second view of the pin without a second master on port 502.

## Troubleshooting

### Writing coil 0 kills the Ethernet link

Not a Modbus fault, and the log says so if you read the order rather than the errors:

```
I (8143) mb_server: [eth] function 0x05 -> 5 bytes     <- the write was served
I (8146) mb_server: [eth] session closed               <- and the session ended cleanly
I (8151) io_bind: coil[0] ON -> GPIO1 active           <- then the pin moved
E (8151) mb_tx: accept failed: errno 22                <- and every socket died at once
E (9152) mb_tx: listen(502) failed: errno 95
E (9152) mb_server: [eth] cannot reopen port 502, giving up
```

The PDU is executed and answered before the pin is touched, so a protocol bug cannot produce this shape. What follows is the whole chip going away in the same millisecond as the level change: `EINVAL` from `accept()`, then `EOPNOTSUPP` from `listen()` for as long as the pin stays low, while the ESP32 keeps running and keeps logging. The link comes back when the coil is cleared, which is the tell.

Measured on the board this example is configured for: **GPIO5 does this and GPIO1 does not**, with nothing else changed. Whatever GPIO5 is tied to there — a bridge to a rail is the usual answer — it is not visible in any pin table, because pin tables record what a design intends rather than what a board does.

If a bound output does this on your hardware:

1. Clear the coil, or reset the board, and confirm the link comes back. That distinguishes this from a wedged socket, which does not recover on its own.
2. Power the board down and check continuity from that pad to 3V3 and to GND.
3. Move `IO_OUT_PIN` to another free pad and repeat the toggle. Two pins behaving differently is the proof; one pin failing is only a symptom.

Worth doing once for every pad this example binds, before a relay is anywhere near it.

### The relay works backwards

Coil 0 on opens the contact and off closes it. The relay board drives the opposite way round from the default: set `IO_OUT_ACTIVE_LOW` to `0` in `inc/io_bind.h` and rebuild. Nothing in the protocol is wrong, which is why this is worth checking first — the frames are correct either way.

### Discrete input 0 flickers, or reports closed with nothing connected

Flicker means bounce outrunning the debounce: raise `IO_DEBOUNCE_SAMPLES`, at 20 ms of added latency per sample. A permanent 1 with nothing wired means the pin is floating — `IO_IN_ACTIVE_LOW` decides whether the internal pull-up or pull-down is enabled, so an active-high setting with a contact wired to ground reads as closed forever.

### `listen(502) failed: errno 95`, right after a version mismatch

Look one line up. If the boot log says

```
E (440) wsm_driver_spi: W5500 version mismatch: 0x00 (expected 0x04)
```

the chip is not answering on SPI at all, and the listen failure is a consequence, not the fault. Wrong board selected, wrong pins, or the RST/INT lines colliding with the console — see the GPIO43/44 note in [Step 3](#step-3-setup-modbus-tcp-example).

An `IO_OUT_PIN` or `IO_IN_PIN` pointed at one of the SPI pins produces exactly this log and nothing else: `io_bind` configures its pins before the chip is brought up, so the driver finds a bus somebody else is holding. The build should have refused it — if it did not, `CONFIG_WSM_DRIVER_PIN_*` was changed in `sdkconfig` after the last build, so rebuild and let the check in `inc/io_bind.h` run against the current values.

### The device is running something else entirely

If `idf.py monitor` prints a warning like

```
--- Warning: Checksum mismatch between flashed and built applications.
```

the binary on the chip is not the one in `build/`. Read the two lines just above it:

```
I (637) app_init: Project name:     wsm_driver_modbus_tcp_example
I (637) app_init: Compile time:     Aug 21 2026 11:04:12
```

A different project name means an older example is still flashed — monitor was started without a flash, or the flash went to a different port or a different directory. `idf.py -p COM6 flash monitor` from this example's directory fixes it. Worth checking first whenever the log does not match the code you are reading: everything after that line describes a program you did not build.

### The master reports "illegal data address"

Exception 0x02, and usually an off-by-one rather than a bug. Modbus documentation numbers holding registers from 40001 and coils from 1, while the wire carries a zero-based address; `mbpoll -r 1` sends address 0. This server holds 64 of each, so any address past 63 — or a read whose count runs past 63 — is refused.

### The master reports "illegal data value"

Exception 0x03. A count of zero, a count past the protocol's own ceiling (125 registers or 2000 bits per read), or a write whose byte count does not agree with its element count. The last one is worth knowing about: the byte count in a multiple-write request is redundant with the element count, so a mismatch means the frame contradicts itself and neither number can be trusted.

### Wi-Fi answers a few requests and then stalls

Not a Modbus problem. If replies arrive normally and then latency climbs — measured here at 45 ms, then 1.2 s, then 1.5 s, then nothing — the station is losing frames to power save. The default `WIFI_PS_MIN_MODEM` (the `wifi:pm start, type: 1` line at boot) wakes the radio on a listen interval of 307 ms scaled by the AP's DTIM period, and an AP that buffers poorly drops what does not fit.

On this setup, with `esp_wifi_set_ps(WIFI_PS_NONE)` after `esp_wifi_start()`, the same fifteen-request sequence completed twice with no delay at all; with the default it failed at the second and fifth request on consecutive runs. The DHCP lease shows the same split even earlier in the boot — 31.8 s, 90.6 s and 173.1 s on the default against 2.2 s with power save off, on the same board with the antenna untouched and RSSI between -28 and -29 dBm.

It is left at the default here because it costs idle current and most APs do not show the problem. Request/response protocols expose it more than streaming ones do — a browser reconnects and hides it, a Modbus master times out and reports a failure.

### The server stops answering after a browser was pointed at it

Fixed here, and worth knowing about because it affects anything built on the TOE backend.

`wiztoe_accept()` advances a hardware socket when it reads `SOCK_ESTABLISHED` and re-arms it when it reads `SOCK_CLOSED`. A client that connects and then drops without sending anything leaves the socket in `SOCK_CLOSE_WAIT`, which is neither, so `accept()` times out against it forever and that listening socket is gone. A browser closing a tab does exactly this.

Measured before the workaround: six connect-and-close cycles took the Modbus server out for more than ten seconds, thirty rapid sessions kept it out, and on the web side — three listeners, three chances — sustained load fell from 120 of 120 requests served to 32 of 120 as the listeners died one at a time.

Both servers now rebuild a listener that has been silent for about two seconds. That costs a socket open and a listen on an idle server, which is nothing, and it is the only escape from the wedged case. `tools/abuse.py` covers it.

The real fix belongs in `wiztoe_accept()`, which should treat `SOCK_CLOSE_WAIT` the way it treats `SOCK_CLOSED`.

### `listen(80) failed: errno 95`, thousands of times a second

A rebuild that cannot succeed, retried with no pause between attempts. Fixed, and worth reading because of what it took down with it.

When a hardware socket is wedged rather than busy, `listen()` returns `EOPNOTSUPP` — `wiztoe_wrap.c` maps every `wiztoe_listen()` failure to that — and it will keep returning it until something else frees the socket. The web task's retry had a deadline in the branch that handles a failed `accept()` and none at all in the branch that reopens a slot already marked dead, so it went around the loop at full speed: three error lines per millisecond, one per listener per pass.

The flood was the visible part and the cheap part. Every attempt is a `socket()` plus a `listen()`, and on the TOE both are SPI transactions, so the web task took the chip's bus for itself. The Modbus server runs at the same priority and needs the same bus — **a wedged web listener stopped Modbus TCP from answering**, on a port with nothing wrong with it. Two servers on one chip share more than the address.

The retry now backs off 1, 2, 4, then 8 seconds per slot, tracked as a deadline rather than a sleep so a dead listener does not stall the healthy ones, and a pass with nothing left to poll yields instead of spinning. Recovery is unchanged — the same rebuild, at a rate that is free.

Reading a wedge like this from the console alone is misleading, because the error names the port that failed rather than the resource that ran out. The line to trust is the first one, not the ten thousand after it.

## Testing it properly

Three tools, in the order worth running them:

```bash
python tools/mb_probe.py 192.168.11.2      # the protocol: 8 function codes, 3 exceptions
python tools/web_probe.py 192.168.11.2     # the API and the refusals
python tools/abuse.py 192.168.11.2         # clients behaving badly, then recovery
python tools/soak.py 192.168.11.2 --minutes 30
```

`abuse.py` and `soak.py` exist because the failures that mattered here were not visible in the code. They needed the board, a client behaving badly, and enough time for the damage to accumulate: a mask check that rejected `255.255.255.0` and so masked the gateway and port checks behind it; an accept timeout that set the response latency at 430 ms and refused 261 of 300 requests under load; a byte-at-a-time read costing an SPI round trip per byte; and the CLOSE_WAIT case above, which only appeared as a slow decline across hundreds of requests.

`soak.py` reports free heap and its low-water mark once a minute rather than a total at the end, because a leak is a trend and a total hides it. Eight minutes of the full mix: 1478 of 1478 web requests, 739 of 739 Modbus, heap flat at 351 KB, low-water mark unmoved across 25 sessions.

## Known limits

Worth knowing before this gets pointed at anything that matters. None of these are accidents; they are where the example stops.

- **Timeouts are per read, not per session.** `REQUEST_TIMEOUT_MS` bounds one `recv`, so a peer that sends one byte just often enough keeps its session alive indefinitely. With one session per interface, that is enough to lock out every other master. A product server wants an absolute deadline on the whole ADU and an idle-session limit.
- **Deadlines are per operation, not per ADU or per session.** Each accepted socket gets a 2 s `SO_SNDTIMEO` (`mb_transport_accept`), and the TOE backend honours it while waiting for transmit room, yielding between checks — so a master that stops reading gives the task back instead of holding it until the chip's own TCP timeout. The timeout is set on the accepted descriptor rather than the listener on purpose: on the TOE the two are the same socket, but on the Wi-Fi path `accept()` returns a new one that inherits nothing. What is still missing is an absolute deadline on a whole ADU and a limit on an idle session — `REQUEST_TIMEOUT_MS` bounds one `recv`, so a peer that sends one byte just often enough keeps its session alive indefinitely, and with one session per interface that is enough to lock out every other master.
- **An unauthenticated write now moves a contact.** This is the limit that changed when the pins were bound, and it is first for that reason. Everything below about authentication was already true; what is new is that the consequence is physical rather than a number in RAM. A device with an output bound to a relay belongs on a bench or an isolated segment until something in front of it decides who may write coil 0.
- **No authentication, no transport security.** Plain Modbus TCP on port 502 and plain HTTP on 80: anyone who can reach the address can write every register *and change the device's IP*. The web UI widens what an unauthenticated peer can do, which is the trade it makes for being usable without a toolchain — on a segment where that is not acceptable, do not build it in. Modbus Security (MB/TCP over TLS) exists for the protocol side.
- **A settings change can be saved without being applied.** If a master stops reading mid-response the Modbus task is stuck inside a send that has no deadline, so it cannot be stopped, and the new port lands at the next reboot instead. The dashboard says so rather than letting NVS and the running device disagree quietly.
- **One master per interface**, for the reason in [Step 3](#one-master-at-a-time).
- **The I/O is bound to one interface.** `io_bind` binds the Ethernet data model. Coil 0 written over Wi-Fi changes a number in the Wi-Fi model and moves nothing, which follows from the next limit rather than from anything about the pins.
- **Two independent slaves, not a redundant pair.** Each interface owns its own `mb_datastore_t`, so a register written over Ethernet is *not* visible over Wi-Fi. If you want one data model behind two interfaces, add a lock and share the struct — deliberately not done here, because the mutex would be the most interesting part of the example and it is not what the example is about.

## Appendix

- **Widening the data model:** `MB_REG_COUNT` and `MB_COIL_COUNT` in `inc/mb_core.h`. Nothing else needs to change; the bounds checks and the exception replies follow from them.
- **Widening the I/O:** `io_bind.c` binds one element of each kind because the seam is the point, not the width. More outputs is the same loop over an index range and a pin table; the protocol side needs no edits at all, which is the property worth taking from it. An analog input is not this — `ds->input[n]` is where a scaled reading would be published, but 4-20mA is a front-end design problem rather than a firmware one, and it belongs in its own example.
- **Unit id:** echoed, not checked. On serial Modbus it addresses one slave on a shared bus; over TCP the IP address does that job, and the spec's own guidance is to echo whatever arrived. A gateway to a serial segment would be the case where it starts to matter.
- **The WIZnet S2E Modbus stack:** [W55RP20-S2E][link-s2e] carries a Modbus implementation under `port/app/modbus`, and it solves a different problem — it is a TCP↔RTU/ASCII *gateway* (`mbTCPtoRTU`, `mbRTUtoTCP`), so it forwards frames to a serial slave rather than answering them, and it calls ioLibrary's hardware sockets directly (`getSn_RX_RSR`, `recv(sn, ...)`) rather than BSD ones, which ties it to the TOE backend and rules out the Wi-Fi path. Useful as a reference for the MBAP layout; not a drop-in for a standalone slave.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-mbpoll]: https://github.com/epsilonrt/mbpoll
[link-modbus_poll]: https://www.modbustools.com/modbus_poll.html
[link-qmodmaster]: https://sourceforge.net/projects/qmodmaster/
[link-pymodbus]: https://github.com/pymodbus-dev/pymodbus
[link-s2e]: https://github.com/WIZnet-ioNIC/W55RP20-S2E
