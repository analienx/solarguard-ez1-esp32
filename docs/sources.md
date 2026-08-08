# Implementation sources

- Seeed Studio XIAO ESP32-S3 board documentation: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- ESP-IDF ESP32-S3 documentation (release pinned to 6.0.2): https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/
- ESP-IDF OTA/rollback documentation for the later gated phase: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html
- Community APsystems EZ1 local API reference: https://sonnenladengmbh.github.io/APsystems-EZ1-API/api/
- APsystems EZ1 user manual and Bluetooth Direct Connect evidence: https://global.apsystems.com/document/apsystems-ez1-m-ez1-h-series-user-manual/
- Android HCI snoop setup: https://developer.android.com/studio/debug/dev-options
- Android Platform-Tools: https://developer.android.com/tools/releases/platform-tools

The EZ1 local API is community-documented rather than a formal vendor contract. The implementation therefore validates schemas, identity, acknowledgement, and readback against the actual inverter during commissioning.