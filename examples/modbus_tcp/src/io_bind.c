/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Coil -> pin, pin -> discrete input (see io_bind.h).
 */
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io_bind.h"
#include "mb_core.h"
#include "mb_store.h"

static const char *TAG = "io_bind";

/* How long a sync cycle waits for the data model. Shorter than the server's
 * timeout because a missed cycle costs nothing: the pin keeps its level and the
 * next cycle is 20 ms away. Blocking here to insist on one sample would be
 * holding up the input for the benefit of nobody. */
#define STORE_TIMEOUT_MS  20

/* Pin levels for "the load is on" and "the contact is closed". */
#define OUT_ACTIVE_LEVEL  (IO_OUT_ACTIVE_LOW ? 0 : 1)
#define IN_ACTIVE_LEVEL   (IO_IN_ACTIVE_LOW ? 0 : 1)

static mb_store_t *g_store;

static void drive_output(bool on)
{
    gpio_set_level(IO_OUT_PIN,
                   on ? OUT_ACTIVE_LEVEL : !OUT_ACTIVE_LEVEL);
}

/*
 * One cycle: sample the input, publish the output.
 *
 * The store is touched once and briefly -- one bool out, one bool in -- and the
 * GPIO write happens after the release. Holding the lock across gpio_set_level()
 * would work, but the rule this example follows everywhere else is that the lock
 * covers memory and nothing else, and a binding layer is exactly where that rule
 * starts to erode.
 */
static void sync_once(bool *out_state, bool in_state)
{
    mb_datastore_t *ds = mb_store_acquire(g_store, STORE_TIMEOUT_MS);
    if (ds == NULL) {
        return;
    }

    bool coil = ds->coil[IO_COIL_INDEX];
    ds->discrete[IO_INPUT_INDEX] = in_state;
    mb_store_release(g_store);

    if (coil != *out_state) {
        drive_output(coil);
        *out_state = coil;
        ESP_LOGI(TAG, "coil[%d] %s -> GPIO%d %s", IO_COIL_INDEX,
                 coil ? "ON" : "OFF", IO_OUT_PIN,
                 coil ? "active" : "inactive");
    }
}

static void io_task(void *arg)
{
    (void)arg;

    /* Seeded from the pin's present level rather than from a guess, so a
     * contact already closed at boot is reported as closed on the first poll
     * instead of after IO_DEBOUNCE_SAMPLES cycles of disagreement. */
    bool stable    = (gpio_get_level(IO_IN_PIN) == IN_ACTIVE_LEVEL);
    bool candidate = stable;
    int  agreed    = IO_DEBOUNCE_SAMPLES;
    bool out_state = false;   /* io_bind_start() drove the pin inactive */

    ESP_LOGI(TAG, "coil[%d] -> GPIO%d, GPIO%d -> discrete[%d] (%s)",
             IO_COIL_INDEX, IO_OUT_PIN, IO_IN_PIN, IO_INPUT_INDEX,
             stable ? "input closed" : "input open");

    for (;;) {
        bool raw = (gpio_get_level(IO_IN_PIN) == IN_ACTIVE_LEVEL);

        /* Count agreement rather than time since the last edge: a bouncing
         * contact produces both edges repeatedly, and a timer restarted on each
         * one can be held off indefinitely. A run of equal samples cannot. */
        if (raw != candidate) {
            candidate = raw;
            agreed = 1;
        } else if (agreed < IO_DEBOUNCE_SAMPLES) {
            agreed++;
        }

        if (agreed >= IO_DEBOUNCE_SAMPLES && candidate != stable) {
            stable = candidate;
            ESP_LOGI(TAG, "GPIO%d %s -> discrete[%d] %d", IO_IN_PIN,
                     stable ? "closed" : "open", IO_INPUT_INDEX, stable);
        }

        sync_once(&out_state, stable);
        vTaskDelay(pdMS_TO_TICKS(IO_POLL_MS));
    }
}

bool io_bind_start(mb_store_t *store)
{
    if (store == NULL) {
        return false;
    }
    g_store = store;

    /*
     * Output first, and driven before it is enabled.
     *
     * gpio_config() leaves the pin at whatever the level register already held,
     * so setting the level after enabling the driver would put a pulse of
     * unknown width on the load. Setting it first means the driver comes on
     * already holding the inactive level.
     */
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << IO_OUT_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    drive_output(false);
    if (gpio_config(&out) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d could not be configured as an output", IO_OUT_PIN);
        return false;
    }
    drive_output(false);

    /* The internal pull holds the input at its inactive level while the contact
     * is open. It is weak (~45 kOhm); a run of any length wants an external
     * resistor and, off-board, some protection in front of it. */
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << IO_IN_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = IO_IN_ACTIVE_LOW ? GPIO_PULLUP_ENABLE
                                         : GPIO_PULLUP_DISABLE,
        .pull_down_en = IO_IN_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE
                                         : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&in) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d could not be configured as an input", IO_IN_PIN);
        return false;
    }

    /*
     * Clear the seeded coil now that the pin is safe.
     *
     * mb_datastore_init() turns on every even coil as a pattern a master can
     * recognise, which is the right thing for 63 of them and the wrong thing
     * for the one wired to a relay. Nothing else is running yet, so the lock is
     * uncontended and a failure to take it means something is badly wrong.
     */
    mb_datastore_t *ds = mb_store_acquire(store, 100);
    if (ds == NULL) {
        ESP_LOGE(TAG, "data model unavailable; not binding I/O");
        return false;
    }
    ds->coil[IO_COIL_INDEX] = false;
    ds->discrete[IO_INPUT_INDEX] = (gpio_get_level(IO_IN_PIN) == IN_ACTIVE_LEVEL);
    mb_store_release(store);

    if (xTaskCreate(io_task, "io_bind", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not create the I/O task");
        return false;
    }
    return true;
}
