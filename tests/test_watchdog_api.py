import io
from unittest.mock import patch

from host.watchdog_api import WatchdogClient


def test_status_retries_a_transient_transport_failure():
    response = io.BytesIO(b'{"state":"ARMED"}')
    with patch("host.watchdog_api.urlopen", side_effect=[TimeoutError(), response]) as request:
        assert WatchdogClient("http://watchdog", "secret", attempts=2).status()["state"] == "ARMED"
    assert request.call_count == 2
