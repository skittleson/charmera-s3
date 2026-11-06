#include "display.h"

#include "board.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

/* Portrait 240x320, held with USB-C at the bottom. */
#define ORIENT_SWAP_XY  false
#define ORIENT_MIRROR_X false
#define ORIENT_MIRROR_Y false

#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define DRAW_BUF_LINES     40

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

static esp_err_t init_panel(void)
{
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_PIN_LCD_DC,
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");
    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(init_panel(), TAG, "panel");

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_W * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_W,
        .vres = BOARD_LCD_H,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = ORIENT_SWAP_XY,
            .mirror_x = ORIENT_MIRROR_X,
            .mirror_y = ORIENT_MIRROR_Y,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool display_lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms < 0 ? 0 : timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
