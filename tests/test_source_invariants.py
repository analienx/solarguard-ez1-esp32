from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_firmware_power_write_is_only_fixed_failsafe():
    source = (ROOT / "firmware/main/ez1_http.c").read_text(encoding="utf-8")
    assert "_Static_assert(WATCHDOG_FAILSAFE_WATTS == 30" in source
    assert source.count("setMaxPower?p=") == 1
    assert "write_fixed_30" in source


def test_firmware_ble_protocol_is_fixed_stop_only():
    source = (ROOT / "firmware/main/ez1_ble.c").read_text(encoding="utf-8")
    header = (ROOT / "firmware/main/ez1_ble.h").read_text(encoding="utf-8")
    assert source.count("FBFB06AAE1000000000191FEFE") == 1
    assert "FBFB06AAE10000031F01B3FEFE" not in source
    assert 'SUCCESS_REPLY = "fbfb06de02000100000000fefe"' in source
    assert "ble_att_set_preferred_mtu(247)" in source
    assert "mtu < 71" in source
    assert "response_fragment_exceeds_declared_length" in source
    assert "ez1_ble_stop" in header
    assert "command" not in header


def test_board_ble_commissioning_is_confirmed_and_observable():
    app = (ROOT / "firmware/main/app_main.c").read_text(encoding="utf-8")
    server = (ROOT / "firmware/main/lease_server.c").read_text(encoding="utf-8")
    assert 'strcmp(confirm->valuestring, "BLESTOP")' in app
    assert '"last_ble_result"' in server
    assert '"ble_seen_recently", status.ble_identity_verified' in server


def test_android_capture_never_requests_non_failsafe_power_writes():
    source = (ROOT / "host/research.py").read_text(encoding="utf-8")
    assert '"set 120 W"' not in source
    assert '"set 60 W"' not in source


def test_wifi_test_does_not_persist_or_arm_watchdog():
    source = (ROOT / "firmware/main/app_main.c").read_text(encoding="utf-8")
    wifi_test = source.split('strcmp(command->valuestring, "wifi_test")', 1)[1].split(
        'strcmp(command->valuestring, "provision")', 1
    )[0]
    assert "wifi_manager_start" in wifi_test
    assert "config_store_save" not in wifi_test
    assert "safety_" not in wifi_test


def test_xiao_user_led_is_on_while_firmware_runs():
    source = (ROOT / "firmware/main/app_main.c").read_text(encoding="utf-8")
    assert "gpio_set_level(GPIO_NUM_21, 0)" in source
    assert "indicator_led_init();" in source
    assert 'case ESP_RST_USB: return "USB";' in source
    assert 'case ESP_RST_TASK_WDT: return "TASK_WDT";' in source


def test_watchdog_wifi_http_service_is_kept_responsive():
    server = (ROOT / "firmware/main/lease_server.c").read_text(encoding="utf-8")
    wifi = (ROOT / "firmware/main/wifi_manager.c").read_text(encoding="utf-8")
    assert "server_config.lru_purge_enable = true;" in server
    assert "esp_wifi_set_ps(WIFI_PS_NONE)" in wifi


def test_network_stack_starts_before_latched_enforcement_task():
    source = (ROOT / "firmware/main/app_main.c").read_text(encoding="utf-8")
    assert source.index("wifi_manager_start(&config)") < source.index(
        'xTaskCreate(safety_controller, "safety_controller"'
    )
