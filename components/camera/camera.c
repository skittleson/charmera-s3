#include "camera.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"

#define PIN_PWDN  17
#define PIN_XCLK  8
#define PIN_SIOD  21
#define PIN_SIOC  16
#define PIN_VSYNC 6
#define PIN_HREF  4
#define PIN_PCLK  9

/* The viewfinder is the sensor frame turned 90 degrees to be upright in the portrait
   grip (USB-C down). Flip if it shows upside down. */
#define ROTATE_CCW 1

/* OV2640 sensor-bank registers (bank bit = 0x100 for get_reg/set_reg). */
#define REG_GAIN  0x100
#define REG_04    0x104
#define REG_AEC   0x110
#define REG_45    0x145

typedef struct {
    const char *name;
    framesize_t full_size;
    int full_w, full_h;
    bool jpeg;          /* full-res as sensor JPEG (one init, size switched live) */
    int jpeg_quality;   /* esp32-camera scale, lower is better */
    size_t jpeg_buf;
    int xclk_hz;
    int settle_frames;  /* frames dropped after a size switch / restart */
} profile_t;

/* OV2640 on this board: JPEG output is broken (NO-SOI at every clock and size) and
   YUV422 is green-tinted, but RGB565 is clean. Raw frame buffers can't change size, so
   the driver is restarted at full size for each shot. 16 MHz: ~0.8 s per frame. */
static const profile_t OV2640_PROFILE = {
    .name = "OV2640", .full_size = FRAMESIZE_UXGA, .full_w = 1600, .full_h = 1200,
    .jpeg = false, .xclk_hz = 16000000, .settle_frames = 1,
};

/* OV5640: 2560x1920 sensor JPEG at 24 MHz, two 1.5 MB buffers (a truncated frame is
   skipped instead of stalling the driver). Measured on this module: quality 6-9 works
   for the first shot or two, then every full-res frame arrives truncated; 12-14 stayed
   reliable shot after shot. So capture uses 12, and if frames ever fail it steps down
   (12 -> 15 -> 18) and stays there until reboot. */
static const profile_t OV5640_PROFILE = {
    .name = "OV5640", .full_size = FRAMESIZE_QSXGA, .full_w = 2560, .full_h = 1920,
    .jpeg = true, .jpeg_quality = 12, .jpeg_buf = 1536 * 1024, .xclk_hz = 24000000,
    .settle_frames = 2,
};

static const char *TAG = "camera";

static const profile_t *s_prof;
static sensor_t *s_sensor;
static bool s_has_af;
static uint16_t *s_preview;
static uint16_t *s_land; /* 320x240 decoded JPEG preview (OV5640) */
static jpeg_dec_handle_t s_prev_dec;
static look_t s_look = LOOK_NORMAL;
/* OV5640 JPEG quality in use (esp32-camera scale, lower = better). Starts at the
   profile's best and backs off when a sharp, detailed scene doesn't fit the buffer. */
static int s_jpeg_q;
#define JPEG_Q_WORST 18

static void drop_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

/* Start the driver. For JPEG profiles the buffers are sized for full resolution and
   the frame size is switched live; for raw profiles they fit `size` exactly. */
static esp_err_t start(framesize_t size, pixformat_t fmt, int xclk, int quality, size_t jpeg_buf, int fb_count)
{
    const camera_config_t c = {
        .pin_pwdn = PIN_PWDN,
        .pin_reset = -1,
        .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD,
        .pin_sccb_scl = PIN_SIOC,
        .pin_d7 = 2,
        .pin_d6 = 7,
        .pin_d5 = 10,
        .pin_d4 = 14,
        .pin_d3 = 11,
        .pin_d2 = 15,
        .pin_d1 = 13,
        .pin_d0 = 12,
        .pin_vsync = PIN_VSYNC,
        .pin_href = PIN_HREF,
        .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = xclk,
        .ledc_timer = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = fmt,
        .frame_size = size,
        .jpeg_quality = quality,
        .jpeg_buffer_size = jpeg_buf,
        .fb_count = fb_count,
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* With one buffer, "latest" lets the driver overwrite a frame we're still
           reading; "when empty" holds it until returned. */
        .grab_mode = fb_count > 1 ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        s_sensor = NULL;
        return err;
    }
    s_sensor = esp_camera_sensor_get();
    camera_set_look(s_look);
    return ESP_OK;
}

static void stop(void)
{
    s_sensor = NULL;
    esp_camera_deinit();
}

static esp_err_t start_preview(void)
{
    if (s_prof->jpeg) {
        return ESP_OK; /* JPEG profiles never stop */
    }
    return start(FRAMESIZE_QVGA, PIXFORMAT_RGB565, s_prof->xclk_hz, 12, 0, 2);
}

