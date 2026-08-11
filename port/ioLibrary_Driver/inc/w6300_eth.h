/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * W6300 register map for the esp_eth MACRAW driver (esp_eth_mac_w6300.c /
 * esp_eth_phy_w6300.c). Private to the driver — it deliberately does NOT
 * include the vendored ioLibrary headers so the esp_eth path stays free of the
 * ioLibrary globals (WIZCHIP, socket()/close() names, ...).
 *
 * Named w6300_eth.h (not w6300.h) on purpose: the vendored
 * third_party/ioLibrary_Driver/Ethernet/W6300/w6300.h is on the public include
 * path for the TOE build, and a second "w6300.h" would shadow it.
 *
 * ---------------------------------------------------------------------------
 * Addressing
 * ---------------------------------------------------------------------------
 * W6300 selects a register with a 32-bit value laid out exactly like
 * ioLibrary's AddrSel:
 *
 *     address = (offset << 8) | block
 *
 * and the QSPI frame carries it as:
 *
 *     opcode (8 bits, always 1-line) = block | R/W | QSPI mode bits
 *     address phase (16 bits)        = offset
 *     dummy                          = 8 clocks in single mode, 2 in quad
 *     data                           = payload
 *
 * The dummy phase is emitted by widening the address phase to 24 bits with a
 * trailing driven 0x00 byte — one byte is 8 clocks on 1 line and 2 clocks on 4
 * lines, which is exactly what each mode needs. This matches the official
 * WIZnet reference and the TOE transport in wsm_driver_spi.c bit-for-bit.
 *
 * The offsets below are the same ones the vendored w6300.h defines (e.g.
 * _SYSR_ = 0x2000, _Sn_RX_RD_(N) = 0x0228 + SREG block), so they can be
 * cross-checked against it.
 */

#pragma once

#include <stdint.h>
#include "sdkconfig.h"

/* ---- frame construction --------------------------------------------------- */

#define W6300_ADDR_OFFSET  (8)     // address = (offset << ADDR_OFFSET) | block
#define W6300_BLOCK_MASK   (0xFF)

#define W6300_ACCESS_MODE_READ  (0x00 << 5) // R/W bit of the opcode: read
#define W6300_ACCESS_MODE_WRITE (0x01 << 5) // R/W bit of the opcode: write

/* QSPI mode bits embedded in every opcode. Follows the component Kconfig so the
 * esp_eth path and the TOE path drive the bus identically. */
#ifdef CONFIG_WSM_DRIVER_QSPI_QUAD
#define W6300_QSPI_MODE (0x02 << 6) // 4-bit (quad) mode
#else
#define W6300_QSPI_MODE (0x00 << 6) // 1-bit (single) mode
#endif

/* Block select values (low byte of the address / low bits of the opcode). */
#define W6300_BSB_COM_REG        (0x00)      // Common register block
#define W6300_BSB_SOCK_REG(s)    ((s)*4+1)   // SOCKETn register block
#define W6300_BSB_SOCK_TX_BUF(s) ((s)*4+2)   // SOCKETn TX buffer block
#define W6300_BSB_SOCK_RX_BUF(s) ((s)*4+3)   // SOCKETn RX buffer block

#define W6300_MAKE_MAP(offset, bsb) (((uint32_t)(offset) << W6300_ADDR_OFFSET) | (bsb))

/* ---- common registers ----------------------------------------------------- */

