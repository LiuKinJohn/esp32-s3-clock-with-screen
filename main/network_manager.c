#include "network_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_http_lock.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "network";
static const char *NAMESPACE = "clock";
static const char *PORTAL_SSID = "ESP32-Clock-Setup";
static const char *PORTAL_PASSWORD = "12345678";

#define LOCATION_URL_ZH "http://ip-api.com/json/?fields=status,message,regionName,city,district,lat,lon&lang=zh-CN"
#define LOCATION_URL_EN "http://ip-api.com/json/?fields=status,message,regionName,city,district,lat,lon&lang=en"
#define LOCATION_RESPONSE_CAPACITY 2048
#define LOCATION_REFRESH_MS (6LL * 60LL * 60LL * 1000LL)
#define LOCATION_RETRY_MS (5LL * 60LL * 1000LL)

static const char *const timezone_names_zh[] = {
    "北京时间 UTC+8", "协调世界时 UTC", "东京 UTC+9", "纽约", "伦敦"
};
static const char *const timezone_names_en[] = {
    "Beijing UTC+8", "UTC", "Tokyo UTC+9", "New York", "London"
};
static const char *const timezone_rules[] = {
    "CST-8", "UTC0", "JST-9", "EST5EDT,M3.2.0,M11.1.0", "GMT0BST,M3.5.0/1,M10.5.0"
};

static SemaphoreHandle_t status_mutex;
static network_status_t status_data;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static httpd_handle_t http_server;
static TaskHandle_t dns_task_handle;
static volatile bool dns_running;
static bool wifi_started;
static bool sntp_started;
static bool ntp_enabled = true;
static uint8_t ntp_interval_hours = 6;
static uint8_t timezone_index;
static unsigned reconnect_attempts;
static char pending_ssid[33];
static char pending_password[65];
static wifi_ap_record_t scan_records[NETWORK_MAX_APS];

typedef struct {
    char data[LOCATION_RESPONSE_CAPACITY];
    size_t length;
    bool overflow;
} location_http_response_t;

static void status_lock(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(status_mutex);
}

size_t network_manager_timezone_count(void)
{
    return sizeof(timezone_rules) / sizeof(timezone_rules[0]);
}

const char *network_manager_timezone_name(uint8_t index, bool chinese)
{
    if (index >= network_manager_timezone_count()) {
        index = 0;
    }
    return chinese ? timezone_names_zh[index] : timezone_names_en[index];
}

static void apply_timezone(uint8_t index)
{
    if (index >= network_manager_timezone_count()) {
        index = 0;
    }
    timezone_index = index;
    setenv("TZ", timezone_rules[index], 1);
    tzset();
}

static void set_compile_time_if_needed(void)
{
    time_t now;
    time(&now);
    if (now > 1700000000) {
        return;
    }

    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    struct tm tm = {0};
    int year, day, hour, minute, second;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return;
    }
    const char *month_ptr = strstr(months, mon);
    if (!month_ptr) {
        return;
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = (int)((month_ptr - months) / 3);
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    time_t compiled = mktime(&tm);
    struct timeval tv = {.tv_sec = compiled, .tv_usec = 0};
    settimeofday(&tv, NULL);
}

static void save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "wifi_ssid", ssid);
    nvs_set_str(nvs, "wifi_pass", password);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static bool load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t ssid_len = ssid_size;
    size_t pass_len = password_size;
    esp_err_t a = nvs_get_str(nvs, "wifi_ssid", ssid, &ssid_len);
    esp_err_t b = nvs_get_str(nvs, "wifi_pass", password, &pass_len);
    nvs_close(nvs);
    return a == ESP_OK && b == ESP_OK && ssid[0] != '\0';
}

static void clear_credentials(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, "wifi_ssid");
        nvs_erase_key(nvs, "wifi_pass");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool is_ascii_text(const char *text)
{
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        if (*cursor >= 0x80) return false;
    }
    return true;
}

static bool valid_coordinates(double latitude, double longitude)
{
    return latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0 &&
           !(latitude == 0.0 && longitude == 0.0);
}