esp_err_t camera_init(void)
{
    /* Probe at a size and format every supported sensor handles. */
    esp_err_t err = start(FRAMESIZE_QVGA, PIXFORMAT_RGB565, 16000000, 12, 0, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no camera found");
        return err;
    }
    const uint16_t pid = s_sensor->id.PID;
    if (pid == OV5640_PID) {
        s_prof = &OV5640_PROFILE;
    } else if (pid == OV2640_PID) {
        s_prof = &OV2640_PROFILE;
    } else {
        ESP_LOGW(TAG, "unknown sensor 0x%04x, using OV2640 settings", pid);
        s_prof = &OV2640_PROFILE;
    }
    ESP_LOGI(TAG, "sensor %s: photos %dx%d %s", s_prof->name, s_prof->full_h, s_prof->full_w,
             s_prof->jpeg ? "(sensor JPEG)" : "(raw, firmware JPEG)");

    s_preview = heap_caps_malloc(CAMERA_PREVIEW_W * CAMERA_PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_preview) {
        return ESP_ERR_NO_MEM;
    }
    if (s_prof->jpeg) {
        /* Restart once in JPEG mode with full-size buffers, then run the viewfinder at
           320x240 by switching the frame size live. */
        stop();
        s_jpeg_q = s_prof->jpeg_quality;
        err = start(s_prof->full_size, PIXFORMAT_JPEG, s_prof->xclk_hz, s_jpeg_q, s_prof->jpeg_buf, 2);
        if (err != ESP_OK) {
            return err;
        }
        s_sensor->set_framesize(s_sensor, FRAMESIZE_QVGA);
        s_land = jpeg_calloc_align(320 * 240 * 2, 16);
        jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
        dc.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        if (!s_land || jpeg_dec_open(&dc, &s_prev_dec) != JPEG_ERR_OK) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        /* raw profile: keep the probe's QVGA RGB565 preview but at the profile clock */
        stop();
        err = start_preview();
        if (err != ESP_OK) {
            return err;
        }
    }

#if CONFIG_CAMERA_AF_SUPPORT
    if (s_prof->jpeg && s_sensor->af_is_supported && s_sensor->af_is_supported(s_sensor) && s_sensor->af_init &&
        s_sensor->af_init(s_sensor, 3000) == 0) {
        s_has_af = true;


    }
#endif
    ESP_LOGI(TAG, "autofocus: %s", s_has_af ? "yes" : "no");
    drop_frames(s_prof->settle_frames);
    return ESP_OK;
}

const char *camera_sensor_name(void)
{
    return s_prof ? s_prof->name : "none";
}

int camera_full_width(void)
{
    return s_prof ? s_prof->full_w : 0;
}

int camera_full_height(void)
{
    return s_prof ? s_prof->full_h : 0;
}

bool camera_full_is_jpeg(void)
{
    return s_prof && s_prof->jpeg;
}

int camera_photo_width(void)
{
    return camera_full_height();
}

int camera_photo_height(void)
{
    return camera_full_width();
}

/* Portrait (px, py) -> landscape (sx, sy) for a sw x sh landscape image. */
static inline void portrait_to_land(int px, int py, int sw, int sh, int *sx, int *sy)
{
#if ROTATE_CCW
    (void)sh;
    *sx = sw - 1 - py;
    *sy = px;
#else
    (void)sw;
    *sx = py;
    *sy = sh - 1 - px;
#endif
}

void camera_rotate_to_portrait(const uint16_t *land, uint16_t *out)
{
    for (int dy = 0; dy < CAMERA_PREVIEW_H; dy++) {
        for (int dx = 0; dx < CAMERA_PREVIEW_W; dx++) {
            int sx, sy;
            portrait_to_land(dx, dy, 320, 240, &sx, &sy);
            out[dy * CAMERA_PREVIEW_W + dx] = land[sy * 320 + sx];
        }
    }
}

/* Decode a JPEG to 320x240 RGB565 (scale 1/1..1/8 as needed). */
static bool decode_320x240(const uint8_t *buf, size_t len, int w, int h, uint16_t *out)
{
    jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
    dc.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (w != 320 || h != 240) {
        dc.scale.width = 320;
        dc.scale.height = 240;
    }
    jpeg_dec_handle_t dec;
    if (jpeg_dec_open(&dc, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)buf, .inbuf_len = (int)len, .outbuf = (uint8_t *)out};
    jpeg_dec_header_info_t info;
    const bool ok = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
    jpeg_dec_close(dec);
    return ok;
}

