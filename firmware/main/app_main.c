#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "ez1_ble.h"
#include "ez1_http.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lease_server.h"
#include "safety_state.h"
#include "wifi_manager.h"

#define LED_GPIO GPIO_NUM_21
#define ENFORCEMENT_INTERVAL_MS 20000
/* BLE stop retry policy while latched and HTTP is unavailable. */
#define BLE_RETRY_MIN_MS 20000
#define BLE_RETRY_MAX_MS (5 * 60 * 1000)
#define BLE_VERIFIED_REASSERT_MS (10 * 60 * 1000)
#define BLE_INIT_RETRY_MS 60000
/* Read-only health checks while armed. A check blocks the controller, so it
 * only runs when the lease has at least this much budget left before trip. */
#define HEALTH_CHECK_INTERVAL_MS (10 * 60 * 1000)
#define HEALTH_CHECK_BUDGET_MS 45000
/* Consecutive watchdog/panic resets without an intervening lease that latch
 * the fail-safe at boot, so a crash loop cannot keep restarting the trip timer. */
#define ABNORMAL_RESET_LATCH_THRESHOLD 3

static const char *TAG = "watchdog";
static watchdog_config_t config;
static watchdog_status_t published_status;
static QueueHandle_t event_queue;
static SemaphoreHandle_t status_mutex;
static bool device_configured;
static bool nvs_ok;
static bool nvs_erased;

typedef struct {
    int64_t last_enforcement_ms;
    int64_t next_ble_attempt_ms;
    uint32_t ble_retry_ms;
    bool last_ble_ok;
    /* After a BLE stop, the EZ1 HTTP limit still holds its pre-incident value;
     * the first HTTP enforcement afterwards is not a competing writer. */
    bool http_limit_unverified;
    int64_t last_ble_init_attempt_ms;
    bool clear_abnormal_resets_on_lease;
} controller_t;

static controller_t controller;

static void indicator_led_init(void) {
    ESP_ERROR_CHECK(gpio_reset_pin(LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_21, 0)); // XIAO user LED is active-low.
}

static void unprovisioned_blink(void *argument) {
    (void)argument;
    int level = 0;
    while (true) {
        level = !level;
        gpio_set_level(LED_GPIO, level);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void usb_console_init(void) {
    usb_serial_jtag_driver_config_t console_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&console_config));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
}

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static const char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INTERRUPT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "POWER_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default: return "UNKNOWN";
    }
}

static bool is_abnormal_reset(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_CPU_LOCKUP;
}

static void publish(const watchdog_status_t *status) {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    published_status = *status;
    xSemaphoreGive(status_mutex);
}

static void record_http_result(watchdog_status_t *status, const ez1_result_t *result,
                               esp_err_t err) {
    status->ez1_identity_verified = result->identity_verified;
    status->ez1_http_reachable = result->reachable;
    if (err == ESP_OK) status->last_verified_max_power = result->max_power;
}

static void reset_ble_schedule(void) {
    controller.next_ble_attempt_ms = 0;
    controller.ble_retry_ms = BLE_RETRY_MIN_MS;
}

/* Sends the fixed BLE stop. The caller has already set FAILSAFE_APPLYING_BLE. */
static void apply_ble_result(watchdog_status_t *status) {
    ez1_ble_result_t result;
    snprintf(status->last_action, sizeof(status->last_action), "STOP_PRODUCTION");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "BLE");
    esp_err_t err = ez1_ble_stop(&config, &result);
    int64_t now = now_ms();
    status->ble_ready = ez1_ble_ready();
    status->ble_identity_verified = result.identity_verified;
    if (result.identity_verified) status->last_ble_identity_ms = now;
    snprintf(status->last_ble_result, sizeof(status->last_ble_result), "%s", result.detail);
    snprintf(status->last_result, sizeof(status->last_result), "%s", result.detail);
    safety_ble_result(status, err == ESP_OK);
    if (err == ESP_OK) {
        controller.last_ble_ok = true;
        controller.http_limit_unverified = true;
        controller.ble_retry_ms = BLE_RETRY_MIN_MS;
        controller.next_ble_attempt_ms = now + BLE_VERIFIED_REASSERT_MS;
    } else {
        controller.last_ble_ok = false;
        controller.next_ble_attempt_ms = now + controller.ble_retry_ms;
        controller.ble_retry_ms = controller.ble_retry_ms * 2 > BLE_RETRY_MAX_MS
                                      ? BLE_RETRY_MAX_MS
                                      : controller.ble_retry_ms * 2;
    }
}

