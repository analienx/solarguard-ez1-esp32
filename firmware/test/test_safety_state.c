#include <assert.h>

#include "safety_state.h"

int main(void) {
    watchdog_status_t state;
    safety_init(&state, true, false, 0, 1, 100, 200, 0);
    assert(state.state == WD_STATE_BOOT_CHECK);
    safety_on_lease(&state, 1);
    assert(state.state == WD_STATE_ARMED);
    assert(safety_on_tick(&state, 200) == SAFETY_ACTION_NONE);
    assert(safety_on_tick(&state, 201) == SAFETY_ACTION_PERSIST_LATCH);
    assert(state.latched && state.latch_id == 1);
    assert(safety_latch_persisted(&state) == SAFETY_ACTION_APPLY_HTTP);
    safety_http_result(&state, true);
    assert(state.state == WD_STATE_FAILSAFE_LATCHED);
    safety_on_lease(&state, 202);
    assert(state.state == WD_STATE_FAILSAFE_LATCHED);
    assert(safety_on_release(&state, 203, 0) == SAFETY_ACTION_NONE);
    safety_on_lease(&state, 203);
    assert(safety_on_release(&state, 204, 1) == SAFETY_ACTION_CLEAR_LATCH);
    assert(state.state == WD_STATE_ARMED && !state.latched);

    safety_init(&state, true, false, 0, 2, 100, 200, 0);
    safety_on_lease(&state, 1);
    safety_on_tick(&state, 201);
    safety_latch_persisted(&state);
    assert(safety_http_result(&state, false) == SAFETY_ACTION_APPLY_BLE);
    safety_ble_result(&state, true);
    assert(state.state == WD_STATE_FAILSAFE_LATCHED && state.latched);
    return 0;
}
