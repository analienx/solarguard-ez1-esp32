#include "config_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_MAGIC 0x455A3157u
#define CONFIG_SCHEMA 1u
#define NAMESPACE "watchdog"

typedef struct {
    uint32_t magic;
    watchdog_config_t config;
    uint32_t crc;
} stored_config_t;

static uint32_t config_crc(const stored_config_t *stored) {
    return esp_crc32_le(UINT32_MAX, (const uint8_t *)stored,
                        offsetof(stored_config_t, crc));
}

static bool terminated(const char *value, size_t size) {
    return memchr(value, '\0', size) != NULL;
}

bool config_store_validate(const watchdog_config_t *config) {
    if (config->schema_version != CONFIG_SCHEMA ||
        !terminated(config->wifi_ssid, sizeof(config->wifi_ssid)) ||
        !terminated(config->wifi_password, sizeof(config->wifi_password)) ||
        !terminated(config->ez1_host, sizeof(config->ez1_host)) ||
        !terminated(config->ez1_device_id, sizeof(config->ez1_device_id)) ||
        !terminated(config->auth_secret, sizeof(config->auth_secret))) {
        return false;
    }
    size_t secret_len = strnlen(config->auth_secret, sizeof(config->auth_secret));
    if (config->wifi_ssid[0] == '\0' || config->ez1_host[0] == '\0' ||
        config->ez1_device_id[0] == '\0' || secret_len != WATCHDOG_SECRET_HEX_LEN ||
        strpbrk(config->ez1_host, "/?#@") != NULL ||
        config->lease_interval_s == 0 ||
        config->lease_interval_s >= config->warning_s ||
        config->warning_s >= config->trip_s || config->trip_s > 3600) {
        return false;
    }
    for (size_t i = 0; i < secret_len; ++i) {
        char c = config->auth_secret[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

esp_err_t config_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_store_load(watchdog_config_t *config) {
    nvs_handle_t handle = 0;
    stored_config_t stored = {0};
    size_t size = sizeof(stored);
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, "config", &stored, &size);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    if (size != sizeof(stored) || stored.magic != CONFIG_MAGIC ||
        stored.crc != config_crc(&stored) || !config_store_validate(&stored.config)) {
        return ESP_ERR_INVALID_CRC;
    }
    *config = stored.config;
    return ESP_OK;
}

esp_err_t config_store_save(const watchdog_config_t *config) {
    if (!config_store_validate(config)) return ESP_ERR_INVALID_ARG;
    stored_config_t stored = {.magic = CONFIG_MAGIC, .config = *config};
    stored.crc = config_crc(&stored);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_blob(handle, "config", &stored, sizeof(stored));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err;
}

esp_err_t config_store_get_latch(bool *latched, uint32_t *latch_id) {
    nvs_handle_t handle = 0;
    uint8_t flag = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *latched = false;
        *latch_id = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    esp_err_t flag_err = nvs_get_u8(handle, "latched", &flag);
    esp_err_t id_err = nvs_get_u32(handle, "latch_id", latch_id);
    nvs_close(handle);
    if (flag_err == ESP_ERR_NVS_NOT_FOUND) flag_err = ESP_OK;
    if (id_err == ESP_ERR_NVS_NOT_FOUND) {
        *latch_id = 0;
        id_err = ESP_OK;
    }
    *latched = flag != 0;
    return flag_err != ESP_OK ? flag_err : id_err;
}

esp_err_t config_store_set_latch(bool latched, uint32_t latch_id) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, "latched", latched ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(handle, "latch_id", latch_id);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err;
}

esp_err_t config_store_next_boot_id(uint32_t *boot_id) {
    nvs_handle_t handle = 0;
    uint32_t value = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(handle, "boot_id", &value);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_u32(handle, "boot_id", ++value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    *boot_id = value;
    return err;
}

esp_err_t config_store_get_override_count(uint32_t *count) {
    nvs_handle_t handle = 0;
    *count = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_u32(handle, "overrides", count);
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t config_store_set_override_count(uint32_t count) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u32(handle, "overrides", count);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err;
}
