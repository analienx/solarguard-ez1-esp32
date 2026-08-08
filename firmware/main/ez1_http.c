#include "ez1_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESPONSE_CAPACITY 2048

_Static_assert(WATCHDOG_FAILSAFE_WATTS == 30,
               "production watchdog fail-safe must remain fixed at 30 W");

static const char *TAG = "ez1_http";

typedef struct {
    char data[RESPONSE_CAPACITY];
    size_t length;
    bool overflow;
} response_buffer_t;

static esp_err_t on_http_event(esp_http_client_event_t *event) {
    response_buffer_t *buffer = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t remaining = RESPONSE_CAPACITY - 1 - buffer->length;
        if ((size_t)event->data_len > remaining) {
            buffer->overflow = true;
            return ESP_FAIL;
        }
        memcpy(buffer->data + buffer->length, event->data, event->data_len);
        buffer->length += event->data_len;
        buffer->data[buffer->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t request_once(const watchdog_config_t *config, const char *endpoint,
                              char *output, size_t output_size) {
    char url[256];
    int written = snprintf(url, sizeof(url), "http://%s:8050/%s", config->ez1_host, endpoint);
    if (written <= 0 || (size_t)written >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    response_buffer_t response = {0};
    esp_http_client_config_t client_config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 3000,
        .buffer_size = 1024,
        .event_handler = on_http_event,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || response.overflow || response.length == 0) {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    if (response.length >= output_size) return ESP_ERR_INVALID_SIZE;
    memcpy(output, response.data, response.length + 1);
    return ESP_OK;
}

static esp_err_t request(const watchdog_config_t *config, const char *endpoint,
                         char *output, size_t output_size) {
    static const TickType_t backoff[] = {pdMS_TO_TICKS(500), pdMS_TO_TICKS(1000)};
    esp_err_t err = ESP_FAIL;
    for (size_t attempt = 0; attempt < 3; ++attempt) {
        esp_task_wdt_reset();
        err = request_once(config, endpoint, output, output_size);
        if (err == ESP_OK) return ESP_OK;
        if (attempt < 2) vTaskDelay(backoff[attempt]);
    }
    return err;
}

static cJSON *data_object(const char *json, cJSON **root) {
    *root = cJSON_Parse(json);
    if (!*root) return NULL;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(*root, "data");
    return cJSON_IsObject(data) ? data : NULL;
}

static bool json_int(cJSON *object, const char *name, int *value) {
    cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    if (cJSON_IsNumber(item)) {
        *value = item->valueint;
        return *value >= 0 && *value <= 2000;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        char *end = NULL;
        long parsed = strtol(item->valuestring, &end, 10);
        if (end && *end == '\0' && parsed >= 0 && parsed <= 2000) {
            *value = (int)parsed;
            return true;
        }
    }
    return false;
}

static esp_err_t verify_identity(const watchdog_config_t *config, ez1_result_t *result) {
    char response[RESPONSE_CAPACITY];
    esp_err_t err = request(config, "getDeviceInfo", response, sizeof(response));
    if (err != ESP_OK) return err;
    result->reachable = true;
    cJSON *root = NULL;
    cJSON *data = data_object(response, &root);
    cJSON *id = data ? cJSON_GetObjectItemCaseSensitive(data, "deviceId") : NULL;
    int min_power = -1;
    bool valid = cJSON_IsString(id) && id->valuestring && json_int(data, "minPower", &min_power);
    if (!valid || strcmp(id->valuestring, config->ez1_device_id) != 0 || min_power > WATCHDOG_FAILSAFE_WATTS) {
        snprintf(result->detail, sizeof(result->detail), "%s",
                 !valid ? "invalid_device_info" :
                 strcmp(id->valuestring, config->ez1_device_id) ? "identity_mismatch" : "minimum_above_30w");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    result->identity_verified = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t read_max(const watchdog_config_t *config, int *watts) {
    char response[RESPONSE_CAPACITY];
    esp_err_t err = request(config, "getMaxPower", response, sizeof(response));
    if (err != ESP_OK) return err;
    cJSON *root = NULL;
    cJSON *data = data_object(response, &root);
    bool valid = data && json_int(data, "maxPower", watts);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t write_fixed_30(const watchdog_config_t *config) {
    char response[RESPONSE_CAPACITY];
    char endpoint[32];
    snprintf(endpoint, sizeof(endpoint), "setMaxPower?p=%d", WATCHDOG_FAILSAFE_WATTS);
    esp_err_t err = request(config, endpoint, response, sizeof(response));
    if (err != ESP_OK) return err;
    cJSON *root = NULL;
    cJSON *data = data_object(response, &root);
    int acknowledged = -1;
    bool valid = data && json_int(data, "maxPower", &acknowledged) && acknowledged == WATCHDOG_FAILSAFE_WATTS;
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ez1_http_probe(const watchdog_config_t *config, ez1_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->max_power = -1;
    esp_err_t err = verify_identity(config, result);
    if (err == ESP_OK) err = read_max(config, &result->max_power);
    if (err == ESP_OK) snprintf(result->detail, sizeof(result->detail), "read_ok");
    return err;
}

esp_err_t ez1_http_enforce_failsafe(const watchdog_config_t *config,
                                    ez1_result_t *result) {
    esp_err_t err = ez1_http_probe(config, result);
    if (err != ESP_OK) return err;
    if (result->max_power <= WATCHDOG_FAILSAFE_WATTS) {
        snprintf(result->detail, sizeof(result->detail), "already_at_or_below_30w");
        return ESP_OK;
    }
    result->override_detected = true;
    err = write_fixed_30(config);
    if (err == ESP_OK) err = read_max(config, &result->max_power);
    if (err != ESP_OK || result->max_power != WATCHDOG_FAILSAFE_WATTS) {
        snprintf(result->detail, sizeof(result->detail), "30w_readback_failed");
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    snprintf(result->detail, sizeof(result->detail), "30w_verified");
    ESP_LOGW(TAG, "verified fail-safe limit at %d W", result->max_power);
    return ESP_OK;
}

esp_err_t ez1_http_apply_failsafe(const watchdog_config_t *config,
                                  ez1_result_t *result) {
    return ez1_http_enforce_failsafe(config, result);
}
