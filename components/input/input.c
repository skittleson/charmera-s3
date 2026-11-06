#include "input.h"

#include <math.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* QMI8658 */
#define IMU_ADDR      0x6B
#define IMU_WHO_AM_I  0x00
#define IMU_CTRL1     0x02
#define IMU_CTRL2     0x03
#define IMU_CTRL7     0x08
#define IMU_AX_L      0x35
#define IMU_ID        0x05

/* Portrait grip, USB-C down: gravity runs along the accelerometer's X axis and a
   left/right tilt swings it into Y. (Landscape USB-right measured roll=+X over Y,
   so turning the board 90 degrees moves gravity onto X.) */
#ifndef INPUT_ROLL_AXIS
#define INPUT_ROLL_AXIS 1
#endif
#ifndef INPUT_GRAVITY_AXIS
#define INPUT_GRAVITY_AXIS 0
#endif
#ifndef INPUT_ROLL_SIGN
#define INPUT_ROLL_SIGN 1.0f
#endif

#define POLL_MS 10

static const char *TAG = "input";

static QueueHandle_t s_queue;
static i2c_master_dev_handle_t s_imu;
static volatile bool s_tilt_enabled = true;
static volatile bool s_suppress;
static volatile float s_roll;
static int16_t s_raw[3];

static esp_err_t imu_write(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_imu, b, 2, 50);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_imu, &reg, 1, out, n, 50);
}

static esp_err_t imu_init(void)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &dev, &s_imu), TAG, "imu dev");
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(imu_read(IMU_WHO_AM_I, &id, 1), TAG, "imu id");
    if (id != IMU_ID) {
        ESP_LOGW(TAG, "unexpected IMU id 0x%02x", id);
    }
    ESP_RETURN_ON_ERROR(imu_write(IMU_CTRL1, 0x40), TAG, "ctrl1"); /* auto-increment */
    ESP_RETURN_ON_ERROR(imu_write(IMU_CTRL2, 0x16), TAG, "ctrl2"); /* +-4 g, ~117 Hz */
    ESP_RETURN_ON_ERROR(imu_write(IMU_CTRL7, 0x01), TAG, "ctrl7"); /* accel on */
    return ESP_OK;
}

static bool imu_roll(float *roll_deg, int16_t raw[3])
{
    uint8_t b[6];
    if (imu_read(IMU_AX_L, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        raw[i] = (int16_t)(b[i * 2] | (b[i * 2 + 1] << 8));
    }
    const float r = (float)raw[INPUT_ROLL_AXIS];
    const float g = fabsf((float)raw[INPUT_GRAVITY_AXIS]);
    const float face = fabsf((float)raw[3 - INPUT_ROLL_AXIS - INPUT_GRAVITY_AXIS]);
    /* Lying flat or aimed straight up/down: left/right tilt has no meaning. */
    if (face > 0.7f * sqrtf(r * r + g * g + face * face)) {
        *roll_deg = 0.0f;
        return true;
    }
    *roll_deg = INPUT_ROLL_SIGN * atan2f(r, g + 1.0f) * (180.0f / (float)M_PI);
    return true;
}

static void push(input_event_t e)
{
    if (e != INPUT_NONE) {
        xQueueSend(s_queue, &e, 0);
    }
}

static void poll_task(void *arg)
{
    btn_state_t btn;
    tilt_state_t tilt;
    btn_init(&btn);
    tilt_init(&tilt);
    int16_t raw[3] = {0};
    for (;;) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_suppress) {
            s_suppress = false;
            btn_suppress(&btn);
            xQueueReset(s_queue);
        }
        push(btn_update(&btn, gpio_get_level(BOARD_PIN_BOOT) == 0, now));
        float roll;
        if (s_imu && imu_roll(&roll, raw)) {
            s_roll = roll;
            s_raw[0] = raw[0];
            s_raw[1] = raw[1];
            s_raw[2] = raw[2];
            if (s_tilt_enabled) {
                push(tilt_update(&tilt, roll, now));
            } else {
                tilt_init(&tilt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t input_init(void)
{
    s_queue = xQueueCreate(16, sizeof(input_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "boot gpio");
    if (imu_init() != ESP_OK) {
        ESP_LOGE(TAG, "IMU not available; tilt disabled");
        s_imu = NULL;
    }
    return xTaskCreatePinnedToCore(poll_task, "input", 4096, NULL, 5, NULL, 0) == pdPASS ? ESP_OK
                                                                                       : ESP_FAIL;
}

input_event_t input_get(uint32_t timeout_ms)
{
    input_event_t e;
    return xQueueReceive(s_queue, &e, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? e : INPUT_NONE;
}

void input_flush(void)
{
    xQueueReset(s_queue);
    /* Also drop a press still being timed (a short press is only reported 350 ms after
       release) and ignore a button held through the end of the work. */
    s_suppress = true;
    while (s_suppress) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void input_set_tilt_enabled(bool on)
{
    s_tilt_enabled = on;
}

float input_roll_deg(void)
{
    return s_roll;
}

void input_accel_raw(int16_t out[3])
{
    out[0] = s_raw[0];
    out[1] = s_raw[1];
    out[2] = s_raw[2];
}
