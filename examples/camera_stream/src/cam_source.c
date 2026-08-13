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

/* Defined with the control table further down; declared here because both
 * init paths have to re-apply every control after esp_camera_init() resets
 * the sensor. */
static void apply_all_controls(sensor_t *s);

/* Plain int rather than cam_res_t so reinit_with_rollback can take its
 * address; an enum's underlying type is not guaranteed to be int. */
static int       s_res     = CAM_RES_VGA;
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
        apply_all_controls(s);
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
    apply_all_controls(esp_camera_sensor_get());
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

/*
 * Change a setting that needs the driver rebuilt, and put the camera back if it
 * does not come up.
 *
 * Restoring the variable is not enough on its own. build_config() reads these
 * globals, so a failed re-init leaves the driver deinitialised while s_res says
 * something perfectly reasonable -- the state is consistent and the camera is
 * dead. The rollback has to run the init again with the old value, and if that
 * fails too there is nothing left to try: say so rather than return a code that
 * suggests the previous setting is still working.
 */
static int reinit_with_rollback(int *setting, int wanted, const char *what)
{
    int previous = *setting;
    if (wanted == previous) {
        return 0;
    }
    *setting = wanted;

    if (with_lock(reinit_locked) == 0) {
        ESP_LOGI(TAG, "%s -> %d", what, wanted);
        return 0;
    }

    *setting = previous;
    if (with_lock(reinit_locked) == 0) {
        ESP_LOGW(TAG, "%s %d refused; back on %d", what, wanted, previous);
    } else {
        ESP_LOGE(TAG, "%s %d refused and %d would not come back either -- "
                 "the sensor is down until /api/reset or a reboot",
                 what, wanted, previous);
    }
    return -1;
}

int cam_set_resolution(cam_res_t res)
{
    if (res < 0 || res >= CAM_RES_COUNT) {
        return -1;
    }
    return reinit_with_rollback(&s_res, (int)res, "resolution");
}

int cam_set_quality(int quality)
{
    if (quality < 0 || quality > 63) {
        return -1;
    }

    /* Quality is the one control the sensor takes live, with no re-init -- and
     * the stored value follows the sensor rather than leading it, so a setter
     * that refuses does not leave the page reporting a quality the hardware
     * never accepted. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return -1;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc = (s != NULL) ? s->set_quality(s, quality) : -1;
    xSemaphoreGive(s_lock);

    if (rc != 0) {
        ESP_LOGW(TAG, "sensor refused quality %d", quality);
        return -1;
    }
    s_quality = quality;
    ESP_LOGI(TAG, "quality -> %d", quality);
    return 0;
}

int cam_set_xclk_mhz(int mhz)
{
    /* Below 10 MHz the sensor stops producing usable frames; above 24 it is
     * out of spec for the OV3660 on this module. */
    if (mhz < 10 || mhz > 24) {
        return -1;
    }
    return reinit_with_rollback(&s_xclk, mhz, "xclk");
}

int cam_reset(void)
{
    ESP_LOGW(TAG, "sensor reset requested");
    return with_lock(reinit_locked);
}

/* ---- sensor controls ------------------------------------------------------
 *
 * One row per control, and one apply function per row, so the parser, the JSON
 * and the page's panel all come from this table rather than from three lists
 * that have to be kept in step.
 *
 * Defaults are the sensor's own after reset, except the three cam_source_init()
 * already applied: vflip because the module sits upside down on the Sense
 * board, and brightness/saturation because the OV3660 is washed out at the JPEG
 * quality this example streams at.
 */
typedef int (*cam_apply_fn)(sensor_t *s, int value);

static int ap_brightness(sensor_t *s, int v) { return s->set_brightness(s, v); }
static int ap_contrast(sensor_t *s, int v)   { return s->set_contrast(s, v); }
static int ap_saturation(sensor_t *s, int v) { return s->set_saturation(s, v); }
static int ap_sharpness(sensor_t *s, int v)  { return s->set_sharpness(s, v); }
static int ap_special(sensor_t *s, int v)    { return s->set_special_effect(s, v); }

