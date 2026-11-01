from pathlib import Path

from host.ha_config import generate
import pytest
import yaml


class HomeAssistantLoader(yaml.SafeLoader):
    pass


HomeAssistantLoader.add_constructor("!secret", lambda loader, node: loader.construct_scalar(node))


def test_generated_package_uses_secret_reference_and_no_plain_token(tmp_path):
    token = "ab" * 32
    package, secret_file, _ = generate(tmp_path, "http://192.168.1.60", 30, token)
    content = package.read_text(encoding="utf-8")
    assert token not in content
    assert "!secret ez1_watchdog_authorization" in content
    assert "ez1_watchdog_ble_identity_verified" in content
    assert "ez1_watchdog_last_ble_result" in content
    assert 'seconds: "/30"' in content
    assert token in secret_file.read_text(encoding="utf-8")
    parsed = yaml.load(content, Loader=HomeAssistantLoader)
    assert {"rest_command", "rest", "template", "automation"} <= parsed.keys()


def test_optional_safety_guard_blocks_lease_when_off(tmp_path):
    package, _secret_file, _ = generate(
        tmp_path,
        "http://WATCHDOG_IP",
        30,
        "ab" * 32,
        "binary_sensor.solar_phase_b_data_healthy",
    )
    automation = yaml.load(package.read_text(encoding="utf-8"), Loader=HomeAssistantLoader)["automation"][0]
    assert automation["actions"][0] == {
        "condition": "state",
        "entity_id": "binary_sensor.solar_phase_b_data_healthy",
        "state": "on",
    }
    assert automation["actions"][1]["action"] == "rest_command.ez1_watchdog_lease"


def test_package_alerts_on_lease_failure_and_offline_watchdog(tmp_path):
    package, _secret_file, _ = generate(tmp_path, "http://192.168.1.60", 30, "ab" * 32)
    parsed = yaml.load(package.read_text(encoding="utf-8"), Loader=HomeAssistantLoader)
    lease = parsed["automation"][0]
    call = lease["actions"][0]
    assert call["action"] == "rest_command.ez1_watchdog_lease"
    assert call["continue_on_error"] is True
    assert call["response_variable"] == "lease_response"
    failure = lease["actions"][1]
    assert "lease_response is not defined" in failure["if"][0]["value_template"]
    assert failure["then"][0]["data"]["notification_id"] == "ez1_watchdog_lease_failed"
    offline = next(a for a in parsed["automation"] if a["id"] == "ez1_watchdog_offline_notification")
    trigger = offline["triggers"][0]
    assert {"unavailable", "unknown", "UNPROVISIONED"} <= set(trigger["to"])
    assert trigger["for"] == "00:03:00"


def test_committed_example_package_matches_generator(tmp_path):
    package, _secret_file, _ = generate(
        tmp_path, "http://XIAO_IP", 30, "ab" * 32, "binary_sensor.solar_phase_b_data_healthy"
    )
    committed = Path(__file__).resolve().parents[1] / "home_assistant/solar_watchdog.generated.yaml"
    assert committed.read_text(encoding="utf-8") == package.read_text(encoding="utf-8")


@pytest.mark.parametrize("guard", ["sensor.power", "binary_sensor.Bad", "binary_sensor.x;bad"])
def test_rejects_unsafe_lease_guard_entity_id(tmp_path, guard):
    with pytest.raises(ValueError, match="lease guard"):
        generate(tmp_path, "http://192.168.1.60", 30, "ab" * 32, guard)


@pytest.mark.parametrize("secret", ["0x" + "a" * 62, "ab_" + "c" * 61, " " + "a" * 63, "-" + "a" * 63])
def test_rejects_secrets_the_firmware_would_reject(tmp_path, secret):
    with pytest.raises(ValueError, match="64 hexadecimal"):
        generate(tmp_path, "http://192.168.1.60", 30, secret)
