# BLE research gate

The AP EasyPower application documents a Bluetooth Direct Connect capability, but the production UUIDs, payload, authentication, and verification semantics are not public. Never guess them and never use a factory-firmware replacement tool on the production inverter.

## Evidence workflow

1. `ble-scan --seconds 60` records advertisements without writes.
2. `android-setup` verifies an already installed ADB, or `android-setup --download` downloads official Google Platform-Tools into `.watchdog/tools` without changing system PATH.
3. Enable USB debugging and Bluetooth HCI snoop logging on your own Android phone.
4. `apk-pull --confirm PULLAPK` dynamically finds APsystems packages and pulls the installed APK/split APKs.
5. `android-capture` records timestamps for Direct Connect and 120/60/30 W actions, collects an Android bugreport, and extracts named btsnoop candidates.
6. `apk-analyze --apk ...` uses an existing JADX installation and marks static findings `LOW_STATIC_ONLY`.
7. `hci-analyze --capture ...` uses an existing tshark installation to normalize ATT records.
8. Create a reviewed protocol evidence JSON containing `watts: 30`, evidence grade, address, characteristic, and captured payload. `ble-poc --confirm BLE30` performs one desktop write and optional characteristic readback.
9. Repeat desktop tests with the router online/off and after Bluetooth/inverter restarts. Grade HIGH only after independent, verified reproduction.
10. Port to ESP-NimBLE, repeat tests, and run a physical router-off test before marking PRODUCTION.

Until step 10 passes, router-loss BLE protection remains unavailable and the firmware reports that honestly.