#define W6300_REG_CIDR     W6300_MAKE_MAP(0x0000, W6300_BSB_COM_REG) // Chip ID (2B, RO) — see W6300_CHIP_ID
#define W6300_REG_VER      W6300_MAKE_MAP(0x0002, W6300_BSB_COM_REG) // Chip version (2B, RO)
#define W6300_REG_RTL      W6300_MAKE_MAP(0x0004, W6300_BSB_COM_REG) // RTL revision (1B, RO) — feeds the chip ID
#define W6300_REG_SYSR     W6300_MAKE_MAP(0x2000, W6300_BSB_COM_REG) // System status (lock bits)
#define W6300_REG_SYCR0    W6300_MAKE_MAP(0x2004, W6300_BSB_COM_REG) // System config 0 (soft reset)
#define W6300_REG_SYCR1    W6300_MAKE_MAP(0x2005, W6300_BSB_COM_REG) // System config 1 (global IEN)
#define W6300_REG_IR       W6300_MAKE_MAP(0x2100, W6300_BSB_COM_REG) // Common interrupt (RO)
#define W6300_REG_SIR      W6300_MAKE_MAP(0x2101, W6300_BSB_COM_REG) // Socket interrupt (RO)
#define W6300_REG_IMR      W6300_MAKE_MAP(0x2104, W6300_BSB_COM_REG) // Common interrupt mask
#define W6300_REG_IRCLR    W6300_MAKE_MAP(0x2108, W6300_BSB_COM_REG) // Common interrupt clear (W1C)
#define W6300_REG_SIMR     W6300_MAKE_MAP(0x2114, W6300_BSB_COM_REG) // Socket interrupt mask
#define W6300_REG_PHYSR    W6300_MAKE_MAP(0x3000, W6300_BSB_COM_REG) // PHY status (RO)
#define W6300_REG_PHYCR0   W6300_MAKE_MAP(0x301C, W6300_BSB_COM_REG) // PHY operation mode
#define W6300_REG_PHYCR1   W6300_MAKE_MAP(0x301D, W6300_BSB_COM_REG) // PHY reset / power down
#define W6300_REG_NET4MR   W6300_MAKE_MAP(0x4000, W6300_BSB_COM_REG) // IPv4 network mode
#define W6300_REG_NET6MR   W6300_MAKE_MAP(0x4004, W6300_BSB_COM_REG) // IPv6 network mode
#define W6300_REG_NETMR    W6300_MAKE_MAP(0x4008, W6300_BSB_COM_REG) // Network mode
#define W6300_REG_MAC      W6300_MAKE_MAP(0x4120, W6300_BSB_COM_REG) // Source hardware address (6B)
#define W6300_REG_INTPTMR  W6300_MAKE_MAP(0x41C5, W6300_BSB_COM_REG) // Interrupt pending time (2B)
#define W6300_REG_CHPLCKR  W6300_MAKE_MAP(0x41F4, W6300_BSB_COM_REG) // Chip config lock (WO)
#define W6300_REG_NETLCKR  W6300_MAKE_MAP(0x41F5, W6300_BSB_COM_REG) // Network config lock (WO)
#define W6300_REG_PHYLCKR  W6300_MAKE_MAP(0x41F6, W6300_BSB_COM_REG) // PHY config lock (WO)

/* ---- socket registers ----------------------------------------------------- */

