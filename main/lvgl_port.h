/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LVGL_PORT_H_RES (480)
#define LVGL_PORT_V_RES (480)
#define LVGL_PORT_TICK_PERIOD_MS (CONFIG_EXAMPLE_LVGL_PORT_TICK)

#define LVGL_PORT_MEM_ALIGNMENT 32
#define LVGL_PORT_DMA_BURST_SIZE 32

#define LVGL_PORT_TASK_MAX_DELAY_MS (CONFIG_EXAMPLE_LVGL_PORT_TASK_MAX_DELAY_MS)
#define LVGL_PORT_TASK_MIN_DELAY_MS (CONFIG_EXAMPLE_LVGL_PORT_TASK_MIN_DELAY_MS)
#define LVGL_PORT_TASK_STACK_SIZE (CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB * 1024)
#define LVGL_PORT_TASK_PRIORITY (CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY)
#define LVGL_PORT_TASK_CORE (CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE)

/** 与 app_init 中 RGB 面板 `num_fbs` 一致：双缓冲 + 直接模式 */
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS (2)

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle);

bool lvgl_port_lock(int timeout_ms);

void lvgl_port_unlock(void);

bool lvgl_port_notify_rgb_vsync(void);

#ifdef __cplusplus
}
#endif
