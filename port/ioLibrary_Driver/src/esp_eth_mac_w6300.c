/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * esp_eth MAC driver for the WIZnet W6300 (MACRAW on hardware socket 0 + the
 * software LwIP stack). Structure follows the vendored esp_eth_mac_w5500.c so
 * the two backends behave alike from esp_eth's point of view; everything
 * chip-specific is different and is called out inline:
 *
 *   1) Transport. The W5500 uses a full-duplex frame of
 *      addr(16b) + control(8b: block|RW|VDM) + data. The W6300 uses a
 *      half-duplex QSPI frame of opcode(8b: block|RW|mode) + addr(16b) +
 *      dummy + data, optionally 4-bit wide. See w6300_eth.h and w6300_qspi_*().
 *
 *   2) Locks. The W6300 boots with its chip / network / PHY register groups
 *      LOCKED (SYSR = 0xE0, 1 = locked) and re-locks after every reset. SYCR0
 *      (soft reset), SHAR (MAC address) and PHYCR0/1 are unwritable until the
 *      matching lock register is unlocked, so w6300_unlock_all() runs right
 *      after the reset and the result is verified through SYSR. The W5500 has
 *      no equivalent, which is why this cannot be a rename of that driver.
 *
 *   3) Interrupts. Sn_IR is read-only; acknowledgement goes to the separate
 *      Sn_IRCLR register. A global enable (SYCR1.IEN) must also be set or INTn
 *      never asserts.
 *
 *   4) Soft reset is triggered by writing 0 to SYCR0.RST (not by setting a bit),
 *      and the chip ID lives in CIDR (0x6300) rather than in a version register.
 *
 * The RX side (2-byte PACKET-INFO header whose length field includes the header
 * itself, Sn_RX_RD advanced by software and committed with Sn_CR_RECV) works the
 * same way as on the W5500, except that only bits[2:0] of the header's high byte
 * belong to the length — the upper bits are packet-info flags.
 */
#include <string.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include <inttypes.h>
#include "esp_eth_mac_spi.h"
#include "esp_eth_mac_w6300.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/io_mux_reg.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "w6300_eth.h"
#include "sdkconfig.h"

static const char *TAG = "w6300.mac";

#define W6300_SPI_LOCK_TIMEOUT_MS (50)
#define W6300_100M_TX_TMO_US (200)
#define W6300_10M_TX_TMO_US (1500)
#define W6300_ETH_MAC_RX_BUF_SIZE_AUTO (0)

/* MACRAW is only available on socket 0 (same restriction as the W5500). */
#define W6300_MACRAW_SOCK (0)

/* Whole TX/RX memory to socket 0, nothing to the others. The per-socket size is
 * in KB and only 0/1/2/4/8/16 are legal; the sum over all sockets must fit the
 * chip's TX (resp. RX) memory. 16 KB on socket 0 is the largest value that is
 * valid under both the datasheet's 16 KB-per-direction figure and the
 * ioLibrary's more permissive 32 KB check, so it is what we use. A larger TX
 * buffer buys nothing anyway: each SEND is one frame and must complete before
 * the next. */
#define W6300_SOCK0_BUF_KB (16)

/* Only bits[2:0] of the first PACKET-INFO byte are the length; the rest are
 * flags (IPv6 / multicast / completed ...) that stay 0 for MACRAW frames. */
#define W6300_RX_HEADER_LEN_MASK (0x07)
#define W6300_RX_HEADER_SIZE (2)

typedef struct {
    uint32_t offset;
    uint32_t copy_len;
    uint32_t rx_len;
    uint32_t remain;
} __attribute__((packed)) emac_w6300_auto_buf_info_t;

typedef struct {
    spi_device_handle_t hdl;
    SemaphoreHandle_t lock;
} eth_spi_info_t;

typedef struct {
    void *ctx;
    void *(*init)(const void *spi_config);
    esp_err_t (*deinit)(void *spi_ctx);
    esp_err_t (*read)(void *spi_ctx, uint32_t cmd, uint32_t addr, void *data, uint32_t data_len);
    esp_err_t (*write)(void *spi_ctx, uint32_t cmd, uint32_t addr, const void *data, uint32_t data_len);
} eth_spi_custom_driver_t;

typedef struct {
    esp_eth_mac_t parent;
    esp_eth_mediator_t *eth;
    eth_spi_custom_driver_t spi;
    TaskHandle_t rx_task_hdl;
    uint32_t sw_reset_timeout_ms;
    int int_gpio_num;
    esp_timer_handle_t poll_timer;
    uint32_t poll_period_ms;
    uint8_t addr[ETH_ADDR_LEN];
    bool packets_remain;
    uint8_t *rx_buffer;
    uint8_t mcast_cnt;
    uint32_t tx_tmo;
} emac_w6300_t;

/* ===========================================================================
 * SPI / QSPI transport
 * ========================================================================= */

static void *w6300_spi_init(const void *spi_config)
{
    void *ret = NULL;
    eth_w6300_config_t *w6300_config = (eth_w6300_config_t *)spi_config;
    eth_spi_info_t *spi = calloc(1, sizeof(eth_spi_info_t));
    ESP_GOTO_ON_FALSE(spi, NULL, err, TAG, "no memory for SPI context data");

    /* SPI device init */
    spi_device_interface_config_t spi_devcfg;
    spi_devcfg = *(w6300_config->spi_devcfg);
    /* The QSPI frame is phased, so the device has to be half-duplex — a
     * full-duplex device silently corrupts every read. */
    ESP_GOTO_ON_FALSE(spi_devcfg.flags & SPI_DEVICE_HALFDUPLEX, NULL, err, TAG,
                      "W6300 requires SPI_DEVICE_HALFDUPLEX");
    if (spi_devcfg.command_bits == 0 && spi_devcfg.address_bits == 0) {
        /* configure default QSPI frame format */
        spi_devcfg.command_bits = 8;  // opcode phase: block | R/W | QSPI mode
        spi_devcfg.address_bits = 24; // 16-bit address + 8 dummy bits (see w6300_eth.h)
    } else {
        ESP_GOTO_ON_FALSE(spi_devcfg.command_bits == 8 && spi_devcfg.address_bits == 24,
                          NULL, err, TAG, "incorrect QSPI frame format (command_bits/address_bits)");
    }
    ESP_GOTO_ON_FALSE(spi_bus_add_device(w6300_config->spi_host_id, &spi_devcfg, &spi->hdl) == ESP_OK, NULL,
                      err, TAG, "adding device to SPI host #%i failed", w6300_config->spi_host_id + 1);
    /* create mutex */
    spi->lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(spi->lock, NULL, err, TAG, "create lock failed");

    ret = spi;
    return ret;
err:
    if (spi) {
        if (spi->lock) {
            vSemaphoreDelete(spi->lock);
        }
        free(spi);
    }
    return ret;
}

