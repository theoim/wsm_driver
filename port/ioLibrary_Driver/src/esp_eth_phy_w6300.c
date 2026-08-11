/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * esp_eth PHY driver for the W6300 internal PHY. Same shape as the vendored
 * esp_eth_phy_w5500.c, but the register model is different:
 *
 *   - The W5500 exposes its whole PHY as one PHYCFGR (status and control mixed,
 *     reset active-low, opmode field). The W6300 splits it into PHYSR
 *     (read-only status), PHYCR0 (operation mode) and PHYCR1 (reset / power
 *     down), and PHYCR0/1 are lock-protected — the MAC unlocks them during its
 *     init, which runs before phy->init().
 *   - PHYSR's speed and duplex bits are INVERTED with respect to the W5500: a
 *     set bit means 10 Mbps / half duplex.
 *   - A PHY reset is started by writing PHYCR1.RST = 1 (self-clearing) and needs
 *     ~60.3 ms to stabilise, instead of the W5500's write-0-then-1 sequence.
 */
#include <string.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include <inttypes.h>
#include "esp_eth_phy.h"
#include "esp_eth_phy_w6300.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/io_mux_reg.h"
#include "esp_rom_sys.h"
#include "w6300_eth.h"

/* RSTn assert width and the settling time the chip needs afterwards. Matches
 * what the TOE transport (wsm_driver_spi_reset) uses on this hardware. */
#define W6300_HW_RESET_ASSERT_MS (2)
#define W6300_HW_RESET_SETTLE_MS (150)

static const char *TAG = "w6300.phy";

typedef struct {
    esp_eth_phy_t parent;
    esp_eth_mediator_t *eth;
    int addr;
    uint32_t reset_timeout_ms;
    uint32_t autonego_timeout_ms;
    eth_link_t link_status;
    int reset_gpio_num;
} phy_w6300_t;

static esp_err_t w6300_phy_read(phy_w6300_t *w6300, uint32_t reg, uint8_t *value)
{
    esp_err_t ret = ESP_OK;
    uint32_t v = 0;

    ESP_GOTO_ON_ERROR(w6300->eth->phy_reg_read(w6300->eth, w6300->addr, reg, &v), err, TAG,
                      "read PHY register 0x%04" PRIx32 " failed", reg);
    *value = (uint8_t)v;
err:
    return ret;
}

static esp_err_t w6300_phy_write(phy_w6300_t *w6300, uint32_t reg, uint8_t value)
{
    return w6300->eth->phy_reg_write(w6300->eth, w6300->addr, reg, value);
}

/* PHYCR0 mode value for a (speed, duplex) pair. */
static uint8_t w6300_phycr0_fixed_mode(eth_speed_t speed, eth_duplex_t duplex)
{
    if (speed == ETH_SPEED_100M) {
        return (duplex == ETH_DUPLEX_FULL) ? W6300_PHYCR0_100F : W6300_PHYCR0_100H;
    }
    return (duplex == ETH_DUPLEX_FULL) ? W6300_PHYCR0_10F : W6300_PHYCR0_10H;
}

/* Start a PHY reset through PHYCR1.RST. The bit self-clears; the PHY needs
 * ~60.3 ms afterwards before its status is meaningful. */
static esp_err_t w6300_phy_restart(phy_w6300_t *w6300)
{
    esp_err_t ret = ESP_OK;
    uint8_t phycr1 = 0;

    ESP_GOTO_ON_ERROR(w6300_phy_read(w6300, W6300_REG_PHYCR1, &phycr1), err, TAG, "read PHYCR1 failed");
    ESP_GOTO_ON_ERROR(w6300_phy_write(w6300, W6300_REG_PHYCR1, phycr1 | W6300_PHYCR1_RST), err, TAG,
                      "write PHYCR1 failed");
    vTaskDelay(pdMS_TO_TICKS(W6300_PHY_RESET_STABILIZE_MS));
err:
    return ret;
}

