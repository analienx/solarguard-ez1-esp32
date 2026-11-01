#pragma once

#include "esp_err.h"
#include "watchdog_types.h"

esp_err_t config_store_init(bool *erased);
esp_err_t config_store_load(watchdog_config_t *config);
esp_err_t config_store_save(const watchdog_config_t *config);
esp_err_t config_store_get_latch(bool *latched, uint32_t *latch_id);
esp_err_t config_store_set_latch(bool latched, uint32_t latch_id);
esp_err_t config_store_next_boot_id(uint32_t *boot_id);
esp_err_t config_store_get_override_count(uint32_t *count);
esp_err_t config_store_set_override_count(uint32_t count);
esp_err_t config_store_get_abnormal_resets(uint8_t *count);
esp_err_t config_store_set_abnormal_resets(uint8_t count);
bool config_store_validate(const watchdog_config_t *config);
