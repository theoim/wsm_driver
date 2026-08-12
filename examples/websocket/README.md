# How to Test WebSocket Example

> **Verified on a W5500.** Run on an XIAO ESP32-S3 + W5500 over Ethernet, TOE backend. The Wi-Fi path builds but has not been exercised on hardware yet.

## Step 1: Prepare software

Only a serial terminal and a web browser. There is no server to install, no test client to download, and no HTML file to open by hand — the device serves the page that talks to it.

- [Tera Term][link-tera_term]
- Any modern browser (Chrome, Edge, Firefox, Safari)

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-websocket-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

## Step 3: Setup WebSocket Example

### Chip and SPI configuration

Set the target and open menuconfig:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Select **Component config**.
![][link-config_main]

Select **WIZnet WSM Driver** under Component config.
![][link-config_component]

Choose the **Board**. The chip and its pins both follow from that, because picking the chip alone cannot express the wiring — the W6300 Dev-kit and the W6300 SoM carry the same chip on different GPIOs. For wiring no listed board covers, choose **Custom**: that is the one setting where the pin fields become editable.
![][link-config_wiz_toe]

> Do not set `CONFIG_WSM_DRIVER_CHIP_*` by hand in `sdkconfig` or `sdkconfig.defaults`. It contradicts whichever board is selected and leaves the driver talking to the wrong chip on the right pins, which shows up at boot as `CID mismatch: 0x0000`.

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

Network identity and the ports live in `examples/websocket/inc/net_config.h`, the same way as in `examples/loopback`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define WS_PORT               80      /* Ethernet */
#define WIFI_WS_PORT          81      /* Wi-Fi    */
#define WS_MAX_MESSAGE_SIZE   2048
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

Port 80 so a browser reaches the device as `http://192.168.11.2` with no port suffix. The two interfaces bind different ports because with `SOCKET_WRAP=0` (esp_eth backend) they share one LwIP stack, where the same port would clash on bind.

### Architecture

Same layout as `examples/loopback`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, ports, buffer size |
| `inc/ws_core.h` · `src/ws_core.c` | RFC 6455 handshake and framing, ported from mWebSockets |
| `inc/ws_transport.h` · `src/ws_transport.c` | the network seam |
| `inc/ws_index_html.h` | the page served at `/`, as a string literal |
| `src/ws_server.c` | one session: classify the request, serve or upgrade, echo |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |

The protocol implementation is [mWebSockets][link-mwebsockets] (MIT, Dawid Kurek), copied into the example rather than pulled in as a dependency, so its network calls could be swapped for BSD ones. It reaches the network exclusively through `ws_transport.h`, whose implementation calls the component's `net_sock_ops_t` vtable — so the same code runs on the WIZnet hardware sockets or on the software LwIP behind the Wi-Fi netif.

### Serving the page and the socket on one port

A WebSocket server is not an HTTP server, so the obvious build leaves the user opening an HTML file by hand and editing the device address into it. Reading the request once and branching on whether it carries `Upgrade` costs little and means the whole example is "type the device's address into a browser":

```
                ESP32 + WIZnet chip
              ┌─────────────────────┐
  GET /       │  HTTP               │
Browser ─────►│    └─ index.html    │
              │                     │
  GET /ws     │  WebSocket          │
Browser ═════►│    └─ echo          │
              └─────────────────────┘
```

`ws_read_request()` classifies each request:

| Request | Result |
|---|---|
| `GET /` with no `Upgrade` | `WS_REQ_PLAIN_HTTP` — the page is served |
| `GET /ws` with `Upgrade: websocket` | `WS_REQ_UPGRADED` — 101, then frames |
| anything else | `WS_REQ_FAILED` — an HTTP error is sent |

The page asks for `'ws://' + location.host + '/ws'`, so it follows whatever address the browser used to fetch it. The same firmware works on Ethernet, on Wi-Fi, and after the address changes, with nothing to edit.

### One connection at a time

That is a property of the chip, not a shortcut. The TOE has no separate accepted socket: the listening hardware socket *becomes* the connection, so `accept()` returns the same descriptor it was given, where LwIP returns a new one and keeps the listener. The component absorbs the difference on the way out — closing an accepted listener reopens and re-listens it — so the ordinary accept / serve / close loop works unchanged on both backends.

What does not carry over is serving several clients from one listening socket. On the TOE that needs one hardware socket per client, which is what `examples/tcp_server_multi_socket` demonstrates.

