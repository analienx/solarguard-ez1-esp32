#include <assert.h>
#include <string.h>

#include "safety_state.h"

static void test_trip_latch_and_release(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 0, 1, 100, 200, 0);
    assert(state.state == WD_STATE_BOOT_CHECK);
    safety_on_lease(&state, 1);
    assert(state.state == WD_STATE_ARMED);
    assert(safety_on_tick(&state, 200) == SAFETY_ACTION_NONE);
    assert(safety_on_tick(&state, 201) == SAFETY_ACTION_PERSIST_LATCH);
    assert(state.latched && state.latch_id == 1);
    assert(strcmp(state.latch_reason, "lease_expired") == 0);
    assert(safety_latch_persisted(&state) == SAFETY_ACTION_APPLY_HTTP);
    safety_http_result(&state, true);
    assert(state.state == WD_STATE_FAILSAFE_LATCHED);
    safety_on_lease(&state, 202);
    assert(state.state == WD_STATE_FAILSAFE_LATCHED);
    assert(safety_on_release(&state, 203, 0) == SAFETY_ACTION_NONE);
    safety_on_lease(&state, 203);
    assert(safety_on_release(&state, 204, 1) == SAFETY_ACTION_CLEAR_LATCH);
    assert(state.state == WD_STATE_ARMED && !state.latched);
}

static void test_ble_fallback(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 0, 2, 100, 200, 0);
    safety_on_lease(&state, 1);
    safety_on_tick(&state, 201);
    safety_latch_persisted(&state);
    assert(safety_http_result(&state, false) == SAFETY_ACTION_APPLY_BLE);
    safety_ble_result(&state, true);
    assert(state.state == WD_STATE_FAILSAFE_LATCHED && state.latched);
}

static void test_both_transports_fail_blocks_release(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 0, 3, 100, 200, 0);
    safety_on_tick(&state, 200);
    safety_latch_persisted(&state);
    safety_http_result(&state, false);
    safety_ble_result(&state, false);
    assert(state.state == WD_STATE_ACTUATOR_UNREACHABLE && state.latched);
    safety_on_lease(&state, 210);
    safety_on_lease(&state, 220);
    assert(safety_on_release(&state, 221, state.latch_id) == SAFETY_ACTION_NONE);
    assert(state.latched);
}

static void test_persisted_latch_boot(void) {
    watchdog_status_t state;
    safety_init(&state, true, true, 7, 4, 100, 200, 0);
    assert(state.latched && state.latch_persisted && state.latch_id == 7);
    assert(state.state == WD_STATE_FAILSAFE_APPLYING_HTTP);
    assert(strcmp(state.latch_reason, "persistent_latch") == 0);
    assert(safety_on_tick(&state, 10000) == SAFETY_ACTION_NONE);
    safety_http_result(&state, true);
    /* Leases from before boot do not exist; two fresh ones are required. */
    safety_on_lease(&state, 10);
    assert(safety_on_release(&state, 11, 7) == SAFETY_ACTION_NONE);
    safety_on_lease(&state, 20);
    assert(safety_on_release(&state, 21, 6) == SAFETY_ACTION_NONE);
    assert(safety_on_release(&state, 21, 7) == SAFETY_ACTION_CLEAR_LATCH);
}

static void test_release_requires_fresh_lease(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 0, 5, 100, 200, 0);
    safety_on_tick(&state, 200);
    safety_latch_persisted(&state);
    safety_http_result(&state, true);
    safety_on_lease(&state, 300);
    safety_on_lease(&state, 310);
    assert(safety_on_release(&state, 410, 1) == SAFETY_ACTION_NONE);
    assert(safety_on_release(&state, 409, 1) == SAFETY_ACTION_CLEAR_LATCH);
}

static void test_out_of_order_lease_time_never_moves_backwards(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 0, 6, 100, 200, 0);
    safety_on_lease(&state, 150);
    safety_on_lease(&state, 50);
    assert(state.last_lease_ms == 150);
    assert(safety_on_tick(&state, 349) == SAFETY_ACTION_NONE);
}

static void test_explicit_trip(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 4, 7, 100, 200, 0);
    assert(safety_trip(&state, "repeated_abnormal_reset") == SAFETY_ACTION_PERSIST_LATCH);
    assert(state.latched && !state.latch_persisted && state.latch_id == 5);
    assert(strcmp(state.latch_reason, "repeated_abnormal_reset") == 0);
    assert(safety_trip(&state, "again") == SAFETY_ACTION_NONE);
    assert(state.latch_id == 5);

    safety_init(&state, false, false, 0, 8, 100, 200, 0);
    assert(safety_trip(&state, "x") == SAFETY_ACTION_NONE && !state.latched);
}

int main(void) {
    test_trip_latch_and_release();
    test_ble_fallback();
    test_both_transports_fail_blocks_release();
    test_persisted_latch_boot();
    test_release_requires_fresh_lease();
    test_out_of_order_lease_time_never_moves_backwards();
    test_explicit_trip();
    return 0;
}
