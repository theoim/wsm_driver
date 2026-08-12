# How to Test Camera Streaming Example

> **Verified on a W5500.** Run on a Seeed XIAO ESP32-S3 Sense (OV3660) with a W5500 module on SPI, TOE backend, over Ethernet and over Wi-Fi. Frame rates below are measured on that board.

An MJPEG camera server. Point a browser at the device and it serves the page that streams from it — live view, resolution and sensor controls, and charts of frame rate and link bandwidth. One address, no client software, no HTML file to open by hand.

## Step 1: Prepare software

A serial terminal and a browser. That is the whole test rig.

- [Tera Term][link-tera_term] for the serial log
- Any modern browser (Chrome, Edge, Firefox, Safari)

## Step 2: Prepare hardware

1. Seat the Sense expansion board so the OV3660 is connected — the camera sits on the B2B connector, not on the castellated pads.
2. Connect the WIZnet Ethernet chip (W5500 or W6300) to the XIAO over SPI, following the pin table in [Step 3](#step-3-setup-camera-streaming-example).
3. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
4. Connect the XIAO to your PC with a USB cable.

The camera and the WIZnet module do not share a pin. `inc/cam_pins.h` carries both maps side by side so that can be checked rather than assumed:

```
camera   10 11 12 13 14 15 16 17 18 38 39 40 47 48
W5500     6  7  8  9 43 44
```

## Step 3: Setup Camera Streaming Example

### Chip and SPI configuration

Set the target and open menuconfig:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Select **Component config**, then **WIZnet WSM Driver**, then choose the **Board**. The chip and its pins both follow from that. For wiring no listed board covers, choose **Custom**: that is the one setting where the pin fields become editable.

**W5500 wiring (standard SPI)**

| W5500 | ESP32-S3 Pin |
|-------|--------------|
| MISO  | 13 |
| MOSI  | 11 |
| SCLK  | 12 |
| CS    | 10 |
| RESET | 9  |
| INT   | 14 |

> The XIAO's SPI pads are GPIO7/8/9 with RSTn and INTn commonly on GPIO43/44, which are the ESP32-S3's UART0 console pins. This example's `sdkconfig.defaults` already moves the console to the USB Serial/JTAG controller for that reason — the XIAO's USB is native, so the log keeps working and both pins come free.

### What this example needs that the others do not

Three settings in `sdkconfig.defaults`, all of them load-bearing:

| Setting | Why |
|---|---|
| `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y` | The frame buffers live in PSRAM. The XIAO carries **octal** PSRAM; a quad setting fails to detect it and the camera then fails with `ESP_ERR_NO_MEM`, which does not name the real cause. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` + a 3 MB app partition | esp32-camera plus the Wi-Fi stack overruns the stock 1 MB partition. |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` | Frees GPIO43/44 for the WIZnet reset and interrupt lines. |

A correct boot says so:

```
I (120) esp_psram: Found 8MB PSRAM device
I (563) camera: Camera PID=0x3660 VER=0x00 MIDL=0x00 MIDH=0x00
I (563) camera: Detected OV3660 camera
I (914) cam: sensor PID 0x3660 up at 640x480, quality 12, xclk 20 MHz
I (1070) wsm_driver_spi: W5500 version check OK: 0x04
I (1073) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (1078) cam_server: [eth] camera server on port 80, 3 listeners (TOE)
```

### Network configuration

Network identity and the ports live in `examples/camera_stream/inc/net_config.h`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define CAM_PORT              80      /* Ethernet */
#define WIFI_CAM_PORT         81      /* Wi-Fi    */
```

### Architecture

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, ports |
| `inc/cam_pins.h` | the camera's wiring, and the check that it clears the SPI pins |
| `inc/cam_source.h` · `src/cam_source.c` | the sensor and the numbers the page plots |
| `inc/http_core.h` · `src/http_core.c` | request-line parsing and response headers |
| `inc/http_transport.h` · `src/http_transport.c` | the network seam |
| `inc/web_page.h` | the page, as a string literal |
| `src/cam_server.c` | the listener pool, the router, the MJPEG stream |
| `main/main.c` | orchestration only: camera up, interfaces up, start the tasks |

Only `http_transport.c` includes lwIP, and it calls the component's `net_sock_ops_t` vtable, so the same server runs on the WIZnet hardware sockets or on the software LwIP behind the Wi-Fi netif. Only `cam_source.c` includes `esp_camera.h`.

This is the first example here with an external dependency. Everything else carries its protocol code in the example, because a TFTP client or a WebSocket framer is small enough to read and showing it is the point. A camera driver is neither — `espressif/esp32-camera` is DMA setup, sensor tables and JPEG plumbing, none of which teaches anything about a network stack.

### Serving the page, the stream and the API on one port

```
                    ESP32-S3 + OV3660 + WIZnet chip
                  ┌────────────────────────────────┐
  GET /           │  HTTP      → the page          │
Browser ─────────►│                                │
  GET /stream     │  multipart → JPEG, JPEG, ...   │
Browser ═════════►│                                │
  GET /api/status │  JSON      → fps, KB, timings  │
Browser ─────────►│                                │
                  └────────────────────────────────┘
```

| Endpoint | Does |
|---|---|
| `GET /` | the page |
| `GET /stream` | `multipart/x-mixed-replace`, one JPEG per part |
| `GET /api/status` | the status document — every control returns it too |
| `GET /api/start` · `/api/stop` | start and stop the stream |
| `GET /api/res?v=640x480` | frame size |
| `GET /api/cam?quality=12&xclk=20&contrast=1` | JPEG quality, sensor clock, and any sensor control |
| `GET /api/controls` | the control descriptors — name, label, group, range |
| `GET /api/reset` | re-initialise a wedged sensor |

Every control answers with the same status document as `/api/status`, so the page feeds each response through one render path and never has to work out what changed.

### The sensor controls

Twenty-three of them, in four groups:

| Group | Controls |
|---|---|
| Image | brightness, contrast, saturation, sharpness, effect |
| Exposure | AWB, AWB gain, WB mode, AEC, AEC DSP, AE level, exposure, AGC, gain, gain ceiling |
| Correction | lens correction, gamma, black pixel, white pixel, downsize |
| Orientation | mirror, flip, test pattern |

They live in one table in `cam_source.c` — name, label, group, range and an apply function per row — and everything else is generated from it: the query parser walks it, the status JSON walks it, and the page builds its panel by fetching `/api/controls` at load. Adding a row makes a new slider appear in the browser with no edit to the HTML. Written three separate times instead, the three lists would drift.

Two of them are worth knowing about before the rest:

- **Test pattern** puts a known set of colour bars out of the sensor itself. If the bars arrive clean the fault is in front of the lens; if they do not, it is behind it. That splits "the picture looks wrong" in half without any equipment.
- **Exposure**, **gain** and **WB mode** only bite once their automatic counterpart — AEC, AGC, AWB — is switched off. That is why each sits next to its own toggle rather than in a "manual" group of its own.

A range of 0..1 renders as a checkbox and anything wider as a slider; sliders send on release rather than on every step, because a drag across the range would otherwise fire one request per notch and several of these re-initialise the sensor.

Controls survive a re-initialisation. Changing resolution or XCLK tears the driver down, and `esp_camera_init()` resets the sensor to its own defaults, so `apply_all_controls()` pushes the whole table back afterwards — otherwise every setting would silently revert the first time the frame size changed.

### Why this one listens more than once

The other examples in this repository serve one connection at a time. This one cannot. The page holds `/stream` open for as long as it is on screen **while** polling `/api/status` once a second, so two connections have to be live at the same instant or the charts never update.

On LwIP that is free: `accept()` returns a new descriptor and the listener stays open, so one listener feeds any number of connections. On the TOE it is not — the listening hardware socket *becomes* the connection, so one listener holds exactly one client and nothing is left listening while it does. Serving several clients there means several listening hardware sockets on the same port, which is what `examples/tcp_server_multi_socket` demonstrates and what `http_listen_pool()` does here. The count follows the backend rather than being a preference:

```c
#if defined(WSM_DRIVER_SOCKET_WRAP) && WSM_DRIVER_SOCKET_WRAP
#define CAM_ETH_LISTENERS  3      /* TOE: stream + poll + a reload arriving early */
#else
#define CAM_ETH_LISTENERS  1      /* software LwIP: one listener feeds them all   */
#endif
```

**Listeners and connections are two different arrays**, and conflating them is a mistake worth describing because the first version made it. Tying one connection slot to each listener is right on the TOE, where the two really are the same socket, and wrong on LwIP, where one listener should feed several: the Wi-Fi side ended up with a single slot, so the stream occupied it and the page's own status poll queued behind its own stream, leaving the charts frozen while the video played. `MAX_CONNS` now sizes the slots, `http_listen_pool()` sizes the listeners, and `listener_in_use()` — asked by descriptor rather than by backend, so no `#if` is needed — is what keeps a TOE listener from being accepted on twice.

Three connections is the ceiling either way. A browser holding a stream, a poll and a stale socket will use all of them, and the next request waits for a slot rather than being queued by the stack.

## Step 4: Build

```bash
idf.py build
```

The first build also fetches `espressif/esp32-camera` into `managed_components/`.

## Step 5: Upload and Run

```bash
idf.py -p COM6 flash monitor
```

Open **http://192.168.11.2** in a browser, press **START**, and the live view fills in. The serial log names each stream as it opens:

```
I (17012) cam_server: [eth] stream opened
```

Frame rate, link bandwidth and the timing legend update once a second from `/api/status`.

### Measured on a W5500

Ethernet, TOE backend, JPEG quality 12, one browser-equivalent client polling status once a second while streaming:

| Resolution | Frame | Frame rate | Link |
|---|---|---|---|
| 320 x 240 | 5.3 KB | 27.3 fps | 1.16 Mbps |
| 640 x 480 | 23.5 KB | 25.3 fps | 4.80 Mbps |
| 800 x 600 | 32.0 KB | 18.3 fps | 4.69 Mbps |

Those numbers are the reason the charts exist, and getting them right took one fix worth repeating. The first working version ran VGA at 14.5 fps, and the missing time was not the sensor or the link — each idle listener was being offered a 20 ms accept window once per frame, and with two idle listeners that is 40 ms of dead time the stream paid for:

```
640x480   send 32 ms  ->  1/(0.032 + 0.040) = 13.9 fps   (measured 14.5)
800x600   send 68 ms  ->  1/(0.068 + 0.040) =  9.3 fps   (measured  9.5)
```

Dropping the accept window to 1 ms while any slot is streaming — `ACCEPT_BUSY_MS` in `cam_server.c` — took VGA from 14.5 to 25.3 fps and SVGA from 9.5 to 18.3, with the status poll still answering every second. The frame rate had been set by a timeout constant rather than by the hardware, which is exactly the kind of thing this example is built to make visible.

### Over Wi-Fi as well

Filling in `WIFI_SSID` starts a second server on the Wi-Fi interface, and the two run side by side from the one camera. The page reads `stack` out of `/api/status` and changes its badge and accent colour, so two browser tabs show the same sensor going out through hardware TCP/IP and through software TCP/IP:

```
http://192.168.11.2        TOE badge, red
http://<sta-ip>:81         lwIP badge, dark
```

The Wi-Fi server comes up once the station holds a DHCP lease:

```
I (3392) esp_netif_handlers: sta ip: 192.168.11.8, mask: 255.255.255.0, gw: 192.168.11.1
I (3458) cam_server: [wifi] camera server on port 81, 1 listener (lwIP)
```

Both interfaces measured at 640x480, quality 12, with the status poll running:

| Interface | Stack | Frame rate | Link |
|---|---|---|---|
| Ethernet | TOE | 27.2 fps | 2.68 Mbps |
| Wi-Fi | LwIP | 15.8 fps | 1.56 Mbps |

Same camera, same frame size, same second — the two servers are running side by side off one sensor, which is the comparison this example exists to make. Read it as an ordering rather than a ratio: the Wi-Fi figure depends on the AP, the distance and what else is on the channel, none of which are properties of LwIP.

## Troubleshooting

### `camera init failed` at boot

Check the line above it. `Detected OV3660 camera` missing entirely means the Sense board is not seated — the sensor is on the B2B connector and a loose expansion board looks exactly like a missing camera. `ESP_ERR_NO_MEM` instead means PSRAM: confirm `Found 8MB PSRAM device` appears, and that `CONFIG_SPIRAM_MODE_OCT` is set, since the XIAO's PSRAM is octal.

### `W5500 version mismatch: 0x00`, then `cannot listen on 80`

The chip is not answering on SPI at all and the listen failure is a consequence, not the fault. Wrong board selected, wrong pins, or RSTn/INTn colliding with the console — see the GPIO43/44 note in [Step 3](#step-3-setup-camera-streaming-example).

### `send stopped after 0 of NNNNN bytes: errno 5`

Routine on a stream. The page reopens `/stream` whenever a control changes, so the abandoned connection fails its next write. Logged as a warning for that reason. It is only worth investigating if it happens without anything having been clicked.

### The page loads but the charts stay flat

The status poll is not getting through, which on the TOE means the listeners are all occupied. Close any other tab pointed at the device: three connections is the ceiling, and a stale stream socket counts against it.

### The stream stutters when a control is changed

Expected, and it is the sensor rather than the network. Changing resolution or XCLK tears the camera driver down and rebuilds it — esp32-camera fixes its DMA layout at init — which takes a few hundred milliseconds with the frame lock held. JPEG quality is the one control that applies live.

## Known limits

Worth knowing before this gets pointed at anything that matters. None of these are accidents; they are where the example stops.

- **Three connections, total** (`MAX_CONNS`). A fourth waits for a slot to free rather than being served, and on the TOE it also needs one of the three listening hardware sockets to be back in listen.
- **Timeouts are per read, not per session**, and `send()` has no deadline at all — on the TOE backend `SO_SNDTIMEO` is accepted and then ignored by `wiztoe_send()`, so a client that stops reading mid-frame can park the server task. That one is in the component rather than the example.
- **No authentication, no TLS.** Anything that can reach the address can watch the camera and change its settings.
- **One sensor, one lock.** Both interfaces stream from the same camera under one mutex, so a frame going out on Ethernet delays the Wi-Fi stream's next capture and the two frame rates are not independent.
- **The capture and send timings are two numbers, not three.** `esp_camera_fb_get()` returns a finished frame, so the VSYNC wait and the sensor readout arrive together; splitting them would mean inventing the split.

## Appendix

- **Serving your own page:** `inc/web_page.h` holds the UI as one string literal, deliberately dependency-free — no CDN, no framework — because the device may be on a network with no route to the internet. It is adapted from the [WIZnet ArduCAM MEGA streaming page][link-arducam], whose API shape it keeps: every control returns the status document, so the page has one render path.
- **Different sensor, different controls:** the ArduCAM original swept `CLK_DIV`/`PLL_DIV`. The OV3660 driven by esp32-camera has no divider pair, so those became JPEG quality — the lever that actually moves link bandwidth — and XCLK.
- **Wiring it to something else:** `cam_source.h` is the whole interface between the sensor and the server. A different camera board means a different `cam_pins.h` and nothing more.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-arducam]: https://github.com/theoim/WIZnet-ArduCAM-Web-Streaming