static esp_err_t w6300_update_link_duplex_speed(phy_w6300_t *w6300)
{
    esp_err_t ret = ESP_OK;
    esp_eth_mediator_t *eth = w6300->eth;
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    uint8_t physr = 0;

    ESP_GOTO_ON_ERROR(w6300_phy_read(w6300, W6300_REG_PHYSR, &physr), err, TAG, "read PHYSR failed");
    eth_link_t link = (physr & W6300_PHYSR_LNK) ? ETH_LINK_UP : ETH_LINK_DOWN;
    /* check if link status changed */
    if (w6300->link_status != link) {
        /* when link up, read negotiation result */
        if (link == ETH_LINK_UP) {
            /* inverted vs. W5500: set bit == 10Mbps / half duplex */
            speed = (physr & W6300_PHYSR_SPD) ? ETH_SPEED_10M : ETH_SPEED_100M;
            duplex = (physr & W6300_PHYSR_DPX) ? ETH_DUPLEX_HALF : ETH_DUPLEX_FULL;
            ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_SPEED, (void *)speed), err, TAG, "change speed failed");
            ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_DUPLEX, (void *)duplex), err, TAG, "change duplex failed");
        }
        ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_LINK, (void *)link), err, TAG, "change link failed");
        w6300->link_status = link;
    }
    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_set_mediator(esp_eth_phy_t *phy, esp_eth_mediator_t *eth)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(eth, ESP_ERR_INVALID_ARG, err, TAG, "mediator can't be null");
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    w6300->eth = eth;
    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_get_link(esp_eth_phy_t *phy)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    /* Update information about link, speed, duplex */
    ESP_GOTO_ON_ERROR(w6300_update_link_duplex_speed(w6300), err, TAG, "update link duplex speed failed");
    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_set_link(esp_eth_phy_t *phy, eth_link_t link)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    esp_eth_mediator_t *eth = w6300->eth;

    if (w6300->link_status != link) {
        w6300->link_status = link;
        // link status changed, immediately report to upper layers
        ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_LINK, (void *)w6300->link_status), err, TAG,
                          "change link failed");
    }
err:
    return ret;
}

static esp_err_t w6300_reset(esp_eth_phy_t *phy)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    w6300->link_status = ETH_LINK_DOWN;

    ESP_GOTO_ON_ERROR(w6300_phy_restart(w6300), err, TAG, "phy reset failed");
    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_reset_hw(esp_eth_phy_t *phy)
{
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    // set reset_gpio_num to a negative value can skip hardware reset phy chip
    if (w6300->reset_gpio_num >= 0) {
        gpio_func_sel(w6300->reset_gpio_num, PIN_FUNC_GPIO);
        gpio_set_level(w6300->reset_gpio_num, 0);
        gpio_output_enable(w6300->reset_gpio_num);
        vTaskDelay(pdMS_TO_TICKS(W6300_HW_RESET_ASSERT_MS));
        gpio_set_level(w6300->reset_gpio_num, 1);
        /* the chip is unreachable until its internal PLL has locked */
        vTaskDelay(pdMS_TO_TICKS(W6300_HW_RESET_SETTLE_MS));
    }
    return ESP_OK;
}