static bool load_location_cache(char *zh, size_t zh_size, char *en, size_t en_size,
                                double *latitude, double *longitude, bool *manual)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t zh_len = zh_size;
    size_t en_len = en_size;
    esp_err_t zh_err = nvs_get_str(nvs, "location_zh", zh, &zh_len);
    esp_err_t en_err = nvs_get_str(nvs, "location_en", en, &en_len);
    size_t coordinate_size = sizeof(double);
    esp_err_t lat_err = nvs_get_blob(nvs, "location_lat", latitude, &coordinate_size);
    coordinate_size = sizeof(double);
    esp_err_t lon_err = nvs_get_blob(nvs, "location_lon", longitude, &coordinate_size);
    uint8_t manual_value = 0;
    nvs_get_u8(nvs, "location_man", &manual_value);
    nvs_close(nvs);
    if (zh_err != ESP_OK) zh[0] = '\0';
    if (en_err != ESP_OK || !is_ascii_text(en)) en[0] = '\0';
    if (lat_err != ESP_OK || lon_err != ESP_OK || !valid_coordinates(*latitude, *longitude)) {
        *latitude = 0.0;
        *longitude = 0.0;
    }
    *manual = manual_value != 0 && valid_coordinates(*latitude, *longitude);
    if (*manual) {
        strlcpy(zh, "手动位置", zh_size);
        strlcpy(en, "Manual location", en_size);
    }
    return zh[0] != '\0' || en[0] != '\0' || valid_coordinates(*latitude, *longitude);
}

