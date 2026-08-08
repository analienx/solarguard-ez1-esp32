from __future__ import annotations

import asyncio
import json
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path


@dataclass(frozen=True)
class Advertisement:
    address: str
    name: str | None
    rssi: int | None
    service_uuids: list[str]
    manufacturer_data: dict[str, str]


async def _scan(seconds: float) -> list[Advertisement]:
    try:
        from bleak import BleakScanner
    except ImportError as exc:
        raise RuntimeError("bleak is not installed; install requirements.lock") from exc
    found: dict[str, Advertisement] = {}

    def callback(device, data) -> None:
        found[device.address] = Advertisement(
            address=device.address,
            name=data.local_name or device.name,
            rssi=getattr(data, "rssi", None),
            service_uuids=list(data.service_uuids or []),
            manufacturer_data={str(k): bytes(v).hex() for k, v in data.manufacturer_data.items()},
        )

    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    await asyncio.sleep(seconds)
    await scanner.stop()
    return sorted(found.values(), key=lambda item: item.address)


def scan(seconds: float, output: Path) -> list[Advertisement]:
    if not 5 <= seconds <= 180:
        raise ValueError("scan duration must be 5-180 seconds")
    results = asyncio.run(_scan(seconds))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(
            {
                "captured_at": datetime.now(timezone.utc).isoformat(),
                "read_only": True,
                "devices": [asdict(item) for item in results],
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    return results