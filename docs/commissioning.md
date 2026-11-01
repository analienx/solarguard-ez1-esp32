# Commissioning

Use the included U.FL antenna, keep it clear of metal, and power the XIAO from a USB supply independent of the HA host.

## Safe order

1. Run `python watchdog.py doctor`.
2. Connect the exact Seeed XIAO ESP32-S3 SKU 113991114 and run `board-info --port COMx`. A different ESP chip is rejected.
3. Build with ESP-IDF 6.0.2 and create the verified factory image, or use a reviewed release image plus manifest.
4. Flash and provision over USB. Reserve stable DHCP addresses for the XIAO and EZ1.
5. Run PC read-only verification: `ez1-info --host EZ1_IP --output`.
6. Run XIAO read-only verification: `xiao-http-read --url http://XIAO_IP --secret SECRET`.
7. If desired, perform the explicit PC clamp test. It requires typing `CLAMP30` and never restores higher power.
8. Install the generated HA package and secret. Validate HA configuration before restarting/reloading.
9. Run `lease-test`. It requires typing `LEASEFAILSAFE`, waits for real expiry, requires a verified HTTP 30 W latch, proves returned leases do not release, rejects a stale ID, and then releases the current incident.
10. Disconnect WAN only. Leases must continue locally and no trip may occur.
11. Test a Wi-Fi interruption shorter than the trip timeout; no clamp should occur.
12. Run bounded soak tests (24 h, 72 h, then 7 days), preserving the JSONL logs and commissioning report.
13. Close AP EasyPower and run the confirmed desktop `ble-stop`, verify HTTP reports status off and zero output, then run `ble-limit30` and verify status on with a 30 W limit.
14. With the XIAO armed and HA leases healthy, perform a supervised router/AP power-off test. After the trip timeout plus bounded actuator retries, restore the router and verify `ez1.ble_seen_recently` is true (BLE identity verified within the last 15 minutes), `ez1.last_ble_result` is `stopped_verified`, and independent inverter status is off with zero output. Release only with the current latch ID and fresh leases. This test passed on the maintainer installation on 2026-08-08.

15. `wifi_test` is only accepted on an unprovisioned board; a provisioned watchdog answers `already_provisioned`.
16. Confirm the HA offline and lease-failure notifications by briefly unplugging the XIAO (offline after 3 minutes; the watchdog will trip if leases stay absent past the trip timeout).

## Release checklist

- HA has sent at least two recent leases.
- The watchdog state is `FAILSAFE_LATCHED`, not `ACTUATOR_UNREACHABLE`.
- The displayed latch ID is current.
- Normal HA solar inputs and its centralized write wrapper are healthy.
- Release does not itself raise the inverter limit; HA resumes gradually.

## Hardware gates

Source tests cannot prove radio placement, EZ1 firmware behavior, real flash integrity, or electrical outcomes. Record the actual board identity, MAC/IP, RSSI, EZ1 ID/firmware, clamp/readback timestamps, reset reason, and soak results with `python watchdog.py report`.
