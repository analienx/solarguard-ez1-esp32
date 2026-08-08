# Hardware regression report — 2026-08-08 rerun

## Scope

Regression of the flashed XIAO ESP32-S3, APsystems EZ1-M, Home Assistant, ASUS router, and Zigbee2MQTT/SLZB coordinator boundary after adding the guarded HA lease and stricter BLE response framing. Secrets, device identifiers, MAC addresses, and router configuration exports are omitted.

## Results

| Scenario | Evidence | Result |
|---|---|---|
| Host safety/protocol suite | 55 tests, including randomized latch invariants and malformed HTTP/BLE responses | PASS |
| ESP-IDF firmware build | ESP-IDF 6.0.2; image built and flashed; board self-test passed NVS, flash, PSRAM, RNG, Wi-Fi, BLE, and task watchdog | PASS |
| HA configuration | Guarded package passed `ha core check`; Core restart recovered guarded leases | PASS |
| HA unavailable, LAN alive | Lease warning, persisted incident, verified device identity and HTTP 30 W readback | PASS |
| SLZB/Zigbee2MQTT unavailable | Phase-B health guard stopped leases; clean rerun produced a stable HTTP latch while HA and LAN stayed online | PASS |
| Reboot while latched | Same incident survived a USB reset and immediately re-enforced 30 W over HTTP | PASS |
| Release controls | Bad authentication returned 401; stale ID and correct ID with stale leases returned 409; fresh leases alone did not release; current authenticated ID did | PASS |
| Router Wi-Fi unavailable | Both radios were transiently off for 240 s; XIAO recorded `stopped_verified`; independent HTTP readback after recovery showed inverter off/0 W | PASS |
| BLE recovery | Exact device identity and DSP acknowledgement verified; inverter restored to on/30 W; independent HTTP readback matched | PASS |
| Short Wi-Fi interruption | 45 s radio outage recovered armed with unchanged incident ID | PASS |
| WAN-only interruption | WAN state changed from connected to stopped while local HA leases remained fresh; XIAO stayed armed; WAN restored | PASS |
| Final stability soak | 30 samples over 300 s, expected `ARMED`, zero failures | PASS |

## Defects found and corrected

- The HA lease was unconditional, so an SLZB/Zigbee meter outage could block HA writes without invoking the XIAO. The generated package now supports a validated `--lease-guard` binary sensor; this installation uses `binary_sensor.solar_phase_b_data_healthy`.
- A BLE continuation fragment could contain more bytes than its declared remaining length. Both host tests and native firmware now reject it.
- The soak CLI previously returned success even when its report contained failed samples. It now exits nonzero on any failure.

## Test-environment interference

An older concurrent Codex session was found running `watchdog.py lease-test` and holding COM5. It deliberately renewed and released incidents and caused USB resets during the first SLZB attempts. Those results were rejected. The exact process tree was stopped, the environment was verified clean, and the SLZB, persistence, release, and BLE tests were repeated successfully.

## Deliberately not exercised live

- A wrong inverter at the configured IP was not introduced because that requires unsafe reprovisioning; automated tests prove identity mismatch causes zero writes.
- A competing controller was not allowed to raise the inverter above 30 W; fixed-30 enforcement is covered by the HTTP client tests and firmware source invariants.
- A fully unpowered/asleep inverter was not created because it requires physical DC isolation. The expected persistent `ACTUATOR_UNREACHABLE` retry behavior remains covered by the state-machine tests.
- Exhausting or corrupting flash/NVS, RF jamming, power-brownout injection, and modifying EZ1 factory firmware are outside safe commissioning scope.

No finite test campaign proves 100% reliability. This rerun covers the meaningful, safely reproducible failure modes in this installation and records the remaining physical limits explicitly.
