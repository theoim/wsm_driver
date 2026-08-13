/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Camera wiring, per board.
 *
 * The sensor is the same OV3660 either way -- esp32-camera does not care which
 * board it is on -- so the only thing that changes between them is this map.
 *
 * The one check worth making before wiring anything is that the camera's
 * fourteen pins clear the WIZnet module's six, which is why both maps are
 * listed side by side below rather than left for a reader to work out. On the
 * ESP32-S3 the pins that are not available at all are also worth having in
 * front of you:
 *
 *     flash / PSRAM   26..37   (33..37 only on octal parts, but assume them)
 *     USB             19 20
 *     UART0           43 44
 *     strapping       0 3 45 46
 *
 * Pick the board with CAM_BOARD_* below.
 */
#ifndef CAM_PINS_H
#define CAM_PINS_H

/* ---------------------------------------------------------------------------
 * Board selection. Exactly one.
 * ------------------------------------------------------------------------- */
#define CAM_BOARD_XIAO_SENSE     1
#define CAM_BOARD_WIZNET_DEVKIT  0

#if CAM_BOARD_XIAO_SENSE + CAM_BOARD_WIZNET_DEVKIT != 1
#error "Set exactly one CAM_BOARD_* to 1"
#endif

#if CAM_BOARD_XIAO_SENSE
/*
 * Seeed XIAO ESP32-S3 Sense.
 *
 * Fixed by the board: the sensor sits on the Sense expansion board's B2B
 * connector, so these are not a choice. Verified against the WIZnet module on
 * the castellated pads (MOSI 9, MISO 6, SCLK 8, CS 7, RST 43, INT 44):
 *
 *     camera   10 11 12 13 14 15 16 17 18 38 39 40 47 48
 *     W5500     6  7  8  9 43 44
 *
 * No overlap.
 */
#define CAM_PIN_PWDN    -1      /* not wired on the Sense board */
#define CAM_PIN_RESET   -1      /* not wired; the driver resets over SCCB */
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40      /* SCCB data  */
#define CAM_PIN_SIOC    39      /* SCCB clock */

#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15

#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

#else /* CAM_BOARD_WIZNET_DEVKIT */
/*
 * WIZnet ESP32-W5500-Dev-V1 or ESP32-W6300-Dev-V1, with an OV3660 wired to the
 * headers. One map for both, taken from the two schematics.
 *
 * What each board has already spoken for:
 *
 *   ESP32-W5500-Dev-V1   "IO9~IO14 are connected with W5500."
 *   ESP32-W6300-Dev-V1   "IO8~IO14 and IO21 are connected with W6300."
 *                        (QSPI, so four data lines rather than two)
 *   both                 IO38 drives the WS2812B; TXD0/RXD0 go to the CH340X
 *
 * Removing those, the USB pair, the strapping pins and IO35..37 leaves
 *
 *     1 2 4 5 6 7 15 16 17 18 39 40 41 42 47 48
 *
 * which is sixteen pins for the camera's fourteen. Using the same map on both
 * boards costs the two spares and means one wiring diagram instead of two.
 *
 *     camera   4 5 6 7 15 16 17 18 39 40 41 42 47 48
 *     W5500          9 10 11 12 13 14
 *     W6300        8 9 10 11 12 13 14 21
 *
 * IO35..37 are left out deliberately even though both schematics bring them to
 * a header. On an ESP32-S3-WROOM-1 those three belong to octal PSRAM, so a
 * module that exposes them is a quad-PSRAM or no-PSRAM part -- and using them
 * would tie this map to that variant.
 *
 * Which brings up the gate that is not a pin at all. The frame buffers live in
 * PSRAM (cam_source.c asks for CAMERA_FB_IN_PSRAM), and sdkconfig.defaults
 * selects OCTAL mode for the XIAO. If the module here turns out to be quad,
 * that setting has to change; if it has no PSRAM, this example does not run on
 * it above the smallest frame sizes. Either way the symptom is the same
 * unhelpful one: esp_camera_init() returning ESP_ERR_NO_MEM, which does not
 * mention PSRAM. Check the marking on the module can before wiring anything.
 */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4       /* SCCB data  */
#define CAM_PIN_SIOC    5       /* SCCB clock */

#define CAM_PIN_D7      48
#define CAM_PIN_D6      42
#define CAM_PIN_D5      41
#define CAM_PIN_D4      40
#define CAM_PIN_D3      39
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      16

#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    47

/* Spare, if something above turns out to be unusable: IO1, IO2. */

#endif

#endif /* CAM_PINS_H */
