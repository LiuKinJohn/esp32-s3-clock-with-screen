#include "map_manager.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "libs/tjpgd/tjpgd.h"
#include "network_http_lock.h"
#include "network_manager.h"

#define MAP_RESPONSE_CAPACITY (384 * 1024)
#define MAP_LABEL_RESPONSE_CAPACITY (48 * 1024)
#define MAP_RETRY_MS (5 * 60 * 1000)
#define MAP_ERROR_RETRY_MS (30 * 1000)
#define MAP_START_DELAY_MS 12000
#define EARTH_RADIUS_KM 6371.0
#define MAP_DETAIL_RADIUS_KM 30
#define MAP_DETAIL_SOURCE_WIDTH 1280
#define MAP_DETAIL_SOURCE_HEIGHT 1066
#define MAP_DETAIL_ZOOM 12
#define MAP_LABEL_CANDIDATES 24

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    bool overflow;
} map_http_response_t;

typedef struct {
    const uint8_t *jpeg;
    size_t jpeg_size;
    size_t offset;
    uint16_t *pixels;
    uint16_t pixel_width;
    uint16_t pixel_height;
    bool enhance_details;
} map_decode_context_t;

typedef struct {
    map_label_t label;
    uint8_t priority;
    double distance;
} map_label_candidate_t;

static const char *TAG = "map";
static SemaphoreHandle_t status_mutex;
static map_status_t status_data;
static uint8_t *image_data;
static double loaded_latitude;
static double loaded_longitude;
static uint16_t requested_radius_km;
static bool requested_english;
static int64_t last_attempt_ms;
static volatile bool refresh_requested;

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

static double radians(double degrees)
{
    return degrees * M_PI / 180.0;
}

static double distance_km(double first_latitude, double first_longitude,
                          double second_latitude, double second_longitude)
{
    double lat1 = radians(first_latitude);
    double lat2 = radians(second_latitude);
    double dlat = lat2 - lat1;
    double dlon = radians(second_longitude - first_longitude);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1) * cos(lat2) * sin(dlon / 2.0) * sin(dlon / 2.0);
    return EARTH_RADIUS_KM * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    map_http_response_t *response = event->user_data;
    size_t available = response->capacity - response->length;
    if ((size_t)event->data_len > available) {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    return ESP_OK;
}

static size_t jpeg_input(JDEC *decoder, uint8_t *buffer, size_t requested)
{
    map_decode_context_t *context = decoder->device;
    size_t available = context->jpeg_size - context->offset;
    size_t count = requested < available ? requested : available;
    if (buffer && count) memcpy(buffer, context->jpeg + context->offset, count);
    context->offset += count;
    return count;
}

static uint16_t blend_map_pixel(uint8_t red, uint8_t green, uint8_t blue, bool enhance_details)
{
    uint8_t source_min = red < green ? red : green;
    if (blue < source_min) source_min = blue;
    uint8_t source_max = red > green ? red : green;
    if (blue > source_max) source_max = blue;
    uint16_t source_luminance = (uint16_t)(77 * red + 150 * green + 29 * blue) >> 8;
    const uint16_t recolor_alpha = 72;
    const uint16_t image_alpha = 118;
    red = (uint8_t)((red * (255 - recolor_alpha) + 10 * recolor_alpha + 127) / 255);
    green = (uint8_t)((green * (255 - recolor_alpha) + 49 * recolor_alpha + 127) / 255);
    blue = (uint8_t)((blue * (255 - recolor_alpha) + 66 * recolor_alpha + 127) / 255);
    red = (uint8_t)((red * image_alpha + 6 * (255 - image_alpha) + 127) / 255);
    green = (uint8_t)((green * image_alpha + 16 * (255 - image_alpha) + 127) / 255);
    blue = (uint8_t)((blue * image_alpha + 23 * (255 - image_alpha) + 127) / 255);
    if (enhance_details && source_luminance > 70 && source_max - source_min < 64) {
        uint16_t lift = (source_luminance - 70) * 72 / 185;
        uint8_t target_red = (uint8_t)(30 + lift);
        uint8_t target_green = (uint8_t)(48 + lift);
        uint8_t target_blue = (uint8_t)(56 + lift);
        if (red < target_red) red = target_red;
        if (green < target_green) green = target_green;
        if (blue < target_blue) blue = target_blue;
    }
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static int jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    map_decode_context_t *context = decoder->device;
    const uint8_t *source = bitmap;
    size_t source_index = 0;
    for (uint16_t y = rect->top; y <= rect->bottom; ++y) {
        for (uint16_t x = rect->left; x <= rect->right; ++x) {
            context->pixels[(size_t)y * context->pixel_width + x] =
                blend_map_pixel(source[source_index], source[source_index + 1],
                                source[source_index + 2],
                                context->enhance_details && y + 40 < context->pixel_height);
            source_index += 3;
        }
    }
    return 1;
}

