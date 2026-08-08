import pytest

hypothesis = pytest.importorskip("hypothesis")
from hypothesis import given, strategies as st

from host.safety import SafetyMachine, State


@given(st.lists(st.sampled_from(["lease", "tick", "http_ok", "http_fail", "ble_ok", "ble_fail", "release_bad"]), min_size=1, max_size=100))
def test_latched_machine_never_auto_releases(events):
    machine = SafetyMachine(warning_ms=10, trip_ms=20)
    machine.boot(0, configured=True)
    machine.lease(1)
    machine.tick(21)
    incident = machine.latch_id
    now = 22
    for event in events:
        now += 1
        if event == "lease":
            machine.lease(now)
        elif event == "tick":
            machine.tick(now)
        elif event == "http_ok":
            machine.http_result(True)
        elif event == "http_fail":
            machine.http_result(False)
        elif event in ("ble_ok", "ble_fail"):
            machine.ble_result(event == "ble_ok")
        else:
            machine.release(now, incident + 1)
        assert machine.latched
        assert machine.state != State.ARMED
