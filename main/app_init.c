/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_st7701.h"
#include "lv_demos.h"
#include "lvgl_port.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_H_RES (480)
#define EXAMPLE_LCD_V_RES (480)
#define EXAMPLE_LCD_BIT_PER_PIXEL (18)
#define EXAMPLE_RGB_BIT_PER_PIXEL (16)
#define EXAMPLE_RGB_DATA_WIDTH (16)
#define EXAMPLE_RGB_BOUNCE_BUFFER_MIN_HEIGHT (24)
#define EXAMPLE_RGB_BOUNCE_BUFFER_HEIGHT \
    ((CONFIG_EXAMPLE_LCD_RGB_BOUNCE_BUFFER_HEIGHT < EXAMPLE_RGB_BOUNCE_BUFFER_MIN_HEIGHT) ? \
         EXAMPLE_RGB_BOUNCE_BUFFER_MIN_HEIGHT : CONFIG_EXAMPLE_LCD_RGB_BOUNCE_BUFFER_HEIGHT)
#define EXAMPLE_RGB_BOUNCE_BUFFER_SIZE (EXAMPLE_LCD_H_RES * EXAMPLE_RGB_BOUNCE_BUFFER_HEIGHT)

#define EXAMPLE_LCD_IO_SPI_CS   (GPIO_NUM_45)
#define EXAMPLE_LCD_IO_SPI_SCL  (GPIO_NUM_38)
#define EXAMPLE_LCD_IO_SPI_SDA  (GPIO_NUM_39)

#define EXAMPLE_LCD_IO_RGB_DISP (-1) // -1 if not used
#define EXAMPLE_LCD_IO_RGB_VSYNC (GPIO_NUM_21)
#define EXAMPLE_LCD_IO_RGB_HSYNC (GPIO_NUM_14)
#define EXAMPLE_LCD_IO_RGB_DE (GPIO_NUM_47)
#define EXAMPLE_LCD_IO_RGB_PCLK (GPIO_NUM_48)

#define EXAMPLE_LCD_IO_RGB_DATA0 (GPIO_NUM_0)
#define EXAMPLE_LCD_IO_RGB_DATA1 (GPIO_NUM_12)
#define EXAMPLE_LCD_IO_RGB_DATA2 (GPIO_NUM_11)
#define EXAMPLE_LCD_IO_RGB_DATA3 (GPIO_NUM_10)
#define EXAMPLE_LCD_IO_RGB_DATA4 (GPIO_NUM_9)
#define EXAMPLE_LCD_IO_RGB_DATA5 (GPIO_NUM_46)
#define EXAMPLE_LCD_IO_RGB_DATA6 (GPIO_NUM_3)
#define EXAMPLE_LCD_IO_RGB_DATA7 (GPIO_NUM_20)
#define EXAMPLE_LCD_IO_RGB_DATA8 (GPIO_NUM_19)
#define EXAMPLE_LCD_IO_RGB_DATA9 (GPIO_NUM_8)
#define EXAMPLE_LCD_IO_RGB_DATA10 (GPIO_NUM_18)
#define EXAMPLE_LCD_IO_RGB_DATA11 (GPIO_NUM_17)
#define EXAMPLE_LCD_IO_RGB_DATA12 (GPIO_NUM_16)
#define EXAMPLE_LCD_IO_RGB_DATA13 (GPIO_NUM_15)
#define EXAMPLE_LCD_IO_RGB_DATA14 (GPIO_NUM_7)
#define EXAMPLE_LCD_IO_RGB_DATA15 (GPIO_NUM_6)

#define EXAMPLE_LCD_IO_RST (-1)                // -1 if not used
#define EXAMPLE_PIN_NUM_BK_LIGHT (GPIO_NUM_13) // -1 if not used
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL (1)
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your touch spec ////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define TOUCH_HOST (I2C_NUM_0)
#define EXAMPLE_PIN_NUM_TOUCH_SCL (GPIO_NUM_4)
#define EXAMPLE_PIN_NUM_TOUCH_SDA (GPIO_NUM_5)
#define EXAMPLE_PIN_NUM_TOUCH_RST (-1) // -1 if not used
#define EXAMPLE_PIN_NUM_TOUCH_INT (-1) // -1 if not used

static const char *TAG = "app_init";
static bool backlight_ready;

void app_set_backlight(uint8_t percent)
{
    if (!backlight_ready) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (255U * percent) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

IRAM_ATTR static bool rgb_lcd_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    return lvgl_port_notify_rgb_vsync();
}

static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0B, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x37, 0x02}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x0F, 0x16, 0x0E, 0x11, 0x07, 0x09, 0x09, 0x08, 0x23, 0x05, 0x11, 0x0F, 0x28, 0x2D, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x0F, 0x16, 0x0E, 0x11, 0x07, 0x09, 0x08, 0x09, 0x23, 0x05, 0x11, 0x0F, 0x28, 0x2D, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x4D}, 1, 0},
    {0xB1, (uint8_t[]){0x33}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x4B}, 1, 0},
    {0xB7, (uint8_t[]){0x8C}, 1, 0},
    {0xB8, (uint8_t[]){0x20}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x02, 0xF0, 0x00, 0x00, 0x03, 0xF0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x10, 0x10, 0x40, 0x40, 0xF2, 0xF0, 0x00, 0x00, 0xF2, 0xF0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x07, 0xEF, 0xF0, 0xF0, 0x09, 0xF1, 0xF0, 0xF0, 0x03, 0xF3, 0xF0, 0xF0, 0x05, 0xED, 0xF0, 0xF0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0xF0, 0xF0, 0xF0, 0x0A, 0xF2, 0xF0, 0xF0, 0x04, 0xF4, 0xF0, 0xF0, 0x06, 0xEE, 0xF0, 0xF0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x78, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0x20, 0xF9, 0x87, 0x76, 0x65, 0x54, 0x4F, 0xFF, 0xFF, 0xF4, 0x45, 0x56, 0x67, 0x78, 0x9F, 0x02}, 16, 0},
    {0xEF, (uint8_t[]){0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 6, 0},

    {0x11, (uint8_t[]){0x00}, 0, 120}, // Sleep Out
    {0x29, (uint8_t[]){0x00}, 0, 0},   // Display On
};

