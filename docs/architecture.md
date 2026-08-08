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

HTTP and serial handlers enqueue events. `safety_controller` is the sole state owner. It uses the monotonic ESP timer, persists only configuration and rare latch/override transitions, and is supervised by the ESP-IDF task watchdog.

## Actuation

The production actuator has no arbitrary-wattage parameter. It performs:

1. `getDeviceInfo`
2. exact configured device-ID comparison
3. verify reported minimum permits 30 W
4. `getMaxPower`
5. only if above 30 W, `setMaxPower?p=30`
6. `getMaxPower` readback equal to 30 W

While latched it checks every 20 seconds. A competing writer above 30 W triggers one reassertion and increments a persistent counter. If this HTTP sequence fails, the firmware scans only for `EZ1_<configured device ID>`, reads and compares `deviceInfo`, sends the fixed production-stop UART frame through the captured BLE wrapper, and requires the exact successful DSP acknowledgement. Failure of both transports is `ACTUATOR_UNREACHABLE`, never success.

## BLE boundary

BLE is implemented as a narrow secondary actuator. The firmware exposes no arbitrary BLE command and no variable power setting. The desktop tool retains separately confirmed fixed stop and 30 W diagnostics for commissioning evidence; the XIAO uses only stop after HTTP failure. Factory EZ1 firmware is never modified.
