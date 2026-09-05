#include "flight_manager.h"

#include <math.h>
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

#define FLIGHT_RESPONSE_CAPACITY (512 * 1024)
#define FLIGHT_REFRESH_MS (30 * 1000)
#define FLIGHT_STALE_MS (90 * 1000)
#define EARTH_RADIUS_KM 6371.0

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} flight_http_response_t;

static const char *TAG = "flight";
static SemaphoreHandle_t status_mutex;
static flight_status_t status_data;
static volatile bool refresh_requested;
static int64_t last_attempt_ms;
static int64_t last_success_ms;

static void status_lock(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(status_mutex);
}

static bool valid_radius(uint16_t radius_km)
{
    return radius_km == 20 || radius_km == 50 || radius_km == 70 || radius_km == 100 ||
           radius_km == 120 || radius_km == 200;
}

static bool valid_limit(uint8_t max_aircraft)
{
    return max_aircraft == 5 || max_aircraft == 8 || max_aircraft == 10 ||
           max_aircraft == 13 || max_aircraft == 15;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    flight_http_response_t *response = event->user_data;
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
    flight_http_response_t response = {
        .data = heap_caps_malloc(FLIGHT_RESPONSE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .capacity = FLIGHT_RESPONSE_CAPACITY,
    };
    if (!response.data) {
        network_http_unlock();
        return ESP_ERR_NO_MEM;
    }
    response.data[0] = '\0';

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 9000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(response.data);
        network_http_unlock();
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-Flight-Compass/1.0");
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    network_http_unlock();
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        ESP_LOGW(TAG, "ADS-B request failed: err=%s status=%d bytes=%u overflow=%d",
                 esp_err_to_name(err), status_code, (unsigned)response.length, response.overflow);
        heap_caps_free(response.data);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    *json_out = response.data;
    return ESP_OK;
}

static double radians(double degrees)
{
    return degrees * M_PI / 180.0;
}

static float aircraft_distance_km(double center_latitude, double center_longitude,
                                  double latitude, double longitude)
{
    double lat1 = radians(center_latitude);
    double lat2 = radians(latitude);
    double dlat = lat2 - lat1;
    double dlon = radians(longitude - center_longitude);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1) * cos(lat2) * sin(dlon / 2.0) * sin(dlon / 2.0);
    return (float)(EARTH_RADIUS_KM * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
}

static float aircraft_bearing_deg(double center_latitude, double center_longitude,
                                  double latitude, double longitude)
{
    double lat1 = radians(center_latitude);
    double lat2 = radians(latitude);
    double dlon = radians(longitude - center_longitude);
    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);
    double bearing = atan2(y, x) * 180.0 / M_PI;
    if (bearing < 0.0) bearing += 360.0;
    return (float)bearing;
}

static void trim_text(char *text)
{
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == ' ') text[--length] = '\0';
    size_t leading = 0;
    while (text[leading] == ' ') leading++;
    if (leading > 0) memmove(text, text + leading, strlen(text + leading) + 1);
}

static bool json_number(const cJSON *object, const char *name, double *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) return false;
    *value = item->valuedouble;
    return true;
}

static void insert_nearest(flight_aircraft_t *aircraft, uint8_t *count, uint8_t limit,
                           const flight_aircraft_t *candidate)
{
    uint8_t used = *count;
    if (used == limit && candidate->distance_km >= aircraft[used - 1].distance_km) return;
    uint8_t position = 0;
    while (position < used && aircraft[position].distance_km <= candidate->distance_km) position++;
    uint8_t new_count = used < limit ? used + 1 : used;
    if (position < new_count) {
        size_t move_count = new_count - position - 1;
        if (move_count > 0) {
            memmove(&aircraft[position + 1], &aircraft[position],
                    move_count * sizeof(flight_aircraft_t));
        }
        aircraft[position] = *candidate;
    }
    *count = new_count;
}