static esp_err_t w6300_spi_deinit(void *spi_ctx)
{
    esp_err_t ret = ESP_OK;
    eth_spi_info_t *spi = (eth_spi_info_t *)spi_ctx;

    spi_bus_remove_device(spi->hdl);
    vSemaphoreDelete(spi->lock);

    free(spi);
    return ret;
}

static inline bool w6300_spi_lock(eth_spi_info_t *spi)
{
    return xSemaphoreTake(spi->lock, pdMS_TO_TICKS(W6300_SPI_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static inline bool w6300_spi_unlock(eth_spi_info_t *spi)
{
    return xSemaphoreGive(spi->lock) == pdTRUE;
}

/* Common part of the two QSPI callbacks: build the phased transaction. `cmd` is
 * the 8-bit opcode, `addr` the already-shifted 24-bit address phase. */
static esp_err_t w6300_qspi_xfer(eth_spi_info_t *spi, uint32_t cmd, uint32_t addr,
                                 const void *tx, void *rx, uint32_t len)
{
    esp_err_t ret = ESP_OK;

    spi_transaction_ext_t trans = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY,
            .cmd = cmd,
            .addr = addr,
            .length = (tx != NULL) ? 8 * len : 0,
            .rxlength = (rx != NULL) ? 8 * len : 0,
        },
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits = 0,
    };
#ifdef CONFIG_WSM_DRIVER_QSPI_QUAD
    /* Data and address go out on 4 lines; the opcode always stays 1-line. */
    trans.base.flags |= SPI_TRANS_MODE_QIO | SPI_TRANS_MULTILINE_ADDR;
#endif

    /* Small register accesses use the transaction's inline buffers: DMA writes
     * whole words, so a 1/2-byte read into a stack variable could clobber its
     * neighbours, and a short unaligned DMA buffer is rejected outright. */
    bool inline_buf = (len <= 4);
    if (inline_buf) {
        if (tx != NULL) {
            trans.base.flags |= SPI_TRANS_USE_TXDATA;
            memcpy(trans.base.tx_data, tx, len);
        }
        if (rx != NULL) {
            trans.base.flags |= SPI_TRANS_USE_RXDATA;
        }
    } else {
        trans.base.tx_buffer = tx;
        trans.base.rx_buffer = rx;
    }

    if (w6300_spi_lock(spi)) {
        if (spi_device_polling_transmit(spi->hdl, (spi_transaction_t *)&trans) != ESP_OK) {
            ESP_LOGE(TAG, "%s(%d): spi transmit failed", __FUNCTION__, __LINE__);
            ret = ESP_FAIL;
        }
        w6300_spi_unlock(spi);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }

    if (ret == ESP_OK && inline_buf && rx != NULL) {
        memcpy(rx, trans.base.rx_data, len);
    }
    return ret;
}

static esp_err_t w6300_spi_write(void *spi_ctx, uint32_t cmd, uint32_t addr, const void *value, uint32_t len)
{
    return w6300_qspi_xfer((eth_spi_info_t *)spi_ctx, cmd, addr, value, NULL, len);
}

static esp_err_t w6300_spi_read(void *spi_ctx, uint32_t cmd, uint32_t addr, void *value, uint32_t len)
{
    return w6300_qspi_xfer((eth_spi_info_t *)spi_ctx, cmd, addr, NULL, value, len);
}

/* address == (offset << 8) | block  ->  opcode = block | R/W | mode,
 *                                       address phase = offset << 8 (dummy byte) */
static esp_err_t w6300_read(emac_w6300_t *emac, uint32_t address, void *data, uint32_t len)
{
    uint32_t cmd = (address & W6300_BLOCK_MASK) | W6300_ACCESS_MODE_READ | W6300_QSPI_MODE;
    uint32_t addr = (address >> W6300_ADDR_OFFSET) << 8;

    return emac->spi.read(emac->spi.ctx, cmd, addr, data, len);
}

static esp_err_t w6300_write(emac_w6300_t *emac, uint32_t address, const void *data, uint32_t len)
{
    uint32_t cmd = (address & W6300_BLOCK_MASK) | W6300_ACCESS_MODE_WRITE | W6300_QSPI_MODE;
    uint32_t addr = (address >> W6300_ADDR_OFFSET) << 8;

    return emac->spi.write(emac->spi.ctx, cmd, addr, data, len);
}

static esp_err_t w6300_write_u8(emac_w6300_t *emac, uint32_t address, uint8_t value)
{
    return w6300_write(emac, address, &value, sizeof(value));
}

/* The 16-bit registers are big-endian and auto-increment inside the chip, so a
 * 2-byte burst is equivalent to the two single-byte accesses the ioLibrary
 * macros perform. */
static esp_err_t w6300_read_u16(emac_w6300_t *emac, uint32_t address, uint16_t *value)
{
    esp_err_t ret = ESP_OK;
    uint8_t buf[2] = {0};

    ESP_GOTO_ON_ERROR(w6300_read(emac, address, buf, sizeof(buf)), err, TAG, "read u16 failed");
    *value = ((uint16_t)buf[0] << 8) | buf[1];
err:
    return ret;
}

static esp_err_t w6300_write_u16(emac_w6300_t *emac, uint32_t address, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value >> 8), (uint8_t)value };

    return w6300_write(emac, address, buf, sizeof(buf));
}

/* ===========================================================================
 * chip housekeeping
 * ========================================================================= */

static esp_err_t w6300_send_command(emac_w6300_t *emac, uint8_t command, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_CR(W6300_MACRAW_SOCK), command),
                      err, TAG, "write Sn_CR failed");
    /* the command register clears itself once the chip has accepted it */
    uint32_t to = 0;
    for (to = 0; to < timeout_ms / 10; to++) {
        ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SOCK_CR(W6300_MACRAW_SOCK), &command, sizeof(command)),
                          err, TAG, "read Sn_CR failed");
        if (!command) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_GOTO_ON_FALSE(to < timeout_ms / 10, ESP_ERR_TIMEOUT, err, TAG, "send command timeout");

err:
    return ret;
}

