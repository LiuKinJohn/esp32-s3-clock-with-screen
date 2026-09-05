#include "app_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NAMESPACE = "clock";

esp_err_t app_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_settings_defaults(app_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->language = APP_LANG_ZH;
    settings->theme = APP_THEME_LIGHT;
    settings->brightness = 70;
    settings->sleep_minutes = 0;
    settings->ntp_interval_hours = 6;
    settings->timezone_index = 0;
    settings->hour_24 = true;
    settings->ntp_enabled = true;
    settings->eta_station = APP_ETA_STATION_ADMIRALTY;
    settings->eta_direction_admiralty = APP_ETA_DIRECTION_PRIMARY;
    settings->eta_direction_sheung_shui = APP_ETA_DIRECTION_PRIMARY;
    settings->clock_style = APP_CLOCK_STYLE_STAGGERED;
    settings->flight_radius_km = 50;
    settings->flight_max_aircraft = 8;
    settings->flight_label_size = APP_FLIGHT_LABEL_LARGER;
    settings->settings_version = 1;
}

esp_err_t app_settings_load(app_settings_t *settings)
{
    app_settings_defaults(settings);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    app_settings_t stored;
    app_settings_defaults(&stored);
    size_t size = 0;
    err = nvs_get_blob(nvs, "settings", NULL, &size);
    if (err == ESP_OK && size <= sizeof(stored)) {
        uint8_t raw[sizeof(stored)] = {0};
        size_t read_size = size;
        err = nvs_get_blob(nvs, "settings", raw, &read_size);
        if (err == ESP_OK) {
            memcpy(&stored, raw, read_size);
            if (read_size < sizeof(stored)) {
                stored.flight_label_size = APP_FLIGHT_LABEL_LARGER;
                stored.settings_version = 1;
            }
        }
    }
    nvs_close(nvs);
    if (err == ESP_OK && size <= sizeof(stored)) {
        *settings = stored;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Settings load failed: %s", esp_err_to_name(err));
    }

    if (settings->language > APP_LANG_EN || settings->theme > APP_THEME_AUTO ||
        settings->brightness > 100 || settings->ntp_interval_hours == 0 ||
        settings->eta_station > APP_ETA_STATION_SHEUNG_SHUI ||
        settings->eta_direction_admiralty > APP_ETA_DIRECTION_REVERSE ||
        settings->eta_direction_sheung_shui > APP_ETA_DIRECTION_REVERSE ||
        settings->clock_style > APP_CLOCK_STYLE_BLUE_OVERLAP ||
        (settings->flight_radius_km != 20 && settings->flight_radius_km != 50 &&
         settings->flight_radius_km != 70 && settings->flight_radius_km != 100 &&
         settings->flight_radius_km != 120 && settings->flight_radius_km != 200) ||
        (settings->flight_max_aircraft != 5 && settings->flight_max_aircraft != 8 &&
         settings->flight_max_aircraft != 10 && settings->flight_max_aircraft != 13 &&
         settings->flight_max_aircraft != 15) ||
        settings->flight_label_size >= APP_FLIGHT_LABEL_SIZE_COUNT ||
        settings->settings_version != 1) {
        app_settings_defaults(settings);
    }
    return ESP_OK;
}

esp_err_t app_settings_save(const app_settings_t *settings)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "settings", settings, sizeof(*settings));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    return err;
}
