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

static const char *TAG = "watchdog";
static watchdog_config_t config;
static watchdog_status_t published_status;
static QueueHandle_t event_queue;
static SemaphoreHandle_t status_mutex;

static void indicator_led_init(void) {
    ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_21));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_21, 0)); // XIAO user LED is active-low.
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

static void publish(const watchdog_status_t *status) {
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    published_status = *status;
    xSemaphoreGive(status_mutex);
}

static void apply_ble_result(watchdog_status_t *status) {
    ez1_ble_result_t result;
    snprintf(status->last_action, sizeof(status->last_action), "STOP_PRODUCTION");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "BLE");
    esp_err_t err = ez1_ble_stop(&config, &result);
    status->ez1_identity_verified = result.identity_verified;
    status->ble_identity_verified = result.identity_verified;
    snprintf(status->last_ble_result, sizeof(status->last_ble_result), "%s", result.detail);
    snprintf(status->last_result, sizeof(status->last_result), "%s", result.detail);
    safety_ble_result(status, err == ESP_OK);
}

static void apply_http_result(watchdog_status_t *status) {
    ez1_result_t result;
    snprintf(status->last_action, sizeof(status->last_action), "APPLY_FAILSAFE");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "HTTP");
    esp_err_t err = ez1_http_apply_failsafe(&config, &result);
    status->ez1_identity_verified = result.identity_verified;
    status->ez1_http_reachable = result.reachable;
    status->last_verified_max_power = result.max_power;
    snprintf(status->last_result, sizeof(status->last_result), "%s", result.detail);
    safety_action_t next = safety_http_result(status, err == ESP_OK);
    if (next == SAFETY_ACTION_APPLY_BLE) {
        apply_ble_result(status);
    }
}

