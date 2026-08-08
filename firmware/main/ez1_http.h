#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "watchdog_types.h"

typedef struct {
    bool identity_verified;
    bool reachable;
    int max_power;
    bool override_detected;
    char detail[64];
} ez1_result_t;

esp_err_t ez1_http_probe(const watchdog_config_t *config, ez1_result_t *result);
esp_err_t ez1_http_apply_failsafe(const watchdog_config_t *config,
                                  ez1_result_t *result);
esp_err_t ez1_http_enforce_failsafe(const watchdog_config_t *config,
                                    ez1_result_t *result);