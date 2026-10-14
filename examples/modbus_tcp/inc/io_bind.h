/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Binding two elements of the Modbus data model to two pins.
 *
 * This is the seam between the protocol and the world. mb_core.c deliberately
 * knows nothing about hardware -- no socket, no timer, no GPIO -- so the binding
 * cannot live there without giving up the property that makes mb_pdu_execute()
 * testable on a host. It lives here instead, on the other side of
 * mb_datastore_t, which is the whole interface between Modbus and an
 * application:
 *
 *   coil[IO_COIL_INDEX]      -> a real output pin   (a master writes it)
 *   discrete[IO_INPUT_INDEX] <- a real input pin    (a master reads it)
 *
 * A task polls rather than a callback firing from the write path. The write
 * path runs inside the store lock, and driving a pin from there would stretch
 * that lock out to hardware for every coil write -- and it would still leave
 * the input side without a home, because nothing in the protocol path is
 * triggered by a contact closing. One task samples the input, publishes the
 * output, and the lock stays what it is: a short copy of memory.
 *
 * IO_POLL_MS bounds how long a master waits to see its own write on the pin.
 * At 20 ms that is invisible next to a relay's own 5-10 ms of coil travel.
 *
 * Everything below is deliberately two elements rather than eight. The pattern
 * generalises by widening the index range; the point of the example is the
 * seam, not the width.
 */
#ifndef IO_BIND_H
#define IO_BIND_H

#include <stdbool.h>

#include "mb_store.h"

/* ---- wiring ----------------------------------------------------------------
 * GPIO1 and GPIO4 are clear of every WIZnet pin set this example is built with:
 * the W5500 dev-kit uses GPIO9-14, the W6300 SoM uses GPIO33-42, and the XIAO
 * carrier in this example's sdkconfig uses GPIO6-9 plus 43/44. They are also
 * clear of the ESP32-S3's strapping pins (0, 3, 45, 46) and its native USB
 * (19, 20), either of which turns a wiring mistake into a board that will not
 * boot rather than one that reports an error.
 *
 * The output was on GPIO5 until a bench run showed why "free in the pin table"
 * is not the same as "free". Driving GPIO5 low took the W5500 down with it --
 * every socket failed in the same millisecond as the level change, accept()
 * with EINVAL and then listen() with EOPNOTSUPP for as long as the pin stayed
 * low, while the ESP32 itself carried on. The same test on GPIO1 does not
 * disturb the link at all. Whatever GPIO5 is tied to on that board, the lesson
 * generalises: a pin that no table claims can still be shorted to a rail, and
 * the symptom arrives as an Ethernet fault rather than a GPIO one.
 *
 * Change them for your own carrier, and read the SPI pins out of sdkconfig
 * rather than out of the Kconfig defaults -- CONFIG_WSM_DRIVER_BOARD_CUSTOM
 * means the defaults say nothing about what is actually wired. The static
 * assertion below catches the overlap that a table can see; the rest is a
 * multimeter and one toggle with a master watching. */
#define IO_OUT_PIN        1     /* driven from coil[IO_COIL_INDEX]      */
#define IO_IN_PIN         4     /* sampled into discrete[IO_INPUT_INDEX] */

/*
 * Fail the build rather than the board.
 *
 * Binding an output to a pin the Ethernet chip is using does not report an
 * error at runtime: io_bind takes the pin over, SPI stops working, and the
 * device comes up saying "W5500 version mismatch: 0x00" -- which reads as a
 * cable or a solder problem and sends you looking at the wrong thing entirely.
 * Driving an input pull-up onto MISO is the same story and quieter still.
 */
#include "sdkconfig.h"

#if defined(CONFIG_WSM_DRIVER_PIN_MOSI)
#define IO_PIN_CLASHES(p)                          \
    ((p) == CONFIG_WSM_DRIVER_PIN_MOSI ||          \
     (p) == CONFIG_WSM_DRIVER_PIN_MISO ||          \
     (p) == CONFIG_WSM_DRIVER_PIN_SCLK ||          \
     (p) == CONFIG_WSM_DRIVER_PIN_CS   ||          \
     (p) == CONFIG_WSM_DRIVER_PIN_RST  ||          \
     (p) == CONFIG_WSM_DRIVER_PIN_INT)

#if IO_PIN_CLASHES(IO_OUT_PIN)
#error "IO_OUT_PIN collides with a WIZnet SPI pin -- see CONFIG_WSM_DRIVER_PIN_* in sdkconfig"
#endif
#if IO_PIN_CLASHES(IO_IN_PIN)
#error "IO_IN_PIN collides with a WIZnet SPI pin -- see CONFIG_WSM_DRIVER_PIN_* in sdkconfig"
#endif
#if IO_OUT_PIN == IO_IN_PIN
#error "IO_OUT_PIN and IO_IN_PIN are the same pin"
#endif
#endif /* CONFIG_WSM_DRIVER_PIN_MOSI */

#define IO_COIL_INDEX     0
#define IO_INPUT_INDEX    0

/*
 * Most opto-isolated relay boards pull IN low to energise, and most field
 * contacts close to ground. Both default to active-low for that reason. Set
 * either to 0 for a board that drives high.
 *
 * Getting this wrong does not fail loudly -- the relay simply works backwards,
 * which reads as a protocol bug and is not one.
 */
#define IO_OUT_ACTIVE_LOW   1
#define IO_IN_ACTIVE_LOW    1

/* ---- timing ----------------------------------------------------------------
 * A dry contact bounces for 5-20 ms and a relay's own contacts do the same, so
 * a single sample would publish a burst of transitions to the master and the
 * value would visibly flicker in a polling GUI. Requiring N consecutive equal
 * samples costs (N-1) * IO_POLL_MS of latency and removes the flicker. */
#define IO_POLL_MS          20
#define IO_DEBOUNCE_SAMPLES 3

/*
 * Drive the output pin to its inactive level, clear the bound coil, and start
 * the sync task on `store`. Returns false if the pin could not be configured or
 * the task could not be created; the Modbus server still runs in that case, it
 * simply drives nothing.
 *
 * Call this before any server is started. The order matters and is the reason
 * this is not just a task spawn: mb_datastore_init() seeds coil[0] on as part
 * of the demo pattern, and a bound coil is not a display value -- leaving it
 * would close the relay on the way out of reset, before any master has said
 * anything. The pin is driven inactive first, then the coil is cleared, so
 * neither the hardware nor the data model ever reports an output nobody asked
 * for.
 *
 * `store` is the Ethernet data model. The Wi-Fi server owns a separate one and
 * is NOT bound -- writing coil[0] over Wi-Fi moves a number and nothing else.
 * Two stores was already the example's arrangement; binding the pin to both
 * would make the I/O the place where two interfaces synchronise, which is the
 * design the README declines under "Two independent slaves".
 */
bool io_bind_start(mb_store_t *store);

#endif /* IO_BIND_H */
