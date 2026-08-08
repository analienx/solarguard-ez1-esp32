#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "watchdog_types.h"

esp_err_t lease_server_start(const watchdog_config_t *config, QueueHandle_t events,
                             const watchdog_status_t *status,
                             SemaphoreHandle_t status_mutex);