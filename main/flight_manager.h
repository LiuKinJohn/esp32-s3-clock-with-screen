#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define FLIGHT_MAX_AIRCRAFT 15

typedef enum {
    FLIGHT_STATE_WAITING_LOCATION = 0,
    FLIGHT_STATE_LOADING,
    FLIGHT_STATE_READY,
    FLIGHT_STATE_ERROR,
} flight_state_t;

typedef struct {
    char hex[9];
    char callsign[12];
    char aircraft_type[8];
    int32_t altitude_ft;
    int32_t vertical_rate_fpm;
    float speed_kts;
    float heading_deg;
    float distance_km;
    float bearing_deg;
    bool speed_valid;
    bool heading_valid;
    bool vertical_rate_valid;
} flight_aircraft_t;

typedef struct {
    flight_state_t state;
    bool data_valid;
    bool data_stale;
    bool refreshing;
    uint16_t radius_km;
    uint8_t max_aircraft;
    uint16_t total_aircraft;
    uint8_t aircraft_count;
    time_t updated;
    uint32_t generation;
    flight_aircraft_t aircraft[FLIGHT_MAX_AIRCRAFT];
} flight_status_t;

esp_err_t flight_manager_init(uint16_t radius_km, uint8_t max_aircraft);
void flight_manager_get_status(flight_status_t *status);
void flight_manager_set_config(uint16_t radius_km, uint8_t max_aircraft);
void flight_manager_refresh(void);