static esp_err_t w6300_autonego_ctrl(esp_eth_phy_t *phy, eth_phy_autoneg_cmd_t cmd, bool *autonego_en_stat)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    uint8_t phycr0 = 0;
    uint8_t physr = 0;

    /* PHYCR0 is write-only (datasheet 4.1.24), so the configured operation mode
     * must be read back from PHYSR[5:3] — MODE2 clear means auto negotiation.
     * ioLibrary reads it the same way in wizphy_getphystat() and deliberately
     * provides no getPHYCR0(). */
    ESP_GOTO_ON_ERROR(w6300_phy_read(w6300, W6300_REG_PHYSR, &physr), err, TAG, "read PHYSR failed");

    switch (cmd) {
    case ESP_ETH_PHY_AUTONEGO_RESTART:
        ESP_GOTO_ON_FALSE(!(physr & W6300_PHYSR_MODE2), ESP_ERR_INVALID_STATE, err, TAG,
                          "auto negotiation is disabled");
        /* in case any link status has changed, let's assume we're in link down status */
        w6300->link_status = ETH_LINK_DOWN;
        /* a PHY reset restarts the negotiation */
        ESP_GOTO_ON_ERROR(w6300_phy_restart(w6300), err, TAG, "phy reset failed");
        *autonego_en_stat = true;
        break;
    case ESP_ETH_PHY_AUTONEGO_DIS:
        /* W6300 has no separate autoneg-disable bit either: pin the operation
         * mode to what is currently negotiated (PHYSR polarity is inverted) */
        phycr0 = w6300_phycr0_fixed_mode((physr & W6300_PHYSR_SPD) ? ETH_SPEED_10M : ETH_SPEED_100M,
                                         (physr & W6300_PHYSR_DPX) ? ETH_DUPLEX_HALF : ETH_DUPLEX_FULL);
        w6300->link_status = ETH_LINK_DOWN;
        ESP_GOTO_ON_ERROR(w6300_phy_write(w6300, W6300_REG_PHYCR0, phycr0), err, TAG, "write PHYCR0 failed");
        ESP_GOTO_ON_ERROR(w6300_phy_restart(w6300), err, TAG, "phy reset failed");
        *autonego_en_stat = false;
        break;
    case ESP_ETH_PHY_AUTONEGO_EN:
        w6300->link_status = ETH_LINK_DOWN;
        ESP_GOTO_ON_ERROR(w6300_phy_write(w6300, W6300_REG_PHYCR0, W6300_PHYCR0_AUTO), err, TAG,
                          "write PHYCR0 failed");
        ESP_GOTO_ON_ERROR(w6300_phy_restart(w6300), err, TAG, "phy reset failed");
        *autonego_en_stat = true;
        break;
    case ESP_ETH_PHY_AUTONEGO_G_STAT:
        *autonego_en_stat = !(physr & W6300_PHYSR_MODE2);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_pwrctl(esp_eth_phy_t *phy, bool enable)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    uint8_t phycr1 = 0;

    ESP_GOTO_ON_ERROR(w6300_phy_read(w6300, W6300_REG_PHYCR1, &phycr1), err, TAG, "read PHYCR1 failed");
    if (enable) {
        phycr1 &= ~W6300_PHYCR1_PWDN;
    } else {
        phycr1 |= W6300_PHYCR1_PWDN;
    }
    /* never write RST back as 1 here — that would trigger a reset */
    phycr1 &= ~W6300_PHYCR1_RST;
    ESP_GOTO_ON_ERROR(w6300_phy_write(w6300, W6300_REG_PHYCR1, phycr1), err, TAG, "write PHYCR1 failed");
err:
    return ret;
}

static esp_err_t w6300_set_addr(esp_eth_phy_t *phy, uint32_t addr)
{
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    w6300->addr = addr;
    return ESP_OK;
}

static esp_err_t w6300_get_addr(esp_eth_phy_t *phy, uint32_t *addr)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_INVALID_ARG, err, TAG, "addr can't be null");
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    *addr = w6300->addr;
    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_del(esp_eth_phy_t *phy)
{
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    free(w6300);
    return ESP_OK;
}

static esp_err_t w6300_advertise_pause_ability(esp_eth_phy_t *phy, uint32_t ability)
{
    // pause ability advertisement is not supported for W6300 internal PHY
    return ESP_OK;
}

static esp_err_t w6300_loopback(esp_eth_phy_t *phy, bool enable)
{
    // Loopback is not supported for W6300 internal PHY
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t w6300_set_speed(esp_eth_phy_t *phy, eth_speed_t speed)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    uint8_t physr = 0;

    /* Since the link is going to be reconfigured, consider it down to be status updated once the driver re-started */
    w6300->link_status = ETH_LINK_DOWN;

    ESP_GOTO_ON_ERROR(w6300_phy_read(w6300, W6300_REG_PHYSR, &physr), err, TAG, "read PHYSR failed");
    uint8_t phycr0 = w6300_phycr0_fixed_mode(speed,
                                             (physr & W6300_PHYSR_DPX) ? ETH_DUPLEX_HALF : ETH_DUPLEX_FULL);
    ESP_GOTO_ON_ERROR(w6300_phy_write(w6300, W6300_REG_PHYCR0, phycr0), err, TAG, "write PHYCR0 failed");
    ESP_GOTO_ON_ERROR(w6300_phy_restart(w6300), err, TAG, "phy reset failed");

err:
    return ret;
}

