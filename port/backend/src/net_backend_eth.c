/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * ETH backend (CONFIG_WSM_DRIVER_BACKEND_ETH) implementation of the
 * backend-neutral harness (net_backend.h): the WIZnet chip is a SPI Ethernet MAC
 * via ESP-IDF esp_eth (MACRAW), and the LwIP software stack runs on the ESP32-S3.
 *
 * Works with either chip; only the bus setup and the MAC/PHY constructors
 * differ, and both differences are isolated in this file:
 *   - W5500: standard full-duplex SPI, 3-byte VDM frame  -> esp_eth_*_w5500
 *   - W6300: half-duplex QSPI (opcode + address + dummy + data), 1-bit or 4-bit
 *            per CONFIG_WSM_DRIVER_QSPI_*                -> esp_eth_*_w6300
 *
 * Config conventions follow wsm_driver:
 *   - pins / SPI -> component Kconfig (CONFIG_WSM_DRIVER_*).
 *   - network    -> the caller's wiz_NetInfo (byte arrays), mirrored onto the
 *                   esp_netif as a static IPv4 identity + MAC + DNS.
 */
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#if defined(CONFIG_WSM_DRIVER_CHIP_W6300)
#include "esp_eth_mac_w6300.h"
#include "esp_eth_phy_w6300.h"
#else
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#endif
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "net_backend.h"   /* wiznet_net_init(const wiz_NetInfo*), wiz_NetInfo */

#if defined(CONFIG_WSM_DRIVER_CHIP_W6300)
static const char *TAG = "w6300_eth";
#else
static const char *TAG = "w5500_eth";
#endif

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t     *s_eth_netif  = NULL;
static volatile bool    s_eth_connected = false;

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        s_eth_connected = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet link down");
        s_eth_connected = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
}

/* Mirror the caller's wiz_NetInfo onto the esp_netif as a static identity. */
static void apply_net_info(const wiz_NetInfo *net_info)
{
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_eth_netif));

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = ESP_IP4TOADDR(net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
    ip.netmask.addr = ESP_IP4TOADDR(net_info->sn[0], net_info->sn[1], net_info->sn[2], net_info->sn[3]);
    ip.gw.addr      = ESP_IP4TOADDR(net_info->gw[0], net_info->gw[1], net_info->gw[2], net_info->gw[3]);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_eth_netif, &ip));

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr =
        ESP_IP4TOADDR(net_info->dns[0], net_info->dns[1], net_info->dns[2], net_info->dns[3]);
    if (dns.ip.u_addr.ip4.addr != 0) {
        ESP_ERROR_CHECK(esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns));
    }

    ESP_LOGI(TAG, "Static IP set: %u.%u.%u.%u",
             net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
}

void wiznet_net_init(const wiz_NetInfo *net_info)
{
    /* 1) TCP/IP stack + default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2) netif for Ethernet */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    /* 3) SPI bus + device — wiring from Kconfig (wsm_driver convention). */
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_WSM_DRIVER_PIN_MOSI,
        .miso_io_num = CONFIG_WSM_DRIVER_PIN_MISO,
        .sclk_io_num = CONFIG_WSM_DRIVER_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Must be -1, not left zero-initialised. spicommon_bus_initialize_io()
         * gates its octal (data4..data7) wiring on `flags & SPICOMMON_BUSFLAG_OCTAL`
         * rather than `(flags & OCTAL) == OCTAL`, and OCTAL is a superset of QUAD,
         * so the plain quad bus configured below enters that branch too. Left at 0
         * these read as GPIO0 -- a boot strapping pin -- and routing it to SPI
         * kills the chip before app_main() gets a log line out. The TOE transport
         * (esp_wiz_toe_spi.c) already carries this same fix. */
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
    };
#if defined(CONFIG_WSM_DRIVER_CHIP_W6300) && defined(CONFIG_WSM_DRIVER_QSPI_QUAD)
    /* Quad mode needs the two extra data lines declared on the bus. */
    buscfg.quadwp_io_num = CONFIG_WSM_DRIVER_PIN_IO2;   /* D2 */
    buscfg.quadhd_io_num = CONFIG_WSM_DRIVER_PIN_IO3;   /* D3 */
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
#endif
#if defined(CONFIG_WSM_DRIVER_CHIP_W6300)
    /* one MACRAW frame + the QSPI frame header */
    buscfg.max_transfer_sz = 2048;
#endif
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_WSM_DRIVER_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .mode = 0,                                       /* both chips = SPI mode 0 */
        .clock_speed_hz = CONFIG_WSM_DRIVER_SPI_CLOCK_HZ,   /* already in Hz */
        .queue_size = 20,
        .spics_io_num = CONFIG_WSM_DRIVER_PIN_CS,
#if defined(CONFIG_WSM_DRIVER_CHIP_W6300)
        /* The W6300 QSPI frame is phased (opcode / address / dummy / data), which
         * the SPI master can only emit half-duplex. The MAC rejects the device
         * outright if this flag is missing. command_bits / address_bits are left
         * at 0 so the MAC fills in its own 8 / 24. */
        .flags = SPI_DEVICE_HALFDUPLEX,
#endif
    };

    /* 4) MAC + PHY for the selected chip (interrupt-driven; INT pin from Kconfig). */
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = CONFIG_WSM_DRIVER_PIN_RST;

#if defined(CONFIG_WSM_DRIVER_CHIP_W6300)
    eth_w6300_config_t chip_cfg = ETH_W6300_DEFAULT_CONFIG(CONFIG_WSM_DRIVER_SPI_HOST, &devcfg);
    chip_cfg.int_gpio_num   = CONFIG_WSM_DRIVER_PIN_INT;
    chip_cfg.poll_period_ms = 0;

    esp_eth_mac_t *mac = esp_eth_mac_new_w6300(&chip_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w6300(&phy_cfg);
#else
    eth_w5500_config_t chip_cfg = ETH_W5500_DEFAULT_CONFIG(CONFIG_WSM_DRIVER_SPI_HOST, &devcfg);
    chip_cfg.int_gpio_num   = CONFIG_WSM_DRIVER_PIN_INT;
    chip_cfg.poll_period_ms = 0;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&chip_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
#endif
    ESP_ERROR_CHECK(mac != NULL && phy != NULL ? ESP_OK : ESP_FAIL);

    /* 5) GPIO ISR service, then install the driver. The MAC attaches its INT
     *    handler with gpio_isr_handler_add() during esp_eth_driver_install(), and
     *    that needs the shared GPIO ISR service — which in ESP-IDF the
     *    application installs, not the esp_eth SPI drivers. The TOE backend has
     *    no equivalent step because it leaves INT at GPIO_INTR_DISABLE and polls.
     *    ESP_ERR_INVALID_STATE only means another component got here first. */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth_handle));

    /* 6) Neither chip has a built-in MAC address — set it from wiz_NetInfo.
     *    On the W6300 this write reaches SHAR only because the MAC unlocked the
     *    network register group during init (NETLCKR), which is exactly the step
     *    the TOE path is missing. */
    uint8_t mac_addr[6];
    memcpy(mac_addr, net_info->mac, sizeof(mac_addr));
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    /* 7) attach driver to netif */
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

    /* 8) events + static identity from wiz_NetInfo */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));
    apply_net_info(net_info);

    /* 9) go */
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
}

bool wiznet_net_is_up(void)
{
    return s_eth_connected;
}