/* Sn_IR is read-only; acknowledge through Sn_IRCLR (write 1 to clear). */
static esp_err_t w6300_clear_sock_intr(emac_w6300_t *emac, uint8_t bits)
{
    return w6300_write_u8(emac, W6300_REG_SOCK_IRCLR(W6300_MACRAW_SOCK), bits);
}

static esp_err_t w6300_get_tx_free_size(emac_w6300_t *emac, uint16_t *size)
{
    esp_err_t ret = ESP_OK;
    uint16_t free0 = 0, free1 = 0;
    /* read TX_FSR more than once until two reads agree: the 16-bit value is
     * fetched in two byte accesses and the chip may update it in between */
    do {
        ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_TX_FSR(W6300_MACRAW_SOCK), &free0),
                          err, TAG, "read TX FSR failed");
        ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_TX_FSR(W6300_MACRAW_SOCK), &free1),
                          err, TAG, "read TX FSR failed");
    } while (free0 != free1);

    *size = free0;

err:
    return ret;
}

static esp_err_t w6300_get_rx_received_size(emac_w6300_t *emac, uint16_t *size)
{
    esp_err_t ret = ESP_OK;
    uint16_t received0 = 0, received1 = 0;
    do {
        ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_RX_RSR(W6300_MACRAW_SOCK), &received0),
                          err, TAG, "read RX RSR failed");
        ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_RX_RSR(W6300_MACRAW_SOCK), &received1),
                          err, TAG, "read RX RSR failed");
    } while (received0 != received1);
    *size = received0;

err:
    return ret;
}

static esp_err_t w6300_write_buffer(emac_w6300_t *emac, const void *buffer, uint32_t len, uint16_t offset)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_ERROR(w6300_write(emac, W6300_MEM_SOCK_TX(W6300_MACRAW_SOCK, offset), buffer, len),
                      err, TAG, "write TX buffer failed");
err:
    return ret;
}

static esp_err_t w6300_read_buffer(emac_w6300_t *emac, void *buffer, uint32_t len, uint16_t offset)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_MEM_SOCK_RX(W6300_MACRAW_SOCK, offset), buffer, len),
                      err, TAG, "read RX buffer failed");
err:
    return ret;
}

static esp_err_t w6300_set_mac_addr(emac_w6300_t *emac)
{
    esp_err_t ret = ESP_OK;
    /* SHAR is behind the network lock (unlocked in w6300_unlock_all) and is a
     * 6-byte burst, so keep it word aligned for the DMA path. */
    WORD_ALIGNED_ATTR uint8_t shar[ETH_ADDR_LEN];

    memcpy(shar, emac->addr, sizeof(shar));
    ESP_GOTO_ON_ERROR(w6300_write(emac, W6300_REG_MAC, shar, sizeof(shar)), err, TAG,
                      "write MAC address register failed");
err:
    return ret;
}

/* Open up the chip / network / PHY register groups and confirm through SYSR that
 * they really are writable. The W6300 boots locked and re-locks on every reset,
 * so this has to run after each one — before SHAR or PHYCR0/1 are touched. */
static esp_err_t w6300_unlock_all(emac_w6300_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint8_t sysr = 0;

    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_CHPLCKR, W6300_CHIP_UNLOCK), err, TAG, "chip unlock failed");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_NETLCKR, W6300_NET_UNLOCK), err, TAG, "net unlock failed");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_PHYLCKR, W6300_PHY_UNLOCK), err, TAG, "phy unlock failed");

    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SYSR, &sysr, sizeof(sysr)), err, TAG, "read SYSR failed");
    /* SYSR bit set == still locked */
    ESP_GOTO_ON_FALSE(!(sysr & (W6300_SYSR_CHPL | W6300_SYSR_NETL | W6300_SYSR_PHYL)), ESP_ERR_INVALID_STATE,
                      err, TAG, "registers still locked (SYSR=0x%02" PRIx8 ")", sysr);
err:
    return ret;
}

/* Compose the chip ID exactly like ioLibrary's getCIDR(): CIDR alone yields
 * 0x6100, the remaining bit lives in the RTL revision register. See the
 * W6300_CHIP_ID comment in w6300_eth.h. */
static esp_err_t w6300_read_id(emac_w6300_t *emac, uint16_t *id)
{
    esp_err_t ret = ESP_OK;
    uint8_t cidr[2] = {0};
    uint8_t rtl = 0;

    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_CIDR, cidr, sizeof(cidr)), err, TAG, "read CIDR failed");
    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_RTL, &rtl, sizeof(rtl)), err, TAG, "read RTL failed");
    *id = (uint16_t)(((cidr[0] | ((rtl & W6300_RTL_MASK) << 1)) << 8) | cidr[1]);
err:
    return ret;
}

static esp_err_t w6300_verify_id(emac_w6300_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint16_t id = 0;

    /* Poll: right after a reset the chip may answer before it is fully up. */
    ESP_LOGD(TAG, "Waiting W6300 to start & verify chip ID...");
    uint32_t to = 0;
    for (to = 0; to < emac->sw_reset_timeout_ms / 10; to++) {
        ESP_GOTO_ON_ERROR(w6300_read_id(emac, &id), err, TAG, "read chip ID failed");
        if (id == W6300_CHIP_ID) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "W6300 chip ID mismatched, expected 0x%04x, got 0x%04" PRIx16, W6300_CHIP_ID, id);
    return ESP_ERR_INVALID_VERSION;
err:
    return ret;
}

static esp_err_t w6300_reset(emac_w6300_t *emac)
{
    esp_err_t ret = ESP_OK;

    /* SYCR0 itself is behind the chip lock, so unlock before resetting. */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_CHPLCKR, W6300_CHIP_UNLOCK), err, TAG, "chip unlock failed");
    /* RST is active low on this chip: writing 0 starts the soft reset. */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SYCR0, W6300_SYCR0_RST), err, TAG, "write SYCR0 failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* The chip ID register is readable regardless of the locks — use it to tell
     * when the chip is back, then re-open the register groups (the reset put
     * them back into their locked default). */
    ESP_GOTO_ON_ERROR(w6300_verify_id(emac), err, TAG, "chip did not come back after reset");
    ESP_GOTO_ON_ERROR(w6300_unlock_all(emac), err, TAG, "unlock after reset failed");

err:
    return ret;
}