static esp_err_t w6300_set_duplex(esp_eth_phy_t *phy, eth_duplex_t duplex)
{
    esp_err_t ret = ESP_OK;
    phy_w6300_t *w6300 = __containerof(phy, phy_w6300_t, parent);
    uint8_t physr = 0;

    /* Since the link is going to be reconfigured, consider it down to be status updated once the driver re-started */
    w6300->link_status = ETH_LINK_DOWN;

    ESP_GOTO_ON_ERROR(w6300_phy_read(w6300, W6300_REG_PHYSR, &physr), err, TAG, "read PHYSR failed");
    uint8_t phycr0 = w6300_phycr0_fixed_mode((physr & W6300_PHYSR_SPD) ? ETH_SPEED_10M : ETH_SPEED_100M,
                                             duplex);
    ESP_GOTO_ON_ERROR(w6300_phy_write(w6300, W6300_REG_PHYCR0, phycr0), err, TAG, "write PHYCR0 failed");
    ESP_GOTO_ON_ERROR(w6300_phy_restart(w6300), err, TAG, "phy reset failed");

err:
    return ret;
}

static esp_err_t w6300_init(esp_eth_phy_t *phy)
{
    esp_err_t ret = ESP_OK;
    /* Power on Ethernet PHY */
    ESP_GOTO_ON_ERROR(w6300_pwrctl(phy, true), err, TAG, "power control failed");
    /* Reset Ethernet PHY */
    ESP_GOTO_ON_ERROR(w6300_reset(phy), err, TAG, "reset failed");
    return ESP_OK;
err:
    return ret;
}

static esp_err_t w6300_deinit(esp_eth_phy_t *phy)
{
    esp_err_t ret = ESP_OK;
    /* Power off Ethernet PHY */
    ESP_GOTO_ON_ERROR(w6300_pwrctl(phy, false), err, TAG, "power control failed");
    return ESP_OK;
err:
    return ret;
}

esp_eth_phy_t *esp_eth_phy_new_w6300(const eth_phy_config_t *config)
{
    esp_eth_phy_t *ret = NULL;
    ESP_GOTO_ON_FALSE(config, NULL, err, TAG, "invalid arguments");
    phy_w6300_t *w6300 = calloc(1, sizeof(phy_w6300_t));
    ESP_GOTO_ON_FALSE(w6300, NULL, err, TAG, "no mem for PHY instance");
    w6300->addr = config->phy_addr;
    w6300->reset_timeout_ms = config->reset_timeout_ms;
    w6300->reset_gpio_num = config->reset_gpio_num;
    w6300->link_status = ETH_LINK_DOWN;
    w6300->autonego_timeout_ms = config->autonego_timeout_ms;
    w6300->parent.reset = w6300_reset;
    w6300->parent.reset_hw = w6300_reset_hw;
    w6300->parent.init = w6300_init;
    w6300->parent.deinit = w6300_deinit;
    w6300->parent.set_mediator = w6300_set_mediator;
    w6300->parent.autonego_ctrl = w6300_autonego_ctrl;
    w6300->parent.get_link = w6300_get_link;
    w6300->parent.set_link = w6300_set_link;
    w6300->parent.pwrctl = w6300_pwrctl;
    w6300->parent.get_addr = w6300_get_addr;
    w6300->parent.set_addr = w6300_set_addr;
    w6300->parent.advertise_pause_ability = w6300_advertise_pause_ability;
    w6300->parent.loopback = w6300_loopback;
    w6300->parent.set_speed = w6300_set_speed;
    w6300->parent.set_duplex = w6300_set_duplex;
    w6300->parent.del = w6300_del;
    return &(w6300->parent);
err:
    return ret;
}