static esp_err_t decode_map(const uint8_t *jpeg, size_t jpeg_size, uint8_t scale,
                            bool enhance_details,
                            uint8_t **result, size_t *result_size)
{
    uint8_t *work_buffer = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work_buffer) return ESP_ERR_NO_MEM;
    map_decode_context_t context = {
        .jpeg = jpeg,
        .jpeg_size = jpeg_size,
    };
    JDEC decoder;
    JRESULT result_code = jd_prepare(&decoder, jpeg_input, work_buffer, 4096, &context);
    if (result_code != JDR_OK) {
        heap_caps_free(work_buffer);
        ESP_LOGW(TAG, "Map JPEG prepare failed: %d", result_code);
        return ESP_FAIL;
    }
    uint16_t decoded_width = decoder.width >> scale;
    uint16_t decoded_height = decoder.height >> scale;
    if (decoded_width == 0 || decoded_height == 0) {
        heap_caps_free(work_buffer);
        return ESP_FAIL;
    }
    size_t decoded_bytes = (size_t)decoded_width * decoded_height * sizeof(uint16_t);
    uint16_t *decoded_pixels = heap_caps_malloc(decoded_bytes,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded_pixels) {
        heap_caps_free(work_buffer);
        return ESP_ERR_NO_MEM;
    }
    context.pixels = decoded_pixels;
    context.pixel_width = decoded_width;
    context.pixel_height = decoded_height;
    context.enhance_details = enhance_details;
    result_code = jd_decomp(&decoder, jpeg_output, scale);
    heap_caps_free(work_buffer);
    if (result_code != JDR_OK) {
        ESP_LOGW(TAG, "Map JPEG decode failed: %d", result_code);
        heap_caps_free(decoded_pixels);
        return ESP_FAIL;
    }

    const size_t output_bytes = MAP_IMAGE_WIDTH * MAP_IMAGE_HEIGHT * sizeof(uint16_t);
    if (decoded_width == MAP_IMAGE_WIDTH && decoded_height == MAP_IMAGE_HEIGHT) {
        *result = (uint8_t *)decoded_pixels;
        *result_size = output_bytes;
        return ESP_OK;
    }
    uint16_t *output_pixels = heap_caps_malloc(output_bytes,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output_pixels) {
        heap_caps_free(decoded_pixels);
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t y = 0; y < MAP_IMAGE_HEIGHT; ++y) {
        uint16_t source_y = (uint16_t)(((uint32_t)y * decoded_height) / MAP_IMAGE_HEIGHT);
        if (source_y >= decoded_height) source_y = decoded_height - 1;
        for (uint16_t x = 0; x < MAP_IMAGE_WIDTH; ++x) {
            uint16_t source_x = (uint16_t)(((uint32_t)x * decoded_width) / MAP_IMAGE_WIDTH);
            if (source_x >= decoded_width) source_x = decoded_width - 1;
            output_pixels[(size_t)y * MAP_IMAGE_WIDTH + x] =
                decoded_pixels[(size_t)source_y * decoded_width + source_x];
        }
    }
    heap_caps_free(decoded_pixels);
    if (enhance_details) {
        for (int16_t y = MAP_IMAGE_HEIGHT - 22; y >= 1; --y) {
            for (int16_t x = MAP_IMAGE_WIDTH - 2; x >= 1; --x) {
                uint16_t pixel = output_pixels[(size_t)y * MAP_IMAGE_WIDTH + x];
                uint16_t brightness = (pixel >> 11) * 8 + ((pixel >> 5) & 0x3f) * 4 +
                                      (pixel & 0x1f) * 8;
                if (brightness < 320) continue;
                uint16_t *right = &output_pixels[(size_t)y * MAP_IMAGE_WIDTH + x + 1];
                uint16_t right_brightness = (*right >> 11) * 8 + ((*right >> 5) & 0x3f) * 4 +
                                            (*right & 0x1f) * 8;
                if (right_brightness < brightness) *right = pixel;
            }
        }
    }
    *result = (uint8_t *)output_pixels;
    *result_size = output_bytes;
    return ESP_OK;
}

