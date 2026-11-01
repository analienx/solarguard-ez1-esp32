# Architecture

```text
Home Assistant normal controller
        |
        | authenticated lease every 30 s
        v
XIAO ESP32-S3 independent watchdog
        |
        | lease expires; verify configured EZ1 identity
        v
EZ1 local HTTP API -> fixed 30 W -> readback -> persistent latch
        |
        +-- HTTP unavailable -> BLE identity check -> fixed production stop
```

Home Assistant owns ordinary regulation. The XIAO owns only lease timing, the persistent incident latch, and the 30 W fail-safe request. It stores no HA credentials and needs no internet or wall clock.

The existing HA/Zigbee breaker path remains independent. It can still act when HA and Zigbee are alive but Wi-Fi routing is not.

## State ownership

HTTP and serial handlers enqueue events; leases carry their monotonic receipt time, so a lease that waited in the queue while the controller was blocked is not counted as fresher than it is. `safety_controller` is the sole state owner and drains all queued events before each trip check. It uses the monotonic ESP timer and persists only configuration and rare latch/override/abnormal-reset transitions. It is supervised by the ESP-IDF task watchdog with `CONFIG_ESP_TASK_WDT_PANIC=y`: a hang reboots the board, a persisted latch is re-enforced at boot, and three consecutive abnormal resets without a lease latch the fail-safe.

## Actuation

The production actuator has no arbitrary-wattage parameter. It performs:

1. `getDeviceInfo`
2. exact configured device-ID comparison
3. verify reported minimum permits 30 W
4. `getMaxPower`
5. only if above 30 W, `setMaxPower?p=30`
6. `getMaxPower` readback equal to 30 W

While latched it checks HTTP every 20 seconds. A competing writer above 30 W triggers one reassertion and increments a persistent counter; the first HTTP enforcement after a BLE stop is not counted, because the inverter still holds its pre-incident limit. If the HTTP sequence fails, the firmware scans only for `EZ1_<configured device ID>`, reads and compares `deviceInfo`, sends the fixed production-stop UART frame through the captured BLE wrapper, and requires the exact successful DSP acknowledgement. The BLE fallback never depends on whether the latch could be persisted. BLE attempts back off (20 s doubling to 5 min after failures; 10 min re-assertion after a verified stop). Failure of both transports is `ACTUATOR_UNREACHABLE`, never success.

Every BLE operation has a generation number passed to all NimBLE callbacks; callbacks from an abandoned attempt are ignored and a late connection from one is terminated. Timeouts are delivered to the NimBLE host task, which owns the operation state, and the caller copies the result only after completion.

## BLE boundary

BLE is implemented as a narrow secondary actuator. The firmware exposes no arbitrary BLE command and no variable power setting. The desktop tool retains separately confirmed fixed stop and 30 W diagnostics for commissioning evidence; the XIAO uses only stop after HTTP failure. Factory EZ1 firmware is never modified.
