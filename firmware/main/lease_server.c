#include "lease_server.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "safety_state.h"
#include "wifi_manager.h"

#ifndef WATCHDOG_VERSION
#define WATCHDOG_VERSION "development"
#endif

typedef struct {
    const watchdog_config_t *config;
    QueueHandle_t events;
    const watchdog_status_t *status;
    SemaphoreHandle_t status_mutex;
} server_context_t;

static server_context_t context;

static bool constant_time_equal(const char *left, const char *right) {
    size_t left_len = strlen(left), right_len = strlen(right);
    unsigned char difference = (unsigned char)(left_len ^ right_len);
    size_t length = left_len > right_len ? left_len : right_len;
    for (size_t i = 0; i < length; ++i) {
        unsigned char a = i < left_len ? (unsigned char)left[i] : 0;
        unsigned char b = i < right_len ? (unsigned char)right[i] : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

static bool authorized(httpd_req_t *request) {
    char received[80] = {0};
    char expected[80] = {0};
    size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (length == 0 || length >= sizeof(received) || context.config->auth_secret[0] == '\0') return false;
    if (httpd_req_get_hdr_value_str(request, "Authorization", received, sizeof(received)) != ESP_OK) return false;
    snprintf(expected, sizeof(expected), "Bearer %s", context.config->auth_secret);
    return constant_time_equal(received, expected);
}

static esp_err_t json_response(httpd_req_t *request, cJSON *json) {
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return httpd_resp_send_500(request);
    httpd_resp_set_type(request, "application/json");
    esp_err_t err = httpd_resp_sendstr(request, body);
    cJSON_free(body);
    return err;
}

static esp_err_t unauthorized(httpd_req_t *request) {
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"error\":\"unauthorized\"}");
}

static esp_err_t health(httpd_req_t *request) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "version", WATCHDOG_VERSION);
    return json_response(request, root);
}

static esp_err_t status_handler(httpd_req_t *request) {
    if (!authorized(request)) return unauthorized(request);
    watchdog_status_t status;
    xSemaphoreTake(context.status_mutex, portMAX_DELAY);
    status = *context.status;
    xSemaphoreGive(context.status_mutex);
    int64_t now_ms = esp_timer_get_time() / 1000;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", WATCHDOG_VERSION);
    cJSON_AddNumberToObject(root, "boot_id", status.boot_id);
    cJSON_AddNumberToObject(root, "uptime_s", (now_ms - status.boot_ms) / 1000);
    cJSON_AddStringToObject(root, "state", watchdog_state_name(status.state));
    cJSON_AddNumberToObject(root, "lease_age_ms", safety_lease_age_ms(&status, now_ms));
    cJSON_AddNumberToObject(root, "warning_timeout_ms", status.warning_ms);
    cJSON_AddNumberToObject(root, "trip_timeout_ms", status.trip_ms);
    cJSON_AddBoolToObject(root, "failsafe_latched", status.latched);
    cJSON_AddBoolToObject(root, "latch_persisted", status.latch_persisted);
    cJSON_AddNumberToObject(root, "latch_id", status.latch_id);
    if (status.latch_reason[0]) cJSON_AddStringToObject(root, "latch_reason", status.latch_reason);
    else cJSON_AddNullToObject(root, "latch_reason");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_manager_connected());
    cJSON_AddNumberToObject(wifi, "rssi", wifi_manager_rssi());
    cJSON *ez1 = cJSON_AddObjectToObject(root, "ez1");
    cJSON_AddBoolToObject(ez1, "identity_verified", status.ez1_identity_verified);
    cJSON_AddBoolToObject(ez1, "http_reachable", status.ez1_http_reachable);
    cJSON_AddBoolToObject(ez1, "ble_seen_recently", status.ble_identity_verified);
    if (status.last_ble_result[0]) cJSON_AddStringToObject(ez1, "last_ble_result", status.last_ble_result);
    else cJSON_AddNullToObject(ez1, "last_ble_result");
    cJSON_AddNumberToObject(ez1, "last_verified_max_power", status.last_verified_max_power);
    cJSON *action = cJSON_AddObjectToObject(root, "last_action");
    cJSON_AddStringToObject(action, "type", status.last_action[0] ? status.last_action : "NONE");
    if (status.last_action_path[0]) cJSON_AddStringToObject(action, "path", status.last_action_path);
    else cJSON_AddNullToObject(action, "path");
    cJSON_AddStringToObject(action, "result", status.last_result);
    cJSON_AddNumberToObject(root, "override_detected_count", status.override_count);
    cJSON_AddStringToObject(root, "reset_reason", status.reset_reason);
    return json_response(request, root);
}

static esp_err_t lease_handler(httpd_req_t *request) {
    if (!authorized(request)) return unauthorized(request);
    watchdog_event_t event = {.type = WD_EVENT_LEASE};
    if (xQueueSend(context.events, &event, 0) != pdTRUE) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"event_queue_full\"}");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static esp_err_t release_handler(httpd_req_t *request) {
    if (!authorized(request)) return unauthorized(request);
    if (request->content_len <= 0 || request->content_len > 128) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid_body\"}");
    }
    char body[129] = {0};
    int received = httpd_req_recv(request, body, request->content_len);
    if (received != request->content_len) return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "latch_id") : NULL;
    if (!cJSON_IsNumber(id) || id->valuedouble < 0 || id->valuedouble > UINT32_MAX) {
        cJSON_Delete(root);
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid_latch_id\"}");
    }
    watchdog_status_t status;
    xSemaphoreTake(context.status_mutex, portMAX_DELAY);
    status = *context.status;
    xSemaphoreGive(context.status_mutex);
    uint32_t requested_id = (uint32_t)id->valuedouble;
    cJSON_Delete(root);
    bool fresh = status.last_lease_ms >= 0 &&
                 safety_lease_age_ms(&status, esp_timer_get_time() / 1000) < status.warning_ms;
    if (!status.latched || status.state != WD_STATE_FAILSAFE_LATCHED ||
        requested_id != status.latch_id || !fresh || status.recent_leases < 2) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "{\"accepted\":false,\"error\":\"release_preconditions_failed\"}");
    }
    watchdog_event_t event = {.type = WD_EVENT_RELEASE, .latch_id = requested_id};
    if (xQueueSend(context.events, &event, 0) != pdTRUE) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"event_queue_full\"}");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

static esp_err_t probe_handler(httpd_req_t *request) {
    if (!authorized(request)) return unauthorized(request);
    watchdog_event_t event = {.type = WD_EVENT_EZ1_PROBE};
    if (xQueueSend(context.events, &event, 0) != pdTRUE) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"event_queue_full\"}");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true,\"read_only\":true}");
}

esp_err_t lease_server_start(const watchdog_config_t *config, QueueHandle_t events,
                             const watchdog_status_t *status,
                             SemaphoreHandle_t status_mutex) {
    context = (server_context_t){config, events, status, status_mutex};
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 8;
    server_config.stack_size = 6144;
    server_config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &server_config);
    if (err != ESP_OK) return err;
    const httpd_uri_t endpoints[] = {
        {.uri = "/api/v1/health", .method = HTTP_GET, .handler = health},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/v1/lease", .method = HTTP_POST, .handler = lease_handler},
        {.uri = "/api/v1/release", .method = HTTP_POST, .handler = release_handler},
        {.uri = "/api/v1/ez1-probe", .method = HTTP_POST, .handler = probe_handler},
    };
    for (size_t i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &endpoints[i]));
    }
    return ESP_OK;
}