static void apply_http_result(watchdog_status_t *status) {
    ez1_result_t result;
    snprintf(status->last_action, sizeof(status->last_action), "APPLY_FAILSAFE");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "HTTP");
    esp_err_t err = ez1_http_apply_failsafe(&config, &result);
    record_http_result(status, &result, err);
    snprintf(status->last_result, sizeof(status->last_result), "%s", result.detail);
    safety_action_t next = safety_http_result(status, err == ESP_OK);
    if (err == ESP_OK) {
        controller.http_limit_unverified = false;
        reset_ble_schedule();
    }
    if (next == SAFETY_ACTION_APPLY_BLE) {
        apply_ble_result(status);
    }
}

static void mark_unpersisted_latch(watchdog_status_t *status) {
    if (status->latched && !status->latch_persisted) {
        /* Actuation has still been attempted on both transports; this state
         * blocks release until the latch is durable. */
        status->state = WD_STATE_ACTUATOR_UNREACHABLE;
        snprintf(status->last_result, sizeof(status->last_result), "latch_persist_failed");
    }
}

static void execute_action(watchdog_status_t *status, safety_action_t action) {
    if (action == SAFETY_ACTION_PERSIST_LATCH) {
        reset_ble_schedule();
        controller.last_ble_ok = false;
        controller.http_limit_unverified = false;
        esp_err_t err = config_store_set_latch(true, status->latch_id);
        status->latch_persisted = err == ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist latch: %s", esp_err_to_name(err));
        }
        action = safety_latch_persisted(status);
    }
    if (action == SAFETY_ACTION_APPLY_HTTP) {
        controller.last_enforcement_ms = now_ms();
        apply_http_result(status);
        mark_unpersisted_latch(status);
    } else if (action == SAFETY_ACTION_APPLY_BLE) {
        apply_ble_result(status);
        mark_unpersisted_latch(status);
    } else if (action == SAFETY_ACTION_CLEAR_LATCH) {
        esp_err_t err = config_store_set_latch(false, status->latch_id);
        if (err != ESP_OK) {
            status->latched = true;
            status->latch_persisted = true;
            status->state = WD_STATE_FAILSAFE_LATCHED;
            snprintf(status->latch_reason, sizeof(status->latch_reason), "release_not_persisted");
            snprintf(status->last_result, sizeof(status->last_result), "latch_clear_persist_failed");
        } else {
            status->latch_persisted = false;
            snprintf(status->last_action, sizeof(status->last_action), "RELEASE");
            snprintf(status->last_action_path, sizeof(status->last_action_path), "LOCAL");
            snprintf(status->last_result, sizeof(status->last_result), "accepted");
        }
    }
}

