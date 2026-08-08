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