uint16_t *camera_preview_frame(void)
{
    if (!s_sensor) {
        return NULL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        return NULL;
    }
    bool ok = false;
    if (fb->format == PIXFORMAT_JPEG) {
        if (fb->width == 320 && fb->height == 240) {
            jpeg_dec_io_t io = {.inbuf = fb->buf, .inbuf_len = (int)fb->len, .outbuf = (uint8_t *)s_land};
            jpeg_dec_header_info_t info;
            ok = jpeg_dec_parse_header(s_prev_dec, &io, &info) == JPEG_ERR_OK && info.width == 320 &&
                 info.height == 240 && jpeg_dec_process(s_prev_dec, &io) == JPEG_ERR_OK;
        }
        esp_camera_fb_return(fb);
        if (ok) {
            camera_rotate_to_portrait(s_land, s_preview);
        }
        return ok ? s_preview : NULL;
    }
    const int sw = fb->width, sh = fb->height;
    if (sw == 320 && sh == 240 && fb->len >= (size_t)sw * sh * 2) {
        /* Big-endian RGB565 from the sensor. */
        const uint8_t *src = fb->buf;
        for (int dy = 0; dy < CAMERA_PREVIEW_H; dy++) {
            for (int dx = 0; dx < CAMERA_PREVIEW_W; dx++) {
                int sx, sy;
                portrait_to_land(dx, dy, sw, sh, &sx, &sy);
                const uint8_t *p = src + ((size_t)sy * sw + sx) * 2;
                s_preview[dy * CAMERA_PREVIEW_W + dx] = (uint16_t)((p[0] << 8) | p[1]);
            }
        }
        ok = true;
    }
    esp_camera_fb_return(fb);
    return ok ? s_preview : NULL;
}

/* Raw frames are big-endian RGB565. */
static inline void raw_rgb(const camera_fb_t *fb, int sx, int sy, uint8_t *rgb)
{
    const uint8_t *q = fb->buf + ((size_t)sy * fb->width + sx) * 2;
    const uint16_t v = (uint16_t)((q[0] << 8) | q[1]);
    rgb[0] = (uint8_t)(((v >> 11) & 0x1f) << 3);
    rgb[1] = (uint8_t)(((v >> 5) & 0x3f) << 2);
    rgb[2] = (uint8_t)((v & 0x1f) << 3);
}

void camera_photo_rows_rgb888(const camera_fb_t *fb, int y0, int rows, uint8_t *out)
{
    /* Portrait row py comes from sensor column sx, so walk each sensor row once and
       read the strip's span of it contiguously (much kinder to the PSRAM cache). */
    const int sw = fb->width, sh = fb->height, w = sh;
    for (int px = 0; px < w; px++) {
        for (int r = 0; r < rows; r++) {
            int sx, sy;
            portrait_to_land(px, y0 + r, sw, sh, &sx, &sy);
            raw_rgb(fb, sx, sy, out + ((size_t)r * w + px) * 3);
        }
    }
}

