#include "board.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_10_BIT
#define BL_LEDC_MAX     ((1 << 10) - 1)
#define BL_LEDC_FREQ_HZ 10000

#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_4
#define BAT_DIVIDER     3.0f

#define SPI_MAX_TRANSFER (BOARD_LCD_W * 40 * 2 + 64)

static const char *TAG = "board";

static SemaphoreHandle_t s_spi_lock;
static i2c_master_bus_handle_t s_i2c_bus;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;

static esp_err_t init_spi(void)
{
    const spi_bus_config_t cfg = {
        .sclk_io_num = BOARD_PIN_SPI_SCLK,
        .mosi_io_num = BOARD_PIN_SPI_MOSI,
        .miso_io_num = BOARD_PIN_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER,
    };
    return spi_bus_initialize(BOARD_SPI_HOST, &cfg, SPI_DMA_CH_AUTO);
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t init_backlight(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }
    const ledc_channel_config_t ch = {
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .gpio_num = BOARD_PIN_LCD_BL,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

static esp_err_t init_battery_adc(void)
{
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = BAT_ADC_UNIT};
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) {
        return err;
    }
    const adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ch);
    if (err != ESP_OK) {
        return err;
    }
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_adc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; battery reading will be approximate");
        s_adc_cali = NULL;
    }
    return ESP_OK;
}

esp_err_t board_init(void)
{
    s_spi_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_spi_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err;
    if ((err = init_spi()) != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = init_i2c()) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = init_backlight()) != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = init_battery_adc()) != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void board_spi_lock(void)
{
    xSemaphoreTakeRecursive(s_spi_lock, portMAX_DELAY);
}

void board_spi_unlock(void)
{
    xSemaphoreGiveRecursive(s_spi_lock);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, (BL_LEDC_MAX * percent) / 100);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

float board_battery_volts(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
        return 0.0f;
    }
    int mv = 0;
    if (s_adc_cali && adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
        return (mv / 1000.0f) * BAT_DIVIDER;
    }
    return (raw / 4095.0f) * 3.3f * BAT_DIVIDER;
}

int board_battery_percent(float volts)
{
    int p = (int)((volts - 3.3f) / (4.2f - 3.3f) * 100.0f + 0.5f);
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}