static void save_location_cache(const char *zh, const char *en, double latitude, double longitude,
                                bool manual)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "location_zh", zh);
    nvs_set_str(nvs, "location_en", en);
    nvs_set_blob(nvs, "location_lat", &latitude, sizeof(latitude));
    nvs_set_blob(nvs, "location_lon", &longitude, sizeof(longitude));
    nvs_set_u8(nvs, "location_man", manual ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static esp_err_t location_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    location_http_response_t *response = event->user_data;
    size_t available = sizeof(response->data) - response->length - 1;
    if ((size_t)event->data_len > available) {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_location(const char *url, char *out, size_t out_size,
                                double *latitude, double *longitude)
{
    if (!network_http_lock(pdMS_TO_TICKS(15000))) return ESP_ERR_TIMEOUT;
    location_http_response_t response = {0};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = location_http_event_handler,
        .user_data = &response,
        .timeout_ms = 7000,
        .buffer_size = 1024,
        .buffer_size_tx = 256,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        network_http_unlock();
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    network_http_unlock();
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        ESP_LOGW(TAG, "Location request failed: err=%s status=%d bytes=%u",
                 esp_err_to_name(err), status_code, (unsigned)response.length);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *region = cJSON_GetObjectItemCaseSensitive(root, "regionName");
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    const cJSON *district = cJSON_GetObjectItemCaseSensitive(root, "district");
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "lon");
    bool success = cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0;
    const char *city_text = cJSON_IsString(city) && city->valuestring[0] != '\0'
                                ? city->valuestring
                                : (cJSON_IsString(region) ? region->valuestring : "");
    const char *district_text = cJSON_IsString(district) ? district->valuestring : "";
    if (!success || city_text[0] == '\0' || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) ||
        !valid_coordinates(lat->valuedouble, lon->valuedouble)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    if (district_text[0] != '\0' && strcmp(city_text, district_text) != 0) {
        snprintf(out, out_size, "%s %s", city_text, district_text);
    } else {
        strlcpy(out, city_text, out_size);
    }
    *latitude = lat->valuedouble;
    *longitude = lon->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

static void location_task(void *arg)
{
    (void)arg;
    bool have_zh;
    bool have_en;
    status_lock();
    have_zh = status_data.location_zh[0] != '\0';
    have_en = status_data.location_en[0] != '\0' && is_ascii_text(status_data.location_en);
    status_unlock();

    int64_t last_attempt_ms = -LOCATION_REFRESH_MS;
    int64_t retry_after_ms = LOCATION_REFRESH_MS;
    while (true) {
        status_lock();
        bool connected = status_data.state == NETWORK_STATE_CONNECTED;
        bool manual_coordinates = status_data.coordinates_manual;
        status_unlock();
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (connected && !manual_coordinates && now_ms - last_attempt_ms >= retry_after_ms) {
            status_lock();
            status_data.location_updating = true;
            status_unlock();
            if (!have_zh && !have_en) {
                status_lock();
                status_data.location_state = NETWORK_LOCATION_LOCATING;
                status_data.location_generation++;
                status_unlock();
            }

            char location_zh[96] = {0};
            char location_en[96] = {0};
            double latitude_zh = 0.0;
            double longitude_zh = 0.0;
            double latitude_en = 0.0;
            double longitude_en = 0.0;
            esp_err_t zh_err = fetch_location(LOCATION_URL_ZH, location_zh, sizeof(location_zh),
                                              &latitude_zh, &longitude_zh);
            vTaskDelay(pdMS_TO_TICKS(750));
            esp_err_t en_err = fetch_location(LOCATION_URL_EN, location_en, sizeof(location_en),
                                              &latitude_en, &longitude_en);
            last_attempt_ms = esp_timer_get_time() / 1000;
            if (zh_err == ESP_OK || en_err == ESP_OK) {
                status_lock();
                if (status_data.coordinates_manual) {
                    status_data.location_updating = false;
                    status_unlock();
                    continue;
                }
                if (zh_err == ESP_OK) {
                    strlcpy(status_data.location_zh, location_zh, sizeof(status_data.location_zh));
                    have_zh = true;
                }
                if (en_err == ESP_OK) {
                    strlcpy(status_data.location_en, location_en, sizeof(status_data.location_en));
                    have_en = true;
                }
                if (!have_zh && have_en) {
                    strlcpy(status_data.location_zh, status_data.location_en, sizeof(status_data.location_zh));
                }
                if (!have_en && have_zh) {
                    strlcpy(status_data.location_en, status_data.location_zh, sizeof(status_data.location_en));
                }
                status_data.latitude = zh_err == ESP_OK ? latitude_zh : latitude_en;
                status_data.longitude = zh_err == ESP_OK ? longitude_zh : longitude_en;
                status_data.coordinates_valid = true;
                status_data.coordinates_manual = false;
                status_data.location_state = NETWORK_LOCATION_AVAILABLE;
                status_data.location_generation++;
                char cached_zh[96];
                char cached_en[96];
                double cached_latitude = status_data.latitude;
                double cached_longitude = status_data.longitude;
                strlcpy(cached_zh, status_data.location_zh, sizeof(cached_zh));
                strlcpy(cached_en, status_data.location_en, sizeof(cached_en));
                status_unlock();
                if (have_zh && have_en) {
                    save_location_cache(cached_zh, cached_en, cached_latitude, cached_longitude, false);
                }
                retry_after_ms = have_zh && have_en ? LOCATION_REFRESH_MS : LOCATION_RETRY_MS;
                ESP_LOGI(TAG, "Approximate location updated: %s / %s", cached_zh, cached_en);
            } else {
                if (!have_zh && !have_en) {
                    status_lock();
                    status_data.location_state = NETWORK_LOCATION_UNAVAILABLE;
                    status_data.location_generation++;
                    status_unlock();
                }
                retry_after_ms = LOCATION_RETRY_MS;
            }
            status_lock();
            status_data.location_updating = false;
            status_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void time_sync_callback(struct timeval *tv)
{
    (void)tv;
    time_t now;
    time(&now);
    status_lock();
    status_data.time_synchronized = true;
    status_data.last_sync = now;
    status_data.next_sync = now + (time_t)ntp_interval_hours * 3600;
    status_unlock();
    ESP_LOGI(TAG, "Time synchronized");
}

static void start_sntp(void)
{
    if (!ntp_enabled) {
        return;
    }
    if (sntp_started) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_set_sync_interval((uint32_t)ntp_interval_hours * 3600U * 1000U);
    esp_sntp_set_time_sync_notification_cb(time_sync_callback);
    esp_sntp_init();
    sntp_started = true;
}

void network_manager_set_time_config(bool enabled, uint8_t interval_hours, uint8_t tz_index)
{
    ntp_enabled = enabled;
    ntp_interval_hours = interval_hours ? interval_hours : 6;
    apply_timezone(tz_index);
    if (!enabled && sntp_started) {
        esp_sntp_stop();
        sntp_started = false;
    } else if (enabled && status_data.state == NETWORK_STATE_CONNECTED) {
        start_sntp();
    }
}

void network_manager_force_sync(void)
{
    if (ntp_enabled && status_data.state == NETWORK_STATE_CONNECTED) {
        start_sntp();
    }
}

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[512];
    while (dns_running) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *)&client, &client_len);
        if (len < 12) {
            continue;
        }
        int question_end = 12;
        while (question_end < len && packet[question_end] != 0) {
            question_end += packet[question_end] + 1;
        }
        question_end += 5;
        if (question_end > len || question_end + 16 > (int)sizeof(packet)) {
            continue;
        }
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        int pos = question_end;
        const uint8_t answer[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x3C, 0x00, 0x04, 192, 168, 4, 1
        };
        memcpy(packet + pos, answer, sizeof(answer));
        pos += sizeof(answer);
        sendto(sock, packet, pos, 0, (struct sockaddr *)&client, client_len);
    }
    close(sock);
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

static const char portal_html[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
    "<title>ESP32 Clock Setup</title><style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:20px;color:#182230}"
    "input,button{box-sizing:border-box;width:100%;padding:14px;margin:7px 0;border:1px solid #ccd3dc;border-radius:6px}"
    "button{background:#1769e0;color:white;border:0;font-weight:600}.secondary{background:#e8eef7;color:#18324b}"
    "section{border-top:1px solid #dde3ea;margin-top:24px;padding-top:12px}.hint{color:#5a6570;font-size:14px}</style></head><body>"
    "<h2>ESP32 Clock Wi-Fi</h2><p>Enter your Wi-Fi network details.</p>"
    "<form method='post' action='/save'><input name='ssid' maxlength='32' placeholder='Wi-Fi name' required>"
    "<input name='password' maxlength='64' type='password' placeholder='Password'>"
    "<button type='submit'>Connect</button></form>"
    "<section><h2>Aircraft board location / 航班看板位置</h2>"
    "<p class='hint'>Use the phone position first. If the browser blocks location access, enter latitude and longitude manually.</p>"
    "<form method='post' action='/save-location'><input id='lat' name='lat' type='number' step='0.000001' min='-90' max='90' placeholder='Latitude / 纬度' required>"
    "<input id='lon' name='lon' type='number' step='0.000001' min='-180' max='180' placeholder='Longitude / 经度' required>"
    "<button class='secondary' type='button' onclick='locate()'>Use phone location / 使用手机定位</button>"
    "<button type='submit'>Save location / 保存位置</button></form><p id='location-status' class='hint'></p></section>"
    "<p class='hint'>Device setup address: 192.168.4.1</p>"
    "<script>function locate(){var s=document.getElementById('location-status');"
    "if(!navigator.geolocation){s.textContent='Location is unavailable. Please enter coordinates manually.';return;}"
    "s.textContent='Locating... / 正在定位';navigator.geolocation.getCurrentPosition(function(p){"
    "document.getElementById('lat').value=p.coords.latitude.toFixed(6);document.getElementById('lon').value=p.coords.longitude.toFixed(6);"
    "s.textContent='Location acquired. Press Save location. / 已获取位置，请保存。';},function(){"
    "s.textContent='Location permission was blocked. Please enter coordinates manually. / 定位权限不可用，请手动填写。';},"
    "{enableHighAccuracy:true,timeout:12000,maximumAge:60000});}</script></body></html>";

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, portal_html, HTTPD_RESP_USE_STRLEN);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_size; ++i) {
        if (src[i] == '+' ) {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int high = hex_value(src[i + 1]);
            int low = hex_value(src[i + 2]);
            if (high >= 0 && low >= 0) {
                dst[out++] = (char)((high << 4) | low);
                i += 2;
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool form_value(const char *body, const char *key, char *value, size_t value_size)
{
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%s=", key);
    const char *start = strstr(body, prefix);
    if (!start) return false;
    start += strlen(prefix);
    const char *end = strchr(start, '&');
    if (!end) end = start + strlen(start);
    url_decode(value, value_size, start, (size_t)(end - start));
    return true;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int wanted = req->content_len;
    if (wanted >= (int)sizeof(body)) wanted = sizeof(body) - 1;
    int received = httpd_req_recv(req, body, wanted);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    }
    body[received] = '\0';
    char ssid[33] = {0};
    char password[65] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi name is required");
    }
    form_value(body, "password", password, sizeof(password));
    network_manager_connect(ssid, password);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req,
        "<html><meta name='viewport' content='width=device-width'><body><h2>Connecting...</h2>"
        "<p>You can return to the clock screen.</p></body></html>", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_location_save_handler(httpd_req_t *req)
{
    char body[160] = {0};
    int wanted = req->content_len;
    if (wanted >= (int)sizeof(body)) wanted = sizeof(body) - 1;
    int received = httpd_req_recv(req, body, wanted);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    }
    body[received] = '\0';
    char latitude_text[32] = {0};
    char longitude_text[32] = {0};
    if (!form_value(body, "lat", latitude_text, sizeof(latitude_text)) ||
        !form_value(body, "lon", longitude_text, sizeof(longitude_text))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Coordinates are required");
    }
    char *latitude_end = NULL;
    char *longitude_end = NULL;
    double latitude = strtod(latitude_text, &latitude_end);
    double longitude = strtod(longitude_text, &longitude_end);
    if (latitude_end == latitude_text || longitude_end == longitude_text ||
        !valid_coordinates(latitude, longitude)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid coordinates");
    }
    esp_err_t err = network_manager_set_manual_coordinates(latitude, longitude);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to save location");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req,
        "<html><meta name='viewport' content='width=device-width'><body><h2>Location saved</h2>"
        "<p>Aircraft data will refresh automatically. / 位置已保存，航班数据将自动刷新。</p>"
        "<p><a href='/'>Back / 返回</a></p></body></html>", HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    if (http_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&http_server, &config) != ESP_OK) {
        http_server = NULL;
        return;
    }
    const httpd_uri_t location_post = {
        .uri = "/save-location", .method = HTTP_POST, .handler = portal_location_save_handler};
    const httpd_uri_t post = {.uri = "/save", .method = HTTP_POST, .handler = portal_save_handler};
    const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = portal_get_handler};
    httpd_register_uri_handler(http_server, &location_post);
    httpd_register_uri_handler(http_server, &post);
    httpd_register_uri_handler(http_server, &get);
}

