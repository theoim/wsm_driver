/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Camera access and statistics (see cam_source.h).
 *
 * The only file that includes esp_camera.h.
 */
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cam_pins.h"
#include "cam_source.h"

static const char *TAG = "cam";

/* One sensor shared by both server tasks. The mutex covers the borrow, not the
 * whole send: esp32-camera hands out a pointer into its own DMA buffer and
 * takes it back on return, so two tasks holding frames at once would have the
 * driver reusing a buffer that is still going out on the wire. */
static SemaphoreHandle_t s_lock;
static camera_fb_t      *s_fb;

static cam_res_t s_res     = CAM_RES_VGA;
static int       s_quality = 12;
static int       s_xclk    = 20;

static const struct {
    framesize_t framesize;
    const char *name;
} kRes[CAM_RES_COUNT] = {
    [CAM_RES_QVGA] = { FRAMESIZE_QVGA, "320x240"   },
    [CAM_RES_VGA]  = { FRAMESIZE_VGA,  "640x480"   },
    [CAM_RES_SVGA] = { FRAMESIZE_SVGA, "800x600"   },
    [CAM_RES_HD]   = { FRAMESIZE_HD,   "1280x720"  },
    [CAM_RES_UXGA] = { FRAMESIZE_UXGA, "1600x1200" },
};

static camera_config_t build_config(void)
{
    camera_config_t c = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,

        .xclk_freq_hz = s_xclk * 1000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        /* JPEG out of the sensor, not RGB. The ESP32-S3 could encode in
         * software, but the point here is to measure the network, and an
         * already-compressed frame is what a streaming device would send. */
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = kRes[s_res].framesize,
        .jpeg_quality = s_quality,

        /* Two buffers in PSRAM with a grab-latest policy: the sensor keeps
         * filling one while the other goes out on the wire, and when the link
         * cannot keep up the older frame is discarded rather than queued.
         * A queue would trade a growing delay for frames nobody wants -- on a
         * live view, late is the same as wrong. */
        .fb_count    = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode   = CAMERA_GRAB_LATEST,
    };
    return c;
}

int cam_source_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "no memory for the frame lock");
            return -1;
        }
    }

    camera_config_t c = build_config();
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        /* The OV3660 leaves the factory upside down on this module, and the
         * defaults are washed out at the low JPEG quality this example uses. */
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -1);
        ESP_LOGI(TAG, "sensor PID 0x%04X up at %s, quality %d, xclk %d MHz",
                 s->id.PID, kRes[s_res].name, s_quality, s_xclk);
    }
    return 0;
}

const uint8_t *cam_frame_get(size_t *len, uint32_t *capture_ms)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return NULL;
    }

    int64_t t0 = esp_timer_get_time();
    s_fb = esp_camera_fb_get();
    *capture_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (s_fb == NULL) {
        xSemaphoreGive(s_lock);
        return NULL;
    }

    *len = s_fb->len;
    return s_fb->buf;
}

void cam_frame_release(void)
{
    if (s_fb != NULL) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
    xSemaphoreGive(s_lock);
}

/* Changing frame size or clock means tearing the driver down and building it
 * again -- esp32-camera fixes the DMA layout at init. Done under the same lock
 * the readers use, so no task is inside esp_camera_fb_get() while it happens. */
static int reinit_locked(void)
{
    esp_camera_deinit();

    camera_config_t c = build_config();
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "re-init failed: %s", esp_err_to_name(err));
        return -1;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -1);
    }
    return 0;
}

static int with_lock(int (*fn)(void))
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return -1;
    }
    int rc = fn();
    xSemaphoreGive(s_lock);
    return rc;
}

int cam_set_resolution(cam_res_t res)
{
    if (res < 0 || res >= CAM_RES_COUNT) {
        return -1;
    }
    if (res == s_res) {
        return 0;
    }
    cam_res_t previous = s_res;
    s_res = res;

    if (with_lock(reinit_locked) < 0) {
        s_res = previous;
        return -1;
    }
    ESP_LOGI(TAG, "resolution -> %s", kRes[s_res].name);
    return 0;
}

int cam_set_quality(int quality)
{
    if (quality < 0 || quality > 63) {
        return -1;
    }
    s_quality = quality;

    /* Quality is the one control the sensor takes live, with no re-init. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return -1;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc = (s != NULL) ? s->set_quality(s, quality) : -1;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "quality -> %d", quality);
    return rc;
}

int cam_set_xclk_mhz(int mhz)
{
    /* Below 10 MHz the sensor stops producing usable frames; above 24 it is
     * out of spec for the OV3660 on this module. */
    if (mhz < 10 || mhz > 24) {
        return -1;
    }
    if (mhz == s_xclk) {
        return 0;
    }
    int previous = s_xclk;
    s_xclk = mhz;

    if (with_lock(reinit_locked) < 0) {
        s_xclk = previous;
        return -1;
    }
    ESP_LOGI(TAG, "xclk -> %d MHz", mhz);
    return 0;
}

int cam_reset(void)
{
    ESP_LOGW(TAG, "sensor reset requested");
    return with_lock(reinit_locked);
}

cam_res_t cam_get_resolution(void) { return s_res; }
int       cam_get_quality(void)    { return s_quality; }
int       cam_get_xclk_mhz(void)   { return s_xclk; }

const char *cam_res_name(cam_res_t res)
{
    return (res >= 0 && res < CAM_RES_COUNT) ? kRes[res].name : "?";
}

int cam_res_from_name(const char *name)
{
    for (int i = 0; i < CAM_RES_COUNT; i++) {
        if (strcmp(name, kRes[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

void cam_stats_tick(cam_stats_t *st)
{
    int64_t now = esp_timer_get_time();
    if (st->window_start_us == 0) {
        st->window_start_us = now;
        return;
    }

    int64_t elapsed_us = now - st->window_start_us;
    if (elapsed_us < 1000000) {
        return;
    }

    float seconds = (float)elapsed_us / 1000000.0f;
    st->fps = (float)st->acc_frames / seconds;

    if (st->acc_frames > 0) {
        st->frame_kb   = st->acc_bytes / st->acc_frames / 1024;
        st->capture_ms = st->acc_capture_ms / st->acc_frames;
        st->send_ms    = st->acc_send_ms / st->acc_frames;
    } else {
        st->frame_kb = st->capture_ms = st->send_ms = 0;
    }

    st->acc_frames = st->acc_bytes = 0;
    st->acc_capture_ms = st->acc_send_ms = 0;
    st->window_start_us = now;
}