### Changes made porting mWebSockets

- **C rather than C++.** The three classes flatten into one `ws_conn_t`; there was no depth to the hierarchy to lose.
- **Bulk reads instead of byte-at-a-time.** Arduino's `Client` has no read-with-timeout, so upstream calls `read()` per byte. Over a hardware socket that is one SPI transaction per byte: a ~500-byte browser handshake measured **877 ms** on a W5500. Routing every read — handshake line, frame header, payload — through one 256-byte staging buffer brought the same handshake to **6 ms**. Sharing that buffer between the handshake and the frame parser is also what makes bytes read past the blank line simply still be there when the parser asks for them.
- **SHA-1 and Base64 from mbedTLS.** That drops upstream's bundled `CryptoLegacy` and `base64` (~2,200 lines), leaving this port smaller than the original. mbedTLS 4.0 (ESP-IDF v6) moved the low-level hash modules behind `mbedtls/private/`, so the digest goes through `psa_hash_compute(PSA_ALG_SHA_1, ...)` rather than `mbedtls/sha1.h`.
- **An HTTP path.** Upstream is WebSocket-only; serving the page is added here.

> SHA-1 is weak for signatures, and using it here is not a judgement about that: RFC 6455 section 1.3 fixes it as the accept-key derivation, and the value is a handshake token with no secrecy requirement.

## Step 4: Build

After completing the setup, build the project.

```bash
idf.py build
```

![][link-build_log]

## Step 5: Upload and Run

Flash the firmware and open the serial monitor. Replace the port with your board's serial port.

```bash
idf.py -p COMx flash monitor
```

On Linux/macOS:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

If flashing succeeds, the chip is identified and the server comes up.

```
I (268) wsm_driver_spi: W5500 version check OK: 0x04
I (270) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (272) ws_server: [eth] WebSocket server on port 80
```

Open **http://192.168.11.2** in a browser. The page loads and connects itself; the status turns green with no further action.
![][link-run_page]

```
I (7958) ws_tx: client connected from 192.168.11.4
I (7962) ws: plain HTTP request for "/"
I (7963) ws_server: [eth] serving the page
I (8194) ws_tx: client connected from 192.168.11.4
I (8200) ws: handshake complete (/ws)
I (8200) ws_server: [eth] connection open
```

The three buttons each exercise a different path through the framing code:

| Button | What it covers |
|---|---|
| Send | short text frame, length ≤ 125 (the short form) |
| 300 bytes | the 126 extended-length path — the header grows from 2 bytes to 4 |
| Binary | binary opcode with a non-UTF-8 payload |

Every message comes straight back, which is what confirms both directions of the framing.

```
I (11522) ws_server: [eth] received 22 bytes: hello from the browser
I (14582) ws_server: [eth] received 300 bytes: xxxxxxxx...
I (17121) ws_server: [eth] received 6 binary bytes
```

![][link-run_echo]

Reloading the page closes the connection and opens a new one. That is worth doing a few times: it is the path that re-arms the listener on the TOE.

### Over Wi-Fi as well

Filling in `WIFI_SSID` starts a second server on the Wi-Fi interface, and the two run side by side. It comes up on its own schedule, once the station holds a DHCP lease:

```
I (90642) esp_netif_handlers: sta ip: 192.168.11.8, mask: 255.255.255.0, gw: 192.168.11.1
I (90643) wifi: got IP 192.168.11.8
I (90735) ws_server: [wifi] WebSocket server on port 81
```

