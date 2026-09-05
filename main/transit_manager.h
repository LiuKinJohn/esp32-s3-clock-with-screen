#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define TRANSIT_MAX_TRAINS 4

typedef enum {
    TRANSIT_STATION_ADMIRALTY = 0,
    TRANSIT_STATION_SHEUNG_SHUI,
} transit_station_t;

typedef enum {
    TRANSIT_DIRECTION_PRIMARY = 0,
    TRANSIT_DIRECTION_REVERSE,
} transit_direction_t;

typedef enum {
    TRANSIT_DESTINATION_UNKNOWN = 0,
    TRANSIT_DESTINATION_KENNEDY_TOWN,
    TRANSIT_DESTINATION_LO_WU,
    TRANSIT_DESTINATION_LOK_MA_CHAU,
    TRANSIT_DESTINATION_CHAI_WAN,
    TRANSIT_DESTINATION_ADMIRALTY,
} transit_destination_t;

typedef struct {
    uint8_t station;
    uint8_t direction;
    bool eta_valid;
    bool eta_stale;
    bool eta_refreshing;
    uint8_t train_count;
    int16_t eta_minutes[TRANSIT_MAX_TRAINS];
    uint8_t platforms[TRANSIT_MAX_TRAINS];
    uint8_t destinations[TRANSIT_MAX_TRAINS];
    time_t eta_updated;
    bool weather_valid;
    int16_t temperature_c;
    uint8_t weather_icon;
    time_t weather_updated;
    uint32_t generation;
} transit_status_t;

esp_err_t transit_manager_init(void);
void transit_manager_get_status(transit_status_t *status);
void transit_manager_refresh(void);
void transit_manager_set_route(transit_station_t station, transit_direction_t direction);