esp_err_t camera_photo_thumbnail(const camera_fb_t *fb, uint16_t *out)
{
    if (fb->format == PIXFORMAT_JPEG) {
        if (!s_land || !decode_320x240(fb->buf, fb->len, fb->width, fb->height, s_land)) {
            return ESP_FAIL;
        }
        camera_rotate_to_portrait(s_land, out);
        return ESP_OK;
    }
    const int sw = fb->width, sh = fb->height, pw = sh, ph = sw;
    for (int dy = 0; dy < CAMERA_PREVIEW_H; dy++) {
        for (int dx = 0; dx < CAMERA_PREVIEW_W; dx++) {
            int sx, sy;
            portrait_to_land(dx * pw / CAMERA_PREVIEW_W, dy * ph / CAMERA_PREVIEW_H, sw, sh, &sx, &sy);
            uint8_t rgb[3];
            raw_rgb(fb, sx, sy, rgb);
            out[dy * CAMERA_PREVIEW_W + dx] =
                (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
        }
    }
    return ESP_OK;
}

esp_err_t camera_set_look(look_t l)
{
    s_look = l; /* all looks are applied in software; the sensor stays neutral */
    if (!s_sensor) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sensor->set_whitebal(s_sensor, 1);
    s_sensor->set_awb_gain(s_sensor, 1);
    s_sensor->set_wb_mode(s_sensor, 0);
    s_sensor->set_special_effect(s_sensor, 0);
    return ESP_OK;
}

bool camera_has_af(void)
{
    return s_has_af;
}

esp_err_t camera_focus(uint32_t timeout_ms)
{
    /* Focus happens inside camera_capture_full(), at full resolution. */
    (void)timeout_ms;
    return ESP_OK;
}

/* OV2640: freeze the viewfinder's auto exposure/gain into manual values at full
   resolution, so the photo matches it and isn't taken mid-adjustment (1:1 matched
   brightness on the board). */
static void ov2640_carry_exposure(int aec, int gain)
{
    s_sensor->set_exposure_ctrl(s_sensor, 0);
    s_sensor->set_gain_ctrl(s_sensor, 0);
    s_sensor->set_reg(s_sensor, REG_GAIN, 0xff, gain);
    const int v = aec < 1 ? 1 : aec;
    s_sensor->set_reg(s_sensor, REG_04, 0x03, v & 0x3);
    s_sensor->set_reg(s_sensor, REG_AEC, 0xff, (v >> 2) & 0xff);
    s_sensor->set_reg(s_sensor, REG_45, 0x3f, (v >> 10) & 0x3f);
}

static bool frame_ok(const camera_fb_t *fb)
{
    if (!fb || (int)fb->width != s_prof->full_w || (int)fb->height != s_prof->full_h) {
        return false;
    }
    if (s_prof->jpeg) {
        return fb->len > 4 && fb->buf[0] == 0xff && fb->buf[1] == 0xd8;
    }
    return fb->len >= (size_t)s_prof->full_w * s_prof->full_h * 2;
}

/* Single autofocus, run at full resolution right before the grab. */
static void af_single_full(void)
{
#if CONFIG_CAMERA_AF_SUPPORT
    if (!s_has_af) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    if (s_sensor->af_trigger(s_sensor) != 0) {
        ESP_LOGW(TAG, "AF trigger failed");
        return;
    }
    bool focused = false;
    for (int i = 0; i < 50; i++) {
        uint8_t raw;
        bool busy = true;
        if (s_sensor->af_get_status(s_sensor, &raw, &focused, &busy) == 0 && !busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGD(TAG, "AF %s in %lld ms", focused ? "focused" : "not focused", (esp_timer_get_time() - t0) / 1000);
    drop_frames(1); /* a frame exposed while the lens moved */
#endif
}

camera_fb_t *camera_capture_full(void)
{
    if (!s_sensor) {
        return NULL;
    }
    if (s_prof->jpeg) {

        const int r = s_sensor->set_framesize(s_sensor, s_prof->full_size);
        if (r != 0) {
            return NULL;
        }
        drop_frames(s_prof->settle_frames);
        af_single_full();
    } else {
        const int aec = (s_sensor->get_reg(s_sensor, REG_45, 0x3f) << 10) |
                        (s_sensor->get_reg(s_sensor, REG_AEC, 0xff) << 2) | s_sensor->get_reg(s_sensor, REG_04, 0x03);
        const int gain = s_sensor->get_reg(s_sensor, REG_GAIN, 0xff);
        stop();
        if (start(s_prof->full_size, PIXFORMAT_RGB565, s_prof->xclk_hz, 12, 0, 1) != ESP_OK) {
            start_preview();
            return NULL;
        }
        if (aec > 0) {
            ov2640_carry_exposure(aec, gain);
        }
        drop_frames(s_prof->settle_frames);
    }
    /* A truncated JPEG frame (no end marker) is dropped by the driver and shows up as a
       timeout: step the quality down and try again. */
    camera_fb_t *fb = NULL;
    for (int attempt = 0; attempt < 6 && !fb; attempt++) {
        const int64_t t = esp_timer_get_time();
        fb = esp_camera_fb_get();
        ESP_LOGD(TAG, "grab %d (q%d): %s in %lld ms", attempt, s_jpeg_q, fb ? "frame" : "timeout",
                 (esp_timer_get_time() - t) / 1000);
        if (fb && !frame_ok(fb)) {
            ESP_LOGW(TAG, "bad capture %ux%u len %u", (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len);
            esp_camera_fb_return(fb);
            fb = NULL;
        }
        if (!fb && s_prof->jpeg && s_jpeg_q < JPEG_Q_WORST) {
            s_jpeg_q += 3;
            s_sensor->set_quality(s_sensor, s_jpeg_q);
            drop_frames(1);
        }
    }
    if (!fb) {
        camera_capture_done(NULL);
    }
    return fb;
}

void camera_capture_done(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
    if (s_prof->jpeg) {
        if (s_sensor) {
            s_sensor->set_framesize(s_sensor, FRAMESIZE_QVGA);
            drop_frames(1);
        }
        return;
    }
    stop();
    start_preview();
}

void camera_power(bool on)
{
    if (!s_sensor) {
        return;
    }
    gpio_set_level(PIN_PWDN, on ? 0 : 1);
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
