# EZ1 BLE protocol evidence

Status: desktop reads, fixed 30 W, and fixed production-stop commands verified on 2026-08-08. Fixed production stop is implemented as the XIAO fallback after HTTP failure.

## Anonymized reference capture

- Device ID: `E17010020001` (anonymized example)
- Firmware: `EZ1 1.12.2t`
- BLE address/name: `AA:BB:CC:DD:EE:FF` / `EZ1_E17010020001` (anonymized example)
- Service: `0000fffe-0000-1000-8000-00805f9b34fb`
- Request characteristic: `0000ff0a-0000-1000-8000-00805f9b34fb`
- Notification characteristic: `0000ff0b-0000-1000-8000-00805f9b34fb`

An authorized Android AP EasyPower session was recorded through Android HCI snoop logging. The private bug report and APK remain under ignored `.watchdog/` storage and are not committed.

The capture contained matching successful request/reply pairs for `deviceInfo`, `wifiStatus`, `outputData`, `httpServer`, and `si`. Static analysis of the installed application confirmed its AES-CBC/zero-padding message construction and fragmentation. The new `ble-read` command independently reproduced `deviceInfo` and `outputData` with Bleak and verified the returned device ID.

## Safety boundary

The client verifies the exact advertised name and returned `deviceInfo` ID before any control request. It accepts only the exact captured DSP success reply. Neither the host CLI nor firmware exposes an arbitrary payload or variable wattage. The XIAO contains only the production-stop frame; it does not contain firmware, Wi-Fi configuration, or rated-power upgrade operations.

## Controlled 30 W experiment

On 2026-08-08, the desktop verified the advertised name and `deviceInfo` identity, then sent the app-derived semantic `maxPower` request with `power: "30"`. The inverter returned a matching `maxPower` / `set_reply` with code 200, but its independent local HTTP readback remained 800 W both immediately and after five seconds. The operation therefore failed verification and was not retained as an available command.

Further static analysis showed that the app's visible 600–800 W power-upgrade workflow converts the displayed value and follows `maxPower` with a separate low-level `inverterCmd`. That workflow changes rated-power settings and provides no evidence that extrapolating it to 30 W is valid. That inferred workflow remains rejected; the separately verified fixed runtime/on frame below came from reverse-engineered inverter UART evidence.

As a safe control comparison, the verified local HTTP API subsequently requested, acknowledged, and read back a 30 W runtime limit. With the array covered and producing about 4 W, the independent desktop BLE client still verified the same device and returned matching live output telemetry.

## Verified inverterCmd operations

Reverse-engineered EZ1 UART research provided two checksum-valid fixed frames. Each was wrapped in the app-confirmed AES-CBC `inverterCmd` request and tested independently on the exact inverter while the panels were covered:

- 30 W runtime/on: `FBFB06AAE10000031F01B3FEFE`
- production off: `FBFB06AAE1000000000191FEFE`
- exact successful DSP reply for each: `fbfb06de02000100000000fefe`

The stop test changed the independent HTTP status from on to off and output to zero. The 30 W test then changed status back to on and HTTP readback to 30 W. The inverter was left on at 30 W after the test.

The XIAO build uses native NimBLE and PSA AES. It negotiates ATT MTU 247 because the EZ1 returns a 68-byte notification that is truncated at the default 23-byte MTU. Its flashed self-test reported `ble_protocol_ready: true`, its direct USB commissioning command returned `stopped_verified`, and a full router-radio-off lease-expiry test also returned `stopped_verified`. Independent HTTP readback after Wi-Fi restoration reported status off and zero output.
