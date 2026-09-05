#include "network_http_lock.h"

#include "freertos/semphr.h"

static SemaphoreHandle_t http_mutex;

esp_err_t network_http_lock_init(void)
{
    if (http_mutex) return ESP_OK;
    http_mutex = xSemaphoreCreateMutex();
    return http_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

bool network_http_lock(TickType_t timeout)
{
    return http_mutex && xSemaphoreTake(http_mutex, timeout) == pdTRUE;
}

void network_http_unlock(void)
{
    if (http_mutex) xSemaphoreGive(http_mutex);
}