static esp_err_t fetch_map(double latitude, double longitude, uint16_t radius_km, bool english,
                           uint8_t **result, size_t *result_size,
                           double *south, double *west, double *north, double *east)
{
    bool detailed = radius_km <= MAP_DETAIL_RADIUS_KM;
    double vertical_half_km;
    double horizontal_half_km;
    if (detailed) {
        double source_km_per_pixel =
            156.54303392 * cos(radians(latitude)) / (1U << MAP_DETAIL_ZOOM);
        vertical_half_km = source_km_per_pixel * MAP_DETAIL_SOURCE_HEIGHT * 0.5;
        horizontal_half_km = source_km_per_pixel * MAP_DETAIL_SOURCE_WIDTH * 0.5;
    } else {
        vertical_half_km = radius_km * 1.08;
        horizontal_half_km = vertical_half_km * MAP_IMAGE_WIDTH / MAP_IMAGE_HEIGHT;
    }
    double latitude_delta = vertical_half_km / 110.574;
    double longitude_scale = 111.320 * cos(radians(latitude));
    if (fabs(longitude_scale) < 1.0) longitude_scale = longitude_scale < 0.0 ? -1.0 : 1.0;
    double longitude_delta = horizontal_half_km / longitude_scale;
    *south = latitude - latitude_delta;
    *west = longitude - longitude_delta;
    *north = latitude + latitude_delta;
    *east = longitude + longitude_delta;
    char url[448];
    if (detailed) {
        snprintf(url, sizeof(url),
                 "https://mapmap.ai/api/static-map?center=%.6f%%2C%.6f&zoom=%d"
                 "&size=%dx%d&style=dark&format=jpeg&quality=68&pois=0&lang=%s",
                 longitude, latitude, MAP_DETAIL_ZOOM,
                 MAP_DETAIL_SOURCE_WIDTH, MAP_DETAIL_SOURCE_HEIGHT, english ? "en" : "zh");
    } else {
        snprintf(url, sizeof(url),
                 "https://mapmap.ai/api/static-map?bbox=%.6f%%2C%.6f%%2C%.6f%%2C%.6f"
                 "&size=%dx%d&style=dark&format=jpeg&quality=76&pois=0&lang=%s",
                 *west, *south, *east, *north,
                 MAP_IMAGE_WIDTH, MAP_IMAGE_HEIGHT, english ? "en" : "zh");
    }

    if (!network_http_lock(pdMS_TO_TICKS(15000))) return ESP_ERR_TIMEOUT;
    map_http_response_t response = {
        .data = heap_caps_malloc(MAP_RESPONSE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .capacity = MAP_RESPONSE_CAPACITY,
    };
    if (!response.data) {
        network_http_unlock();
        return ESP_ERR_NO_MEM;
    }

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 40000,
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
    esp_http_client_set_header(client, "User-Agent", "ESP32-Flight-Radar/1.0");
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    network_http_unlock();
    bool jpeg_valid = response.length > 3 && response.data[0] == 0xff &&
                      response.data[1] == 0xd8 && response.data[response.length - 2] == 0xff &&
                      response.data[response.length - 1] == 0xd9;
    if (err != ESP_OK || status_code != 200 || response.overflow || !jpeg_valid) {
        ESP_LOGW(TAG, "Map request failed: err=%s status=%d bytes=%u overflow=%d",
                 esp_err_to_name(err), status_code, (unsigned)response.length, response.overflow);
        heap_caps_free(response.data);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    esp_err_t decode_err = decode_map(response.data, response.length, 0, detailed,
                                      &decoded, &decoded_size);
    heap_caps_free(response.data);
    if (decode_err != ESP_OK) return decode_err;
    *result = decoded;
    *result_size = decoded_size;
    return ESP_OK;
}

static size_t utf8_character_count(const char *text)
{
    size_t count = 0;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
        if ((*cursor & 0xc0) != 0x80) ++count;
    }
    return count;
}

static bool map_label_name_used(const map_label_t *labels, uint8_t count, const char *name)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (strcmp(labels[i].text, name) == 0) return true;
    }
    return false;
}

