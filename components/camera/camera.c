#include "camera.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
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

/* This board's OV2640 gives broken JPEG (NO-SOI at every clock and size) and
   green-tinted YUV422, but clean RGB565. So preview and capture are both RGB565 and we
   encode JPEG ourselves. 16 MHz gives ~0.8 s per full-res frame. */
#define XCLK_HZ 16000000

/* Frames dropped after a restart. */
#define SETTLE_FRAMES 1

/* OV2640 sensor-bank registers (bank bit = 0x100 for get_reg/set_reg). */
#define REG_GAIN  0x100
#define REG_04    0x104
#define REG_AEC   0x110
#define REG_45    0x145

/* Preview exposure (in sensor lines) carried into the capture, scaled x/256.
   1:1 matched brightness on the board. */
#ifndef AEC_SCALE
#define AEC_SCALE 256
#endif

/* Preview: the sensor's 320x240 frame is turned 90 degrees to be upright in the
   portrait grip (USB-C down). Flip if it shows upside down. */
#define ROTATE_CCW 1

static const char *TAG = "camera";

static sensor_t *s_sensor;
static bool s_has_af;
static uint16_t *s_preview;
static framesize_t s_full_size = FRAMESIZE_UXGA;
static int s_full_w = 1600, s_full_h = 1200;

static void drop_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

static look_t s_look = LOOK_NORMAL;

static esp_err_t start(framesize_t size)
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
        .xclk_freq_hz = XCLK_HZ,
        .ledc_timer = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = size,
        .fb_count = size == FRAMESIZE_QVGA ? 2 : 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* With one full-res buffer, "latest" lets the driver overwrite the frame while
           we encode it; "when empty" holds it until returned. */
        .grab_mode = size == FRAMESIZE_QVGA ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        return err;
    }
    s_sensor = esp_camera_sensor_get();
    camera_set_look(s_look);
    return ESP_OK;
}

esp_err_t camera_init(void)
{
    esp_err_t err = start(FRAMESIZE_QVGA);
    if (err != ESP_OK) {
        return err;
    }
    s_sensor = esp_camera_sensor_get();
    if (s_sensor->id.PID == OV5640_PID) {
        s_full_size = FRAMESIZE_5MP;
        s_full_w = 2592;
        s_full_h = 1944;
    }
    ESP_LOGI(TAG, "sensor PID 0x%04x, full size %dx%d", s_sensor->id.PID, s_full_w, s_full_h);

#if CONFIG_CAMERA_AF_SUPPORT
    if (s_sensor->af_is_supported && s_sensor->af_is_supported(s_sensor) && s_sensor->af_init &&
        s_sensor->af_init(s_sensor, 3000) == 0) {
        s_has_af = true;
    }
#endif
    ESP_LOGI(TAG, "autofocus: %s", s_has_af ? "yes" : "no");

    s_preview = heap_caps_malloc(CAMERA_PREVIEW_W * CAMERA_PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_preview) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int camera_full_width(void)
{
    return s_full_w;
}

int camera_full_height(void)
{
    return s_full_h;
}

uint16_t *camera_preview_frame(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        return NULL;
    }
    const int sw = fb->width, sh = fb->height;
    if (sw != CAMERA_PREVIEW_H || sh != CAMERA_PREVIEW_W ||
        fb->len < (size_t)sw * sh * 2) {
        esp_camera_fb_return(fb);
        return NULL;
    }
    /* Sensor frame is 320x240 (big-endian RGB565); the portrait view is the same pixels
       turned 90 degrees, 240x320. */
    const uint8_t *src = fb->buf;
    for (int dy = 0; dy < CAMERA_PREVIEW_H; dy++) {
        for (int dx = 0; dx < CAMERA_PREVIEW_W; dx++) {
#if ROTATE_CCW
            const int sx = sw - 1 - dy, sy = dx;
#else
            const int sx = dy, sy = sh - 1 - dx;
#endif
            const uint8_t *p = src + ((size_t)sy * sw + sx) * 2;
            s_preview[dy * CAMERA_PREVIEW_W + dx] = (uint16_t)((p[0] << 8) | p[1]);
        }
    }
    esp_camera_fb_return(fb);
    return s_preview;
}

int camera_photo_width(void)
{
    return s_full_h;
}

int camera_photo_height(void)
{
    return s_full_w;
}

/* Portrait (px, py) -> sensor (sx, sy); same turn as the preview. */
static inline void photo_to_sensor(int px, int py, int *sx, int *sy)
{
#if ROTATE_CCW
    *sx = s_full_w - 1 - py;
    *sy = px;
#else
    *sx = py;
    *sy = s_full_h - 1 - px;
#endif
}

/* Sensor frames are big-endian RGB565. */
static inline void sensor_rgb(const camera_fb_t *fb, int sx, int sy, uint8_t *rgb)
{
    const uint8_t *q = fb->buf + ((size_t)sy * s_full_w + sx) * 2;
    const uint16_t v = (uint16_t)((q[0] << 8) | q[1]);
    rgb[0] = (uint8_t)(((v >> 11) & 0x1f) << 3);
    rgb[1] = (uint8_t)(((v >> 5) & 0x3f) << 2);
    rgb[2] = (uint8_t)((v & 0x1f) << 3);
}

