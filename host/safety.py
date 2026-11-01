from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class State(str, Enum):
    UNPROVISIONED = "UNPROVISIONED"
    BOOT_CHECK = "BOOT_CHECK"
    ARMED = "ARMED"
    LEASE_WARNING = "LEASE_WARNING"
    FAILSAFE_APPLYING_HTTP = "FAILSAFE_APPLYING_HTTP"
    FAILSAFE_APPLYING_BLE = "FAILSAFE_APPLYING_BLE"
    FAILSAFE_LATCHED = "FAILSAFE_LATCHED"
    ACTUATOR_UNREACHABLE = "ACTUATOR_UNREACHABLE"
    RECOVERY_WAIT = "RECOVERY_WAIT"  # reserved for API compatibility; never entered


class Action(str, Enum):
    NONE = "NONE"
    APPLY_HTTP = "APPLY_HTTP"
    APPLY_BLE = "APPLY_BLE"
    PERSIST_LATCH = "PERSIST_LATCH"
    CLEAR_LATCH = "CLEAR_LATCH"


@dataclass(frozen=True)
class Decision:
    state: State
    action: Action = Action.NONE
    reason: str | None = None


@dataclass
class SafetyMachine:
    warning_ms: int = 120_000
    trip_ms: int = 180_000
    state: State = State.UNPROVISIONED
    configured: bool = False
    latched: bool = False
    latch_id: int = 0
    boot_ms: int = 0
    last_lease_ms: int | None = None
    recent_leases: int = 0

    def __post_init__(self) -> None:
        if not 0 < self.warning_ms < self.trip_ms:
            raise ValueError("warning_ms must be less than trip_ms")

    def boot(self, now_ms: int, configured: bool, persisted_latch: bool = False) -> Decision:
        self.boot_ms = now_ms
        self.last_lease_ms = None
        self.recent_leases = 0
        self.configured = configured
        self.latched = persisted_latch
        if not configured:
            self.state = State.UNPROVISIONED
            return Decision(self.state)
        if persisted_latch:
            self.state = State.FAILSAFE_APPLYING_HTTP
            return Decision(self.state, Action.APPLY_HTTP, "persistent_latch")
        self.state = State.BOOT_CHECK
        return Decision(self.state)

    def lease(self, now_ms: int, authenticated: bool = True) -> Decision:
        if not authenticated or not self.configured:
            return Decision(self.state, reason="lease_rejected")
        if self.last_lease_ms is None or now_ms > self.last_lease_ms:
            self.last_lease_ms = now_ms
        self.recent_leases = min(self.recent_leases + 1, 2)
        if self.latched:
            return Decision(self.state, reason="lease_accepted_latch_retained")
        self.state = State.ARMED
        return Decision(self.state)

    def lease_age(self, now_ms: int) -> int:
        origin = self.last_lease_ms if self.last_lease_ms is not None else self.boot_ms
        return max(0, now_ms - origin)

    def tick(self, now_ms: int) -> Decision:
        if not self.configured or self.latched:
            return Decision(self.state)
        age = self.lease_age(now_ms)
        if age >= self.trip_ms:
            return self.trip("lease_expired")
        if age >= self.warning_ms:
            self.state = State.LEASE_WARNING
        elif self.last_lease_ms is not None:
            self.state = State.ARMED
        return Decision(self.state)

    def trip(self, reason: str) -> Decision:
        if not self.configured or self.latched:
            return Decision(self.state)
        self.latched = True
        self.latch_id += 1
        self.recent_leases = 0
        self.state = State.FAILSAFE_APPLYING_HTTP
        return Decision(self.state, Action.PERSIST_LATCH, reason)

    def latch_persisted(self) -> Decision:
        if self.state != State.FAILSAFE_APPLYING_HTTP:
            return Decision(self.state)
        return Decision(self.state, Action.APPLY_HTTP, "lease_expired")

    def http_result(self, success: bool) -> Decision:
        if self.state != State.FAILSAFE_APPLYING_HTTP:
            return Decision(self.state)
        if success:
            self.state = State.FAILSAFE_LATCHED
            return Decision(self.state)
        self.state = State.FAILSAFE_APPLYING_BLE
        return Decision(self.state, Action.APPLY_BLE, "http_failed")

    def ble_result(self, success: bool) -> Decision:
        if self.state != State.FAILSAFE_APPLYING_BLE:
            return Decision(self.state)
        self.state = State.FAILSAFE_LATCHED if success else State.ACTUATOR_UNREACHABLE
        return Decision(self.state, reason=None if success else "all_actuators_unreachable")

    def release(self, now_ms: int, latch_id: int, authenticated: bool = True) -> Decision:
        fresh = self.last_lease_ms is not None and self.lease_age(now_ms) < self.warning_ms
        if not authenticated:
            return Decision(self.state, reason="release_unauthorized")
        if not self.latched or self.state != State.FAILSAFE_LATCHED or latch_id != self.latch_id:
            return Decision(self.state, reason="stale_latch_id")
        if not fresh or self.recent_leases < 2:
            return Decision(self.state, reason="fresh_lease_required")
        self.latched = False
        self.state = State.ARMED
        return Decision(self.state, Action.CLEAR_LATCH)