static bool map_label_rect_overlaps(const map_label_t *labels, uint8_t count,
                                    int16_t x, int16_t y, int16_t width, bool english)
{
    const int16_t height = 18;
    int16_t left = x - width / 2;
    int16_t top = y - height / 2;
    for (uint8_t i = 0; i < count; ++i) {
        size_t glyphs = utf8_character_count(labels[i].text);
        int16_t existing_width = (int16_t)(glyphs * (english ? 8 : 14) + 6);
        if (existing_width > 124) existing_width = 124;
        int16_t existing_left = labels[i].x - existing_width / 2;
        int16_t existing_top = labels[i].y - height / 2;
        if (left < existing_left + existing_width + 6 && left + width + 6 > existing_left &&
            top < existing_top + height + 4 && top + height + 4 > existing_top) {
            return true;
        }
    }
    return false;
}

static int map_label_candidate_compare(const void *first, const void *second)
{
    const map_label_candidate_t *a = first;
    const map_label_candidate_t *b = second;
    if (a->priority != b->priority) return (int)a->priority - (int)b->priority;
    if (a->distance < b->distance) return -1;
    if (a->distance > b->distance) return 1;
    return 0;
}

static uint8_t fetch_map_labels(double latitude, double longitude, bool english,
                                double south, double west, double north, double east,
                                map_label_t labels[MAP_MAX_LABELS])
{
    char url[640];
    snprintf(url, sizeof(url),
             "https://overpass-api.de/api/interpreter?data="
             "%%5Bout%%3Ajson%%5D%%5Btimeout%%3A8%%5D%%3B"
             "node%%5B%%22place%%22~%%22%%5E%%28city%%7Ctown%%7Csuburb%%7Cborough%%29%%24%%22%%5D"
             "%%28%.6f%%2C%.6f%%2C%.6f%%2C%.6f%%29%%3B"
             "out%%20center%%20tags%%20%d%%3B",
             south, west, north, east, MAP_LABEL_CANDIDATES);

    if (!network_http_lock(pdMS_TO_TICKS(15000))) return 0;
    map_http_response_t response = {
        .data = heap_caps_malloc(MAP_LABEL_RESPONSE_CAPACITY + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .capacity = MAP_LABEL_RESPONSE_CAPACITY,
    };
    if (!response.data) {
        network_http_unlock();
        return 0;
    }
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(response.data);
        network_http_unlock();
        return 0;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-Flight-Radar/1.0");
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    network_http_unlock();
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        ESP_LOGW(TAG, "Map label request failed: err=%s status=%d bytes=%u overflow=%d",
                 esp_err_to_name(err), status_code, (unsigned)response.length, response.overflow);
        heap_caps_free(response.data);
        return 0;
    }
    response.data[response.length] = '\0';
    cJSON *root = cJSON_Parse((const char *)response.data);
    heap_caps_free(response.data);
    if (!root) return 0;

    map_label_candidate_t candidates[MAP_LABEL_CANDIDATES] = {0};
    uint8_t candidate_count = 0;
    const cJSON *elements = cJSON_GetObjectItemCaseSensitive(root, "elements");
    const cJSON *element = NULL;
    cJSON_ArrayForEach(element, elements) {
        if (candidate_count >= MAP_LABEL_CANDIDATES) break;
        const cJSON *tags = cJSON_GetObjectItemCaseSensitive(element, "tags");
        if (!cJSON_IsObject(tags)) continue;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(tags, english ? "name:en" : "name:zh");
        if (!cJSON_IsString(name) || !name->valuestring[0]) {
            name = cJSON_GetObjectItemCaseSensitive(tags, "name");
        }
        if (!cJSON_IsString(name) || !name->valuestring[0]) continue;

        const cJSON *lat_value = cJSON_GetObjectItemCaseSensitive(element, "lat");
        const cJSON *lon_value = cJSON_GetObjectItemCaseSensitive(element, "lon");
        if (!cJSON_IsNumber(lat_value) || !cJSON_IsNumber(lon_value)) {
            const cJSON *center = cJSON_GetObjectItemCaseSensitive(element, "center");
            lat_value = cJSON_GetObjectItemCaseSensitive(center, "lat");
            lon_value = cJSON_GetObjectItemCaseSensitive(center, "lon");
        }
        if (!cJSON_IsNumber(lat_value) || !cJSON_IsNumber(lon_value)) continue;
        double item_latitude = lat_value->valuedouble;
        double item_longitude = lon_value->valuedouble;
        if (item_latitude < south || item_latitude > north ||
            item_longitude < west || item_longitude > east) continue;

        const cJSON *place = cJSON_GetObjectItemCaseSensitive(tags, "place");
        const cJSON *admin_level = cJSON_GetObjectItemCaseSensitive(tags, "admin_level");
        uint8_t priority = 3;
        if (cJSON_IsString(place)) {
            if (strcmp(place->valuestring, "city") == 0) priority = 0;
            else if (strcmp(place->valuestring, "town") == 0) priority = 1;
            else if (strcmp(place->valuestring, "suburb") == 0 ||
                     strcmp(place->valuestring, "borough") == 0) priority = 2;
        } else if (cJSON_IsString(admin_level)) {
            priority = strcmp(admin_level->valuestring, "6") == 0 ? 1 : 2;
        }
        map_label_candidate_t *candidate = &candidates[candidate_count++];
        candidate->priority = priority;
        candidate->distance = distance_km(latitude, longitude, item_latitude, item_longitude);
        candidate->label.x = (int16_t)lround((item_longitude - west) /
                                             (east - west) * MAP_IMAGE_WIDTH);
        candidate->label.y = (int16_t)lround((north - item_latitude) /
                                             (north - south) * MAP_IMAGE_HEIGHT);
        strlcpy(candidate->label.text, name->valuestring, sizeof(candidate->label.text));
    }
    cJSON_Delete(root);
    qsort(candidates, candidate_count, sizeof(candidates[0]), map_label_candidate_compare);

    uint8_t label_count = 0;
    for (uint8_t i = 0; i < candidate_count && label_count < MAP_MAX_LABELS; ++i) {
        const map_label_t *candidate = &candidates[i].label;
        if (candidate->x < 18 || candidate->x > MAP_IMAGE_WIDTH - 18 ||
            candidate->y < 18 || candidate->y > MAP_IMAGE_HEIGHT - 18 ||
            map_label_name_used(labels, label_count, candidate->text)) continue;
        size_t glyphs = utf8_character_count(candidate->text);
        int16_t width = (int16_t)(glyphs * (english ? 8 : 14) + 6);
        if (width > 124) width = 124;
        bool overlaps = map_label_rect_overlaps(labels, label_count,
                                                candidate->x, candidate->y, width, english);
        if (overlaps && label_count >= 3) continue;
        labels[label_count++] = *candidate;
    }
    ESP_LOGI(TAG, "Map labels updated: %u", label_count);
    return label_count;
}