static int ap_awb(sensor_t *s, int v)        { return s->set_whitebal(s, v); }
static int ap_awb_gain(sensor_t *s, int v)   { return s->set_awb_gain(s, v); }
static int ap_wb_mode(sensor_t *s, int v)    { return s->set_wb_mode(s, v); }
static int ap_aec(sensor_t *s, int v)        { return s->set_exposure_ctrl(s, v); }
static int ap_aec2(sensor_t *s, int v)       { return s->set_aec2(s, v); }
static int ap_ae_level(sensor_t *s, int v)   { return s->set_ae_level(s, v); }
static int ap_aec_value(sensor_t *s, int v)  { return s->set_aec_value(s, v); }
static int ap_agc(sensor_t *s, int v)        { return s->set_gain_ctrl(s, v); }
static int ap_agc_gain(sensor_t *s, int v)   { return s->set_agc_gain(s, v); }
static int ap_gainceiling(sensor_t *s, int v)
{
    return s->set_gainceiling(s, (gainceiling_t)v);
}

static int ap_lenc(sensor_t *s, int v)       { return s->set_lenc(s, v); }
static int ap_raw_gma(sensor_t *s, int v)    { return s->set_raw_gma(s, v); }
static int ap_bpc(sensor_t *s, int v)        { return s->set_bpc(s, v); }
static int ap_wpc(sensor_t *s, int v)        { return s->set_wpc(s, v); }
static int ap_dcw(sensor_t *s, int v)        { return s->set_dcw(s, v); }

static int ap_hmirror(sensor_t *s, int v)    { return s->set_hmirror(s, v); }
static int ap_vflip(sensor_t *s, int v)      { return s->set_vflip(s, v); }
static int ap_colorbar(sensor_t *s, int v)   { return s->set_colorbar(s, v); }

static struct {
    cam_ctrl_t   ctrl;
    cam_apply_fn apply;
} s_ctrls[] = {
    /* Image */
    {{ "brightness", "Brightness",   CAM_GROUP_IMAGE,       -2,    2,  1 }, ap_brightness },
    {{ "contrast",   "Contrast",     CAM_GROUP_IMAGE,       -2,    2,  0 }, ap_contrast   },
    {{ "saturation", "Saturation",   CAM_GROUP_IMAGE,       -2,    2, -1 }, ap_saturation },
    {{ "sharpness",  "Sharpness",    CAM_GROUP_IMAGE,       -2,    2,  0 }, ap_sharpness  },
    /* 0 none, 1 negative, 2 grayscale, 3 red, 4 green, 5 blue, 6 sepia */
    {{ "effect",     "Effect",       CAM_GROUP_IMAGE,        0,    6,  0 }, ap_special    },

    /* Exposure and white balance. The manual levers only bite once their
     * automatic counterpart is switched off, which is why they sit together. */
    {{ "awb",        "Auto WB",      CAM_GROUP_EXPOSURE,     0,    1,  1 }, ap_awb        },
    {{ "awb_gain",   "AWB gain",     CAM_GROUP_EXPOSURE,     0,    1,  1 }, ap_awb_gain   },
    /* 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home */
    {{ "wb_mode",    "WB mode",      CAM_GROUP_EXPOSURE,     0,    4,  0 }, ap_wb_mode    },
    {{ "aec",        "Auto exposure", CAM_GROUP_EXPOSURE,    0,    1,  1 }, ap_aec        },
    {{ "aec2",       "AEC DSP",      CAM_GROUP_EXPOSURE,     0,    1,  1 }, ap_aec2       },
    {{ "ae_level",   "AE level",     CAM_GROUP_EXPOSURE,    -2,    2,  0 }, ap_ae_level   },
    {{ "aec_value",  "Exposure",     CAM_GROUP_EXPOSURE,     0, 1200, 300 }, ap_aec_value },
    {{ "agc",        "Auto gain",    CAM_GROUP_EXPOSURE,     0,    1,  1 }, ap_agc        },
    {{ "agc_gain",   "Gain",         CAM_GROUP_EXPOSURE,     0,   30,  0 }, ap_agc_gain   },
    /* 0 = 2x through 6 = 128x */
    {{ "gainceiling", "Gain ceiling", CAM_GROUP_EXPOSURE,    0,    6,  0 }, ap_gainceiling },

    /* Correction */
    {{ "lenc",       "Lens correction", CAM_GROUP_CORRECTION, 0,   1,  1 }, ap_lenc       },
    {{ "raw_gma",    "Gamma",        CAM_GROUP_CORRECTION,    0,   1,  1 }, ap_raw_gma    },
    {{ "bpc",        "Black pixel",  CAM_GROUP_CORRECTION,    0,   1,  0 }, ap_bpc        },
    {{ "wpc",        "White pixel",  CAM_GROUP_CORRECTION,    0,   1,  1 }, ap_wpc        },
    {{ "dcw",        "Downsize",     CAM_GROUP_CORRECTION,    0,   1,  1 }, ap_dcw        },

    /* Orientation and test */
    {{ "hmirror",    "Mirror",       CAM_GROUP_ORIENTATION,   0,   1,  0 }, ap_hmirror    },
    {{ "vflip",      "Flip",         CAM_GROUP_ORIENTATION,   0,   1,  1 }, ap_vflip      },
    /* A known pattern straight out of the sensor: if the bars arrive clean the
     * fault is in front of the lens, and if they do not it is behind it. */
    {{ "colorbar",   "Test pattern", CAM_GROUP_ORIENTATION,   0,   1,  0 }, ap_colorbar   },
};

