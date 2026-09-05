#include "clock_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_init.h"
#include "app_settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "flight_manager.h"
#include "lvgl.h"
#include "map_manager.h"
#include "network_manager.h"
#include "transit_manager.h"

static const char *TAG = "clock_ui";
static const uint16_t flight_radii_km[] = {20, 50, 70, 100, 120, 200};
static const uint8_t flight_limits[] = {5, 8, 10, 13, 15};

LV_FONT_DECLARE(lv_font_chinese_16);
LV_FONT_DECLARE(lv_font_ui_14);
LV_FONT_DECLARE(lv_font_ui_16);
LV_FONT_DECLARE(lv_font_ui_20);
LV_FONT_DECLARE(lv_font_ui_28);
LV_FONT_DECLARE(lv_font_ui_48);
LV_FONT_DECLARE(lv_font_pids_16);
LV_FONT_DECLARE(lv_font_pids_20);
LV_FONT_DECLARE(lv_font_pids_28);
extern const lv_image_dsc_t *const clock_digit_images[10];
extern const lv_image_dsc_t *const blue_clock_light_images[10];
extern const lv_image_dsc_t *const blue_clock_dark_images[10];
extern const lv_image_dsc_t *const blue_clock_colon_image;
typedef enum {
    TXT_DASHBOARD,
    TXT_SETTINGS,
    TXT_WIFI,
    TXT_TIME_SYNC,
    TXT_SWIPE,
    TXT_BRIGHTNESS,
    TXT_LANGUAGE,
    TXT_THEME,
    TXT_TIME_FORMAT,
    TXT_CLOCK_STYLE,
    TXT_SCREEN_SLEEP,
    TXT_NTP,
    TXT_NTP_INTERVAL,
    TXT_TIMEZONE,
    TXT_IP_ADDRESS,
    TXT_NEXT_SYNC,
    TXT_CHANGE_WIFI,
    TXT_CLEAR_WIFI,
    TXT_SETUP_AP,
    TXT_SYNC_NOW,
    TXT_RESTART,
    TXT_RESET_SETTINGS,
    TXT_CONNECTED,
    TXT_CONNECTING,
    TXT_OFFLINE,
    TXT_SYNCED,
    TXT_WAITING,
    TXT_SCAN_WIFI,
    TXT_REFRESH,
    TXT_CONNECT,
    TXT_CANCEL,
    TXT_PASSWORD,
    TXT_PORTAL_INFO,
    TXT_NEVER,
    TXT_ETA_STATION,
    TXT_ETA_DIRECTION,
    TXT_FLIGHT_RADIUS,
    TXT_FLIGHT_LIMIT,
    TXT_FLIGHT_LABEL_SIZE,
    TXT_COUNT,
} text_id_t;

static const char *const text_zh[TXT_COUNT] = {
    "时间仪表盘", "设置", "无线网络", "网络校时", "左右滑动切换页面", "屏幕亮度", "界面语言",
    "显示主题", "时间格式", "时钟样式", "自动息屏", "NTP 自动校时", "校时间隔", "时区", "当前 IP", "下次校时",
    "切换网络", "清除网络", "启动手机配网", "立即校时", "长按重启", "长按恢复设置", "已连接", "连接中",
    "离线", "已校时", "等待校时", "选择无线网络", "刷新", "连接", "取消", "Wi-Fi 密码",
    "手机连接 ESP32-Clock-Setup，密码 12345678，然后访问 192.168.4.1", "永不", "ETA 看板站点", "行车方向",
    "航班搜索半径", "显示飞机数量", "航班信息字号",
};

static const char *const text_en[TXT_COUNT] = {
    "TIME DASHBOARD", "SETTINGS", "Wi-Fi", "TIME SYNC", "Swipe left or right to change page", "Brightness",
    "Language", "Theme", "Time format", "Clock style", "Screen sleep", "NTP time sync", "Sync interval", "Time zone",
    "Current IP", "Next sync", "Change network", "Clear network", "Start phone setup", "Sync now", "Hold to restart",
    "Hold to reset settings", "Connected", "Connecting", "Offline", "Synchronized", "Waiting for sync", "Choose Wi-Fi",
    "Refresh", "Connect", "Cancel", "Wi-Fi password",
    "Join ESP32-Clock-Setup with password 12345678, then open 192.168.4.1", "Never", "ETA board station", "Train direction",
    "Flight search radius", "Aircraft shown", "Aircraft label size",
};

static app_settings_t settings;
static bool dark_active;
static bool screen_dimmed;
static uint32_t last_scan_generation;
static uint32_t last_transit_generation;
static bool last_transit_stale;
static uint32_t last_flight_generation;
static uint32_t last_flight_location_generation;
static uint32_t last_map_generation;
static uint32_t last_map_label_generation;
static bool last_flight_stale;
static uint32_t last_flight_projection_tick;
static bool pids_english;
static uint32_t pids_language_started;
static char selected_ssid[33];

static lv_style_t style_screen;
static lv_style_t style_page;
static lv_style_t style_card;
static lv_style_t style_button;
static lv_style_t style_secondary_button;
static lv_style_t style_status;
static lv_style_t style_tonal;
static lv_style_t style_eta_row;
static lv_style_t style_route_badge;

static lv_obj_t *screen;
static lv_obj_t *viewport;
static lv_obj_t *clock_page;
static lv_obj_t *transit_page;
static lv_obj_t *flight_page;
static lv_obj_t *settings_page;
static lv_obj_t *page_dots[4];
static lv_timer_t *page_dots_hide_timer;
static int32_t page_dots_opa;
static lv_image_dsc_t transit_badge_background;
static lv_image_dsc_t page_dot_background;
static uint32_t *round_asset_pixels;
static bool round_assets_ready;

static lv_obj_t *title_clock;
static lv_obj_t *clock_digits[4];
static int8_t displayed_clock_digits[4] = {-1, -1, -1, -1};
static lv_obj_t *blue_clock_digits[4];
static lv_obj_t *blue_clock_colon;
static lv_obj_t *blue_clock_location_row;
static lv_obj_t *blue_clock_location_icon;
static lv_obj_t *blue_clock_location_text;
static bool blue_clock_location_icon_is_pulsing;
static int8_t displayed_blue_clock_digits[4] = {-1, -1, -1, -1};
typedef struct {
    lv_obj_t *object;
    uint8_t index;
    uint8_t pending_digit;
} blue_clock_digit_fade_t;
static blue_clock_digit_fade_t blue_clock_digit_fades[4];
static lv_obj_t *wifi_title;
static lv_obj_t *wifi_value;
static lv_obj_t *sync_title;
static lv_obj_t *sync_value;
static lv_obj_t *swipe_hint;
static lv_obj_t *top_status;
static lv_timer_t *top_status_hide_timer;
static int32_t top_status_opa = LV_OPA_COVER;
static bool top_status_initialized;
static bool top_status_last_healthy;

static lv_obj_t *transit_title;
static lv_obj_t *transit_direction;
static lv_obj_t *transit_clock;
static lv_obj_t *transit_weather_icon;
static lv_obj_t *transit_weather;
static lv_obj_t *transit_weather_caption;
static lv_obj_t *transit_destinations[TRANSIT_MAX_TRAINS];
static lv_obj_t *transit_eta_values[TRANSIT_MAX_TRAINS];
static lv_obj_t *transit_eta_units[TRANSIT_MAX_TRAINS];
static lv_obj_t *transit_eta_badges[TRANSIT_MAX_TRAINS];
static lv_obj_t *transit_service_panel;
static lv_obj_t *transit_service_message;
static lv_obj_t *transit_footer;
static lv_obj_t *transit_refresh_button;

typedef struct {
    lv_obj_t *icon;
    uint8_t heading_image_index;
    lv_obj_t *label;
    lv_obj_t *stats_label;
    bool transition_running;
    int16_t pending_icon_x;
    int16_t pending_icon_y;
    int16_t pending_label_x;
    int16_t pending_label_y;
    int16_t pending_label_width;
    int16_t pending_label_height;
    int16_t pending_stats_width;
    int16_t pending_stats_height;
    lv_text_align_t pending_text_align;
    bool pending_heading_valid;
    float pending_heading_deg;
    bool displayed_heading_valid;
    float displayed_heading_deg;
    char displayed_hex[9];
    char pending_hex[9];
    char pending_callsign[16];
    char pending_stats[48];
} flight_item_widgets_t;

#define FLIGHT_ICON_SIZE 24
#define FLIGHT_HEADING_IMAGE_COUNT 24
#define FLIGHT_HEADING_STEP_DEG (360.0f / FLIGHT_HEADING_IMAGE_COUNT)
#define FLIGHT_RADAR_WIDTH MAP_IMAGE_WIDTH
#define FLIGHT_RADAR_HEIGHT MAP_IMAGE_HEIGHT
#define FLIGHT_LABEL_PADDING 3

static lv_image_dsc_t flight_heading_images[FLIGHT_HEADING_IMAGE_COUNT];
static uint32_t *flight_heading_pixels;

static lv_obj_t *flight_title;
static lv_obj_t *flight_count;
static lv_obj_t *flight_compass;
static lv_obj_t *flight_map_image;
static lv_obj_t *flight_map_labels[MAP_MAX_LABELS];
static lv_obj_t *flight_map_label_echoes[MAP_MAX_LABELS];
static lv_image_dsc_t flight_map_descriptor;
static uint8_t *flight_map_bytes;
static lv_obj_t *flight_message;
static lv_obj_t *flight_footer;
static lv_obj_t *flight_detail_panel;
static lv_obj_t *flight_detail_title;
static lv_obj_t *flight_detail_line_one;
static lv_obj_t *flight_detail_line_two;
static lv_obj_t *flight_detail_line_three;
static lv_timer_t *flight_detail_timer;
static flight_item_widgets_t flight_items[FLIGHT_MAX_AIRCRAFT];
static flight_status_t displayed_flight_status;
static bool flight_page_active;
static bool viewport_scrolling;

static lv_obj_t *title_settings;
static lv_obj_t *label_brightness;
static lv_obj_t *label_language;
static lv_obj_t *label_theme;
static lv_obj_t *label_time_format;
static lv_obj_t *label_clock_style;
static lv_obj_t *label_sleep;
static lv_obj_t *label_ntp;
static lv_obj_t *label_interval;
static lv_obj_t *label_timezone;
static lv_obj_t *label_eta_station;
static lv_obj_t *label_eta_direction;
static lv_obj_t *label_flight_radius;
static lv_obj_t *label_flight_limit;
static lv_obj_t *label_flight_label_size;
static lv_obj_t *label_ip_title;
static lv_obj_t *label_ip_value;
static lv_obj_t *label_next_title;
static lv_obj_t *label_next_value;

static lv_obj_t *slider_brightness;
static lv_obj_t *dropdown_language;
static lv_obj_t *dropdown_theme;
static lv_obj_t *dropdown_time_format;
static lv_obj_t *dropdown_clock_style;
static lv_obj_t *dropdown_sleep;
static lv_obj_t *switch_ntp;
static lv_obj_t *dropdown_interval;
static lv_obj_t *dropdown_timezone;
static lv_obj_t *dropdown_eta_station;
static lv_obj_t *dropdown_eta_direction;
static lv_obj_t *button_flight_radius_minus;
static lv_obj_t *button_flight_radius_plus;
static lv_obj_t *label_flight_radius_value;
static lv_obj_t *button_flight_limit_minus;
static lv_obj_t *button_flight_limit_plus;
static lv_obj_t *label_flight_limit_value;
static lv_obj_t *dropdown_flight_label_size;
static lv_obj_t *button_change_wifi;
static lv_obj_t *button_clear_wifi;
static lv_obj_t *button_portal;
static lv_obj_t *button_sync;
static lv_obj_t *button_restart;
static lv_obj_t *button_reset;
static lv_obj_t *button_language_quick;

static lv_obj_t *wifi_overlay;
static lv_obj_t *wifi_overlay_title;
static lv_obj_t *wifi_list;
static lv_obj_t *wifi_overlay_status;
static lv_obj_t *wifi_refresh_button;
static lv_obj_t *wifi_portal_button;
static lv_obj_t *wifi_close_button;
static lv_obj_t *password_panel;
static lv_obj_t *password_title;
static lv_obj_t *password_area;
static lv_obj_t *password_keyboard;
static lv_obj_t *password_connect_button;
static lv_obj_t *password_cancel_button;
static char wifi_ssids[NETWORK_MAX_APS][33];
enum {
    PIDS_ROW_EDGE_MARGIN = 16,
    PIDS_ETA_VALUE_GAP = 4,
};
static bool wifi_open[NETWORK_MAX_APS];

static const uint32_t lunar_info[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5d0,0x14573,0x052d0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b5a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x05ac0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
    0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
};

static const char *tr(text_id_t id)
{
    return settings.language == APP_LANG_ZH ? text_zh[id] : text_en[id];
}

static const char *localized(const char *chinese, const char *english)
{
    return settings.language == APP_LANG_ZH ? chinese : english;
}

static const char *pids_localized(const char *chinese, const char *english)
{
    return pids_english ? english : chinese;
}

static int lunar_leap_month(int year)
{
    return lunar_info[year - 1900] & 0x0f;
}

static int lunar_leap_days(int year)
{
    int leap = lunar_leap_month(year);
    return leap ? ((lunar_info[year - 1900] & 0x10000) ? 30 : 29) : 0;
}

static int lunar_year_days(int year)
{
    int days = 348;
    uint32_t mask = 0x8000;
    for (int i = 0; i < 12; ++i, mask >>= 1) {
        if (lunar_info[year - 1900] & mask) ++days;
    }
    return days + lunar_leap_days(year);
}

static int lunar_month_days(int year, int month)
{
    return (lunar_info[year - 1900] & (0x10000 >> month)) ? 30 : 29;
}

static void lunar_from_date(const struct tm *date, int *year, int *month, int *day, bool *is_leap)
{
    struct tm base = {.tm_year = 0, .tm_mon = 0, .tm_mday = 31, .tm_isdst = -1};
    struct tm current = *date;
    current.tm_hour = 12;
    base.tm_hour = 12;
    int offset = (int)(difftime(mktime(&current), mktime(&base)) / 86400.0);
    int y = 1900;
    while (y < 2100) {
        int days = lunar_year_days(y);
        if (offset < days) break;
        offset -= days;
        ++y;
    }
    int leap = lunar_leap_month(y);
    bool leap_now = false;
    int m = 1;
    while (m <= 12) {
        int days = leap_now ? lunar_leap_days(y) : lunar_month_days(y, m);
        if (offset < days) break;
        offset -= days;
        if (leap && m == leap && !leap_now) {
            leap_now = true;
        } else {
            if (leap_now) leap_now = false;
            ++m;
        }
    }
    *year = y;
    *month = m;
    *day = offset + 1;
    *is_leap = leap_now;
}

