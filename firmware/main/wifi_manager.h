#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "watchdog_types.h"

esp_err_t wifi_manager_start(const watchdog_config_t *config);
bool wifi_manager_connected(void);
int wifi_manager_rssi(void);