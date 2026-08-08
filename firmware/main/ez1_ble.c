#include "ez1_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "psa/crypto.h"

#define BLE_SYNCED BIT0
#define BLE_DONE BIT1
#define BLE_FAILED BIT2
#define BLE_TIMEOUT_MS 30000
#define RESPONSE_CAPACITY 1024
#define REQUEST_CAPACITY 1025

static const char *TAG = "ez1_ble";
static const char *KEY = "E7MiPPrs9v6i3DY3";
static const char *IV = "8914934610490056";
static const char *STOP_COMMAND = "FBFB06AAE1000000000191FEFE";
static const char *SUCCESS_REPLY = "fbfb06de02000100000000fefe";

typedef enum {
    STAGE_IDLE,
    STAGE_DEVICE_INFO,
    STAGE_COMMAND,
} operation_stage_t;

static EventGroupHandle_t events;
static SemaphoreHandle_t operation_mutex;
static bool initialized;
static bool running;
static uint8_t own_addr_type;
static uint16_t connection_handle;
static uint16_t service_start_handle;
static uint16_t service_end_handle;
static uint16_t write_handle;
static uint16_t notify_handle;
static uint16_t cccd_handle;
static uint8_t sequence;
static operation_stage_t stage;
static char expected_name[80];
static char expected_device_id[65];
static char request_payload[REQUEST_CAPACITY];
static size_t request_length;
static size_t request_offset;
static uint8_t response_payload[RESPONSE_CAPACITY + 1];
static size_t response_length;
static int response_remaining;
static int response_next_sequence;
static ez1_ble_result_t *active_result;
static bool target_found;

static int gap_event(struct ble_gap_event *event, void *argument);
static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *argument);
static void finish(bool success, const char *detail);
static void send_next_fragment(void);

static int mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error,
                         uint16_t mtu, void *argument) {
    (void)argument;
    if (error->status != 0 || mtu < 71) {
        finish(false, "mtu_exchange_failed");
    } else if (ble_gattc_disc_svc_by_uuid(conn_handle, BLE_UUID16_DECLARE(0xfffe),
                                          service_discovered, NULL) != 0) {
        finish(false, "service_discovery_start_failed");
    }
    return 0;
}

static void set_detail(const char *detail) {
    if (active_result) snprintf(active_result->detail, sizeof(active_result->detail), "%s", detail);
}

static void finish(bool success, const char *detail) {
    if (!running) return;
    running = false;
    set_detail(detail);
    xEventGroupSetBits(events, success ? BLE_DONE : BLE_FAILED);
    if (connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static bool json_string_equals(cJSON *object, const char *name, const char *expected) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, expected) == 0;
}

static bool validate_response(cJSON *root) {
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 200 ||
        !json_string_equals(root, "deviceId", expected_device_id)) {
        return false;
    }
    if (stage == STAGE_DEVICE_INFO) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        return json_string_equals(root, "identifier", "deviceInfo") &&
               json_string_equals(root, "method", "get_reply") && cJSON_IsObject(data) &&
               json_string_equals(data, "deviceId", expected_device_id);
    }
    if (stage == STAGE_COMMAND) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        cJSON *reply = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "cmdResult") : NULL;
        if (!json_string_equals(root, "identifier", "inverterCmd") ||
            !json_string_equals(root, "method", "set_reply") || !cJSON_IsString(reply) ||
            !reply->valuestring) {
            return false;
        }
        for (char *cursor = reply->valuestring; *cursor; ++cursor) {
            if (*cursor >= 'A' && *cursor <= 'F') *cursor = (char)(*cursor - 'A' + 'a');
        }
        return strcmp(reply->valuestring, SUCCESS_REPLY) == 0;
    }
    return false;
}