#define CTRL_COUNT ((int)(sizeof(s_ctrls) / sizeof(s_ctrls[0])))

static const char *kGroupName[CAM_GROUP_COUNT] = {
    [CAM_GROUP_IMAGE]       = "Image",
    [CAM_GROUP_EXPOSURE]    = "Exposure",
    [CAM_GROUP_CORRECTION]  = "Correction",
    [CAM_GROUP_ORIENTATION] = "Orientation",
};

int cam_ctrl_count(void) { return CTRL_COUNT; }

const cam_ctrl_t *cam_ctrl_at(int index)
{
    return (index >= 0 && index < CTRL_COUNT) ? &s_ctrls[index].ctrl : NULL;
}

const char *cam_group_name(cam_group_t group)
{
    return (group >= 0 && group < CAM_GROUP_COUNT) ? kGroupName[group] : "?";
}

int cam_ctrl_set(const char *name, int value)
{
    for (int i = 0; i < CTRL_COUNT; i++) {
        cam_ctrl_t *ctrl = &s_ctrls[i].ctrl;
        if (strcmp(ctrl->name, name) != 0) {
            continue;
        }
        if (value < ctrl->min || value > ctrl->max) {
            return -1;
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
            return -1;
        }
        sensor_t *s = esp_camera_sensor_get();
        int rc = (s != NULL) ? s_ctrls[i].apply(s, value) : -1;
        xSemaphoreGive(s_lock);

        if (rc == 0) {
            /* Only remember it once the sensor accepted it, so the page never
             * shows a value the hardware refused. */
            ctrl->value = value;
            ESP_LOGI(TAG, "%s -> %d", name, value);
        }
        return rc;
    }
    return -1;
}

/* Push the whole table at the sensor. Used after every re-init, because
 * esp_camera_init() resets the sensor and would otherwise silently undo
 * everything the user set. */
static void apply_all_controls(sensor_t *s)
{
    if (s == NULL) {
        return;
    }
    for (int i = 0; i < CTRL_COUNT; i++) {
        s_ctrls[i].apply(s, s_ctrls[i].ctrl.value);
    }
}

cam_res_t cam_get_resolution(void) { return (cam_res_t)s_res; }
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