void app_init(void)
{
    /* GPIO20 输出高（若与 EXAMPLE_LCD_IO_RGB_DATA7 同源，RGB 初始化后会由 LCD 外设接管该脚） */
    {
        gpio_config_t io20 = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << GPIO_NUM_20,
        };
        ESP_ERROR_CHECK(gpio_config(&io20));
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_20, 1));
    }

    if (EXAMPLE_PIN_NUM_BK_LIGHT >= 0)
    {
        ESP_LOGI(TAG, "Turn off LCD backlight");
        const ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
        const ledc_channel_config_t channel_cfg = {
            .gpio_num = EXAMPLE_PIN_NUM_BK_LIGHT,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
        backlight_ready = true;
    }

    ESP_LOGI(TAG, "Install 3-wire SPI panel IO");
    spi_line_config_t line_config = {
        .cs_io_type = IO_TYPE_GPIO,
        .cs_gpio_num = EXAMPLE_LCD_IO_SPI_CS,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = EXAMPLE_LCD_IO_SPI_SCL,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = EXAMPLE_LCD_IO_SPI_SDA,
        .io_expander = NULL,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_config = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST7701 panel driver");
    esp_lcd_panel_handle_t lcd_handle = NULL;
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .psram_trans_align = 64,
        .data_width = EXAMPLE_RGB_DATA_WIDTH,
        .bits_per_pixel = EXAMPLE_RGB_BIT_PER_PIXEL,
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,
        .data_gpio_nums = {
            EXAMPLE_LCD_IO_RGB_DATA0,
            EXAMPLE_LCD_IO_RGB_DATA1,
            EXAMPLE_LCD_IO_RGB_DATA2,
            EXAMPLE_LCD_IO_RGB_DATA3,
            EXAMPLE_LCD_IO_RGB_DATA4,
            EXAMPLE_LCD_IO_RGB_DATA5,
            EXAMPLE_LCD_IO_RGB_DATA6,
            EXAMPLE_LCD_IO_RGB_DATA7,
            EXAMPLE_LCD_IO_RGB_DATA8,
            EXAMPLE_LCD_IO_RGB_DATA9,
            EXAMPLE_LCD_IO_RGB_DATA10,
            EXAMPLE_LCD_IO_RGB_DATA11,
            EXAMPLE_LCD_IO_RGB_DATA12,
            EXAMPLE_LCD_IO_RGB_DATA13,
            EXAMPLE_LCD_IO_RGB_DATA14,
            EXAMPLE_LCD_IO_RGB_DATA15,
        },
        .timings ={                                 \
        .pclk_hz = 14 * 1000 * 1000,                \
        .h_res = 480,                               \
        .v_res = 480,                               \
        .hsync_pulse_width = 10,                    \
        .hsync_back_porch = 10,                     \
        .hsync_front_porch = 20,                    \
        .vsync_pulse_width = 10,                    \
        .vsync_back_porch = 10,                     \
        .vsync_front_porch = 10,                    \
        .flags.pclk_active_neg = false,             \
        },
        .flags.fb_in_psram = true,
        .num_fbs = LVGL_PORT_LCD_RGB_BUFFER_NUMS,
        .bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE,
    };
    rgb_config.timings.h_res = EXAMPLE_LCD_H_RES;
    rgb_config.timings.v_res = EXAMPLE_LCD_V_RES;
    st7701_vendor_config_t vendor_config = {
        .rgb_config = &rgb_config,
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .auto_del_panel_io = 0,
            .mirror_by_cmd = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_LCD_IO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = EXAMPLE_LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io_handle, &panel_config, &lcd_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_handle));
    esp_lcd_panel_disp_on_off(lcd_handle, true);

    esp_lcd_touch_handle_t tp_handle = NULL;
#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_FT5X06
    ESP_LOGI(TAG, "Initialize I2C bus (FT5x06)");
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400 * 1000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    /* 使用 legacy i2c_driver 时，总线时钟已在 i2c_config_t 中配置；此处 scl_speed_hz 必须为 0（ESP-IDF 5.5+）。 */
    const esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 0,
    };

    ESP_LOGI(TAG, "Initialize I2C panel IO (FT5x06)");
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle));

    ESP_LOGI(TAG, "Initialize FT5x06 touch controller");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp_handle));
#else
    ESP_LOGI(TAG, "FT5x06 touch disabled in menuconfig");
#endif

    ESP_ERROR_CHECK(lvgl_port_init(lcd_handle, tp_handle));

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
#if EXAMPLE_RGB_BOUNCE_BUFFER_SIZE > 0
        .on_bounce_frame_finish = rgb_lcd_on_vsync_event,
#else
        .on_vsync = rgb_lcd_on_vsync_event,
#endif
    };
    esp_lcd_rgb_panel_register_event_callbacks(lcd_handle, &cbs, NULL);

    if (EXAMPLE_PIN_NUM_BK_LIGHT >= 0)
    {
        ESP_LOGI(TAG, "Turn on LCD backlight");
        app_set_backlight(70);
    }

}
