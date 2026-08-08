import pytest

from host.errors import ClampVerifyFailed, ClampWriteFailed, EZ1IdentityMismatch, EZ1SchemaInvalid, EZ1Unreachable
from host.ez1_http import EZ1Client, FAILSAFE_WATTS
from tests.protocol.mock_ez1 import running_mock


def client(port: int) -> EZ1Client:
    return EZ1Client("127.0.0.1", port, timeout=0.2, attempts=1)


def test_read_only_device_and_limit():
    with running_mock() as (_mock, port):
        inverter = client(port)
        assert inverter.get_device_info().device_id == "EZ1-TEST"
        assert inverter.get_max_power() == 700
        assert inverter.get_output_data()["p1"] == 10.0


def test_failsafe_is_fixed_at_30_and_verified():
    with running_mock() as (mock, port):
        result = client(port).apply_failsafe("EZ1-TEST")
        assert result.requested_watts == FAILSAFE_WATTS == 30
        assert result.verified_watts == 30
        assert mock.write_count == 1


def test_identity_mismatch_never_writes():
    with running_mock(device_id="OTHER") as (mock, port):
        with pytest.raises(EZ1IdentityMismatch):
            client(port).apply_failsafe("EZ1-TEST")
        assert mock.write_count == 0


def test_set_ack_without_readback_is_failure():
    with running_mock(ignore_set=True) as (mock, port):
        with pytest.raises(ClampVerifyFailed):
            client(port).apply_failsafe("EZ1-TEST")
        assert mock.write_count == 1


def test_malformed_json_is_schema_error():
    with running_mock(malformed=True) as (_mock, port):
        with pytest.raises(EZ1SchemaInvalid):
            client(port).get_device_info()


def test_http_500_is_retried_and_never_writes():
    with running_mock(http_500=True) as (mock, port):
        inverter = EZ1Client("127.0.0.1", port, timeout=0.2, attempts=3, sleeper=lambda _delay: None)
        with pytest.raises(EZ1Unreachable):
            inverter.apply_failsafe("EZ1-TEST")
        assert mock.write_count == 0


def test_reported_minimum_above_failsafe_never_writes():
    with running_mock(min_power=31) as (mock, port):
        with pytest.raises(ClampWriteFailed):
            client(port).apply_failsafe("EZ1-TEST")
        assert mock.write_count == 0
