from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "firmware/build"
DIST = ROOT / "dist/firmware"


def main() -> int:
    arguments = json.loads((BUILD / "flasher_args.json").read_text(encoding="utf-8"))
    flash_files = arguments.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError("firmware/build/flasher_args.json has no flash_files mapping")
    DIST.mkdir(parents=True, exist_ok=True)
    image = DIST / "ez1-watchdog-xiao-esp32s3.factory.bin"
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin", "-o", str(image)]
    for offset, filename in sorted(flash_files.items(), key=lambda item: int(item[0], 0)):
        command.extend([offset, str(BUILD / filename)])
    subprocess.run(command, check=True)
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    manifest = {
        "version": "0.1.0",
        "target": "esp32s3",
        "board": "xiao_esp32s3",
        "esp_idf": "6.0.2",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "image": image.name,
        "sha256": digest,
    }
    (DIST / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())