static void execute_action(watchdog_status_t *status, safety_action_t action) {
    if (action == SAFETY_ACTION_PERSIST_LATCH) {
        esp_err_t err = config_store_set_latch(true, status->latch_id);
        status->latch_persisted = err == ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist latch: %s", esp_err_to_name(err));
            snprintf(status->last_result, sizeof(status->last_result), "latch_persist_failed");
        }
        action = safety_latch_persisted(status);
    }
    if (action == SAFETY_ACTION_APPLY_HTTP) {
        apply_http_result(status);
        if (status->latched && !status->latch_persisted) {
            status->state = WD_STATE_ACTUATOR_UNREACHABLE;
            snprintf(status->last_result, sizeof(status->last_result), "latch_persist_failed");
        }
    } else if (action == SAFETY_ACTION_APPLY_BLE) {
        apply_ble_result(status);
    } else if (action == SAFETY_ACTION_CLEAR_LATCH) {
        esp_err_t err = config_store_set_latch(false, status->latch_id);
        if (err != ESP_OK) {
            status->latched = true;
            status->latch_persisted = true;
            status->state = WD_STATE_FAILSAFE_LATCHED;
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
    if (!status->latch_persisted) {
        status->latch_persisted =
            config_store_set_latch(true, status->latch_id) == ESP_OK;
    }
    status->state = WD_STATE_FAILSAFE_APPLYING_HTTP;
    ez1_result_t result;
    esp_err_t err = ez1_http_enforce_failsafe(&config, &result);
    status->ez1_identity_verified = result.identity_verified;
    status->ez1_http_reachable = result.reachable;
    status->last_verified_max_power = result.max_power;
    snprintf(status->last_action, sizeof(status->last_action), "ENFORCE_FAILSAFE");
    snprintf(status->last_action_path, sizeof(status->last_action_path), "HTTP");
    snprintf(status->last_result, sizeof(status->last_result), "%s", result.detail);
    if (err == ESP_OK && status->latch_persisted) {
        status->state = WD_STATE_FAILSAFE_LATCHED;
        if (result.override_detected) {
            status->override_count++;
            config_store_set_override_count(status->override_count);
        }
    } else if (status->latch_persisted) {
        status->state = WD_STATE_FAILSAFE_APPLYING_BLE;
        apply_ble_result(status);
    } else {
        status->state = WD_STATE_ACTUATOR_UNREACHABLE;
        if (!status->latch_persisted) {
            snprintf(status->last_result, sizeof(status->last_result), "latch_persist_failed");
        }
    }
}

static void safety_controller(void *argument) {
    watchdog_status_t status = *(watchdog_status_t *)argument;
    esp_task_wdt_add(NULL);
    int64_t last_enforcement_ms = 0;
    if (status.latched) execute_action(&status, SAFETY_ACTION_APPLY_HTTP);
    publish(&status);
    while (true) {
        watchdog_event_t event;
        if (xQueueReceive(event_queue, &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (event.type == WD_EVENT_LEASE) {
                safety_on_lease(&status, now_ms());
            } else if (event.type == WD_EVENT_RELEASE) {
                execute_action(&status, safety_on_release(&status, now_ms(), event.latch_id));
            } else if (event.type == WD_EVENT_EZ1_PROBE && !status.latched) {
                ez1_result_t result;
                esp_err_t err = ez1_http_probe(&config, &result);
                status.ez1_identity_verified = result.identity_verified;
                status.ez1_http_reachable = result.reachable;
                status.last_verified_max_power = result.max_power;
                snprintf(status.last_action, sizeof(status.last_action), "READ_ONLY_PROBE");
                snprintf(status.last_action_path, sizeof(status.last_action_path), "HTTP");
                snprintf(status.last_result, sizeof(status.last_result), "%s",
                         err == ESP_OK ? result.detail : "probe_failed");
            }
        }
        execute_action(&status, safety_on_tick(&status, now_ms()));
        if (status.latched && now_ms() - last_enforcement_ms >= 20000 &&
            status.state != WD_STATE_FAILSAFE_APPLYING_HTTP) {
            last_enforcement_ms = now_ms();
            enforce_latched(&status);
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
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) return false;
    *destination = (uint32_t)item->valuedouble;
    return true;
}

static void serial_reply(bool ok, const char *command, const char *error) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "command", command);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (error) cJSON_AddStringToObject(root, "error", error);
    char *line = cJSON_PrintUnformatted(root);
    if (line) {
        printf("%s\n", line);
        fflush(stdout);
        cJSON_free(line);
    }
    cJSON_Delete(root);
}

static void serial_self_test(void) {
    uint32_t flash_size = 0;
    uint32_t random_sample = esp_random();
    bool flash_ok = esp_flash_get_size(esp_flash_default_chip, &flash_size) == ESP_OK &&
                    flash_size >= 8 * 1024 * 1024;
    bool psram_ok = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "command", "self_test");
    cJSON_AddBoolToObject(root, "ok", flash_ok && psram_ok);
    cJSON *checks = cJSON_AddObjectToObject(root, "checks");
    cJSON_AddBoolToObject(checks, "nvs", true);
    cJSON_AddNumberToObject(checks, "flash_size", flash_size);
    cJSON_AddBoolToObject(checks, "psram_detected", psram_ok);
    cJSON_AddBoolToObject(checks, "rng", true);
    cJSON_AddBoolToObject(checks, "wifi_connected", wifi_manager_connected());
    cJSON_AddBoolToObject(checks, "ble_protocol_ready", ez1_ble_ready());
    cJSON_AddBoolToObject(checks, "task_watchdog", true);
    cJSON_AddNumberToObject(checks, "free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddStringToObject(checks, "reset_reason", reset_reason_name(esp_reset_reason()));
    (void)random_sample;
    char *line = cJSON_PrintUnformatted(root);
    if (line) {
        printf("%s\n", line);
        fflush(stdout);
        cJSON_free(line);
    }
    cJSON_Delete(root);
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
    char *line = cJSON_PrintUnformatted(root);
    if (line) {
        printf("%s\n", line);
        fflush(stdout);
        cJSON_free(line);
    }
    cJSON_Delete(root);
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
            if (!valid) {
                serial_reply(false, "wifi_test", "invalid_config");
                continue;
            }
            if (!wifi_test_started) {
                esp_err_t err = wifi_manager_start(&candidate);
                if (err != ESP_OK) {
                    serial_reply(false, "wifi_test", esp_err_to_name(err));
                    continue;
                }
                wifi_test_started = true;
            }
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
    ESP_ERROR_CHECK(config_store_init());
    bool configured = config_store_load(&config) == ESP_OK;
    bool latched = false;
    uint32_t latch_id = 0, boot_id = 0, override_count = 0;
    ESP_ERROR_CHECK(config_store_get_latch(&latched, &latch_id));
    ESP_ERROR_CHECK(config_store_next_boot_id(&boot_id));
    ESP_ERROR_CHECK(config_store_get_override_count(&override_count));
    uint32_t warning_ms = configured ? config.warning_s * 1000 : 120000;
    uint32_t trip_ms = configured ? config.trip_s * 1000 : 180000;
    safety_init(&published_status, configured, latched, latch_id, boot_id,
                warning_ms, trip_ms, now_ms());
    published_status.override_count = override_count;
    snprintf(published_status.reset_reason, sizeof(published_status.reset_reason), "%s",
             reset_reason_name(esp_reset_reason()));
    event_queue = xQueueCreate(WATCHDOG_EVENT_QUEUE_LENGTH, sizeof(watchdog_event_t));
    status_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(event_queue && status_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    if (configured) {
        esp_err_t ble_err = ez1_ble_init();
        if (ble_err != ESP_OK) ESP_LOGE(TAG, "BLE initialization failed: %s", esp_err_to_name(ble_err));
    }
    ESP_LOGI(TAG, "boot=%lu configured=%d latched=%d reset_reason=%d free_heap=%lu",
             (unsigned long)boot_id, configured, latched, esp_reset_reason(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    xTaskCreate(serial_admin, "serial_admin", 6144, NULL, 4, NULL);
    if (!configured) {
        ESP_LOGW(TAG, "UNPROVISIONED: send protocol 1 provisioning JSON over USB serial");
        return;
    }
    ESP_ERROR_CHECK(wifi_manager_start(&config));
    ESP_ERROR_CHECK(lease_server_start(&config, event_queue, &published_status, status_mutex));
    for (int attempt = 0; attempt < 40 && !wifi_manager_connected(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    xTaskCreate(safety_controller, "safety_controller", 8192, &published_status, 8, NULL);
}