static esp_err_t w6300_setup_default(emac_w6300_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint8_t sycr1 = 0;

    /* Whole TX/RX memory to socket 0 (the only MACRAW-capable one), none to the
     * rest — an over-committed total would silently break transfers. */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_RXBUF_SIZE(0), W6300_SOCK0_BUF_KB),
                      err, TAG, "set rx buffer size failed");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_TXBUF_SIZE(0), W6300_SOCK0_BUF_KB),
                      err, TAG, "set tx buffer size failed");
    for (int i = 1; i < 8; i++) {
        ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_RXBUF_SIZE(i), 0),
                          err, TAG, "set rx buffer size failed");
        ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_TXBUF_SIZE(i), 0),
                          err, TAG, "set tx buffer size failed");
    }

    /* The chip's own stack is bypassed in MACRAW, but block ICMP ping replies
     * anyway so the chip never answers for an address LwIP owns. */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_NET4MR, W6300_NETXMR_PB), err, TAG, "write NET4MR failed");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_NET6MR, W6300_NETXMR_PB), err, TAG, "write NET6MR failed");

    /* No common interrupts, no socket interrupts yet (enabled on start()). */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_IMR, 0), err, TAG, "write IMR failed");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SIMR, 0), err, TAG, "write SIMR failed");

    /* MACRAW on socket 0: MAC filter on, broadcast allowed, IPv4 multicast
     * blocked until the stack asks for a group (mirrors the W5500 driver). */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_MR(W6300_MACRAW_SOCK),
                                     W6300_SMR_MAC_RAW | W6300_SMR_MAC_FILTER | W6300_SMR_MAC_BLOCK_MCAST),
                      err, TAG, "write Sn_MR failed");
    /* Receive event only. */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_IMR(W6300_MACRAW_SOCK), W6300_SIR_RECV),
                      err, TAG, "write Sn_IMR failed");
    /* Re-assert INTn as soon as an unmasked interrupt is still pending (0 = no
     * pending time). Combined with the RX task's level check this keeps the
     * latency of a missed edge bounded. */
    ESP_GOTO_ON_ERROR(w6300_write_u16(emac, W6300_REG_INTPTMR, 0), err, TAG, "write INTPTMR failed");

    /* Global interrupt enable — without it INTn never asserts. Read-modify-write
     * so the clock-select bit keeps its value. */
    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SYCR1, &sycr1, sizeof(sycr1)), err, TAG, "read SYCR1 failed");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SYCR1, sycr1 | W6300_SYCR1_IEN),
                      err, TAG, "write SYCR1 failed");

err:
    return ret;
}

/* ===========================================================================
 * esp_eth_mac_t implementation
 * ========================================================================= */

static esp_err_t emac_w6300_start(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    uint8_t status = 0;

    /* open socket 0 in MACRAW */
    ESP_GOTO_ON_ERROR(w6300_send_command(emac, W6300_SCR_OPEN, 100), err, TAG, "issue OPEN command failed");
    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SOCK_SR(W6300_MACRAW_SOCK), &status, sizeof(status)),
                      err, TAG, "read Sn_SR failed");
    ESP_GOTO_ON_FALSE(status == W6300_SOCK_MACRAW, ESP_ERR_INVALID_STATE, err, TAG,
                      "socket did not enter MACRAW (Sn_SR=0x%02" PRIx8 ")", status);
    /* enable interrupt for socket 0 */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SIMR, W6300_SIMR_SOCK0), err, TAG, "write SIMR failed");

err:
    return ret;
}

static esp_err_t emac_w6300_stop(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    /* disable interrupt */
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SIMR, 0), err, TAG, "write SIMR failed");
    /* close socket 0 */
    ESP_GOTO_ON_ERROR(w6300_send_command(emac, W6300_SCR_CLOSE, 100), err, TAG, "issue CLOSE command failed");

err:
    return ret;
}

static esp_err_t emac_w6300_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(eth, ESP_ERR_INVALID_ARG, err, TAG, "can't set mac's mediator to null");
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    emac->eth = eth;
    return ESP_OK;
err:
    return ret;
}

/* The W6300's PHY registers live in the same address space as the MAC ones, so
 * the PHY driver reaches them through the mediator. Unlike the W5500 (single
 * PHYCFGR) there are three: PHYSR (status), PHYCR0 (mode), PHYCR1 (reset/pwr). */
static bool w6300_is_phy_reg(uint32_t phy_reg)
{
    return phy_reg == W6300_REG_PHYSR || phy_reg == W6300_REG_PHYCR0 || phy_reg == W6300_REG_PHYCR1;
}

static esp_err_t emac_w6300_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    ESP_GOTO_ON_FALSE(w6300_is_phy_reg(phy_reg), ESP_FAIL, err, TAG, "wrong PHY register");
    ESP_GOTO_ON_FALSE(phy_reg != W6300_REG_PHYSR, ESP_FAIL, err, TAG, "PHYSR is read-only");
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, phy_reg, (uint8_t)reg_value), err, TAG, "write PHY register failed");

err:
    return ret;
}

static esp_err_t emac_w6300_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(reg_value, ESP_ERR_INVALID_ARG, err, TAG, "can't set reg_value to null");
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    ESP_GOTO_ON_FALSE(w6300_is_phy_reg(phy_reg), ESP_FAIL, err, TAG, "wrong PHY register");
    uint8_t value = 0;
    ESP_GOTO_ON_ERROR(w6300_read(emac, phy_reg, &value, sizeof(value)), err, TAG, "read PHY register failed");
    *reg_value = value;

err:
    return ret;
}

static esp_err_t emac_w6300_set_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    memcpy(emac->addr, addr, ETH_ADDR_LEN);
    ESP_GOTO_ON_ERROR(w6300_set_mac_addr(emac), err, TAG, "set mac address failed");

err:
    return ret;
}

static esp_err_t emac_w6300_get_addr(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    memcpy(addr, emac->addr, ETH_ADDR_LEN);

err:
    return ret;
}

/* MAC-filter / all-multicast callbacks were added to esp_eth_mac_t in ESP-IDF
 * 5.5. The component requires >= 6.0, so they are unconditionally present. */
static esp_err_t emac_w6300_set_block_ip4_mcast(esp_eth_mac_t *mac, bool block)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    uint8_t smr;
    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SOCK_MR(W6300_MACRAW_SOCK), &smr, sizeof(smr)),
                      err, TAG, "read Sn_MR failed");
    if (block) {
        smr |= W6300_SMR_MAC_BLOCK_MCAST;
    } else {
        smr &= ~W6300_SMR_MAC_BLOCK_MCAST;
    }
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_MR(W6300_MACRAW_SOCK), smr),
                      err, TAG, "write Sn_MR failed");
