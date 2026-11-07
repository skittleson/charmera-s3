#include "settings.h"

#include "lapse_logic.h"
#include "orient.h"
#include "nvs.h"

#define NS "charmera"

static uint8_t s_brightness = 80;
static uint8_t s_look = LOOK_NORMAL;
static uint8_t s_level;
static uint8_t s_ble = 1;
static uint8_t s_orient; /* portrait */
static uint8_t s_lapse = LAPSE_INTERVAL_DEFAULT;

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bright", &s_brightness);
        nvs_get_u8(h, "look", &s_look);
        nvs_get_u8(h, "level", &s_level);
        nvs_get_u8(h, "ble", &s_ble);
        nvs_get_u8(h, "orient", &s_orient);
        nvs_get_u8(h, "lapse", &s_lapse);
        nvs_close(h);
    }
    if (s_brightness < 10 || s_brightness > 100) {
        s_brightness = 80;
    }
    if (s_look >= LOOK_COUNT) {
        s_look = LOOK_NORMAL;
    }
    if (s_orient >= ORIENT_SETTING_COUNT) {
        s_orient = ORIENT_PORTRAIT;
    }
    if (s_lapse >= LAPSE_INTERVAL_COUNT) {
        s_lapse = LAPSE_INTERVAL_DEFAULT;
    }
}

uint8_t settings_brightness(void)
{
    return s_brightness;
}

void settings_set_brightness(uint8_t pct)
{
    if (pct < 10) {
        pct = 10;
    } else if (pct > 100) {
        pct = 100;
    }
    s_brightness = pct;
    save_u8("bright", pct);
}

bool settings_level(void)
{
    return s_level != 0;
}

void settings_set_level(bool on)
{
    s_level = on ? 1 : 0;
    save_u8("level", s_level);
}

look_t settings_look(void)
{
    return (look_t)s_look;
}

void settings_set_look(look_t l)
{
    s_look = (uint8_t)l;
    save_u8("look", s_look);
}

int settings_lapse_interval(void)
{
    return s_lapse;
}

void settings_set_lapse_interval(int index)
{
    s_lapse = (index >= 0 && index < LAPSE_INTERVAL_COUNT) ? (uint8_t)index : LAPSE_INTERVAL_DEFAULT;
    save_u8("lapse", s_lapse);
}

bool settings_bluetooth(void)
{
    return s_ble != 0;
}

void settings_set_bluetooth(bool on)
{
    s_ble = on ? 1 : 0;
    save_u8("ble", s_ble);
}

int settings_orientation(void)
{
    return s_orient;
}

void settings_set_orientation(int setting)
{
    s_orient = (setting >= 0 && setting < ORIENT_SETTING_COUNT) ? (uint8_t)setting : ORIENT_PORTRAIT;
    save_u8("orient", s_orient);
}
