#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "watchdog_types.h"

typedef struct {
    bool identity_verified;
    char detail[64];
} ez1_ble_result_t;

esp_err_t ez1_ble_init(void);
bool ez1_ble_ready(void);
esp_err_t ez1_ble_stop(const watchdog_config_t *config, ez1_ble_result_t *result);