err:
    return ret;
}

static esp_err_t emac_w6300_add_mac_filter(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    /* Like the W5500, the W6300 has no per-address filter in MACRAW: multicast
     * is only blockable as a class (Sn_MR MMB4 for IPv4, MMB6 for IPv6), so
     * joining a group just un-blocks the corresponding class. */
    if (addr[0] == 0x01 && addr[1] == 0x00 && addr[2] == 0x5e) {
        ESP_GOTO_ON_ERROR(emac_w6300_set_block_ip4_mcast(mac, false), err, TAG, "set block multicast failed");
        emac->mcast_cnt++;
    } else if (addr[0] == 0x33 && addr[1] == 0x33) {
        ESP_LOGW(TAG, "IPv6 multicast is not blocked by W6300 (MMB6 left clear).");
    } else {
        ESP_LOGE(TAG, "W6300 filters in IP multicast frames only!");
        ret = ESP_ERR_NOT_SUPPORTED;
    }
err:
    return ret;
}

static esp_err_t emac_w6300_del_mac_filter(esp_eth_mac_t *mac, uint8_t *addr)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);

    ESP_GOTO_ON_FALSE(!(addr[0] == 0x33 && addr[1] == 0x33), ESP_FAIL, err, TAG,
                      "IPv6 multicast is not blocked by W6300.");

    if (addr[0] == 0x01 && addr[1] == 0x00 && addr[2] == 0x5e && emac->mcast_cnt > 0) {
        emac->mcast_cnt--;
    }
    if (emac->mcast_cnt == 0) {
        ESP_GOTO_ON_ERROR(emac_w6300_set_block_ip4_mcast(mac, true), err, TAG, "set block multicast failed");
    }
err:
    return ret;
}

static esp_err_t emac_w6300_set_link(esp_eth_mac_t *mac, eth_link_t link)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    switch (link) {
    case ETH_LINK_UP:
        ESP_LOGD(TAG, "link is up");
        ESP_GOTO_ON_ERROR(mac->start(mac), err, TAG, "w6300 start failed");
        if (emac->poll_timer) {
            ESP_GOTO_ON_ERROR(esp_timer_start_periodic(emac->poll_timer, emac->poll_period_ms * 1000),
                              err, TAG, "start poll timer failed");
        }
        break;
    case ETH_LINK_DOWN:
        ESP_LOGD(TAG, "link is down");
        ESP_GOTO_ON_ERROR(mac->stop(mac), err, TAG, "w6300 stop failed");
        if (emac->poll_timer) {
            ESP_GOTO_ON_ERROR(esp_timer_stop(emac->poll_timer), err, TAG, "stop poll timer failed");
        }
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown link status");
        break;
    }

err:
    return ret;
}

static esp_err_t emac_w6300_set_speed(esp_eth_mac_t *mac, eth_speed_t speed)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    switch (speed) {
    case ETH_SPEED_10M:
        emac->tx_tmo = W6300_10M_TX_TMO_US;
        ESP_LOGD(TAG, "working in 10Mbps");
        break;
    case ETH_SPEED_100M:
        emac->tx_tmo = W6300_100M_TX_TMO_US;
        ESP_LOGD(TAG, "working in 100Mbps");
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown speed");
        break;
    }

err:
    return ret;
}

static esp_err_t emac_w6300_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex)
{
    esp_err_t ret = ESP_OK;
    switch (duplex) {
    case ETH_DUPLEX_HALF:
        ESP_LOGD(TAG, "working in half duplex");
        break;
    case ETH_DUPLEX_FULL:
        ESP_LOGD(TAG, "working in full duplex");
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "unknown duplex");
        break;
    }

err:
    return ret;
}

static esp_err_t emac_w6300_set_promiscuous(esp_eth_mac_t *mac, bool enable)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    uint8_t smr = 0;
    ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SOCK_MR(W6300_MACRAW_SOCK), &smr, sizeof(smr)),
                      err, TAG, "read Sn_MR failed");
    if (enable) {
        smr &= ~W6300_SMR_MAC_FILTER;
    } else {
        smr |= W6300_SMR_MAC_FILTER;
    }
    ESP_GOTO_ON_ERROR(w6300_write_u8(emac, W6300_REG_SOCK_MR(W6300_MACRAW_SOCK), smr),
                      err, TAG, "write Sn_MR failed");

err:
    return ret;
}

static esp_err_t emac_w6300_set_all_multicast(esp_eth_mac_t *mac, bool enable)
{
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    ESP_RETURN_ON_ERROR(emac_w6300_set_block_ip4_mcast(mac, !enable), TAG, "set block multicast failed");
    emac->mcast_cnt = 0;
    if (enable) {
        ESP_LOGW(TAG, "W6300 filters in IP multicast frames only!");
    }
    return ESP_OK;
}

static esp_err_t emac_w6300_enable_flow_ctrl(esp_eth_mac_t *mac, bool enable)
{
    /* w6300 doesn't support flow control function, so accept any value */
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t emac_w6300_set_peer_pause_ability(esp_eth_mac_t *mac, uint32_t ability)
{
    /* w6300 doesn't support PAUSE function, so accept any value */
    return ESP_ERR_NOT_SUPPORTED;
}

static inline bool is_w6300_sane_for_rxtx(emac_w6300_t *emac)
{
    uint8_t physr;
    /* ok for rx and tx as long as the link is up */
    if (w6300_read(emac, W6300_REG_PHYSR, &physr, sizeof(physr)) == ESP_OK && (physr & W6300_PHYSR_LNK)) {
        return true;
    }
    return false;
}

static esp_err_t emac_w6300_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    uint16_t offset = 0;

    ESP_GOTO_ON_FALSE(length <= ETH_MAX_PACKET_SIZE, ESP_ERR_INVALID_ARG, err,
                      TAG, "frame size is too big (actual %" PRIu32 ", maximum %u)", length, ETH_MAX_PACKET_SIZE);
    // check if there're free memory to store this packet
    uint16_t free_size = 0;
    ESP_GOTO_ON_ERROR(w6300_get_tx_free_size(emac, &free_size), err, TAG, "get free size failed");
    ESP_GOTO_ON_FALSE(length <= free_size, ESP_ERR_NO_MEM, err, TAG,
                      "free size (%" PRIu16 ") < send length (%" PRIu32 ")", free_size, length);
    // get current write pointer
    ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_TX_WR(W6300_MACRAW_SOCK), &offset),
                      err, TAG, "read TX WR failed");
    // copy data to tx memory (the chip wraps the pointer inside the socket buffer)
    ESP_GOTO_ON_ERROR(w6300_write_buffer(emac, buf, length, offset), err, TAG, "write frame failed");
    // update write pointer
    offset += length;
    ESP_GOTO_ON_ERROR(w6300_write_u16(emac, W6300_REG_SOCK_TX_WR(W6300_MACRAW_SOCK), offset),
                      err, TAG, "write TX WR failed");
    // issue SEND command
    ESP_GOTO_ON_ERROR(w6300_send_command(emac, W6300_SCR_SEND, 100), err, TAG, "issue SEND command failed");

    // polling the TX done event
    uint8_t status = 0;
    uint64_t start = esp_timer_get_time();
    uint64_t now = 0;
    do {
        now = esp_timer_get_time();
        if (!is_w6300_sane_for_rxtx(emac) || (now - start) > emac->tx_tmo) {
            return ESP_FAIL;
        }
        ESP_GOTO_ON_ERROR(w6300_read(emac, W6300_REG_SOCK_IR(W6300_MACRAW_SOCK), &status, sizeof(status)),
                          err, TAG, "read Sn_IR failed");
    } while (!(status & W6300_SIR_SENDOK));
    // acknowledge the event (Sn_IR is read-only -> write the bit to Sn_IRCLR)
    ESP_GOTO_ON_ERROR(w6300_clear_sock_intr(emac, W6300_SIR_SENDOK), err, TAG, "clear Sn_IR failed");