static bool aes_cbc(bool decrypt, uint8_t *data, size_t length) {
    if (!data || length == 0 || length % 16 != 0 || length > RESPONSE_CAPACITY) return false;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, decrypt ? PSA_KEY_USAGE_DECRYPT : PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);

    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attributes, (const uint8_t *)KEY, 16, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) return false;

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    status = decrypt ? psa_cipher_decrypt_setup(&operation, key_id, PSA_ALG_CBC_NO_PADDING)
                     : psa_cipher_encrypt_setup(&operation, key_id, PSA_ALG_CBC_NO_PADDING);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, (const uint8_t *)IV, 16);
    }
    uint8_t *output = malloc(length + 16);
    if (!output) {
        psa_cipher_abort(&operation);
        psa_destroy_key(key_id);
        return false;
    }
    size_t output_length = 0;
    size_t final_length = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, data, length, output, length + 16, &output_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, output + output_length,
                                   length + 16 - output_length, &final_length);
    }
    psa_cipher_abort(&operation);
    psa_destroy_key(key_id);
    bool valid = status == PSA_SUCCESS && output_length + final_length == length;
    if (valid) memcpy(data, output, length);
    free(output);
    return valid;
}

static bool decrypt_response(void) {
    if (response_length == 0 || response_length % 16 != 0 || response_length > RESPONSE_CAPACITY) {
        return false;
    }
    if (!aes_cbc(true, response_payload, response_length)) return false;
    while (response_length > 0 && response_payload[response_length - 1] == 0) response_length--;
    response_payload[response_length] = 0;
    cJSON *root = cJSON_ParseWithLength((char *)response_payload, response_length);
    bool valid = root && validate_response(root);
    cJSON_Delete(root);
    return valid;
}

static bool build_request(const char *identifier, const char *method, const char *request_id,
                          const char *command) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "deviceId", expected_device_id);
    cJSON_AddStringToObject(root, "type", "property");
    if (strcmp(method, "set") == 0) {
        cJSON_AddStringToObject(root, "identifier", identifier);
        cJSON_AddStringToObject(root, "method", method);
    } else {
        cJSON_AddStringToObject(root, "method", method);
        cJSON_AddStringToObject(root, "identifier", identifier);
    }
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    if (command) cJSON_AddStringToObject(params, "cmd", command);
    cJSON_AddStringToObject(root, "company", "apsystems");
    cJSON_AddStringToObject(root, "companyKey", "AmS4SV9oy3gk");
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(root, "productKey", "EZ1");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    size_t plain_length = strlen(json);
    size_t padded_length = (plain_length + 15) & ~(size_t)15;
    if (padded_length * 2 >= sizeof(request_payload)) {
        cJSON_free(json);
        return false;
    }
    uint8_t plain[REQUEST_CAPACITY / 2] = {0};
    memcpy(plain, json, plain_length);
    cJSON_free(json);
    if (!aes_cbc(false, plain, padded_length)) return false;
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < padded_length; ++index) {
        request_payload[index * 2] = hex[plain[index] >> 4];
        request_payload[index * 2 + 1] = hex[plain[index] & 0x0f];
    }
    request_length = padded_length * 2;
    request_offset = 0;
    return true;
}

static int write_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *argument) {
    (void)conn_handle;
    (void)attr;
    (void)argument;
    if (error->status != 0) {
        finish(false, "gatt_write_failed");
    } else {
        send_next_fragment();
    }
    return 0;
}

static void send_next_fragment(void) {
    if (!running || request_offset >= request_length) return;
    uint8_t fragment[46];
    size_t remaining = request_length - request_offset;
    size_t fragment_length;
    if (remaining > 40) {
        fragment[0] = 0x4d;
        fragment[1] = 0x10;
        fragment[2] = sequence++;
        fragment[3] = 42;
        fragment[4] = (uint8_t)(remaining & 0xff);
        fragment[5] = (uint8_t)(remaining >> 8);
        memcpy(fragment + 6, request_payload + request_offset, 40);
        request_offset += 40;
        fragment_length = 46;
    } else {
        fragment[0] = 0x4d;
        fragment[1] = 0x00;
        fragment[2] = sequence++;
        fragment[3] = (uint8_t)remaining;
        memcpy(fragment + 4, request_payload + request_offset, remaining);
        request_offset += remaining;
        fragment_length = remaining + 4;
    }
    int rc = ble_gattc_write_flat(connection_handle, write_handle, fragment, fragment_length,
                                  write_complete, NULL);
    if (rc != 0) finish(false, "gatt_write_start_failed");
}

static void start_request(operation_stage_t next_stage) {
    stage = next_stage;
    response_length = 0;
    response_remaining = -1;
    response_next_sequence = -1;
    bool built = next_stage == STAGE_DEVICE_INFO
                     ? build_request("deviceInfo", "get", "1", NULL)
                     : build_request("inverterCmd", "set", "16", STOP_COMMAND);
    if (!built) {
        finish(false, "request_build_failed");
        return;
    }
    send_next_fragment();
}

