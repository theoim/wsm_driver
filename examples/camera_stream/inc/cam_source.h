/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The camera, and the numbers the web page plots.
 *
 * Wraps espressif/esp32-camera so the rest of the example never includes it:
 * the server moves JPEG buffers around and asks for statistics, and this file
 * is the only place that knows what a sensor is.
 *
 * One sensor, many readers. Both network interfaces stream from the same
 * camera because there is only one, so the frame buffer is handed out under a
 * mutex and the statistics are per interface -- Ethernet and Wi-Fi each keep
 * their own frame rate and byte counts, which is the entire point of serving
 * the page on both: the two stacks are measured against one identical source.
 */
#ifndef CAM_SOURCE_H
#define CAM_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resolutions offered by the page, as the strings it sends to /api/res. Kept
 * short deliberately: every step up costs frame time and link bandwidth, and
 * the point of the example is to watch that trade rather than to enumerate
 * everything the OV3660 can do. */
typedef enum {
    CAM_RES_QVGA = 0,   /* 320 x 240  */
    CAM_RES_VGA,        /* 640 x 480  */
    CAM_RES_SVGA,       /* 800 x 600  */
    CAM_RES_HD,         /* 1280 x 720 */
    CAM_RES_UXGA,       /* 1600 x 1200 */
    CAM_RES_COUNT,
} cam_res_t;

/* Per-interface counters. One of these belongs to each server task, so the
 * TOE and the LwIP side never share a frame count. */
typedef struct {
    bool     streaming;
    uint32_t frames;
    uint32_t dropped;

    /* Rolling averages over the last second, which is also the rate the page
     * polls at -- averaging over a shorter window makes the chart jitter, over
     * a longer one it stops responding to a resolution change. */
    float    fps;
    uint32_t frame_kb;
    uint32_t capture_ms;
    uint32_t send_ms;

    /* Accumulators behind those averages; the server updates them per frame
     * and cam_stats_tick() folds them down once a second. */
    uint32_t acc_frames;
    uint32_t acc_bytes;
    uint32_t acc_capture_ms;
    uint32_t acc_send_ms;
    int64_t  window_start_us;
} cam_stats_t;

/* Bring the sensor up. Safe to call once, before any server starts.
 *
 * Named cam_source_init rather than cam_init because esp32-camera exports a
 * cam_init() of its own from cam_hal.c, and the two collide at link. */
int cam_source_init(void);

/*
 * Borrow the newest frame. Blocks until one is ready, so the caller's elapsed
 * time is the capture cost. Returns NULL if the sensor failed, in which case
 * the caller should count a drop rather than tear the connection down: a
 * dropped frame is recoverable and a closed stream is not.
 *
 * The buffer belongs to the driver until cam_frame_release(). Both interfaces
 * take turns through one mutex, so hold it only for as long as the send takes.
 */
const uint8_t *cam_frame_get(size_t *len, uint32_t *capture_ms);
void cam_frame_release(void);

/* Sensor controls, all of which the page can reach. Return 0 on success. */
int cam_set_resolution(cam_res_t res);
int cam_set_quality(int quality);       /* 0..63, lower is better and bigger */
int cam_set_xclk_mhz(int mhz);          /* 10..24; the sensor clock sweep      */
int cam_reset(void);                    /* re-init after a wedged sensor       */

/*
 * ---- sensor controls -------------------------------------------------------
 *
 * Everything the OV3660 exposes beyond frame size and JPEG quality: the image
 * levels, the exposure and gain machinery, the correction toggles, and mirroring.
 *
 * Described as a table rather than a function per control, because otherwise the
 * same list has to be written three times -- once to parse the query, once to
 * emit the status JSON, once to lay out the page -- and the three drift. The
 * server walks this table for all three, and the page builds its panel from
 * /api/controls, so adding a control here makes it appear in the browser with
 * no edit to the HTML.
 */
typedef enum {
    CAM_GROUP_IMAGE = 0,    /* brightness and friends           */
    CAM_GROUP_EXPOSURE,     /* AEC / AGC / white balance        */
    CAM_GROUP_CORRECTION,   /* lens shading, gamma, bad pixels  */
    CAM_GROUP_ORIENTATION,  /* mirror, flip, test pattern       */
    CAM_GROUP_COUNT,
} cam_group_t;

typedef struct {
    const char *name;       /* the query key and the JSON key    */
    const char *label;      /* what the page shows               */
    cam_group_t group;
    int         min;
    int         max;        /* min 0 / max 1 means a checkbox    */
    int         value;
} cam_ctrl_t;

int                 cam_ctrl_count(void);
const cam_ctrl_t   *cam_ctrl_at(int index);
const char         *cam_group_name(cam_group_t group);

/* Apply one control by name. Returns 0 on success, -1 if the name is unknown
 * or the value is out of range. */
int cam_ctrl_set(const char *name, int value);

cam_res_t   cam_get_resolution(void);
int         cam_get_quality(void);
int         cam_get_xclk_mhz(void);
const char *cam_res_name(cam_res_t res);        /* "640x480" */
int         cam_res_from_name(const char *name); /* -1 if unknown */

/* Fold the last second of accumulators into the averages the page reads.
 * Called by each server task once per second against its own stats. */
void cam_stats_tick(cam_stats_t *st);

#endif /* CAM_SOURCE_H */