static void format_lunar(const struct tm *date, char *out, size_t out_size)
{
    if (date->tm_year + 1900 < 1900 || date->tm_year + 1900 >= 2100) {
        strlcpy(out, "Lunar --", out_size);
        return;
    }
    int year, month, day;
    bool leap;
    lunar_from_date(date, &year, &month, &day, &leap);
    if (settings.language == APP_LANG_EN) {
        snprintf(out, out_size, "Lunar %s%d/%d", leap ? "Leap " : "", month, day);
        return;
    }
    static const char *months[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
    static const char *digits[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    char day_text[16];
    if (day == 10) strlcpy(day_text, "初十", sizeof(day_text));
    else if (day == 20) strlcpy(day_text, "二十", sizeof(day_text));
    else if (day == 30) strlcpy(day_text, "三十", sizeof(day_text));
    else if (day < 10) snprintf(day_text, sizeof(day_text), "初%s", digits[day - 1]);
    else if (day < 20) snprintf(day_text, sizeof(day_text), "十%s", digits[day - 11]);
    else snprintf(day_text, sizeof(day_text), "廿%s", digits[day - 21]);
    snprintf(out, out_size, "农历%s%s月%s", leap ? "闰" : "", months[month - 1], day_text);
}

static void style_set_colors(bool dark)
{
    lv_color_t bg = lv_color_hex(dark ? 0x111318 : 0xF8F9FF);
    lv_color_t card = lv_color_hex(dark ? 0x1A1C21 : 0xFFFFFF);
    lv_color_t tonal = lv_color_hex(dark ? 0x243240 : 0xDCEBFA);
    lv_color_t text = lv_color_hex(dark ? 0xE2E2E9 : 0x191C20);
    lv_color_t border = lv_color_hex(dark ? 0x44474E : 0xC3C7CF);
    lv_color_t primary = lv_color_hex(dark ? 0x91CDF4 : 0x00639A);
    lv_color_t on_primary = lv_color_hex(dark ? 0x00344F : 0xFFFFFF);

    lv_style_set_bg_color(&style_screen, bg);
    lv_style_set_bg_color(&style_page, bg);
    lv_style_set_text_color(&style_page, text);
    lv_style_set_bg_color(&style_card, card);
    lv_style_set_text_color(&style_card, text);
    lv_style_set_border_color(&style_card, border);
    lv_style_set_bg_color(&style_tonal, tonal);
    lv_style_set_text_color(&style_tonal, text);
    lv_style_set_bg_color(&style_eta_row, card);
    lv_style_set_text_color(&style_eta_row, text);
    lv_style_set_border_color(&style_eta_row, border);
    lv_style_set_bg_color(&style_route_badge, primary);
    lv_style_set_text_color(&style_route_badge, on_primary);
    lv_style_set_bg_color(&style_button, primary);
    lv_style_set_text_color(&style_button, on_primary);
    lv_style_set_border_color(&style_secondary_button, border);
    lv_style_set_text_color(&style_secondary_button, text);
    lv_style_set_bg_color(&style_status, dark ? lv_color_hex(0x123724) : lv_color_hex(0xD5F6E3));
    lv_style_set_text_color(&style_status, dark ? lv_color_hex(0x8BDBA8) : lv_color_hex(0x155D38));
    lv_obj_report_style_change(NULL);
}

static void apply_theme(void)
{
    bool dark = settings.theme == APP_THEME_DARK;
    if (settings.theme == APP_THEME_AUTO) {
        time_t now;
        struct tm local;
        time(&now);
        localtime_r(&now, &local);
        dark = local.tm_hour < 7 || local.tm_hour >= 19;
    }
    if (dark != dark_active) {
        dark_active = dark;
        style_set_colors(dark);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_ui_16, 0);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, bool primary)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_add_style(button, primary ? &style_button : &style_secondary_button, 0);
    lv_obj_set_height(button, 42);
    lv_obj_t *label = make_label(button, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_dropdown(lv_obj_t *parent)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_set_style_radius(dropdown, 0, 0);
    lv_obj_set_style_clip_corner(dropdown, false, 0);
    lv_obj_set_style_border_post(dropdown, false, 0);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (list) {
        lv_obj_set_style_radius(list, 0, 0);
        lv_obj_set_style_clip_corner(list, false, 0);
        lv_obj_set_style_border_post(list, false, 0);
    }
    return dropdown;
}

static lv_obj_t *make_setting_row(lv_obj_t *parent, lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &style_eta_row, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 58);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    *label_out = make_label(row, "");
    return row;
}

static void set_button_text(lv_obj_t *button, const char *text)
{
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) lv_label_set_text(label, text);
}

static size_t selected_flight_radius_index(void)
{
    for (size_t i = 0; i < sizeof(flight_radii_km) / sizeof(flight_radii_km[0]); ++i) {
        if (flight_radii_km[i] == settings.flight_radius_km) return i;
    }
    return 1;
}

static void update_flight_radius_control(void)
{
    lv_label_set_text_fmt(label_flight_radius_value, "%u km", settings.flight_radius_km);
    lv_obj_set_style_text_font(label_flight_radius_value, &lv_font_ui_16, 0);
    size_t index = selected_flight_radius_index();
    if (index == 0) lv_obj_add_state(button_flight_radius_minus, LV_STATE_DISABLED);
    else lv_obj_remove_state(button_flight_radius_minus, LV_STATE_DISABLED);
    if (index + 1 >= sizeof(flight_radii_km) / sizeof(flight_radii_km[0])) {
        lv_obj_add_state(button_flight_radius_plus, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(button_flight_radius_plus, LV_STATE_DISABLED);
    }
}

static size_t selected_flight_limit_index(void)
{
    for (size_t i = 0; i < sizeof(flight_limits) / sizeof(flight_limits[0]); ++i) {
        if (flight_limits[i] == settings.flight_max_aircraft) return i;
    }
    return 1;
}

static void update_flight_limit_control(void)
{
    lv_label_set_text_fmt(label_flight_limit_value,
                          settings.language == APP_LANG_ZH ? "%u 架" : "%u aircraft",
                          settings.flight_max_aircraft);
    lv_obj_set_style_text_font(label_flight_limit_value,
                               settings.language == APP_LANG_ZH ? &lv_font_chinese_16 : &lv_font_ui_16, 0);
    size_t index = selected_flight_limit_index();
    if (index == 0) lv_obj_add_state(button_flight_limit_minus, LV_STATE_DISABLED);
    else lv_obj_remove_state(button_flight_limit_minus, LV_STATE_DISABLED);
    if (index + 1 >= sizeof(flight_limits) / sizeof(flight_limits[0])) {
        lv_obj_add_state(button_flight_limit_plus, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(button_flight_limit_plus, LV_STATE_DISABLED);
    }
}

typedef struct {
    const lv_font_t *callsign_font;
    const lv_font_t *detail_font;
} flight_label_profile_t;

static flight_label_profile_t selected_flight_label_profile(void)
{
    static const flight_label_profile_t profiles[APP_FLIGHT_LABEL_SIZE_COUNT] = {
        {&lv_font_montserrat_26, &lv_font_montserrat_16},
        {&lv_font_montserrat_22, &lv_font_montserrat_12},
        {&lv_font_montserrat_20, &lv_font_montserrat_10},
        {&lv_font_montserrat_18, &lv_font_montserrat_10},
        {&lv_font_montserrat_16, &lv_font_montserrat_8},
    };
    uint8_t index = settings.flight_label_size;
    if (index >= APP_FLIGHT_LABEL_SIZE_COUNT) index = APP_FLIGHT_LABEL_LARGER;
    return profiles[index];
}

static void update_flight_label_size_control(void)
{
    if (!dropdown_flight_label_size) return;
    lv_dropdown_set_selected(dropdown_flight_label_size, settings.flight_label_size);
    flight_label_profile_t profile = selected_flight_label_profile();
    for (size_t i = 0; i < FLIGHT_MAX_AIRCRAFT; ++i) {
        if (!flight_items[i].label) continue;
        lv_obj_set_style_text_font(flight_items[i].label, profile.callsign_font, 0);
        lv_obj_set_style_text_font(flight_items[i].stats_label, profile.detail_font, 0);
    }
    last_flight_generation = UINT32_MAX;
}

static uint8_t selected_eta_direction(void)
{
    return settings.eta_station == APP_ETA_STATION_SHEUNG_SHUI
               ? settings.eta_direction_sheung_shui
               : settings.eta_direction_admiralty;
}

static void set_selected_eta_direction(uint8_t direction)
{
    if (settings.eta_station == APP_ETA_STATION_SHEUNG_SHUI) {
        settings.eta_direction_sheung_shui = direction;
    } else {
        settings.eta_direction_admiralty = direction;
    }
}

static void update_eta_direction_dropdown(void)
{
    if (!dropdown_eta_direction) return;
    bool sheung_shui = settings.eta_station == APP_ETA_STATION_SHEUNG_SHUI;
    if (settings.language == APP_LANG_ZH) {
        lv_dropdown_set_options(dropdown_eta_direction,
                                sheung_shui ? "往罗湖／落马洲\n往金钟" : "往坚尼地城\n往柴湾");
    } else {
        lv_dropdown_set_options(dropdown_eta_direction,
                                sheung_shui ? "to Lo Wu / Lok Ma Chau\nto Admiralty"
                                            : "to Kennedy Town\nto Chai Wan");
    }
    lv_dropdown_set_selected(dropdown_eta_direction, selected_eta_direction());
}

static void apply_eta_route(void)
{
    transit_manager_set_route((transit_station_t)settings.eta_station,
                              (transit_direction_t)selected_eta_direction());
    last_transit_generation = UINT32_MAX;
}

static void update_localized_text(void)
{
    if (title_clock) lv_label_set_text(title_clock, tr(TXT_DASHBOARD));
    if (wifi_title) lv_label_set_text(wifi_title, tr(TXT_WIFI));
    if (sync_title) lv_label_set_text(sync_title, tr(TXT_TIME_SYNC));
    lv_label_set_text(swipe_hint, "");
    lv_label_set_text(transit_title, settings.eta_station == APP_ETA_STATION_SHEUNG_SHUI
                                         ? "上水  SHEUNG SHUI"
                                         : "金鐘  ADMIRALTY");
    if (flight_title) {
        lv_label_set_text(flight_title, localized("实时航班罗盘", "LIVE AIR TRAFFIC"));
        lv_obj_set_style_text_font(flight_title, &lv_font_chinese_16, 0);
    }
    lv_label_set_text(title_settings, tr(TXT_SETTINGS));
    lv_label_set_text(label_language, tr(TXT_LANGUAGE));
    lv_label_set_text(label_theme, tr(TXT_THEME));
    lv_label_set_text(label_time_format, tr(TXT_TIME_FORMAT));
    lv_label_set_text(label_clock_style, tr(TXT_CLOCK_STYLE));
    lv_label_set_text(label_sleep, tr(TXT_SCREEN_SLEEP));
    lv_label_set_text(label_ntp, tr(TXT_NTP));
    lv_label_set_text(label_interval, tr(TXT_NTP_INTERVAL));
    lv_label_set_text(label_timezone, tr(TXT_TIMEZONE));
    lv_label_set_text(label_eta_station, tr(TXT_ETA_STATION));
    lv_label_set_text(label_eta_direction, tr(TXT_ETA_DIRECTION));
    lv_label_set_text(label_flight_radius, tr(TXT_FLIGHT_RADIUS));
    lv_label_set_text(label_flight_limit, tr(TXT_FLIGHT_LIMIT));
    lv_label_set_text(label_flight_label_size, tr(TXT_FLIGHT_LABEL_SIZE));
    lv_obj_set_style_text_font(label_flight_radius,
                               settings.language == APP_LANG_ZH ? &lv_font_chinese_16 : &lv_font_ui_16, 0);
    lv_obj_set_style_text_font(label_flight_limit,
                               settings.language == APP_LANG_ZH ? &lv_font_chinese_16 : &lv_font_ui_16, 0);
    lv_obj_set_style_text_font(label_flight_label_size,
                               settings.language == APP_LANG_ZH ? &lv_font_chinese_16 : &lv_font_ui_16, 0);
    lv_label_set_text(label_ip_title, tr(TXT_IP_ADDRESS));
    lv_label_set_text(label_next_title, tr(TXT_NEXT_SYNC));
    set_button_text(button_change_wifi, tr(TXT_CHANGE_WIFI));
    set_button_text(button_clear_wifi, tr(TXT_CLEAR_WIFI));
    set_button_text(button_portal, tr(TXT_SETUP_AP));
    set_button_text(button_sync, tr(TXT_SYNC_NOW));
    set_button_text(button_restart, tr(TXT_RESTART));
    set_button_text(button_reset, tr(TXT_RESET_SETTINGS));
    lv_label_set_text(wifi_overlay_title, tr(TXT_SCAN_WIFI));
    if (title_clock) lv_obj_set_style_text_font(title_clock, &lv_font_ui_20, 0);
    lv_obj_set_style_text_font(transit_title, &lv_font_ui_16, 0);
    lv_obj_set_style_text_font(title_settings, &lv_font_ui_20, 0);
    lv_obj_set_style_text_font(wifi_overlay_title, &lv_font_ui_20, 0);
    set_button_text(wifi_refresh_button, tr(TXT_REFRESH));
    set_button_text(wifi_portal_button, tr(TXT_SETUP_AP));
    lv_label_set_text(password_title, tr(TXT_PASSWORD));
    lv_textarea_set_placeholder_text(password_area, tr(TXT_PASSWORD));
    set_button_text(password_connect_button, tr(TXT_CONNECT));
    set_button_text(password_cancel_button, tr(TXT_CANCEL));

    if (settings.language == APP_LANG_ZH) {
        lv_dropdown_set_options(dropdown_language, "简体中文\nEnglish");
        lv_dropdown_set_options(dropdown_theme, "明亮\n深色\n自动");
        lv_dropdown_set_options(dropdown_time_format, "24 小时\n12 小时");
        lv_dropdown_set_options(dropdown_clock_style, "错位数字\n蓝色重叠");
        lv_dropdown_set_options(dropdown_sleep, "永不\n1 分钟\n3 分钟\n5 分钟\n10 分钟\n30 分钟");
        lv_dropdown_set_options(dropdown_interval, "1 小时\n6 小时\n12 小时\n24 小时");
        lv_dropdown_set_options(dropdown_eta_station, "金钟\n上水");
        lv_dropdown_set_options(dropdown_flight_label_size, "大\n较大\n中等\n较小\n小");
    } else {
        lv_dropdown_set_options(dropdown_language, "简体中文\nEnglish");
        lv_dropdown_set_options(dropdown_theme, "Light\nDark\nAuto");
        lv_dropdown_set_options(dropdown_time_format, "24 hour\n12 hour");
        lv_dropdown_set_options(dropdown_clock_style, "Staggered\nBlue overlap");
        lv_dropdown_set_options(dropdown_sleep, "Never\n1 minute\n3 minutes\n5 minutes\n10 minutes\n30 minutes");
        lv_dropdown_set_options(dropdown_interval, "1 hour\n6 hours\n12 hours\n24 hours");
        lv_dropdown_set_options(dropdown_eta_station, "Admiralty\nSheung Shui");
        lv_dropdown_set_options(dropdown_flight_label_size, "Large\nLarger\nMedium\nSmaller\nSmall");
    }
    char tz_options[160] = {0};
    for (size_t i = 0; i < network_manager_timezone_count(); ++i) {
        if (i) strlcat(tz_options, "\n", sizeof(tz_options));
        strlcat(tz_options, network_manager_timezone_name(i, settings.language == APP_LANG_ZH), sizeof(tz_options));
    }
    lv_dropdown_set_options(dropdown_timezone, tz_options);
    lv_dropdown_set_selected(dropdown_language, settings.language);
    lv_dropdown_set_selected(dropdown_theme, settings.theme);
    lv_dropdown_set_selected(dropdown_time_format, settings.hour_24 ? 0 : 1);
    lv_dropdown_set_selected(dropdown_clock_style, settings.clock_style);
    static const uint8_t sleeps[] = {0, 1, 3, 5, 10, 30};
    for (size_t i = 0; i < sizeof(sleeps); ++i) if (sleeps[i] == settings.sleep_minutes) lv_dropdown_set_selected(dropdown_sleep, i);
    static const uint8_t intervals[] = {1, 6, 12, 24};
    for (size_t i = 0; i < sizeof(intervals); ++i) if (intervals[i] == settings.ntp_interval_hours) lv_dropdown_set_selected(dropdown_interval, i);
    lv_dropdown_set_selected(dropdown_timezone, settings.timezone_index);
    lv_dropdown_set_selected(dropdown_eta_station, settings.eta_station);
    update_flight_radius_control();
    update_flight_limit_control();
    update_flight_label_size_control();
    update_eta_direction_dropdown();
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_top_status_opa(void *var, int32_t value)
{
    (void)var;
    top_status_opa = value;
    if (top_status) lv_obj_set_style_opa(top_status, (lv_opa_t)value, 0);
}

static void animate_top_status(int32_t target)
{
    if (!top_status || top_status_opa == target) return;
    lv_anim_delete(top_status, set_top_status_opa);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, top_status);
    lv_anim_set_exec_cb(&animation, set_top_status_opa);
    lv_anim_set_values(&animation, top_status_opa, target);
    lv_anim_set_duration(&animation, 300);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

static void top_status_hide_timer_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    if (settings.clock_style == APP_CLOCK_STYLE_BLUE_OVERLAP && top_status_last_healthy) {
        animate_top_status(LV_OPA_TRANSP);
    }
}

static void show_top_status_for_connection(void)
{
    animate_top_status(LV_OPA_COVER);
    if (!top_status_hide_timer) return;
    lv_timer_reset(top_status_hide_timer);
    lv_timer_resume(top_status_hide_timer);
}

static void set_blue_clock_digit_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void blue_clock_digit_fade_out_completed(lv_anim_t *animation)
{
    blue_clock_digit_fade_t *fade = lv_anim_get_user_data(animation);
    lv_image_set_src(fade->object,
                     fade->index % 2 ? blue_clock_dark_images[fade->pending_digit]
                                     : blue_clock_light_images[fade->pending_digit]);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, fade->object);
    lv_anim_set_exec_cb(&fade_in, set_blue_clock_digit_opa);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_in, 220);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_linear);
    lv_anim_start(&fade_in);
}

