#include "safety_state.h"

#include <stdio.h>
#include <string.h>

const char *watchdog_state_name(watchdog_state_t state) {
    static const char *names[] = {
        "UNPROVISIONED", "BOOT_CHECK", "ARMED", "LEASE_WARNING",
        "FAILSAFE_APPLYING_HTTP", "FAILSAFE_APPLYING_BLE",
        "FAILSAFE_LATCHED", "ACTUATOR_UNREACHABLE", "RECOVERY_WAIT",
    };
    return state <= WD_STATE_RECOVERY_WAIT ? names[state] : "UNKNOWN";
}

void safety_init(watchdog_status_t *status, bool configured, bool persisted_latch,
                 uint32_t latch_id, uint32_t boot_id, uint32_t warning_ms,
                 uint32_t trip_ms, int64_t now_ms) {
    memset(status, 0, sizeof(*status));
    status->configured = configured;
    status->latched = persisted_latch;
    status->latch_persisted = persisted_latch;
    status->latch_id = latch_id;
    status->boot_id = boot_id;
    status->boot_ms = now_ms;
    status->last_lease_ms = -1;
    status->warning_ms = warning_ms;
    status->trip_ms = trip_ms;
    status->last_verified_max_power = -1;
    status->state = !configured ? WD_STATE_UNPROVISIONED
                                : persisted_latch ? WD_STATE_FAILSAFE_APPLYING_HTTP
                                                  : WD_STATE_BOOT_CHECK;
    if (persisted_latch) {
        snprintf(status->latch_reason, sizeof(status->latch_reason), "persistent_latch");
    }
}

int64_t safety_lease_age_ms(const watchdog_status_t *status, int64_t now_ms) {
    int64_t origin = status->last_lease_ms >= 0 ? status->last_lease_ms : status->boot_ms;
    return now_ms > origin ? now_ms - origin : 0;
}

safety_action_t safety_on_lease(watchdog_status_t *status, int64_t now_ms) {
    if (!status->configured) {
        return SAFETY_ACTION_NONE;
    }
    status->last_lease_ms = now_ms;
    if (status->recent_leases < 2) {
        status->recent_leases++;
    }
    if (!status->latched) {
        status->state = WD_STATE_ARMED;
    }
    return SAFETY_ACTION_NONE;
}

safety_action_t safety_on_tick(watchdog_status_t *status, int64_t now_ms) {
    if (!status->configured || status->latched) {
        return SAFETY_ACTION_NONE;
    }
    int64_t age = safety_lease_age_ms(status, now_ms);
    if (age >= status->trip_ms) {
        status->latched = true;
        status->recent_leases = 0;
        status->latch_id++;
        status->state = WD_STATE_FAILSAFE_APPLYING_HTTP;
        snprintf(status->latch_reason, sizeof(status->latch_reason), "lease_expired");
        return SAFETY_ACTION_PERSIST_LATCH;
    }
    if (age >= status->warning_ms) {
        status->state = WD_STATE_LEASE_WARNING;
    } else if (status->last_lease_ms >= 0) {
        status->state = WD_STATE_ARMED;
    }
    return SAFETY_ACTION_NONE;
}

safety_action_t safety_latch_persisted(watchdog_status_t *status) {
    return status->state == WD_STATE_FAILSAFE_APPLYING_HTTP
               ? SAFETY_ACTION_APPLY_HTTP
               : SAFETY_ACTION_NONE;
}

safety_action_t safety_http_result(watchdog_status_t *status, bool success) {
    if (status->state != WD_STATE_FAILSAFE_APPLYING_HTTP) {
        return SAFETY_ACTION_NONE;
    }
    if (success) {
        status->state = WD_STATE_FAILSAFE_LATCHED;
        return SAFETY_ACTION_NONE;
    }
    status->state = WD_STATE_FAILSAFE_APPLYING_BLE;
    return SAFETY_ACTION_APPLY_BLE;
}

safety_action_t safety_ble_result(watchdog_status_t *status, bool success) {
    if (status->state != WD_STATE_FAILSAFE_APPLYING_BLE) {
        return SAFETY_ACTION_NONE;
    }
    status->state = success ? WD_STATE_FAILSAFE_LATCHED : WD_STATE_ACTUATOR_UNREACHABLE;
    return SAFETY_ACTION_NONE;
}

safety_action_t safety_on_release(watchdog_status_t *status, int64_t now_ms,
                                  uint32_t requested_latch_id) {
    bool fresh = status->last_lease_ms >= 0 &&
                 safety_lease_age_ms(status, now_ms) < status->warning_ms;
    if (!status->latched || status->state != WD_STATE_FAILSAFE_LATCHED ||
        requested_latch_id != status->latch_id ||
        !fresh || status->recent_leases < 2) {
        return SAFETY_ACTION_NONE;
    }
    status->state = WD_STATE_RECOVERY_WAIT;
    status->latched = false;
    status->state = WD_STATE_ARMED;
    status->latch_reason[0] = '\0';
    return SAFETY_ACTION_CLEAR_LATCH;
}
