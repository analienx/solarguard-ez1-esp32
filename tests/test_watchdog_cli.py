from argparse import Namespace

import pytest

import host.provision as provision_module
from host.watchdog import _read_provision_config, command_generate_ha, command_soak, parser, secret_from_file


def test_secret_from_generated_ha_file(tmp_path):
    token = "ab" * 32
    secret_file = tmp_path / "secrets.generated.txt"
    secret_file.write_text(f'ez1_watchdog_authorization: "Bearer {token}"\n', encoding="utf-8")
    assert secret_from_file(secret_file) == token


def test_secret_file_rejects_invalid_token(tmp_path):
    secret_file = tmp_path / "bad.txt"
    secret_file.write_text('ez1_watchdog_authorization: "Bearer nope"\n', encoding="utf-8")
    with pytest.raises(ValueError):
        secret_from_file(secret_file)


def test_generate_ha_does_not_print_secret(tmp_path, capsys):
    args = Namespace(
        output=tmp_path,
        watchdog_url="http://WATCHDOG_IP",
        lease_interval=30,
        lease_guard=None,
        secret=None,
    )
    command_generate_ha(args)
    token = secret_from_file(tmp_path / "secrets.generated.txt")
    assert token not in capsys.readouterr().out


def test_provision_reads_wifi_password_from_environment(monkeypatch):
    token = "ab" * 32
    monkeypatch.setenv("WATCHDOG_TEST_WIFI", "test-password")
    args = Namespace(
        secret=token,
        secret_file=None,
        wifi_password=None,
        wifi_password_env="WATCHDOG_TEST_WIFI",
        wifi_ssid="test-network",
        ez1_host="EZ1_IP",
        device_id="E17010020001",
        lease_interval=30,
        warning=120,
        trip=180,
    )
    assert _read_provision_config(args).wifi_password == "test-password"


def test_board_ble_stop_requires_exact_confirmation():
    with pytest.raises(SystemExit):
        parser().parse_args(["board-ble-stop", "--port", "COM5", "--confirm", "wrong"])
    args = parser().parse_args(["board-ble-stop", "--port", "COM5", "--confirm", "BLESTOP"])
    assert args.confirm == "BLESTOP"


def test_board_ble_stop_sends_once_with_hardware_timeout(monkeypatch):
    captured = {}

    def fake_exchange(*args, **kwargs):
        captured["args"] = args
        captured["kwargs"] = kwargs
        return {"ok": True}

    monkeypatch.setattr(provision_module, "_exchange", fake_exchange)
    assert provision_module.ble_stop_test("COM5") == {"ok": True}
    assert captured["args"][1] == {
        "protocol": 1,
        "command": "ble_stop_test",
        "confirm": "BLESTOP",
    }
    assert captured["kwargs"] == {"timeout": 40, "resend": False}


def test_soak_command_fails_when_any_sample_fails(monkeypatch, tmp_path):
    monkeypatch.setattr("host.watchdog.watchdog_client", lambda _args: object())
    monkeypatch.setattr("host.watchdog.soak", lambda *_args: {"failures": 1})
    args = Namespace(output=tmp_path / "soak.jsonl", duration=1, interval=1, expected_state="ARMED")
    assert command_soak(args) == 1
