#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t network_http_lock_init(void);
bool network_http_lock(TickType_t timeout);
void network_http_unlock(void);