static void enforce_latched(watchdog_status_t *status) {
    int64_t now = now_ms();
    controller.last_enforcement_ms = now;
    if (!status->latch_persisted) {
        status->latch_persisted =
            config_store_set_latch(true, status->latch_id) == ESP_OK;
    }
    status->state = WD_STATE_FAILSAFE_APPLYING_HTTP;
    ez1_result_t result;
    esp_err_t err = ez1_http_enforce_failsafe(&config, &result);
    record_http_result(status, &result, err);
    snprintf(status->last_action, sizeof(status->last_action), "ENFORCE_FAILSAFE");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "HTTP");
    snprintf(status->last_result, sizeof(status->last_result), "%s", result.detail);
    if (err == ESP_OK) {
        status->state = WD_STATE_FAILSAFE_LATCHED;
        if (result.override_detected && !controller.http_limit_unverified) {
            status->override_count++;
            config_store_set_override_count(status->override_count);
        }
        controller.http_limit_unverified = false;
        reset_ble_schedule();
    } else if (now >= controller.next_ble_attempt_ms) {
        /* BLE fallback never depends on whether the latch could be persisted. */
        status->state = WD_STATE_FAILSAFE_APPLYING_BLE;
        apply_ble_result(status);
    } else {
        /* HTTP still unavailable; BLE is backing off. Keep the state of the
         * most recent BLE attempt. */
        status->state = controller.last_ble_ok ? WD_STATE_FAILSAFE_LATCHED
                                               : WD_STATE_ACTUATOR_UNREACHABLE;
    }
    mark_unpersisted_latch(status);
}

static bool lease_budget_allows_blocking(const watchdog_status_t *status) {
    return safety_lease_age_ms(status, now_ms()) + HEALTH_CHECK_BUDGET_MS < status->trip_ms;
}

static void run_http_probe(watchdog_status_t *status) {
    ez1_result_t result;
    esp_err_t err = ez1_http_probe(&config, &result);
    record_http_result(status, &result, err);
    snprintf(status->last_action, sizeof(status->last_action), "READ_ONLY_PROBE");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "HTTP");
    snprintf(status->last_result, sizeof(status->last_result), "%s",
             err == ESP_OK ? result.detail : (result.detail[0] ? result.detail : "probe_failed"));
}

static void run_health_check(watchdog_status_t *status) {
    status->last_health_check_ms = now_ms();
    run_http_probe(status);
    esp_task_wdt_reset();
    if (!lease_budget_allows_blocking(status)) return;
    ez1_ble_result_t ble;
    status->ble_advert_seen = ez1_ble_check_advertisement(&config, &ble) == ESP_OK;
    status->ble_ready = ez1_ble_ready();
}

static void handle_event(watchdog_status_t *status, const watchdog_event_t *event) {
    if (event->type == WD_EVENT_LEASE) {
        safety_on_lease(status, event->at_ms > 0 ? event->at_ms : now_ms());
        if (controller.clear_abnormal_resets_on_lease &&
            config_store_set_abnormal_resets(0) == ESP_OK) {
            controller.clear_abnormal_resets_on_lease = false;
        }
    } else if (event->type == WD_EVENT_RELEASE) {
        execute_action(status, safety_on_release(status, now_ms(), event->latch_id));
    } else if (event->type == WD_EVENT_EZ1_PROBE && !status->latched) {
        if (lease_budget_allows_blocking(status)) {
            run_http_probe(status);
        } else {
            snprintf(status->last_result, sizeof(status->last_result), "probe_deferred_near_trip");
        }
    }
}

