# EZ1 watchdog repository guidance

- The production watchdog may only request the fixed 30 W HTTP runtime limit or the fixed verified BLE production-stop command.
- Never add guessed BLE UUIDs, payloads, variable wattages, Wi-Fi settings, or firmware operations. Every production BLE operation requires captured evidence and independent hardware verification.
- Never modify or flash the EZ1-M factory firmware.
- Verify the configured EZ1 device ID before every first write after boot or path change.
- A returning lease never clears a latch. Release requires authentication, a fresh lease, and the current latch ID.
- Safety timing uses monotonic time. Do not persist heartbeat samples.
- Keep Home Assistant normal inverter control outside this repository.
- Run the host tests after every behavior change.