static void transition_blue_clock_digit(size_t index, uint8_t next_digit)
{
    blue_clock_digit_fade_t *fade = &blue_clock_digit_fades[index];
    fade->pending_digit = next_digit;
    lv_anim_delete(fade->object, set_blue_clock_digit_opa);
    lv_obj_set_style_opa(fade->object, LV_OPA_COVER, 0);

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, fade->object);
    lv_anim_set_exec_cb(&fade_out, set_blue_clock_digit_opa);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_out, 220);
    lv_anim_set_path_cb(&fade_out, lv_anim_path_linear);
    lv_anim_set_user_data(&fade_out, fade);
    lv_anim_set_completed_cb(&fade_out, blue_clock_digit_fade_out_completed);
    lv_anim_start(&fade_out);
}

static void set_blue_clock_location_icon_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void update_blue_clock_location_icon_animation(bool pulsing)
{
    if (!blue_clock_location_icon || blue_clock_location_icon_is_pulsing == pulsing) return;

    blue_clock_location_icon_is_pulsing = pulsing;
    lv_anim_delete(blue_clock_location_icon, set_blue_clock_location_icon_opa);
    if (!pulsing) {
        lv_obj_set_style_opa(blue_clock_location_icon, LV_OPA_COVER, 0);
        return;
    }

    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, blue_clock_location_icon);
    lv_anim_set_exec_cb(&pulse, set_blue_clock_location_icon_opa);
    lv_anim_set_values(&pulse, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&pulse, 600);
    lv_anim_set_playback_duration(&pulse, 600);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_linear);
    lv_anim_start(&pulse);
}

