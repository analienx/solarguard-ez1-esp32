from __future__ import annotations

import argparse
import getpass
import json
import logging
import os
import platform
import secrets
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

from . import __version__
from .ble_scan import scan as ble_scan
from .ez1_ble import limit_30 as ble_limit_30, read as ble_read, stop as ble_stop
from .board import flash_factory, serial_boards, verify_esp32s3
from .errors import WatchdogError
from .ez1_http import EZ1Client
from .ha_config import generate as generate_ha
from .models import ProvisionConfig
from .provision import ble_stop_test, provision, self_test, wifi_test
from .report import write_report
from .commissioning import lease_latch_test, read_only_probe, soak
from .research import (
    adb_path,
    analyze_apk,
    android_info,
    ble_write_30,
    capture_bugreport,
    extract_hci,
    install_platform_tools,
    pull_easypower_apk,
)
from .safety import SafetyMachine
from .watchdog_api import WatchdogClient


ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".watchdog"


def print_json(value) -> None:
    print(json.dumps(value, indent=2, default=lambda item: item.__dict__))


def secret_from_file(path: Path) -> str:
    content = path.read_text(encoding="utf-8").strip()
    marker = "Bearer "
    if marker not in content:
        raise ValueError("secret file does not contain a Bearer token")
    secret = content.split(marker, 1)[1].strip().strip('"')
    if len(secret) != 64:
        raise ValueError("watchdog secret must be 64 hexadecimal characters")
    int(secret, 16)
    return secret


def watchdog_client(args: argparse.Namespace) -> WatchdogClient:
    secret = getattr(args, "secret", None)
    secret_file = getattr(args, "secret_file", None)
    if not secret and secret_file:
        secret = secret_from_file(secret_file)
    if not secret:
        secret = getpass.getpass("Watchdog bearer secret: ")
    if len(secret) != 64:
        raise ValueError("watchdog secret must be 64 hexadecimal characters")
    int(secret, 16)
    return WatchdogClient(args.url, secret)


def add_watchdog_connection(command: argparse.ArgumentParser) -> None:
    command.add_argument("--url", required=True)
    credentials = command.add_mutually_exclusive_group()
    credentials.add_argument("--secret", help="prefer an interactive prompt or --secret-file")
    credentials.add_argument("--secret-file", type=Path)


