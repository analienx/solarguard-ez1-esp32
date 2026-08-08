# APsystems EZ1-M independent fail-safe watchdog

An independently powered Seeed Studio XIAO ESP32-S3 watches a renewable safety lease from Home Assistant. If the lease expires, it latches and reduces the inverter through its local interfaces:

1. Local HTTP: verify the configured EZ1 identity, request the fixed 30 W runtime limit, and verify the readback.
2. Bluetooth LE fallback: when HTTP is unavailable, verify the same identity and send the fixed production-stop command.

Returning Home Assistant leases never clear a latch. Recovery requires the current incident ID, authentication, and fresh leases.

## Why this exists

Home Assistant is excellent for normal solar control, but HA and the inverter can share the same router as a failure point. This watchdog is a separate, USB-powered controller that can still stop production over Bluetooth when the router or access point is unavailable.

This project does not replace normal export limiting, grid protection, or a certified electrical disconnect.

## Proven status

The complete router-loss path passed a hardware-in-the-loop test on 2026-08-08:

- both router Wi-Fi radios were disabled for longer than the real 180-second trip timeout;
- the XIAO and EZ1 became unreachable over IP while the XIAO stayed independently powered;
- the latch was persisted before actuation;
- HTTP failed and the XIAO switched to BLE;
- BLE advertisement and `deviceInfo` identities matched the configured inverter;
- the exact DSP success reply was received;
- after Wi-Fi restoration, independent HTTP readback reported production off and zero output;
- renewed HA leases did not clear the latch;
- an authenticated release restored `ARMED`, and the fixed 30 W command restored the inverter to on/30 W.

A same-day full regression also passed guarded SLZB/Zigbee failure, HA-only HTTP fallback, persisted-latch reboot, rejected-release cases, short Wi-Fi loss, WAN-only loss, and a 30-sample armed soak. See the [rerun report](docs/hardware-test-report-2026-08-08-rerun.md).

Tested hardware and software:

- Seeed Studio XIAO ESP32-S3, 8 MB PSRAM/8 MB flash, with external U.FL antenna
- APsystems EZ1-M firmware `EZ1 1.12.2t`, DSP `5344`, DCM `1.3.17.0`
- ESP-IDF `6.0.2`
- Python 3.14 on Windows 11; host tools require Python 3.10 or newer

The test proves this exact installation and protocol version. No physical safety system can honestly be guaranteed “100%” across every inverter firmware, antenna placement, power condition, and network; commission every installation using the supplied checks.

## Safety design

- The firmware has no arbitrary-wattage or arbitrary-BLE-command interface.
- HTTP can request only the compile-time fixed 30 W limit.
- Firmware BLE contains only the verified production-stop frame.
- Every first write verifies the configured inverter ID on that transport.
- BLE success requires the exact captured DSP acknowledgement.
- The incident latch is persisted in NVS before actuation.
- A reboot reloads and re-enforces a persisted latch.
- Factory EZ1 firmware is never modified.
- The XIAO user LED is orange and active-low; solid on means the watchdog firmware is running.

See [architecture](docs/architecture.md), [failure matrix](docs/failure_matrix.md), and [BLE protocol evidence](docs/ble-protocol-evidence.md) for details.

## Hardware

- Seeed Studio XIAO ESP32-S3, SKU 113991114
- External U.FL 2.4 GHz antenna, positioned clear of metal
- USB power supply independent of the Home Assistant host and router
- APsystems EZ1-M with Bluetooth enabled
- Home Assistant reachable on the local LAN during normal operation

Both the XIAO and EZ1 require 2.4 GHz Wi-Fi. A dual-band SSID is fine when it is also advertised on 2.4 GHz.

## Quick start

Clone the repository and create the pinned Python environment:

```powershell
git clone <YOUR_FORK_OR_REPOSITORY_URL> ez1-watchdog
Set-Location ez1-watchdog
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.lock
python watchdog.py doctor
```

Connect the XIAO over USB and verify the exact chip before flashing:

```powershell
python watchdog.py board-info
python watchdog.py board-info --port COM5
```

Build with ESP-IDF 6.0.2:

```powershell
Set-Location firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash
Set-Location ..
python watchdog.py board-self-test --port COM5
```

Do not provision until Home Assistant is ready to send leases. Provisioning arms the warning and trip timers.

## Configuration and Home Assistant

First verify the inverter read-only:

```powershell
python watchdog.py ez1-info --host <EZ1_IP> --output
```

Generate the Home Assistant package and secret:

```powershell
python watchdog.py generate-ha `
  --watchdog-url http://<XIAO_IP> `
  --lease-guard binary_sensor.solar_phase_b_data_healthy
```

`--lease-guard` makes HA renew the lease only while that binary sensor is on. On this installation it covers the SLZB/Zigbee Phase-B meter: if the coordinator hangs and the meter data becomes stale, HA stops renewing and the XIAO applies its fail-safe. Omit the option only when the lease itself is intentionally the sole health signal.

Install the generated package as described in [home_assistant/README.md](home_assistant/README.md), validate HA configuration, and start the lease automation. Then provision over USB; the Wi-Fi password is prompted and is not placed in shell history:

```powershell
python watchdog.py provision `
  --port COM5 `
  --wifi-ssid <SSID> `
  --ez1-host <EZ1_IP> `
  --device-id <EZ1_DEVICE_ID> `
  --secret-file home_assistant\secrets.generated.txt
```