err:
    return ret;
}

/* Read the 2-byte PACKET-INFO header at `offset` and return the payload length.
 * Layout: byte0 bits[7:3] = flags, bits[2:0] = length high bits; byte1 = length
 * low. The length counts the 2 header bytes, hence the -2. */
static esp_err_t w6300_read_frame_header(emac_w6300_t *emac, uint16_t offset, uint16_t *payload_len)
{
    esp_err_t ret = ESP_OK;
    uint8_t head[W6300_RX_HEADER_SIZE] = {0};
    uint16_t total;

    ESP_GOTO_ON_ERROR(w6300_read_buffer(emac, head, sizeof(head), offset), err, TAG, "read frame header failed");
    total = (uint16_t)(((head[0] & W6300_RX_HEADER_LEN_MASK) << 8) | head[1]);
    ESP_GOTO_ON_FALSE(total >= W6300_RX_HEADER_SIZE, ESP_ERR_INVALID_SIZE, err, TAG,
                      "invalid frame header 0x%02" PRIx8 "%02" PRIx8, head[0], head[1]);
    *payload_len = total - W6300_RX_HEADER_SIZE;

err:
    return ret;
}

static esp_err_t emac_w6300_alloc_recv_buf(emac_w6300_t *emac, uint8_t **buf, uint32_t *length)
{
    esp_err_t ret = ESP_OK;
    uint16_t offset = 0;
    uint16_t rx_len = 0;
    uint32_t copy_len = 0;
    uint16_t remain_bytes = 0;
    *buf = NULL;

    w6300_get_rx_received_size(emac, &remain_bytes);
    if (remain_bytes) {
        // get current read pointer
        ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_RX_RD(W6300_MACRAW_SOCK), &offset),
                          err, TAG, "read RX RD failed");
        // read head
        ESP_GOTO_ON_ERROR(w6300_read_frame_header(emac, offset, &rx_len), err, TAG, "read frame header failed");
        // frames larger than expected will be truncated
        copy_len = rx_len > *length ? *length : rx_len;
        // runt frames are dropped by the chip, but the length could still be corrupted on the bus
        ESP_GOTO_ON_FALSE(copy_len >= ETH_MIN_PACKET_SIZE - ETH_CRC_LEN, ESP_ERR_INVALID_SIZE, err, TAG,
                          "invalid frame length %" PRIu32, copy_len);
        *buf = malloc(copy_len);
        if (*buf != NULL) {
            emac_w6300_auto_buf_info_t *buff_info = (emac_w6300_auto_buf_info_t *)*buf;
            buff_info->offset = offset;
            buff_info->copy_len = copy_len;
            buff_info->rx_len = rx_len;
            buff_info->remain = remain_bytes;
        } else {
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
    }
err:
    *length = rx_len;
    return ret;
}

static esp_err_t emac_w6300_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    uint16_t offset = 0;
    uint16_t rx_len = 0;
    uint16_t copy_len = 0;
    uint16_t remain_bytes = 0;
    emac->packets_remain = false;

    if (*length != W6300_ETH_MAC_RX_BUF_SIZE_AUTO) {
        w6300_get_rx_received_size(emac, &remain_bytes);
        if (remain_bytes) {
            // get current read pointer
            ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_RX_RD(W6300_MACRAW_SOCK), &offset),
                              err, TAG, "read RX RD failed");
            // read head first
            ESP_GOTO_ON_ERROR(w6300_read_frame_header(emac, offset, &rx_len), err, TAG, "read frame header failed");
            // frames larger than expected will be truncated
            copy_len = rx_len > *length ? *length : rx_len;
        } else {
            // silently return when no frame is waiting
            goto err;
        }
    } else {
        emac_w6300_auto_buf_info_t *buff_info = (emac_w6300_auto_buf_info_t *)buf;
        offset = buff_info->offset;
        copy_len = buff_info->copy_len;
        rx_len = buff_info->rx_len;
        remain_bytes = buff_info->remain;
    }
    // skip the 2 header bytes
    offset += W6300_RX_HEADER_SIZE;
    // read the payload
    ESP_GOTO_ON_ERROR(w6300_read_buffer(emac, emac->rx_buffer, copy_len, offset), err, TAG,
                      "read payload failed, len=%" PRIu16 ", offset=%" PRIu16, rx_len, offset);
    memcpy(buf, emac->rx_buffer, copy_len);
    offset += rx_len;
    // update read pointer and commit
    ESP_GOTO_ON_ERROR(w6300_write_u16(emac, W6300_REG_SOCK_RX_RD(W6300_MACRAW_SOCK), offset),
                      err, TAG, "write RX RD failed");
    /* issue RECV command */
    ESP_GOTO_ON_ERROR(w6300_send_command(emac, W6300_SCR_RECV, 100), err, TAG, "issue RECV command failed");
    // check if there're more data need to process
    remain_bytes -= rx_len + W6300_RX_HEADER_SIZE;
    emac->packets_remain = remain_bytes > 0;

    *length = copy_len;
    return ret;
