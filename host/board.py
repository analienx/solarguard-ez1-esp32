from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from .errors import FlashFailed, NoEspBoard, WrongEspChip


@dataclass(frozen=True)
class SerialBoard:
    port: str
    description: str
    hardware_id: str


def serial_boards() -> list[SerialBoard]:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise NoEspBoard("pyserial is not installed; install requirements.lock") from exc
    return [SerialBoard(p.device, p.description or "", p.hwid or "") for p in list_ports.comports()]


def esptool(port: str, *arguments: str) -> str:
    command = [sys.executable, "-m", "esptool", "--port", port, *arguments]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
    output = (result.stdout + "\n" + result.stderr).strip()
    if result.returncode:
        raise FlashFailed(output or "esptool failed")
    return output


def verify_esp32s3(port: str) -> str:
    output = esptool(port, "chip-id")
    normalized = output.upper().replace("_", "-")
    if "ESP32-S3" not in normalized:
        raise WrongEspChip(f"expected ESP32-S3 on {port}; esptool reported:\n{output}")
    return output


def flash_factory(port: str, image: Path, manifest_path: Path) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected = manifest.get("sha256")
    actual = hashlib.sha256(image.read_bytes()).hexdigest()
    if not isinstance(expected, str) or actual.lower() != expected.lower():
        raise FlashFailed("factory image SHA-256 does not match manifest")
    if manifest.get("target") != "esp32s3" or manifest.get("board") != "xiao_esp32s3":
        raise FlashFailed("manifest target is not the Seeed XIAO ESP32-S3")
    verify_esp32s3(port)
    esptool(port, "write-flash", "--flash-size", "detect", "0x0", str(image))