#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_LANG_ZH = 0,
    APP_LANG_EN,
} app_language_t;

typedef enum {
    APP_THEME_LIGHT = 0,
    APP_THEME_DARK,
    APP_THEME_AUTO,
} app_theme_t;

typedef enum {
    APP_ETA_STATION_ADMIRALTY = 0,
    APP_ETA_STATION_SHEUNG_SHUI,
} app_eta_station_t;

typedef enum {
    APP_ETA_DIRECTION_PRIMARY = 0,
    APP_ETA_DIRECTION_REVERSE,
} app_eta_direction_t;

typedef enum {
    APP_CLOCK_STYLE_STAGGERED = 0,
    APP_CLOCK_STYLE_BLUE_OVERLAP,
} app_clock_style_t;

typedef enum {
    APP_FLIGHT_LABEL_LARGE = 0,
    APP_FLIGHT_LABEL_LARGER,
    APP_FLIGHT_LABEL_MEDIUM,
    APP_FLIGHT_LABEL_SMALLER,
    APP_FLIGHT_LABEL_SMALL,
    APP_FLIGHT_LABEL_SIZE_COUNT,
} app_flight_label_size_t;

typedef struct {
    uint8_t language;
    uint8_t theme;
    uint8_t brightness;
    uint8_t sleep_minutes;
    uint8_t ntp_interval_hours;
    uint8_t timezone_index;
    bool hour_24;
    bool ntp_enabled;
    uint8_t eta_station;
    uint8_t eta_direction_admiralty;
    uint8_t eta_direction_sheung_shui;
    uint8_t clock_style;
    uint16_t flight_radius_km;
    uint8_t flight_max_aircraft;
    uint8_t flight_label_size;
    uint8_t settings_version;
} app_settings_t;

esp_err_t app_settings_init(void);
void app_settings_defaults(app_settings_t *settings);
esp_err_t app_settings_load(app_settings_t *settings);
esp_err_t app_settings_save(const app_settings_t *settings);