esp_err_t network_manager_start_portal(void)
{
    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, PORTAL_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, PORTAL_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(PORTAL_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) return err;
    status_lock();
    status_data.portal_running = true;
    status_unlock();
    start_http_server();
    if (!dns_running) {
        dns_running = true;
        xTaskCreate(dns_server_task, "captive_dns", 3072, NULL, 3, &dns_task_handle);
    }
    ESP_LOGI(TAG, "Setup AP started: %s", PORTAL_SSID);
    return ESP_OK;
}

esp_err_t network_manager_stop_portal(void)
{
    dns_running = false;
    if (http_server) {
        httpd_stop(http_server);
        http_server = NULL;
    }
    status_lock();
    status_data.portal_running = false;
    status_unlock();
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = NETWORK_MAX_APS;
        memset(scan_records, 0, sizeof(scan_records));
        esp_wifi_scan_get_ap_records(&count, scan_records);
        status_lock();
        status_data.ap_count = count;
        for (size_t i = 0; i < count; ++i) {
            strlcpy(status_data.aps[i].ssid, (const char *)scan_records[i].ssid, sizeof(status_data.aps[i].ssid));
            status_data.aps[i].rssi = scan_records[i].rssi;
            status_data.aps[i].open = scan_records[i].authmode == WIFI_AUTH_OPEN;
        }
        status_data.scan_generation++;
        if (status_data.state == NETWORK_STATE_SCANNING) {
            status_data.state = status_data.ip[0] ? NETWORK_STATE_CONNECTED : NETWORK_STATE_IDLE;
        }
        status_unlock();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        status_lock();
        bool should_retry = status_data.has_credentials || pending_ssid[0] != '\0';
        status_data.state = should_retry ? NETWORK_STATE_CONNECTING : NETWORK_STATE_IDLE;
        status_data.ip[0] = '\0';
        status_unlock();
        if (should_retry && reconnect_attempts++ < 5) {
            esp_wifi_connect();
        } else if (should_retry) {
            status_lock();
            status_data.state = NETWORK_STATE_ERROR;
            strlcpy(status_data.error, "Unable to connect to Wi-Fi", sizeof(status_data.error));
            status_unlock();
            network_manager_start_portal();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        status_lock();
        status_data.state = NETWORK_STATE_CONNECTED;
        status_data.error[0] = '\0';
        strlcpy(status_data.ip, ip, sizeof(status_data.ip));
        if (pending_ssid[0]) {
            strlcpy(status_data.ssid, pending_ssid, sizeof(status_data.ssid));
            status_data.has_credentials = true;
        }
        status_unlock();
        reconnect_attempts = 0;
        if (pending_ssid[0]) {
            save_credentials(pending_ssid, pending_password);
            pending_ssid[0] = '\0';
            pending_password[0] = '\0';
        }
        start_sntp();
    }
}

