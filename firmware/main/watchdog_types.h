#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WATCHDOG_FAILSAFE_WATTS 30
#define WATCHDOG_SECRET_HEX_LEN 64
#define WATCHDOG_EVENT_QUEUE_LENGTH 16

typedef enum {
    WD_STATE_UNPROVISIONED,
    WD_STATE_BOOT_CHECK,
    WD_STATE_ARMED,
    WD_STATE_LEASE_WARNING,
    WD_STATE_FAILSAFE_APPLYING_HTTP,
    WD_STATE_FAILSAFE_APPLYING_BLE,
    WD_STATE_FAILSAFE_LATCHED,
    WD_STATE_ACTUATOR_UNREACHABLE,
    WD_STATE_RECOVERY_WAIT, /* reserved for API compatibility; never entered */
} watchdog_state_t;

typedef enum {
    WD_EVENT_LEASE,
    WD_EVENT_RELEASE,
    WD_EVENT_EZ1_PROBE,
    WD_EVENT_PROVISIONED,
} watchdog_event_type_t;

typedef struct {
    watchdog_event_type_t type;
    uint32_t latch_id;
    int64_t at_ms; /* monotonic receipt time, stamped by the HTTP handler */
} watchdog_event_t;

typedef struct {
    uint32_t schema_version;
    char wifi_ssid[33];
    char wifi_password[65];
    char ez1_host[129];
    char ez1_device_id[65];
    char auth_secret[WATCHDOG_SECRET_HEX_LEN + 1];
    uint32_t lease_interval_s;
    uint32_t warning_s;
    uint32_t trip_s;
} watchdog_config_t;

typedef struct {
    watchdog_state_t state;
    uint32_t boot_id;
    int64_t boot_ms;
    int64_t last_lease_ms;
    uint32_t warning_ms;
    uint32_t trip_ms;
    bool configured;
    bool latched;
    bool latch_persisted;
    uint32_t latch_id;
    uint8_t recent_leases;
    char latch_reason[32];
    bool ez1_identity_verified;
    bool ez1_http_reachable;
    bool ble_identity_verified;
    int64_t last_ble_identity_ms;
    bool ble_ready;
    bool ble_advert_seen;
    int64_t last_health_check_ms;
    char last_ble_result[64];
    int last_verified_max_power;
    uint32_t override_count;
    char last_action[32];
    char last_action_path[8];
    char last_result[64];
    char reset_reason[24];
} watchdog_status_t;

const char *watchdog_state_name(watchdog_state_t state);