static void apply_clock_style(void)
{
    bool blue = settings.clock_style == APP_CLOCK_STYLE_BLUE_OVERLAP;
    lv_obj_set_style_bg_color(clock_page, lv_color_hex(blue ? 0x000000 : 0xE7EDF0), 0);
    lv_obj_set_style_text_color(top_status, lv_color_hex(blue ? 0xD7DEE3 : 0x6B746F), 0);
    for (size_t i = 0; i < 4; ++i) {
        if (blue) {
            lv_obj_add_flag(clock_digits[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(blue_clock_digits[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_anim_delete(blue_clock_digits[i], set_blue_clock_digit_opa);
            lv_obj_set_style_opa(blue_clock_digits[i], LV_OPA_COVER, 0);
            lv_obj_remove_flag(clock_digits[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(blue_clock_digits[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (blue) {
        lv_obj_remove_flag(blue_clock_colon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(blue_clock_location_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(blue_clock_colon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blue_clock_location_row, LV_OBJ_FLAG_HIDDEN);
        if (top_status_hide_timer) lv_timer_pause(top_status_hide_timer);
        animate_top_status(LV_OPA_COVER);
    }
    top_status_initialized = false;
    lv_obj_move_foreground(top_status);
}

static void update_clock(void)
{
    time_t now;
    struct tm local;
    time(&now);
    localtime_r(&now, &local);
    int display_hour = local.tm_hour;
    if (!settings.hour_24) {
        display_hour %= 12;
        if (display_hour == 0) display_hour = 12;
    }
    const uint8_t next_digits[4] = {
        (uint8_t)(display_hour / 10),
        (uint8_t)(display_hour % 10),
        (uint8_t)(local.tm_min / 10),
        (uint8_t)(local.tm_min % 10),
    };
    for (size_t i = 0; i < 4; ++i) {
        if (displayed_clock_digits[i] != next_digits[i]) {
            displayed_clock_digits[i] = next_digits[i];
            lv_image_set_src(clock_digits[i], clock_digit_images[next_digits[i]]);
        }
        if (displayed_blue_clock_digits[i] != next_digits[i]) {
            bool animate = displayed_blue_clock_digits[i] >= 0 &&
                           settings.clock_style == APP_CLOCK_STYLE_BLUE_OVERLAP;
            displayed_blue_clock_digits[i] = next_digits[i];
            if (animate) {
                transition_blue_clock_digit(i, next_digits[i]);
            } else {
                lv_image_set_src(blue_clock_digits[i],
                                 i % 2 ? blue_clock_dark_images[next_digits[i]]
                                       : blue_clock_light_images[next_digits[i]]);
                lv_obj_set_style_opa(blue_clock_digits[i], LV_OPA_COVER, 0);
            }
        }
    }
    char transit_time[8];
    strftime(transit_time, sizeof(transit_time), "%H:%M", &local);
    set_label_text_if_changed(transit_clock, transit_time);

}

static void format_short_time(time_t value, char *out, size_t size)
{
    if (!value) {
        strlcpy(out, "--", size);
        return;
    }
    struct tm local;
    localtime_r(&value, &local);
    strftime(out, size, "%m-%d %H:%M", &local);
}

static void update_network_labels(const network_status_t *net)
{
    char wifi_text[96];
    bool connected = net->state == NETWORK_STATE_CONNECTED;
    bool healthy = connected && net->time_synchronized;
    if (net->state == NETWORK_STATE_CONNECTED) {
        snprintf(wifi_text, sizeof(wifi_text), "%s\n%s", tr(TXT_CONNECTED), net->ssid);
        if (wifi_value) set_label_text_if_changed(wifi_value, wifi_text);
        set_label_text_if_changed(top_status, net->time_synchronized ? tr(TXT_CONNECTED) : tr(TXT_WAITING));
    } else if (net->state == NETWORK_STATE_CONNECTING || net->state == NETWORK_STATE_SCANNING) {
        if (wifi_value) set_label_text_if_changed(wifi_value, tr(TXT_CONNECTING));
        set_label_text_if_changed(top_status, tr(TXT_CONNECTING));
    } else {
        if (wifi_value) set_label_text_if_changed(wifi_value, tr(TXT_OFFLINE));
        set_label_text_if_changed(top_status, tr(TXT_OFFLINE));
    }
    if (settings.clock_style != APP_CLOCK_STYLE_BLUE_OVERLAP) {
        if (top_status_hide_timer) lv_timer_pause(top_status_hide_timer);
        animate_top_status(LV_OPA_COVER);
    } else if (!healthy) {
        if (top_status_hide_timer) lv_timer_pause(top_status_hide_timer);
        animate_top_status(LV_OPA_COVER);
    } else if (!top_status_initialized || !top_status_last_healthy) {
        show_top_status_for_connection();
    }
    top_status_initialized = true;
    top_status_last_healthy = healthy;
    if (sync_value) {
        set_label_text_if_changed(sync_value, net->time_synchronized ? tr(TXT_SYNCED) : tr(TXT_WAITING));
    }
    set_label_text_if_changed(label_ip_value, net->ip[0] ? net->ip : "--");
    const char *location = settings.language == APP_LANG_ZH ? net->location_zh : net->location_en;
    if (net->location_state == NETWORK_LOCATION_LOCATING) {
        location = settings.language == APP_LANG_ZH ? "定位中" : "Locating";
    } else if (net->location_state != NETWORK_LOCATION_AVAILABLE || location[0] == '\0') {
        location = settings.language == APP_LANG_ZH ? "未知位置" : "Location unavailable";
    }
    set_label_text_if_changed(blue_clock_location_text, location);
    update_blue_clock_location_icon_animation(
        net->location_updating && settings.clock_style == APP_CLOCK_STYLE_BLUE_OVERLAP);
    char next[32];
    format_short_time(net->next_sync, next, sizeof(next));
    set_label_text_if_changed(label_next_value, next);
}

static const char *weather_caption(uint8_t icon)
{
    if (icon >= 80) {
        return pids_localized("雷暴", "STORM");
    }
    if (icon >= 60) {
        return pids_localized("驟雨", "RAIN");
    }
    if (icon >= 52) {
        return pids_localized("多雲", "CLOUDY");
    }
    return pids_localized("晴", "SUNNY");
}

static const char *weather_icon_glyph(uint8_t icon)
{
    if (icon >= 80) return "☂";
    if (icon >= 60) return "☂";
    if (icon >= 52) return "☁";
    return "☀";
}

static const char *destination_text(uint8_t destination, uint8_t station, uint8_t direction)
{
    switch (destination) {
        case TRANSIT_DESTINATION_LO_WU:
            return pids_localized("羅湖", "LO WU");
        case TRANSIT_DESTINATION_LOK_MA_CHAU:
            return pids_localized("落馬洲", "LOK MA CHAU");
        case TRANSIT_DESTINATION_KENNEDY_TOWN:
            return pids_localized("堅尼地城", "KENNEDY TOWN");
        case TRANSIT_DESTINATION_CHAI_WAN:
            return pids_localized("柴灣", "CHAI WAN");
        case TRANSIT_DESTINATION_ADMIRALTY:
            return pids_localized("金鐘", "ADMIRALTY");
        default:
            if (station == TRANSIT_STATION_SHEUNG_SHUI) {
                return direction == TRANSIT_DIRECTION_PRIMARY
                           ? pids_localized("羅湖／落馬洲", "LO WU / LOK MA CHAU")
                           : pids_localized("金鐘", "ADMIRALTY");
            }
            return direction == TRANSIT_DIRECTION_PRIMARY
                       ? pids_localized("堅尼地城", "KENNEDY TOWN")
                       : pids_localized("柴灣", "CHAI WAN");
    }
}

static void update_transit_labels(const transit_status_t *transit)
{
    bool sheung_shui = transit->station == TRANSIT_STATION_SHEUNG_SHUI;
    bool primary_direction = transit->direction == TRANSIT_DIRECTION_PRIMARY;
    bool service_ended = transit->eta_valid && transit->train_count == 0;
    if (service_ended) {
        set_label_text_if_changed(transit_service_message,
                                  pids_localized("尾班車已開出", "Service has ended"));
        lv_obj_remove_flag(transit_service_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(transit_service_panel, LV_OBJ_FLAG_HIDDEN);
    }
    set_label_text_if_changed(transit_title, sheung_shui ? "上水  SHEUNG SHUI" : "金鐘  ADMIRALTY");
    if (sheung_shui) {
        set_label_text_if_changed(
            transit_direction,
            primary_direction
                ? pids_localized("東鐵綫  往羅湖／落馬洲", "EAST RAIL LINE  TO LO WU / LOK MA CHAU")
                : pids_localized("東鐵綫  往金鐘", "EAST RAIL LINE  TO ADMIRALTY"));
    } else {
        set_label_text_if_changed(
            transit_direction,
            primary_direction ? pids_localized("港島綫  往堅尼地城", "ISLAND LINE  TO KENNEDY TOWN")
                              : pids_localized("港島綫  往柴灣", "ISLAND LINE  TO CHAI WAN"));
    }

    char value[16];
    const char *unit;
    char platform[8];
    for (size_t i = 0; i < TRANSIT_MAX_TRAINS; ++i) {
        bool numeric_eta = false;
        if (transit->eta_valid && i < transit->train_count) {
            set_label_text_if_changed(transit_destinations[i],
                                      destination_text(transit->destinations[i], transit->station,
                                                       transit->direction));
            snprintf(platform, sizeof(platform), "%u",
                     transit->platforms[i] ? transit->platforms[i] : (sheung_shui ? 1 : 2));
            if (transit->eta_minutes[i] <= 0) {
                value[0] = '\0';
                unit = pids_localized("即將抵達", "Arriving");
            } else {
                snprintf(value, sizeof(value), "%d", transit->eta_minutes[i]);
                unit = pids_localized("分鐘", "min");
                numeric_eta = true;
            }
        } else if (i == 0 && !transit->eta_valid) {
            set_label_text_if_changed(
                transit_destinations[i],
                transit->eta_refreshing ? pids_localized("正在更新", "UPDATING")
                                        : pids_localized("暫無列車資料", "NO TRAIN DATA"));
            strlcpy(platform, "--", sizeof(platform));
            value[0] = '\0';
            unit = "--";
        } else {
            set_label_text_if_changed(transit_destinations[i], "");
            strlcpy(platform, "--", sizeof(platform));
            value[0] = '\0';
            unit = "--";
        }
        set_label_text_if_changed(transit_eta_badges[i], platform);
        set_label_text_if_changed(transit_eta_values[i], value);
        set_label_text_if_changed(transit_eta_units[i], unit);
        lv_obj_set_width(transit_eta_units[i], LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(transit_eta_units[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(transit_eta_units[i], LV_ALIGN_RIGHT_MID, -PIDS_ROW_EDGE_MARGIN, 4);
        if (numeric_eta) {
            lv_obj_align_to(transit_eta_values[i], transit_eta_units[i], LV_ALIGN_OUT_LEFT_MID,
                            -PIDS_ETA_VALUE_GAP, -4);
        }
    }

    if (transit->weather_valid) {
        snprintf(value, sizeof(value), "%d℃", transit->temperature_c);
        set_label_text_if_changed(transit_weather_icon, weather_icon_glyph(transit->weather_icon));
        set_label_text_if_changed(transit_weather, value);
        set_label_text_if_changed(transit_weather_caption, weather_caption(transit->weather_icon));
    } else {
        set_label_text_if_changed(transit_weather_icon, "--");
        set_label_text_if_changed(transit_weather, "--℃");
        set_label_text_if_changed(transit_weather_caption, "HKO");
    }

    char updated[16] = "--:--";
    if (transit->eta_updated) {
        struct tm local;
        localtime_r(&transit->eta_updated, &local);
        strftime(updated, sizeof(updated), "%H:%M:%S", &local);
    }
    char footer[96];
    if (transit->eta_stale) {
        snprintf(footer, sizeof(footer), "%s | %s %s | MTR / HKO",
                 localized("資料已過期", "DATA STALE"), localized("最後更新", "UPDATED"), updated);
    } else if (transit->eta_refreshing && !transit->eta_valid) {
        snprintf(footer, sizeof(footer), "%s | MTR / HKO", localized("正在更新", "UPDATING"));
    } else {
        snprintf(footer, sizeof(footer), "%s %s | MTR / HKO", localized("最後更新", "UPDATED"), updated);
    }
    set_label_text_if_changed(transit_footer, footer);
}

static void flight_detail_timer_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    lv_obj_add_flag(flight_detail_panel, LV_OBJ_FLAG_HIDDEN);
}

static void show_flight_detail(size_t index)
{
    if (index >= displayed_flight_status.aircraft_count) return;
    const flight_aircraft_t *aircraft = &displayed_flight_status.aircraft[index];
    char line[96];
    set_label_text_if_changed(flight_detail_title, aircraft->callsign);
    if (aircraft->speed_valid) {
        snprintf(line, sizeof(line), "%s    %s %ld ft    %s %.0f kt",
                 aircraft->aircraft_type, localized("高度", "ALT"), (long)aircraft->altitude_ft,
                 localized("速度", "SPD"), aircraft->speed_kts);
    } else {
        snprintf(line, sizeof(line), "%s    %s %ld ft    %s -- kt",
                 aircraft->aircraft_type, localized("高度", "ALT"), (long)aircraft->altitude_ft,
                 localized("速度", "SPD"));
    }
    set_label_text_if_changed(flight_detail_line_one, line);
    char heading[16] = "--";
    char vertical_rate[20] = "--";
    if (aircraft->heading_valid) snprintf(heading, sizeof(heading), "%03.0f", aircraft->heading_deg);
    if (aircraft->vertical_rate_valid) {
        snprintf(vertical_rate, sizeof(vertical_rate), "%+ld", (long)aircraft->vertical_rate_fpm);
    }
    snprintf(line, sizeof(line), "%s %s DEG    %s %s fpm",
             localized("航向", "HDG"), heading, localized("垂直速度", "V/S"), vertical_rate);
    set_label_text_if_changed(flight_detail_line_two, line);
    char updated[16] = "--:--:--";
    if (displayed_flight_status.updated) {
        struct tm local;
        localtime_r(&displayed_flight_status.updated, &local);
        strftime(updated, sizeof(updated), "%H:%M:%S", &local);
    }
    snprintf(line, sizeof(line), "%s %.1f km    %s %s",
             localized("距离", "DIST"), aircraft->distance_km,
             localized("更新", "UPDATED"), updated);
    set_label_text_if_changed(flight_detail_line_three, line);
    lv_obj_remove_flag(flight_detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(flight_detail_panel);
    lv_timer_reset(flight_detail_timer);
    lv_timer_resume(flight_detail_timer);
}

static void flight_item_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    show_flight_detail(index);
}

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} flight_rect_t;

static int32_t flight_rect_overlap_area(flight_rect_t first, flight_rect_t second, int16_t padding)
{
    int32_t left = first.x > second.x - padding ? first.x : second.x - padding;
    int32_t top = first.y > second.y - padding ? first.y : second.y - padding;
    int32_t right = first.x + first.width < second.x + second.width + padding
                        ? first.x + first.width
                        : second.x + second.width + padding;
    int32_t bottom = first.y + first.height < second.y + second.height + padding
                         ? first.y + first.height
                         : second.y + second.height + padding;
    return right > left && bottom > top ? (right - left) * (bottom - top) : 0;
}

static int16_t clamp_flight_coordinate(int16_t value, int16_t minimum, int16_t maximum)
{
    if (maximum < minimum) return minimum;
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static bool point_inside_plane(float x, float y)
{
    static const float outline[][2] = {
        {0, -11}, {2, -7}, {2, -3}, {10, 1}, {10, 3}, {2, 2},
        {2, 7}, {5, 10}, {5, 11}, {0, 9}, {-5, 11}, {-5, 10},
        {-2, 7}, {-2, 2}, {-10, 3}, {-10, 1}, {-2, -3}, {-2, -7},
    };
    bool inside = false;
    size_t count = sizeof(outline) / sizeof(outline[0]);
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        float yi = outline[i][1];
        float yj = outline[j][1];
        if (((yi > y) != (yj > y)) &&
            (x < (outline[j][0] - outline[i][0]) * (y - yi) / (yj - yi) + outline[i][0])) {
            inside = !inside;
        }
    }
    return inside;
}

static bool init_flight_heading_images(void)
{
    if (flight_heading_pixels) return true;
    size_t pixels_per_image = FLIGHT_ICON_SIZE * FLIGHT_ICON_SIZE;
    flight_heading_pixels = heap_caps_calloc(FLIGHT_HEADING_IMAGE_COUNT * pixels_per_image,
                                             sizeof(uint32_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!flight_heading_pixels) {
        ESP_LOGE(TAG, "Unable to allocate shared aircraft heading images");
        return false;
    }

    uint32_t plane_color =
        (lv_color_to_u32(lv_color_hex(0x2ED3FF)) & 0x00ffffffU) | 0xff000000U;
    for (size_t image_index = 0; image_index < FLIGHT_HEADING_IMAGE_COUNT; ++image_index) {
        float angle = (float)image_index * FLIGHT_HEADING_STEP_DEG * 3.14159265f / 180.0f;
        float cosine = cosf(angle);
        float sine = sinf(angle);
        uint32_t *pixels = flight_heading_pixels + image_index * pixels_per_image;
        for (int y = 0; y < FLIGHT_ICON_SIZE; ++y) {
            for (int x = 0; x < FLIGHT_ICON_SIZE; ++x) {
                float screen_x = (float)x - 11.5f;
                float screen_y = (float)y - 11.5f;
                float model_x = screen_x * cosine + screen_y * sine;
                float model_y = -screen_x * sine + screen_y * cosine;
                pixels[y * FLIGHT_ICON_SIZE + x] =
                    point_inside_plane(model_x, model_y) ? plane_color : 0;
            }
        }
        lv_image_dsc_t *image = &flight_heading_images[image_index];
        memset(image, 0, sizeof(*image));
        image->header.magic = LV_IMAGE_HEADER_MAGIC;
        image->header.cf = LV_COLOR_FORMAT_ARGB8888;
        image->header.flags = LV_IMAGE_FLAGS_PREMULTIPLIED;
        image->header.w = FLIGHT_ICON_SIZE;
        image->header.h = FLIGHT_ICON_SIZE;
        image->header.stride = FLIGHT_ICON_SIZE * sizeof(uint32_t);
        image->data_size = pixels_per_image * sizeof(uint32_t);
        image->data = (const uint8_t *)pixels;
    }
    return true;
}

static void configure_argb_image(lv_image_dsc_t *image, uint32_t *pixels, uint16_t size)
{
    memset(image, 0, sizeof(*image));
    image->header.magic = LV_IMAGE_HEADER_MAGIC;
    image->header.cf = LV_COLOR_FORMAT_ARGB8888;
    image->header.flags = LV_IMAGE_FLAGS_PREMULTIPLIED;
    image->header.w = size;
    image->header.h = size;
    image->header.stride = size * sizeof(uint32_t);
    image->data_size = size * size * sizeof(uint32_t);
    image->data = (const uint8_t *)pixels;
}

static bool init_round_assets(void)
{
    if (round_assets_ready) return true;
    const uint16_t badge_size = 38;
    const uint16_t dot_size = 7;
    size_t badge_pixels = badge_size * badge_size;
    size_t dot_pixels = dot_size * dot_size;
    round_asset_pixels = heap_caps_calloc(badge_pixels + dot_pixels, sizeof(uint32_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!round_asset_pixels) {
        ESP_LOGE(TAG, "Unable to allocate round image assets");
        return false;
    }

    uint32_t badge_color =
        (lv_color_to_u32(lv_color_hex(0xB76550)) & 0x00ffffffU) | 0xff000000U;
    uint32_t dot_color =
        (lv_color_to_u32(lv_color_hex(0x1769E0)) & 0x00ffffffU) | 0xff000000U;
    float badge_center = ((float)badge_size - 1.0f) * 0.5f;
    float badge_radius_squared = badge_center * badge_center;
    for (uint16_t y = 0; y < badge_size; ++y) {
        for (uint16_t x = 0; x < badge_size; ++x) {
            float dx = x - badge_center;
            float dy = y - badge_center;
            if (dx * dx + dy * dy <= badge_radius_squared) {
                round_asset_pixels[y * badge_size + x] = badge_color;
            }
        }
    }
    uint32_t *dot_map = round_asset_pixels + badge_pixels;
    float dot_center = ((float)dot_size - 1.0f) * 0.5f;
    float dot_radius_squared = dot_center * dot_center;
    for (uint16_t y = 0; y < dot_size; ++y) {
        for (uint16_t x = 0; x < dot_size; ++x) {
            float dx = x - dot_center;
            float dy = y - dot_center;
            if (dx * dx + dy * dy <= dot_radius_squared) {
                dot_map[y * dot_size + x] = dot_color;
            }
        }
    }
    configure_argb_image(&transit_badge_background, round_asset_pixels, badge_size);
    configure_argb_image(&page_dot_background, dot_map, dot_size);
    round_assets_ready = true;
    return true;
}

static void update_flight_heading_icon(flight_item_widgets_t *item, bool heading_valid,
                                       float heading_deg)
{
    if (!flight_heading_pixels) return;
    uint8_t image_index = heading_valid
                              ? (uint8_t)((uint16_t)lroundf(heading_deg / FLIGHT_HEADING_STEP_DEG) %
                                          FLIGHT_HEADING_IMAGE_COUNT)
                              : 0;
    if (item->heading_image_index == image_index) return;
    lv_image_set_src(item->icon, &flight_heading_images[image_index]);
    item->heading_image_index = image_index;
}

static void set_flight_item_opa(void *var, int32_t value)
{
    flight_item_widgets_t *item = var;
    lv_obj_set_style_opa(item->icon, (lv_opa_t)value, 0);
    lv_obj_set_style_opa(item->label, (lv_opa_t)value, 0);
    lv_obj_set_style_opa(item->stats_label, (lv_opa_t)value, 0);
}

static bool flight_item_is_hidden(const flight_item_widgets_t *item)
{
    return lv_obj_has_flag(item->icon, LV_OBJ_FLAG_HIDDEN);
}

static void set_flight_item_hidden(flight_item_widgets_t *item, bool hidden)
{
    if (flight_item_is_hidden(item) == hidden) return;
    if (hidden) {
        lv_obj_add_flag(item->icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(item->label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(item->stats_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(item->icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(item->label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(item->stats_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_pending_flight_item(flight_item_widgets_t *item)
{
    flight_label_profile_t profile = selected_flight_label_profile();
    lv_obj_set_style_text_font(item->label, profile.callsign_font, 0);
    lv_obj_set_style_text_font(item->stats_label, profile.detail_font, 0);
    lv_obj_set_pos(item->icon, item->pending_icon_x, item->pending_icon_y);
    lv_obj_set_pos(item->label, item->pending_label_x, item->pending_label_y);
    lv_obj_set_size(item->label, item->pending_label_width, item->pending_label_height);
    lv_obj_set_pos(item->stats_label, item->pending_label_x,
                   item->pending_label_y + item->pending_label_height);
    lv_obj_set_size(item->stats_label, item->pending_stats_width, item->pending_stats_height);
    lv_obj_set_style_text_align(item->label, item->pending_text_align, 0);
    lv_obj_set_style_text_align(item->stats_label, item->pending_text_align, 0);
    update_flight_heading_icon(item, item->pending_heading_valid, item->pending_heading_deg);
    set_label_text_if_changed(item->label, item->pending_callsign);
    set_label_text_if_changed(item->stats_label, item->pending_stats);
    strlcpy(item->displayed_hex, item->pending_hex, sizeof(item->displayed_hex));
    item->displayed_heading_valid = item->pending_heading_valid;
    item->displayed_heading_deg = item->pending_heading_deg;
}

static void flight_item_fade_in_completed(lv_anim_t *animation)
{
    flight_item_widgets_t *item = lv_anim_get_user_data(animation);
    item->transition_running = false;
}

static void flight_item_fade_out_completed(lv_anim_t *animation)
{
    flight_item_widgets_t *item = lv_anim_get_user_data(animation);
    apply_pending_flight_item(item);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, item);
    lv_anim_set_exec_cb(&fade_in, set_flight_item_opa);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_in, 180);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_linear);
    lv_anim_set_user_data(&fade_in, item);
    lv_anim_set_completed_cb(&fade_in, flight_item_fade_in_completed);
    lv_anim_start(&fade_in);
}

static float heading_difference(float first, float second)
{
    float difference = fabsf(first - second);
    return difference > 180.0f ? 360.0f - difference : difference;
}

static void update_flight_item(flight_item_widgets_t *item, int16_t icon_x, int16_t icon_y,
                               int16_t label_x, int16_t label_y, int16_t label_width,
                               int16_t label_height, int16_t stats_width, int16_t stats_height,
                               lv_text_align_t text_align, const flight_aircraft_t *aircraft,
                               const char *stats)
{
    item->pending_icon_x = icon_x;
    item->pending_icon_y = icon_y;
    item->pending_label_x = label_x;
    item->pending_label_y = label_y;
    item->pending_label_width = label_width;
    item->pending_label_height = label_height;
    item->pending_stats_width = stats_width;
    item->pending_stats_height = stats_height;
    item->pending_text_align = text_align;
    item->pending_heading_valid = aircraft->heading_valid;
    item->pending_heading_deg = aircraft->heading_deg;
    strlcpy(item->pending_hex, aircraft->hex, sizeof(item->pending_hex));
    strlcpy(item->pending_callsign, aircraft->callsign, sizeof(item->pending_callsign));
    strlcpy(item->pending_stats, stats, sizeof(item->pending_stats));

    if (flight_item_is_hidden(item)) {
        apply_pending_flight_item(item);
        set_flight_item_opa(item, LV_OPA_COVER);
        set_flight_item_hidden(item, false);
        return;
    }
    if (item->transition_running) return;

    int32_t dx = icon_x - lv_obj_get_x(item->icon);
    int32_t dy = icon_y - lv_obj_get_y(item->icon);
    bool identity_changed = strcmp(item->displayed_hex, aircraft->hex) != 0;
    bool heading_jump = item->displayed_heading_valid && aircraft->heading_valid &&
                        heading_difference(item->displayed_heading_deg, aircraft->heading_deg) > 75.0f;
    if (identity_changed) {
        apply_pending_flight_item(item);
    } else if (heading_jump || dx * dx + dy * dy > 36 * 36) {
        item->transition_running = true;
        lv_anim_delete(item, set_flight_item_opa);
        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, item);
        lv_anim_set_exec_cb(&fade_out, set_flight_item_opa);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&fade_out, 180);
        lv_anim_set_path_cb(&fade_out, lv_anim_path_linear);
        lv_anim_set_user_data(&fade_out, item);
        lv_anim_set_completed_cb(&fade_out, flight_item_fade_out_completed);
        lv_anim_start(&fade_out);
    } else {
        apply_pending_flight_item(item);
    }
}

static void extrapolate_flight_position(const flight_aircraft_t *aircraft, time_t updated,
                                        float *distance_km, float *bearing_deg)
{
    *distance_km = aircraft->distance_km;
    *bearing_deg = aircraft->bearing_deg;
    if (!aircraft->speed_valid || !aircraft->heading_valid || updated == 0) return;

    double elapsed_seconds = difftime(time(NULL), updated);
    if (elapsed_seconds < 0.0) elapsed_seconds = 0.0;
    if (elapsed_seconds > 45.0) elapsed_seconds = 45.0;
    float bearing_radians = aircraft->bearing_deg * 3.14159265f / 180.0f;
    float heading_radians = aircraft->heading_deg * 3.14159265f / 180.0f;
    float north_km = aircraft->distance_km * cosf(bearing_radians);
    float east_km = aircraft->distance_km * sinf(bearing_radians);
    float travelled_km = aircraft->speed_kts * 1.852f * (float)elapsed_seconds / 3600.0f;
    north_km += travelled_km * cosf(heading_radians);
    east_km += travelled_km * sinf(heading_radians);
    *distance_km = hypotf(north_km, east_km);
    *bearing_deg = atan2f(east_km, north_km) * 180.0f / 3.14159265f;
    if (*bearing_deg < 0.0f) *bearing_deg += 360.0f;
}

static void update_flight_labels(const flight_status_t *flight, const network_status_t *network)
{
    displayed_flight_status = *flight;
    char text[160];
    snprintf(text, sizeof(text), localized("范围内 %u 架", "%u IN RANGE"), flight->total_aircraft);
    set_label_text_if_changed(flight_count, text);

    const char *message = NULL;
    if (!network->coordinates_valid || flight->state == FLIGHT_STATE_WAITING_LOCATION) {
        message = localized("无法确定位置\n连接 ESP32-Clock-Setup\n访问 192.168.4.1 设置位置",
                            "LOCATION REQUIRED\nJOIN ESP32-Clock-Setup\nOPEN 192.168.4.1");
    } else if (!flight->data_valid && flight->state == FLIGHT_STATE_LOADING) {
        message = localized("正在搜索附近航班", "SEARCHING FOR AIRCRAFT");
    } else if (!flight->data_valid && flight->state == FLIGHT_STATE_ERROR) {
        message = localized("航班数据暂时不可用", "AIRCRAFT DATA UNAVAILABLE");
    } else if (flight->data_valid && flight->aircraft_count == 0) {
        message = localized("范围内暂无飞行中的航班", "NO AIRBORNE TRAFFIC IN RANGE");
    }
    if (message) {
        set_label_text_if_changed(flight_message, message);
        lv_obj_remove_flag(flight_message, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(flight_message, LV_OBJ_FLAG_HIDDEN);
    }

    size_t aircraft_count = flight->aircraft_count < FLIGHT_MAX_AIRCRAFT
                                ? flight->aircraft_count
                                : FLIGHT_MAX_AIRCRAFT;
    int16_t icon_x[FLIGHT_MAX_AIRCRAFT] = {0};
    int16_t icon_y[FLIGHT_MAX_AIRCRAFT] = {0};
    char stats[FLIGHT_MAX_AIRCRAFT][48] = {{0}};
    flight_label_profile_t profile = selected_flight_label_profile();

    for (size_t i = 0; i < aircraft_count; ++i) {
        const flight_aircraft_t *aircraft = &flight->aircraft[i];
        float projected_distance;
        float projected_bearing;
        extrapolate_flight_position(aircraft, flight->updated, &projected_distance, &projected_bearing);
        float distance_ratio = flight->radius_km ? projected_distance / flight->radius_km : 0.0f;
        if (distance_ratio > 1.0f) distance_ratio = 1.0f;
        float radius = 20.0f + distance_ratio * 164.0f;
        float angle = projected_bearing * 3.14159265f / 180.0f;
        int16_t center_x = (int16_t)lroundf(FLIGHT_RADAR_WIDTH * 0.5f + sinf(angle) * radius);
        int16_t center_y = (int16_t)lroundf(FLIGHT_RADAR_HEIGHT * 0.5f - cosf(angle) * radius);
        icon_x[i] = clamp_flight_coordinate(center_x - FLIGHT_ICON_SIZE / 2, 0,
                                             FLIGHT_RADAR_WIDTH - FLIGHT_ICON_SIZE);
        icon_y[i] = clamp_flight_coordinate(center_y - FLIGHT_ICON_SIZE / 2, 0,
                                             FLIGHT_RADAR_HEIGHT - FLIGHT_ICON_SIZE);
        const char *type = aircraft->aircraft_type[0] ? aircraft->aircraft_type : "--";
        if (aircraft->speed_valid) {
            snprintf(stats[i], sizeof(stats[i]), "%s  %ldft / %.0fkt", type,
                     (long)aircraft->altitude_ft, aircraft->speed_kts);
        } else {
            snprintf(stats[i], sizeof(stats[i]), "%s  %ldft / --kt", type,
                     (long)aircraft->altitude_ft);
        }
    }

    flight_rect_t used_labels[FLIGHT_MAX_AIRCRAFT] = {{0}};
    size_t used_label_count = 0;
    for (size_t i = 0; i < aircraft_count; ++i) {
        const flight_aircraft_t *aircraft = &flight->aircraft[i];
        lv_point_t callsign_size;
        lv_point_t stats_size;
        lv_text_get_size(&callsign_size, aircraft->callsign, profile.callsign_font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&stats_size, stats[i], profile.detail_font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int16_t callsign_width = callsign_size.x + FLIGHT_LABEL_PADDING * 2;
        int16_t callsign_height = profile.callsign_font->line_height + 2;
        int16_t stats_width = stats_size.x + FLIGHT_LABEL_PADDING * 2;
        int16_t stats_height = profile.detail_font->line_height + 1;
        int16_t block_width = callsign_width > stats_width ? callsign_width : stats_width;
        int16_t block_height = callsign_height + stats_height;
        int16_t icon_center_x = icon_x[i] + FLIGHT_ICON_SIZE / 2;
        int16_t icon_center_y = icon_y[i] + FLIGHT_ICON_SIZE / 2;
        const int16_t candidates[][2] = {
            {icon_x[i] + FLIGHT_ICON_SIZE + 2, icon_center_y - block_height / 2},
            {icon_x[i] - block_width - 2, icon_center_y - block_height / 2},
            {icon_center_x - block_width / 2, icon_y[i] - block_height - 2},
            {icon_center_x - block_width / 2, icon_y[i] + FLIGHT_ICON_SIZE + 2},
            {icon_x[i] + FLIGHT_ICON_SIZE + 2, icon_y[i] - block_height + 8},
            {icon_x[i] + FLIGHT_ICON_SIZE + 2, icon_y[i] + 16},
            {icon_x[i] - block_width - 2, icon_y[i] - block_height + 8},
            {icon_x[i] - block_width - 2, icon_y[i] + 16},
        };
        flight_rect_t best = {0};
        int32_t best_overlap = INT32_MAX;
        for (size_t candidate_index = 0;
             candidate_index < sizeof(candidates) / sizeof(candidates[0]); ++candidate_index) {
            flight_rect_t candidate = {
                .x = clamp_flight_coordinate(candidates[candidate_index][0], 2,
                                             FLIGHT_RADAR_WIDTH - block_width - 2),
                .y = clamp_flight_coordinate(candidates[candidate_index][1], 2,
                                             FLIGHT_RADAR_HEIGHT - block_height - 2),
                .width = block_width,
                .height = block_height,
            };
            int32_t overlap = 0;
            for (size_t j = 0; j < used_label_count; ++j) {
                overlap += flight_rect_overlap_area(candidate, used_labels[j], 4);
            }
            for (size_t j = 0; j < aircraft_count; ++j) {
                if (j == i) continue;
                flight_rect_t icon = {icon_x[j], icon_y[j], FLIGHT_ICON_SIZE, FLIGHT_ICON_SIZE};
                overlap += flight_rect_overlap_area(candidate, icon, 2);
            }
            if (overlap < best_overlap) {
                best = candidate;
                best_overlap = overlap;
            }
            if (overlap == 0) break;
        }
        used_labels[used_label_count++] = best;
        int16_t label_center_x = best.x + best.width / 2;
        int16_t label_center_y = best.y + best.height / 2;
        int16_t horizontal_offset = label_center_x - icon_center_x;
        int16_t vertical_offset = label_center_y - icon_center_y;
        lv_text_align_t text_align = LV_TEXT_ALIGN_CENTER;
        if (abs(horizontal_offset) > abs(vertical_offset)) {
            text_align = horizontal_offset > 0 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT;
        }
        update_flight_item(&flight_items[i], icon_x[i], icon_y[i], best.x, best.y,
                           block_width, callsign_height, block_width, stats_height,
                           text_align, aircraft, stats[i]);
    }
    for (size_t i = aircraft_count; i < FLIGHT_MAX_AIRCRAFT; ++i) {
        lv_anim_delete(&flight_items[i], set_flight_item_opa);
        flight_items[i].transition_running = false;
        set_flight_item_hidden(&flight_items[i], true);
    }

    char updated[16] = "--:--:--";
    if (flight->updated) {
        struct tm local;
        localtime_r(&flight->updated, &local);
        strftime(updated, sizeof(updated), "%H:%M:%S", &local);
    }
    const char *location = settings.language == APP_LANG_ZH ? network->location_zh : network->location_en;
    if (!location[0]) location = localized("未知位置", "UNKNOWN LOCATION");
    if (flight->data_stale || (flight->state == FLIGHT_STATE_ERROR && flight->data_valid)) {
        snprintf(text, sizeof(text), "%s | %s | %u km | %s %s",
                 localized("数据已过期", "DATA STALE"), location, flight->radius_km,
                 localized("更新", "UPDATED"), updated);
    } else {
        snprintf(text, sizeof(text), "%s | %u km | %s %s", location, flight->radius_km,
                 localized("更新", "UPDATED"), updated);
    }
    set_label_text_if_changed(flight_footer, text);

}

static void hide_flight_map_labels(void)
{
    for (size_t i = 0; i < MAP_MAX_LABELS; ++i) {
        if (flight_map_labels[i]) lv_obj_add_flag(flight_map_labels[i], LV_OBJ_FLAG_HIDDEN);
        if (flight_map_label_echoes[i]) {
            lv_obj_add_flag(flight_map_label_echoes[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_flight_map_labels(const map_status_t *map)
{
    if (last_map_label_generation == map->label_generation) return;
    hide_flight_map_labels();
    flight_rect_t used[MAP_MAX_LABELS] = {{0}};
    size_t used_count = 0;
    const lv_font_t *font = map->english ? &lv_font_ui_14 : &lv_font_chinese_16;
    for (uint8_t i = 0; i < map->label_count && i < MAP_MAX_LABELS; ++i) {
        if (!map->labels[i].text[0]) continue;
        lv_point_t text_size;
        lv_text_get_size(&text_size, map->labels[i].text, font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int16_t width = text_size.x + 4;
        if (width > 124) width = 124;
        int16_t height = font->line_height + 2;
        flight_rect_t rect = {
            .x = clamp_flight_coordinate(map->labels[i].x - width / 2, 4,
                                         FLIGHT_RADAR_WIDTH - width - 4),
            .y = clamp_flight_coordinate(map->labels[i].y - height / 2, 4,
                                         FLIGHT_RADAR_HEIGHT - height - 4),
            .width = width,
            .height = height,
        };
        bool overlaps = false;
        for (size_t j = 0; j < used_count; ++j) {
            if (flight_rect_overlap_area(rect, used[j], 5) > 0) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) continue;
        lv_obj_t *label = flight_map_labels[used_count];
        lv_obj_t *echo = flight_map_label_echoes[used_count];
        set_label_text_if_changed(label, map->labels[i].text);
        set_label_text_if_changed(echo, map->labels[i].text);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_font(echo, font, 0);
        lv_obj_set_pos(label, rect.x, rect.y);
        lv_obj_set_pos(echo, rect.x + 1, rect.y);
        lv_obj_set_size(label, rect.width, rect.height);
        lv_obj_set_size(echo, rect.width, rect.height);
        lv_obj_remove_flag(echo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        used[used_count++] = rect;
    }
    last_map_label_generation = map->label_generation;
}

static void update_flight_map(void)
{
    map_status_t map;
    map_manager_get_status(&map);
    if (!map.data_valid || map.radius_km != settings.flight_radius_km ||
        map.english != (settings.language == APP_LANG_EN)) {
        if (!lv_obj_has_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN);
        }
        hide_flight_map_labels();
        last_map_label_generation = UINT32_MAX;
        return;
    }
    update_flight_map_labels(&map);
    if (map.generation == last_map_generation && flight_map_bytes) {
        if (lv_obj_has_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    uint8_t *new_bytes = NULL;
    size_t new_size = 0;
    if (map_manager_copy_image(map.generation, &new_bytes, &new_size) != ESP_OK) return;

    if (flight_map_bytes) {
        heap_caps_free(flight_map_bytes);
    }
    flight_map_bytes = new_bytes;
    memset(&flight_map_descriptor, 0, sizeof(flight_map_descriptor));
    flight_map_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    flight_map_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    flight_map_descriptor.header.w = MAP_IMAGE_WIDTH;
    flight_map_descriptor.header.h = MAP_IMAGE_HEIGHT;
    flight_map_descriptor.header.stride = MAP_IMAGE_WIDTH * 2;
    flight_map_descriptor.data_size = new_size;
    flight_map_descriptor.data = flight_map_bytes;
    lv_image_set_src(flight_map_image, &flight_map_descriptor);
    if (lv_obj_has_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN);
    }
    last_map_generation = map.generation;
}

static void save_settings(void)
{
    esp_err_t err = app_settings_save(&settings);
    if (err != ESP_OK) ESP_LOGW(TAG, "Unable to save settings: %s", esp_err_to_name(err));
}

static void show_wifi_setup(void);

static void settings_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    static const uint8_t sleeps[] = {0, 1, 3, 5, 10, 30};
    static const uint8_t intervals[] = {1, 6, 12, 24};

    if (target == slider_brightness && code == LV_EVENT_VALUE_CHANGED) {
        settings.brightness = lv_slider_get_value(slider_brightness);
        lv_label_set_text_fmt(label_brightness, "%s  %u%%", tr(TXT_BRIGHTNESS), settings.brightness);
        if (!screen_dimmed) app_set_backlight(settings.brightness);
    } else if (target == slider_brightness && code == LV_EVENT_RELEASED) {
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_language) {
        settings.language = lv_dropdown_get_selected(target);
        last_transit_generation = UINT32_MAX;
        last_flight_generation = UINT32_MAX;
        map_manager_set_config(settings.flight_radius_km, settings.language == APP_LANG_EN);
        update_localized_text();
        update_clock();
        save_settings();
    } else if (code == LV_EVENT_CLICKED && target == button_language_quick) {
        settings.language = settings.language == APP_LANG_ZH ? APP_LANG_EN : APP_LANG_ZH;
        last_transit_generation = UINT32_MAX;
        last_flight_generation = UINT32_MAX;
        map_manager_set_config(settings.flight_radius_km, settings.language == APP_LANG_EN);
        lv_dropdown_set_selected(dropdown_language, settings.language);
        update_localized_text();
        update_clock();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_theme) {
        settings.theme = lv_dropdown_get_selected(target);
        apply_theme();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_time_format) {
        settings.hour_24 = lv_dropdown_get_selected(target) == 0;
        update_clock();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_clock_style) {
        settings.clock_style = lv_dropdown_get_selected(target);
        apply_clock_style();
        update_clock();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_sleep) {
        settings.sleep_minutes = sleeps[lv_dropdown_get_selected(target)];
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_eta_station) {
        settings.eta_station = lv_dropdown_get_selected(target);
        update_eta_direction_dropdown();
        apply_eta_route();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_eta_direction) {
        set_selected_eta_direction(lv_dropdown_get_selected(target));
        apply_eta_route();
        save_settings();
    } else if (code == LV_EVENT_CLICKED &&
               (target == button_flight_radius_minus || target == button_flight_radius_plus)) {
        size_t index = selected_flight_radius_index();
        if (target == button_flight_radius_minus && index > 0) --index;
        if (target == button_flight_radius_plus &&
            index + 1 < sizeof(flight_radii_km) / sizeof(flight_radii_km[0])) ++index;
        settings.flight_radius_km = flight_radii_km[index];
        flight_manager_set_config(settings.flight_radius_km, settings.flight_max_aircraft);
        map_manager_set_config(settings.flight_radius_km, settings.language == APP_LANG_EN);
        update_flight_radius_control();
        save_settings();
    } else if (code == LV_EVENT_CLICKED &&
               (target == button_flight_limit_minus || target == button_flight_limit_plus)) {
        size_t index = selected_flight_limit_index();
        if (target == button_flight_limit_minus && index > 0) --index;
        if (target == button_flight_limit_plus && index + 1 < sizeof(flight_limits) / sizeof(flight_limits[0])) ++index;
        settings.flight_max_aircraft = flight_limits[index];
        flight_manager_set_config(settings.flight_radius_km, settings.flight_max_aircraft);
        update_flight_limit_control();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_flight_label_size) {
        settings.flight_label_size = lv_dropdown_get_selected(target);
        update_flight_label_size_control();
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == switch_ntp) {
        settings.ntp_enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
        network_manager_set_time_config(settings.ntp_enabled, settings.ntp_interval_hours, settings.timezone_index);
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_interval) {
        settings.ntp_interval_hours = intervals[lv_dropdown_get_selected(target)];
        network_manager_set_time_config(settings.ntp_enabled, settings.ntp_interval_hours, settings.timezone_index);
        save_settings();
    } else if (code == LV_EVENT_VALUE_CHANGED && target == dropdown_timezone) {
        settings.timezone_index = lv_dropdown_get_selected(target);
        network_manager_set_time_config(settings.ntp_enabled, settings.ntp_interval_hours, settings.timezone_index);
        update_clock();
        save_settings();
    } else if (code == LV_EVENT_CLICKED && target == button_change_wifi) {
        show_wifi_setup();
    } else if (code == LV_EVENT_CLICKED && target == button_clear_wifi) {
        network_manager_forget();
        show_wifi_setup();
    } else if (code == LV_EVENT_CLICKED && target == button_portal) {
        network_manager_start_portal();
        show_wifi_setup();
    } else if (code == LV_EVENT_CLICKED && target == button_sync) {
        network_manager_force_sync();
    } else if (code == LV_EVENT_CLICKED && target == transit_refresh_button) {
        transit_manager_refresh();
    } else if (code == LV_EVENT_LONG_PRESSED && target == button_restart) {
        esp_restart();
    } else if (code == LV_EVENT_LONG_PRESSED && target == button_reset) {
        app_settings_defaults(&settings);
        app_set_backlight(settings.brightness);
        lv_slider_set_value(slider_brightness, settings.brightness, LV_ANIM_ON);
        lv_obj_add_state(switch_ntp, LV_STATE_CHECKED);
        update_localized_text();
        apply_eta_route();
        flight_manager_set_config(settings.flight_radius_km, settings.flight_max_aircraft);
        map_manager_set_config(settings.flight_radius_km, settings.language == APP_LANG_EN);
        network_manager_set_time_config(settings.ntp_enabled, settings.ntp_interval_hours, settings.timezone_index);
        apply_theme();
        apply_clock_style();
        save_settings();
    }
}

static void connect_selected_wifi(void)
{
    const char *password = lv_textarea_get_text(password_area);
    network_manager_connect(selected_ssid, password);
    lv_textarea_set_text(password_area, "");
    lv_obj_add_flag(password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void password_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    if ((target == password_keyboard && code == LV_EVENT_READY) ||
        (target == password_connect_button && code == LV_EVENT_CLICKED)) {
        connect_selected_wifi();
    } else if ((target == password_keyboard && code == LV_EVENT_CANCEL) ||
               (target == password_cancel_button && code == LV_EVENT_CLICKED)) {
        lv_obj_add_flag(password_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_item_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const char *ssid = lv_event_get_user_data(event);
    strlcpy(selected_ssid, ssid, sizeof(selected_ssid));
    bool open = false;
    for (size_t i = 0; i < NETWORK_MAX_APS; ++i) {
        if (strcmp(wifi_ssids[i], ssid) == 0) {
            open = wifi_open[i];
            break;
        }
    }
    if (open) {
        network_manager_connect(selected_ssid, "");
        lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(password_title, "%s\n%s", tr(TXT_PASSWORD), selected_ssid);
        lv_obj_remove_flag(password_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(password_area);
    }
}

static void rebuild_wifi_list(const network_status_t *net)
{
    lv_obj_clean(wifi_list);
    memset(wifi_ssids, 0, sizeof(wifi_ssids));
    for (size_t i = 0; i < net->ap_count && i < NETWORK_MAX_APS; ++i) {
        strlcpy(wifi_ssids[i], net->aps[i].ssid, sizeof(wifi_ssids[i]));
        wifi_open[i] = net->aps[i].open;
        lv_obj_t *button = make_button(wifi_list, "", false);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_t *label = lv_obj_get_child(button, 0);
        lv_label_set_text_fmt(label, "%s    %d dBm%s", net->aps[i].ssid, net->aps[i].rssi,
                              net->aps[i].open ? "  OPEN" : "");
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 380);
        lv_obj_add_event_cb(button, wifi_item_event, LV_EVENT_CLICKED, wifi_ssids[i]);
    }
    if (net->ap_count == 0) {
        lv_obj_t *label = make_label(wifi_list, tr(TXT_CONNECTING));
        lv_obj_set_style_pad_all(label, 20, 0);
    }
}

static void wifi_overlay_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_obj_t *target = lv_event_get_target(event);
    if (target == wifi_refresh_button) {
        network_manager_scan();
    } else if (target == wifi_portal_button) {
        network_manager_start_portal();
        lv_label_set_text(wifi_overlay_status, tr(TXT_PORTAL_INFO));
    } else if (target == wifi_close_button) {
        lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_wifi_setup(void)
{
    lv_obj_remove_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_overlay_status, tr(TXT_PORTAL_INFO));
    network_manager_scan();
}

static void set_page_dots_opa(void *var, int32_t value)
{
    (void)var;
    page_dots_opa = value;
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_set_style_opa(page_dots[i], (lv_opa_t)value, 0);
    }
}

static void animate_page_dots(int32_t target)
{
    lv_anim_delete(page_dots[0], set_page_dots_opa);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, page_dots[0]);
    lv_anim_set_exec_cb(&animation, set_page_dots_opa);
    lv_anim_set_values(&animation, page_dots_opa, target);
    lv_anim_set_duration(&animation, 300);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

static void show_page_dots(void)
{
    if (page_dots_hide_timer) lv_timer_pause(page_dots_hide_timer);
    animate_page_dots(LV_OPA_COVER);
}

static void schedule_page_dots_hide(void)
{
    if (!page_dots_hide_timer) return;
    lv_timer_reset(page_dots_hide_timer);
    lv_timer_resume(page_dots_hide_timer);
}

static void page_dots_hide_timer_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    animate_page_dots(LV_OPA_TRANSP);
}

static void page_touch_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        show_page_dots();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        schedule_page_dots_hide();
    }
}

static void suspend_flight_visuals(void)
{
    for (size_t i = 0; i < FLIGHT_MAX_AIRCRAFT; ++i) {
        lv_anim_delete(&flight_items[i], set_flight_item_opa);
        flight_items[i].transition_running = false;
    }
}

static void viewport_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SCROLL_BEGIN) {
        viewport_scrolling = true;
        flight_page_active = false;
        suspend_flight_visuals();
        show_page_dots();
        return;
    }
    if (code != LV_EVENT_SCROLL_END) return;
    int32_t x = lv_obj_get_scroll_x(viewport);
    int page = (x + 240) / 480;
    if (page < 0) page = 0;
    if (page > 3) page = 3;
    viewport_scrolling = false;
    flight_page_active = page == 2;
    if (flight_page_active) {
        last_flight_generation = UINT32_MAX;
        last_flight_projection_tick = 0;
    } else {
        suspend_flight_visuals();
    }
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_style_image_opa(page_dots[i], i == page ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    schedule_page_dots_hide();
}

static void periodic_timer(lv_timer_t *timer)
{
    (void)timer;
    /* Keep a drag frame immutable. Updating labels, images, or map visibility while
       the full viewport is moving creates competing invalidation regions. */
    if (viewport_scrolling) return;

    update_clock();
    apply_theme();
    network_status_t net;
    network_manager_get_status(&net);
    update_network_labels(&net);
    if (net.scan_generation != last_scan_generation) {
        last_scan_generation = net.scan_generation;
        rebuild_wifi_list(&net);
    }
    transit_status_t transit;
    transit_manager_get_status(&transit);
    bool next_pids_english = (((lv_tick_get() - pids_language_started) / 10000U) & 1U) != 0;
    bool pids_language_changed = next_pids_english != pids_english;
    pids_english = next_pids_english;
    if (transit.generation != last_transit_generation || transit.eta_stale != last_transit_stale ||
        pids_language_changed) {
        last_transit_generation = transit.generation;
        last_transit_stale = transit.eta_stale;
        update_transit_labels(&transit);
    }
    flight_status_t flight;
    flight_manager_get_status(&flight);
    uint32_t now_tick = lv_tick_get();
    bool flight_snapshot_changed = flight.generation != last_flight_generation ||
                                   flight.data_stale != last_flight_stale ||
                                   net.location_generation != last_flight_location_generation;
    bool flight_projection_due = flight.data_valid &&
                                 now_tick - last_flight_projection_tick >= 5000U;
    if (flight_page_active && !viewport_scrolling &&
        (flight_snapshot_changed || flight_projection_due)) {
        last_flight_generation = flight.generation;
        last_flight_stale = flight.data_stale;
        last_flight_location_generation = net.location_generation;
        last_flight_projection_tick = now_tick;
        update_flight_labels(&flight, &net);
    }
    if (flight_page_active && !viewport_scrolling) update_flight_map();

    lv_display_t *display = lv_display_get_default();
    uint32_t inactive = lv_display_get_inactive_time(display);
    if (settings.sleep_minutes && inactive >= (uint32_t)settings.sleep_minutes * 60000U) {
        if (!screen_dimmed) {
            screen_dimmed = true;
            app_set_backlight(0);
        }
    } else if (screen_dimmed && inactive < 1000) {
        screen_dimmed = false;
        app_set_backlight(settings.brightness);
    }
}

static void init_styles(void)
{
    lv_style_init(&style_screen);
    lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
    lv_style_set_text_font(&style_screen, &lv_font_ui_16);
    lv_style_init(&style_page);
    lv_style_set_bg_opa(&style_page, LV_OPA_COVER);
    lv_style_set_border_width(&style_page, 0);
    lv_style_set_text_font(&style_page, &lv_font_ui_16);
    lv_style_set_radius(&style_page, 0);
    lv_style_set_pad_all(&style_page, 0);
    lv_style_init(&style_card);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 8);
    lv_style_set_pad_all(&style_card, 16);
    lv_style_init(&style_button);
    lv_style_set_bg_color(&style_button, lv_color_hex(0x1769E0));
    lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
    lv_style_set_text_color(&style_button, lv_color_hex(0xFFFFFF));
    lv_style_set_radius(&style_button, 6);
    lv_style_set_border_width(&style_button, 0);
    lv_style_init(&style_secondary_button);
    lv_style_set_bg_opa(&style_secondary_button, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_secondary_button, 1);
    lv_style_set_radius(&style_secondary_button, 6);
    lv_style_init(&style_status);
    lv_style_set_bg_opa(&style_status, LV_OPA_COVER);
    lv_style_set_radius(&style_status, 8);
    lv_style_set_pad_hor(&style_status, 10);
    lv_style_set_pad_ver(&style_status, 5);
    lv_style_set_text_font(&style_status, &lv_font_ui_14);
    lv_style_init(&style_tonal);
    lv_style_set_bg_opa(&style_tonal, LV_OPA_COVER);
    lv_style_set_border_width(&style_tonal, 0);
    lv_style_set_radius(&style_tonal, 8);
    lv_style_set_pad_all(&style_tonal, 12);
    lv_style_init(&style_eta_row);
    lv_style_set_bg_opa(&style_eta_row, LV_OPA_COVER);
    lv_style_set_border_width(&style_eta_row, 1);
    lv_style_set_radius(&style_eta_row, 8);
    lv_style_set_pad_hor(&style_eta_row, 14);
    lv_style_set_pad_ver(&style_eta_row, 8);
    lv_style_init(&style_route_badge);
    lv_style_set_bg_opa(&style_route_badge, LV_OPA_COVER);
    lv_style_set_border_width(&style_route_badge, 0);
    lv_style_set_radius(&style_route_badge, 0);
    lv_style_set_text_font(&style_route_badge, &lv_font_ui_16);
    dark_active = settings.theme == APP_THEME_DARK;
    style_set_colors(dark_active);
}

static void create_clock_page(void)
{
    lv_obj_clear_flag(clock_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(clock_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(clock_page, lv_color_hex(0xE7EDF0), 0);
    lv_obj_set_style_text_color(clock_page, lv_color_hex(0x087A5B), 0);

    top_status = make_label(clock_page, "");
    lv_obj_set_style_text_font(top_status, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(top_status, lv_color_hex(0x6B746F), 0);
    lv_obj_set_style_text_opa(top_status, LV_OPA_80, 0);
    lv_obj_align(top_status, LV_ALIGN_TOP_RIGHT, -8, 8);
    top_status_opa = LV_OPA_COVER;
    top_status_hide_timer = lv_timer_create(top_status_hide_timer_cb, 10000, NULL);
    lv_timer_pause(top_status_hide_timer);

    static const int16_t digit_x[4] = {-20, 100, 220, 340};
    static const int16_t digit_y[4] = {-8, 56, 8, 56};
    for (size_t i = 0; i < 4; ++i) {
        clock_digits[i] = lv_image_create(clock_page);
        lv_image_set_src(clock_digits[i], clock_digit_images[0]);
        lv_obj_set_pos(clock_digits[i], digit_x[i], digit_y[i]);
        displayed_clock_digits[i] = -1;
    }

    static const int16_t blue_digit_x[4] = {4, 104, 240, 336};
    for (size_t i = 0; i < 4; ++i) {
        blue_clock_digits[i] = lv_image_create(clock_page);
        lv_image_set_src(blue_clock_digits[i], i % 2 ? blue_clock_dark_images[0] : blue_clock_light_images[0]);
        lv_obj_set_pos(blue_clock_digits[i], blue_digit_x[i], 120);
        lv_obj_add_flag(blue_clock_digits[i], LV_OBJ_FLAG_HIDDEN);
        blue_clock_digit_fades[i] = (blue_clock_digit_fade_t) {
            .object = blue_clock_digits[i],
            .index = i,
            .pending_digit = 0,
        };
        displayed_blue_clock_digits[i] = -1;
    }
    blue_clock_colon = lv_image_create(clock_page);
    lv_image_set_src(blue_clock_colon, blue_clock_colon_image);
    lv_obj_set_pos(blue_clock_colon, 211, 140);
    lv_obj_add_flag(blue_clock_colon, LV_OBJ_FLAG_HIDDEN);

    blue_clock_location_row = lv_obj_create(clock_page);
    lv_obj_remove_style_all(blue_clock_location_row);
    lv_obj_set_size(blue_clock_location_row, 440, 24);
    lv_obj_set_pos(blue_clock_location_row, 20, 84);
    lv_obj_set_flex_flow(blue_clock_location_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(blue_clock_location_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(blue_clock_location_row, 5, 0);
    lv_obj_clear_flag(blue_clock_location_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(blue_clock_location_row, LV_OBJ_FLAG_HIDDEN);

    blue_clock_location_icon = lv_label_create(blue_clock_location_row);
    lv_label_set_text(blue_clock_location_icon, LV_SYMBOL_GPS);
    lv_obj_set_style_text_font(blue_clock_location_icon, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(blue_clock_location_icon, lv_color_hex(0xD7DEE3), 0);
    lv_obj_set_style_text_opa(blue_clock_location_icon, LV_OPA_80, 0);

    blue_clock_location_text = lv_label_create(blue_clock_location_row);
    lv_label_set_text(blue_clock_location_text, "");
    lv_obj_set_style_text_font(blue_clock_location_text, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(blue_clock_location_text, lv_color_hex(0xD7DEE3), 0);
    lv_obj_set_style_text_opa(blue_clock_location_text, LV_OPA_80, 0);
    lv_label_set_long_mode(blue_clock_location_text, LV_LABEL_LONG_CLIP);

    lv_obj_move_foreground(top_status);

    swipe_hint = make_label(clock_page, "");
    lv_obj_add_flag(swipe_hint, LV_OBJ_FLAG_HIDDEN);
}

static void create_transit_page(void)
{
    lv_obj_t *header = lv_obj_create(transit_page);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 440, 50);
    lv_obj_set_pos(header, 20, 12);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x102A67), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    transit_weather_icon = make_label(header, "--");
    lv_obj_set_pos(transit_weather_icon, 12, 8);
    lv_obj_set_style_text_font(transit_weather_icon, &lv_font_pids_28, 0);
    lv_obj_set_style_text_color(transit_weather_icon, lv_color_hex(0xFFFFFF), 0);

    transit_weather_caption = make_label(header, "HKO");
    lv_obj_set_pos(transit_weather_caption, 50, 17);
    lv_obj_set_style_text_font(transit_weather_caption, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(transit_weather_caption, lv_color_hex(0xFFFFFF), 0);

    transit_weather = make_label(header, "--℃");
    lv_obj_set_pos(transit_weather, 118, 13);
    lv_obj_set_style_text_font(transit_weather, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(transit_weather, lv_color_hex(0xFFFFFF), 0);

    transit_clock = make_label(header, "--:--");
    lv_obj_set_style_text_font(transit_clock, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(transit_clock, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(transit_clock, LV_ALIGN_RIGHT_MID, -14, 0);

    transit_direction = make_label(transit_page, "");
    lv_obj_set_pos(transit_direction, 20, 62);
    lv_obj_set_size(transit_direction, 440, 26);
    lv_label_set_long_mode(transit_direction, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(transit_direction, &lv_font_pids_16, 0);
    lv_obj_set_style_text_color(transit_direction, lv_color_hex(0x153665), 0);
    lv_obj_set_style_bg_color(transit_direction, lv_color_hex(0xD8EEFA), 0);
    lv_obj_set_style_bg_opa(transit_direction, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(transit_direction, 8, 0);
    lv_obj_set_style_pad_top(transit_direction, 3, 0);

    for (size_t i = 0; i < TRANSIT_MAX_TRAINS; ++i) {
        lv_obj_t *row = lv_obj_create(transit_page);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 440, 64);
        lv_obj_set_pos(row, 20, 88 + (int)i * 64);
        lv_obj_set_style_bg_color(row, lv_color_hex((i & 1U) ? 0x5CC8ED : 0xEDF7FF), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_text_color(row, lv_color_hex(0x092654), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        transit_destinations[i] = make_label(row, "");
        lv_obj_set_width(transit_destinations[i], 220);
        lv_label_set_long_mode(transit_destinations[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(transit_destinations[i], &lv_font_pids_28, 0);
        lv_obj_align(transit_destinations[i], LV_ALIGN_LEFT_MID, PIDS_ROW_EDGE_MARGIN, 0);

        lv_obj_t *badge = lv_image_create(row);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 38, 38);
        if (round_assets_ready) {
            lv_image_set_src(badge, &transit_badge_background);
        } else {
            lv_obj_set_style_bg_color(badge, lv_color_hex(0xB76550), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        }
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -150, 0);
        transit_eta_badges[i] = make_label(badge, "");
        lv_obj_set_style_text_font(transit_eta_badges[i], &lv_font_ui_20, 0);
        lv_obj_set_style_text_color(transit_eta_badges[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(transit_eta_badges[i]);

        transit_eta_values[i] = make_label(row, "--");
        lv_obj_set_width(transit_eta_values[i], 50);
        lv_obj_set_style_text_align(transit_eta_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(transit_eta_values[i], &lv_font_pids_28, 0);
        lv_obj_align(transit_eta_values[i], LV_ALIGN_RIGHT_MID, -76, 0);

        transit_eta_units[i] = make_label(row, "--");
        lv_obj_set_width(transit_eta_units[i], LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(transit_eta_units[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(transit_eta_units[i], &lv_font_pids_20, 0);
        lv_obj_align(transit_eta_units[i], LV_ALIGN_RIGHT_MID, -PIDS_ROW_EDGE_MARGIN, 4);
    }

    transit_service_panel = lv_obj_create(transit_page);
    lv_obj_remove_style_all(transit_service_panel);
    lv_obj_set_size(transit_service_panel, 440, 256);
    lv_obj_set_pos(transit_service_panel, 20, 88);
    lv_obj_set_style_bg_color(transit_service_panel, lv_color_hex(0xEDF7FF), 0);
    lv_obj_set_style_bg_opa(transit_service_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(transit_service_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(transit_service_panel, LV_OBJ_FLAG_HIDDEN);

    transit_service_message = make_label(transit_service_panel, "");
    lv_obj_set_width(transit_service_message, 400);
    lv_obj_set_style_text_align(transit_service_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(transit_service_message, &lv_font_pids_28, 0);
    lv_obj_set_style_text_color(transit_service_message, lv_color_hex(0x092654), 0);
    lv_obj_center(transit_service_message);

    transit_footer = make_label(transit_page, "");
    lv_obj_set_pos(transit_footer, 20, 354);
    lv_obj_set_width(transit_footer, 388);
    lv_label_set_long_mode(transit_footer, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(transit_footer, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(transit_footer, lv_color_hex(0x73777F), 0);

    transit_title = make_label(transit_page, "金鐘  ADMIRALTY");
    lv_obj_set_pos(transit_title, 20, 383);
    lv_obj_set_width(transit_title, 340);
    lv_obj_set_style_text_font(transit_title, &lv_font_ui_16, 0);

    transit_refresh_button = make_button(transit_page, LV_SYMBOL_REFRESH, false);
    lv_obj_set_size(transit_refresh_button, 42, 38);
    lv_obj_set_pos(transit_refresh_button, 418, 376);
    lv_obj_set_style_radius(transit_refresh_button, 0, 0);
    lv_obj_add_event_cb(transit_refresh_button, settings_event, LV_EVENT_CLICKED, NULL);
}

static void create_flight_page(void)
{
    lv_obj_clear_flag(flight_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(flight_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(flight_page, lv_color_hex(0x061017), 0);
    lv_obj_set_style_text_color(flight_page, lv_color_hex(0xD8F4FF), 0);

    flight_title = make_label(flight_page, localized("实时航班罗盘", "LIVE AIR TRAFFIC"));
    lv_obj_set_pos(flight_title, 18, 414);
    lv_obj_set_style_text_font(flight_title, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(flight_title, lv_color_hex(0x7EDAF1), 0);

    flight_count = make_label(flight_page, "--");
    lv_obj_set_width(flight_count, 190);
    lv_obj_set_pos(flight_count, 272, 414);
    lv_obj_set_style_text_align(flight_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(flight_count, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(flight_count, lv_color_hex(0x7EDAF1), 0);

    flight_compass = lv_obj_create(flight_page);
    lv_obj_remove_style_all(flight_compass);
    lv_obj_set_size(flight_compass, FLIGHT_RADAR_WIDTH, FLIGHT_RADAR_HEIGHT);
    lv_obj_set_pos(flight_compass, 0, 0);
    lv_obj_set_style_border_width(flight_compass, 2, 0);
    lv_obj_set_style_border_color(flight_compass, lv_color_hex(0x2ED3FF), 0);
    lv_obj_set_style_border_opa(flight_compass, LV_OPA_60, 0);
    lv_obj_set_style_border_post(flight_compass, false, 0);
    lv_obj_set_style_clip_corner(flight_compass, false, 0);
    lv_obj_set_style_bg_opa(flight_compass, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(flight_compass, LV_OBJ_FLAG_SCROLLABLE);

    flight_map_image = lv_image_create(flight_compass);
    lv_obj_remove_style_all(flight_map_image);
    lv_obj_set_size(flight_map_image, FLIGHT_RADAR_WIDTH, FLIGHT_RADAR_HEIGHT);
    lv_obj_set_pos(flight_map_image, 0, 0);
    lv_obj_set_style_image_opa(flight_map_image, LV_OPA_COVER, 0);
    lv_obj_clear_flag(flight_map_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(flight_map_image, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < MAP_MAX_LABELS; ++i) {
        flight_map_label_echoes[i] = make_label(flight_compass, "");
        lv_obj_remove_style_all(flight_map_label_echoes[i]);
        lv_label_set_long_mode(flight_map_label_echoes[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(flight_map_label_echoes[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(flight_map_label_echoes[i], lv_color_hex(0x86A8B2), 0);
        lv_obj_set_style_text_opa(flight_map_label_echoes[i], LV_OPA_40, 0);
        lv_obj_clear_flag(flight_map_label_echoes[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(flight_map_label_echoes[i], LV_OBJ_FLAG_HIDDEN);

        flight_map_labels[i] = make_label(flight_compass, "");
        lv_obj_remove_style_all(flight_map_labels[i]);
        lv_label_set_long_mode(flight_map_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(flight_map_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(flight_map_labels[i], lv_color_hex(0x86A8B2), 0);
        lv_obj_set_style_text_opa(flight_map_labels[i], LV_OPA_40, 0);
        lv_obj_clear_flag(flight_map_labels[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(flight_map_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *center = lv_obj_create(flight_compass);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, 6, 6);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_center(center);

    bool heading_images_ready = init_flight_heading_images();
    for (size_t i = 0; i < FLIGHT_MAX_AIRCRAFT; ++i) {
        flight_items[i].icon = lv_image_create(flight_compass);
        flight_items[i].heading_image_index = UINT8_MAX;
        if (heading_images_ready) lv_image_set_src(flight_items[i].icon, &flight_heading_images[0]);
        lv_obj_set_size(flight_items[i].icon, FLIGHT_ICON_SIZE, FLIGHT_ICON_SIZE);
        lv_obj_add_flag(flight_items[i].icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(flight_items[i].icon, flight_item_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        update_flight_heading_icon(&flight_items[i], false, 0.0f);

        flight_items[i].label = make_label(flight_compass, "");
        lv_label_set_long_mode(flight_items[i].label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(flight_items[i].label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(flight_items[i].label, lv_color_hex(0xD8F4FF), 0);
        lv_obj_set_style_bg_color(flight_items[i].label, lv_color_hex(0x061017), 0);
        lv_obj_set_style_bg_opa(flight_items[i].label, LV_OPA_70, 0);
        lv_obj_set_style_pad_hor(flight_items[i].label, FLIGHT_LABEL_PADDING, 0);
        lv_obj_add_flag(flight_items[i].label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(flight_items[i].label, flight_item_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        flight_items[i].stats_label = make_label(flight_compass, "");
        lv_label_set_long_mode(flight_items[i].stats_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(flight_items[i].stats_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(flight_items[i].stats_label, lv_color_hex(0x7EDAF1), 0);
        lv_obj_set_style_bg_color(flight_items[i].stats_label, lv_color_hex(0x061017), 0);
        lv_obj_set_style_bg_opa(flight_items[i].stats_label, LV_OPA_70, 0);
        lv_obj_set_style_pad_hor(flight_items[i].stats_label, FLIGHT_LABEL_PADDING, 0);
        lv_obj_add_flag(flight_items[i].stats_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(flight_items[i].stats_label, flight_item_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_add_flag(flight_items[i].icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(flight_items[i].label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(flight_items[i].stats_label, LV_OBJ_FLAG_HIDDEN);
    }

    flight_message = make_label(flight_compass, "");
    lv_obj_set_width(flight_message, 300);
    lv_label_set_long_mode(flight_message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(flight_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(flight_message, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(flight_message, lv_color_hex(0xB7D7E3), 0);
    lv_obj_center(flight_message);

    flight_detail_panel = lv_obj_create(flight_page);
    lv_obj_remove_style_all(flight_detail_panel);
    lv_obj_set_size(flight_detail_panel, 440, 112);
    lv_obj_set_pos(flight_detail_panel, 20, 288);
    lv_obj_set_style_bg_color(flight_detail_panel, lv_color_hex(0x102631), 0);
    lv_obj_set_style_bg_opa(flight_detail_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(flight_detail_panel, 1, 0);
    lv_obj_set_style_border_color(flight_detail_panel, lv_color_hex(0x2ED3FF), 0);
    lv_obj_set_style_pad_left(flight_detail_panel, 14, 0);
    lv_obj_clear_flag(flight_detail_panel, LV_OBJ_FLAG_SCROLLABLE);

    flight_detail_title = make_label(flight_detail_panel, "");
    lv_obj_set_pos(flight_detail_title, 0, 8);
    lv_obj_set_style_text_font(flight_detail_title, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(flight_detail_title, lv_color_hex(0x2ED3FF), 0);
    flight_detail_line_one = make_label(flight_detail_panel, "");
    lv_obj_set_pos(flight_detail_line_one, 0, 38);
    flight_detail_line_two = make_label(flight_detail_panel, "");
    lv_obj_set_pos(flight_detail_line_two, 0, 61);
    flight_detail_line_three = make_label(flight_detail_panel, "");
    lv_obj_set_pos(flight_detail_line_three, 0, 84);
    lv_obj_t *detail_lines[] = {flight_detail_line_one, flight_detail_line_two, flight_detail_line_three};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_set_width(detail_lines[i], 410);
        lv_obj_set_style_text_font(detail_lines[i], &lv_font_chinese_16, 0);
        lv_obj_set_style_text_color(detail_lines[i], lv_color_hex(0xD8F4FF), 0);
    }
    lv_obj_add_flag(flight_detail_panel, LV_OBJ_FLAG_HIDDEN);
    flight_detail_timer = lv_timer_create(flight_detail_timer_cb, 15000, NULL);
    lv_timer_pause(flight_detail_timer);

    flight_footer = make_label(flight_page, "");
    lv_obj_set_pos(flight_footer, 18, 440);
    lv_obj_set_width(flight_footer, 444);
    lv_label_set_long_mode(flight_footer, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(flight_footer, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(flight_footer, lv_color_hex(0x7EDAF1), 0);
}

static void create_settings_page(void)
{
    lv_obj_set_scroll_dir(settings_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(settings_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(settings_page, 18, 0);
    lv_obj_set_style_pad_right(settings_page, 18, 0);
    lv_obj_set_style_pad_top(settings_page, 16, 0);
    lv_obj_set_style_pad_bottom(settings_page, 36, 0);

    title_settings = make_label(settings_page, "");
    lv_obj_set_style_text_font(title_settings, &lv_font_ui_20, 0);
    lv_obj_set_style_pad_bottom(title_settings, 8, 0);

    lv_obj_t *row = make_setting_row(settings_page, &label_brightness);
    slider_brightness = lv_slider_create(row);
    lv_obj_set_width(slider_brightness, 170);
    lv_slider_set_range(slider_brightness, 5, 100);
    lv_slider_set_value(slider_brightness, settings.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_brightness, settings_event, LV_EVENT_ALL, NULL);

    row = make_setting_row(settings_page, &label_language);
    dropdown_language = make_dropdown(row);
    lv_obj_set_width(dropdown_language, 170);
    lv_obj_add_event_cb(dropdown_language, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_theme);
    dropdown_theme = make_dropdown(row);
    lv_obj_set_width(dropdown_theme, 170);
    lv_obj_add_event_cb(dropdown_theme, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_time_format);
    dropdown_time_format = make_dropdown(row);
    lv_obj_set_width(dropdown_time_format, 170);
    lv_obj_add_event_cb(dropdown_time_format, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_clock_style);
    dropdown_clock_style = make_dropdown(row);
    lv_obj_set_width(dropdown_clock_style, 170);
    lv_obj_add_event_cb(dropdown_clock_style, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_sleep);
    dropdown_sleep = make_dropdown(row);
    lv_obj_set_width(dropdown_sleep, 170);
    lv_obj_add_event_cb(dropdown_sleep, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_eta_station);
    dropdown_eta_station = make_dropdown(row);
    lv_obj_set_width(dropdown_eta_station, 170);
    lv_obj_add_event_cb(dropdown_eta_station, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_eta_direction);
    dropdown_eta_direction = make_dropdown(row);
    lv_obj_set_width(dropdown_eta_direction, 170);
    lv_obj_add_event_cb(dropdown_eta_direction, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_flight_radius);
    lv_obj_t *flight_radius_control = lv_obj_create(row);
    lv_obj_remove_style_all(flight_radius_control);
    lv_obj_set_size(flight_radius_control, 170, 42);
    lv_obj_set_flex_flow(flight_radius_control, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flight_radius_control, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button_flight_radius_minus = make_button(flight_radius_control, "-", false);
    lv_obj_set_width(button_flight_radius_minus, 42);
    lv_obj_add_event_cb(button_flight_radius_minus, settings_event, LV_EVENT_CLICKED, NULL);
    label_flight_radius_value = make_label(flight_radius_control, "");
    lv_obj_set_width(label_flight_radius_value, 82);
    lv_obj_set_style_text_align(label_flight_radius_value, LV_TEXT_ALIGN_CENTER, 0);
    button_flight_radius_plus = make_button(flight_radius_control, "+", false);
    lv_obj_set_width(button_flight_radius_plus, 42);
    lv_obj_add_event_cb(button_flight_radius_plus, settings_event, LV_EVENT_CLICKED, NULL);

    row = make_setting_row(settings_page, &label_flight_limit);
    lv_obj_t *flight_limit_control = lv_obj_create(row);
    lv_obj_remove_style_all(flight_limit_control);
    lv_obj_set_size(flight_limit_control, 170, 42);
    lv_obj_set_flex_flow(flight_limit_control, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flight_limit_control, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button_flight_limit_minus = make_button(flight_limit_control, "-", false);
    lv_obj_set_width(button_flight_limit_minus, 42);
    lv_obj_add_event_cb(button_flight_limit_minus, settings_event, LV_EVENT_CLICKED, NULL);
    label_flight_limit_value = make_label(flight_limit_control, "");
    lv_obj_set_width(label_flight_limit_value, 82);
    lv_obj_set_style_text_align(label_flight_limit_value, LV_TEXT_ALIGN_CENTER, 0);
    button_flight_limit_plus = make_button(flight_limit_control, "+", false);
    lv_obj_set_width(button_flight_limit_plus, 42);
    lv_obj_add_event_cb(button_flight_limit_plus, settings_event, LV_EVENT_CLICKED, NULL);

    row = make_setting_row(settings_page, &label_flight_label_size);
    dropdown_flight_label_size = make_dropdown(row);
    lv_obj_set_width(dropdown_flight_label_size, 170);
    lv_obj_add_event_cb(dropdown_flight_label_size, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_ntp);
    switch_ntp = lv_switch_create(row);
    if (settings.ntp_enabled) lv_obj_add_state(switch_ntp, LV_STATE_CHECKED);
    lv_obj_add_event_cb(switch_ntp, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_interval);
    dropdown_interval = make_dropdown(row);
    lv_obj_set_width(dropdown_interval, 170);
    lv_obj_add_event_cb(dropdown_interval, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_timezone);
    dropdown_timezone = make_dropdown(row);
    lv_obj_set_width(dropdown_timezone, 190);
    lv_obj_add_event_cb(dropdown_timezone, settings_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_setting_row(settings_page, &label_ip_title);
    label_ip_value = make_label(row, "--");
    row = make_setting_row(settings_page, &label_next_title);
    label_next_value = make_label(row, "--");

    button_change_wifi = make_button(settings_page, "", true);
    button_clear_wifi = make_button(settings_page, "", false);
    button_portal = make_button(settings_page, "", false);
    button_sync = make_button(settings_page, "", false);
    button_restart = make_button(settings_page, "", false);
    button_reset = make_button(settings_page, "", false);
    lv_obj_t *buttons[] = {button_change_wifi, button_clear_wifi, button_portal, button_sync, button_restart, button_reset};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_set_width(buttons[i], LV_PCT(100));
        lv_obj_add_event_cb(buttons[i], settings_event, LV_EVENT_CLICKED, NULL);
    }
    for (size_t i = 4; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        lv_obj_set_width(buttons[i], LV_PCT(100));
        lv_obj_add_event_cb(buttons[i], settings_event, LV_EVENT_LONG_PRESSED, NULL);
    }
}

static void create_wifi_overlay(void)
{
    wifi_overlay = lv_obj_create(screen);
    lv_obj_add_style(wifi_overlay, &style_page, 0);
    lv_obj_set_size(wifi_overlay, 480, 480);
    lv_obj_center(wifi_overlay);
    lv_obj_set_style_pad_all(wifi_overlay, 18, 0);

    wifi_overlay_title = make_label(wifi_overlay, "");
    lv_obj_set_style_text_font(wifi_overlay_title, &lv_font_chinese_16, 0);
    lv_obj_align(wifi_overlay_title, LV_ALIGN_TOP_LEFT, 0, 2);
    wifi_close_button = make_button(wifi_overlay, "X", false);
    lv_obj_set_size(wifi_close_button, 42, 38);
    lv_obj_align(wifi_close_button, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_add_event_cb(wifi_close_button, wifi_overlay_event, LV_EVENT_CLICKED, NULL);

    wifi_list = lv_obj_create(wifi_overlay);
    lv_obj_remove_style_all(wifi_list);
    lv_obj_set_size(wifi_list, 444, 292);
    lv_obj_set_pos(wifi_list, 0, 52);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list, 8, 0);
    lv_obj_set_scroll_dir(wifi_list, LV_DIR_VER);

    wifi_overlay_status = make_label(wifi_overlay, "");
    lv_obj_set_width(wifi_overlay_status, 444);
    lv_label_set_long_mode(wifi_overlay_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(wifi_overlay_status, lv_color_hex(0x718096), 0);
    lv_obj_set_pos(wifi_overlay_status, 0, 350);

    wifi_refresh_button = make_button(wifi_overlay, "", false);
    lv_obj_set_size(wifi_refresh_button, 120, 42);
    lv_obj_align(wifi_refresh_button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    wifi_portal_button = make_button(wifi_overlay, "", true);
    lv_obj_set_size(wifi_portal_button, 304, 42);
    lv_obj_align(wifi_portal_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(wifi_refresh_button, wifi_overlay_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(wifi_portal_button, wifi_overlay_event, LV_EVENT_CLICKED, NULL);

    password_panel = lv_obj_create(wifi_overlay);
    lv_obj_add_style(password_panel, &style_page, 0);
    lv_obj_set_size(password_panel, 480, 480);
    lv_obj_set_pos(password_panel, -18, -18);
    lv_obj_set_style_pad_all(password_panel, 18, 0);
    password_title = make_label(password_panel, "");
    lv_obj_set_width(password_title, 440);
    lv_obj_set_pos(password_title, 0, 10);
    password_area = lv_textarea_create(password_panel);
    lv_obj_set_size(password_area, 444, 54);
    lv_obj_set_pos(password_area, 0, 72);
    lv_textarea_set_one_line(password_area, true);
    lv_textarea_set_password_mode(password_area, true);
    password_cancel_button = make_button(password_panel, "", false);
    lv_obj_set_size(password_cancel_button, 130, 42);
    lv_obj_set_pos(password_cancel_button, 0, 140);
    password_connect_button = make_button(password_panel, "", true);
    lv_obj_set_size(password_connect_button, 296, 42);
    lv_obj_set_pos(password_connect_button, 148, 140);
    password_keyboard = lv_keyboard_create(password_panel);
    lv_obj_set_size(password_keyboard, 444, 270);
    lv_obj_align(password_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(password_keyboard, password_area);
    lv_obj_add_event_cb(password_keyboard, password_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(password_connect_button, password_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(password_cancel_button, password_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_overlay, LV_OBJ_FLAG_HIDDEN);
}

void clock_ui_start(const app_settings_t *initial_settings)
{
    settings = *initial_settings;
    pids_english = false;
    pids_language_started = lv_tick_get();
    init_styles();
    init_round_assets();
    screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_add_style(screen, &style_screen, 0);

    viewport = lv_obj_create(screen);
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, 480, 480);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(viewport, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(viewport, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_column(viewport, 0, 0);
    lv_obj_add_event_cb(viewport, viewport_event, LV_EVENT_ALL, NULL);

    clock_page = lv_obj_create(viewport);
    transit_page = lv_obj_create(viewport);
    flight_page = lv_obj_create(viewport);
    settings_page = lv_obj_create(viewport);
    lv_obj_t *pages[] = {clock_page, transit_page, flight_page, settings_page};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_add_style(pages[i], &style_page, 0);
        lv_obj_set_size(pages[i], 480, 480);
        lv_obj_add_flag(pages[i], LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_set_flex_grow(pages[i], 0);
    }

    create_clock_page();
    create_transit_page();
    create_flight_page();
    suspend_flight_visuals();
    create_settings_page();
    lv_obj_add_event_cb(clock_page, page_touch_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(transit_page, page_touch_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(flight_page, page_touch_event, LV_EVENT_ALL, NULL);

    for (size_t i = 0; i < 4; ++i) {
        page_dots[i] = lv_image_create(screen);
        lv_obj_remove_style_all(page_dots[i]);
        lv_obj_set_size(page_dots[i], 7, 7);
        if (round_assets_ready) lv_image_set_src(page_dots[i], &page_dot_background);
        lv_obj_set_style_image_opa(page_dots[i], i == 0 ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_opa(page_dots[i], LV_OPA_TRANSP, 0);
        lv_obj_align(page_dots[i], LV_ALIGN_BOTTOM_MID, (int)i * 14 - 21, -10);
    }
    page_dots_opa = LV_OPA_TRANSP;
    page_dots_hide_timer = lv_timer_create(page_dots_hide_timer_cb, 3000, NULL);
    lv_timer_pause(page_dots_hide_timer);

    create_wifi_overlay();
    update_localized_text();
    lv_label_set_text_fmt(label_brightness, "%s  %u%%", tr(TXT_BRIGHTNESS), settings.brightness);
    app_set_backlight(settings.brightness);
    apply_clock_style();
    update_clock();
    apply_theme();
    network_status_t net;
    network_manager_get_status(&net);
    update_network_labels(&net);
    transit_status_t transit;
    transit_manager_get_status(&transit);
    update_transit_labels(&transit);
    last_transit_generation = transit.generation;
    last_transit_stale = transit.eta_stale;
    flight_status_t flight;
    flight_manager_get_status(&flight);
    last_flight_generation = flight.generation;
    last_flight_stale = flight.data_stale;
    last_flight_location_generation = net.location_generation;
    if (!net.has_credentials) show_wifi_setup();
    lv_timer_create(periodic_timer, 500, NULL);
}
