from host.safety import Action, SafetyMachine, State


def armed_machine() -> SafetyMachine:
    machine = SafetyMachine(warning_ms=100, trip_ms=200)
    machine.boot(0, configured=True)
    machine.lease(1)
    return machine


def test_unprovisioned_cannot_write():
    machine = SafetyMachine(warning_ms=100, trip_ms=200)
    assert machine.boot(0, configured=False).state == State.UNPROVISIONED
    assert machine.tick(1000).action == Action.NONE


def test_boot_with_fresh_lease_armed():
    assert armed_machine().state == State.ARMED


def test_boot_without_lease_times_out():
    machine = SafetyMachine(warning_ms=100, trip_ms=200)
    machine.boot(0, configured=True)
    assert machine.tick(200).state == State.FAILSAFE_APPLYING_HTTP


def test_warning_and_trip_boundaries():
    machine = armed_machine()
    assert machine.tick(100).state == State.ARMED
    assert machine.tick(101).state == State.LEASE_WARNING
    assert machine.tick(200).state == State.LEASE_WARNING
    decision = machine.tick(201)
    assert decision.state == State.FAILSAFE_APPLYING_HTTP
    assert decision.action == Action.PERSIST_LATCH


def test_http_success_latches():
    machine = armed_machine()
    machine.tick(201)
    machine.latch_persisted()
    assert machine.http_result(True).state == State.FAILSAFE_LATCHED


def test_http_failure_tries_ble_and_failure_is_unreachable():
    machine = armed_machine()
    machine.tick(201)
    assert machine.http_result(False).action == Action.APPLY_BLE
    assert machine.ble_result(False).state == State.ACTUATOR_UNREACHABLE
    assert machine.latched


def test_http_failure_then_ble_success_latches():
    machine = armed_machine()
    machine.tick(201)
    assert machine.http_result(False).action == Action.APPLY_BLE
    assert machine.ble_result(True).state == State.FAILSAFE_LATCHED
    assert machine.latched


def test_returned_lease_does_not_release():
    machine = armed_machine()
    machine.tick(201)
    machine.http_result(True)
    assert machine.lease(210).state == State.FAILSAFE_LATCHED
    assert machine.latched


def test_release_requires_current_id_fresh_lease_and_two_renewals():
    machine = armed_machine()
    machine.tick(201)
    machine.http_result(True)
    current = machine.latch_id
    assert machine.release(202, current).reason == "fresh_lease_required"
    machine.lease(203)
    assert machine.release(204, current).reason == "fresh_lease_required"
    machine.lease(204)
    assert machine.release(204, current - 1).reason == "stale_latch_id"
    assert machine.release(205, current).state == State.ARMED
    assert not machine.latched


def test_reboot_preserves_latch():
    machine = armed_machine()
    machine.tick(201)
    latch_id = machine.latch_id
    decision = machine.boot(300, configured=True, persisted_latch=True)
    assert decision.action == Action.APPLY_HTTP
    assert machine.latched and machine.latch_id == latch_id


def test_old_release_cannot_unlock_new_incident():
    machine = armed_machine()
    machine.tick(201)
    old = machine.latch_id
    machine.http_result(True)
    machine.lease(202)
    machine.lease(203)
    machine.release(204, old)
    machine.lease(205)
    machine.tick(406)
    machine.http_result(True)
    machine.lease(407)
    machine.lease(408)
    assert machine.release(409, old).reason == "stale_latch_id"