void camera_photo_rows_rgb888(const camera_fb_t *fb, int y0, int rows, uint8_t *out)
{
    /* Portrait row py comes from sensor column sx, so walk each sensor row once and
       read the strip's span of it contiguously (much kinder to the PSRAM cache than
       reading columns). */
    const int w = s_full_h;
    for (int px = 0; px < w; px++) {
        for (int r = 0; r < rows; r++) {
            int sx, sy;
            photo_to_sensor(px, y0 + r, &sx, &sy);
            sensor_rgb(fb, sx, sy, out + ((size_t)r * w + px) * 3);
        }
    }
}

void camera_photo_thumbnail(const camera_fb_t *fb, uint16_t *out)
{
    const int pw = s_full_h, ph = s_full_w;
    for (int dy = 0; dy < CAMERA_PREVIEW_H; dy++) {
        for (int dx = 0; dx < CAMERA_PREVIEW_W; dx++) {
            int sx, sy;
            photo_to_sensor(dx * pw / CAMERA_PREVIEW_W, dy * ph / CAMERA_PREVIEW_H, &sx, &sy);
            uint8_t rgb[3];
            sensor_rgb(fb, sx, sy, rgb);
            out[dy * CAMERA_PREVIEW_W + dx] =
                (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
        }
    }
}

esp_err_t camera_set_look(look_t l)
{
    s_look = l;
    if (!s_sensor) {
        return ESP_ERR_INVALID_STATE;
    }
    /* esp32-camera: wb_mode 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home;
       special_effect 0 none, 2 grayscale. */
    int wb = 0, effect = 0;
    switch (l) {
    case LOOK_COOL: wb = 3; break;
    case LOOK_WARM: wb = 2; break;
    case LOOK_BW: effect = 2; break;
    default: break;
    }
    s_sensor->set_whitebal(s_sensor, 1);
    s_sensor->set_awb_gain(s_sensor, wb == 0 ? 1 : 0);
    s_sensor->set_wb_mode(s_sensor, wb);
    s_sensor->set_special_effect(s_sensor, effect);
    return ESP_OK;
}

bool camera_has_af(void)
{
    return s_has_af;
}

esp_err_t camera_focus(uint32_t timeout_ms)
{
#if CONFIG_CAMERA_AF_SUPPORT
    if (!s_has_af) {
        return ESP_OK;
    }
    if (s_sensor->af_trigger(s_sensor) != 0) {
        return ESP_FAIL;
    }
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        uint8_t raw;
        bool focused = false, busy = true;
        if (s_sensor->af_get_status(s_sensor, &raw, &focused, &busy) == 0 && !busy) {
            return focused ? ESP_OK : ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return ESP_ERR_TIMEOUT;
#else
    (void)timeout_ms;
    return ESP_OK;
#endif
}

static int s_last_aec, s_last_gain;

/* Freeze the preview's auto exposure/gain into manual values at full resolution, so the
   photo matches the viewfinder and isn't taken mid-adjustment. */
static void carry_exposure(int aec, int gain)
{
    s_sensor->set_exposure_ctrl(s_sensor, 0);
    s_sensor->set_gain_ctrl(s_sensor, 0);
    s_sensor->set_reg(s_sensor, REG_GAIN, 0xff, gain);
    int v = aec * AEC_SCALE / 256;
    if (v < 1) {
        v = 1;
    }
    s_sensor->set_reg(s_sensor, REG_04, 0x03, v & 0x3);
    s_sensor->set_reg(s_sensor, REG_AEC, 0xff, (v >> 2) & 0xff);
    s_sensor->set_reg(s_sensor, REG_45, 0x3f, (v >> 10) & 0x3f);
}

void camera_last_exposure(int *aec, int *gain)
{
    *aec = s_last_aec;
    *gain = s_last_gain;
}

camera_fb_t *camera_capture_full(void)
{
    const int aec = (s_sensor->get_reg(s_sensor, REG_45, 0x3f) << 10) |
                    (s_sensor->get_reg(s_sensor, REG_AEC, 0xff) << 2) |
                    s_sensor->get_reg(s_sensor, REG_04, 0x03);
    const int gain = s_sensor->get_reg(s_sensor, REG_GAIN, 0xff);
    s_last_aec = aec;
    s_last_gain = gain;
    esp_camera_deinit();
    if (start(s_full_size) != ESP_OK) {
        start(FRAMESIZE_QVGA);
        return NULL;
    }
    if (s_sensor->id.PID == OV2640_PID && aec > 0) {
        carry_exposure(aec, gain);
    }
    drop_frames(SETTLE_FRAMES);
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb && ((int)fb->width != s_full_w || (int)fb->height != s_full_h ||
               fb->len < (size_t)s_full_w * s_full_h * 2)) {
        ESP_LOGE(TAG, "unexpected capture %ux%u len %u", (unsigned)fb->width, (unsigned)fb->height,
                 (unsigned)fb->len);
        esp_camera_fb_return(fb);
        fb = NULL;
    }
    if (!fb) {
        esp_camera_deinit();
        start(FRAMESIZE_QVGA);
    }
    return fb;
}

void camera_capture_done(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
    esp_camera_deinit();
    start(FRAMESIZE_QVGA);
}

void camera_power(bool on)
{
    gpio_set_level(PIN_PWDN, on ? 0 : 1);
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