static void safety_controller(void *argument) {
    watchdog_status_t status;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    status = *(watchdog_status_t *)argument;
    xSemaphoreGive(status_mutex);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    reset_ble_schedule();
    status.ble_ready = ez1_ble_ready();
    controller.last_ble_init_attempt_ms = now_ms();
    if (status.latched) {
        execute_action(&status, status.latch_persisted ? SAFETY_ACTION_APPLY_HTTP
                                                       : SAFETY_ACTION_PERSIST_LATCH);
    }
    publish(&status);
    while (true) {
        watchdog_event_t event;
        if (xQueueReceive(event_queue, &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            handle_event(&status, &event);
            /* Drain what queued up while blocked, so leases that arrived before
             * a deadline are applied before the trip check. */
            for (int i = 0; i < WATCHDOG_EVENT_QUEUE_LENGTH &&
                            xQueueReceive(event_queue, &event, 0) == pdTRUE;
                 ++i) {
                handle_event(&status, &event);
            }
        }
        execute_action(&status, safety_on_tick(&status, now_ms()));
        int64_t now = now_ms();
        if (status.latched && now - controller.last_enforcement_ms >= ENFORCEMENT_INTERVAL_MS &&
            status.state != WD_STATE_FAILSAFE_APPLYING_HTTP) {
            enforce_latched(&status);
        } else if (!status.latched && status.state == WD_STATE_ARMED &&
                   now - status.last_health_check_ms >= HEALTH_CHECK_INTERVAL_MS &&
                   lease_budget_allows_blocking(&status)) {
            run_health_check(&status);
        }
        status.ble_ready = ez1_ble_ready();
        if (!status.ble_ready && now_ms() - controller.last_ble_init_attempt_ms >= BLE_INIT_RETRY_MS) {
            controller.last_ble_init_attempt_ms = now_ms();
            esp_task_wdt_reset();
            esp_err_t err = ez1_ble_init();
            if (err != ESP_OK) ESP_LOGW(TAG, "BLE init retry failed: %s", esp_err_to_name(err));
            status.ble_ready = ez1_ble_ready();
        }
        publish(&status);
        esp_task_wdt_reset();
    }
}

static bool copy_json_string(cJSON *object, const char *name, char *destination, size_t size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= size) return false;
    strlcpy(destination, item->valuestring, size);
    return true;
}

static bool copy_json_u32(cJSON *object, const char *name, uint32_t *destination) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return false;
    }
    *destination = (uint32_t)item->valuedouble;
    return true;
}

static void print_json_line(cJSON *root) {
    char *line = cJSON_PrintUnformatted(root);
    if (line) {
        printf("%s\n", line);
        fflush(stdout);
        cJSON_free(line);
    }
    cJSON_Delete(root);
}

static void serial_reply(bool ok, const char *command, const char *error) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "command", command);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (error) cJSON_AddStringToObject(root, "error", error);
    print_json_line(root);
}

static void serial_self_test(void) {
    uint32_t flash_size = 0;
    bool flash_ok = esp_flash_get_size(esp_flash_default_chip, &flash_size) == ESP_OK &&
                    flash_size >= 8 * 1024 * 1024;
    bool psram_ok = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "command", "self_test");
    cJSON_AddBoolToObject(root, "ok", flash_ok && psram_ok && nvs_ok);
    cJSON *checks = cJSON_AddObjectToObject(root, "checks");
    cJSON_AddBoolToObject(checks, "nvs", nvs_ok);
    cJSON_AddBoolToObject(checks, "nvs_erased_at_boot", nvs_erased);
    cJSON_AddBoolToObject(checks, "configured", device_configured);
    cJSON_AddNumberToObject(checks, "flash_size", flash_size);
    cJSON_AddBoolToObject(checks, "psram_detected", psram_ok);
    cJSON_AddBoolToObject(checks, "rng", esp_random() != esp_random());
    cJSON_AddBoolToObject(checks, "wifi_connected", wifi_manager_connected());
    cJSON_AddBoolToObject(checks, "ble_protocol_ready", ez1_ble_ready());
    cJSON_AddBoolToObject(checks, "task_watchdog", true);
    cJSON_AddNumberToObject(checks, "free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddStringToObject(checks, "reset_reason", reset_reason_name(esp_reset_reason()));
    print_json_line(root);
}

static void serial_ble_stop_test(void) {
    ez1_ble_result_t result;
    esp_err_t err = ez1_ble_stop(&config, &result);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "command", "ble_stop_test");
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "identity_verified", result.identity_verified);
    cJSON_AddStringToObject(root, "result", result.detail);
    if (err != ESP_OK) cJSON_AddStringToObject(root, "error", result.detail);
    print_json_line(root);
}

