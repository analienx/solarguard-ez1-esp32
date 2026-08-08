from __future__ import annotations

import json
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from .errors import LeaseTestFailed


class WatchdogClient:
    def __init__(self, base_url: str, secret: str, timeout: float = 3.0, attempts: int = 3) -> None:
        self.base_url = base_url.rstrip("/")
        self.secret = secret
        self.timeout = timeout
        self.attempts = attempts

    def _request(self, method: str, path: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
        data = json.dumps(body).encode() if body is not None else None
        request = Request(
            self.base_url + path,
            data=data,
            method=method,
            headers={
                "Authorization": f"Bearer {self.secret}",
                "Content-Type": "application/json",
                "Accept": "application/json",
            },
        )
        for attempt in range(self.attempts):
            try:
                with urlopen(request, timeout=self.timeout) as response:
                    payload = json.load(response)
                break
            except HTTPError as exc:
                raise LeaseTestFailed(f"watchdog request {path} failed: {exc}") from exc
            except (URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
                if attempt + 1 == self.attempts:
                    raise LeaseTestFailed(f"watchdog request {path} failed: {exc}") from exc
                time.sleep(0.2)
        if not isinstance(payload, dict):
            raise LeaseTestFailed(f"watchdog response {path} is not an object")
        return payload

    def health(self) -> dict[str, Any]:
        request = Request(self.base_url + "/api/v1/health", headers={"Accept": "application/json"})
        try:
            with urlopen(request, timeout=self.timeout) as response:
                payload = json.load(response)
        except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
            raise LeaseTestFailed(f"health request failed: {exc}") from exc
        if not isinstance(payload, dict):
            raise LeaseTestFailed("health response is not an object")
        return payload

    def status(self) -> dict[str, Any]:
        return self._request("GET", "/api/v1/status")

    def lease(self) -> dict[str, Any]:
        return self._request("POST", "/api/v1/lease", {})

    def ez1_probe(self) -> dict[str, Any]:
        return self._request("POST", "/api/v1/ez1-probe", {})

    def release(self, latch_id: int) -> dict[str, Any]:
        return self._request("POST", "/api/v1/release", {"latch_id": latch_id})