#define W6300_REG_SOCK_MR(s)      W6300_MAKE_MAP(0x0000, W6300_BSB_SOCK_REG(s)) // Socket mode
#define W6300_REG_SOCK_CR(s)      W6300_MAKE_MAP(0x0010, W6300_BSB_SOCK_REG(s)) // Socket command
#define W6300_REG_SOCK_IR(s)      W6300_MAKE_MAP(0x0020, W6300_BSB_SOCK_REG(s)) // Socket interrupt (RO)
#define W6300_REG_SOCK_IMR(s)     W6300_MAKE_MAP(0x0024, W6300_BSB_SOCK_REG(s)) // Socket interrupt mask
#define W6300_REG_SOCK_IRCLR(s)   W6300_MAKE_MAP(0x0028, W6300_BSB_SOCK_REG(s)) // Socket int clear (W1C)
#define W6300_REG_SOCK_SR(s)      W6300_MAKE_MAP(0x0030, W6300_BSB_SOCK_REG(s)) // Socket status
#define W6300_REG_SOCK_TXBUF_SIZE(s) W6300_MAKE_MAP(0x0200, W6300_BSB_SOCK_REG(s)) // TX buffer size (KB)
#define W6300_REG_SOCK_TX_FSR(s)  W6300_MAKE_MAP(0x0204, W6300_BSB_SOCK_REG(s)) // TX free size (2B, RO)
#define W6300_REG_SOCK_TX_RD(s)   W6300_MAKE_MAP(0x0208, W6300_BSB_SOCK_REG(s)) // TX read pointer (2B)
#define W6300_REG_SOCK_TX_WR(s)   W6300_MAKE_MAP(0x020C, W6300_BSB_SOCK_REG(s)) // TX write pointer (2B)
#define W6300_REG_SOCK_RXBUF_SIZE(s) W6300_MAKE_MAP(0x0220, W6300_BSB_SOCK_REG(s)) // RX buffer size (KB)
#define W6300_REG_SOCK_RX_RSR(s)  W6300_MAKE_MAP(0x0224, W6300_BSB_SOCK_REG(s)) // RX received size (2B, RO)
#define W6300_REG_SOCK_RX_RD(s)   W6300_MAKE_MAP(0x0228, W6300_BSB_SOCK_REG(s)) // RX read pointer (2B)
#define W6300_REG_SOCK_RX_WR(s)   W6300_MAKE_MAP(0x022C, W6300_BSB_SOCK_REG(s)) // RX write pointer (2B)

#define W6300_MEM_SOCK_TX(s, addr) W6300_MAKE_MAP(addr, W6300_BSB_SOCK_TX_BUF(s))
#define W6300_MEM_SOCK_RX(s, addr) W6300_MAKE_MAP(addr, W6300_BSB_SOCK_RX_BUF(s))

/* ---- bit definitions ------------------------------------------------------ */

/* Chip ID. CIDR on its own does NOT read back 0x6300: the silicon reports 0x61
 * in CIDR[0] and the missing bit comes from the RTL revision register. ioLibrary
 * composes the two in getCIDR() (third_party/.../W6300/w6300.h):
 *
 *     id = ((CIDR[0] | ((RTL & 0x0F) << 1)) << 8) | CIDR[1]
 *
 * so a plain 2-byte CIDR read compared against 0x6300 always fails with 0x6100.
 * w6300_verify_id() reproduces the composition instead. */
#define W6300_CHIP_ID  (0x6300) // composed CIDR + RTL value
#define W6300_RTL_MASK (0x0F)   // RTL bits that feed into the chip ID

/* SYSR — lock status. NOTE the polarity: 1 = LOCKED, 0 = unlocked. The W6300
 * comes out of reset with all three LOCKED (SYSR = 0xE0), which is why the MAC
 * has to unlock before writing SHAR / PHYCR — a step the W5500 has no notion
 * of, and the single biggest difference in bring-up between the two chips. */
#define W6300_SYSR_CHPL (1 << 7) // chip config registers locked
#define W6300_SYSR_NETL (1 << 6) // network config registers (SHAR/SIPR/...) locked
#define W6300_SYSR_PHYL (1 << 5) // PHY config registers locked

/* Magic values for the write-only lock registers. */
#define W6300_CHIP_UNLOCK (0xCE)
#define W6300_CHIP_LOCK   (0xFF)
#define W6300_NET_UNLOCK  (0x3A)
#define W6300_NET_LOCK    (0xC5)
#define W6300_PHY_UNLOCK  (0x53)
#define W6300_PHY_LOCK    (0xFF)

/* SYCR0 — RST bit: 0 triggers the soft reset, 1 is normal operation, so the
 * reset is performed by writing 0x00 (not by setting a bit as on the W5500). */
#define W6300_SYCR0_RST    (0x00)
#define W6300_SYCR0_NORMAL (0x80)

/* SYCR1 — global interrupt enable. Without this the INTn pin never asserts,
 * regardless of IMR/SIMR/Sn_IMR. The W5500 has no equivalent. */
