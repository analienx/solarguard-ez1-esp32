# Hardware test report — 2026-08-08

## Scope

End-to-end commissioning of the HTTP and BLE fail-safe paths on a Seeed Studio XIAO ESP32-S3 and APsystems EZ1-M running firmware `EZ1 1.12.2t`.

Sensitive credentials, the inverter serial number, BLE address, packet captures, and router export are intentionally omitted.

## Automated checks

- Python host/state/protocol suite: passed.
- ESP-IDF 6.0.2 native firmware build: passed.
- ESP32-S3 flash hash verification: passed.
- Board self-test: NVS, 8 MB flash, PSRAM, RNG, Wi-Fi, BLE initialization, and task watchdog passed.

## Desktop BLE controls

With PV output covered/near zero:

- Fixed production-stop command returned code 200 and the exact DSP acknowledgement.
- Independent HTTP `getOnOff` changed to off and output readback was zero.
- Fixed 30 W runtime/on command returned the same exact DSP acknowledgement.
- Independent HTTP `getOnOff` changed to on and `getMaxPower` returned 30 W.

## XIAO BLE commissioning

The physical USB command `board-ble-stop --confirm BLESTOP` exercised the firmware’s native NimBLE/PSA code. It verified the configured inverter identity and returned `stopped_verified`. Independent HTTP readback reported off and zero output.

During development this test found and fixed two hardware-only defects:

- latched enforcement could start before the ESP-IDF TCP/IP mailbox existed;
- the default BLE ATT MTU truncated the EZ1’s 68-byte notification.

Regression guards now verify network initialization ordering and fixed-command boundaries. The firmware negotiates ATT MTU 247 before GATT discovery and control.

## Router-radio-off test

Preconditions:

- watchdog `ARMED` with fresh 30-second HA leases;
- warning timeout 120 seconds, trip timeout 180 seconds;
- inverter on with a verified 30 W runtime limit;
- XIAO and inverter on Wi-Fi; test PC and Home Assistant retained a wired recovery path.

Procedure and result:

1. Both router Wi-Fi radios were disabled using a transient runtime command; no NVRAM setting or reboot was used.
2. The EZ1 and XIAO became unreachable over IP while the router and wired Home Assistant remained reachable.
3. Wi-Fi remained off beyond the real 180-second trip timeout and bounded HTTP/BLE actuation time.
4. Wi-Fi was restored.
5. Watchdog status showed a persisted `lease_expired` latch, `ble_seen_recently: true`, and `last_ble_result: stopped_verified`.
6. Independent EZ1 HTTP readback showed status off and zero output.
7. Fresh HA leases resumed but the latch remained set.
8. The current latch ID was explicitly released after two fresh leases.
9. The fixed BLE 30 W command restored status on with a 30 W readback.
10. Final watchdog state was `ARMED`.

## Remaining limits

This is strong evidence for the tested hardware and inverter firmware, not a universal guarantee. Each installation still needs checks for inverter firmware compatibility, antenna/radio placement, independent power, Home Assistant configuration, and its own real router-loss test. Long-duration 24-hour, 72-hour, and seven-day soak tests remain separate commissioning stages.
