# Failure matrix

| Failure | Watchdog result | Complementary HA result |
|---|---|---|
| WAN only down; local HA/LAN alive | Lease stays fresh; no clamp | Normal local control continues |
| HA Core/host down; router alive | HTTP identity check and verified 30 W latch | HA unavailable |
| Short Wi-Fi interruption below trip timeout | Warning at most; no clamp | Existing HA behavior |
| Router/AP hard-off | Lease expires; HTTP fails; BLE verifies identity and requests production stop | Existing Zigbee breaker may remain available |
| SLZB/Zigbee coordinator hangs | Guarded HA lease stops when Phase-B data health turns off; HTTP applies verified 30 W | Normal HA inverter increases are blocked by stale-data checks |
| XIAO reboot while latched | Persistent latch is loaded and 30 W is reapplied | HA must not assume release |
| EZ1 asleep at night | Latch remains; bounded HTTP retries continue | Optional breaker logic remains separate |
| Competing writer raises limit while latched | Detect, reassert 30 W, increment override count | HA should alert and stop the competing path |
| Wrong device at configured IP | Identity mismatch; no write | Alert through watchdog status |

The protocol and fixed commands, including a physical router-radio-off lease-expiry test, have passed hardware commissioning. See the dated hardware test report for exact evidence and remaining limitations.
