from __future__ import annotations

import json
import time

from .errors import ProvisionFailed
from .models import ProvisionConfig


def _exchange(
    port: str,
    message: dict,
    expected_command: str,
    baudrate: int,
    timeout: int = 15,
    resend: bool = True,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise ProvisionFailed("pyserial is not installed; install requirements.lock") from exc
    wire = json.dumps(message, separators=(",", ":")) + "\r\n"
    deadline = time.monotonic() + timeout
    recent: list[str] = []
    try:
        connection = serial.Serial(port=None, baudrate=baudrate, timeout=1)
        connection.dtr = False
        connection.rts = False
        connection.port = port
        connection.open()
        try:
            connection.reset_input_buffer()
            time.sleep(0.25)
            next_send = 0.0
            while time.monotonic() < deadline:
                if time.monotonic() >= next_send and (resend or next_send == 0.0):
                    connection.write(wire.encode("utf-8"))
                    connection.flush()
                    next_send = time.monotonic() + 1
                line = connection.readline()
                if not line:
                    continue
                try:
                    decoded = line.decode("utf-8", errors="replace").strip()
                    recent = (recent + [decoded])[-20:]
                    response = json.loads(decoded)
                except (UnicodeError, json.JSONDecodeError):
                    continue
                if response.get("protocol") == 1 and response.get("command") == expected_command:
                    if response.get("ok") is True:
                        return response
                    raise ProvisionFailed(str(response.get("error", "board rejected command")))
        finally:
            connection.close()
    except (OSError, ValueError) as exc:
        raise ProvisionFailed(str(exc)) from exc
    detail = " | ".join(recent)
    raise ProvisionFailed(
        f"timed out waiting for board {expected_command} response" + (f": {detail}" if detail else "")
    )


def provision(port: str, config: ProvisionConfig, baudrate: int = 115200) -> dict:
    return _exchange(port, config.serial_message(), "provision", baudrate)


def self_test(port: str, baudrate: int = 115200) -> dict:
    return _exchange(port, {"protocol": 1, "command": "self_test"}, "self_test", baudrate)


def wifi_test(port: str, ssid: str, password: str, baudrate: int = 115200) -> dict:
    if not ssid or len(ssid.encode("utf-8")) > 32 or len(password) > 64:
        raise ValueError("invalid Wi-Fi credentials")
    message = {
        "protocol": 1,
        "command": "wifi_test",
        "config": {"wifi_ssid": ssid, "wifi_password": password},
    }
    return _exchange(port, message, "wifi_test", baudrate)


def ble_stop_test(port: str, baudrate: int = 115200) -> dict:
    return _exchange(
        port,
        {"protocol": 1, "command": "ble_stop_test", "confirm": "BLESTOP"},
        "ble_stop_test",
        baudrate,
        timeout=40,
        resend=False,
    )
