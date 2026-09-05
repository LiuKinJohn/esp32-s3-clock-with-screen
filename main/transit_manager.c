#include "transit_manager.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "network_http_lock.h"

#define ETA_URL_ADMIRALTY "https://rt.data.gov.hk/v1/transport/mtr/getSchedule.php?line=ISL&sta=ADM&lang=TC"
#define ETA_URL_SHEUNG_SHUI "https://rt.data.gov.hk/v1/transport/mtr/getSchedule.php?line=EAL&sta=SHS&lang=TC"
#define WEATHER_URL "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=tc"
#define HTTP_RESPONSE_CAPACITY (16 * 1024)
#define ETA_REFRESH_MS (30 * 1000)
#define ETA_STALE_MS (90 * 1000)
#define WEATHER_REFRESH_MS (10 * 60 * 1000)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} http_response_t;

static const char *TAG = "transit";
static SemaphoreHandle_t status_mutex;
static transit_status_t status_data;
static transit_station_t active_station;
static transit_direction_t active_direction;
static volatile bool refresh_requested;
static int64_t last_eta_attempt_ms;
static int64_t last_eta_success_ms;
static int64_t last_weather_attempt_ms;

static void status_lock(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(status_mutex);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    http_response_t *response = event->user_data;
    size_t available = response->capacity - response->length - 1;
    if ((size_t)event->data_len > available) {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_json(const char *url, char **json_out)
{
    if (!network_http_lock(pdMS_TO_TICKS(15000))) return ESP_ERR_TIMEOUT;
    http_response_t response = {
        .data = heap_caps_malloc(HTTP_RESPONSE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .capacity = HTTP_RESPONSE_CAPACITY,
    };
    if (response.data == NULL) {
        network_http_unlock();
        return ESP_ERR_NO_MEM;
    }
    response.data[0] = '\0';

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 7000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(response.data);
        network_http_unlock();
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    network_http_unlock();
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        ESP_LOGW(TAG, "HTTP request failed: err=%s status=%d bytes=%u", esp_err_to_name(err),
                 status_code, (unsigned)response.length);
        heap_caps_free(response.data);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    *json_out = response.data;
    return ESP_OK;
}

static transit_destination_t parse_destination(const char *code)
{
    if (code == NULL) return TRANSIT_DESTINATION_UNKNOWN;
    if (strcmp(code, "KET") == 0) return TRANSIT_DESTINATION_KENNEDY_TOWN;
    if (strcmp(code, "LOW") == 0) return TRANSIT_DESTINATION_LO_WU;
    if (strcmp(code, "LMC") == 0) return TRANSIT_DESTINATION_LOK_MA_CHAU;
    if (strcmp(code, "CHW") == 0) return TRANSIT_DESTINATION_CHAI_WAN;
    if (strcmp(code, "ADM") == 0) return TRANSIT_DESTINATION_ADMIRALTY;
    return TRANSIT_DESTINATION_UNKNOWN;
}

static esp_err_t update_eta(void)
{
    status_lock();
    transit_station_t requested_station = active_station;
    transit_direction_t requested_direction = active_direction;
    status_unlock();
    const char *url = requested_station == TRANSIT_STATION_SHEUNG_SHUI
                          ? ETA_URL_SHEUNG_SHUI : ETA_URL_ADMIRALTY;
    const char *station_key = requested_station == TRANSIT_STATION_SHEUNG_SHUI
                                  ? "EAL-SHS" : "ISL-ADM";
    const char *direction_key;
    if (requested_station == TRANSIT_STATION_SHEUNG_SHUI) {
        direction_key = requested_direction == TRANSIT_DIRECTION_PRIMARY ? "UP" : "DOWN";
    } else {
        direction_key = requested_direction == TRANSIT_DIRECTION_PRIMARY ? "DOWN" : "UP";
    }

    char *json = NULL;
    esp_err_t err = fetch_json(url, &json);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *api_status = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *station = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, station_key) : NULL;
    cJSON *direction = cJSON_IsObject(station) ? cJSON_GetObjectItemCaseSensitive(station, direction_key) : NULL;
    bool valid_response = cJSON_IsNumber(api_status) && api_status->valueint == 1 && cJSON_IsArray(direction);
    if (!valid_response) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int16_t minutes[TRANSIT_MAX_TRAINS] = {0};
    uint8_t platforms[TRANSIT_MAX_TRAINS] = {0};
    uint8_t destinations[TRANSIT_MAX_TRAINS] = {0};
    uint8_t count = 0;
    cJSON *train = NULL;
    cJSON_ArrayForEach(train, direction) {
        if (count >= TRANSIT_MAX_TRAINS) {
            break;
        }
        cJSON *is_valid = cJSON_GetObjectItemCaseSensitive(train, "valid");
        cJSON *ttnt = cJSON_GetObjectItemCaseSensitive(train, "ttnt");
        cJSON *platform = cJSON_GetObjectItemCaseSensitive(train, "plat");
        cJSON *destination = cJSON_GetObjectItemCaseSensitive(train, "dest");
        if (!cJSON_IsString(is_valid) || strcmp(is_valid->valuestring, "Y") != 0 || !cJSON_IsString(ttnt)) {
            continue;
        }
        minutes[count] = (int16_t)atoi(ttnt->valuestring);
        platforms[count] = cJSON_IsString(platform) ? (uint8_t)atoi(platform->valuestring) : 0;
        destinations[count] = cJSON_IsString(destination)
                                  ? (uint8_t)parse_destination(destination->valuestring)
                                  : TRANSIT_DESTINATION_UNKNOWN;
        count++;
    }
    cJSON_Delete(root);

    time_t now;
    time(&now);
    status_lock();
    if (active_station != requested_station || active_direction != requested_direction) {
        status_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    status_data.eta_valid = true;
    status_data.train_count = count;
    memcpy(status_data.eta_minutes, minutes, sizeof(minutes));
    memcpy(status_data.platforms, platforms, sizeof(platforms));
    memcpy(status_data.destinations, destinations, sizeof(destinations));
    status_data.eta_updated = now;
    status_data.generation++;
    last_eta_success_ms = esp_timer_get_time() / 1000;
    status_unlock();
    ESP_LOGI(TAG, "MTR ETA updated: station=%s direction=%s trains=%u", station_key, direction_key, count);
    return ESP_OK;
}

static esp_err_t update_weather(void)
{
    char *json = NULL;
    esp_err_t err = fetch_json(WEATHER_URL, &json);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    int icon_value = 0;
    cJSON *icons = cJSON_GetObjectItemCaseSensitive(root, "icon");
    cJSON *first_icon = cJSON_IsArray(icons) ? cJSON_GetArrayItem(icons, 0) : NULL;
    if (cJSON_IsNumber(first_icon)) {
        icon_value = first_icon->valueint;
    }

    bool found_temperature = false;
    int temperature = 0;
    cJSON *temperature_obj = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    cJSON *temperature_data = cJSON_IsObject(temperature_obj)
                                  ? cJSON_GetObjectItemCaseSensitive(temperature_obj, "data") : NULL;
    cJSON *fallback_entry = NULL;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, temperature_data) {
        cJSON *place = cJSON_GetObjectItemCaseSensitive(entry, "place");
        cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "value");
        if (!cJSON_IsString(place) || !cJSON_IsNumber(value)) {
            continue;
        }
        if (fallback_entry == NULL || strcmp(place->valuestring, "香港天文台") == 0) {
            fallback_entry = entry;
        }
        if (strcmp(place->valuestring, "香港公園") == 0) {
            temperature = value->valueint;
            found_temperature = true;
            break;
        }
    }
    if (!found_temperature && fallback_entry != NULL) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(fallback_entry, "value");
        temperature = value->valueint;
        found_temperature = true;
    }
    cJSON_Delete(root);
    if (!found_temperature || icon_value <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    time_t now;
    time(&now);
    status_lock();
    status_data.weather_valid = true;
    status_data.temperature_c = (int16_t)temperature;
    status_data.weather_icon = (uint8_t)icon_value;
    status_data.weather_updated = now;
    status_data.generation++;
    status_unlock();
    ESP_LOGI(TAG, "HKO weather updated: %d C, icon %d", temperature, icon_value);
    return ESP_OK;
}

static void transit_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    while (true) {
        network_status_t network;
        network_manager_get_status(&network);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (network.state == NETWORK_STATE_CONNECTED) {
            bool fetch_eta = refresh_requested || last_eta_attempt_ms == 0 ||
                             now_ms - last_eta_attempt_ms >= ETA_REFRESH_MS;
            bool fetch_weather = last_weather_attempt_ms == 0 ||
                                 now_ms - last_weather_attempt_ms >= WEATHER_REFRESH_MS;
            if (fetch_eta) {
                refresh_requested = false;
                last_eta_attempt_ms = now_ms;
                status_lock();
                status_data.eta_refreshing = true;
                status_data.generation++;
                status_unlock();
                esp_err_t err = update_eta();
                status_lock();
                status_data.eta_refreshing = false;
                status_data.generation++;
                status_unlock();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Unable to update MTR ETA: %s", esp_err_to_name(err));
                }
            }
            now_ms = esp_timer_get_time() / 1000;
            if (fetch_weather) {
                last_weather_attempt_ms = now_ms;
                esp_err_t err = update_weather();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Unable to update HKO weather: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t transit_manager_init(void)
{
    status_mutex = xSemaphoreCreateMutex();
    if (status_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&status_data, 0, sizeof(status_data));
    active_station = TRANSIT_STATION_ADMIRALTY;
    active_direction = TRANSIT_DIRECTION_PRIMARY;
    status_data.station = active_station;
    status_data.direction = active_direction;
    BaseType_t created = xTaskCreate(transit_task, "transit", 8192, NULL, 4, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void transit_manager_get_status(transit_status_t *status)
{
    if (status == NULL || status_mutex == NULL) {
        return;
    }
    status_lock();
    *status = status_data;
    int64_t eta_success_ms = last_eta_success_ms;
    status_unlock();
    int64_t now_ms = esp_timer_get_time() / 1000;
    status->eta_stale = status->eta_valid &&
                        (eta_success_ms == 0 || now_ms - eta_success_ms > ETA_STALE_MS);
}

void transit_manager_refresh(void)
{
    refresh_requested = true;
}

void transit_manager_set_route(transit_station_t station, transit_direction_t direction)
{
    if (status_mutex == NULL || station > TRANSIT_STATION_SHEUNG_SHUI ||
        direction > TRANSIT_DIRECTION_REVERSE) return;
    status_lock();
    if (active_station != station || active_direction != direction) {
        active_station = station;
        active_direction = direction;
        status_data.station = station;
        status_data.direction = direction;
        status_data.eta_valid = false;
        status_data.eta_stale = false;
        status_data.eta_refreshing = true;
        status_data.train_count = 0;
        memset(status_data.eta_minutes, 0, sizeof(status_data.eta_minutes));
        memset(status_data.platforms, 0, sizeof(status_data.platforms));
        memset(status_data.destinations, 0, sizeof(status_data.destinations));
        status_data.eta_updated = 0;
        status_data.generation++;
        refresh_requested = true;
    }
    status_unlock();
}
