import secrets

import pytest

from host.models import ProvisionConfig


def config(**overrides):
    values = dict(
        wifi_ssid="home",
        wifi_password="secret",
        ez1_host="192.168.1.50",
        ez1_device_id="EZ1",
        auth_secret=secrets.token_hex(32),
        lease_interval_s=30,
        warning_s=120,
        trip_s=180,
    )
    values.update(overrides)
    return ProvisionConfig(**values)


def test_provision_validation_accepts_production_profile():
    config().validate()


@pytest.mark.parametrize("field,value", [("lease_interval_s", 120), ("warning_s", 180), ("trip_s", 120)])
def test_provision_validation_rejects_bad_timeout_order(field, value):
    with pytest.raises(ValueError):
        config(**{field: value}).validate()

@pytest.mark.parametrize(
    "field,value",
    [
        ("auth_secret", "0x" + "a" * 62),
        ("auth_secret", "a_" + "b" * 62),
        ("ez1_host", "192.168.1.50/path"),
        ("ez1_host", "h" * 129),
        ("ez1_device_id", "d" * 65),
        ("wifi_password", "\u00e9" * 33),
    ],
)
def test_provision_validation_matches_firmware_limits(field, value):
    with pytest.raises(ValueError):
        config(**{field: value}).validate()