err:
    *length = 0;
    return ret;
}

static esp_err_t emac_w6300_flush_recv_frame(emac_w6300_t *emac)
{
    esp_err_t ret = ESP_OK;
    uint16_t offset = 0;
    uint16_t rx_len = 0;
    uint16_t remain_bytes = 0;
    emac->packets_remain = false;

    w6300_get_rx_received_size(emac, &remain_bytes);
    if (remain_bytes) {
        // get current read pointer
        ESP_GOTO_ON_ERROR(w6300_read_u16(emac, W6300_REG_SOCK_RX_RD(W6300_MACRAW_SOCK), &offset),
                          err, TAG, "read RX RD failed");
        // read head first
        ESP_GOTO_ON_ERROR(w6300_read_frame_header(emac, offset, &rx_len), err, TAG, "read frame header failed");
        // drop the whole frame: header + payload
        offset += rx_len + W6300_RX_HEADER_SIZE;
        ESP_GOTO_ON_ERROR(w6300_write_u16(emac, W6300_REG_SOCK_RX_RD(W6300_MACRAW_SOCK), offset),
                          err, TAG, "write RX RD failed");
        /* issue RECV command */
        ESP_GOTO_ON_ERROR(w6300_send_command(emac, W6300_SCR_RECV, 100), err, TAG, "issue RECV command failed");
        // check if there're more data need to process
        remain_bytes -= rx_len + W6300_RX_HEADER_SIZE;
        emac->packets_remain = remain_bytes > 0;
    }
err:
    return ret;
}

IRAM_ATTR static void w6300_isr_handler(void *arg)
{
    emac_w6300_t *emac = (emac_w6300_t *)arg;
    BaseType_t high_task_wakeup = pdFALSE;
    /* notify w6300 task */
    vTaskNotifyGiveFromISR(emac->rx_task_hdl, &high_task_wakeup);
    if (high_task_wakeup != pdFALSE) {
        portYIELD_FROM_ISR();
    }
}

static void w6300_poll_timer(void *arg)
{
    emac_w6300_t *emac = (emac_w6300_t *)arg;
    xTaskNotifyGive(emac->rx_task_hdl);
}