static void notification_received(struct os_mbuf *mbuf) {
    uint8_t fragment[256];
    uint16_t length = OS_MBUF_PKTLEN(mbuf);
    if (length < 4 || length > sizeof(fragment) || os_mbuf_copydata(mbuf, 0, length, fragment) != 0 ||
        fragment[0] != 0x4d) {
        finish(false, "invalid_notification");
        return;
    }
    if (response_next_sequence >= 0 && fragment[2] != response_next_sequence) {
        finish(false, "response_sequence_mismatch");
        return;
    }
    response_next_sequence = (fragment[2] + 1) & 0xff;
    size_t offset;
    size_t chunk_length;
    bool final;
    if (fragment[1] == 0x14 && length >= 6) {
        int declared = fragment[4] | (fragment[5] << 8);
        if (response_remaining >= 0 && declared != response_remaining) {
            finish(false, "response_length_mismatch");
            return;
        }
        offset = 6;
        chunk_length = length - 6;
        if ((int)chunk_length > declared) {
            finish(false, "response_fragment_exceeds_declared_length");
            return;
        }
        response_remaining = declared - (int)chunk_length;
        final = false;
    } else if (fragment[1] == 0x04 && fragment[3] == length - 4) {
        offset = 4;
        chunk_length = length - 4;
        if (response_remaining >= 0 && (int)chunk_length != response_remaining) {
            finish(false, "response_final_length_mismatch");
            return;
        }
        final = true;
    } else {
        char detail[64];
        snprintf(detail, sizeof(detail), "invalid_fragment_%02x_%02x_%02x_%02x_len%u",
                 fragment[0], fragment[1], fragment[2], fragment[3], length);
        finish(false, detail);
        return;
    }
    if (response_length + chunk_length > RESPONSE_CAPACITY) {
        finish(false, "response_too_large");
        return;
    }
    memcpy(response_payload + response_length, fragment + offset, chunk_length);
    response_length += chunk_length;
    if (!final) return;
    if (!decrypt_response()) {
        finish(false, stage == STAGE_DEVICE_INFO ? "identity_response_invalid" : "command_response_invalid");
    } else if (stage == STAGE_DEVICE_INFO) {
        if (active_result) active_result->identity_verified = true;
        start_request(STAGE_COMMAND);
    } else {
        finish(true, "stopped_verified");
    }
}

static int subscribe_complete(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *argument) {
    (void)conn_handle;
    (void)attr;
    (void)argument;
    if (error->status != 0) finish(false, "subscribe_failed");
    else start_request(STAGE_DEVICE_INFO);
    return 0;
}

static int descriptor_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 uint16_t chr_val_handle, const struct ble_gatt_dsc *descriptor,
                                 void *argument) {
    (void)chr_val_handle;
    (void)argument;
    if (error->status == 0 && descriptor &&
        ble_uuid_cmp(&descriptor->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
        cccd_handle = descriptor->handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!cccd_handle) {
            finish(false, "notify_cccd_missing");
            return 0;
        }
        uint8_t enable[] = {1, 0};
        int rc = ble_gattc_write_flat(conn_handle, cccd_handle, enable, sizeof(enable),
                                      subscribe_complete, NULL);
        if (rc != 0) finish(false, "subscribe_start_failed");
    } else if (error->status != 0) {
        finish(false, "descriptor_discovery_failed");
    }
    return 0;
}

static int characteristic_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic, void *argument) {
    (void)argument;
    if (error->status == 0 && characteristic) {
        if (ble_uuid_cmp(&characteristic->uuid.u, BLE_UUID16_DECLARE(0xff0a)) == 0) {
            write_handle = characteristic->val_handle;
        } else if (ble_uuid_cmp(&characteristic->uuid.u, BLE_UUID16_DECLARE(0xff0b)) == 0) {
            notify_handle = characteristic->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!write_handle || !notify_handle) {
            finish(false, "required_characteristic_missing");
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn_handle, notify_handle, service_end_handle,
                                         descriptor_discovered, NULL);
        if (rc != 0) finish(false, "descriptor_discovery_start_failed");
    } else if (error->status != 0) {
        finish(false, "characteristic_discovery_failed");
    }
    return 0;
}

