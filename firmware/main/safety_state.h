#pragma once

#include "watchdog_types.h"

typedef enum {
    SAFETY_ACTION_NONE,
    SAFETY_ACTION_PERSIST_LATCH,
    SAFETY_ACTION_APPLY_HTTP,
    SAFETY_ACTION_APPLY_BLE,
    SAFETY_ACTION_CLEAR_LATCH,
} safety_action_t;

void safety_init(watchdog_status_t *status, bool configured, bool persisted_latch,
                 uint32_t latch_id, uint32_t boot_id, uint32_t warning_ms,
                 uint32_t trip_ms, int64_t now_ms);
safety_action_t safety_on_lease(watchdog_status_t *status, int64_t now_ms);
safety_action_t safety_trip(watchdog_status_t *status, const char *reason);
safety_action_t safety_on_tick(watchdog_status_t *status, int64_t now_ms);
safety_action_t safety_latch_persisted(watchdog_status_t *status);
safety_action_t safety_http_result(watchdog_status_t *status, bool success);
safety_action_t safety_ble_result(watchdog_status_t *status, bool success);
safety_action_t safety_on_release(watchdog_status_t *status, int64_t now_ms,
                                  uint32_t requested_latch_id);
int64_t safety_lease_age_ms(const watchdog_status_t *status, int64_t now_ms);
