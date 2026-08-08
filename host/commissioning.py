from __future__ import annotations

import json
import time
from datetime import datetime, timezone
from pathlib import Path

from .errors import LeaseTestFailed
from .watchdog_api import WatchdogClient


def wait_for_state(client: WatchdogClient, expected: set[str], timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    last = {}
    while time.monotonic() < deadline:
        last = client.status()
        if last.get("state") in expected:
            return last
        time.sleep(2)
    raise LeaseTestFailed(f"timed out waiting for {sorted(expected)}; last status: {last.get('state')}")


def read_only_probe(client: WatchdogClient) -> dict:
    client.ez1_probe()
    deadline = time.monotonic() + 10
    status = {}
    while time.monotonic() < deadline:
        status = client.status()
        ez1 = status.get("ez1") or {}
        if ez1.get("identity_verified") and ez1.get("http_reachable"):
            return status
        time.sleep(0.5)
    raise LeaseTestFailed(f"XIAO could not verify the configured EZ1: {status.get('last_action')}")


def lease_latch_test(client: WatchdogClient) -> dict:
    client.lease()
    time.sleep(1)
    client.lease()
    armed = wait_for_state(client, {"ARMED"}, 10)
    timeout_s = float(armed.get("trip_timeout_ms", 180000)) / 1000 + 20
    latched = wait_for_state(client, {"FAILSAFE_LATCHED", "ACTUATOR_UNREACHABLE"}, timeout_s)
    if latched.get("state") != "FAILSAFE_LATCHED":
        raise LeaseTestFailed("lease expired, but the XIAO could not prove the 30 W clamp")
    latch_id = int(latched["latch_id"])
    client.lease()
    time.sleep(1)
    client.lease()
    still_latched = client.status()
    if still_latched.get("state") != "FAILSAFE_LATCHED":
        raise LeaseTestFailed("returning leases unexpectedly released the fail-safe")
    try:
        client.release(latch_id - 1 if latch_id else latch_id + 1)
    except LeaseTestFailed:
        pass
    else:
        raise LeaseTestFailed("stale release was unexpectedly accepted")
    client.release(latch_id)
    released = wait_for_state(client, {"ARMED"}, 10)
    return {"armed": armed, "latched": latched, "released": released}


def soak(client: WatchdogClient, output: Path, duration_s: int, interval_s: int, expected_state: str) -> dict:
    if duration_s < interval_s or interval_s < 5:
        raise ValueError("duration must exceed an interval of at least five seconds")
    output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + duration_s
    samples = failures = 0
    with output.open("a", encoding="utf-8") as stream:
        while time.monotonic() < deadline:
            try:
                status = client.status()
                ok = status.get("state") == expected_state
                failures += 0 if ok else 1
                record = {"time": datetime.now(timezone.utc).isoformat(), "ok": ok, "status": status}
            except LeaseTestFailed as exc:
                failures += 1
                record = {"time": datetime.now(timezone.utc).isoformat(), "ok": False, "error": str(exc)}
            stream.write(json.dumps(record, separators=(",", ":")) + "\n")
            stream.flush()
            samples += 1
            time.sleep(interval_s)
    return {"samples": samples, "failures": failures, "expected_state": expected_state, "log": str(output)}
