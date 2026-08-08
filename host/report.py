from __future__ import annotations

import json
import platform
import re
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path


REDACT_KEYS = {"wifi_password", "auth_secret", "authorization", "token", "password"}


def redact(value):
    if isinstance(value, dict):
        return {key: ("<redacted>" if key.lower() in REDACT_KEYS else redact(item)) for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


def redact_text(value: str) -> str:
    value = re.sub(r"Bearer\s+[0-9a-fA-F]{64}", "Bearer <redacted>", value)
    value = re.sub(
        r'("(?:auth_secret|wifi_password|password|token)"\s*:\s*")[^"]*(")',
        r"\1<redacted>\2",
        value,
        flags=re.IGNORECASE,
    )
    return value


def write_report(output: Path, results: dict, project_root: Path | None = None) -> Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    document = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "board": "Seeed Studio XIAO ESP32-S3 113991114",
        "firmware_target": "esp32s3",
        "http_ha_death_protection": results.get("http_ha_death_protection", "NOT_TESTED"),
        "ble_router_loss_protection": "NOT_READY_UNTIL_PROTOCOL_AND_HIL_VERIFIED",
        "results": redact(results),
    }
    report_json = json.dumps(document, indent=2)
    if output.suffix.lower() != ".zip":
        output.write_text(report_json, encoding="utf-8")
        return output
    root = project_root or Path(__file__).resolve().parents[1]
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        bundle.writestr("commissioning.json", report_json)
        for relative in (
            Path("dist/firmware/manifest.json"),
            Path("home_assistant/solar_watchdog.generated.yaml"),
        ):
            path = root / relative
            if path.exists():
                bundle.writestr(str(relative).replace("\\", "/"), redact_text(path.read_text(encoding="utf-8")))
        logs = root / ".watchdog/logs"
        if logs.exists():
            for path in logs.glob("*.log"):
                bundle.writestr(f"logs/{path.name}", redact_text(path.read_text(encoding="utf-8", errors="replace")))
    return output