static void map_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(MAP_START_DELAY_MS));
    while (true) {
        network_status_t network;
        network_manager_get_status(&network);
        if (!network.coordinates_valid) {
            status_lock();
            status_data.state = MAP_STATE_WAITING_LOCATION;
            status_unlock();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        status_lock();
        uint16_t radius_km = requested_radius_km;
        bool english = requested_english;
        bool data_valid = status_data.data_valid;
        uint16_t loaded_radius_km = status_data.radius_km;
        bool loaded_english = status_data.english;
        map_state_t map_state = status_data.state;
        status_unlock();
        double movement_threshold_km = fmax(2.0, radius_km * 0.10);
        bool location_changed = data_valid &&
                                distance_km(loaded_latitude, loaded_longitude,
                                            network.latitude, network.longitude) >= movement_threshold_km;
        bool needs_refresh = refresh_requested || !data_valid || loaded_radius_km != radius_km ||
                             loaded_english != english || location_changed;
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t retry_ms = map_state == MAP_STATE_ERROR ? MAP_ERROR_RETRY_MS : MAP_RETRY_MS;
        if (!needs_refresh || (last_attempt_ms && now_ms - last_attempt_ms < retry_ms)) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        refresh_requested = false;
        last_attempt_ms = now_ms;
        status_lock();
        status_data.state = MAP_STATE_LOADING;
        status_unlock();
        uint8_t *new_image = NULL;
        size_t new_image_size = 0;
        double south = 0.0;
        double west = 0.0;
        double north = 0.0;
        double east = 0.0;
        esp_err_t err = fetch_map(network.latitude, network.longitude, radius_km, english,
                                  &new_image, &new_image_size, &south, &west, &north, &east);
        uint32_t map_generation = 0;
        status_lock();
        if (err == ESP_OK) {
            heap_caps_free(image_data);
            image_data = new_image;
            loaded_latitude = network.latitude;
            loaded_longitude = network.longitude;
            status_data.state = MAP_STATE_READY;
            status_data.data_valid = true;
            status_data.radius_km = radius_km;
            status_data.english = english;
            status_data.image_size = new_image_size;
            status_data.generation++;
            map_generation = status_data.generation;
            status_data.label_count = 0;
            memset(status_data.labels, 0, sizeof(status_data.labels));
            status_data.label_generation++;
            ESP_LOGI(TAG, "Map updated: radius=%u km language=%s bytes=%u", radius_km,
                     english ? "en" : "zh", (unsigned)new_image_size);
        } else {
            status_data.state = MAP_STATE_ERROR;
        }
        status_unlock();
        if (err == ESP_OK && radius_km > MAP_DETAIL_RADIUS_KM) {
            map_label_t labels[MAP_MAX_LABELS] = {0};
            uint8_t label_count = fetch_map_labels(network.latitude, network.longitude, english,
                                                   south, west, north, east, labels);
            if (label_count == 0) {
                const char *fallback = english ? network.location_en : network.location_zh;
                if (fallback[0]) {
                    labels[0].x = MAP_IMAGE_WIDTH / 2;
                    labels[0].y = MAP_IMAGE_HEIGHT / 2 - 20;
                    strlcpy(labels[0].text, fallback, sizeof(labels[0].text));
                    label_count = 1;
                }
            }
            status_lock();
            if (status_data.generation == map_generation && status_data.radius_km == radius_km &&
                status_data.english == english) {
                status_data.label_count = label_count;
                memcpy(status_data.labels, labels, sizeof(status_data.labels));
                status_data.label_generation++;
            }
            status_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t map_manager_init(uint16_t radius_km, bool english)
{
    if (!valid_radius(radius_km)) return ESP_ERR_INVALID_ARG;
    status_mutex = xSemaphoreCreateMutex();
    if (!status_mutex) return ESP_ERR_NO_MEM;
    requested_radius_km = radius_km;
    requested_english = english;
    status_data.radius_km = radius_km;
    status_data.english = english;
    status_data.state = MAP_STATE_WAITING_LOCATION;
    BaseType_t created = xTaskCreate(map_task, "map", 8192, NULL, 3, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void map_manager_set_config(uint16_t radius_km, bool english)
{
    if (!valid_radius(radius_km)) return;
    status_lock();
    if (requested_radius_km != radius_km || requested_english != english) {
        requested_radius_km = radius_km;
        requested_english = english;
        refresh_requested = true;
        last_attempt_ms = 0;
    }
    status_unlock();
}

void map_manager_get_status(map_status_t *status)
{
    if (!status || !status_mutex) return;
    status_lock();
    *status = status_data;
    status_unlock();
}

esp_err_t map_manager_copy_image(uint32_t generation, uint8_t **data, size_t *size)
{
    if (!data || !size || !status_mutex) return ESP_ERR_INVALID_ARG;
    *data = NULL;
    *size = 0;
    status_lock();
    if (!status_data.data_valid || status_data.generation != generation || !image_data ||
        status_data.image_size == 0) {
        status_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *copy = heap_caps_malloc(status_data.image_size,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        status_unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, image_data, status_data.image_size);
    *data = copy;
    *size = status_data.image_size;
    status_unlock();
    return ESP_OK;
}