#define W6300_SYCR1_IEN    (1 << 7)
#define W6300_SYCR1_CLKSEL (1 << 0)

/* Sn_MR — protocol in bits[3:0], option flags above. */
#define W6300_SMR_MAC_RAW          (0x07)    // MACRAW mode
#define W6300_SMR_MAC_FILTER       (1 << 7)  // MF: accept only own/broadcast frames
#define W6300_SMR_BLOCK_BCAST      (1 << 6)  // BRDB: block broadcast
#define W6300_SMR_MAC_BLOCK_MCAST  (1 << 5)  // MMB4: block IPv4 multicast
#define W6300_SMR_MAC_BLOCK_MCAST6 (1 << 4)  // MMB6: block IPv6 multicast

/* Sn_CR */
#define W6300_SCR_OPEN  (0x01)
#define W6300_SCR_CLOSE (0x10)
#define W6300_SCR_SEND  (0x20)
#define W6300_SCR_RECV  (0x40)

/* Sn_IR / Sn_IRCLR. Sn_IR is read-only on the W6300 — interrupts are
 * acknowledged by writing the bit to the separate Sn_IRCLR register, unlike the
 * W5500 where Sn_IR itself is write-1-to-clear. */
#define W6300_SIR_SENDOK  (1 << 4)
#define W6300_SIR_TIMEOUT (1 << 3)
#define W6300_SIR_RECV    (1 << 2)

/* SIMR — one bit per socket. */
#define W6300_SIMR_SOCK0 (1 << 0)

/* Sn_SR */
#define W6300_SOCK_CLOSED (0x00)
#define W6300_SOCK_MACRAW (0x42)

/* NETxMR (IPv4 / IPv6 network mode) */
#define W6300_NETXMR_PB (1 << 0) // block ICMP ping replies

/* PHYSR. Careful: SPD and DPX are INVERTED with respect to the W5500's
 * PHYCFGR — here a set bit means 10Mbps / half duplex. */
#define W6300_PHYSR_LNK      (1 << 0) // 1 = link up
#define W6300_PHYSR_SPD      (1 << 1) // 1 = 10Mbps, 0 = 100Mbps
#define W6300_PHYSR_DPX      (1 << 2) // 1 = half duplex, 0 = full duplex
#define W6300_PHYSR_MODE     (7 << 3) // current operation mode (MODE[2:0])
/* Top bit of the MODE field. Datasheet 4.1.18: MODE2 = 0 means auto negotiation
 * (MODE1/MODE0 are then don't-care). This is the ONLY way to read back what
 * PHYCR0 was set to, because PHYCR0 itself is write-only (4.1.24: "Bits set by
 * PHYCR0 can be checked to PHYSR [5:3]") — which is also why ioLibrary offers
 * setPHYCR0() but no getPHYCR0(). */
#define W6300_PHYSR_MODE2    (1 << 5) // 0 = auto negotiation, 1 = fixed mode
#define W6300_PHYSR_CAB_OFF  (1 << 7) // 1 = cable unplugged

/* PHYCR0 — operation mode */
#define W6300_PHYCR0_AUTO (0x00)
#define W6300_PHYCR0_100F (0x04)
#define W6300_PHYCR0_100H (0x05)
#define W6300_PHYCR0_10F  (0x06)
#define W6300_PHYCR0_10H  (0x07)

/* PHYCR1 — writing RST=1 starts a PHY hardware reset; the bit self-clears and
 * the PHY needs ~60.3 ms to stabilise afterwards. */
#define W6300_PHYCR1_RST  (1 << 0)
#define W6300_PHYCR1_TE   (1 << 3)
#define W6300_PHYCR1_PWDN (1 << 5)

#define W6300_PHY_RESET_STABILIZE_MS (70) // datasheet: 60.3 ms after PHYCR1_RST
