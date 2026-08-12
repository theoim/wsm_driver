/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Modbus TCP data model and PDU executor (see mb_core.h).
 *
 * No sockets, no lwIP, no FreeRTOS. Everything the wire needs -- big-endian
 * fields, bit packing, exception responses -- happens here so that mb_server.c
 * only has to move bytes.
 *
 * The MBAP frame layout this serves, for reference while reading:
 *
 *     <---------------- Modbus TCP ADU ---------------->
 *                       <-------- PDU ---------->
 *     +-----+-----+-----+-----+------+------------------+
 *     | TID | PID | Len | UID | Func | Data             |
 *     +-----+-----+-----+-----+------+------------------+
 *        2     2     2     1      1     0..252 bytes
 *
 * TID is echoed untouched (the master matches replies with it), PID is 0 for
 * Modbus, and Len counts UID onward -- so Len == PDU length + 1.
 */
#include <string.h>

#include "mb_core.h"

/* ---- wire helpers ---------------------------------------------------------
 * Modbus is big-endian regardless of the host, so read and write it explicitly
 * rather than casting a pointer at the buffer. */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static size_t exception(uint8_t *out, uint8_t func, uint8_t code)
{
    out[0] = (uint8_t)(func | 0x80);    /* high bit marks an exception reply */
    out[1] = code;
    return 2;
}

void mb_datastore_init(mb_datastore_t *ds)
{
    memset(ds, 0, sizeof(*ds));

    /* A pattern a master can recognise at a glance, so a wrong address or a
     * byte-order mistake is visible without a debugger. */
    for (int i = 0; i < MB_REG_COUNT; i++) {
        ds->holding[i] = (uint16_t)(1000 + i);   /* 1000, 1001, 1002, ... */
        ds->input[i]   = (uint16_t)(i * i);      /* 0, 1, 4, 9, 16, ...   */
    }
    for (int i = 0; i < MB_COIL_COUNT; i++) {
        ds->coil[i]     = (i % 2) == 0;          /* alternating            */
        ds->discrete[i] = (i % 4) == 0;          /* every fourth           */
    }
}

/* ---- reads ---------------------------------------------------------------- */

/* 0x01 Read Coils / 0x02 Read Discrete Inputs. Both answer with a bit count
 * packed LSB-first into a byte count, so they differ only in which array. */
static size_t read_bits(const bool *bits, size_t count_max,
                        const uint8_t *pdu, size_t pdu_len, uint8_t *out)
{
    uint8_t func = pdu[0];
    if (pdu_len != 5) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t addr  = rd16(pdu + 1);
    uint16_t count = rd16(pdu + 3);

    /* 2000 is the protocol's own ceiling for a bit read. */
    if (count < 1 || count > 2000) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }
    if ((size_t)addr + count > count_max) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_ADDRESS);
    }

    uint8_t nbytes = (uint8_t)((count + 7) / 8);
    out[0] = func;
    out[1] = nbytes;
    memset(out + 2, 0, nbytes);
    for (uint16_t i = 0; i < count; i++) {
        if (bits[addr + i]) {
            out[2 + (i / 8)] |= (uint8_t)(1u << (i % 8));
        }
    }
    return (size_t)(2 + nbytes);
}

/* 0x03 Read Holding Registers / 0x04 Read Input Registers. */
static size_t read_regs(const uint16_t *regs, size_t count_max,
                        const uint8_t *pdu, size_t pdu_len, uint8_t *out)
{
    uint8_t func = pdu[0];
    if (pdu_len != 5) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t addr  = rd16(pdu + 1);
    uint16_t count = rd16(pdu + 3);

    /* 125 registers is 250 bytes, the most that fits a 253-byte PDU. */
    if (count < 1 || count > 125) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }
    if ((size_t)addr + count > count_max) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_ADDRESS);
    }

    out[0] = func;
    out[1] = (uint8_t)(count * 2);
    for (uint16_t i = 0; i < count; i++) {
        wr16(out + 2 + i * 2, regs[addr + i]);
    }
    return (size_t)(2 + count * 2);
}

/* ---- writes --------------------------------------------------------------- */

/* 0x05 Write Single Coil. The value is 0xFF00 for on and 0x0000 for off --
 * nothing else is legal, which is why this is a value check and not a cast. */