Open that address **with the port**: `http://192.168.11.8:81`. Plain `http://192.168.11.8` will not answer, because a browser sends it to port 80 and only the Ethernet server listens there. The split is deliberate — see `WIFI_WS_PORT` in [Step 3](#step-3-setup-websocket-example).

Leave the Ethernet tab open while you do it. Both stay up, and the log prefix says which one is talking:

```
I (106946) ws_server: [eth] serving the page
I (107041) ws_server: [eth] connection open
```

That is the whole point of the example: identical protocol code driving WIZnet hardware sockets and software LwIP at the same time, differing only in the socket vtable each connection carries.

## Troubleshooting

### `CID mismatch: 0x0000` at boot

The build selected a different chip than the board carries. Check that `sdkconfig` has no hand-set `CONFIG_WSM_DRIVER_CHIP_*` contradicting the selected board — picking the board is enough.

### `send stopped after 0 of NNNN bytes: errno 5`

A client that went away mid-response. Holding F5 does this routinely: the browser abandons one page request to start the next. Logged as a warning because it is the peer's business, not a fault here.

### The page loads but the status stays red

The page arrived over HTTP but the WebSocket did not open. Check the serial log for `handshake complete`:

- No `plain HTTP request` line either: nothing reached the server.
- `plain HTTP request for "/ws"`: the upgrade headers did not arrive. A proxy between the browser and the device can strip them.
- `handshake: bad version or missing key`: the client is not speaking RFC 6455.

### Nothing at all after `WebSocket server on port 80`

The device is listening but unreachable. Confirm the PC is on the same subnet as `NET_IP_ADDR` and that the RJ45 link LEDs are on.

### The Wi-Fi address refuses the connection

Two different causes, and the serial log tells them apart.

If `[wifi] WebSocket server on port 81` has been printed, the address is right but the port is missing: use `http://<sta-ip>:81`, not `http://<sta-ip>`. Ethernet owns port 80.

If that line has not appeared yet, the station is still waiting for DHCP and no Wi-Fi server exists. `wifi:connected with <ssid>` only means the AP accepted the association; the lease is separate:

```
I (567)   wifi:connected with theo, aid = 4, channel 8, rssi: -29
I (570)   wifi:pm start, type: 1
I (90642) esp_netif_handlers: sta ip: 192.168.11.8      <- 90 s later
```

On this setup the gap ranged from 2 s to 173 s across reboots, with the antenna untouched and RSSI between -9 and -29 dBm. The cause is the default `WIFI_PS_MIN_MODEM` power save (the `wifi:pm start, type: 1` line) against an AP that buffers broadcast poorly: the station wakes on a listen interval of 307 ms scaled by a DTIM of 3, and a DHCP OFFER that misses that window waits for the next retry. Calling `esp_wifi_set_ps(WIFI_PS_NONE)` after `esp_wifi_start()` brought the same board to a steady 2.1-2.4 s. It is left at the default here because it costs idle current and most APs do not show the problem — this is worth knowing about, not worth changing for everyone.

## Known limits

Worth knowing before this gets pointed at anything that matters. None of these are accidents; they are where the example stops.

- **Timeouts are per read, not per session.** `POLL_TIMEOUT_MS` bounds one `recv`, so a client that sends one byte just often enough keeps its session alive indefinitely — during the handshake as well as after it. With one connection per interface, that is enough to lock everyone else out. A product server wants an absolute deadline on the handshake, a cap on its total size, and an idle-session limit.
- **`send()` has no deadline at all.** `ws_transport_send` loops until every byte is gone, and on the TOE backend `SO_SNDTIMEO` is accepted and then ignored — `wiztoe_send()` calls ioLibrary's blocking `send()` and never consults the stored timeout. A client that stops reading can therefore park the server task for as long as it likes. This one is in the component rather than the example, so the example cannot fix it.
- **`ws://`, not `wss://`.** No TLS and no authentication: anything that can reach the address can open a session.
- **Close frames are checked, not fully validated.** A 1-byte body is rejected and the peer's code is echoed back, but reserved close codes pass and the reason string is not checked for valid UTF-8. RFC 6455 asks for both.
- **One connection per interface**, for the reason in [Step 3](#one-connection-at-a-time).

## Appendix

- **Serving your own page:** `inc/ws_index_html.h` holds the page as a string literal, deliberately dependency-free — no CDN, no framework — because the device may be on a network with no route to the internet. Replace the literal to serve something else; nothing else has to change.
- **Message size:** `WS_MAX_MESSAGE_SIZE` bounds a reassembled message, fragments included. A frame declaring a 64-bit length is refused with close code 1009: nothing on a chip with 2 KB socket buffers wants to assemble a payload past 64 KB.
- **Control frames:** ping is answered automatically with the same payload, and a close frame is echoed before the socket goes down, so the browser sees a clean close code rather than a dropped connection.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-mwebsockets]: https://github.com/skaarj1989/mWebSockets

[link-config_main]: ../../static/image/websocket/config_main.png
[link-config_component]: ../../static/image/websocket/config_component.png
[link-config_wiz_toe]: ../../static/image/websocket/config_wiz_toe.png

[link-build_log]: ../../static/image/websocket/build_log.png
[link-run_page]: ../../static/image/websocket/run_page.png
[link-run_echo]: ../../static/image/websocket/run_echo.png
