#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MAP_IMAGE_WIDTH 480
#define MAP_IMAGE_HEIGHT 400
#define MAP_MAX_LABELS 6
#define MAP_LABEL_TEXT_LENGTH 48

typedef struct {
    int16_t x;
    int16_t y;
    char text[MAP_LABEL_TEXT_LENGTH];
} map_label_t;

typedef enum {
    MAP_STATE_WAITING_LOCATION = 0,
    MAP_STATE_LOADING,
    MAP_STATE_READY,
    MAP_STATE_ERROR,
} map_state_t;

typedef struct {
    map_state_t state;
    bool data_valid;
    bool english;
    uint16_t radius_km;
    size_t image_size;
    uint32_t generation;
    uint32_t label_generation;
    uint8_t label_count;
    map_label_t labels[MAP_MAX_LABELS];
} map_status_t;

esp_err_t map_manager_init(uint16_t radius_km, bool english);
void map_manager_set_config(uint16_t radius_km, bool english);
void map_manager_get_status(map_status_t *status);
esp_err_t map_manager_copy_image(uint32_t generation, uint8_t **data, size_t *size);
