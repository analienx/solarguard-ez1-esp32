from __future__ import annotations

import re
from dataclasses import asdict, dataclass
from typing import Any

from .errors import EZ1SchemaInvalid


_SECRET_PATTERN = re.compile(r"[0-9a-fA-F]{64}")


def validate_secret(secret: str) -> str:
    """Accept exactly what the firmware accepts: 64 ASCII hex characters."""
    if not isinstance(secret, str) or not _SECRET_PATTERN.fullmatch(secret):
        raise ValueError("watchdog secret must be exactly 64 hexadecimal characters")
    return secret


def _required(data: dict[str, Any], name: str) -> Any:
    if name not in data or data[name] is None or data[name] == "":
        raise EZ1SchemaInvalid(f"missing field data.{name}")
    return data[name]


def _integer(data: dict[str, Any], name: str) -> int:
    value = _required(data, name)
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise EZ1SchemaInvalid(f"data.{name} is not an integer") from exc


@dataclass(frozen=True)
class EZ1DeviceInfo:
    device_id: str
    firmware: str
    ssid: str
    ip_address: str
    min_power: int
    max_power: int

    @classmethod
    def from_api(cls, payload: dict[str, Any]) -> "EZ1DeviceInfo":
        data = payload.get("data")
        if not isinstance(data, dict):
            raise EZ1SchemaInvalid("response has no data object")
        info = cls(
            device_id=str(_required(data, "deviceId")),
            firmware=str(_required(data, "devVer")),
            ssid=str(_required(data, "ssid")),
            ip_address=str(_required(data, "ipAddr")),
            min_power=_integer(data, "minPower"),
            max_power=_integer(data, "maxPower"),
        )
        if info.min_power < 0 or info.max_power < info.min_power:
            raise EZ1SchemaInvalid("invalid device power range")
        return info

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class ClampResult:
    device: EZ1DeviceInfo
    requested_watts: int
    acknowledged_watts: int
    verified_watts: int


@dataclass(frozen=True)
class ProvisionConfig:
    wifi_ssid: str
    wifi_password: str
    ez1_host: str
    ez1_device_id: str
    auth_secret: str
    lease_interval_s: int = 30
    warning_s: int = 120
    trip_s: int = 180

    def validate(self) -> None:
        # Mirrors firmware config_store_validate() and the NVS field sizes.
        if not self.wifi_ssid or len(self.wifi_ssid.encode()) > 32:
            raise ValueError("Wi-Fi SSID must contain 1-32 bytes")
        if len(self.wifi_password.encode()) > 64:
            raise ValueError("Wi-Fi password must be at most 64 bytes")
        if not self.ez1_host or not self.ez1_device_id:
            raise ValueError("EZ1 host and device ID are required")
        if len(self.ez1_host.encode()) > 128 or any(char in self.ez1_host for char in "/?#@"):
            raise ValueError("EZ1 host must be an IP address or hostname of at most 128 bytes")
        if len(self.ez1_device_id.encode()) > 64:
            raise ValueError("EZ1 device ID must be at most 64 bytes")
        validate_secret(self.auth_secret)
        if not (1 <= self.lease_interval_s < self.warning_s < self.trip_s <= 3600):
            raise ValueError("require lease_interval < warning < trip <= 3600 seconds")

    def serial_message(self) -> dict[str, Any]:
        self.validate()
        return {"protocol": 1, "command": "provision", "config": asdict(self)}