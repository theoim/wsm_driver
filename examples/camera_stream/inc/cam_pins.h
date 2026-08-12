/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Camera wiring for the Seeed XIAO ESP32-S3 Sense.
 *
 * The sensor sits on the Sense expansion board's B2B connector, so these pins
 * are fixed by the board rather than chosen. They are listed here because the
 * one thing a reader has to check before building is that they do not collide
 * with the WIZnet SPI wiring, and that check is only possible if both maps are
 * visible. They do not overlap:
 *
 *     camera   10 11 12 13 14 15 16 17 18 38 39 40 47 48
 *     W5500     6  7  8  9 43 44
 *
 * GPIO43/44 are the ESP32-S3's UART0 console pins, which is why this example's
 * sdkconfig.defaults moves the console to the USB Serial/JTAG controller: the
 * reset and interrupt lines would otherwise fight the log output.
 *
 * A different camera board means a different map. Nothing else in the example
 * refers to these numbers.
 */
#ifndef CAM_PINS_H
#define CAM_PINS_H

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

#endif /* CAM_PINS_H */