def command_doctor(_args: argparse.Namespace) -> int:
    checks = {
        "python": {"status": "PASS" if sys.version_info >= (3, 10) else "FAIL", "value": sys.version.split()[0]},
        "platform": {"status": "PASS" if os.name == "nt" else "WARN", "value": platform.platform()},
        "disk_free_mb": {"status": "PASS", "value": shutil.disk_usage(ROOT).free // 1_048_576},
        "factory_image": {
            "status": "PASS" if (ROOT / "dist/firmware/ez1-watchdog-xiao-esp32s3.factory.bin").exists() else "WARN",
            "value": "prebuilt image present" if (ROOT / "dist/firmware/ez1-watchdog-xiao-esp32s3.factory.bin").exists() else "build/verified release required",
        },
    }
    try:
        checks["serial"] = {"status": "PASS", "value": [board.__dict__ for board in serial_boards()]}
    except WatchdogError as exc:
        checks["serial"] = {"status": "WARN", "value": str(exc)}
    print_json(checks)
    return 1 if any(item["status"] == "FAIL" for item in checks.values()) else 0


def command_board_info(args: argparse.Namespace) -> int:
    if args.port:
        print(verify_esp32s3(args.port))
    else:
        print_json([board.__dict__ for board in serial_boards()])
    return 0


def command_flash(args: argparse.Namespace) -> int:
    flash_factory(args.port, args.image, args.manifest)
    print("Verified ESP32-S3 and flashed the checked factory image.")
    return 0


def _read_provision_config(args: argparse.Namespace) -> ProvisionConfig:
    secret = args.secret or (secret_from_file(args.secret_file) if args.secret_file else secrets.token_hex(32))
    password = args.wifi_password
    if not password and args.wifi_password_env:
        password = os.environ.get(args.wifi_password_env)
        if password is None:
            raise ValueError(f"environment variable {args.wifi_password_env} is not set")
    password = password or getpass.getpass("Wi-Fi password: ")
    return ProvisionConfig(
        wifi_ssid=args.wifi_ssid,
        wifi_password=password,
        ez1_host=args.ez1_host,
        ez1_device_id=args.device_id,
        auth_secret=secret,
        lease_interval_s=args.lease_interval,
        warning_s=args.warning,
        trip_s=args.trip,
    )


def command_provision(args: argparse.Namespace) -> int:
    config = _read_provision_config(args)
    response = provision(args.port, config)
    print_json({"board_response": response, "auth_secret": "<redacted>"})
    if args.secret_file:
        print("Used the watchdog secret file; the secret was not printed.")
    elif not args.secret:
        print("Store the generated secret in Home Assistant; it is not written to host state.")
        print(f"Generated secret (shown once): {config.auth_secret}")
    return 0


def command_board_self_test(args: argparse.Namespace) -> int:
    print_json(self_test(args.port))
    return 0


def command_board_ble_stop(args: argparse.Namespace) -> int:
    print_json(ble_stop_test(args.port))
    return 0


def command_wifi_test(args: argparse.Namespace) -> int:
    password = args.wifi_password or getpass.getpass("Wi-Fi password: ")
    print_json(wifi_test(args.port, args.wifi_ssid, password))
    return 0


def command_ez1_info(args: argparse.Namespace) -> int:
    client = EZ1Client(args.host, args.port, timeout=args.timeout)
    print_json(
        {
            "device": client.get_device_info().to_dict(),
            "runtime_limit_w": client.get_max_power(),
            "output": client.get_output_data() if args.output else "not requested",
        }
    )
    return 0


def command_clamp(args: argparse.Namespace) -> int:
    print("This will LOWER the EZ1 runtime limit to 30 W.")
    print("It will NOT automatically restore a higher value.")
    confirmation = args.confirm or input("Type CLAMP30 to continue: ").strip()
    if confirmation != "CLAMP30":
        print("Cancelled without writing to the inverter.")
        return 2
    result = EZ1Client(args.host, args.port, timeout=args.timeout).apply_failsafe(args.device_id)
    print_json(
        {
            "device_id": result.device.device_id,
            "requested_watts": result.requested_watts,
            "acknowledged_watts": result.acknowledged_watts,
            "verified_watts": result.verified_watts,
        }
    )
    return 0


def command_status(args: argparse.Namespace) -> int:
    print_json(watchdog_client(args).status())
    return 0


def command_lease(args: argparse.Namespace) -> int:
    print_json(watchdog_client(args).lease())
    return 0


def command_release(args: argparse.Namespace) -> int:
    print_json(watchdog_client(args).release(args.latch_id))
    return 0


def command_xiao_probe(args: argparse.Namespace) -> int:
    print_json(read_only_probe(watchdog_client(args)))
    return 0


def command_lease_test(args: argparse.Namespace) -> int:
    print("This test deliberately stops leases until the watchdog clamps the EZ1 to 30 W.")
    print("It never restores a higher inverter limit.")
    confirmation = args.confirm or input("Type LEASEFAILSAFE to continue: ").strip()
    if confirmation != "LEASEFAILSAFE":
        print("Cancelled without stopping the lease.")
        return 2
    print_json(lease_latch_test(watchdog_client(args)))
    return 0


def command_generate_ha(args: argparse.Namespace) -> int:
    package, secret_file, _ = generate_ha(
        args.output,
        args.watchdog_url,
        args.lease_interval,
        args.secret,
        getattr(args, "lease_guard", None),
    )
    print(f"Generated package: {package}")
    print(f"Generated secret:  {secret_file}")
    if not args.secret:
        print("Generated bearer secret is stored only in the generated secret file.")
    return 0


def command_simulate(_args: argparse.Namespace) -> int:
    machine = SafetyMachine(warning_ms=30_000, trip_ms=60_000)
    timeline = []

    def record(at: int, event: str, decision) -> None:
        timeline.append({"at_ms": at, "event": event, "state": decision.state.value, "action": decision.action.value, "reason": decision.reason})

    record(0, "boot", machine.boot(0, configured=True))
    record(1_000, "lease", machine.lease(1_000))
    record(31_000, "lease", machine.lease(31_000))
    record(60_999, "warning_minus_1ms", machine.tick(60_999))
    record(61_000, "warning_exact", machine.tick(61_000))
    record(90_999, "trip_minus_1ms", machine.tick(90_999))
    record(91_000, "trip_exact", machine.tick(91_000))
    record(91_001, "persist_latch", machine.latch_persisted())
    record(91_100, "http_verified_30w", machine.http_result(True))
    record(92_000, "lease_returns", machine.lease(92_000))
    record(93_000, "second_lease", machine.lease(93_000))
    record(93_100, "stale_release", machine.release(93_100, machine.latch_id - 1))
    record(93_200, "valid_release", machine.release(93_200, machine.latch_id))
    print_json(timeline)
    return 0


def command_ble_scan(args: argparse.Namespace) -> int:
    results = ble_scan(args.seconds, args.output)
    print_json([item.__dict__ for item in results])
    print("Read-only scan complete. No BLE write was attempted.")
    return 0


def command_ble_read(args: argparse.Namespace) -> int:
    print_json(ble_read(args.address, args.device_id))
    return 0


def command_ble_stop(args: argparse.Namespace) -> int:
    if args.confirm != "BLESTOP":
        print("Cancelled. Pass --confirm BLESTOP to stop inverter production over Bluetooth.")
        return 2
    print_json(ble_stop(args.address, args.device_id))
    return 0


def command_ble_limit_30(args: argparse.Namespace) -> int:
    if args.confirm != "BLE30":
        print("Cancelled. Pass --confirm BLE30 to request the fixed 30 W runtime limit over Bluetooth.")
        return 2
    print_json(ble_limit_30(args.address, args.device_id))
    return 0


def command_report(args: argparse.Namespace) -> int:
    results = {}
    if args.results and args.results.exists():
        results = json.loads(args.results.read_text(encoding="utf-8"))
    print(write_report(args.output, results, ROOT))
    return 0


def command_soak(args: argparse.Namespace) -> int:
    result = soak(watchdog_client(args), args.output, args.duration, args.interval, args.expected_state)
    print_json(result)
    return 1 if result["failures"] else 0


def _adb(args: argparse.Namespace, install: bool = False) -> Path:
    path = adb_path(LOCAL)
    if not path and install:
        path = install_platform_tools(LOCAL)
    if not path:
        raise FileNotFoundError("adb was not found; run android-setup --download or install official Platform-Tools")
    return path


def command_android_setup(args: argparse.Namespace) -> int:
    print_json(android_info(_adb(args, args.download)))
    return 0


def command_apk_pull(args: argparse.Namespace) -> int:
    if args.confirm != "PULLAPK":
        print("Cancelled. Pass --confirm PULLAPK after confirming this is your own phone/app installation.")
        return 2
    print_json(pull_easypower_apk(_adb(args), args.output))
    return 0


def command_android_capture(args: argparse.Namespace) -> int:
    print_json(capture_bugreport(_adb(args), args.output))
    return 0


def command_apk_analyze(args: argparse.Namespace) -> int:
    print_json(analyze_apk(args.apk, args.output, args.jadx))
    return 0


def command_hci_analyze(args: argparse.Namespace) -> int:
    print(extract_hci(args.capture, args.output, args.tshark))
    return 0


def command_ble_poc(args: argparse.Namespace) -> int:
    if args.confirm != "BLE30":
        print("Cancelled. Pass --confirm BLE30 only after the protocol file is backed by capture evidence.")
        return 2
    print_json(ble_write_30(args.protocol))
    return 0


def command_wizard(_args: argparse.Namespace) -> int:
    print("=" * 58)
    print(" EZ1 WATCHDOG INSTALLER")
    print(" Seeed XIAO ESP32-S3 / APsystems EZ1-M")
    print("=" * 58)
    print("This tool never modifies EZ1 factory firmware and never raises inverter power.")
    print("Live inverter writes are restricted to explicit, verified 30 W and stop operations.")
    if input("Type XIAO-S3 to confirm board SKU 113991114: ").strip() != "XIAO-S3":
        print("Cancelled.")
        return 2
    command_doctor(argparse.Namespace())
    boards = serial_boards()
    if not boards:
        print("\nNo serial board is connected. Connect the XIAO by USB-C and run the wizard again.")
        return 1
    print("\nDetected serial devices:")
    for index, board in enumerate(boards, 1):
        print(f"[{index}] {board.port} — {board.description}")
    selected = input("Board number [1]: ").strip() or "1"
    try:
        board = boards[int(selected) - 1]
    except (ValueError, IndexError):
        print("Invalid board selection.")
        return 2
    print(verify_esp32s3(board.port))

    image = ROOT / "dist/firmware/ez1-watchdog-xiao-esp32s3.factory.bin"
    manifest = ROOT / "dist/firmware/manifest.json"
    if image.exists() and manifest.exists():
        if input("Type FLASH to verify and flash the factory image, or press ENTER to skip: ").strip() == "FLASH":
            flash_factory(board.port, image, manifest)
    else:
        print("No verified factory image is present; flashing is skipped until a reproducible build exists.")

    ez1_host = input("EZ1 local IP/hostname (blank to stop safely): ").strip()
    if not ez1_host:
        return 0
    inverter = EZ1Client(ez1_host)
    info = inverter.get_device_info()
    print_json({"device": info.to_dict(), "runtime_limit_w": inverter.get_max_power()})
    if input(f"Type the detected device ID {info.device_id} to confirm the target: ").strip() != info.device_id:
        print("Target not confirmed; no configuration was written.")
        return 2

    if input("Provision Wi-Fi and watchdog configuration now? [y/N]: ").strip().lower() == "y":
        token = secrets.token_hex(32)
        provision_config = ProvisionConfig(
            wifi_ssid=input("Wi-Fi SSID: ").strip(),
            wifi_password=getpass.getpass("Wi-Fi password: "),
            ez1_host=ez1_host,
            ez1_device_id=info.device_id,
            auth_secret=token,
        )
        print_json({"board_response": provision(board.port, provision_config), "auth_secret": "<redacted>"})
        print(f"Generated bearer secret (shown once): {token}")
        watchdog_url = input("XIAO URL after DHCP reservation (for example http://192.168.1.60): ").strip()
        if watchdog_url:
            package, secret_file, _ = generate_ha(ROOT / "home_assistant", watchdog_url, 30, token)
            print(f"Generated {package} and {secret_file}")
            print("After installing the HA package, use xiao-http-read and lease-test for live commissioning.")
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--version", action="version", version=__version__)
    commands = root.add_subparsers(dest="command")
    commands.add_parser("wizard", help="start the guided installer").set_defaults(func=command_wizard)
    commands.add_parser("doctor", help="check the local PC environment").set_defaults(func=command_doctor)

    board = commands.add_parser("board-info", help="list boards or verify one ESP32-S3")
    board.add_argument("--port")
    board.set_defaults(func=command_board_info)

    board_test = commands.add_parser("board-self-test", help="run the flashed board's USB self-test")
    board_test.add_argument("--port", required=True)
    board_test.set_defaults(func=command_board_self_test)

    board_ble = commands.add_parser("board-ble-stop", help="commission the XIAO fixed BLE stop path")
    board_ble.add_argument("--port", required=True)
    board_ble.add_argument("--confirm", required=True, choices=["BLESTOP"])
    board_ble.set_defaults(func=command_board_ble_stop)

    wifi = commands.add_parser("wifi-test", help="temporarily test Wi-Fi without arming the watchdog")
    wifi.add_argument("--port", required=True)
    wifi.add_argument("--wifi-ssid", required=True)
    wifi.add_argument("--wifi-password")
    wifi.set_defaults(func=command_wifi_test)

    flash = commands.add_parser("flash", help="verify and flash a factory image")
    flash.add_argument("--port", required=True)
    flash.add_argument("--image", type=Path, default=ROOT / "dist/firmware/ez1-watchdog-xiao-esp32s3.factory.bin")
    flash.add_argument("--manifest", type=Path, default=ROOT / "dist/firmware/manifest.json")
    flash.set_defaults(func=command_flash)

    provision_command = commands.add_parser("provision", help="provision the board over USB serial")
    provision_command.add_argument("--port", required=True)
    provision_command.add_argument("--wifi-ssid", required=True)
    wifi_credentials = provision_command.add_mutually_exclusive_group()
    wifi_credentials.add_argument("--wifi-password")
    wifi_credentials.add_argument("--wifi-password-env", help=argparse.SUPPRESS)
    provision_command.add_argument("--ez1-host", required=True)
    provision_command.add_argument("--device-id", required=True)
    provision_credentials = provision_command.add_mutually_exclusive_group()
    provision_credentials.add_argument("--secret", help="prefer --secret-file")
    provision_credentials.add_argument("--secret-file", type=Path)
    provision_command.add_argument("--lease-interval", type=int, default=30)
    provision_command.add_argument("--warning", type=int, default=120)
    provision_command.add_argument("--trip", type=int, default=180)
    provision_command.set_defaults(func=command_provision)

    for name in ("ez1-info", "test-http-read"):
        info = commands.add_parser(name, help="read and validate the EZ1 local API")
        info.add_argument("--host", required=True)
        info.add_argument("--port", type=int, default=8050)
        info.add_argument("--timeout", type=float, default=3.0)
        info.add_argument("--output", action="store_true")
        info.set_defaults(func=command_ez1_info)

    clamp = commands.add_parser("test-clamp30", help="explicitly lower and verify the EZ1 limit at 30 W")
    clamp.add_argument("--host", required=True)
    clamp.add_argument("--port", type=int, default=8050)
    clamp.add_argument("--timeout", type=float, default=3.0)
    clamp.add_argument("--device-id", required=True)
    clamp.add_argument("--confirm", help=argparse.SUPPRESS)
    clamp.set_defaults(func=command_clamp)

    for name, handler in (("status", command_status), ("lease", command_lease)):
        api = commands.add_parser(name)
        add_watchdog_connection(api)
        api.set_defaults(func=handler)
    release = commands.add_parser("release")
    add_watchdog_connection(release)
    release.add_argument("--latch-id", required=True, type=int)
    release.set_defaults(func=command_release)

    for name, handler in (("xiao-http-read", command_xiao_probe), ("lease-test", command_lease_test)):
        command = commands.add_parser(name)
        add_watchdog_connection(command)
        if name == "lease-test":
            command.add_argument("--confirm", help=argparse.SUPPRESS)
        command.set_defaults(func=handler)

    ha = commands.add_parser("generate-ha", help="generate the Home Assistant package and secret")
    ha.add_argument("--watchdog-url", required=True)
    ha.add_argument("--lease-interval", type=int, default=30)
    ha.add_argument("--lease-guard", help="binary_sensor that must be on for HA to renew the lease")
    ha.add_argument("--secret")
    ha.add_argument("--output", type=Path, default=ROOT / "home_assistant")
    ha.set_defaults(func=command_generate_ha)

    commands.add_parser("simulate", help="run a no-hardware lease/latch scenario").set_defaults(func=command_simulate)
    ble = commands.add_parser("ble-scan", help="capture BLE advertisements without writing")
    ble.add_argument("--seconds", type=float, default=60)
    ble.add_argument("--output", type=Path, default=LOCAL / "captures/ble-scan.json")
    ble.set_defaults(func=command_ble_scan)
    ble_read_command = commands.add_parser("ble-read", help="perform captured read-only EZ1 BLE requests")
    ble_read_command.add_argument("--address", required=True)
    ble_read_command.add_argument("--device-id", required=True)
    ble_read_command.set_defaults(func=command_ble_read)
    for name, confirmation, handler in (
        ("ble-stop", "BLESTOP", command_ble_stop),
        ("ble-limit30", "BLE30", command_ble_limit_30),
    ):
        command = commands.add_parser(name)
        command.add_argument("--address", required=True)
        command.add_argument("--device-id", required=True)
        command.add_argument("--confirm", required=True, choices=[confirmation])
        command.set_defaults(func=handler)
    report = commands.add_parser("report", help="create a redacted commissioning report")
    report.add_argument("--results", type=Path)
    report.add_argument("--output", type=Path, default=LOCAL / "reports/support-bundle.zip")
    report.set_defaults(func=command_report)

    soak_command = commands.add_parser("soak", help="record watchdog status for a bounded soak test")
    add_watchdog_connection(soak_command)
    soak_command.add_argument("--duration", type=int, required=True, help="seconds")
    soak_command.add_argument("--interval", type=int, default=30, help="seconds")
    soak_command.add_argument("--expected-state", default="ARMED")
    soak_command.add_argument("--output", type=Path, default=LOCAL / "reports/soak.jsonl")
    soak_command.set_defaults(func=command_soak)

    android_setup = commands.add_parser("android-setup", help="verify ADB and the attached research phone")
    android_setup.add_argument("--download", action="store_true", help="download official Google Platform-Tools locally")
    android_setup.set_defaults(func=command_android_setup)
    apk_pull = commands.add_parser("apk-pull", help="pull AP EasyPower APKs from your own phone")
    apk_pull.add_argument("--confirm", required=True)
    apk_pull.add_argument("--output", type=Path, default=LOCAL / "captures/android/apk")
    apk_pull.set_defaults(func=command_apk_pull)
    capture = commands.add_parser("android-capture", help="record action markers and collect an Android bugreport")
    capture.add_argument("--output", type=Path, default=LOCAL / "captures/android/session")
    capture.set_defaults(func=command_android_capture)
    apk_analyze = commands.add_parser("apk-analyze", help="run JADX and extract BLE-related static evidence")
    apk_analyze.add_argument("--apk", type=Path, required=True)
    apk_analyze.add_argument("--output", type=Path, default=LOCAL / "captures/android/jadx")
    apk_analyze.add_argument("--jadx", default="jadx")
    apk_analyze.set_defaults(func=command_apk_analyze)
    hci = commands.add_parser("hci-analyze", help="normalize Bluetooth ATT records with tshark")
    hci.add_argument("--capture", type=Path, required=True)
    hci.add_argument("--output", type=Path, default=LOCAL / "captures/hci-att.csv")
    hci.add_argument("--tshark", default="tshark")
    hci.set_defaults(func=command_hci_analyze)
    ble_poc = commands.add_parser("ble-poc", help="perform one evidence-backed desktop 30 W BLE proof")
    ble_poc.add_argument("--protocol", type=Path, required=True)
    ble_poc.add_argument("--confirm", required=True)
    ble_poc.set_defaults(func=command_ble_poc)
    return root


def main() -> int:
    args = parser().parse_args()
    if not args.command:
        args = parser().parse_args(["wizard"])
    log_dir = LOCAL / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        filename=log_dir / f"session-{datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')}.log",
        level=logging.INFO,
        format='{"time":"%(asctime)s","level":"%(levelname)s","message":"%(message)s"}',
    )
    logging.info("command=%s", args.command)
    try:
        return args.func(args)
    except (WatchdogError, ValueError, FileNotFoundError, json.JSONDecodeError, RuntimeError) as exc:
        logging.error("command=%s failed type=%s", args.command, type(exc).__name__)
        print(exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
