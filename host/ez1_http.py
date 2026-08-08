from __future__ import annotations

import json
import time
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from .errors import (
    ClampVerifyFailed,
    ClampWriteFailed,
    EZ1IdentityMismatch,
    EZ1SchemaInvalid,
    EZ1Unreachable,
)
from .models import ClampResult, EZ1DeviceInfo

FAILSAFE_WATTS = 30


class EZ1Client:
    def __init__(
        self,
        host: str,
        port: int = 8050,
        timeout: float = 3.0,
        attempts: int = 3,
        opener: Callable[..., Any] = urlopen,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        if not host or any(char in host for char in "/?#@"):
            raise ValueError("host must be an IP address or hostname without a URL path")
        if not 1 <= port <= 65535 or attempts < 1 or timeout <= 0:
            raise ValueError("invalid port, attempt count, or timeout")
        self.base_url = f"http://{host}:{port}"
        self.timeout = timeout
        self.attempts = attempts
        self._opener = opener
        self._sleep = sleeper

    def _get(self, endpoint: str, params: dict[str, int] | None = None) -> dict[str, Any]:
        url = f"{self.base_url}/{endpoint}"
        if params:
            url += "?" + urlencode(params)
        last_error: Exception | None = None
        for attempt in range(self.attempts):
            try:
                request = Request(url, method="GET", headers={"Accept": "application/json"})
                with self._opener(request, timeout=self.timeout) as response:
                    if getattr(response, "status", 200) != 200:
                        raise EZ1Unreachable(f"{endpoint} returned HTTP {response.status}")
                    raw = response.read(16385)
                    if len(raw) > 16384:
                        raise EZ1SchemaInvalid(f"{endpoint} response is too large")
                try:
                    payload = json.loads(raw.decode("utf-8"))
                except (UnicodeError, json.JSONDecodeError) as exc:
                    raise EZ1SchemaInvalid(f"{endpoint} response is not valid UTF-8 JSON") from exc
                if not isinstance(payload, dict):
                    raise EZ1SchemaInvalid(f"{endpoint} response is not an object")
                return payload
            except EZ1SchemaInvalid:
                raise
            except (HTTPError, URLError, TimeoutError, OSError) as exc:
                last_error = exc
                if attempt + 1 < self.attempts:
                    self._sleep((0.5, 1.0, 2.0)[min(attempt, 2)])
        raise EZ1Unreachable(f"{endpoint} failed after {self.attempts} attempts: {last_error}")

    @staticmethod
    def _max_power(payload: dict[str, Any], endpoint: str) -> int:
        data = payload.get("data")
        if not isinstance(data, dict) or data.get("maxPower") in (None, ""):
            raise EZ1SchemaInvalid(f"{endpoint} response has no data.maxPower")
        try:
            value = int(data["maxPower"])
        except (TypeError, ValueError) as exc:
            raise EZ1SchemaInvalid(f"{endpoint} data.maxPower is not an integer") from exc
        if not 0 <= value <= 2000:
            raise EZ1SchemaInvalid(f"{endpoint} data.maxPower is implausible")
        return value

    def get_device_info(self) -> EZ1DeviceInfo:
        return EZ1DeviceInfo.from_api(self._get("getDeviceInfo"))

    def get_max_power(self) -> int:
        return self._max_power(self._get("getMaxPower"), "getMaxPower")

    def get_output_data(self) -> dict[str, Any]:
        payload = self._get("getOutputData")
        if not isinstance(payload.get("data"), dict):
            raise EZ1SchemaInvalid("getOutputData response has no data object")
        return payload["data"]

    def _set_failsafe_only(self) -> int:
        """The production write surface is deliberately fixed at 30 W."""
        try:
            acknowledged = self._max_power(
                self._get("setMaxPower", {"p": FAILSAFE_WATTS}), "setMaxPower"
            )
        except (EZ1Unreachable, EZ1SchemaInvalid) as exc:
            raise ClampWriteFailed(str(exc)) from exc
        if acknowledged != FAILSAFE_WATTS:
            raise ClampWriteFailed(f"inverter acknowledged {acknowledged} W, expected 30 W")
        return acknowledged

    def apply_failsafe(self, expected_device_id: str) -> ClampResult:
        info = self.get_device_info()
        if info.device_id != expected_device_id:
            raise EZ1IdentityMismatch(
                f"configured device {expected_device_id!r}, reached {info.device_id!r}"
            )
        if info.min_power > FAILSAFE_WATTS:
            raise ClampWriteFailed(
                f"device reports minimum {info.min_power} W; refusing unsupported 30 W write"
            )
        acknowledged = self._set_failsafe_only()
        verified = self.get_max_power()
        if verified != FAILSAFE_WATTS:
            raise ClampVerifyFailed(f"readback is {verified} W, expected 30 W")
        return ClampResult(info, FAILSAFE_WATTS, acknowledged, verified)