esp_err_t network_manager_scan(void)
{
    status_lock();
    status_data.state = NETWORK_STATE_SCANNING;
    status_unlock();
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        status_lock();
        status_data.state = NETWORK_STATE_ERROR;
        strlcpy(status_data.error, esp_err_to_name(err), sizeof(status_data.error));
        status_unlock();
    }
    return err;
}

esp_err_t network_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password ? password : "", sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    strlcpy(pending_ssid, ssid, sizeof(pending_ssid));
    strlcpy(pending_password, password ? password : "", sizeof(pending_password));
    reconnect_attempts = 0;
    status_lock();
    status_data.state = NETWORK_STATE_CONNECTING;
    strlcpy(status_data.ssid, ssid, sizeof(status_data.ssid));
    status_data.error[0] = '\0';
    status_unlock();
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_connect();
    return err;
}

esp_err_t network_manager_forget(void)
{
    clear_credentials();
    pending_ssid[0] = '\0';
    pending_password[0] = '\0';
    esp_wifi_disconnect();
    status_lock();
    status_data.has_credentials = false;
    status_data.state = NETWORK_STATE_IDLE;
    status_data.ssid[0] = '\0';
    status_data.ip[0] = '\0';
    status_data.time_synchronized = false;
    status_data.last_sync = 0;
    status_data.next_sync = 0;
    status_unlock();
    network_manager_start_portal();
    return network_manager_scan();
}

