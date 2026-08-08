from host.commissioning import read_only_probe


class SlowProbe:
    def __init__(self):
        self.status_calls = 0

    def ez1_probe(self):
        return {}

    def status(self):
        self.status_calls += 1
        ready = self.status_calls > 1
        return {"ez1": {"identity_verified": ready, "http_reachable": ready}}


def test_read_only_probe_waits_for_embedded_http_result(monkeypatch):
    monkeypatch.setattr("host.commissioning.time.sleep", lambda _seconds: None)
    client = SlowProbe()
    assert read_only_probe(client)["ez1"]["identity_verified"] is True
    assert client.status_calls == 2
