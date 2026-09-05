#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "app_settings.h"
#include "esp_err.h"

#define NETWORK_MAX_APS 12

typedef enum {
    NETWORK_STATE_IDLE = 0,
    NETWORK_STATE_SCANNING,
    NETWORK_STATE_CONNECTING,
    NETWORK_STATE_CONNECTED,
    NETWORK_STATE_ERROR,
} network_state_t;

typedef enum {
    NETWORK_LOCATION_NONE = 0,
    NETWORK_LOCATION_LOCATING,
    NETWORK_LOCATION_AVAILABLE,
    NETWORK_LOCATION_UNAVAILABLE,
} network_location_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} network_ap_t;

typedef struct {
    network_state_t state;
    bool has_credentials;
    bool portal_running;
    bool time_synchronized;
    char ssid[33];
    char ip[16];
    char error[64];
    time_t last_sync;
    time_t next_sync;
    network_location_state_t location_state;
    bool location_updating;
    char location_zh[96];
    char location_en[96];
    bool coordinates_valid;
    bool coordinates_manual;
    double latitude;
    double longitude;
    uint32_t location_generation;
    uint32_t scan_generation;
    size_t ap_count;
    network_ap_t aps[NETWORK_MAX_APS];
} network_status_t;

esp_err_t network_manager_init(const app_settings_t *settings);
void network_manager_get_status(network_status_t *status);
esp_err_t network_manager_scan(void);
esp_err_t network_manager_connect(const char *ssid, const char *password);
esp_err_t network_manager_forget(void);
esp_err_t network_manager_start_portal(void);
esp_err_t network_manager_stop_portal(void);
void network_manager_set_time_config(bool enabled, uint8_t interval_hours, uint8_t timezone_index);
void network_manager_force_sync(void);
esp_err_t network_manager_set_manual_coordinates(double latitude, double longitude);

const char *network_manager_timezone_name(uint8_t index, bool chinese);
size_t network_manager_timezone_count(void);
