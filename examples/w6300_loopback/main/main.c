#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "wsm_driver.h"
#include "wsm_driver/Ethernet/socket.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wizchip_conf.h"
#include "loopback.h"
#include "Application/Application.h"

#define EXAMPLE_TCP_SOCKET_NUM 0
#define EXAMPLE_UDP_SOCKET_NUM 1
#define EXAMPLE_TCP_LISTEN_PORT 5000
#define EXAMPLE_UDP_LISTEN_PORT 5001
#define EXAMPLE_IO_TIMEOUT_MS 5000

#ifdef CONFIG_WSM_DRIVER_TX_BUF_KB
#define EXAMPLE_TX_BUF_KB CONFIG_WSM_DRIVER_TX_BUF_KB
#else
#define EXAMPLE_TX_BUF_KB 2
#endif

#ifdef CONFIG_WSM_DRIVER_RX_BUF_KB
#define EXAMPLE_RX_BUF_KB CONFIG_WSM_DRIVER_RX_BUF_KB
#else
#define EXAMPLE_RX_BUF_KB 2
#endif

static const char *TAG = "w6300_loopback_example";

// Keep large networking buffers out of task stack.
static wsm_driver_spi_config_t s_spi_cfg;
static uint8_t s_buf_size_tx[8];
static uint8_t s_buf_size_rx[8];
static uint8_t s_tcp_buf[DATA_BUF_SIZE];
static uint8_t s_udp_buf[DATA_BUF_SIZE];
static bool s_link_up = false;
static bool s_link_was_up = false;

static const wiz_NetInfo s_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip = {192, 168, 11, 2},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 11, 1},
    .dns = {8, 8, 8, 8},
    .ipmode = NETINFO_STATIC_ALL,
    .dhcp = NETINFO_STATIC,
};

static void fill_spi_config(wsm_driver_spi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->host_id = (spi_host_device_t)CONFIG_WSM_DRIVER_SPI_HOST;
    cfg->clock_hz = CONFIG_WSM_DRIVER_SPI_CLOCK_HZ;
    cfg->pin_miso = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_MISO;
    cfg->pin_mosi = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_MOSI;
    cfg->pin_sclk = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_SCLK;
    cfg->pin_cs = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_CS;
    cfg->pin_int = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_INT;
    cfg->pin_rst = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_RST;
#ifdef CONFIG_WSM_DRIVER_PIN_IO2
    cfg->pin_io2 = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_IO2;
#else
    cfg->pin_io2 = GPIO_NUM_NC;
#endif
#ifdef CONFIG_WSM_DRIVER_PIN_IO3
    cfg->pin_io3 = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_IO3;
#else
    cfg->pin_io3 = GPIO_NUM_NC;
#endif
    cfg->lock_timeout_ms = EXAMPLE_IO_TIMEOUT_MS;
}

static void w6300_loopback_task(void *arg)
{
    (void)arg;
    int32_t retval = 0;

    memset(s_buf_size_tx, EXAMPLE_TX_BUF_KB, sizeof(s_buf_size_tx));
    memset(s_buf_size_rx, EXAMPLE_RX_BUF_KB, sizeof(s_buf_size_rx));

    fill_spi_config(&s_spi_cfg);

    ESP_ERROR_CHECK(wsm_driver_spi_init(&s_spi_cfg));
    ESP_ERROR_CHECK(wsm_driver_spi_register_iolib_callbacks());
    ESP_ERROR_CHECK(wsm_driver_spi_reset());
    ESP_ERROR_CHECK(wsm_driver_spi_wizchip_check());

    if (wizchip_init(s_buf_size_tx, s_buf_size_rx) != 0) {
        ESP_LOGE(TAG, "wizchip_init failed");
        vTaskDelete(NULL);
        return;
    }

    wizchip_setnetinfo((wiz_NetInfo *)&s_net_info);
    set_loopback_mode_W6x00(AS_IPV4);

    ESP_LOGI(TAG, "TCP loopback on port %d, UDP loopback on port %d",
             EXAMPLE_TCP_LISTEN_PORT, EXAMPLE_UDP_LISTEN_PORT);

    while (true) {
        if (wsm_driver_spi_link_is_up(&s_link_up) != ESP_OK || !s_link_up) {
            s_link_was_up = false;
            ESP_LOGI(TAG, "PHY link down, waiting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_link_was_up) {
            ESP_LOGI(TAG, "PHY link up");
            s_link_was_up = true;
        }

        retval = loopback_tcps(EXAMPLE_TCP_SOCKET_NUM, s_tcp_buf, EXAMPLE_TCP_LISTEN_PORT);
        if (retval < 0) {
            ESP_LOGE(TAG, "loopback_tcps error: %d, reopening socket", (int)retval);
            close(EXAMPLE_TCP_SOCKET_NUM);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        retval = loopback_udps(EXAMPLE_UDP_SOCKET_NUM, s_udp_buf, EXAMPLE_UDP_LISTEN_PORT);
        if (retval < 0) {
            ESP_LOGE(TAG, "loopback_udps error: %d, reopening socket", (int)retval);
            close(EXAMPLE_UDP_SOCKET_NUM);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    // The loopback helpers open sockets in blocking mode, so this demo can
    // stay in long waits/retries; disable Task WDT to avoid resets during
    // bring-up and manual network testing.
    esp_task_wdt_delete(NULL);
    esp_task_wdt_deinit();

    xTaskCreate(w6300_loopback_task, "w6300_loopback_task", 8192, NULL, 5, NULL);
}