void network_manager_get_status(network_status_t *out)
{
    status_lock();
    *out = status_data;
    status_unlock();
}

esp_err_t network_manager_set_manual_coordinates(double latitude, double longitude)
{
    if (!valid_coordinates(latitude, longitude)) return ESP_ERR_INVALID_ARG;

    status_lock();
    status_data.latitude = latitude;
    status_data.longitude = longitude;
    status_data.coordinates_valid = true;
    status_data.coordinates_manual = true;
    status_data.location_state = NETWORK_LOCATION_AVAILABLE;
    status_data.location_updating = false;
    strlcpy(status_data.location_zh, "手动位置", sizeof(status_data.location_zh));
    strlcpy(status_data.location_en, "Manual location", sizeof(status_data.location_en));
    status_data.location_generation++;
    status_unlock();

    save_location_cache("手动位置", "Manual location", latitude, longitude, true);
    ESP_LOGI(TAG, "Manual coordinates saved: %.5f, %.5f", latitude, longitude);
    return ESP_OK;
}

esp_err_t network_manager_init(const app_settings_t *settings)
{
    status_mutex = xSemaphoreCreateMutex();
    if (!status_mutex) return ESP_ERR_NO_MEM;
    memset(&status_data, 0, sizeof(status_data));
    bool manual_coordinates = false;
    if (load_location_cache(status_data.location_zh, sizeof(status_data.location_zh),
                            status_data.location_en, sizeof(status_data.location_en),
                            &status_data.latitude, &status_data.longitude, &manual_coordinates)) {
        status_data.location_state = NETWORK_LOCATION_AVAILABLE;
    }
    status_data.coordinates_valid = valid_coordinates(status_data.latitude, status_data.longitude);
    status_data.coordinates_manual = manual_coordinates;
    ntp_enabled = settings->ntp_enabled;
    ntp_interval_hours = settings->ntp_interval_hours;
    apply_timezone(settings->timezone_index);
    set_compile_time_if_needed();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) return loop_err;
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    if (!sta_netif || !ap_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    char ssid[33] = {0};
    char password[65] = {0};
    bool saved = load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    status_data.has_credentials = saved;
    if (saved) {
        wifi_config_t config = {0};
        strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        config.sta.pmf_cfg.capable = true;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
        strlcpy(status_data.ssid, ssid, sizeof(status_data.ssid));
        status_data.state = NETWORK_STATE_CONNECTING;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started = true;
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(52));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_LOGI(TAG, "Wi-Fi maximum TX power limited to 13 dBm");
    network_manager_start_portal();
    if (saved) {
        esp_wifi_connect();
    } else {
        network_manager_scan();
    }
    if (xTaskCreate(location_task, "ip_location", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create location task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
