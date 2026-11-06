#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define BOARD_LCD_W 320
#define BOARD_LCD_H 240

/* Pins (Waveshare ESP32-S3-Touch-LCD-2) */
#define BOARD_SPI_HOST      SPI2_HOST
#define BOARD_PIN_SPI_SCLK  39
#define BOARD_PIN_SPI_MOSI  38
#define BOARD_PIN_SPI_MISO  40
#define BOARD_PIN_LCD_CS    45
#define BOARD_PIN_LCD_DC    42
#define BOARD_PIN_LCD_BL    1
#define BOARD_PIN_SD_CS     41
#define BOARD_PIN_I2C_SDA   48
#define BOARD_PIN_I2C_SCL   47
#define BOARD_PIN_BOOT      0

esp_err_t board_init(void);

/* Recursive lock shared by everything on the SPI2 bus (LCD and SD card). */
void board_spi_lock(void);
void board_spi_unlock(void);

i2c_master_bus_handle_t board_i2c_bus(void);

void  board_backlight_set(uint8_t percent);
float board_battery_volts(void);