Reserve stable DHCP addresses for the XIAO and EZ1 in your router. A reservation does not connect a device; it only assigns an address after successful Wi-Fi authentication.

Check live state with:

```powershell
python watchdog.py status `
  --url http://<XIAO_IP> `
  --secret-file home_assistant\secrets.generated.txt
```

## Bluetooth support

Bluetooth is a real fail-safe actuator in this project, not merely a scanner.

The implementation uses the EZ1 service `FFFE`, request characteristic `FF0A`, and notification characteristic `FF0B`. Messages use the captured APsystems AES-CBC wrapper and application fragmentation. The XIAO negotiates ATT MTU 247 so the inverter’s 68-byte response frames are received intact.

The production sequence is deliberately fixed:

1. scan for `EZ1_<configured device ID>`;
2. connect and negotiate MTU;
3. discover the exact service and characteristics;
4. subscribe to notifications;
5. request `deviceInfo` and compare both returned device IDs;
6. send only the fixed production-stop `inverterCmd`;
7. accept success only for the exact DSP acknowledgement.

AP EasyPower normally occupies the EZ1’s single BLE connection. Fully close or force-stop the app before commissioning BLE.

Read-only desktop verification:

```powershell
python watchdog.py ble-read `
  --address <EZ1_BLE_ADDRESS> `
  --device-id <EZ1_DEVICE_ID>
```

Explicit desktop commissioning commands:

```powershell
# Stop production; this does not automatically turn it back on.
python watchdog.py ble-stop `
  --address <EZ1_BLE_ADDRESS> `
  --device-id <EZ1_DEVICE_ID> `
  --confirm BLESTOP

# Turn production on with the fixed 30 W runtime limit.
python watchdog.py ble-limit30 `
  --address <EZ1_BLE_ADDRESS> `
  --device-id <EZ1_DEVICE_ID> `
  --confirm BLE30

# Exercise the actual XIAO BLE implementation over physical USB.
python watchdog.py board-ble-stop --port COM5 --confirm BLESTOP
```

Always confirm the result independently through `getOnOff`, output telemetry, or the AP EasyPower app before treating a commissioning write as successful.

## Commissioning the router-loss path

Follow the full [commissioning checklist](docs/commissioning.md). The decisive test is a real access-point outage, not a mocked failure:

1. Independently power the XIAO and keep USB attached for recovery.
2. Confirm state `ARMED`, fresh leases, the exact device ID, and normal inverter operation.
3. Make the router/AP unavailable for longer than the configured trip timeout plus actuator retries.
4. Restore the router.
5. Confirm `failsafe_latched: true`, `latch_persisted: true`, `ble_seen_recently: true`, and `last_ble_result: stopped_verified`.
6. Independently confirm inverter status off and output zero.
7. Confirm fresh leases did not release the latch.
8. Release using the current latch ID only after normal HA control is healthy.

Do not copy the ASUS commands from this repository blindly; use the supported radio controls for your own router and retain a wired or physical recovery path.

## Tests

Run the host/state/protocol suite:

```powershell
python -m pytest -q
```

GitHub Actions runs the same suite on every push and pull request. Build-time and source-invariant tests cover fixed command boundaries, identity checks, response framing, confirmation gates, latch behavior, stale-release rejection, reboot persistence, and network initialization ordering. The ESP-IDF build verifies the native NimBLE/PSA implementation. Hardware commissioning adds real USB, HTTP, BLE, lease-expiry, reboot, and router-loss checks.

The latest evidence is recorded in [hardware test report](docs/hardware-test-report-2026-08-08.md).

## Recovery

If the watchdog is latched, do not erase NVS or reflash merely to clear it. Restore connectivity, confirm the inverter’s safe state, wait for at least two fresh leases, and release the current incident:

```powershell
python watchdog.py release `
  --url http://<XIAO_IP> `
  --secret-file home_assistant\secrets.generated.txt `
  --latch-id <CURRENT_LATCH_ID>
```

See [recovery](docs/recovery.md) for reboot and failed-update behavior.

The official APsystems EZ1-M manual is linked in [implementation sources](docs/sources.md). The local HTTP API and BLE control details are community-documented and reverse-engineered; they are not presented as an APsystems-supported SDK or command contract.

## Example installation addresses

The maintainer’s private-LAN deployment uses:

- Home Assistant: `<HA_IP>`
- EZ1-M: `<EZ1_IP>`
- XIAO watchdog: `<XIAO_IP>`
- Wi-Fi SSID: `<WIFI_SSID>`, with 2.4 GHz enabled

Passwords, bearer secrets, router exports, Android captures, and device-specific private evidence are ignored and must never be committed.

## License

MIT. See [LICENSE](LICENSE). You may use, copy, modify, publish, distribute, sublicense, or sell the software subject to the license notice and warranty disclaimer.
