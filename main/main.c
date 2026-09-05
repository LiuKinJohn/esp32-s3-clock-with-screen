/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "app_init.h"
#include "app_settings.h"
#include "clock_ui.h"
#include "esp_log.h"
#include "esp_system.h"
#include "flight_manager.h"
#include "lvgl_port.h"
#include "map_manager.h"
#include "network_manager.h"
#include "network_http_lock.h"
#include "transit_manager.h"

static const char *TAG = "clock";

void app_main(void)
{
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    app_settings_init();
    app_settings_t settings;
    app_settings_load(&settings);

    ESP_ERROR_CHECK(network_http_lock_init());
    network_manager_init(&settings);
    app_init();
    ESP_ERROR_CHECK(transit_manager_init());
    ESP_ERROR_CHECK(flight_manager_init(settings.flight_radius_km, settings.flight_max_aircraft));
    ESP_ERROR_CHECK(map_manager_init(settings.flight_radius_km, settings.language == APP_LANG_EN));
    uint8_t eta_direction = settings.eta_station == APP_ETA_STATION_SHEUNG_SHUI
                                ? settings.eta_direction_sheung_shui
                                : settings.eta_direction_admiralty;
    transit_manager_set_route((transit_station_t)settings.eta_station,
                              (transit_direction_t)eta_direction);

    if (lvgl_port_lock(-1))
    {
        clock_ui_start(&settings);
        lvgl_port_unlock();
    }
}