static esp_err_t update_flights(double center_latitude, double center_longitude,
                                uint16_t radius_km, uint8_t max_aircraft)
{
    unsigned radius_nm = (unsigned)ceil((double)radius_km / 1.852);
    char url[160];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.5f/%.5f/%u",
             center_latitude, center_longitude, radius_nm);

    char *json = NULL;
    esp_err_t err = fetch_json(url, &json);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(json);
    heap_caps_free(json);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "ac");
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    flight_aircraft_t nearest[FLIGHT_MAX_AIRCRAFT] = {0};
    uint8_t nearest_count = 0;
    uint16_t total_airborne = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        double latitude;
        double longitude;
        double altitude;
        if (!json_number(entry, "lat", &latitude) || !json_number(entry, "lon", &longitude) ||
            !json_number(entry, "alt_baro", &altitude)) {
            continue;
        }
        float distance = aircraft_distance_km(center_latitude, center_longitude, latitude, longitude);
        if (distance > radius_km) continue;
        total_airborne++;

        flight_aircraft_t candidate = {
            .altitude_ft = (int32_t)lround(altitude),
            .distance_km = distance,
            .bearing_deg = aircraft_bearing_deg(center_latitude, center_longitude, latitude, longitude),
        };
        const cJSON *hex = cJSON_GetObjectItemCaseSensitive(entry, "hex");
        if (cJSON_IsString(hex)) strlcpy(candidate.hex, hex->valuestring, sizeof(candidate.hex));
        const cJSON *flight = cJSON_GetObjectItemCaseSensitive(entry, "flight");
        const cJSON *registration = cJSON_GetObjectItemCaseSensitive(entry, "r");
        if (cJSON_IsString(flight) && flight->valuestring[0]) {
            strlcpy(candidate.callsign, flight->valuestring, sizeof(candidate.callsign));
        } else if (cJSON_IsString(registration) && registration->valuestring[0]) {
            strlcpy(candidate.callsign, registration->valuestring, sizeof(candidate.callsign));
        } else {
            strlcpy(candidate.callsign, candidate.hex[0] ? candidate.hex : "UNKNOWN",
                    sizeof(candidate.callsign));
        }
        trim_text(candidate.callsign);
        const cJSON *aircraft_type = cJSON_GetObjectItemCaseSensitive(entry, "t");
        if (!cJSON_IsString(aircraft_type)) {
            aircraft_type = cJSON_GetObjectItemCaseSensitive(entry, "type");
        }
        if (cJSON_IsString(aircraft_type) && aircraft_type->valuestring[0]) {
            strlcpy(candidate.aircraft_type, aircraft_type->valuestring,
                    sizeof(candidate.aircraft_type));
            trim_text(candidate.aircraft_type);
        }
        if (!candidate.aircraft_type[0]) {
            strlcpy(candidate.aircraft_type, "----", sizeof(candidate.aircraft_type));
        }

        double value;
        if (json_number(entry, "gs", &value)) {
            candidate.speed_kts = (float)value;
            candidate.speed_valid = true;
        }
        if (json_number(entry, "track", &value) || json_number(entry, "true_heading", &value) ||
            json_number(entry, "mag_heading", &value)) {
            candidate.heading_deg = (float)value;
            candidate.heading_valid = true;
        }
        if (json_number(entry, "baro_rate", &value) || json_number(entry, "geom_rate", &value)) {
            candidate.vertical_rate_fpm = (int32_t)lround(value);
            candidate.vertical_rate_valid = true;
        }
        insert_nearest(nearest, &nearest_count, max_aircraft, &candidate);
    }
    cJSON_Delete(root);

    network_status_t network;
    network_manager_get_status(&network);
    if (!network.coordinates_valid || fabs(network.latitude - center_latitude) > 0.00001 ||
        fabs(network.longitude - center_longitude) > 0.00001) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now;
    time(&now);
    status_lock();
    status_data.state = FLIGHT_STATE_READY;
    status_data.data_valid = true;
    status_data.total_aircraft = total_airborne;
    status_data.aircraft_count = nearest_count;
    memcpy(status_data.aircraft, nearest, sizeof(nearest));
    status_data.updated = now;
    status_data.generation++;
    last_success_ms = esp_timer_get_time() / 1000;
    status_unlock();
    ESP_LOGI(TAG, "ADS-B updated: radius=%u km total=%u shown=%u", radius_km,
             total_airborne, nearest_count);
    return ESP_OK;
}

static void flight_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        network_status_t network;
        network_manager_get_status(&network);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (!network.coordinates_valid) {
            status_lock();
            if (status_data.state != FLIGHT_STATE_WAITING_LOCATION || status_data.data_valid) {
                status_data.state = FLIGHT_STATE_WAITING_LOCATION;
                status_data.data_valid = false;
                status_data.aircraft_count = 0;
                status_data.total_aircraft = 0;
                status_data.generation++;
            }
            status_unlock();
        } else if (network.state == NETWORK_STATE_CONNECTED &&
                   (refresh_requested || last_attempt_ms == 0 ||
                    now_ms - last_attempt_ms >= FLIGHT_REFRESH_MS)) {
            refresh_requested = false;
            last_attempt_ms = now_ms;
            status_lock();
            uint16_t radius_km = status_data.radius_km;
            uint8_t max_aircraft = status_data.max_aircraft;
            status_data.state = FLIGHT_STATE_LOADING;
            status_data.refreshing = true;
            status_data.generation++;
            status_unlock();
            esp_err_t err = update_flights(network.latitude, network.longitude, radius_km, max_aircraft);
            status_lock();
            status_data.refreshing = false;
            if (err != ESP_OK) {
                status_data.state = FLIGHT_STATE_ERROR;
                status_data.generation++;
            }
            status_unlock();
            if (err != ESP_OK) ESP_LOGW(TAG, "Unable to update ADS-B data: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t flight_manager_init(uint16_t radius_km, uint8_t max_aircraft)
{
    status_mutex = xSemaphoreCreateMutex();
    if (!status_mutex) return ESP_ERR_NO_MEM;
    memset(&status_data, 0, sizeof(status_data));
    status_data.state = FLIGHT_STATE_WAITING_LOCATION;
    status_data.radius_km = valid_radius(radius_km) ? radius_km : 50;
    status_data.max_aircraft = valid_limit(max_aircraft) ? max_aircraft : 8;
    BaseType_t created = xTaskCreate(flight_task, "flight_adsb", 12288, NULL, 4, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void flight_manager_get_status(flight_status_t *status)
{
    if (!status || !status_mutex) return;
    status_lock();
    *status = status_data;
    int64_t success_ms = last_success_ms;
    status_unlock();
    int64_t now_ms = esp_timer_get_time() / 1000;
    status->data_stale = status->data_valid &&
                         (success_ms == 0 || now_ms - success_ms > FLIGHT_STALE_MS);
}

void flight_manager_set_config(uint16_t radius_km, uint8_t max_aircraft)
{
    if (!status_mutex || !valid_radius(radius_km) || !valid_limit(max_aircraft)) return;
    status_lock();
    bool changed = status_data.radius_km != radius_km || status_data.max_aircraft != max_aircraft;
    status_data.radius_km = radius_km;
    status_data.max_aircraft = max_aircraft;
    if (status_data.aircraft_count > max_aircraft) status_data.aircraft_count = max_aircraft;
    if (changed) status_data.generation++;
    status_unlock();
    if (changed) refresh_requested = true;
}

void flight_manager_refresh(void)
{
    refresh_requested = true;
}
