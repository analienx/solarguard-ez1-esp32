import io

import pytest
from unittest.mock import patch

from host.errors import LeaseTestFailed
from host.watchdog_api import WatchdogClient


def test_status_retries_a_transient_transport_failure():
    response = io.BytesIO(b'{"state":"ARMED"}')
    with patch("host.watchdog_api.urlopen", side_effect=[TimeoutError(), response]) as request:
        assert WatchdogClient("http://watchdog", "secret", attempts=2).status()["state"] == "ARMED"
    assert request.call_count == 2


def test_release_is_not_retried_after_a_lost_response():
    with patch("host.watchdog_api.urlopen", side_effect=[TimeoutError(), io.BytesIO(b"{}")]) as request:
        with pytest.raises(LeaseTestFailed, match="check status"):
            WatchdogClient("http://watchdog", "secret", attempts=3).release(1)
    assert request.call_count == 1
