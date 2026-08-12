/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Modbus TCP protocol core: the data model and the PDU executor.
 *
 * Nothing here touches a socket, a timer, or a UART. mb_pdu_execute() is a pure
 * function over a caller-owned mb_datastore_t -- hand it a request PDU, get a
 * response PDU back -- which is what makes it testable on a host and identical
 * on both network backends.
 *
 * Scope is Modbus TCP only, and that is a deliberate reduction rather than a
 * subset of something bigger. Most of the bulk in a general Modbus library is
 * the serial line: CRC-16, the 3.5-character inter-frame timer, ASCII framing,
 * RTU/ASCII transcoding. TCP has none of it. Framing is the 7-byte MBAP header
 * with an explicit length, the transport already guarantees ordering and
 * integrity, and what remains is this file.
 *
 * Function codes implemented, which is the set mbpoll and Modbus Poll exercise
 * by default:
 *
 *   0x01  Read Coils                 0x05  Write Single Coil
 *   0x02  Read Discrete Inputs       0x06  Write Single Register
 *   0x03  Read Holding Registers     0x0F  Write Multiple Coils
 *   0x04  Read Input Registers       0x10  Write Multiple Registers
 *
 * Anything else returns exception 0x01 (illegal function), which is the correct
 * answer rather than a gap: a master is required to handle it.
 */
#ifndef MB_CORE_H
#define MB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Size of the data model. Small on purpose -- a demonstration server, not a
 * PLC -- but large enough that a master's default 10-register poll fits. */
#define MB_REG_COUNT   64      /* holding + input registers, each */
#define MB_COIL_COUNT  64      /* coils + discrete inputs, each */

/*
 * Longest ADU on Modbus TCP: 7-byte MBAP header + 253-byte PDU. Fixed by the
 * spec, so a static buffer of this size can never be overrun by a well-formed
 * peer and is bounds-checked against a malformed one.
 */
#define MB_MBAP_LEN    7
#define MB_PDU_MAX     253
#define MB_ADU_MAX     (MB_MBAP_LEN + MB_PDU_MAX)

/* Modbus exception codes, returned with the function code's high bit set. */
#define MB_EX_ILLEGAL_FUNCTION      0x01
#define MB_EX_ILLEGAL_DATA_ADDRESS  0x02
#define MB_EX_ILLEGAL_DATA_VALUE    0x03

/*
 * The server's data model.
 *
 * Registers are plain uint16_t in host order; the wire's big-endian layout is
 * applied at the edge, in mb_pdu_execute, so application code never has to
 * think about it. Coils are one bool per element rather than packed bits: the
 * packing is a wire format, and unpacking it here would push that detail into
 * every caller that wants to read a coil.
 */
typedef struct {
    uint16_t holding[MB_REG_COUNT];
    uint16_t input[MB_REG_COUNT];
    bool     coil[MB_COIL_COUNT];
    bool     discrete[MB_COIL_COUNT];
} mb_datastore_t;

/* Fill a datastore with the demo pattern described in the README. */
void mb_datastore_init(mb_datastore_t *ds);

/*
 * Execute one request PDU against `ds` and write the response PDU to `out`.
 *
 *   pdu     - function code followed by its data, NOT including the MBAP header
 *   pdu_len - length of that PDU, at least 1
 *   out     - response buffer, MB_PDU_MAX is always enough
 *
 * Returns the response length, which is always >= 2. A malformed or refused
 * request produces a 2-byte exception response rather than an error return:
 * on Modbus that IS the reply, and the connection stays up.
 */
size_t mb_pdu_execute(mb_datastore_t *ds, const uint8_t *pdu, size_t pdu_len,
                      uint8_t *out);

#endif /* MB_CORE_H */