static size_t write_coil(mb_datastore_t *ds, const uint8_t *pdu, size_t pdu_len,
                         uint8_t *out)
{
    uint8_t func = pdu[0];
    if (pdu_len != 5) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t addr  = rd16(pdu + 1);
    uint16_t value = rd16(pdu + 3);

    if (value != 0xFF00 && value != 0x0000) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }
    if (addr >= MB_COIL_COUNT) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_ADDRESS);
    }

    ds->coil[addr] = (value == 0xFF00);

    /* A successful single write echoes the request verbatim. */
    memcpy(out, pdu, 5);
    return 5;
}

/* 0x06 Write Single Register. */
static size_t write_reg(mb_datastore_t *ds, const uint8_t *pdu, size_t pdu_len,
                        uint8_t *out)
{
    uint8_t func = pdu[0];
    if (pdu_len != 5) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t addr = rd16(pdu + 1);
    if (addr >= MB_REG_COUNT) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_ADDRESS);
    }

    ds->holding[addr] = rd16(pdu + 3);

    memcpy(out, pdu, 5);
    return 5;
}

/* 0x0F Write Multiple Coils. */
static size_t write_coils(mb_datastore_t *ds, const uint8_t *pdu, size_t pdu_len,
                          uint8_t *out)
{
    uint8_t func = pdu[0];
    if (pdu_len < 6) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t addr   = rd16(pdu + 1);
    uint16_t count  = rd16(pdu + 3);
    uint8_t  nbytes = pdu[5];

    /* The byte count is redundant with the bit count, so a mismatch means the
     * frame is inconsistent with itself -- reject rather than trust either. */
    if (count < 1 || count > 1968 ||
        nbytes != (count + 7) / 8 || pdu_len != (size_t)6 + nbytes) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }
    if ((size_t)addr + count > MB_COIL_COUNT) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_ADDRESS);
    }

    for (uint16_t i = 0; i < count; i++) {
        ds->coil[addr + i] = (pdu[6 + (i / 8)] >> (i % 8)) & 1u;
    }

    /* A successful multiple write answers with the address and count only. */
    out[0] = func;
    wr16(out + 1, addr);
    wr16(out + 3, count);
    return 5;
}

/* 0x10 Write Multiple Registers. */
static size_t write_regs(mb_datastore_t *ds, const uint8_t *pdu, size_t pdu_len,
                         uint8_t *out)
{
    uint8_t func = pdu[0];
    if (pdu_len < 6) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t addr   = rd16(pdu + 1);
    uint16_t count  = rd16(pdu + 3);
    uint8_t  nbytes = pdu[5];

    if (count < 1 || count > 123 ||
        nbytes != count * 2 || pdu_len != (size_t)6 + nbytes) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_VALUE);
    }
    if ((size_t)addr + count > MB_REG_COUNT) {
        return exception(out, func, MB_EX_ILLEGAL_DATA_ADDRESS);
    }

    for (uint16_t i = 0; i < count; i++) {
        ds->holding[addr + i] = rd16(pdu + 6 + i * 2);
    }

    out[0] = func;
    wr16(out + 1, addr);
    wr16(out + 3, count);
    return 5;
}

size_t mb_pdu_execute(mb_datastore_t *ds, const uint8_t *pdu, size_t pdu_len,
                      uint8_t *out)
{
    if (pdu_len < 1) {
        /* No function code at all. Nothing to echo back but a refusal, and
         * function 0 is not assigned, so it cannot be confused with a reply. */
        return exception(out, 0, MB_EX_ILLEGAL_FUNCTION);
    }

    switch (pdu[0]) {
    case 0x01: return read_bits(ds->coil,     MB_COIL_COUNT, pdu, pdu_len, out);
    case 0x02: return read_bits(ds->discrete, MB_COIL_COUNT, pdu, pdu_len, out);
    case 0x03: return read_regs(ds->holding,  MB_REG_COUNT,  pdu, pdu_len, out);
    case 0x04: return read_regs(ds->input,    MB_REG_COUNT,  pdu, pdu_len, out);
    case 0x05: return write_coil(ds, pdu, pdu_len, out);
    case 0x06: return write_reg(ds,  pdu, pdu_len, out);
    case 0x0F: return write_coils(ds, pdu, pdu_len, out);
    case 0x10: return write_regs(ds,  pdu, pdu_len, out);
    default:   return exception(out, pdu[0], MB_EX_ILLEGAL_FUNCTION);
    }
}
