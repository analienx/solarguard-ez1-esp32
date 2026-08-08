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
    assert automation["actions"] == [
        {
            "condition": "state",
            "entity_id": "binary_sensor.solar_phase_b_data_healthy",
            "state": "on",
        },
        {"action": "rest_command.ez1_watchdog_lease"},
    ]


@pytest.mark.parametrize("guard", ["sensor.power", "binary_sensor.Bad", "binary_sensor.x;bad"])
def test_rejects_unsafe_lease_guard_entity_id(tmp_path, guard):
    with pytest.raises(ValueError, match="lease guard"):
        generate(tmp_path, "http://192.168.1.60", 30, "ab" * 32, guard)
