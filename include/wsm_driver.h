#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_idf_version.h"

/* Minimum supported ESP-IDF is v6.0. The component manifest already declares
 * `idf >= 6.0.1`, but that only constrains registry installs — a manual clone or
 * submodule bypasses it, so fail loudly here instead of with a pile of unrelated
 * errors from esp_eth / mbedTLS. */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
#error "wsm_driver requires ESP-IDF v6.0 or later"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host_id;
    int clock_hz;
    gpio_num_t pin_miso;   /**< W6300 QSPI: IO1 */
    gpio_num_t pin_mosi;   /**< W6300 QSPI: IO0 */
    gpio_num_t pin_sclk;
    gpio_num_t pin_cs;
    gpio_num_t pin_rst;
    gpio_num_t pin_int;
    gpio_num_t pin_io2;    /**< W6300 quad mode only; ignored otherwise */
    gpio_num_t pin_io3;    /**< W6300 quad mode only; ignored otherwise */
    uint32_t lock_timeout_ms;
} wsm_driver_spi_config_t;

esp_err_t wsm_driver_spi_init(const wsm_driver_spi_config_t *cfg);
esp_err_t wsm_driver_spi_deinit(void);
esp_err_t wsm_driver_spi_register_iolib_callbacks(void);
esp_err_t wsm_driver_spi_reset(void);
esp_err_t wsm_driver_spi_wizchip_check(void);
esp_err_t wsm_driver_spi_link_is_up(bool *is_up);

#ifdef __cplusplus
}
#endif