static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *argument) {
    (void)argument;
    if (error->status == 0 && service) {
        service_start_handle = service->start_handle;
        service_end_handle = service->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!service_end_handle) {
            finish(false, "service_missing");
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn_handle, service_start_handle, service_end_handle,
                                         characteristic_discovered, NULL);
        if (rc != 0) finish(false, "characteristic_discovery_start_failed");
    } else if (error->status != 0) {
        finish(false, "service_discovery_failed");
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *argument) {
    (void)argument;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0 ||
                !fields.name || fields.name_len != strlen(expected_name) ||
                memcmp(fields.name, expected_name, fields.name_len) != 0) {
                return 0;
            }
            ble_addr_t address = event->disc.addr;
            target_found = true;
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(own_addr_type, &address, 10000, NULL, gap_event, NULL);
            if (rc != 0) finish(false, "connect_start_failed");
            return 0;
        }
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                finish(false, "connect_failed");
                return 0;
            }
            connection_handle = event->connect.conn_handle;
            if (ble_gattc_exchange_mtu(connection_handle, mtu_exchanged, NULL) != 0)
                finish(false, "mtu_exchange_start_failed");
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX:
            if (running && event->notify_rx.attr_handle == notify_handle) {
                notification_received(event->notify_rx.om);
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            connection_handle = BLE_HS_CONN_HANDLE_NONE;
            if (running) finish(false, "disconnected");
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (running && !target_found) finish(false, "device_not_found");
            return 0;
        default:
            return 0;
    }
}

static void host_task(void *argument) {
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void host_synced(void) {
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &own_addr_type) == 0) {
        xEventGroupSetBits(events, BLE_SYNCED);
    }
}

esp_err_t ez1_ble_init(void) {
    if (initialized) return ESP_OK;
    events = xEventGroupCreate();
    operation_mutex = xSemaphoreCreateMutex();
    if (!events || !operation_mutex) return ESP_ERR_NO_MEM;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    if (ble_att_set_preferred_mtu(247) != 0) return ESP_FAIL;
    ble_hs_cfg.sync_cb = host_synced;
    nimble_port_freertos_init(host_task);
    EventBits_t bits = xEventGroupWaitBits(events, BLE_SYNCED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(5000));
    if (!(bits & BLE_SYNCED)) return ESP_ERR_TIMEOUT;
    initialized = true;
    return ESP_OK;
}

bool ez1_ble_ready(void) {
    return initialized;
}

esp_err_t ez1_ble_stop(const watchdog_config_t *config, ez1_ble_result_t *result) {
    if (!initialized || !config || !result) return ESP_ERR_INVALID_STATE;
    memset(result, 0, sizeof(*result));
    if (xSemaphoreTake(operation_mutex, 0) != pdTRUE) {
        snprintf(result->detail, sizeof(result->detail), "busy");
        return ESP_ERR_TIMEOUT;
    }
    snprintf(expected_device_id, sizeof(expected_device_id), "%s", config->ez1_device_id);
    snprintf(expected_name, sizeof(expected_name), "EZ1_%s", config->ez1_device_id);
    active_result = result;
    connection_handle = BLE_HS_CONN_HANDLE_NONE;
    service_start_handle = service_end_handle = write_handle = notify_handle = cccd_handle = 0;
    target_found = false;
    sequence = 0;
    stage = STAGE_IDLE;
    running = true;
    xEventGroupClearBits(events, BLE_DONE | BLE_FAILED);
    struct ble_gap_disc_params parameters = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(own_addr_type, 10000, &parameters, gap_event, NULL);
    if (rc != 0) finish(false, "scan_start_failed");
    EventBits_t bits = 0;
    for (int elapsed = 0; elapsed < BLE_TIMEOUT_MS && !(bits & (BLE_DONE | BLE_FAILED));
         elapsed += 500) {
        bits = xEventGroupWaitBits(events, BLE_DONE | BLE_FAILED, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(500));
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    }
    if (!(bits & (BLE_DONE | BLE_FAILED))) finish(false, "timeout");
    bool success = (bits & BLE_DONE) != 0;
    active_result = NULL;
    xSemaphoreGive(operation_mutex);
    ESP_LOGI(TAG, "stop result=%s identity=%d", result->detail, result->identity_verified);
    return success ? ESP_OK : ESP_FAIL;
}
