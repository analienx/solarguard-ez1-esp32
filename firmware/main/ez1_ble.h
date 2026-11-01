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
/* Fixed, verified production stop. Never returns with an uninitialized result. */
esp_err_t ez1_ble_stop(const watchdog_config_t *config, ez1_ble_result_t *result);
/* Read-only health check: scan for the EZ1_<device ID> advertisement, no connection. */
esp_err_t ez1_ble_check_advertisement(const watchdog_config_t *config,
                                      ez1_ble_result_t *result);
