from __future__ import annotations

import argparse
import json
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import parse_qs, urlparse


class MockEZ1:
    def __init__(self, device_id: str = "EZ1-TEST", delay: float = 0, ignore_set: bool = False, malformed: bool = False, http_500: bool = False, min_power: int = 30):
        self.device_id = device_id
        self.delay = delay
        self.ignore_set = ignore_set
        self.malformed = malformed
        self.http_500 = http_500
        self.min_power = min_power
        self.max_power = 700
        self.write_count = 0

    def handler(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format, *_args):
                return

            def do_GET(self):
                time.sleep(owner.delay)
                if owner.http_500:
                    self.send_error(500)
                    return
                if owner.malformed:
                    body = b"{not-json"
                else:
                    parsed = urlparse(self.path)
                    if parsed.path == "/getDeviceInfo":
                        data = {"deviceId": owner.device_id, "devVer": "1.1.7.5", "ssid": "test", "ipAddr": "127.0.0.1", "minPower": owner.min_power, "maxPower": 800}
                    elif parsed.path == "/getMaxPower":
                        data = {"maxPower": owner.max_power}
                    elif parsed.path == "/getOutputData":
                        data = {"p1": 10.0, "p2": 20.0, "e1": 1.0, "e2": 2.0, "te1": 3.0, "te2": 4.0}
                    elif parsed.path == "/setMaxPower":
                        value = int(parse_qs(parsed.query)["p"][0])
                        owner.write_count += 1
                        if not owner.ignore_set:
                            owner.max_power = value
                        data = {"maxPower": value}
                    else:
                        self.send_error(404)
                        return
                    body = json.dumps({"data": data}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        return Handler


@contextmanager
def running_mock(**kwargs):
    mock = MockEZ1(**kwargs)
    server = ThreadingHTTPServer(("127.0.0.1", 0), mock.handler())
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield mock, server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8050)
    parser.add_argument("--wrong-device-id", action="store_true")
    parser.add_argument("--delay", type=float, default=0)
    parser.add_argument("--ignore-set", action="store_true")
    parser.add_argument("--malformed", action="store_true")
    parser.add_argument("--http-500", action="store_true")
    args = parser.parse_args()
    mock = MockEZ1("WRONG" if args.wrong_device_id else "EZ1-TEST", args.delay, args.ignore_set, args.malformed, args.http_500)
    server = ThreadingHTTPServer(("0.0.0.0", args.port), mock.handler())
    print(f"Mock EZ1 listening on http://127.0.0.1:{args.port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