static void emac_w6300_task(void *arg)
{
    emac_w6300_t *emac = (emac_w6300_t *)arg;
    uint8_t status = 0;
    uint8_t *buffer = NULL;
    uint32_t frame_len = 0;
    uint32_t buf_len = 0;
    esp_err_t ret;
    while (1) {
        /* check if the task receives any notification */
        if (emac->int_gpio_num >= 0) {                                   // if in interrupt mode
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0 &&    // if no notification ...
                    gpio_get_level(emac->int_gpio_num) != 0) {           // ...and no interrupt asserted
                continue;                                                // -> just continue to check again
            }
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        /* read interrupt status */
        w6300_read(emac, W6300_REG_SOCK_IR(W6300_MACRAW_SOCK), &status, sizeof(status));
        /* packet received */
        if (status & W6300_SIR_RECV) {
            /* clear interrupt status (write-1-to-clear on the separate Sn_IRCLR) */
            w6300_clear_sock_intr(emac, W6300_SIR_RECV);
            do {
                /* define max expected frame len */
                frame_len = ETH_MAX_PACKET_SIZE;
                if ((ret = emac_w6300_alloc_recv_buf(emac, &buffer, &frame_len)) == ESP_OK) {
                    if (buffer != NULL) {
                        /* we have memory to receive the frame of maximal size previously defined */
                        buf_len = W6300_ETH_MAC_RX_BUF_SIZE_AUTO;
                        if (emac->parent.receive(&emac->parent, buffer, &buf_len) == ESP_OK) {
                            if (buf_len == 0) {
                                free(buffer);
                            } else if (frame_len > buf_len) {
                                ESP_LOGE(TAG, "received frame was truncated");
                                free(buffer);
                            } else {
                                ESP_LOGD(TAG, "receive len=%" PRIu32, buf_len);
                                /* pass the buffer to stack (e.g. TCP/IP layer) */
                                emac->eth->stack_input(emac->eth, buffer, buf_len);
                            }
                        } else {
                            ESP_LOGE(TAG, "frame read from module failed");
                            free(buffer);
                        }
                    } else if (frame_len) {
                        ESP_LOGE(TAG, "invalid combination of frame_len(%" PRIu32 ") and buffer pointer(%p)", frame_len, buffer);
                    }
                } else if (ret == ESP_ERR_NO_MEM) {
                    ESP_LOGE(TAG, "no mem for receive buffer");
                    emac_w6300_flush_recv_frame(emac);
                } else {
                    /* Drop the offending frame instead of retrying it: a corrupt
                     * PACKET-INFO header would otherwise be re-read forever,
                     * because nothing else advances Sn_RX_RD. */
                    ESP_LOGE(TAG, "unexpected error 0x%x, dropping frame", ret);
                    emac_w6300_flush_recv_frame(emac);
                }
            } while (emac->packets_remain);
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t emac_w6300_init(esp_eth_mac_t *mac)
{
    esp_err_t ret = ESP_OK;
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    esp_eth_mediator_t *eth = emac->eth;
    if (emac->int_gpio_num >= 0) {
        gpio_func_sel(emac->int_gpio_num, PIN_FUNC_GPIO);
        gpio_input_enable(emac->int_gpio_num);
        gpio_pullup_en(emac->int_gpio_num);
        gpio_set_intr_type(emac->int_gpio_num, GPIO_INTR_NEGEDGE); // active low
        gpio_intr_enable(emac->int_gpio_num);
        gpio_isr_handler_add(emac->int_gpio_num, w6300_isr_handler, emac);
    }
    /* triggers the PHY driver's hardware reset (RSTn pin), which also puts the
     * chip's lock registers back to their locked default -> our soft reset and
     * w6300_unlock_all() below must come after it */
    ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_LLINIT, NULL), err, TAG, "lowlevel init failed");
    /* reset w6300 (also verifies the chip ID and unlocks the register groups) */
    ESP_GOTO_ON_ERROR(w6300_reset(emac), err, TAG, "reset w6300 failed");
    /* default setup of internal registers */
    ESP_GOTO_ON_ERROR(w6300_setup_default(emac), err, TAG, "w6300 default setup failed");
    return ESP_OK;
err:
    if (emac->int_gpio_num >= 0) {
        gpio_isr_handler_remove(emac->int_gpio_num);
        gpio_reset_pin(emac->int_gpio_num);
    }
    eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
    return ret;
}

static esp_err_t emac_w6300_deinit(esp_eth_mac_t *mac)
{
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    esp_eth_mediator_t *eth = emac->eth;
    mac->stop(mac);
    if (emac->int_gpio_num >= 0) {
        gpio_isr_handler_remove(emac->int_gpio_num);
        gpio_reset_pin(emac->int_gpio_num);
    }
    if (emac->poll_timer && esp_timer_is_active(emac->poll_timer)) {
        esp_timer_stop(emac->poll_timer);
    }
    eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
    return ESP_OK;
}

static esp_err_t emac_w6300_del(esp_eth_mac_t *mac)
{
    emac_w6300_t *emac = __containerof(mac, emac_w6300_t, parent);
    if (emac->poll_timer) {
        esp_timer_delete(emac->poll_timer);
    }
    vTaskDelete(emac->rx_task_hdl);
    emac->spi.deinit(emac->spi.ctx);
    heap_caps_free(emac->rx_buffer);
    free(emac);
    return ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_w6300(const eth_w6300_config_t *w6300_config, const eth_mac_config_t *mac_config)
{
    esp_eth_mac_t *ret = NULL;
    emac_w6300_t *emac = NULL;
    ESP_GOTO_ON_FALSE(w6300_config && mac_config, NULL, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE((w6300_config->int_gpio_num >= 0) != (w6300_config->poll_period_ms > 0), NULL, err, TAG,
                      "invalid configuration argument combination");
    emac = calloc(1, sizeof(emac_w6300_t));
    ESP_GOTO_ON_FALSE(emac, NULL, err, TAG, "no mem for MAC instance");
    /* bind methods and attributes */
    emac->sw_reset_timeout_ms = mac_config->sw_reset_timeout_ms;
    emac->int_gpio_num = w6300_config->int_gpio_num;
    emac->poll_period_ms = w6300_config->poll_period_ms;
    emac->tx_tmo = W6300_100M_TX_TMO_US;
    emac->parent.set_mediator = emac_w6300_set_mediator;
    emac->parent.init = emac_w6300_init;
    emac->parent.deinit = emac_w6300_deinit;
    emac->parent.start = emac_w6300_start;
    emac->parent.stop = emac_w6300_stop;
    emac->parent.del = emac_w6300_del;
    emac->parent.write_phy_reg = emac_w6300_write_phy_reg;
    emac->parent.read_phy_reg = emac_w6300_read_phy_reg;
    emac->parent.set_addr = emac_w6300_set_addr;
    emac->parent.get_addr = emac_w6300_get_addr;
    emac->parent.add_mac_filter = emac_w6300_add_mac_filter;
    emac->parent.rm_mac_filter = emac_w6300_del_mac_filter;
    emac->parent.set_speed = emac_w6300_set_speed;
    emac->parent.set_duplex = emac_w6300_set_duplex;
    emac->parent.set_link = emac_w6300_set_link;
    emac->parent.set_promiscuous = emac_w6300_set_promiscuous;
    emac->parent.set_all_multicast = emac_w6300_set_all_multicast;
    emac->parent.set_peer_pause_ability = emac_w6300_set_peer_pause_ability;
    emac->parent.enable_flow_ctrl = emac_w6300_enable_flow_ctrl;
    emac->parent.transmit = emac_w6300_transmit;
    emac->parent.receive = emac_w6300_receive;

    if (w6300_config->custom_spi_driver.init != NULL && w6300_config->custom_spi_driver.deinit != NULL
            && w6300_config->custom_spi_driver.read != NULL && w6300_config->custom_spi_driver.write != NULL) {
        ESP_LOGD(TAG, "Using user's custom SPI Driver");
        emac->spi.init = w6300_config->custom_spi_driver.init;
        emac->spi.deinit = w6300_config->custom_spi_driver.deinit;
        emac->spi.read = w6300_config->custom_spi_driver.read;
        emac->spi.write = w6300_config->custom_spi_driver.write;
        /* Custom SPI driver device init */
        ESP_GOTO_ON_FALSE((emac->spi.ctx = emac->spi.init(w6300_config->custom_spi_driver.config)) != NULL, NULL,
                          err, TAG, "SPI initialization failed");
    } else {
        ESP_LOGD(TAG, "Using default QSPI Driver");
        emac->spi.init = w6300_spi_init;
        emac->spi.deinit = w6300_spi_deinit;
        emac->spi.read = w6300_spi_read;
        emac->spi.write = w6300_spi_write;
        /* SPI device init */
        ESP_GOTO_ON_FALSE((emac->spi.ctx = emac->spi.init(w6300_config)) != NULL, NULL,
                          err, TAG, "SPI initialization failed");
    }

    /* create w6300 task */
    BaseType_t core_num = tskNO_AFFINITY;
    if (mac_config->flags & ETH_MAC_FLAG_PIN_TO_CORE) {
        core_num = esp_cpu_get_core_id();
    }
    BaseType_t xReturned = xTaskCreatePinnedToCore(emac_w6300_task, "w6300_tsk", mac_config->rx_task_stack_size, emac,
                                                   mac_config->rx_task_prio, &emac->rx_task_hdl, core_num);
    ESP_GOTO_ON_FALSE(xReturned == pdPASS, NULL, err, TAG, "create w6300 task failed");

    emac->rx_buffer = heap_caps_malloc(ETH_MAX_PACKET_SIZE, MALLOC_CAP_DMA);
    ESP_GOTO_ON_FALSE(emac->rx_buffer, NULL, err, TAG, "RX buffer allocation failed");

    if (emac->int_gpio_num < 0) {
        const esp_timer_create_args_t poll_timer_args = {
            .callback = w6300_poll_timer,
            .name = "emac_spi_poll_timer",
            .arg = emac,
            .skip_unhandled_events = true
        };
        ESP_GOTO_ON_FALSE(esp_timer_create(&poll_timer_args, &emac->poll_timer) == ESP_OK, NULL,
                          err, TAG, "create poll timer failed");
    }

    return &(emac->parent);

err:
    if (emac) {
        if (emac->poll_timer) {
            esp_timer_delete(emac->poll_timer);
        }
        if (emac->rx_task_hdl) {
            vTaskDelete(emac->rx_task_hdl);
        }
        if (emac->spi.ctx) {
            emac->spi.deinit(emac->spi.ctx);
        }
        heap_caps_free(emac->rx_buffer);
        free(emac);
    }
    return ret;
}
