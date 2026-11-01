# Recovery

## HA returns after a trip

Leases resume, but the XIAO remains latched at 30 W. Initialize the HA solar controller, verify the current latch ID, send a deliberate release, confirm `ARMED`, then let the existing HA controller increase power gradually.

## XIAO reboots while latched

The latch and ID are stored in NVS. Boot gives Wi-Fi up to 10 seconds to initialize, then immediately re-enforces the latch: fixed 30 W over HTTP when reachable, otherwise fixed production stop over BLE. Wi-Fi often needs longer than 10 seconds, so expect the BLE stop after a reboot. If the EZ1 is asleep or both transports are unavailable, the state remains `ACTUATOR_UNREACHABLE` and retries continue.

## Repeated crashes

If the board resets three times in a row from a panic or watchdog without receiving a lease in between, it latches with reason `repeated_abnormal_reset`. Release it like any other latch once leases are fresh, then investigate `reset_reason` and the serial log.

## Unprovisioned after NVS recovery

If NVS had to be erased at boot, configuration and any latch are gone and the LED blinks. `board-self-test` reports `nvs_erased_at_boot: true`. Re-provision over USB and confirm the inverter state independently.

## Provisioning recovery

Connect USB-C and send a new protocol-1 provisioning message. The board validates the complete configuration before one NVS commit and restarts only after a successful write. Early development intentionally does not enable irreversible eFuse security features.

## Failed update/build

Initial deployments are USB-only. No OTA path is enabled in this version. Keep the last verified factory image and manifest; do not introduce OTA until dual-slot rollback has been exercised while the inverter is already verified at 30 W.