static void serial_admin(void *argument) {
    (void)argument;
    bool wifi_test_started = false;
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        cJSON *root = cJSON_Parse(line);
        cJSON *protocol = root ? cJSON_GetObjectItemCaseSensitive(root, "protocol") : NULL;
        cJSON *command = root ? cJSON_GetObjectItemCaseSensitive(root, "command") : NULL;
        if (!cJSON_IsNumber(protocol) || protocol->valueint != 1 || !cJSON_IsString(command)) {
            cJSON_Delete(root);
            serial_reply(false, "unknown", "invalid_protocol");
            continue;
        }
        if (strcmp(command->valuestring, "self_test") == 0) {
            cJSON_Delete(root);
            serial_self_test();
            continue;
        }
        cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
        if (strcmp(command->valuestring, "ble_stop_test") == 0) {
            bool confirmed = config.ez1_device_id[0] != '\0' && ez1_ble_ready() &&
                             cJSON_IsString(confirm) &&
                             strcmp(confirm->valuestring, "BLESTOP") == 0;
            cJSON_Delete(root);
            if (!confirmed) serial_reply(false, "ble_stop_test", "confirmation_required");
            else serial_ble_stop_test();
            continue;
        }
        cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "config");
        watchdog_config_t candidate = {.schema_version = 1};
        if (strcmp(command->valuestring, "wifi_test") == 0) {
            bool valid = cJSON_IsObject(source) &&
                         copy_json_string(source, "wifi_ssid", candidate.wifi_ssid,
                                          sizeof(candidate.wifi_ssid)) &&
                         copy_json_string(source, "wifi_password", candidate.wifi_password,
                                          sizeof(candidate.wifi_password)) &&
                         candidate.wifi_ssid[0] != '\0';
            cJSON_Delete(root);
            if (device_configured) {
                /* The provisioned watchdog owns the Wi-Fi driver; a test must not
                 * re-initialize or retarget it. */
                serial_reply(false, "wifi_test", "already_provisioned");
                continue;
            }
            if (!valid) {
                serial_reply(false, "wifi_test", "invalid_config");
                continue;
            }
            if (wifi_test_started) {
                serial_reply(false, "wifi_test", "restart_required");
                continue;
            }
            esp_err_t err = wifi_manager_start(&candidate);
            if (err != ESP_OK) {
                serial_reply(false, "wifi_test", esp_err_to_name(err));
                continue;
            }
            wifi_test_started = true;
            for (int attempt = 0; attempt < 40 && !wifi_manager_connected(); ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            serial_reply(wifi_manager_connected(), "wifi_test",
                         wifi_manager_connected() ? NULL : "connection_timeout");
            continue;
        }
        bool valid = strcmp(command->valuestring, "provision") == 0 && cJSON_IsObject(source) &&
                     copy_json_string(source, "wifi_ssid", candidate.wifi_ssid, sizeof(candidate.wifi_ssid)) &&
                     copy_json_string(source, "wifi_password", candidate.wifi_password, sizeof(candidate.wifi_password)) &&
                     copy_json_string(source, "ez1_host", candidate.ez1_host, sizeof(candidate.ez1_host)) &&
                     copy_json_string(source, "ez1_device_id", candidate.ez1_device_id, sizeof(candidate.ez1_device_id)) &&
                     copy_json_string(source, "auth_secret", candidate.auth_secret, sizeof(candidate.auth_secret)) &&
                     copy_json_u32(source, "lease_interval_s", &candidate.lease_interval_s) &&
                     copy_json_u32(source, "warning_s", &candidate.warning_s) &&
                     copy_json_u32(source, "trip_s", &candidate.trip_s) && config_store_validate(&candidate);
        cJSON_Delete(root);
        if (!valid) {
            serial_reply(false, "provision", "invalid_config");
            continue;
        }
        esp_err_t err = config_store_save(&candidate);
        if (err != ESP_OK) {
            serial_reply(false, "provision", esp_err_to_name(err));
            continue;
        }
        serial_reply(true, "provision", NULL);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    indicator_led_init();
    usb_console_init();
    esp_err_t nvs_err = config_store_init(&nvs_erased);
    nvs_ok = nvs_err == ESP_OK;
    if (!nvs_ok) ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(nvs_err));
    if (nvs_erased) ESP_LOGE(TAG, "NVS was erased during recovery; configuration and latch lost");
    bool configured = nvs_ok && config_store_load(&config) == ESP_OK;
    device_configured = configured;
    bool latched = false;
    uint32_t latch_id = 0, boot_id = 0, override_count = 0;
    uint8_t abnormal_resets = 0;
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (configured) {
        esp_err_t err = config_store_get_latch(&latched, &latch_id);
        if (err != ESP_OK) {
            /* Unknown latch state: fail safe. */
            ESP_LOGE(TAG, "latch state unreadable (%s); assuming latched", esp_err_to_name(err));
            latched = true;
        }
        if (config_store_next_boot_id(&boot_id) != ESP_OK) ESP_LOGW(TAG, "boot_id not persisted");
        if (config_store_get_override_count(&override_count) != ESP_OK) override_count = 0;
        if (config_store_get_abnormal_resets(&abnormal_resets) != ESP_OK) abnormal_resets = 0;
        if (is_abnormal_reset(reset_reason)) {
            if (abnormal_resets < UINT8_MAX) abnormal_resets++;
            config_store_set_abnormal_resets(abnormal_resets);
        }
    }
    uint32_t warning_ms = configured ? config.warning_s * 1000 : 120000;
    uint32_t trip_ms = configured ? config.trip_s * 1000 : 180000;
    safety_init(&published_status, configured, latched, latch_id, boot_id,
                warning_ms, trip_ms, now_ms());
    if (configured && !latched && abnormal_resets >= ABNORMAL_RESET_LATCH_THRESHOLD) {
        ESP_LOGE(TAG, "%u consecutive abnormal resets without a lease; latching",
                 (unsigned)abnormal_resets);
        safety_trip(&published_status, "repeated_abnormal_reset");
    }
    controller.clear_abnormal_resets_on_lease = abnormal_resets > 0;
    published_status.override_count = override_count;
    snprintf(published_status.reset_reason, sizeof(published_status.reset_reason), "%s",
             reset_reason_name(reset_reason));
    event_queue = xQueueCreate(WATCHDOG_EVENT_QUEUE_LENGTH, sizeof(watchdog_event_t));
    status_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(event_queue && status_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    if (configured) {
        esp_err_t ble_err = ez1_ble_init();
        if (ble_err != ESP_OK) ESP_LOGE(TAG, "BLE initialization failed: %s (will retry)", esp_err_to_name(ble_err));
    }
    ESP_LOGI(TAG, "boot=%lu configured=%d latched=%d reset_reason=%d free_heap=%lu",
             (unsigned long)boot_id, configured, published_status.latched, reset_reason,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    xTaskCreate(serial_admin, "serial_admin", 6144, NULL, 4, NULL);
    if (!configured) {
        ESP_LOGW(TAG, "UNPROVISIONED: send protocol 1 provisioning JSON over USB serial");
        xTaskCreate(unprovisioned_blink, "led_blink", 2048, NULL, 1, NULL);
        return;
    }
    /* Network failures are logged, not fatal: the controller must still run so
     * that missing leases trip the fail-safe and BLE can actuate. */
    esp_err_t err = wifi_manager_start(&config);
    if (err != ESP_OK) ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    err = lease_server_start(&config, event_queue, &published_status, status_mutex);
    if (err != ESP_OK) ESP_LOGE(TAG, "lease server start failed: %s", esp_err_to_name(err));
    for (int attempt = 0; attempt < 40 && !wifi_manager_connected(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    BaseType_t created =
        xTaskCreate(safety_controller, "safety_controller", 8192, &published_status, 8, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
