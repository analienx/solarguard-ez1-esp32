from __future__ import annotations

import asyncio
import json
import shutil
import subprocess
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from .errors import BleProtocolUnknown, WatchdogError


PLATFORM_TOOLS_URL = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip"


def _run(command: list[str], timeout: int = 120) -> str:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, check=False)
    if result.returncode:
        raise WatchdogError((result.stdout + "\n" + result.stderr).strip())
    return result.stdout.strip()


def adb_path(local_root: Path) -> Path | None:
    found = shutil.which("adb")
    if found:
        return Path(found)
    local = local_root / "tools/android-platform-tools/platform-tools/adb.exe"
    return local if local.exists() else None


def install_platform_tools(local_root: Path) -> Path:
    destination = local_root / "tools/android-platform-tools"
    archive = destination / "platform-tools.zip"
    destination.mkdir(parents=True, exist_ok=True)
    urllib.request.urlretrieve(PLATFORM_TOOLS_URL, archive)
    with zipfile.ZipFile(archive) as package:
        base = destination.resolve()
        for member in package.infolist():
            target = (destination / member.filename).resolve()
            if base not in target.parents and target != base:
                raise WatchdogError("unsafe path in Platform-Tools archive")
        package.extractall(destination)
    adb = adb_path(local_root)
    if not adb:
        raise WatchdogError("download completed but adb.exe was not found")
    return adb


def android_info(adb: Path) -> dict:
    devices = _run([str(adb), "devices", "-l"])
    connected = [line for line in devices.splitlines()[1:] if len(line.split()) > 1 and line.split()[1] == "device"]
    if len(connected) != 1:
        raise WatchdogError("connect exactly one authorized Android device; `adb devices -l` must show state=device")
    return {
        "adb": _run([str(adb), "version"]),
        "device": connected[0],
        "model": _run([str(adb), "shell", "getprop", "ro.product.model"]),
        "android": _run([str(adb), "shell", "getprop", "ro.build.version.release"]),
        "fingerprint": _run([str(adb), "shell", "getprop", "ro.build.fingerprint"]),
    }


def pull_easypower_apk(adb: Path, output: Path) -> dict:
    packages = _run([str(adb), "shell", "pm", "list", "packages", "apsystems"])
    candidates = [line.removeprefix("package:").strip() for line in packages.splitlines() if line.strip()]
    package = "com.apsystems.apeasypower" if "com.apsystems.apeasypower" in candidates else (candidates[0] if len(candidates) == 1 else None)
    if not package:
        raise WatchdogError(f"could not identify one APsystems package; candidates={candidates}")
    paths = [line.removeprefix("package:").strip() for line in _run([str(adb), "shell", "pm", "path", package]).splitlines()]
    output.mkdir(parents=True, exist_ok=True)
    pulled = []
    for index, remote in enumerate(paths):
        destination = output / f"{index:02d}-{Path(remote).name}"
        _run([str(adb), "pull", remote, str(destination)], timeout=300)
        pulled.append(str(destination))
    manifest = {"package": package, "paths": pulled, "captured_at": datetime.now(timezone.utc).isoformat()}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def capture_bugreport(adb: Path, output: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    markers = []
    print("Use AP EasyPower Direct Connect. Keep this capture limited to the listed actions.")
    for action in ("connected", "view live data", "view settings without changes"):
        input(f"Perform '{action}', then press ENTER: ")
        markers.append({"action": action, "time": datetime.now(timezone.utc).isoformat()})
    bugreport = output / "bugreport.zip"
    _run([str(adb), "bugreport", str(bugreport)], timeout=900)
    candidates = []
    if bugreport.exists() and zipfile.is_zipfile(bugreport):
        with zipfile.ZipFile(bugreport) as archive:
            candidates = [name for name in archive.namelist() if "btsnoop" in name.lower()]
            for name in candidates:
                destination = output / Path(name).name
                destination.write_bytes(archive.read(name))
    manifest = {"markers": markers, "bugreport": str(bugreport), "btsnoop_candidates": candidates}
    (output / "capture-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def analyze_apk(apk: Path, output: Path, jadx: str = "jadx") -> dict:
    output.mkdir(parents=True, exist_ok=True)
    _run([jadx, "--output-dir", str(output), str(apk)], timeout=900)
    needles = ("BluetoothGatt", "writeCharacteristic", "UUID.fromString", "setMaxPower", "power limit")
    findings = []
    for path in output.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in {".java", ".kt", ".xml"}:
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            if any(needle.lower() in line.lower() for needle in needles):
                findings.append({"file": str(path), "line": number, "text": line.strip()[:300]})
    report = {"confidence": "LOW_STATIC_ONLY", "findings": findings[:1000]}
    (output / "analysis.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


def extract_hci(capture: Path, output: Path, tshark: str = "tshark") -> Path:
    fields = [
        "frame.time_epoch", "_ws.col.Direction", "btatt.opcode", "btatt.handle",
        "btgatt.uuid128", "btatt.value",
    ]
    command = [tshark, "-r", str(capture), "-Y", "btatt", "-T", "fields", "-E", "header=y", "-E", "separator=,"]
    for field in fields:
        command.extend(["-e", field])
    csv = _run(command, timeout=300)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(csv + "\n", encoding="utf-8")
    return output


async def _ble_write(protocol: dict) -> dict:
    try:
        from bleak import BleakClient
    except ImportError as exc:
        raise BleProtocolUnknown("bleak is not installed") from exc
    if protocol.get("watts") != 30 or protocol.get("evidence") not in {"MEDIUM", "HIGH", "PRODUCTION"}:
        raise BleProtocolUnknown("protocol file must declare watts=30 and evidence MEDIUM or better")
    payload = bytes.fromhex(protocol["payload_hex"])
    async with BleakClient(protocol["address"], timeout=15) as client:
        await client.write_gatt_char(protocol["write_characteristic"], payload, response=True)
        verification = None
        if protocol.get("verify_characteristic"):
            verification = bytes(await client.read_gatt_char(protocol["verify_characteristic"])).hex()
    expected = protocol.get("expected_response_hex")
    if expected is not None and verification != expected.lower():
        raise BleProtocolUnknown(f"BLE readback {verification!r} did not match captured evidence")
    return {"write_completed": True, "verification_hex": verification, "production_ready": False}


def ble_write_30(protocol_path: Path) -> dict:
    protocol = json.loads(protocol_path.read_text(encoding="utf-8"))
    return asyncio.run(_ble_write(protocol))
